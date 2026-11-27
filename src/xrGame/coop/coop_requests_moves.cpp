// Anomaly Coop - requests: who holds an item. A client's take, drop and transfer (ReqTake / ReqDrop / ReqTransfer), destroy
// and use (ReqDestroy / ReqUse), and this process's own script moves (host_transfer_item), with the mods' move rules and
// the weapon riders that go with a move.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_requests_impl.h"
#include "coop_events.h"
#include "coop_manager.h"
#include "xrCoop/coop_mode.h"
#include "coop_world.h"
#include "coop_alife.h"
#include "coop_dialog.h"
#include "xrCoop/coop_validate.h"
#include "xrCoop/coop_moves.h"
#include "xrCoop/coop_item_rider_rules.h"

#include <algorithm>

#include "Level.h"
#include "GameObject.h"
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "InventoryBox.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"
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
constexpr float kDropPosRange = 100.f;
constexpr float kReachRange = 15.f; // player character <-> item/container, with lag slack

Fvector position_of(u16 id)
{
	if (CGameObject* go = world::object_here(id))
		return go->Position();
	if (CSE_Abstract* e = world::server_entity(id))
		return e->o_Position;
	return Fvector().set(0.f, 0.f, 0.f);
}

bool is_container(u16 id)
{
	CSE_Abstract* e = world::server_entity(id);
	return smart_cast<CSE_ALifeTraderAbstract*>(e) || smart_cast<CSE_ALifeInventoryBox*>(e);
}

void inject_item_event(u16 type, u16 dest, u16 item)
{
	NET_Packet P;
	CGameObject::u_EventGen(P, type, dest);
	P.w_u16(item);
	CGameObject::u_EventSend(P);
}

// ---------------------------------------------------------------- weapon riders go with the move
// (xrCoop/coop_item_rider_rules.h). What a mod keeps beside a weapon (a camo, a jam type) are item riders of
// scripts\coop_mods_item_state: the holder's machine sends them in the economy's se channel, which the deciding Lua checks
// against the item's holder when its inbox runs. A move is applied when its request arrives, and a client sends a frame's
// requests before its script events, so that check saw the next holder and dropped the former holder's last change. A client
// appends the riders of a weapon leaving its character to the ReqDrop / ReqTransfer; the process that applies the move (the
// co-op host; the map server of the player's level, to which the world tunnels the request) hands them to the script before
// apply_move, under the move's own holder check.
constexpr const char* kRiderScript = "coop_mods_item_state";

// Under lua_pcall: coop_mods_item_state.<fn> with the `args` arguments at the stack's bottom; its first result.
int lua_rider_fn(lua_State* L, const char* fn, int args)
{
	lua_getglobal(L, kRiderScript); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_getfield(L, -1, fn);
	if (!lua_isfunction(L, -1))
		return 0;
	for (int i = 1; i <= args; ++i)
		lua_pushvalue(L, i);
	lua_call(L, args, 1);
	return 1;
}

int lua_rider_carry(lua_State* L) { return lua_rider_fn(L, "carry", 1); }
int lua_rider_carried(lua_State* L) { return lua_rider_fn(L, "carried", 4); }

