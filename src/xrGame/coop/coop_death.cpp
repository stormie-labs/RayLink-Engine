// Anomaly Coop — what a player's death costs, on the host. See coop_death.h.
#include "stdafx.h"
#include "coop_death.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_alife.h"
#include "coop_events.h"
#include "coop_money.h"
#include "coop_script.h"
#include "coop_survival.h"
#include "xrCoop/coop_death_items.h"
#include "xrCoop/coop_ledger.h"

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "Level.h"
#include "GameObject.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryBox.h"
#include "WeaponAmmo.h"
#include "CustomOutfit.h"
#include "ActorHelmet.h"
#include "PDA.h"
#include "ai_space.h"
#include "level_graph.h"
#include "game_graph.h"
#include "ai_object_location.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_time_manager.h"
#include "xrServer_Objects_ALife.h"
#include "../xrServerEntities/xrMessages.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace coop
{
namespace death
{
namespace
{
constexpr u16 kNoId = 0xffff;
constexpr u64 kStashWaitMs = 5000; // plan T9 step 3: the stash has this long to come online
constexpr size_t kMaxPendingStashes = 16; // one per death; more at once falls back to dropping
constexpr size_t kMaxMarkers = 32;
constexpr size_t kMaxPreviewLine = 900; // section lists in coop_death_preview are cut here

const char* const kShareSection = "coop_death_drop";
const char* const kKindSection = "coop_death_kinds";
const char* const kStashSection = "coop_death_stash";
// The plan's shares (T9 step 1), used for a line the ltx does not have.
const float kDefaultShares[kCategories] = {0.5f, 0.5f, 0.5f, 0.25f, 0.1f, 0.1f, 0.25f};

// ---------------------------------------------------------------- settings

struct Settings
{
	bool loaded = false;
	Shares shares{};
	KindTable kinds;
	bool stash_section = false; // [coop_death_stash] exists: a stash can be spawned
};
Settings g_set;

void load_settings()
{
	g_set.loaded = true;
	g_set.kinds.clear();
	for (u8 i = 0; i < kCategories; ++i)
		g_set.shares[i] = kDefaultShares[i];

	if (pSettings->section_exist(kShareSection))
	{
		for (u8 i = 0; i < kCategories; ++i)
			if (pSettings->line_exist(kShareSection, category_name(i)))
			{
				const float v = pSettings->r_float(kShareSection, category_name(i));
				if (!(v >= 0.f && v <= 1.f))
					::Msg("! [coop] [%s] %s = %f is outside 0..1: cut to the range", kShareSection, category_name(i), v);
				g_set.shares[i] = v; // pick_losses cuts it to 0..1 (NaN -> 0)
			}
	}
	else
		::Msg("! [coop] [%s] not found (configs\\mod_system_coop_death.ltx): the default shares apply", kShareSection);

	if (pSettings->section_exist(kKindSection))
	{
		const u32 n = pSettings->line_count(kKindSection);
		for (u32 i = 0; i < n; ++i)
		{
			LPCSTR name = nullptr;
			LPCSTR value = nullptr;
			if (!pSettings->r_line(kKindSection, int(i), &name, &value))
				continue;
			if (!g_set.kinds.add(name, value))
				::Msg("! [coop] [%s] %s = %s skipped: an unknown category, an empty or overlong kind, or more than %u "
				      "kinds",
				    kKindSection, name ? name : "", value ? value : "", u32(KindTable::kMaxKinds));
		}
	}
	else
		::Msg("! [coop] [%s] not found: items are sorted by class only (ammo, outfits; the rest is 'other')",
		    kKindSection);

	g_set.stash_section = !!pSettings->section_exist(kStashSection);
	if (!g_set.stash_section)
		::Msg("! [coop] [%s] not found: the corpse mode drops the backpack at the body instead", kStashSection);

	::Msg("[coop] death losses: materials %.2f, ammo %.2f, consumables %.2f, gear %.2f, artefacts %.2f, tools %.2f, "
	      "other %.2f; %u item kinds",
	    g_set.shares[Materials], g_set.shares[Ammo], g_set.shares[Consumables], g_set.shares[Gear],
	    g_set.shares[Artefacts], g_set.shares[Tools], g_set.shares[Other], u32(g_set.kinds.size()));
}

void ensure_settings()
{
	if (!g_set.loaded)
		load_settings();
}

// ---------------------------------------------------------------- state

// A stash spawned for a character, waiting to come online before his items go in.
struct PendingStash
{
	u16 stash = kNoId;
	u16 character = kNoId;
	// The character's player key digest (ledger::character_key): the id alone says nothing about who plays it once
	// the character is gone and the id is given to another one (F2-13).
	std::string key;
	std::vector<u16> items; // host ids, as stash_ids gave them
	u64 deadline = 0;
};
xr_vector<PendingStash> g_stashes;

struct QueuedMarker
{
	Marker m;
	u32 frame = 0; // Device.dwFrame it was queued in: sent from the next frame on
};
xr_vector<QueuedMarker> g_markers;

u32 g_epoch = 0xffffffff; // join::host_world_epoch() the pending work belongs to

struct Stats
{
	u64 deaths = 0, dropped = 0, moved = 0, destroyed = 0, stashes = 0, stash_fallbacks = 0, stash_lost = 0;
	u64 markers_sent = 0, markers_in = 0, markers_refused = 0, markers_dropped = 0;
	u64 money_taken = 0;
};
Stats g_st;
u32 g_log_count = 0;

bool log_allowed() { return ++g_log_count <= 20 || (g_log_count % 100) == 0; }

u64 now_ms() { return mgr().now_ms(); }

// ---------------------------------------------------------------- engine helpers

CGameObject* game_object(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CGameObject* go = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return go && !go->getDestroy() ? go : nullptr;
}

// Parent of an entity in the server state: every ownership event changes it at once (game objects follow a round
// later), so this is where an item is right now on the host.
u16 server_parent(u16 id)
{
	CSE_Abstract* e = events::find_entity(id);
	return e ? e->ID_Parent : kNoId;
}

// The key of a character: the player key digest of its custom data (as the money book keys it), "#<id>" without one.
std::string character_key_of(u16 character)
{
	const CSE_Abstract* e = events::find_entity(character);
	std::string key = e && e->m_ini_string.size() ? ledger::character_key(e->m_ini_string.c_str()) : std::string();
	if (key.empty())
		key = "#" + std::to_string(u32(character));
	return key;
}

// A player character online on this machine: the host's actor (0) or a player's proxy.
bool player_character(u16 character, CGameObject*& body, CInventoryOwner*& owner)
{
	body = nullptr;
	owner = nullptr;
	if (character != 0 && !join::is_proxy(character))
		return false;
	CGameObject* go = game_object(character);
	CInventoryOwner* io = go ? smart_cast<CInventoryOwner*>(go) : nullptr;
	if (!io)
		return false;
	body = go;
	owner = io;
	return true;
}

xr_string nick(u16 character) { return mgr().active() ? survival::nick_of(character) : xr_string("me"); }

// The host's actor sees its own slots and belt; a proxy does not show where its player keeps his things.
bool layout_known(u16 character) { return character == 0; }

// The facts of the backpack (m_ruck) or of everything the character carries (m_all).
std::vector<ItemFacts> collect(CInventoryOwner* io, bool everything)
{
	std::vector<ItemFacts> out;
	const TIItemContainer& src = everything ? io->inventory().m_all : io->inventory().m_ruck;
	out.reserve(src.size());
	for (PIItem it : src)
	{
		if (!it || it->object().getDestroy())
			continue;
		CGameObject& go = it->object();
		ItemFacts f;
		f.id = go.ID();
		LPCSTR section = go.cNameSect().c_str();
		if (section && pSettings->line_exist(section, "kind"))
		{
			LPCSTR kind = pSettings->r_string(section, "kind");
			f.kind = kind ? kind : "";
		}
		f.ammo = it->cast_weapon_ammo() != nullptr;
		f.outfit = smart_cast<CCustomOutfit*>(it) != nullptr || smart_cast<CHelmet*>(it) != nullptr;
		f.quest = !!it->IsQuestItem();
		f.pda = smart_cast<CPda*>(it) != nullptr;
		f.helper = it->is_helper_item();
		f.world = ai().get_alife() && ai().alife().objects().object(f.id, true) != nullptr;
		f.wearable = it->BaseSlot() != NO_ACTIVE_SLOT || it->Belt();
		out.push_back(f);
	}
	return out;
}

const char* section_of(u16 id)
{
	CGameObject* go = game_object(id);
	return go ? go->cNameSect().c_str() : "?";
}

// Where the things of a dead character go (Review Focus 4) and the level / game vertex a stash is spawned on.
struct BodyPlace
{
	DropPlace drop;
	u32 level_vertex = u32(-1);
	GameGraph::_GRAPH_ID game_vertex = GameGraph::_GRAPH_ID(-1);
};

BodyPlace body_place(CGameObject* body)
{
	BodyPlace b;
	const Fvector p = body->Position();
	const Point at{p.x, p.y, p.z};
	const u32 own = body->ai_location().level_vertex_id();
	bool own_valid = false;
	bool found = false;
	Point vertex;
	if (ai().get_level_graph())
	{
		const CLevelGraph& graph = ai().level_graph();
		own_valid = graph.valid_vertex_id(own);
		u32 v = own;
		if (!own_valid && _valid(p))
			v = graph.vertex(u32(-1), p); // the nearest vertex (a full search: once per death)
		if (graph.valid_vertex_id(v))
		{
			const Fvector vp = graph.vertex_position(v);
			vertex = Point{vp.x, vp.y, vp.z};
			found = true;
			b.level_vertex = v;
		}
	}
	b.drop = drop_place(at, own_valid, found, vertex);
	const GameGraph::_GRAPH_ID gv = body->ai_location().game_vertex_id();
	if (ai().get_game_graph() && ai().game_graph().valid_vertex_id(gv))
		b.game_vertex = gv;
	else if (CSE_ALifeDynamicObject* e = ai().get_alife() ? ai().alife().objects().object(body->ID(), true) : nullptr)
		if (ai().get_game_graph() && ai().game_graph().valid_vertex_id(e->m_tGraphID))
			b.game_vertex = e->m_tGraphID;
	return b;
}

// Drop: the character lets the item go, as the inventory UI does. Put at the drop point when it was moved.
bool drop_one(u16 character, u16 item, const DropPlace& place, size_t index)
{
	if (server_parent(item) != character)
		return false; // not his any more (or gone): nothing to drop, nothing to duplicate
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_OWNERSHIP_REJECT, character);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
	if (server_parent(item) != kNoId)
		return false;
	if (place.moved)
	{
		const Point off = spread(index);
		Fvector at;
		at.set(place.at.x + off.x, place.at.y + off.y, place.at.z + off.z);
		CGameObject::u_EventGen(P, GE_CHANGE_POS, item); // after the reject in the same queue: it has its shell then
		P.w_vec3(at);
		CGameObject::u_EventSend(P);
		events::host_set_drop_position(item, at); // clients put it there too (EntityParent flag 1)
	}
	return true;
}

// Drops `items` of `character`; the id of the first one that fell, kNoId if none did.
u16 drop_all(u16 character, CGameObject* body, const std::vector<u16>& items, u32& count)
{
	count = 0;
	u16 first = kNoId;
	const BodyPlace place = body_place(body);
	for (u16 id : items)
		if (drop_one(character, id, place.drop, count))
		{
			if (first == kNoId)
				first = id;
			++count;
		}
	g_st.dropped += count;
	if (count)
		::Msg("[coop] death: %u items of %s dropped at (%.1f, %.1f, %.1f)%s", count, nick(character).c_str(),
		    place.drop.at.x, place.drop.at.y, place.drop.at.z,
		    place.drop.moved ? ", above the level vertex (the body is under the ground or off the level graph)" : "");
	return first;
}

void move_item(u16 from, u16 to, u16 item)
{
	NET_Packet P;
	CGameObject::u_EventGen(P, GE_TRADE_SELL, from);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
	CGameObject::u_EventGen(P, GE_TRADE_BUY, to);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
}

// An unused stash goes the normal way: online through GE_DESTROY, offline through ALife. One online without a game
// object (being spawned right now) stays: an empty stash.
void release_stash(u16 id)
{
	if (CGameObject* go = game_object(id))
	{
		go->DestroyObject();
		return;
	}
	CSE_ALifeDynamicObject* d = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
	if (d && !d->m_bOnline)
		alife_rw().release(d, true);
	else if (d)
		::Msg("! [coop] death stash %u is coming online now: left empty", id);
}

void queue_marker(u16 character, u16 object, Drop drop)
{
	if (object == kNoId || drop == Drop::Lose)
		return;
	if (g_markers.size() >= kMaxMarkers)
	{
		++g_st.markers_dropped;
		::Msg("! [coop] death marker of %u (object %u) not sent: %u markers waiting", character, object,
		    u32(g_markers.size()));
		return;
	}
	QueuedMarker q;
	q.m.character = character;
	q.m.object = object;
	q.m.drop = drop;
	q.frame = Device.dwFrame;
	g_markers.push_back(q);
}

void fire_marker_callback(u16 player_local, u16 object_local, Drop drop)
{
	script::queue_callback("coop_death_marker",
	    {script::cb_int(player_local), script::cb_int(object_local), script::cb_str(marker_kind(drop))});
}

// The items' EntityParent / the stash's EntitySpawn go out in this frame's events / entities pass, after this module:
// a marker waits one frame so that every client has its object before the marker names it.
void send_markers()
{
	for (size_t i = 0; i < g_markers.size();)
	{
		if (g_markers[i].frame == Device.dwFrame)
		{
			++i;
			continue;
		}
		const Marker& m = g_markers[i].m;
		Writer w(kMarkerSize);
		write(w, m);
		net::host_broadcast(Msg::DeathMarker, w);
		fire_marker_callback(m.character, m.object, m.drop); // host numbering is the local one here
		++g_st.markers_sent;
		g_markers.erase(g_markers.begin() + i);
	}
}

// Spawns the stash and queues the move; falls back to dropping. Returns the stash id (kNoId if none); `count`: items
// on their way to the stash or dropped.
u16 stash_for(u16 character, CGameObject* body, CInventoryOwner* io, bool everything, u32& count)
{
	count = 0;
	const std::vector<ItemFacts> facts = collect(io, everything);
	const std::vector<u16> picked = stash_ids(facts, g_set.kinds, everything, layout_known(character));
	if (picked.empty())
	{
		::Msg("[coop] death: %s has nothing for a stash", nick(character).c_str());
		return kNoId;
	}
	const BodyPlace place = body_place(body);

	const char* why = nullptr;
	if (!g_set.stash_section)
		why = "no [coop_death_stash] section";
	else if (g_stashes.size() >= kMaxPendingStashes)
		why = "too many stashes waiting";
	else if (!ai().get_alife())
		why = "no ALife";
	else if (place.level_vertex == u32(-1) || place.game_vertex == GameGraph::_GRAPH_ID(-1))
		why = "no level or game vertex at the body";
	CSE_Abstract* e = nullptr;
	if (!why)
	{
		Fvector at;
		at.set(place.drop.at.x, place.drop.at.y, place.drop.at.z);
		e = alife_rw().spawn_item(kStashSection, at, place.level_vertex, place.game_vertex, kNoId);
		if (!e)
			why = "spawn failed";
	}
	if (why)
	{
		::Msg("! [coop] death stash of %s not made (%s): the items drop at the body", nick(character).c_str(), why);
		++g_st.stash_fallbacks;
		const u16 first = drop_all(character, body, picked, count);
		queue_marker(character, first, Drop::Drop);
		return kNoId;
	}

	PendingStash p;
	p.stash = e->ID;
	p.character = character;
	p.key = character_key_of(character);
	p.items = picked;
	p.deadline = now_ms() + kStashWaitMs;
	count = u32(p.items.size());
	g_stashes.push_back(std::move(p));
	++g_st.stashes;
	::Msg("[coop] death stash %u for %s at (%.1f, %.1f, %.1f): %u items go in when it is online%s", e->ID,
	    nick(character).c_str(), place.drop.at.x, place.drop.at.y, place.drop.at.z, count,
	    everything ? " (everything he has)" : "");
	return e->ID;
}

// The stash is online: the items go in, as the stock corpse search window moves them (move_item_from_to).
void fill_stash(PendingStash& p)
{
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (character_key_of(p.character) != p.key || !player_character(p.character, body, io))
	{
		::Msg("! [coop] death stash %u: character %u is gone or offline, his items stay with him", p.stash,
		    p.character);
		++g_st.stash_lost;
		release_stash(p.stash);
		return;
	}
	u32 moved = 0;
	for (u16 item : p.items)
	{
		if (server_parent(item) != p.character)
			continue;
		move_item(p.character, p.stash, item);
		if (server_parent(item) == p.stash)
			++moved;
	}
	g_st.moved += moved;
	::Msg("[coop] death stash %u: %u of %u items of %s are in", p.stash, moved, u32(p.items.size()),
	    nick(p.character).c_str());
	if (!moved)
	{
		release_stash(p.stash);
		return;
	}
	queue_marker(p.character, p.stash, Drop::Corpse);
}

// The stash did not come online in time: it goes, the items drop at the body.
void stash_timed_out(PendingStash& p)
{
	::Msg("! [coop] death stash %u not online after %u s: the items of %s drop at the body", p.stash,
	    u32(kStashWaitMs / 1000), nick(p.character).c_str());
	++g_st.stash_fallbacks;
	release_stash(p.stash);
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (character_key_of(p.character) != p.key || !player_character(p.character, body, io))
	{
		++g_st.stash_lost;
		::Msg("! [coop] death stash %u: character %u is gone or offline, his items stay with him", p.stash,
		    p.character);
		return;
	}
	u32 count = 0;
	const u16 first = drop_all(p.character, body, p.items, count);
	queue_marker(p.character, first, Drop::Drop);
}

void host_frame()
{
	const u32 epoch = join::host_world_epoch();
	if (epoch != g_epoch)
	{
		if (g_epoch != 0xffffffff && (!g_stashes.empty() || !g_markers.empty()))
			::Msg("[coop] death: a new world, %u stashes and %u markers of the old one dropped", u32(g_stashes.size()),
			    u32(g_markers.size()));
		g_epoch = epoch;
		g_stashes.clear();
		g_markers.clear();
	}
	if (!g_stashes.empty())
	{
		const u64 t = now_ms();
		for (size_t i = 0; i < g_stashes.size();)
		{
			PendingStash& p = g_stashes[i];
			if (smart_cast<CInventoryBox*>(game_object(p.stash)))
				fill_stash(p);
			else if (t >= p.deadline || !events::find_entity(p.stash))
				stash_timed_out(p);
			else
			{
				++i;
				continue;
			}
			g_stashes.erase(g_stashes.begin() + i);
		}
	}
	if (!g_markers.empty())
		send_markers();
}

// ---------------------------------------------------------------- client

void on_marker_msg(PeerId from, Reader& r)
{
	if (mgr().is_host())
		return; // host-only: the session refuses it from a client before any handler
	if (!mgr().is_live_client() || from != kHostPeer)
	{
		++g_st.markers_refused;
		return;
	}
	Marker m;
	if (!read(r, m))
	{
		++g_st.markers_refused;
		if (log_allowed())
			::Msg("! [coop] DeathMarker from the host refused: short, no id, or a mode without a marker");
		return;
	}
	++g_st.markers_in;
	fire_marker_callback(join::to_local(m.character), join::to_local(m.object), m.drop);
}

// ---------------------------------------------------------------- console

class CCC_CoopDeathPreview : public IConsole_Command
{
public:
	CCC_CoopDeathPreview(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR args) override
	{
		unsigned long id = 0;
		if (args && *args)
		{
			char* end = nullptr;
			id = strtoul(args, &end, 10);
			if (end == args || id >= kNoId)
			{
				::Msg("! [coop] coop_death_preview [character id]: 0 = the host, a player's proxy id otherwise");
				return;
			}
		}
		host_preview(u16(id));
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "host: log what a death of character N (default 0, the host) would take now; moves nothing");
	}
};
} // namespace

