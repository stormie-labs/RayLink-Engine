// Anomaly Coop - requests: ReqSpawn, an item a client asks the host to create in his character or in one of his items.
// The verdict is xrCoop/coop_spawn.h judge(). Host settings: configs\coop_spawn.ltx, read once per process. The cost
// budgets belong to the characters for the whole session, by player key: a peer that leaves and joins again as the same
// character refills nothing, nor does a reload that gives his character another id; reset() at the session's start and
// end clears them.
#include "stdafx.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_items.h"
#include "coop_dialog.h"
#include "xrCoop/coop_validate.h"

#include "Level.h"
#include "InventoryOwner.h"
#include "xrServer.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"

namespace coop
{
namespace requests
{
namespace
{
// A section as spawn::missing_line asks about it (xrCoop/coop_spawn.h When). It never
// reads a missing line, nor a number or flag of an empty value (r_s32 / r_bool of one would itself be FATAL here).
struct SectionIni
{
	LPCSTR section;

	LPCSTR value(LPCSTR line) const
	{
		return pSettings->line_exist(section, line) ? pSettings->r_string(section, line) : nullptr;
	}
	bool has(LPCSTR line) const { return !!pSettings->line_exist(section, line); }
	long long number(LPCSTR line) const // as r_s32 / r_u32 read it
	{
		const LPCSTR v = value(line);
		return v ? atoi(v) : 0;
	}
	bool flag(LPCSTR line) const { return value(line) && !!pSettings->r_bool(section, line); }
	bool ref_exists(LPCSTR line) const
	{
		const LPCSTR v = value(line);
		return v && v[0] && pSettings->section_exist(v);
	}
	bool ref_has(LPCSTR line, LPCSTR other) const
	{
		return ref_exists(line) && !!pSettings->line_exist(value(line), other);
	}
};

constexpr LPCSTR kSpawnSettings = "coop_spawn";

constexpr int kMaxSpawnSetting = 2000000000; // ltx values are ints
constexpr u64 kSpawnLogFirst = 10;
u64 g_spawn_refusals = 0;

u32 read_spawn_setting(CInifile& ini, LPCSTR key, u32 fallback)
{
	if (!ini.line_exist(kSpawnSettings, key))
		return fallback;
	return u32(_max(0, _min(ini.r_s32(kSpawnSettings, key), kMaxSpawnSetting)));
}

void read_section_list(CInifile& ini, LPCSTR list, xr_set<shared_str>& out)
{
	if (!ini.section_exist(list))
		return;
	const u32 n = ini.line_count(list);
	for (u32 i = 0; i < n; ++i)
	{
		LPCSTR name = nullptr;
		LPCSTR value = nullptr;
		if (ini.r_line(list, int(i), &name, &value) && name && name[0])
			out.insert(shared_str(name));
	}
}

// A refused ReqSpawn: counted by verdict (coop_status), the first ten logged with the section and the player.
void refuse_spawn(const Req& q, spawn::SpawnVerdict v, const char* section, const spawn::Missing& missing, u16 parent,
    double budget_left)
{
	using spawn::SpawnVerdict;
	++g_spawn_refused[u32(v)];
	if (++g_spawn_refusals <= kSpawnLogFirst)
	{
		const PeerInfo* p = mgr().session().peer(q.from);
		string256 why;
		switch (v)
		{
		case SpawnVerdict::UnknownClass:
			xr_sprintf(why, "class %.16s is not an item class a client may ask for",
			    pSettings->r_string(section, "class"));
			break;
		case SpawnVerdict::MissingLine:
			if (missing.no_section)
				xr_sprintf(why, "its `%s` names no section (its class reads it unchecked)", missing.line);
			else if (missing.in)
				xr_sprintf(why, "no `%s` in the section its `%s` names (its class reads it unchecked)", missing.line,
				    missing.in);
			else
				xr_sprintf(why, "no `%s` (its class reads it unchecked)", missing.line ? missing.line : "?");
			break;
		case SpawnVerdict::Denied: xr_strcpy(why, "in [deny_sections] of coop_spawn.ltx");
			break;
		case SpawnVerdict::QuestItem: xr_strcpy(why, "a quest item");
			break;
		case SpawnVerdict::OverBudget: // only when the queue is full
			xr_sprintf(why, "over the cost budget of character %u (cost %u RU, %.0f RU left) and %u of his requests "
			                "already wait",
			    q.proxy, spawn_facts(section).cost, budget_left, u32(spawn::kMaxQueued));
			break;
		case SpawnVerdict::WrongParent:
			xr_sprintf(why, "the item %u it would go into does not take it (only a launcher its rocket, a missile its "
			                "fake missile)",
			    parent);
			break;
		default: xr_strcpy(why, "?");
			break;
		}
		::Msg("! [coop] ReqSpawn #%u of [%s] from player #%u <%s> refused: %s", q.seq, section, q.from,
		    p ? p->nick.c_str() : "?", why);
	}
	Reason reason = Reason::NotAllowed; // not an item class, a parent that does not take it, deny-listed, a quest item
	if (v == SpawnVerdict::MissingLine)
		reason = Reason::BadSection;
	else if (v == SpawnVerdict::OverBudget)
		reason = Reason::RateLimited;
	reply(q, reason, parent);
}

// The item `parent` (one of the asker's, not his character) takes the new entity: the host
// gives it GE_OWNERSHIP_TAKE, whose handlers cast the child unchecked (xrCoop/coop_spawn.h child_allowed).
bool parent_takes(u16 parent, const spawn::ClassRule& child, const char* child_section)
{
	const CSE_Abstract* holder = world::server_entity(parent);
	if (!holder || !holder->s_name.size() || !pSettings->section_exist(holder->s_name) ||
	    !pSettings->line_exist(holder->s_name, "class"))
		return false;
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(holder->s_name, "class"));
	// only while it has room - the children it holds now, the magazine of its section.
	return rule &&
	    spawn::child_allowed(rule->game, holder->s_name.c_str(), child.game, child_section, holder->children.size(),
	        SectionIni{holder->s_name.c_str()}.number("ammo_mag_size"));
}

// Whether `parent` takes a new entity of `rule`'s class: the asker's character any item but a launcher's rocket
// one of his items only what it takes (parent_takes). No rule: the class check refuses first.
bool parent_ok_for(const Req& q, u16 parent, const spawn::ClassRule* rule, const char* section)
{
	if (!rule)
		return true;
	return parent == q.proxy ? spawn::character_takes(rule->game) : parent_takes(parent, *rule, section);
}

// The turn of the front request of `key`: everything checked again from what the host knows now (the section, its
// class and lines, the parent and what it holds, the lists, the item state), the budget charged by judge; Wait while it
// does not hold it. A refusal after the charge gives it back.
spawn::Turn create_queued(const std::string& key, spawn::QueuedRequest& r)
{
	Req q;
	q.from = PeerId(r.peer);
	q.seq = r.seq;
	q.proxy = r.character;
	q.type = Msg::ReqSpawn;
	const char* section = r.section.c_str();
	if (!spawnable_section(section))
	{
		reply(q, Reason::BadSection, r.parent);
		return spawn::Turn::Refused;
	}
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	const spawn::Missing missing = rule ? missing_item_line(section, *rule) : spawn::Missing();
	spawn::SectionFacts facts = spawn_facts(section);
	facts.parent_ok = parent_ok_for(q, r.parent, rule, section);
	facts.launch_child = r.parent != q.proxy && rule != nullptr && facts.parent_ok;
	if (r.id != kNoId)
	{
		// still his id (a lease he gave back or lost meanwhile refuses it), a clone's source still his
		u32 source_cost = 0;
		const Reason leased = host_lease_checks(q, r.parent, r.id, r.source, r.section.c_str(), source_cost);
		if (leased != Reason::Ok)
		{
			reply(q, leased, r.parent);
			return spawn::Turn::Refused;
		}
		if (r.source != kNoId)
			facts.cost = lease::clone_cost(facts.cost, source_cost);
	}
	spawn::CostBudget& budget = g_spawn_budgets.of(key);
	const spawn::SpawnVerdict verdict =
	    spawn::judge_queued(rule, missing.line, facts, r, budget, g_spawn_policy, mgr().now_ms());
	if (verdict == spawn::SpawnVerdict::OverBudget)
		return spawn::Turn::Wait;
	if (verdict != spawn::SpawnVerdict::Allow)
	{
		refuse_spawn(q, verdict, section, missing, r.parent, budget.tokens);
		return spawn::Turn::Refused;
	}
	const auto refuse = [&](Reason why)
	{
		if (!spawn::budget_free(facts))
			spawn::refund(budget, facts.cost, g_spawn_policy);
		reply(q, why, r.parent);
		return spawn::Turn::Refused;
	};
	// The player may have moved or dropped the parent meanwhile.
	const Reason holder = host_spawn_holder(q, r.parent);
	if (holder != Reason::Ok)
		return refuse(holder);
	Reason why = Reason::Ok;
	// a clone from the host's source as it is now, never from the client's blob
	std::vector<u8> source_state;
	if (!clone_state(r.source, r.id, r.state.data(), r.state.size(), source_state))
		return refuse(Reason::Offline);
	const spawn::StateRef state = spawn::spawn_state(r.source, r.state.data(), r.state.size(), source_state);
	CSE_Abstract* e = build_spawn(q, r.parent, section, r.pos, state.data, state.size, why);
	if (!e)
		return refuse(why);
	if (r.source != kNoId && !smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
	{
		F_entity_Destroy(e);
		return refuse(Reason::NotAllowed); // a clone is a magazined weapon
	}
	u16 created = kNoId;
	why = host_replace(q, e, r.parent, created, r.id, r.source, r.state.data(), r.state.size());
	if (why != Reason::Ok)
		return refuse(why);
	if (r.id != kNoId)
		host_leased_made(q, r.id, created, r.source);
	g_spawn_quest_gifts += facts.quest_item && !facts.allow_listed ? 1 : 0;
	reply(q, Reason::Ok, created);
	return spawn::Turn::Created;
}
} // namespace

// A section from a peer (or a script) is a printable name the host knows and that names an entity class:
// F_entity_Create is FATAL for a section without `class`.
bool spawnable_section(const char* section)
{
	return validate::resource_name_ok(section, spawn::kMaxSection) && pSettings->section_exist(section) &&
	    pSettings->line_exist(section, "class");
}

// The lines the constructors of `rule`'s server entity and game object (and their bases and
// Loads) read unchecked are there (xrCoop/coop_spawn.h missing_line). `section` passed spawnable_section.
spawn::Missing missing_item_line(const char* section, const spawn::ClassRule& rule)
{
	return spawn::missing_line(rule, SectionIni{section});
}

// Item-like temporary entity without an id: what CALifeSimulatorBase::spawn_item builds, minus
// the id allocation, registration and supplies (only the host does those).
CSE_Abstract* make_temp(const char* section, const Fvector& pos, u32 level_vertex, u16 game_vertex, u16 parent)
{
	if (!spawnable_section(section))
		return nullptr;
	CSE_Abstract* e = F_entity_Create(section);
	if (!e)
		return nullptr;
	e->s_name = section;
	e->set_name_replace(section);
	e->s_RP = 0xff;
	e->ID = kNoId;
	e->ID_Parent = parent;
	e->ID_Phantom = kNoId;
	e->o_Position = pos;
	e->m_wVersion = SPAWN_VERSION;
	if (CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e))
	{
		d->m_tNodeID = level_vertex;
		d->m_tGraphID = game_vertex;
		d->m_tSpawnID = ALife::_SPAWN_ID(-1);
	}
	if (CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(e))
		weapon->a_elapsed = weapon->get_ammo_magsize();
	return e;
}

spawn::SpawnPolicy g_spawn_policy;
xr_set<shared_str> g_spawn_allow, g_spawn_deny;
spawn::BudgetBook g_spawn_budgets;
u64 g_spawn_refused[kSpawnVerdicts] = {};
u64 g_spawn_quest_gifts = 0; // quest items let through for a talk

// The key of a character's budget, the same as the money book's (coop_money character_key_of): the player
// key digest on the "key" line of the proxy's custom data (ledger::character_key); a character without one (none of
// coop_join's) is keyed by its id.
std::string spawn_budget_key(u16 proxy)
{
	const CSE_Abstract* e = world::server_entity(proxy);
	std::string key = e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
	if (key.empty())
	{
		char id[16];
		xr_sprintf(id, "#%u", u32(proxy));
		key = id;
	}
	return key;
}

void load_spawn_settings()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;
	string_path path;
	FS.update_path(path, "$game_config$", "coop_spawn.ltx");
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\coop_spawn.ltx not found: spawn cost budget is the default, no allow or deny lists");
		return;
	}
	CInifile ini(path, TRUE, TRUE, FALSE); // DLTX: mod_coop_spawn_*.ltx add to it
	if (ini.section_exist(kSpawnSettings))
	{
		g_spawn_policy.cost_burst = read_spawn_setting(ini, "cost_burst", g_spawn_policy.cost_burst);
		g_spawn_policy.cost_per_sec = read_spawn_setting(ini, "cost_per_sec", g_spawn_policy.cost_per_sec);
	}
	else
		::Msg("! [coop] configs\\coop_spawn.ltx has no [%s]: spawn cost budget is the default", kSpawnSettings);
	read_section_list(ini, "allow_sections", g_spawn_allow);
	read_section_list(ini, "deny_sections", g_spawn_deny);
}