// Moves `item` from `src` to `dst` (0xffff = the world) through the engine: ownership events
// when everything is online (server entity, ALife and game objects stay consistent, and the
// server hook relays the change), ALife attach/detach when everything is offline.
Reason apply_move(u16 item, u16 src, u16 dst, const Fvector* drop_pos)
{
	CGameObject* gi = valid_go(item);
	const bool online = gi && (src == kNoId || valid_go(src)) && (dst == kNoId || valid_go(dst));
	if (online)
	{
		// Ownership on the server leads the game-object event queue. Never detach from a stale
		// inventory, or give an item the recipient's queued TAKE will immediately reject.
		const u16 local_src = gi->H_Parent() ? gi->H_Parent()->ID() : kNoId;
		if (local_src != src)
			return Reason::Moved;
		if (CInventoryOwner* recipient = smart_cast<CInventoryOwner*>(valid_go(dst)))
		{
			CInventoryItem* inventory_item = smart_cast<CInventoryItem*>(gi);
			if (!inventory_item || !recipient->inventory().CanTakeItem(inventory_item))
				return Reason::NotAllowed;
		}
		const bool transfer = src != kNoId && dst != kNoId;
		if (src != kNoId)
			inject_item_event(transfer ? GE_TRADE_SELL : GE_OWNERSHIP_REJECT, src, item);
		if (dst != kNoId)
			inject_item_event(transfer ? GE_TRADE_BUY : GE_OWNERSHIP_TAKE, dst, item);
		CSE_Abstract* e = world::server_entity(item);
		if (!e || e->ID_Parent != dst)
		{
			// SELL can succeed while BUY fails. Restore the same entity through normal events;
			// returning Engine alone would leave a gift on the ground instead of with its sender.
			if (src != kNoId && e && e->ID_Parent == kNoId)
			{
				inject_item_event(GE_TRADE_BUY, src, item);
				if (e->ID_Parent != src)
					::Msg("! [coop] transfer item %u: could not restore sender %u after recipient %u refused", item, src, dst);
			}
			return Reason::Engine;
		}
		if (drop_pos && dst == kNoId)
		{
			NET_Packet P; // after the reject in the same queue: the item has its physics shell by then
			CGameObject::u_EventGen(P, GE_CHANGE_POS, item);
			P.w_vec3(*drop_pos);
			CGameObject::u_EventSend(P);
			events::host_set_drop_position(item, *drop_pos);
		}
		return Reason::Ok;
	}
	CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true);
	CSE_ALifeInventoryItem* ii = smart_cast<CSE_ALifeInventoryItem*>(d);
	if (!d || !ii || d->m_bOnline)
		return Reason::Offline;
	CSE_ALifeDynamicObject* from = src == kNoId ? nullptr : ai().alife().objects().object(src, true);
	CSE_ALifeDynamicObject* to = dst == kNoId ? nullptr : ai().alife().objects().object(dst, true);
	if ((src != kNoId && (!from || from->m_bOnline)) || (dst != kNoId && (!to || to->m_bOnline)))
		return Reason::Offline; // half online: the client asks again once it settled
	if (from)
	{
		if (std::find(from->children.begin(), from->children.end(), item) == from->children.end())
			return Reason::Engine;
		alife_rw().graph().detach(*from, ii, from->m_tGraphID, true, true);
	}
	if (to)
		alife_rw().graph().attach(*to, ii, d->m_tGraphID, true, true);
	else if (drop_pos)
		d->o_Position = *drop_pos;
	events::host_on_parent_changed(item, src, dst); // no server event ran: relay it ourselves
	if (drop_pos && !to)
		events::host_set_drop_position(item, *drop_pos);
	return Reason::Ok;
}

// Under lua_pcall, arg 1 the NPC's game object; returns whether it is a companion of the group. The
// companion squads are the host's Lua (axr_companions, coop_npc_companions.is_group_companion answers for an NPC).
int lua_is_group_companion(lua_State* L)
{
	lua_getglobal(L, "coop_npc_companions"); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_getfield(L, -1, "is_group_companion");
	if (!lua_isfunction(L, -1))
		return 0;
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	return 1;
}

// Is the live NPC `npc` (host numbering) a companion of the group? False for an NPC without a game
// object here, not in a companion squad, or when the Lua call fails (logged).
bool group_companion(u16 npc)
{
	CGameObject* go = world::object_here(npc);
	lua_State* L = go && go->lua_game_object() ? ai().script_engine().lua() : nullptr;
	if (!L || !lua_checkstack(L, 8))
		return false;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_is_group_companion);
	::luabind::object(L, go->lua_game_object()).pushvalue();
	if (lua_pcall(L, 1, 1, 0) != 0)
	{
		if (g_log.allow())
		{
			const char* err = lua_tostring(L, -1);
			::Msg("! [coop] coop_npc_companions.is_group_companion(%u) failed (not a companion here): %s", npc, err ? err : "?");
		}
		lua_settop(L, top);
		return false;
	}
	const bool is = lua_toboolean(L, -1) != 0;
	lua_settop(L, top);
	return is;
}