// ---------------------------------------------------------------- public

u32 host_apply_losses(u16 character, u16 seq, Drop mode)
{
	if (!mgr().is_host())
		return 0;
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (!player_character(character, body, io))
	{
		::Msg("! [coop] death losses of %u (seq %u) skipped: not a player character online here", character, seq);
		return 0;
	}
	ensure_settings();
	++g_st.deaths;

	if (mode == Drop::Corpse)
	{
		u32 count = 0;
		stash_for(character, body, io, false, count);
		return count;
	}

	const std::vector<Candidate> candidates =
	    loss_candidates(collect(io, false), g_set.kinds, layout_known(character));
	const u64 game_time = ai().get_alife() ? u64(ai().alife().time_manager().game_time()) : 0;
	const u32 seed = loss_seed(character, seq, game_time);
	const std::vector<u16> picked = pick_losses(candidates, g_set.shares, seed);
	if (picked.empty())
	{
		::Msg("[coop] death of %s (seq %u): nothing of %u backpack items is lost", nick(character).c_str(), seq,
		    u32(candidates.size()));
		return 0;
	}

	if (mode == Drop::Lose)
	{
		u32 count = 0;
		for (u16 id : picked)
		{
			if (server_parent(id) != character)
				continue;
			CGameObject* go = game_object(id);
			if (!go)
				continue;
			go->DestroyObject(); // a world item: the host removes it, clients follow (as a used medkit)
			++count;
		}
		g_st.destroyed += count;
		::Msg("[coop] death of %s (seq %u): %u of %u backpack items are gone", nick(character).c_str(), seq, count,
		    u32(candidates.size()));
		return count;
	}

	u32 count = 0;
	const u16 first = drop_all(character, body, picked, count);
	::Msg("[coop] death of %s (seq %u): %u of %u backpack items dropped", nick(character).c_str(), seq, count,
	    u32(candidates.size()));
	queue_marker(character, first, Drop::Drop);
	return count;
}

