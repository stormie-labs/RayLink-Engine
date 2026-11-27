// Anomaly Coop - requests: item events (ReqItemEvent). An upgrade bought at a mechanic, a silencer taken off, and the
// client's rebuild of an item whose upgrade the host refused.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_script.h"
#include "coop_entities.h"
#include "coop_trade.h"
#include "coop_items.h"
#include "coop_money.h"
#include "coop_dialog.h"
#include "xrCoop/coop_validate.h"

#include <algorithm>
#include <cstring>

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "WeaponMagazined.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "script_engine.h"

namespace coop
{
namespace requests
{
namespace
{
int lua_silencer_state(lua_State* L)
{
	lua_getglobal(L, "coop_mods_items");
	if (!lua_istable(L, -1)) return 0;
	lua_getfield(L, -1, "native_silencer_state");
	if (!lua_isfunction(L, -1)) return 0;
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_call(L, 2, 1);
	return 1;
}

// ---------------------------------------------------------------- a refused upgrade (client)
// The host refused an upgrade this client installed on its item already (the engine installs before the request goes).
// An installed upgrade cannot leave a live object: its properties were added to the ones the item loaded, and a list is
// installed only on a fresh object (CInventoryItem::net_Spawn_install_upgrades). So the item is built again from the
// host's entity, which keeps the host's list: it is destroyed here as a host command (the local server does it,
// nothing goes to the host), and once it is gone it is asked for again with its children (StateDumpRequest; the
// host's EntitySpawn creates it in our inventory with the same id). Bounded: kMaxRebuilds at a time, a few tries.
constexpr size_t kMaxRebuilds = 16;

constexpr size_t kMaxRebuildChildren = 16;
constexpr u64 kRebuildDestroyMs = 5000; // the local destroy chain takes a frame or two
constexpr u64 kRebuildAskMs = 3000; // no EntitySpawn yet: ask again
constexpr u32 kRebuildTries = 3;

void client_ask_rebuild(const Rebuild& rb)
{
	Writer w(4);
	w.u16(rb.host_id); // the holder before its children
	net::client_send(Msg::StateDumpRequest, w);
	const size_t count = std::min<size_t>(rb.children.size(), kMaxRebuildChildren);
	for (size_t i = 0; i < count; ++i)
	{
		Writer wc(4);
		wc.u16(join::to_host(rb.children[i]));
		net::client_send(Msg::StateDumpRequest, wc);
	}
}

void client_unkeep_rebuild(u16 local_item, const Rebuild& rb)
{
	script::unkeep_instance(local_item);
	for (u16 child : rb.children)
		script::unkeep_instance(child);
}

// ---------------------------------------------------------------- upgrades at a mechanic (host)
// A client's upgrade is bought at a mechanic: the request names him and what the client's game charged for it
// (ledger::UpgradePayment), the host checks both (ledger::judge_upgrade) and charges the player's balance itself.
// Settings the stock upgrade scripts read (inventory_upgrades.script ini_file), each opened once per process like the
// Lua ones (CInifile: their DLTX mod files apply the same way) and kept for it.

// `missing`: what a missing file means, for the log (the line says what the host then does).
CInifile* open_game_config(LPCSTR name, LPCSTR missing)
{
	string_path path;
	FS.update_path(path, "$game_config$", name);
	if (!FS.exist(path))
	{
		::Msg("! [coop] configs\\%s not found: %s", name, missing);
		return nullptr;
	}
	return xr_new<CInifile>(path, TRUE, TRUE, FALSE);
}

// Mechanics: [<section>] (discount, tiers) and [<section>_upgr] (his own upgrade conditions).
CInifile* upgrade_info_ini()
{
	static CInifile* ini = open_game_config("misc\\stalkers_upgrade_info.ltx",
	    "no NPC is a mechanic, every client upgrade at one is refused");
	return ini;
}

// Upgrade prices as the Lua price reads them (char_ini): item_upgrades.ltx is not part of system.ltx.
CInifile* item_upgrades_ini()
{
	static CInifile* ini = open_game_config("item_upgrades.ltx",
	    "upgrade costs come from the game settings; an upgrade without one there is refused");
	return ini;
}

// A mechanic's section: the stock scripts read his discount and tiers from [<section>] of
// misc\stalkers_upgrade_info.ltx and his own conditions from [<section>_upgr]. Either one names him: the twelve
// Warfare mechanics (sim_default_*_mechanic) have only the first.
bool is_mechanic_section(const shared_str& section)
{
	CInifile* ini = upgrade_info_ini();
	if (!ini || !section.size() || section.size() > 200)
		return false;
	string256 upgr;
	xr_sprintf(upgr, "%s_upgr", section.c_str());
	return ini->section_exist(section) || ini->section_exist(upgr);
}

// `cost` of upgrade `upgrade` ([upgrade] section = <settings section>), read as the stock price does: from
// item_upgrades.ltx, else from the game settings. False: none (the stock price would fail on it).
bool upgrade_cost(const shared_str& upgrade, u32& cost)
{
	cost = 0;
	if (!pSettings->section_exist(upgrade) || !pSettings->line_exist(upgrade, "section"))
		return false;
	const LPCSTR sect = pSettings->r_string(upgrade, "section");
	if (!sect || !sect[0])
		return false;
	CInifile* ini = item_upgrades_ini();
	if (ini && ini->section_exist(sect) && ini->line_exist(sect, "cost"))
	{
		cost = ini->r_u32(sect, "cost");
		return true;
	}
	if (pSettings->section_exist(sect) && pSettings->line_exist(sect, "cost"))
	{
		cost = pSettings->r_u32(sect, "cost");
		return true;
	}
	return false;
}

// `mechanic` (host numbering) sells upgrades to player `from` now (ledger::check_mechanic): he holds that NPC's talk
// lock (the stock upgrade windows run inside the mechanic's dialog; coop_dialog); it is a live NPC with an inventory, a
// stalker or a trader class (the Forester, m_lesnik : m_trader, is a CAI_Trader), and not a
// player's character; the mechanics' settings name its section.
bool host_his_mechanic(PeerId from, u16 mechanic, const char*& why)
{
	ledger::MechanicFacts m;
	m.talk_lock = dialog::host_lock_of(from) == mechanic;
	CGameObject* go = m.talk_lock ? smart_cast<CGameObject*>(Level().Objects.net_Find(mechanic)) : nullptr;
	if (go && go->getDestroy())
		go = nullptr;
	CEntityAlive* npc = go ? smart_cast<CEntityAlive*>(go) : nullptr;
	m.inventory_owner = go && smart_cast<CInventoryOwner*>(go);
	m.alive = npc && npc->g_Alive();
	m.player = go && world::is_player_character(go);
	m.listed = go && is_mechanic_section(go->cNameSect());
	switch (ledger::check_mechanic(m))
	{
	case ledger::MechanicCheck::Ok: return true;
	case ledger::MechanicCheck::NotHisTalk:
		++g_upgrades.not_his;
		why = "he does not talk to him";
		return false;
	case ledger::MechanicCheck::NotAlive:
		++g_upgrades.not_alive;
		why = "not a live NPC with an inventory";
		return false;
	case ledger::MechanicCheck::NotMechanic:
		++g_upgrades.not_mechanic;
		why = "not a mechanic";
		return false;
	}
	return false;
}

const char* upgrade_verdict_name(ledger::UpgradeVerdict v)
{
	switch (v)
	{
	case ledger::UpgradeVerdict::Ok: return "ok";
	case ledger::UpgradeVerdict::Scripted: return "no mechanic named (scripted_upgrades = false)";
	case ledger::UpgradeVerdict::NoMechanic: return "no mechanic";
	case ledger::UpgradeVerdict::TooFar: return "too far from the mechanic";
	case ledger::UpgradeVerdict::UnknownCost: return "the upgrade's cost is unknown here";
	case ledger::UpgradeVerdict::Underpaid: return "underpaid";
	case ledger::UpgradeVerdict::Insufficient: return "not enough money on the host";
	}
	return "?";
}

// The brief's checks before the upgrade's own (host_check_added_upgrade): a mechanic, within kTradeReach of the
// player's character, a cost the host knows, paid at least floor(cost x upgrade_min_percent),
// and the player's balance on the host covers it. Refusals are NotAllowed on the wire, counted by cause.
Reason host_sell_upgrade(const Req& q, const shared_str& upgrade, const ledger::UpgradePayment& pay)
{
	ledger::UpgradeFacts f;
	f.mechanic_named = pay.mechanic != kNoId;
	f.scripted_allowed = money::scripted_upgrades();
	const char* not_mechanic = nullptr;
	f.mechanic_ok = f.mechanic_named && host_his_mechanic(q.from, pay.mechanic, not_mechanic);
	f.distance = f.mechanic_ok ? talk_distance(pay.mechanic, q.proxy) : flt_max;
	f.reach = trade::kTradeReach;
	f.paid = pay.paid;
	f.cost_known = upgrade_cost(upgrade, f.cost);
	if (f.mechanic_named && !f.cost_known)
		++g_upgrades.no_cost; // refused (UnknownCost) when the mechanic is right and near
	f.min_percent = money::upgrade_min_percent();
	f.balance = money::host_balance(q.from);
	if (!f.mechanic_named)
		++g_upgrades.without_mechanic;
	const ledger::UpgradeVerdict v = ledger::judge_upgrade(f);
	++g_upgrades.verdicts[u32(v)];
	if (v == ledger::UpgradeVerdict::Ok)
		return Reason::Ok;
	if (g_log.allow())
		::Msg("! [coop] upgrade [%s] from player #%u refused: %s%s%s (mechanic %u, %.1f m; paid %u RU, cost %u RU x %.2f; "
		      "balance %u RU)",
		    upgrade.c_str(), q.from, upgrade_verdict_name(v), not_mechanic ? ": " : "", not_mechanic ? not_mechanic : "",
		    pay.mechanic, f.mechanic_ok ? double(f.distance) : -1.0, f.paid, f.cost, double(f.min_percent), f.balance);
	return Reason::NotAllowed;
}

// Every upgrade request is answered, and when the client's game paid for it the player's balance follows the answer:
// his client holds the payment out of the money it shows until the answer (money::client_hold), so the next balance
// is exactly the host's, charged or not.
void answer_upgrade(const Req& q, Reason why, u16 item, u32 paid)
{
	reply(q, why, item);
	if (paid)
		money::host_send_balance(q.from);
}

// A ReqItemEvent after its seq: u16 item, u16 ge_type, u16 size, the event bytes, the payment.
struct ItemEventBody
{
	u16 item = kNoId, type = 0, size = 0;
	u8 data[512];
	ledger::UpgradePayment pay;
};

bool read_item_event(Reader& r, ItemEventBody& b)
{
	b.item = r.u16();
	b.type = r.u16();
	b.size = r.u16();
	return r.ok() && b.size <= sizeof(b.data) && r.bytes(b.data, b.size) && ledger::read(r, b.pay);
}

Reason host_detach_silencer(const Req& q, u16 item, const char* section, float condition)
{
	if (!_valid(condition) || condition < 0.f || condition > 1.f)
		return Reason::Malformed;
	if (events::root_of(item) != q.proxy)
		return Reason::NotOwner;
	CWeaponMagazined* weapon = smart_cast<CWeaponMagazined*>(world::object_here(item));
	if (!weapon || weapon->getDestroy())
		return Reason::Offline;
	if (!weapon->SilencerAttachable() || !weapon->IsSilencerAttached() ||
	    weapon->GetSilencerName() != section)
		return Reason::NotAllowed;
	const Reason holder = host_spawn_holder(q, q.proxy);
	if (holder != Reason::Ok)
		return holder;
	if (!spawnable_section(section))
		return Reason::BadSection;
	const spawn::ClassRule* rule = spawn::class_rule(pSettings->r_string(section, "class"));
	if (!rule || missing_item_line(section, *rule).line)
		return Reason::BadSection;
	const float pos[3] = {weapon->Position().x, weapon->Position().y, weapon->Position().z};
	Reason why = Reason::Ok;
	CSE_Abstract* addon = build_spawn(q, q.proxy, section, pos, nullptr, 0, why);
	if (!addon)
		return why;
	CSE_ALifeInventoryItem* inventory = smart_cast<CSE_ALifeInventoryItem*>(addon);
	if (!inventory)
	{
		F_entity_Destroy(addon);
		return Reason::NotAllowed;
	}
	inventory->m_fCondition = condition;
	// ItemState still accepts addon flags from the owner. Keep the existing spawn
	// policy/budget: this path must not turn a forged flag into unlimited free items.
	const spawn::SectionFacts facts = spawn_facts(section);
	spawn::CostBudget& budget = g_spawn_budgets.of(spawn_budget_key(q.proxy));
	const auto verdict = spawn::judge(rule, nullptr, facts, budget, g_spawn_policy, mgr().now_ms());
	if (verdict != spawn::SpawnVerdict::Allow)
	{
		F_entity_Destroy(addon);
		return verdict == spawn::SpawnVerdict::OverBudget ? Reason::RateLimited : Reason::NotAllowed;
	}
	const auto refund = [&]() { if (!spawn::budget_free(facts)) spawn::refund(budget, facts.cost, g_spawn_policy); };
	u16 created = kNoId;
	why = host_spawn(addon, q.proxy, created);
	if (why != Reason::Ok)
	{
		refund();
		return why; // The gun still has its silencer; no optimistic deletion or budget queue.
	}
	CSE_Abstract* result = world::server_entity(created);
	if (!result || result->ID_Parent != q.proxy || !weapon->Detach(section, false))
	{
		host_destroy(q, created);
		refund();
		return Reason::Engine;
	}
	items::host_silencer_detached(item, q.from);
	silencer_state(weapon, true);
	return Reason::Ok;
}

void host_on_silencer_detach(const Req& q, const ItemEventBody& body)
{
	Reader data(body.data, body.size);
	const std::string section = data.str();
	const float condition = data.f32();
	if (!data.ok() || data.remaining() || body.pay.paid || body.pay.mechanic != kNoId ||
	    !validate::resource_name_ok(section.c_str(), spawn::kMaxSection))
		return reply(q, Reason::Malformed, body.item);
	if (g_silencer_world != join::world_generation())
	{
		g_silencer_receipts.clear();
		g_silencer_world = join::world_generation();
	}
	auto& receipts = g_silencer_receipts[q.from];
	for (const SilencerReceipt& receipt : receipts)
		if (receipt.seq == q.seq)
			return reply(q, receipt.item == body.item && receipt.section == section ? receipt.reason : Reason::Malformed, body.item);
	const Reason why = allow(q.from, CatMisc) ? host_detach_silencer(q, body.item, section.c_str(), condition) : Reason::RateLimited;
	receipts.push_back({q.seq, body.item, section, why});
	if (receipts.size() > 128) receipts.pop_front();
	reply(q, why, body.item);
}
} // namespace

xr_set<u16> g_detaching_silencers;

float silencer_state(CWeaponMagazined* weapon, bool commit)
{
	lua_State* L = weapon ? ai().script_engine().lua() : nullptr;
	if (!L || !lua_checkstack(L, 8)) return -1.f;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, lua_silencer_state);
	::luabind::object(L, weapon->lua_game_object()).pushvalue();
	lua_pushboolean(L, commit);
	float condition = 1.f;
	if (lua_pcall(L, 2, 1, 0) != 0)
	{
		::Msg("! [coop] silencer metadata for weapon %u failed: %s", weapon->ID(), lua_tostring(L, -1));
		condition = -1.f;
	}
	else if (lua_isnumber(L, -1))
		condition = float(lua_tonumber(L, -1));
	lua_settop(L, top);
	return condition;
}

xr_map<u16, Rebuild> g_rebuilds; // local id -> (main thread)
u64 g_rebuilt = 0, g_rebuild_lost = 0;

void client_rebuild_item(u16 local_item)
{
	if (local_item == kNoId || local_item == kActor || g_rebuilds.size() >= kMaxRebuilds ||
	    g_rebuilds.count(local_item) || !is_live_mirror())
		return;
	CSE_Abstract* e = world::server_entity(local_item);
	CGameObject* go = world::object_here(local_item);
	if (!e || !go || go->getDestroy() || events::root_of(local_item) != kActor)
		return; // not ours, not online here: nothing of ours to correct
	Rebuild& rb = g_rebuilds[local_item];
	rb.host_id = join::to_host(local_item);
	rb.children.assign(e->children.begin(), e->children.end());
	rb.epoch = mgr().session().world_epoch();
	rb.at = mgr().now_ms();
	entities::ClientApply apply; // a host command: nothing goes to the host
	NET_Packet packet;
	packet.w_begin(M_EVENT);
	packet.w_u32(Level().timeServer());
	packet.w_u16(GE_DESTROY);
	packet.w_u16(local_item);
	script::keep_instance(local_item);
	for (u16 child : rb.children)
		script::keep_instance(child);
	Level().Send(packet, net_flags(TRUE, TRUE));
	++g_rebuilt;
	if (g_log.allow())
		::Msg("[coop] the host refused an upgrade of item %u: it is built again from the host's copy", local_item);
}

void client_pump_rebuilds()
{
	if (g_rebuilds.empty())
		return;
	const u64 now = mgr().now_ms();
	const u32 epoch = mgr().session().world_epoch();
	for (auto it = g_rebuilds.begin(); it != g_rebuilds.end();)
	{
		Rebuild& rb = it->second;
		const bool here = world::server_entity(it->first) != nullptr;
		if (rb.epoch != epoch || (rb.asked && here))
		{
			client_unkeep_rebuild(it->first, rb);
			it = g_rebuilds.erase(it); // another world (it came with the host's state), or back from the host
			continue;
		}
		if (!rb.asked)
		{
			if (here)
			{
				if (now - rb.at > kRebuildDestroyMs)
				{
					++g_rebuild_lost; // the local destroy did not happen: keep it as it is
					client_unkeep_rebuild(it->first, rb);
					it = g_rebuilds.erase(it);
					continue;
				}
				++it;
				continue;
			}
			client_ask_rebuild(rb); // gone here: the host's copy
			rb.asked = 1;
			rb.at = now;
		}
		else if (now - rb.at > kRebuildAskMs)
		{
			if (rb.asked >= kRebuildTries)
			{
				++g_rebuild_lost; // the host has it no more (or does not answer): its relays decide
				client_unkeep_rebuild(it->first, rb);
				it = g_rebuilds.erase(it);
				continue;
			}
			client_ask_rebuild(rb);
			++rb.asked;
			rb.at = now;
		}
		++it;
	}
}

UpgradeCounters g_upgrades;
xr_map<PeerId, xr_deque<SilencerReceipt>> g_silencer_receipts;
u32 g_silencer_world = 0;

void host_on_item_event(PeerId from, Reader& r)
{
	// host_begin drops a request silently while its player joins or a world is precached. The
	// client of an upgrade holds what it paid until the answer, so this one is answered: Offline (his world is being
	// replaced; the new one brings the host's state of his items), with the balance after it.
	if (mode().decides_here() && host_drops_requests_of(from))
	{
		Reader peek = r;
		Req dropped;
		dropped.from = from;
		dropped.type = Msg::ReqItemEvent;
		dropped.seq = peek.u32();
		ItemEventBody b;
		if (read_item_event(peek, b))
		{
			if (b.type == GE_ADDON_DETACH)
			{
				reply(dropped, Reason::Offline, b.item);
				return;
			}
			++g_upgrades.answered_dropped;
			answer_upgrade(dropped, Reason::Offline, b.item, b.pay.paid);
		}
	}
	Req q;
	if (!host_begin(from, r, Msg::ReqItemEvent, q))
		return;
	ItemEventBody b;
	if (!read_item_event(r, b))
		return reply(q, Reason::Malformed, kNoId);
	const u16 item = b.item;
	const u16 type = b.type;
	const u16 size = b.size;
	const u8* data = b.data;
	const ledger::UpgradePayment& pay = b.pay; // the event bytes are followed by the payment
	if (type == GE_ADDON_DETACH)
		return host_on_silencer_detach(q, b);
	const auto answer = [&q, item, &pay](Reason why) { answer_upgrade(q, why, item, pay.paid); };
	if (!allow(from, CatMisc))
		return answer(Reason::RateLimited); // with the balance, as every other answer
	if (type != GE_INSTALL_UPGRADE)
		return answer(Reason::NotAllowed);
	if (!world::server_entity(item))
		return answer(Reason::Missing);
	if (events::root_of(item) != q.proxy)
		return answer(Reason::NotOwner);
	// the upgrade id ends inside the event bytes (r_stringZ reads up to a zero) and is a name.
	if (!size || !std::memchr(data, 0, size))
		return answer(Reason::Malformed);
	shared_str upgrade;
	{
		NET_Packet probe;
		probe.B.count = size;
		CopyMemory(probe.B.data, data, size);
		probe.r_pos = 0;
		probe.r_stringZ(upgrade);
	}
	if (!validate::resource_name_ok(upgrade.c_str(), spawn::kMaxSection))
		return answer(Reason::Malformed);
	// bought at a mechanic nearby and paid enough, with the money on the host
	const Reason sold = host_sell_upgrade(q, upgrade, pay);
	if (sold != Reason::Ok)
		return answer(sold);
	// The server entity keeps it in the host save, and its next net_Spawn from scratch installs the section
	// defaults, its list, then it with loading = true: FATAL on a taken group or a missing parent group. A
	// repeat must not reach CSE_ALifeInventoryItem::add_upgrade either (FATAL on duplicates).
	CSE_Abstract* e = ai().alife().objects().object(item, true);
	const char* why = nullptr;
	switch (items::host_check_added_upgrade(e, upgrade, why))
	{
	case items::AddedUpgrade::AlreadyThere:
		++g_upgrades.already;
		return answer(Reason::Ok); // nothing to do, nothing charged: the balance tells his client
	case items::AddedUpgrade::Refused:
		if (note_bad(BadUpgrade))
			::Msg("! [coop] upgrade [%s] of item %u from player #%u refused: %s", upgrade.c_str(), item, from,
			    why ? why : "?");
		return answer(Reason::NotAllowed);
	case items::AddedUpgrade::Relay: break;
	}
	NET_Packet P;
	CGameObject::u_EventGen(P, type, item);
	P.w(data, size);
	CGameObject::u_EventSend(P); // the server entity keeps the upgrade in the host save (local server: at once)
	// charged only for what the entity took; an engine refusal costs nothing.
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(e);
	if (!ii || !ii->has_upgrade(upgrade))
	{
		++g_upgrades.engine;
		return answer(Reason::Engine);
	}
	++g_upgrades.bought;
	// The answer first, then the charge, whose MoneyBalance is the first after it (one reliable lane).
	reply(q, Reason::Ok, item);
	if (pay.paid && money::host_adjust(from, -s64(pay.paid), "upgrade"))
		g_upgrades.charged += pay.paid;
	// the host's game object of the item takes it now (the owner's ItemState never adds one),
	// and the other players' copies get the host's state of the item.
	items::host_upgrade_sold(item, from);
}

bool client_detach_silencer(CWeaponMagazined* weapon, const char* section)
{
	if (!is_mirror() || entities::client_applying())
		return false;
	if (!weapon || !is_live_mirror() || client_lease_waiting(weapon->ID()) ||
	    events::root_of(weapon->ID()) != kActor || g_detaching_silencers.count(weapon->ID()))
		return true;
	const float condition = silencer_state(weapon, false);
	if (!_valid(condition) || condition < 0.f || condition > 1.f)
		return true;
	items::client_send_state_now(weapon->ID()); // Includes an attach performed earlier in this same frame.
	Action action;
	action.kind = Act::ItemEvent;
	action.item = weapon->ID();
	action.ge_type = GE_ADDON_DETACH;
	action.section = section;
	Writer body;
	body.str(section);
	body.f32(condition);
	action.bytes.assign(body.data().begin(), body.data().end());
	g_detaching_silencers.insert(weapon->ID());
	push(std::move(action));
	return true;
}
} // namespace requests
} // namespace coop