// One side of a move for moves::check (xrCoop/coop_moves.h), from its top owner: the
// ground, the requester's own character, another player, a live NPC (and whether the requester holds its talk lock or
// it is a companion of the group), a corpse or a box with their locks (the game object's flags, the server object's
// while offline), or anything else.
moves::Side move_side(const Req& q, u16 id)
{
	moves::Side s;
	if (id == kNoId)
		return s; // the ground
	const u16 root = events::root_of(id);
	if (root == q.proxy)
	{
		s.kind = moves::Side::Mine;
		return s;
	}
	if (is_player(root))
	{
		s.kind = moves::Side::Player;
		return s;
	}
	CSE_Abstract* e = world::server_entity(root);
	CGameObject* go = world::object_here(root);
	if (CSE_ALifeInventoryBox* box = smart_cast<CSE_ALifeInventoryBox*>(e))
	{
		s.kind = moves::Side::Box;
		const CInventoryBox* online = smart_cast<CInventoryBox*>(go);
		s.closed = online ? online->closed() : box->m_closed;
		s.can_take = online ? online->can_take() : box->m_can_take;
		return s;
	}
	if (CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(e))
	{
		const CEntityAlive* alive = smart_cast<CEntityAlive*>(go);
		const CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(e);
		if (alive ? !!alive->g_Alive() : (creature && creature->g_Alive()))
		{
			s.kind = moves::Side::LiveNpc;
			s.talking = dialog::host_lock_of(q.from) == root;
			s.companion = !s.talking && group_companion(root);
			return s;
		}
		s.kind = moves::Side::Corpse;
		const CInventoryOwner* owner = smart_cast<CInventoryOwner*>(go);
		s.closed = owner ? owner->deadbody_closed_status() : trader->m_deadbody_closed;
		s.can_take = owner ? owner->deadbody_can_take_status() : trader->m_deadbody_can_take;
		return s;
	}
	s.kind = moves::Side::Other;
	return s;
}

constexpr size_t kAdmissionWhyMax = 160;

// Under lua_pcall: args 1-5 (container, item, pending count, pending weight, from); returns admit's two results.
int lua_move_rules(lua_State* L)
{
	lua_getglobal(L, "coop_mods_move_limits"); // _G.__index loads the module if it was not yet
	if (!lua_istable(L, -1))
		return 0;
	lua_getfield(L, -1, "admit");
	if (!lua_isfunction(L, -1))
		return 0;
	for (int i = 1; i <= 5; ++i)
		lua_pushvalue(L, i);
	lua_call(L, 5, 2);
	return 2;
}

// A text of Lua for one log line: bounded, control characters blanked.
void admission_text(const char* text, size_t size, xr_string& out)
{
	out.assign(text ? text : "", text ? _min(size, kAdmissionWhyMax) : 0);
	for (char& c : out)
		if (u8(c) < 0x20)
			c = ' ';
}

// The move rules' answer for `item` from `src` into `dst`; `text`: a refusal's reason or the Lua error.
admission::Answer ask_move_rules(u16 item, u16 src, u16 dst, const admission::Pending& pending, xr_string& text)
{
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 16))
		return admission::Answer::Failed;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_move_rules);
	lua_pushnumber(L, lua_Number(dst));
	lua_pushnumber(L, lua_Number(item));
	lua_pushnumber(L, lua_Number(pending.count));
	lua_pushnumber(L, lua_Number(pending.weight));
	lua_pushnumber(L, lua_Number(src));
	admission::Answer answer = admission::Answer::Failed;
	size_t size = 0;
	const char* said = nullptr;
	if (lua_pcall(L, 5, 2, 0) != 0)
		said = lua_tolstring(L, -1, &size);
	else
	{
		const int verdict = lua_type(L, -2);
		if (verdict == LUA_TBOOLEAN)
			answer = lua_toboolean(L, -2) ? admission::Answer::Admit : admission::Answer::Refuse;
		else if (verdict == LUA_TNIL)
			answer = admission::Answer::NoRule;
		if (lua_type(L, -1) == LUA_TSTRING)
			said = lua_tolstring(L, -1, &size);
	}
	admission_text(said, size, text);
	lua_settop(L, top);
	return answer;
}