u16 host_move_to_stash(u16 character, bool everything)
{
	if (!mgr().is_host())
		return kNoId;
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (!player_character(character, body, io))
	{
		::Msg("! [coop] death stash of %u not made: not a player character online here", character);
		return kNoId;
	}
	ensure_settings();
	u32 count = 0;
	return stash_for(character, body, io, everything, count);
}

bool host_stash_pending(u16 character)
{
	for (const PendingStash& p : g_stashes)
		if (p.character == character)
			return true;
	return false;
}

u32 host_take_money(u16 character, u8 pct)
{
	if (!mgr().is_host() || pct == 0)
		return 0;
	const PeerId peer = character == 0 ? kHostPeer : join::peer_of_proxy(character);
	if (peer == kInvalidPeer)
	{
		::Msg("! [coop] death money of %u not taken: no player plays this character now", character);
		return 0;
	}
	const u32 balance = money::host_balance(peer);
	const u32 take = u32(u64(balance) * u64(pct > 100 ? 100 : pct) / 100);
	if (!take)
		return 0;
	if (!money::host_adjust(peer, -s64(take), "death"))
		return 0;
	g_st.money_taken += take;
	::Msg("[coop] death of %s: %u RU of %u taken (%u%%)", nick(character).c_str(), take, balance, u32(pct));
	return take;
}

