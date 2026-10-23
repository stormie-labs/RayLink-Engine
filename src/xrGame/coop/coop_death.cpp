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
#include "coop_trade.h"
#include "coop_world.h"
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
#include "alife_graph_registry.h"
#include "alife_time_manager.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "../xrServerEntities/xrMessages.h"

#include <algorithm>
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
constexpr size_t kMaxPendingStashes = 16; // one per death; more at once falls back to dropping
constexpr size_t kMaxMarkers = 32;
constexpr size_t kMaxPreviewLine = 900; // section lists in coop_death_preview are cut here

const char* const kShareSection = "coop_death_drop";
const char* const kKindSection = "coop_death_kinds";
const char* const kStashSection = "coop_death_stash";
const char* const kHardcoreSection = "coop_hardcore"; // Ruling F3-10: new_kit
// The plan's shares (T9 step 1), used for a line the ltx does not have.
const float kDefaultShares[kCategories] = {0.5f, 0.5f, 0.5f, 0.25f, 0.1f, 0.1f, 0.25f};

// ---------------------------------------------------------------- settings

struct Settings
{
	bool loaded = false;
	Shares shares{};
	KindTable kinds;
	bool stash_section = false; // [coop_death_stash] exists: a stash can be spawned
	bool new_kit = false; // Ruling F3-10: [coop_hardcore] new_kit = 1 (a mod pack's default; the console may turn it on)
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
	g_set.new_kit = pSettings->line_exist(kHardcoreSection, "new_kit") && pSettings->r_bool(kHardcoreSection, "new_kit");

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
	// Who it is for (fix round 1, review I1): the player and the key of the character when the stash was made. The id
	// alone says nothing once the character is released and the id given to another one (F2-13); the key alone is
	// rewritten by Task 10 for a retiring character, the peer alone is gone when the player leaves (same_owner).
	StashOwner owner;
	std::vector<u16> items; // host ids, as stash_ids gave them
	u64 deadline = 0; // the stash must be online by then while the character is here (stash_presence restarts it)
	u64 give_up_at = 0; // a player who left: the move waits for him (or for both to go offline) until then
	bool away = false; // the character was not online at the last check
	DropPlace died; // where the stash was made: a fallback drop puts the items there (fallback_place)
	Drop marker = Drop::Corpse; // the DeathMarker mode once filled: Drop for the bag of a drop death (Ruling L-1)
};
xr_vector<PendingStash> g_stashes;

// Ruling L-1, carried from F3 T15 (concern 3): an emptied death stash is looked for once a second and released.
constexpr u64 kBagPollMs = 1000;
u64 g_next_bag_poll = 0;

struct QueuedMarker
{
	Marker m;
	u32 frame = 0; // Device.dwFrame it was queued in: sent from the next frame on
};
xr_vector<QueuedMarker> g_markers;

u32 g_epoch = 0xffffffff; // join::host_world_epoch() the pending work belongs to
// Fix round 1, I1: characters whose last stash gave up (give_up): his things stayed on him (host_stash_outcome). A new
// stash for him (host_move_to_stash) starts over.
xr_set<u16> g_gave_up;
// Ruling F3-10: console coop_hardcore_new_kit (host): the new character of a retired key gets the kit and the money.
int g_cv_new_kit = 0;