// Where a held move of `item` into `container` stands now (admission::Ledger::settle).
admission::Track admission_track(u16 item, u16 container)
{
	const CSE_Abstract* e = world::server_entity(item);
	if (!e || e->ID_Parent != container)
		return admission::Track::Gone; // moved on, released
	CGameObject* go = valid_go(item);
	if (!go || !valid_go(container))
		return admission::Track::Gone; // nothing here weighs that container's game inventory
	const CObject* parent = go->H_Parent();
	return parent && parent->ID() == container ? admission::Track::Landed : admission::Track::Waiting;
}

// A move into a character whose root is a player. The move rules are asked without the admission
// book (nothing pending, nothing settled, held or released: a player's game inventory is no rule's weight). Only a refusal
// stops the move (a mod's rule that vetoes a pickup only on the picker's machine); a let-in, no rule or a failed asking moves it as in the stock game.
Reason player_move(u16 item, u16 src, u16 dst, const Fvector* drop_pos, xr_string& refused)
{
	++g_ac.player_asked;
	xr_string text;
	const admission::Answer answer = ask_move_rules(item, src, dst, admission::Pending{}, text);
	if (answer == admission::Answer::Failed)
	{
		++g_ac.player_failed;
		if (g_log.allow())
			::Msg("! [coop] move rules for item %u into player %u failed (moved as in the stock game): %s", item, dst,
			    text.c_str());
	}
	if (admission::judge(g_admission, admission::Route::Ask, answer, dst, item, 0.f, 0) != admission::Verdict::Full)
		return apply_move(item, src, dst, drop_pos);
	++g_ac.player_refused;
	refused = admission::refusal_text(text.c_str());
	return Reason::NotAllowed;
}

// apply_move of a move this process decided, after the move rules let it in. A move onto the ground asks nothing (the stock
// game's limits); one into a player's character asks without the admission book (player_move). `refused`: the rules' reason
// when they refused it (else left empty).
Reason admitted_move(u16 item, u16 src, u16 dst, const Fvector* drop_pos, xr_string& refused)
{
	switch (admission::route_of(dst, dst != kNoId && is_player(events::root_of(dst))))
	{
	case admission::Route::Stock: return apply_move(item, src, dst, drop_pos);
	case admission::Route::Ask: return player_move(item, src, dst, drop_pos, refused);
	case admission::Route::AskAndHold: break;
	}
	const u64 now = mgr().now_ms();
	g_admission.settle(dst, now, &admission_track);
	++g_ac.asked;
	xr_string text;
	const admission::Answer answer = ask_move_rules(item, src, dst, g_admission.pending(dst), text);
	const CInventoryItem* inventory_item = smart_cast<CInventoryItem*>(valid_go(item));
	const admission::Verdict verdict =
	    admission::decide(g_admission, answer, dst, item, inventory_item ? inventory_item->Weight() : 0.f, now);
	switch (answer)
	{
	case admission::Answer::Admit:
		if (verdict == admission::Verdict::Ok)
			++g_ac.let_in;
		else
			++g_ac.too_many;
		break;
	case admission::Answer::Refuse: ++g_ac.refused; break;
	case admission::Answer::NoRule: ++g_ac.no_rule; break;
	case admission::Answer::Failed:
		++g_ac.failed;
		if (g_log.allow())
			::Msg("! [coop] move rules for item %u into %u failed (moved as in the stock game): %s", item, dst, text.c_str());
		break;
	}
	if (verdict == admission::Verdict::Full)
	{
		refused = answer == admission::Answer::Refuse ? (text.empty() ? xr_string("refused by a move rule") : text) :
		                                                xr_string("too many moves in flight");
		return Reason::NotAllowed;
	}
	const Reason result = apply_move(item, src, dst, drop_pos);
	if (verdict == admission::Verdict::Ok && result != Reason::Ok && g_admission.release(item))
		++g_ac.undone;
	return result;
}