// What the verdict needs to know of the section: `quest_item` exactly as CInventoryItem::Load reads it, `cost` as
// CSE_ALifeInventoryItem does (0 if missing: the class check refuses such a section first), the host's lists.
spawn::SectionFacts spawn_facts(const char* section)
{
	spawn::SectionFacts f;
	f.quest_item = !!READ_IF_EXISTS(pSettings, r_bool, section, "quest_item", FALSE);
	f.cost = pSettings->line_exist(section, "cost") ? pSettings->r_u32(section, "cost") : 0;
	const shared_str name(section);
	f.allow_listed = g_spawn_allow.find(name) != g_spawn_allow.end();
	f.deny_listed = g_spawn_deny.find(name) != g_spawn_deny.end();
	return f;
}

// Who would hold a client's new item: checked before anything is constructed.
Reason host_spawn_holder(const Req& q, u16 parent)
{
	if (parent == kNoId)
		return Reason::NotAllowed; // world spawns are the host's
	if (events::root_of(parent) != q.proxy)
		return Reason::NotOwner;
	if (!world::server_entity(parent))
		return Reason::Missing;
	if (!Level().Server->ID_to_entity(parent))
		return Reason::Offline; // Process_spawn needs an online holder
	return Reason::Ok;
}

// Creates `e` (consumed) as a child of `parent` exactly like alife():create with an online parent:
// the server's Process_spawn with a host id; ALife registration announces it (EntitySpawn), a
// non-ALife child (fake missile, rocket) is announced with EntityLocal. The holder passed host_spawn_holder.
Reason host_spawn(CSE_Abstract* e, u16 parent, u16& created, u16 leased)
{
	created = kNoId;
	const auto fail = [&](Reason why)
	{
		F_entity_Destroy(e);
		return why;
	};
	if (smart_cast<CSE_ALifeCreatureAbstract*>(e) ||
	    (!smart_cast<CSE_ALifeInventoryItem*>(e) && !smart_cast<CSE_Temporary*>(e)))
		return fail(Reason::NotAllowed); // clients create items for themselves, nothing else
	CSE_Abstract* holder = world::server_entity(parent);
	if (!holder)
		return fail(Reason::Missing);

	// what a client may not choose
	e->ID = leased; // the id the client created it under already, else the server picks one
	if (leased != kNoId)
		name_with_id(e); // as the client named it (CALifeSimulatorBase::spawn_item's name)
	e->ID_Parent = parent;
	e->ID_Phantom = kNoId;
	e->RespawnTime = 0;
	e->s_RP = 0xff;
	e->m_tSpawnID = ALife::_SPAWN_ID(-1);
	e->o_Position = holder->o_Position;
	if (CSE_ALifeObject* ao = smart_cast<CSE_ALifeObject*>(e))
	{
		ao->m_story_id = INVALID_STORY_ID;
		ao->m_spawn_story_id = INVALID_SPAWN_STORY_ID;
	}
	CSE_ALifeDynamicObject* d = smart_cast<CSE_ALifeDynamicObject*>(e);
	CSE_ALifeDynamicObject* hd = smart_cast<CSE_ALifeDynamicObject*>(holder);
	if (d && hd)
	{
		d->m_tNodeID = hd->m_tNodeID;
		d->m_tGraphID = hd->m_tGraphID;
	}

	NET_Packet packet;
	e->Spawn_Write(packet, FALSE);
	F_entity_Destroy(e);
	u16 dummy;
	packet.r_begin(dummy);
	ClientID client;
	client.set(0xffff);
	xrServer& server = *Level().Server;
	// a leased id is out of the generator's free list since its grant; back in it right before Process_spawn
	// takes it again (E->ID = PerformIDgen(E->ID)), as spawn_item2 does with the id spawn_item took
	if (leased != kNoId)
		server.FreeID(leased, 0);
	CSE_Abstract* result = server.Process_spawn(packet, client);
	if (!result)
	{
		if (leased != kNoId && server.IsFreeID(leased))
			server.PerformIDgen(leased); // still leased: out of the free list again
		return Reason::Engine;
	}
	created = result->ID;
	if (!ai().alife().objects().object(result->ID, true))
	{
		events::host_relay_local_spawn(result);
		++g_h.spawns_local;
	}
	return Reason::Ok;
}