void host_preview(u16 character)
{
	if (mgr().is_client())
	{
		::Msg("! [coop] coop_death_preview: the host decides deaths, run it there");
		return;
	}
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (!g_pGameLevel || !player_character(character, body, io))
	{
		::Msg("! [coop] coop_death_preview: %u is not a player character in this level (0 = the host)", character);
		return;
	}
	ensure_settings();
	const bool known = layout_known(character);
	const std::vector<ItemFacts> backpack = collect(io, false);
	const std::vector<Candidate> candidates = loss_candidates(backpack, g_set.kinds, known);
	const u64 game_time = ai().get_alife() ? u64(ai().alife().time_manager().game_time()) : 0;
	const u32 seed = loss_seed(character, 0, game_time);
	const std::vector<u16> picked = pick_losses(candidates, g_set.shares, seed);
	::Msg("[coop] death preview of %s (character %u): %u backpack items, %u may be lost (the rest: quest items, the "
	      "PDA%s)",
	    nick(character).c_str(), character, u32(backpack.size()), u32(candidates.size()),
	    known ? "" : ", what the player may have on: the host does not see his slots and belt");
	::Msg("[coop]   drop / lose now (seed %u): %u items", seed, u32(picked.size()));
	for (u8 c = 0; c < kCategories; ++c)
	{
		u32 total = 0;
		for (const Candidate& x : candidates)
			total += x.category == c ? 1 : 0;
		if (!total)
			continue;
		xr_string list;
		u32 taken = 0;
		for (u16 id : picked)
		{
			bool in = false;
			for (const Candidate& x : candidates)
				if (x.id == id)
				{
					in = x.category == c;
					break;
				}
			if (!in)
				continue;
			++taken;
			if (list.size() < kMaxPreviewLine)
			{
				char one[96];
				xr_sprintf(one, "%s%s (%u)", list.empty() ? "" : ", ", section_of(id), u32(id));
				list += one;
			}
			else if (list.size() < kMaxPreviewLine + 4)
				list += ", ...";
		}
		::Msg("[coop]     %s: %u of %u (share %.2f)%s%s", category_name(c), taken, total, g_set.shares[c],
		    list.empty() ? "" : ": ", list.c_str());
	}
	const u32 corpse = u32(stash_ids(backpack, g_set.kinds, false, known).size());
	const u32 all = u32(stash_ids(collect(io, true), g_set.kinds, true, known).size());
	const BodyPlace place = body_place(body);
	::Msg("[coop]   corpse: %u items into a stash; hardcore retirement: %u; drop point (%.1f, %.1f, %.1f)%s", corpse,
	    all, place.drop.at.x, place.drop.at.y, place.drop.at.z,
	    place.drop.moved ? " above the level vertex (the body is under the ground or off the level graph)" : " (the body)");
}