// The riders a client's move of `item` carried, before admitted_move. `holds`: host_move's check that the
// requester's character holds the item. The script checks them again like his se riders and writes them as this process's
// own facts (the co-op host relays them; a map server's go up to the world server). Never logged: client data.
void host_take_carried(const Req& q, u16 item, bool holds, const std::string& text)
{
	switch (riders::judge(text.size(), holds))
	{
	case riders::Carried::None: return;
	case riders::Carried::TooLong: ++g_carry.refused_long; return;
	case riders::Carried::NotHolder: ++g_carry.not_holder; return;
	case riders::Carried::Take: break;
	}
	++g_carry.taken;
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 16))
	{
		++g_carry.lua_failed;
		return;
	}
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_rider_carried);
	lua_pushnumber(L, lua_Number(item));
	lua_pushnumber(L, lua_Number(q.from));
	lua_pushlstring(L, text.data(), text.size());
	lua_pushnumber(L, lua_Number(q.proxy));
	if (lua_pcall(L, 4, 1, 0) != 0)
	{
		++g_carry.lua_failed;
		if (g_log.allow())
			::Msg("! [coop] %s #%u from player #%u: the riders of item %u were not written, the item state script failed",
			    msg_name(q.type), q.seq, q.from, item);
	}
	else if (lua_toboolean(L, -1))
		++g_carry.written;
	lua_settop(L, top);
}

Reason host_move(const Req& q, u16 item, u16 src, u16 dst, u8 flags, const Fvector& pos, const std::string& carried)
{
	CSE_Abstract* e = world::server_entity(item);
	if (!e)
		return Reason::Missing;
	if (!smart_cast<CSE_ALifeInventoryItem*>(e) && !smart_cast<CSE_Temporary*>(e))
		return Reason::NotAllowed;
	if (e->ID_Parent != src)
		return Reason::Moved; // first valid request wins, this one is late
	if (src == dst)
		return Reason::Ok;
	const u16 root_src = src == kNoId ? kNoId : events::root_of(src);
	const u16 root_dst = dst == kNoId ? kNoId : events::root_of(dst);
	const bool mine_src = root_src == q.proxy;
	const bool mine_dst = root_dst == q.proxy;
	if (!mine_src && !mine_dst)
		return Reason::NotOwner;
	if (src != kNoId && !mine_src && is_player(root_src))
		return Reason::NotOwner; // never out of another player's pockets (gifts are given)
	if (dst != kNoId && !is_container(dst))
		return Reason::NotAllowed;
	// A client moves items out of another's inventory to its actor; a
	// live NPC's inventory, out or in, only under the requester's talk lock (a mechanic returning a repaired item, a
	// dialog) or as a companion of the group (the stock companion inventory has no dialog), box and corpse locks
	// as the stock UI keeps them. A stock script that moves against them is refused here, visibly.
	if (const moves::Verdict side = moves::check(move_side(q, src), move_side(q, dst)); side != moves::Verdict::Ok)
	{
		if (g_log.allow())
			::Msg("[coop] %s #%u from player #%u: item %u from %u to %u refused: %s", msg_name(q.type), q.seq, q.from,
			    item, src, dst, moves::verdict_text(side));
		return side == moves::Verdict::NotOwner ? Reason::NotOwner : Reason::NotAllowed;
	}
	// The range is checked whatever the client's flags say. What a client applied locally (kMoveLocal: a
	// missile leaving its grenade, a rocket leaving our launcher) only ever leaves our own hands for the world, which
	// has no far side; with the flag on a take from the world or a container the range still holds.
	const u16 far_side = !mine_src ? (src == kNoId ? item : src) : (mine_dst ? kNoId : dst);
	if (far_side != kNoId && position_of(q.proxy).distance_to(position_of(far_side)) > kReachRange)
	{
		if ((flags & kMoveLocal) && note_bad(BadMoveRange))
			::Msg("! [coop] %s #%u from player #%u refused: item %u out of reach (claimed as already applied)",
			    msg_name(q.type), q.seq, q.from, item);
		return Reason::TooFar;
	}
	const bool exact = (flags & kMovePos) && dst == kNoId && validate::vec_finite(pos) &&
	    position_of(q.proxy).distance_to(pos) <= kDropPosRange;
	// the requester still holds the item; write his riders before admitted_move
	host_take_carried(q, item, mine_src && e->ID_Parent == src, carried);
	xr_string refused; // the mods' limits on moves into containers (stash capacity)
	const Reason result = admitted_move(item, src, dst, exact ? &pos : nullptr, refused);
	if (!refused.empty() && g_log.allow())
		::Msg("[coop] %s #%u from player #%u: item %u into %u refused: %s", msg_name(q.type), q.seq, q.from, item, dst,
		    refused.c_str());
	return result;
}