// How far the asker's character stands from the NPC of his talk lock, as host_try_lock
// (coop_dialog.cpp) measures it: 0 for an NPC that talks from afar (NeedOsoznanieMode), flt_max when either is not in
// the world.
float talk_distance(u16 npc, u16 proxy)
{
	CObject* them = g_pGameLevel ? Level().Objects.net_Find(npc) : nullptr;
	CInventoryOwner* io = smart_cast<CInventoryOwner*>(them);
	if (io && io->NeedOsoznanieMode())
		return 0.f;
	CObject* me = g_pGameLevel ? Level().Objects.net_Find(proxy) : nullptr;
	if (!me || !them)
		return flt_max;
	return me->Position().distance_to(them->Position());
}

// Builds the entity of a section that passed the class and line checks, from the host's section with the client's item
// state (the ItemState checks: counts, upgrades in an order the engine takes, ammo types). nullptr: refused, `why` says
// how (logged here).
CSE_Abstract* build_spawn(const Req& q, u16 parent, const char* section, const float pos[3], const u8* state,
    size_t state_size, Reason& why)
{
	// the section's own installed_upgrades break its tree: init_install is FATAL on the new object.
	if (!items::section_defaults_ok(shared_str(section)))
	{
		if (note_bad(BadSection))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: [%s] has default upgrades its own upgrade tree "
			      "refuses (broken data)",
			    q.seq, q.from, section);
		why = Reason::BadSection;
		return nullptr;
	}
	CSE_Abstract* e =
	    make_temp(section, Fvector().set(pos[0], pos[1], pos[2]), u32(-1), GameGraph::_GRAPH_ID(-1), parent);
	if (!e)
	{
		why = Reason::Engine;
		return nullptr;
	}
	const char* bad = nullptr;
	if (state_size && !items::host_apply_spawn_state(e, state, state_size, bad))
	{
		if (note_bad(BadSpawnState))
			::Msg("! [coop] ReqSpawn #%u of [%s] from player #%u refused: %s", q.seq, section, q.from,
			    bad ? bad : "item state");
		F_entity_Destroy(e);
		why = Reason::Malformed;
		return nullptr;
	}
	return e;
}