void install(Session& s) { s.on(Msg::DeathMarker, &on_marker_msg); }

void reset()
{
	g_stashes.clear(); // a stash spawned but not filled stays empty; the items stay with their character
	g_markers.clear();
	g_epoch = 0xffffffff;
	g_st = Stats();
	g_log_count = 0;
	g_set.loaded = false; // read again at the first death (DLTX may differ between game starts)
}

void frame()
{
	if (mgr().is_host())
		host_frame();
}

void print_status()
{
	if (mgr().is_host())
	{
		::Msg("[coop]   death: %llu deaths charged; items %llu dropped, %llu destroyed, %llu into stashes; %llu stashes "
		      "(%llu fell back to dropping, %llu lost their character), %u waiting; %llu markers sent (%llu not), "
		      "%llu RU taken",
		    g_st.deaths, g_st.dropped, g_st.destroyed, g_st.moved, g_st.stashes, g_st.stash_fallbacks, g_st.stash_lost,
		    u32(g_stashes.size()), g_st.markers_sent, g_st.markers_dropped, g_st.money_taken);
		for (const PendingStash& p : g_stashes)
			::Msg("[coop]     stash %u of %s: %u items, %lld ms left", p.stash, nick(p.character).c_str(),
			    u32(p.items.size()), s64(p.deadline) - s64(now_ms()));
	}
	else if (mgr().is_client())
		::Msg("[coop]   death: %llu markers from the host, %llu refused", g_st.markers_in, g_st.markers_refused);
}

void register_console() { CMD1(CCC_CoopDeathPreview, "coop_death_preview"); }
} // namespace death
} // namespace coop