// The item's game object is being destroyed here (a release or a death clean-up of this frame got there first).
bool item_dying(u16 item)
{
	const CObject* o = item != kNoId && g_pGameLevel ? Level().Objects.net_Find(item) : nullptr;
	return o && o->getDestroy();
}

// One line for a script move this process refused: the item's section and the holder it has
// now tell a stale script (the item moved on), a lost item (no server object, only a game object) and a rule's refusal apart.
void log_script_move_refused(u16 item, u16 from, u16 to, Reason why, const xr_string& refused)
{
	CSE_Abstract* e = world::server_entity(item);
	const CObject* o = g_pGameLevel ? Level().Objects.net_Find(item) : nullptr;
	const char* section = e ? e->s_name.c_str() : o ? o->cNameSect_str() : nullptr;
	const u16 holder = e ? e->ID_Parent : o && o->H_Parent() ? u16(o->H_Parent()->ID()) : kNoId;
	CSE_Abstract* h = world::server_entity(holder);
	const char* holder_section = h ? h->s_name.c_str() : nullptr;
	::Msg("! [coop] script transfer item %u [%s] from %u to %u refused: %s%s%s; it is in %u [%s]%s", item,
	    section ? section : "?", from, to, reason_name(why), refused.empty() ? "" : ": ", refused.c_str(), holder,
	    holder_section ? holder_section : "-", e ? "" : " (no server object here, its game object only)");
}
} // namespace

CarryCounters g_carry;

// Client: the riders of our weapon `local_item` (coop_mods_item_state.carry), empty when it has none.
std::string client_carry(u16 local_item)
{
	std::string out;
	lua_State* L = ai().script_engine().lua();
	if (!L || !lua_checkstack(L, 16))
		return out;
	const int top = lua_gettop(L);
	lua_pushcfunction(L, &lua_rider_carry);
	lua_pushnumber(L, lua_Number(local_item));
	if (lua_pcall(L, 1, 1, 0) != 0)
	{
		++g_carry.failed;
		if (g_log.allow())
			::Msg("! [coop] the riders of weapon %u did not go with its move: the item state script failed", local_item);
	}
	else if (lua_type(L, -1) == LUA_TSTRING)
	{
		size_t size = 0;
		const char* text = lua_tolstring(L, -1, &size);
		switch (riders::text_of(size))
		{
		case riders::Text::None: break;
		case riders::Text::Fits:
			out.assign(text, size);
			++g_carry.sent;
			break;
		case riders::Text::TooLong:
			++g_carry.too_long;
			if (g_log.allow())
				::Msg("! [coop] the riders of weapon %u did not go with its move: %u bytes", local_item, u32(size));
			break;
		}
	}
	lua_settop(L, top);
	return out;
}

// ---------------------------------------------------------------- moves into a container
// A capacity a mod checks only in the mover's window would let two players filling one container at once each see the
// old weight. Before apply_move of a move into a container
// that is not a player's character, this process asks the mods' move rules (coop_mods_move_limits.admit, Lua) with the moves
// it let in that the container's game inventory does not show yet (xrCoop/coop_move_admission.h): a client's request
// (host_move) and this process's own script and window moves (host_transfer_item). The windows stay a preview. A refusal is
// Reason::NotAllowed: the ReqResult a client gets for a refused move already (no new wire field).
// A move into a player's character asks the same rules without this book (player_move), so only a
// rule's refusal stops it; onto the ground nothing is asked.
admission::Ledger g_admission;

AdmissionCounters g_ac;