// ---------------------------------------------------------------- requests waiting for the budget
// A request over its character's budget waits here (xrCoop/coop_spawn.h SpawnQueue) as data only (QueuedRequest: no
// entity: a script class's server entity holds Lua references that a save load closes),
// and is judged, built, created and answered when its turn comes. Its player leaving (on_peer_left), or the world
// replaced (SessionReset: another epoch), drops it unanswered.
spawn::SpawnQueue<spawn::QueuedRequest> g_spawn_queue;

u64 g_spawn_queued = 0, g_spawn_dequeued = 0, g_spawn_queue_dropped = 0;
void drop_queued(spawn::QueuedRequest&) { ++g_spawn_queue_dropped; }

// The player's leased creates still waiting for his budget go with his lease (it is freed when he
// loads the world again; a freed id may come back to him in his new pool, and the old request would create under it).
void drop_leased_queued(PeerId peer)
{
	const auto leased = [peer](const spawn::QueuedRequest& r) { return PeerId(r.peer) == peer && r.id != kNoId; };
	for (const std::string& key : g_spawn_queue.keys())
		g_spawn_queue.drop_if(key, leased, drop_queued);
}

// Host frame: drops what belongs to a replaced world (or to a player who left, should on_peer_left have missed it),
// gives the front requests their turns.
void host_pump_spawns()
{
	PROF_EVENT("coop/coop_requests/host_pump_spawns");
	if (!g_spawn_queue.total() || !world_up() || !Level().Server || Device.dwPrecacheFrame)
		return;
	const u32 epoch = join::host_world_epoch();
	const auto gone = [epoch](const spawn::QueuedRequest& r)
	{ return r.epoch != epoch || join::proxy_of_peer(PeerId(r.peer)) != r.character; };
	for (const std::string& key : g_spawn_queue.keys())
	{
		g_spawn_queue.drop_if(key, gone, drop_queued);
		g_spawn_dequeued +=
		    g_spawn_queue.pump(key, [&key](spawn::QueuedRequest& r) { return create_queued(key, r); });
	}
}