struct Stats
{
	u64 deaths = 0, dropped = 0, moved = 0, destroyed = 0, stashes = 0, stash_fallbacks = 0, stash_lost = 0;
	u64 stash_abandoned = 0; // released empty at the end of a session or on a new world (review minor 5)
	u64 bags_released = 0; // Ruling L-1: emptied by the players, released by the host (release_empty_bags)
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

// Who plays `character` now and the player key digest on its custom data (as the money book keys it; empty if none).
StashOwner owner_of(u16 character)
{
	StashOwner o;
	o.peer = character == 0 ? kHostPeer : join::peer_of_proxy(character);
	const CSE_Abstract* e = events::find_entity(character);
	if (e && e->m_ini_string.size())
		o.key = ledger::character_key(e->m_ini_string.c_str());
	return o;
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

// What the host knows of the things of one character and what it may choose from (F3 T8b, Ruling F3-4).
struct Carried
{
	std::vector<ItemFacts> items;
	bool layout_known = false; // the host's own actor, or a client's proxy whose owner reported what he has on
	bool reported = false; // the second case: `items` is everything the proxy carries and `worn` of them are on him
	bool everything = false; // a hardcore retirement: all of it goes, worn or not
	size_t worn = 0;
};

// The host's own actor: its backpack (m_ruck) is exact, its slots and belt are not in it.
// A client's proxy is a CAI_Stalker: it cannot wear an outfit, has no belt and fills its slots in the order it took
// the items, so m_ruck says nothing of what the player has on. With the list his Dead report brought (seq: the death,
// survival::host_worn_items) the proxy offers everything it carries (m_all: the NPC's slots hold part of his backpack)
// except what the list names, and the layout counts as known. Without it (an old client, a report whose tail was
// refused, no seq): the backpack only, and nothing the player may have on is offered.
// `everything` (a hardcore retirement) takes all of it, so the list is not asked for.
Carried carried_of(u16 character, CInventoryOwner* io, bool everything, u16 seq)
{
	Carried c;
	c.layout_known = character == 0;
	c.everything = everything;
	xr_vector<u16> reported;
	// T8b review N2: an empty list is no list - a live player always carries a knife and a PDA, nothing on him means his
	// inventory was not read, and his armour must not become a candidate by it
	if (!everything && character != 0 && seq != kNoSeq && survival::host_worn_items(character, seq, reported) &&
	    !reported.empty())
	{
		c.items = collect(io, true);
		c.worn = mark_worn(c.items, std::vector<u16>(reported.begin(), reported.end()));
		c.layout_known = true;
		c.reported = true;
		return c;
	}
	c.items = collect(io, everything);
	return c;
}

// For the log: how the layout of a client's proxy was known; nothing for the host's actor.
xr_string layout_note(const Carried& c, u16 character)
{
	if (character == 0 || c.everything)
		return xr_string();
	char buf[112];
	if (c.reported)
		xr_sprintf(buf, " [his list: %u on him stay]", u32(c.worn));
	else
		xr_strcpy(buf, " [no list from his machine: what he may wear stays]");
	return buf;
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

// Drops `items` of `character` at `place`; the id of the first one that fell, kNoId if none did.
u16 drop_all_at(u16 character, const std::vector<u16>& items, const DropPlace& place, u32& count)
{
	count = 0;
	u16 first = kNoId;
	for (u16 id : items)
		if (drop_one(character, id, place, count))
		{
			if (first == kNoId)
				first = id;
			++count;
		}
	g_st.dropped += count;
	if (count)
		::Msg("[coop] death: %u items of %s dropped at (%.1f, %.1f, %.1f)%s", count, nick(character).c_str(),
		    place.at.x, place.at.y, place.at.z,
		    place.moved ? ", put there (the body is under the ground, off the level graph or away from it)" : "");
	return first;
}

u16 drop_all(u16 character, CGameObject* body, const std::vector<u16>& items, u32& count)
{
	return drop_all_at(character, items, body_place(body).drop, count);
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

// Ruling L-1, carried from F3 T15 (concern 4): the stash names its owner in its custom data ([coop_death_stash] nick /
// key, stash_owner_to_ini), written when it is spawned, before it goes online or into a save. The host's Lua reads it
// back when it enters a world (restore_stash_markers), from an entity made anew out of the save.
void write_owner(CSE_Abstract* e, u16 character, const StashOwner& owner)
{
	const xr_string n = survival::nick_of(character);
	std::string text = e->m_ini_string.size() ? e->m_ini_string.c_str() : "";
	stash_owner_to_ini(text, n == "?" ? std::string() : std::string(n.c_str()), owner.key);
	e->m_ini_string = text.c_str();
}

// Spawns a stash at the body of `character` for `picked` (his items, host ids) and queues the move; `marker`: the
// DeathMarker mode once they are in (Drop: the bag of a drop death's share, Ruling L-1; Corpse: the backpack, or all
// he has when a hardcore character retires). `note`: for the log. Falls back to dropping them at the body. Returns the
// stash id (kNoId if none); `count`: items on their way to the stash or dropped.
u16 stash_items(u16 character, CGameObject* body, const std::vector<u16>& picked, Drop marker, const xr_string& note,
    u32& count)
{
	count = 0;
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
	p.owner = owner_of(character);
	p.items = picked;
	p.deadline = now_ms() + kStashWaitMs;
	p.give_up_at = now_ms() + kStashAwayWaitMs;
	p.died = place.drop;
	p.marker = marker;
	write_owner(e, character, p.owner);
	count = u32(p.items.size());
	g_stashes.push_back(std::move(p));
	++g_st.stashes;
	::Msg("[coop] death stash %u (%s) for %s at (%.1f, %.1f, %.1f): %u items go in when it is online%s", e->ID,
	    marker_kind(marker), nick(character).c_str(), place.drop.at.x, place.drop.at.y, place.drop.at.z, count,
	    note.c_str());
	return e->ID;
}

// host_move_to_stash: the backpack (`everything` false, a corpse death) or all he has (a hardcore retirement) into a
// stash at the body.
u16 stash_for(u16 character, CGameObject* body, CInventoryOwner* io, bool everything, u16 seq, u32& count)
{
	count = 0;
	const Carried carried = carried_of(character, io, everything, seq);
	const std::vector<u16> picked = stash_ids(carried.items, g_set.kinds, everything, carried.layout_known);
	if (picked.empty())
	{
		::Msg("[coop] death: %s has nothing for a stash%s", nick(character).c_str(),
		    layout_note(carried, character).c_str());
		return kNoId;
	}
	xr_string note = everything ? " (everything he has)" : "";
	note += layout_note(carried, character);
	return stash_items(character, body, picked, Drop::Corpse, note, count);
}

// Where the character is: Online = a player character in the level; Offline = his player left and the character is
// parked in ALife (join::take_offline); Between = switching; Gone = no entity.
Presence character_presence(u16 character)
{
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (player_character(character, body, io))
		return Presence::Online;
	CSE_ALifeDynamicObject* d = ai().get_alife() ? ai().alife().objects().object(character, true) : nullptr;
	if (d)
		return d->m_bOnline ? Presence::Between : Presence::Offline;
	return events::find_entity(character) ? Presence::Between : Presence::Gone;
}

// Where the stash is: Online only as an inventory box (another class under that section never fills: its 5 s run out).
Presence stash_presence_of(u16 stash)
{
	if (CGameObject* go = game_object(stash))
		return smart_cast<CInventoryBox*>(go) ? Presence::Online : Presence::Between;
	CSE_ALifeDynamicObject* d = ai().get_alife() ? ai().alife().objects().object(stash, true) : nullptr;
	if (d)
		return d->m_bOnline ? Presence::Between : Presence::Offline;
	return events::find_entity(stash) ? Presence::Between : Presence::Gone;
}

void stash_filled(PendingStash& p, u32 moved, const char* how)
{
	g_st.moved += moved;
	::Msg("[coop] death stash %u: %u of %u items of %s are in%s", p.stash, moved, u32(p.items.size()),
	    nick(p.character).c_str(), how);
	if (!moved)
	{
		release_stash(p.stash);
		return;
	}
	queue_marker(p.character, p.stash, p.marker); // a drop bag keeps the drop mode byte (Ruling L-1)
}

// Both online: the items go in, as the stock corpse search window moves them (move_item_from_to).
void fill_stash(PendingStash& p)
{
	u32 moved = 0;
	for (u16 item : p.items)
	{
		if (server_parent(item) != p.character)
			continue;
		move_item(p.character, p.stash, item);
		if (server_parent(item) == p.stash)
			++moved;
	}
	stash_filled(p, moved, "");
}

// Both offline (fix round 1, review minor 1: the player left before the stash came online and nobody is near it):
// there are no game objects for the events, so the move is ALife's, as an offline transfer request (coop_requests
// apply_move): detach from the character, attach to the stash, and the parent change is relayed to the clients by hand.
void move_offline(PendingStash& p)
{
	CSE_ALifeDynamicObject* from = ai().alife().objects().object(p.character, true);
	CSE_ALifeDynamicObject* to = ai().alife().objects().object(p.stash, true);
	u32 moved = 0;
	if (from && to && !from->m_bOnline && !to->m_bOnline)
		for (u16 item : p.items)
		{
			CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true);
			CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(d);
			if (!d || !ii || d->m_bOnline || d->ID_Parent != p.character)
				continue;
			if (std::find(from->children.begin(), from->children.end(), item) == from->children.end())
				continue;
			alife_rw().graph().detach(*from, ii, from->m_tGraphID, true, true);
			alife_rw().graph().attach(*to, ii, d->m_tGraphID, true, true);
			events::host_on_parent_changed(item, p.character, p.stash); // no server event ran: relay it ourselves
			if (d->ID_Parent == p.stash)
				++moved;
		}
	stash_filled(p, moved, " (moved offline: its player is away)");
}

// The character is here and the stash did not come online in time (or is gone): it goes, the items drop at the body.
void drop_at_body(PendingStash& p)
{
	::Msg("! [coop] death stash %u not online %u s after %s was there: his items drop at the body", p.stash,
	    u32(kStashWaitMs / 1000), nick(p.character).c_str());
	++g_st.stash_fallbacks;
	release_stash(p.stash);
	CGameObject* body = nullptr;
	CInventoryOwner* io = nullptr;
	if (!player_character(p.character, body, io))
		return; // stash_step said Online this frame
	const Fvector pos = body->Position();
	const DropPlace place = fallback_place(p.died, body_place(body).drop, Point{pos.x, pos.y, pos.z});
	u32 count = 0;
	const u16 first = drop_all_at(p.character, p.items, place, count);
	queue_marker(p.character, first, Drop::Drop);
}

// Another character now, no character, or his player stayed away: the items stay with the character, the stash goes.
void give_up(PendingStash& p, bool owner_same, Presence character)
{
	const char* why = !owner_same ? "the id belongs to another character now"
	                              : character == Presence::Gone ? "the character is gone"
	                                                             : "his player stayed away";
	::Msg("! [coop] death stash %u given up (%s): the items stay with character %u", p.stash, why, p.character);
	++g_st.stash_lost;
	g_gave_up.insert(p.character);
	release_stash(p.stash);
}

// Review minor 5 (T9): stashes still waiting for their items when the session ends or the world is replaced are empty
// entities nobody will fill. They are released while the world still has them. After a load the id may name something
// else, so the entity under it must still be a coop_death_stash with nothing in it; without a world they are forgotten.
void abandon_stashes(const char* why)
{
	if (g_stashes.empty())
		return;
	const bool world = mgr().is_host() && g_pGameLevel && ai().get_alife();
	u32 released = 0;
	if (world)
		for (const PendingStash& p : g_stashes)
		{
			const CSE_Abstract* e = events::find_entity(p.stash);
			if (!e || xr_strcmp(e->s_name, kStashSection) || !e->children.empty())
				continue;
			release_stash(p.stash);
			++released;
		}
	g_st.stash_abandoned += released;
	::Msg("[coop] death: %s, %u stashes were waiting: %u released empty, %u forgotten (the items stay with their "
	      "characters)",
	    why, u32(g_stashes.size()), released, u32(g_stashes.size()) - released);
	g_stashes.clear();
}

bool stash_waiting(u16 stash)
{
	for (const PendingStash& p : g_stashes)
		if (p.stash == stash)
			return true;
	return false;
}

// Ruling L-1, carried from F3 T15 (concern 3): a death stash whose last item was taken stayed in the world for ever (the
// stock item_backpack.actor_on_item_take_from_box releases only inv_backpack). Once a second the host looks at the
// boxes on its level - only an online stash can have been emptied, and every take, a client's too, is a parent change
// on the host - and releases each coop_death_stash that is empty in the server's state and in its box, does not wait
// for its items (a pending move) and is not open in anybody's window (release_empty_bag: the host's own, in_use, or a
// player's, trade::host_container_open - L-1 round 1, I1: his window reads the box through a raw pointer every frame,
// a teammate taking the last item with the window open is the usual case). Any death stash: of this
// session, of a loaded world, of a hardcore retirement. The release is the usual one (GE_DESTROY), so every client's
// mirror loses it and the host's Lua drops its mark.
void release_empty_bags(u64 t)
{
	if (t < g_next_bag_poll)
		return;
	g_next_bag_poll = t + kBagPollMs;
	if (!g_pGameLevel || !ai().get_alife())
		return;
	xr_vector<u16> empty;
	for (u32 i = 0, n = Level().Objects.o_count(); i < n; ++i)
	{
		CObject* o = Level().Objects.o_get_by_iterator(i);
		LPCSTR section = o ? o->cNameSect_str() : nullptr;
		if (!section || xr_strcmp(section, kStashSection))
			continue;
		CInventoryBox* box = smart_cast<CInventoryBox*>(o);
		const CSE_Abstract* e = box ? events::find_entity(box->ID()) : nullptr;
		BagState s;
		s.death_stash = e && !xr_strcmp(e->s_name, kStashSection);
		s.online = box && !box->getDestroy() && !box->object_removed();
		s.empty = e && e->children.empty() && box->IsEmpty();
		s.pending = stash_waiting(o->ID());
		s.in_use = box && box->in_use();
		s.open_by_player = trade::host_container_open(o->ID()); // L-1 round 1, I1: a client's window too
		if (release_empty_bag(s))
			empty.push_back(o->ID());
	}
	for (const u16 id : empty) // after the walk: a release must not change the list under it
	{
		release_stash(id);
		++g_st.bags_released;
		if (log_allowed())
			::Msg("[coop] death stash %u is empty: released", id);
	}
}

void host_frame()
{
	const u32 epoch = join::host_world_epoch();
	if (epoch != g_epoch)
	{
		if (g_epoch != 0xffffffff && !g_markers.empty())
			::Msg("[coop] death: a new world, %u markers of the old one dropped", u32(g_markers.size()));
		g_epoch = epoch;
		abandon_stashes("a new world");
		g_markers.clear();
		g_gave_up.clear();
	}
	if (!g_stashes.empty() && ai().get_alife())
	{
		const u64 t = now_ms();
		for (size_t i = 0; i < g_stashes.size();)
		{
			PendingStash& p = g_stashes[i];
			const bool owner_same = same_owner(p.owner, owner_of(p.character));
			const Presence character = character_presence(p.character);
			stash_presence(p.deadline, p.away, character, t);
			switch (stash_step(owner_same, character, stash_presence_of(p.stash), t, p.deadline, p.give_up_at))
			{
			case StashStep::Wait: ++i; continue;
			case StashStep::Fill: fill_stash(p); break;
			case StashStep::MoveOffline: move_offline(p); break;
			case StashStep::DropAtBody: drop_at_body(p); break;
			case StashStep::GiveUp: give_up(p, owner_same, character); break;
			}
			g_stashes.erase(g_stashes.begin() + i);
		}
	}
	release_empty_bags(now_ms());
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
		unsigned long seq = kNoSeq;
		bool bad = false;
		if (args && *args)
		{
			char* end = nullptr;
			id = strtoul(args, &end, 10);
			bad = end == args || id >= kNoId;
			if (!bad)
			{
				while (*end == ' ' || *end == '\t')
					++end;
				if (*end)
				{
					char* end2 = nullptr;
					seq = strtoul(end, &end2, 10);
					bad = end2 == end || *end2 || seq >= kNoSeq;
				}
			}
		}
		if (bad)
		{
			::Msg("! [coop] coop_death_preview [character id [death seq]]: 0 = the host, a player's proxy id otherwise; "
			      "the seq of a death he reported applies his list of what he had on");
			return;
		}
		host_preview(u16(id), u16(seq));
	}
	void Info(TInfo& I) override
	{
		xr_strcpy(I, "host: log what a death of character N [report seq S] (default 0, the host) would take now; moves nothing");
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

	// Ruling L-1 (plan_losses): drop puts its share into a bag at the body, corpse the whole backpack, lose destroys
	// the share. The seed is the pick of drop and lose; corpse picks nothing.
	const Carried carried = carried_of(character, io, false, seq);
	const u64 game_time = ai().get_alife() ? u64(ai().alife().time_manager().game_time()) : 0;
	const u32 seed = loss_seed(character, seq, game_time);
	const LossPlan plan = plan_losses(mode, carried.items, g_set.kinds, carried.layout_known, g_set.shares, seed);
	const xr_string note = layout_note(carried, character);
	if (plan.move == LossMove::None)
	{
		::Msg("[coop] death of %s (seq %u, %s): nothing of %u backpack items is lost%s", nick(character).c_str(), seq,
		    mode == Drop::Lose ? "lose" : marker_kind(mode), u32(plan.offered), note.c_str());
		return 0;
	}

	if (plan.move == LossMove::Destroy)
	{
		u32 count = 0;
		for (u16 id : plan.items)
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
		::Msg("[coop] death of %s (seq %u): %u of %u backpack items are gone%s", nick(character).c_str(), seq, count,
		    u32(plan.offered), note.c_str());
		return count;
	}

	u32 count = 0;
	::Msg("[coop] death of %s (seq %u, %s): %u of %u backpack items go into a stash at the body%s",
	    nick(character).c_str(), seq, marker_kind(plan.marker), u32(plan.items.size()), u32(plan.offered),
	    note.c_str());
	stash_items(character, body, plan.items, plan.marker, note, count);
	return count;
}

u16 host_move_to_stash(u16 character, bool everything, u16 seq)
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
	g_gave_up.erase(character); // a new stash: its own outcome
	u32 count = 0;
	return stash_for(character, body, io, everything, seq, count);
}

bool host_stash_pending(u16 character)
{
	for (const PendingStash& p : g_stashes)
		if (p.character == character)
			return true;
	return false;
}

StashOutcome host_stash_outcome(u16 character)
{
	if (host_stash_pending(character))
		return StashOutcome::Pending;
	return g_gave_up.find(character) != g_gave_up.end() ? StashOutcome::GaveUp : StashOutcome::Done;
}

u32 host_items_on(u16 character)
{
	const CSE_Abstract* e = events::find_entity(character);
	if (!e || !ai().get_alife())
		return 0;
	u32 n = 0;
	for (const u16 id : e->children)
		if (smart_cast<CSE_ALifeInventoryItem*>(ai().alife().objects().object(id, true)))
			++n;
	return n;
}

bool host_release_character(u16 character)
{
	if (!mgr().is_host() || character == 0 || !ai().get_alife() || !g_pGameLevel)
		return false;
	CSE_ALifeDynamicObject* d = ai().alife().objects().object(character, true);
	if (!d || !world::is_player_character(d) || !d->m_ini_string.size() || !is_retired(d->m_ini_string.c_str()))
	{
		::Msg("! [coop] character %u not released: no retired player character under that id", character);
		return false;
	}
	if (!d->m_bOnline)
	{
		alife_rw().release(d, true); // offline: ALife at once
		::Msg("[coop] retired character %u leaves the world (offline)", character);
		return true;
	}
	// Fix round 1, C1: a player's proxy is a remote puppet on the host (coop_remote make_remote: setLocal(FALSE)), and
	// CGameObject::DestroyObject sends nothing for a non-local object. The server's own GE_DESTROY, as the stock
	// alife():release (CALifeSimulator__release) and objects::host_destroy_entity send it, removes the game object and
	// the entity; the caller sees him gone (survival: the entity is no longer there) on a later frame.
	NET_Packet P;
	P.w_begin(M_EVENT);
	P.w_u32(Level().timeServer());
	P.w_u16(GE_DESTROY);
	P.w_u16(character);
	Level().Send(P, net_flags(TRUE, TRUE));
	::Msg("[coop] retired character %u: GE_DESTROY sent", character);
	return true;
}

bool host_new_kit()
{
	ensure_settings();
	return g_cv_new_kit != 0 || g_set.new_kit;
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

void host_preview(u16 character, u16 seq)
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
	const Carried carried = carried_of(character, io, false, seq);
	const bool known = carried.layout_known;
	const std::vector<ItemFacts>& backpack = carried.items;
	const std::vector<Candidate> candidates = loss_candidates(backpack, g_set.kinds, known);
	const u64 game_time = ai().get_alife() ? u64(ai().alife().time_manager().game_time()) : 0;
	const u32 seed = loss_seed(character, 0, game_time);
	const std::vector<u16> picked = pick_losses(candidates, g_set.shares, seed);
	if (carried.reported)
		::Msg("[coop] death preview of %s (character %u): %u items carried, %u may be lost (the rest: %u on him by the "
		      "list of death %u, quest items, the PDA)",
		    nick(character).c_str(), character, u32(backpack.size()), u32(candidates.size()), u32(carried.worn), u32(seq));
	else
		::Msg("[coop] death preview of %s (character %u): %u backpack items, %u may be lost (the rest: quest items, the "
		      "PDA%s)",
		    nick(character).c_str(), character, u32(backpack.size()), u32(candidates.size()),
		    known ? "" : ", what the player may have on: the host does not see his slots and belt; give the seq of a "
		                 "death he reported to use his list");
	::Msg("[coop]   drop (into a stash at the body) / lose now (seed %u): %u items", seed, u32(picked.size()));
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
	abandon_stashes("the session ended"); // review minor 5: an unfilled stash is released, not left empty in the world
	g_markers.clear();
	g_epoch = 0xffffffff;
	g_next_bag_poll = 0;
	g_st = Stats();
	g_log_count = 0;
	g_gave_up.clear();
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
		      "(%llu fell back to dropping, %llu given up: items stayed, %llu released empty on a new world, %llu "
		      "released when emptied), %u waiting; %llu markers sent (%llu not), %llu RU taken",
		    g_st.deaths, g_st.dropped, g_st.destroyed, g_st.moved, g_st.stashes, g_st.stash_fallbacks, g_st.stash_lost,
		    g_st.stash_abandoned, g_st.bags_released, u32(g_stashes.size()), g_st.markers_sent, g_st.markers_dropped,
		    g_st.money_taken);
		for (const PendingStash& p : g_stashes)
			::Msg("[coop]     stash %u of %s: %u items, %lld ms left", p.stash, nick(p.character).c_str(),
			    u32(p.items.size()), s64(p.deadline) - s64(now_ms()));
	}
	else if (mgr().is_client())
		::Msg("[coop]   death: %llu markers from the host, %llu refused", g_st.markers_in, g_st.markers_refused);
}

void register_console()
{
	CMD1(CCC_CoopDeathPreview, "coop_death_preview");
	CMD4(CCC_Integer, "coop_hardcore_new_kit", &g_cv_new_kit, 0, 1); // host setting, Ruling F3-10
}
} // namespace death
} // namespace coop