void host_on_move(PeerId from, Reader& r, Msg type)
{
	Req q;
	if (!host_begin(from, r, type, q))
		return;
	const u16 item = r.u16();
	const u16 src = r.u16();
	const u16 dst = r.u16();
	float p[3];
	r.vec3(p);
	const u8 flags = r.u8();
	// a weapon's riders, optional; Reader::str bounds the length
	const std::string carried = r.ok() && r.remaining() > 0 ? r.str() : std::string();
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	if (!validate::finite3(p)) // a fair client sends zeros when it has no drop position
	{
		if (note_bad(BadNumber))
			::Msg("! [coop] %s #%u from player #%u refused: position is not a number", msg_name(type), q.seq, from);
		return reply(q, Reason::Malformed, item);
	}
	if (!allow(from, CatMove))
		return reply(q, Reason::RateLimited, item);
	reply(q, host_move(q, item, src, dst, flags, Fvector().set(p[0], p[1], p[2]), carried), item);
}

Reason host_destroy(const Req& q, u16 item)
{
	CSE_Abstract* e = world::server_entity(item);
	if (!e)
		return Reason::Missing;
	if (smart_cast<CSE_ALifeCreatureAbstract*>(e))
		return Reason::NotAllowed;
	const u16 root = events::root_of(item);
	const bool mine = root == q.proxy || (e->ID_Parent == kNoId && events::host_last_holder(item) == q.proxy);
	if (!mine)
	{
		if (e->ID_Parent == kNoId)
			return Reason::NotOwner;
		const moves::Verdict side = moves::destroy_check(move_side(q, root));
		if (side != moves::Verdict::Ok)
			return side == moves::Verdict::NotOwner ? Reason::NotOwner : Reason::NotAllowed;
		if (position_of(q.proxy).distance_to(position_of(root)) > kReachRange)
			return Reason::TooFar;
	}
	if (world::object_here(item))
	{
		// the standard chain: the server releases it from ALife, the release hook tells clients
		NET_Packet P;
		CGameObject::u_EventGen(P, GE_DESTROY, item);
		CGameObject::u_EventSend(P);
		return world::server_entity(item) ? Reason::Engine : Reason::Ok;
	}
	if (CSE_ALifeDynamicObject* d = ai().alife().objects().object(item, true))
	{
		alife_rw().release(d, true);
		return Reason::Ok;
	}
	return Reason::Engine;
}

void host_on_destroy_req(PeerId from, Reader& r, Msg type)
{
	Req q;
	if (!host_begin(from, r, type, q))
		return;
	const u16 item = r.u16();
	if (!r.ok())
		return reply(q, Reason::Malformed, kNoId);
	if (!allow(from, CatDestroy))
		return reply(q, Reason::RateLimited, item);
	reply(q, host_destroy(q, item), item);
}

ScriptMoveCounters g_script_moves;

// Scripts call npc:transfer_item(item, npc) to move a creature's weapon within its own inventory (at death, in a wounded
// animation): the stock GE_TRADE_SELL + GE_TRADE_BUY to the holder the item has puts a slotted weapon into the
// ruck. False lets CScriptGameObject::TransferItem send that very pair on this process
// (no move rule: it stays in one inventory); the server hooks relay nothing (back where it was), the creature's snapshot carries
// its empty hands to the copies.
bool host_transfer_item(u16 item, u16 from, u16 to)
{
	if (!mode().decides_here())
		return false;
	if (item_dying(item))
	{
		++g_script_moves.dying; // nothing to move, not even in place
		return true;
	}
	CSE_Abstract* e = world::server_entity(item);
	if (e && e->ID_Parent == from && from == to)
	{
		++g_script_moves.in_place;
		return false; // the stock pair
	}
	if (e && e->ID_Parent == to)
	{
		++g_script_moves.already; // a second call for a move that is done already
		return true;
	}
	xr_string refused; // this process's own moves (scripts, a mod's window) go through the same move rules
	const Reason why = !e ? Reason::Missing : e->ID_Parent != from ? Reason::Moved : admitted_move(item, from, to, nullptr, refused);
	if (why != Reason::Ok)
	{
		++g_script_moves.refused;
		log_script_move_refused(item, from, to, why, refused);
	}
	return true;
}
} // namespace requests
} // namespace coop