void host_on_spawn(PeerId from, Reader& r)
{
	// Layouts and markers: xrCoop/coop_spawn.h.
	const spawn::Layout layout = spawn::detect(r.cursor(), r.remaining());
	Req q;
	if (!host_begin(from, r, Msg::ReqSpawn, q))
		return;
	// every spawn request costs a token first, whatever is wrong with it
	if (!allow(from, CatSpawn))
		return reply(q, Reason::RateLimited, kNoId);
	spawn::Request s;
	if (!spawn::read_body(r, layout, s))
		return reply(q, Reason::Malformed, kNoId);
	if (g_clone_world != join::world_generation())
	{
		g_clone_committed.clear();
		g_clone_world = join::world_generation();
	}
	if (s.source != kNoId)
	{
		if (clone_committed(from, s.id, s.state, s.state_size))
			return reply(q, Reason::Ok, s.id);
	}
	if (!validate::finite3(s.pos))
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: position is not a number", q.seq, from);
		return reply(q, Reason::Malformed, s.parent);
	}
	const Reason holder = host_spawn_holder(q, s.parent);
	if (holder != Reason::Ok)
		return reply(q, holder, s.parent);
	// a leased id is the asker's, before anything touches the generator; into his character; a clone's source
	// is his weapon
	u32 source_cost = 0;
	if (s.id != kNoId)
	{
		const Reason leased = host_lease_checks(q, s.parent, s.id, s.source, s.section.c_str(), source_cost);
		if (leased != Reason::Ok)
			return reply(q, leased, s.parent);
	}
	// a section the host knows, with a class; then the host builds the entity from its own section.
	const char* section = s.section.c_str();
	if (!spawnable_section(section))
	{
		if (note_bad(BadSection))
			::Msg("! [coop] ReqSpawn #%u from player #%u refused: [%.64s] is not a section with a class here", q.seq,
			    from, validate::resource_name_ok(section, spawn::kMaxSection) ? section : "<bad name>");
		return reply(q, Reason::BadSection, s.parent);
	}
	// an item class of the game with every line its server entity and game object read unchecked;
	// anything else is never constructed (a missing line is FATAL on the host).
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	const spawn::Missing missing = rule ? missing_item_line(section, *rule) : spawn::Missing();
	CSE_Abstract* e = nullptr;
	if (rule && !missing.line)
	{
		Reason why = Reason::Ok;
		// a clone is built from the host's own source (as lua_clone copies it on the
		// client), whatever the client's blob says; the budget pays only the price difference
		std::vector<u8> source_state;
		if (!clone_state(s.source, s.id, s.state, s.state_size, source_state))
			return reply(q, Reason::Offline, s.parent);
		const spawn::StateRef state = spawn::spawn_state(s.source, s.state, s.state_size, source_state);
		e = build_spawn(q, s.parent, section, s.pos, state.data, state.size, why);
		if (!e)
			return reply(q, why, s.parent);
		if (s.source != kNoId && !smart_cast<CSE_ALifeItemWeaponMagazined*>(e))
		{
			F_entity_Destroy(e);
			return reply(q, Reason::NotAllowed, s.parent); // a clone is a magazined weapon
		}
	}
	// deny/allow lists, quest items, the character's cost budget; after the item state, so a malformed one
	// costs nothing. No rule or a missing line ends here too (judge steps 1-2, nothing was constructed).
	spawn::SectionFacts facts = spawn_facts(section);
	// a clone replaces its source (released with it): the budget pays only what it is worth more
	if (s.id != kNoId && s.source != kNoId)
		facts.cost = lease::clone_cost(facts.cost, source_cost);
	// into his character any item but a launcher's rocket; into one of his items
	// only what that item takes, while it has room.
	facts.parent_ok = parent_ok_for(q, s.parent, rule, section);
	// what an item of his takes is a shot (a rocket, a fake missile): no budget, no queue.
	facts.launch_child = s.parent != q.proxy && rule != nullptr && facts.parent_ok;
	// a quest item into his own character while he holds a talk lock (the stock dialogs give it there)
	// and stands within talk reach of that NPC
	const u16 talk_npc = dialog::host_lock_of(from);
	facts.in_dialog_to_own_actor = spawn::quest_gift(talk_npc != kNoId,
	    talk_npc != kNoId ? talk_distance(talk_npc, q.proxy) : flt_max, s.parent, q.proxy);
	const std::string key = spawn_budget_key(q.proxy);
	facts.queue_waiting = g_spawn_queue.waiting(key); // behind his earlier requests
	spawn::CostBudget& budget = g_spawn_budgets.of(key);
	const spawn::SpawnVerdict verdict = spawn::judge(rule, missing.line, facts, budget, g_spawn_policy, mgr().now_ms());
	const bool gift = facts.quest_item && !facts.allow_listed; // a quest item past judge is a quest gift
	// over the budget it waits (OverBudget needs a rule and no missing line: `e` was built and its state
	// checked). The queue keeps data only: the entity goes now and is built again at its turn, answered then
	// (host_pump_spawns). Refused only when his queue is full.
	if (verdict == spawn::SpawnVerdict::OverBudget && e)
	{
		F_entity_Destroy(e);
		e = nullptr;
		if (g_spawn_queue.push(key, spawn::queued_request(q.seq, q.from, q.proxy, s.parent, s.pos, s.section,
		                                s.state, s.state_size, join::host_world_epoch(), facts.in_dialog_to_own_actor,
		                                s.id, s.source))) // a leased id stays leased while it waits
		{
			if (++g_spawn_queued <= kSpawnLogFirst)
				::Msg("[coop] ReqSpawn #%u of [%s] from player #%u waits for the cost budget of character %u (%u in "
				      "his queue): answered when created",
				    q.seq, section, from, q.proxy, u32(g_spawn_queue.size(key)));
			if (s.id != kNoId)
				notify_queued(q, s.id); // his watchdog waits for this one
			return;
		}
	}
	if (verdict != spawn::SpawnVerdict::Allow)
	{
		if (e)
			F_entity_Destroy(e);
		return refuse_spawn(q, verdict, section, missing, s.parent, budget.tokens);
	}
	g_spawn_quest_gifts += gift ? 1 : 0;
	// Allow needs a rule and no missing line: `e` was built above.
	u16 created = kNoId;
	const Reason why = host_replace(q, e, s.parent, created, s.id, s.source, s.state, s.state_size);
	if (why != Reason::Ok && !spawn::budget_free(facts))
		spawn::refund(budget, facts.cost, g_spawn_policy); // the engine refused what was charged
	if (why == Reason::Ok && s.id != kNoId)
		host_leased_made(q, s.id, created, s.source);
	reply(q, why, why == Reason::Ok ? created : s.parent);
}
} // namespace requests
} // namespace coop
