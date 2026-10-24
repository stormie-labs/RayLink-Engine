// Anomaly Coop — shared sleep on the host (F3 Task 11). See coop_rest.h for the flow, the messages, the Lua surface
// and the callbacks; the pure parts (the checks of a lie-down, the messages, the host's book rest::Group, what a machine
// tells its Lua) are xrCoop/coop_rest_rules.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_rest.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "coop_join.h"
#include "coop_world.h"
#include "coop_survival.h"
#include "coop_script.h"
#include "coop_checkpoints.h"
#include "coop_checkpoint_sources.h"
#include "coop_autosave.h"
#include "coop_worldstate.h"
#include "coop_alife.h" // alife_rw: the clock is changed through mutable ALife
#include "xrCoop/coop_limits.h" // limits::Pacer (Ruling F2-14)

#include "../../xrEngine/XR_IOConsole.h"
#include "../../xrEngine/xr_ioc_cmd.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/Environment.h"
#include "Level.h"
#include "Actor.h"
#include "GameObject.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace coop
{
namespace rest
{
namespace
{
using checkpoints::sources::Place;

constexpr u16 kNoId = 0xffff;
constexpr u64 kTickMs = 500; // the host looks at the players present (plan F3 Task 11, step 3)
constexpr u64 kRefreshMs = 5000; // RestState goes out at least this often (step 4)
constexpr size_t kMaxWorldReason = 64; // set_world_block: a short reason ("emission"), only logged
constexpr const char* kChangedCallback = "coop_rest_changed";
constexpr const char* kSleepCallback = "coop_rest_sleep";
constexpr const char* kRefusedCallback = "coop_rest_refused";
constexpr size_t kRefusals = size_t(Refusal::Camp) + 1;

bool log_allowed(u64 count) { return count <= 10 || count % 100 == 0; }

// This machine's own word (lie down / get up). Lua may call coop.rest from anywhere, so the word is only recorded
// here; frame() sends it to the world server (F3a T7: the host player's over the internal channel). The newest word
// wins: a lie-down and a get-up before the next frame (or before the pacer lets one out) are a get-up. A word of an
// older world is dropped.
// F3a T7 R1: the word is written first and g_pending_on says it is there after it (release / acquire): say() may run on
// the game thread, player_frame on the main thread.
struct Pending
{
	Request rq;
	u32 world = 0; // join::world_generation() when it was said
	bool held = false; // client: the pacer held it back (counted once)
};
Pending g_pending;
std::atomic<bool> g_pending_on{false};

void drop_pending()
{
	g_pending_on.store(false, std::memory_order_relaxed);
	g_pending = Pending();
}

limits::Pacer g_pacer; // client: RestRequest below the host's rate rule

struct Host
{
	bool world = false; // the book belongs to the world of `epoch`
	u32 epoch = 0;
	Group group;
	u64 next_tick_ms = 0;
	u64 next_send_ms = 0;
	bool sent_any = false;
	StateMsg last_sent; // block and entries of the last broadcast
	u8 sleep_hours = 0; // of the sleep running now
};
Host g_host;

// Client: the host's last RestState, characters in local numbering (ascending), for the world it came in.
struct Mirror
{
	u32 world = 0; // join::world_generation()
	Block block = Block::None;
	std::vector<Entry> entries;
};
Mirror g_mirror;

// What this machine's Lua was last told (local numbering): coop_rest_changed goes out for the difference.
std::vector<Entry> g_told;

// The host's Lua holds the sleep (set_world_block): an emission, a story scene. It belongs to the world it was set in
// (every load or level change rebuilds the Lua state, which says it again), not to the session: a host who starts the
// session during an emission keeps it.
struct WorldBlock
{
	std::string reason;
	u32 world = 0;
};
WorldBlock g_world_block;

struct Stats
{
	u64 lie_downs = 0, get_ups = 0, refused[kRefusals] = {}, cancelled[kRefusals] = {}, left = 0;
	u64 sleeps = 0, woke = 0, camps = 0, sent = 0, malformed = 0, not_in_world = 0;
	u64 asked = 0, held = 0, received = 0, refusals = 0, sleeps_seen = 0, sleeps_ignored = 0; // client
};
Stats g_stat;

// ---------------------------------------------------------------- helpers

CGameObject* object_here(u16 id)
{
	if (!g_pGameLevel || id == kNoId)
		return nullptr;
	CGameObject* o = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
	return o && !o->getDestroy() ? o : nullptr;
}

// This machine is in its world: the level and its actor are up (Lua callbacks go out only then).
bool in_world() { return g_pGameLevel && g_actor; }

const std::string& world_block_reason()
{
	static const std::string none;
	return g_world_block.world == join::world_generation() ? g_world_block.reason : none;
}

checkpoint::Kind kind_of(Place p)
{
	switch (p)
	{
	case Place::Rest: return checkpoint::Kind::Rest;
	case Place::Tent: return checkpoint::Kind::Tent;
	case Place::Campfire: return checkpoint::Kind::Campfire;
	case Place::Mod: return checkpoint::Kind::Mod;
	case Place::None:
	default: return checkpoint::Kind::None;
	}
}

xr_string nick_of_local(u16 local_id) { return survival::nick_of(join::to_host(local_id)); }

// coop_rest_changed for every difference between what the Lua here was told and `now` (local numbering, ascending),
// then coop_rest_refused(refused) when this machine's player was refused (T11 review M4: one order on the host and on a
// client, rest::news). The changes only in the world: after a new world they go out once this machine is back in it
// (step 5), from the told list of the old one ("awake" for everybody who rested there). A refusal is queued anyway (the
// queue delivers it in the world).
void tell_lua(const std::vector<Entry>& now, Refusal refused = Refusal::None)
{
	if (!in_world())
	{
		if (refused != Refusal::None)
			script::queue_callback(kRefusedCallback, {script::cb_str(refusal_name(refused))});
		return;
	}
	for (const Note& n : news(g_told, now, refused))
	{
		if (n.refusal)
			script::queue_callback(kRefusedCallback, {script::cb_str(refusal_name(n.refused))});
		else
			script::queue_callback(kChangedCallback, {script::cb_int(n.change.id),
			    script::cb_str(state_name(n.change.state)), script::cb_int(n.change.hours)});
	}
	g_told = now;
}

// ---------------------------------------------------------------- host

// The book belongs to the served world: a load or a level change (join::host_world_epoch) wakes everybody (Review
// Focus 2). The Lua here learns it once it is back in the world (tell_lua), the clients from the next RestState.
void host_sync_world()
{
	const u32 epoch = join::host_world_epoch();
	if (g_host.world && g_host.epoch == epoch)
		return;
	const size_t had = g_host.group.resting();
	g_host = Host();
	g_host.world = true;
	g_host.epoch = epoch;
	if (had)
		::Msg("[coop] rest: a new world (epoch %u), %u rest(s) end: everybody is awake", epoch, u32(had));
}

bool host_world_ready() { return in_world() && ai().get_alife(); }

StateMsg host_state()
{
	StateMsg m;
	m.block = g_host.group.block();
	m.entries = g_host.group.entries();
	return m;
}

// RestState to everybody when the book changed, or every kRefreshMs (force: now).
void host_send_state(u64 now, bool force)
{
	const StateMsg m = host_state();
	const bool changed = !g_host.sent_any || m.block != g_host.last_sent.block || m.entries != g_host.last_sent.entries;
	if (!force && !changed && now < g_host.next_send_ms)
		return;
	Writer w;
	write_state(w, m);
	net::host_broadcast(Msg::RestState, w);
	g_host.last_sent = m;
	g_host.sent_any = true;
	g_host.next_send_ms = now + kRefreshMs;
	++g_stat.sent;
}

// The player of `character` learns why in his copy of RestState (F3a T7: the host player too, over the internal channel;
// his Lua hears it from his copy at the next pump, the changes first, as on a client: M4).
void tell_owner(u16 character, Refusal why)
{
	const PeerId peer = survival::peer_of_character(character);
	if (peer == kInvalidPeer)
		return; // he left meanwhile
	StateMsg m = host_state();
	m.refused = why;
	Writer w;
	write_state(w, m);
	net::host_send(peer, Msg::RestState, w);
}

// A refused lie-down. T11 review M1: a rest he was waiting in ends with it (Group::on_refused), a running sleep stays.
void refuse(u16 character, Refusal why)
{
	const bool ended = g_host.group.on_refused(character);
	u64& n = g_stat.refused[size_t(why)];
	++n;
	if (log_allowed(n))
		::Msg("[coop] rest: character %u may not lie down (%s)%s", character, refusal_name(why),
		    ended ? ", his earlier rest ends" : "");
	tell_owner(character, why);
}

// A lie-down or get-up of `character` (host numbering), checked here (coop_rest.h).
void host_request(u16 character, const Request& rq)
{
	host_sync_world();
	if (rq.op == Op::GetUp)
	{
		if (g_host.group.get_up(character))
		{
			++g_stat.get_ups;
			if (log_allowed(g_stat.get_ups))
				::Msg("[coop] rest: character %u got up", character);
		}
		return;
	}
	++g_stat.lie_downs;
	CGameObject* obj = host_world_ready() ? object_here(character) : nullptr;
	if (!obj)
		return refuse(character, Refusal::Busy); // the host's world is not up, or his character is not in it
	const bool alive = survival::life_of(character) == survival::Life::Alive;
	const bool fight = alive && checkpoints::sources::in_combat(obj);
	u16 source = checkpoint::kNoSource;
	Place place = Place::None;
	if (alive && !rq.forced)
		place = checkpoints::sources::rest_place(character, source);
	Refusal why = check_lie_down(alive, g_host.group.sleeping(), rq.forced, fight, place != Place::None);
	if (why == Refusal::None && !g_host.group.may_lie_down(character))
		why = Refusal::Busy; // asked before the camp is set: a camp never comes without the rest
	if (why != Refusal::None)
		return refuse(character, why);
	// T11 review M2: a rest that is not a faint always comes with his camp there (CHECKPOINTS_DEATH §4.1). A camp the
	// store refuses (host_set logged why: off the AI map, not this level...) refuses the lie-down, and he is told "camp".
	if (!rq.forced)
	{
		if (!checkpoints::host_set(character, checkpoints::record_here(obj, kind_of(place), source)))
			return refuse(character, Refusal::Camp);
		++g_stat.camps;
	}
	g_host.group.lie_down(character, int(rq.hours), rq.forced, fight); // may_lie_down said it takes him
	if (log_allowed(g_stat.lie_downs))
		::Msg("[coop] rest: character %u rests %u h (%s%s)", character, u32(g_host.group.hours_of(character)),
		    rq.forced ? "passed out" : checkpoints::sources::place_name(place), fight ? ", in a fight" : "");
}

// The hours skipped as stock change_game_time does (level_script.cpp): the environment's clock and ALife's.
void skip_time(u8 hours)
{
	worldstate::intercept_time_jump(0, hours, 0); // the host: only marks a TimeSync to go out at once
	if (!ai().get_alife())
		return;
	const u32 seconds = u32(hours) * 3600;
	g_pGamePersistent->Environment().ChangeGameTime(float(seconds));
	alife_rw().time_manager().change_game_time(seconds * 1000);
}

// decide said start: everybody present is Sleeping already (Group::tick).
void start_sleep(u64 now, u8 hours)
{
	++g_stat.sleeps;
	g_host.sleep_hours = hours;
	host_send_state(now, true); // every player reads "sleeping" before his RestSleep (the host player too: one lane)
	skip_time(hours);
	Writer w;
	write_sleep(w, hours);
	u32 sleepers = 0;
	for (const Entry& e : g_host.group.entries())
	{
		if (e.state != State::Sleeping)
			continue;
		++sleepers;
		const PeerId peer = survival::peer_of_character(e.character); // F3a T7: the host player's over the link
		if (peer != kInvalidPeer)
			net::host_send(peer, Msg::RestSleep, w);
	}
	autosave::request(autosave::Reason::Sleep); // written once the host is awake again (autosave: busy while asleep)
	::Msg("[coop] rest: %u player(s) sleep %u h, the time is skipped", sleepers, u32(hours));
}

// Every kTickMs: who is present, rests that end, the decision.
void host_tick(u64 now)
{
	std::vector<Seen> present;
	present.reserve(kMaxPlayers);
	xr_vector<u16> characters;
	characters.push_back(0);
	for (const PeerInfo& p : mgr().session().peers())
	{
		const u16 proxy = join::proxy_of_peer(p.id);
		if (proxy != kNoId && proxy != 0 && join::host_player_in_world(proxy) && !world::is_parked(proxy))
			characters.push_back(proxy);
	}
	for (u16 c : characters)
	{
		const survival::Life life = survival::life_of(c);
		if (life == survival::Life::Spectating)
			continue; // out of a hardcore run: not one of the group (rest::decide)
		const CGameObject* obj = object_here(c);
		if (!obj)
			continue;
		Seen s;
		s.id = c;
		s.alive = life == survival::Life::Alive;
		s.in_combat = s.alive && checkpoints::sources::in_combat(obj);
		present.push_back(s);
	}
	// T11 review M1: when everybody rests, each rest that is not a faint is looked at once more (Group::tick calls this
	// once per sleep, not on every tick): one who walked away from his camp loses his rest ("place").
	const Group::PlaceCheck at_place = [](uint16_t id) {
		u16 source = checkpoint::kNoSource;
		return checkpoints::sources::rest_place(id, source) != Place::None;
	};
	const Tick t = g_host.group.tick(present, !world_block_reason().empty(), now, at_place);
	for (const Cancel& c : t.cancelled)
	{
		if (c.reason == Refusal::None)
		{
			++g_stat.left;
			if (log_allowed(g_stat.left))
				::Msg("[coop] rest: character %u is not in the world any more, his rest ends", c.id);
			continue;
		}
		u64& n = g_stat.cancelled[size_t(c.reason)];
		++n;
		if (log_allowed(n))
			::Msg("[coop] rest: the rest of character %u ends (%s)", c.id, refusal_name(c.reason));
		tell_owner(c.id, c.reason);
	}
	if (t.woke)
	{
		++g_stat.woke;
		::Msg("[coop] rest: the shared sleep is over, everybody is awake");
	}
	if (t.started)
		start_sleep(now, t.hours);
}

// The world server's book. F3a T7: the host player's word is no longer taken here: it comes as his RestRequest over the
// internal channel (player_frame), and his Lua hears the book from his copy of RestState (client_on_state).
void host_frame()
{
	host_sync_world();
	if (!host_world_ready())
		return;
	const u64 now = mgr().now_ms();
	if (now >= g_host.next_tick_ms)
	{
		g_host.next_tick_ms = now + kTickMs;
		host_tick(now);
	}
	host_send_state(now, false);
}

// A player's lie-down or get-up (F3a T7: the host player's from peer 0 over the internal channel).
void host_on_request(PeerId from, Reader& rd)
{
	if (!mgr().is_host())
		return; // ClientOnly: only the host takes it
	Request rq;
	if (!read_request(rd, rq))
	{
		++g_stat.malformed;
		if (log_allowed(g_stat.malformed))
			::Msg("! [coop] rest: malformed RestRequest from #%u ignored", from);
		return;
	}
	const u16 character = survival::character_of_peer(from); // never the packet's word
	if (character == kNoId || !survival::player_in_world(character))
	{
		++g_stat.not_in_world;
		if (log_allowed(g_stat.not_in_world))
			::Msg("[coop] rest: RestRequest from #%u without a character in the world ignored", from);
		return;
	}
	host_request(character, rq);
}

// ---------------------------------------------------------------- a player (a client, and the host player: F3a T7)

// The mirror belongs to one world: a newly loaded world starts with everybody awake (the host's next RestState fills
// it), and a word said in the old one is dropped.
void client_sync_world()
{
	const u32 gen = join::world_generation();
	if (g_mirror.world == gen)
		return;
	g_mirror = Mirror();
	g_mirror.world = gen;
	if (g_pending_on.load(std::memory_order_acquire) && g_pending.world != gen)
		drop_pending();
}

const Entry* mirror_find(u16 local_id)
{
	for (const Entry& e : g_mirror.entries)
		if (e.character == local_id)
			return &e;
	return nullptr;
}

void client_on_state(PeerId from, Reader& rd)
{
	if (!is_player() || from != kHostPeer)
		return; // HostOnly: the session kicks a client that sends it
	client_sync_world();
	StateMsg m;
	if (!read_state(rd, m))
	{
		++g_stat.malformed;
		if (log_allowed(g_stat.malformed))
			::Msg("! [coop] rest: malformed RestState from the host ignored");
		return;
	}
	++g_stat.received;
	for (Entry& e : m.entries)
		e.character = join::to_local(e.character);
	// to_local swaps 0 and my proxy: the order of the host's numbering is not ours
	std::sort(m.entries.begin(), m.entries.end(), [](const Entry& a, const Entry& b) { return a.character < b.character; });
	g_mirror.block = m.block;
	g_mirror.entries = m.entries;
	if (m.refused != Refusal::None)
		++g_stat.refusals;
	tell_lua(g_mirror.entries, m.refused); // the changes, then why (M4: as on the host)
}

void client_on_sleep(PeerId from, Reader& rd)
{
	if (!is_player() || from != kHostPeer)
		return;
	client_sync_world();
	u8 hours = 0;
	if (!read_sleep(rd, hours))
	{
		++g_stat.malformed;
		if (log_allowed(g_stat.malformed))
			::Msg("! [coop] rest: malformed RestSleep from the host ignored");
		return;
	}
	const Entry* me = mirror_find(0);
	if (!me || me->state != State::Sleeping)
	{
		// The RestState that put me to sleep comes first on the same lane: without it this is not my sleep.
		++g_stat.sleeps_ignored;
		if (log_allowed(g_stat.sleeps_ignored))
			::Msg("! [coop] rest: RestSleep %u h while I do not sleep, ignored", u32(hours));
		return;
	}
	++g_stat.sleeps_seen;
	script::queue_callback(kSleepCallback, {script::cb_int(hours)});
}

// This machine's player: his word goes to the world server (F3a T7: the host player's over the internal channel, at the
// pace a client keeps), his Lua hears his copy of the book.
bool player_live() { return mgr().is_host() || mgr().is_live_client(); }

void player_frame()
{
	client_sync_world();
	if (g_pending_on.load(std::memory_order_acquire) && player_live())
	{
		if (g_pacer.take(Msg::RestRequest, mgr().now_ms()))
		{
			Writer w;
			write_request(w, g_pending.rq);
			net::client_send(Msg::RestRequest, w);
			drop_pending();
			++g_stat.asked;
		}
		else if (!g_pending.held)
		{
			g_pending.held = true; // sent by a later frame
			++g_stat.held;
		}
	}
	if (player_live())
		tell_lua(g_mirror.entries);
}

// ---------------------------------------------------------------- this machine's word

bool say(Op op, int hours, bool forced)
{
	if (!mgr().is_host() && !mgr().is_live_client())
		return false;
	Pending p;
	p.rq.op = op;
	p.rq.hours = op == Op::LieDown ? clamp_hours(hours) : u8(0);
	p.rq.forced = op == Op::LieDown && forced;
	p.world = join::world_generation();
	g_pending = p; // the word first (R1), then the flag
	g_pending_on.store(true, std::memory_order_release);
	return true;
}

std::vector<Entry> entries_here()
{
	if (mgr().is_host())
	{
		host_sync_world();
		return g_host.group.entries();
	}
	if (mgr().is_client())
	{
		client_sync_world();
		return g_mirror.entries;
	}
	return {};
}

bool set_world_block(const char* reason)
{
	if (mgr().is_client())
		return false; // the host's world decides
	const std::string now = reason ? reason : "";
	if (now != world_block_reason())
	{
		if (now.empty())
			::Msg("[coop] rest: the world lets the sleep go");
		else
			::Msg("[coop] rest: the world holds the sleep (%s)", now.c_str());
	}
	g_world_block.reason = now;
	g_world_block.world = join::world_generation();
	return true;
}

// ---------------------------------------------------------------- Lua

// A character id from argument `idx`: a whole number 0..0xfffe.
bool arg_id(lua_State* L, int idx, u16& out)
{
	if (lua_type(L, idx) != LUA_TNUMBER)
		return false;
	const double v = double(lua_tonumber(L, idx));
	if (!std::isfinite(v) || v < 0.0 || v >= double(kNoId) || v != std::floor(v))
		return false;
	out = u16(v);
	return true;
}

// coop.rest.lie_down(hours [, forced]) -> bool
int l_lie_down(lua_State* L)
{
	bool ok = lua_type(L, 1) == LUA_TNUMBER;
	const double h = ok ? double(lua_tonumber(L, 1)) : 0.0;
	ok = ok && std::isfinite(h);
	const int t = lua_type(L, 2);
	ok = ok && (t == LUA_TNONE || t == LUA_TNIL || t == LUA_TBOOLEAN);
	const bool forced = t == LUA_TBOOLEAN && lua_toboolean(L, 2) != 0;
	const int hours = h < 0.0 ? 0 : (h > 24.0 ? 24 : int(h)); // clamp_hours makes 0 an hour
	lua_pushboolean(L, ok && say(Op::LieDown, hours, forced) ? 1 : 0);
	return 1;
}

// coop.rest.get_up() -> bool
int l_get_up(lua_State* L)
{
	lua_pushboolean(L, say(Op::GetUp, 0, false) ? 1 : 0);
	return 1;
}

// coop.rest.state_of(local_id) -> "awake" | "resting" | "sleeping"
int l_state_of(lua_State* L)
{
	u16 id = 0;
	lua_pushstring(L, state_name(arg_id(L, 1, id) ? state_of(id) : State::Awake));
	return 1;
}

// coop.rest.hours_of(local_id) -> number
int l_hours_of(lua_State* L)
{
	u16 id = 0;
	lua_pushnumber(L, lua_Number(arg_id(L, 1, id) ? hours_of(id) : 0));
	return 1;
}

// coop.rest.waiting_for() -> { nick, ... }: the present players who are awake.
int l_waiting_for(lua_State* L)
{
	const std::vector<uint16_t> ids = waiting(entries_here());
	lua_createtable(L, int(ids.size()), 0);
	int i = 0;
	for (u16 id : ids)
	{
		lua_pushstring(L, nick_of_local(id).c_str());
		lua_rawseti(L, -2, ++i);
	}
	return 1;
}

// coop.rest.block() -> "none" | "not_all" | "combat" | "world" | "dead"
int l_block(lua_State* L)
{
	Block b = Block::None;
	if (mgr().is_host())
	{
		host_sync_world();
		b = g_host.group.block();
	}
	else if (mgr().is_client())
	{
		client_sync_world();
		b = g_mirror.block;
	}
	lua_pushstring(L, block_name(b));
	return 1;
}

// coop.rest.set_world_block(reason) -> bool: the host (or no session); "" lets the sleep go.
int l_set_world_block(lua_State* L)
{
	size_t n = 0;
	const char* s = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &n) : nullptr;
	lua_pushboolean(L, s && n <= kMaxWorldReason && set_world_block(s) ? 1 : 0);
	return 1;
}

// ---------------------------------------------------------------- console

// "<host id> <nick>: <state>[ <hours> h]"
void entry_line(const Entry& e, u16 host_id, string256& buf)
{
	if (e.state == State::Awake)
		xr_sprintf(buf, "%u %s: awake", u32(host_id), survival::nick_of(host_id).c_str());
	else
		xr_sprintf(buf, "%u %s: %s %u h", u32(host_id), survival::nick_of(host_id).c_str(), state_name(e.state),
		    u32(e.hours));
}

class CCC_CoopRestStatus : public IConsole_Command
{
public:
	CCC_CoopRestStatus(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
	void Execute(LPCSTR) override { print_status(); }
	void Info(TInfo& I) override { xr_strcpy(I, "co-op: who rests and what holds the shared sleep"); }
};
} // namespace

// ---------------------------------------------------------------- lifecycle

void install_server(Session& s) { s.on(Msg::RestRequest, &host_on_request); }

void install_client(Endpoint& e)
{
	e.on(Msg::RestState, &client_on_state);
	e.on(Msg::RestSleep, &client_on_sleep);
}

void reset()
{
	// The session starts or ends: everybody is awake. No callback says so at the end (script::reset drops the queue,
	// and outside a session nothing is delivered): Lua reads state_of, which is "awake" now.
	g_host = Host();
	g_mirror = Mirror();
	drop_pending();
	g_pacer.reset();
	g_told.clear();
	g_stat = Stats();
}

void frame()
{
	if (mgr().is_host())
		host_frame();
	if (is_player())
		player_frame();
}

void print_status()
{
	const std::string& wb = world_block_reason();
	if (mgr().is_host())
	{
		::Msg("[coop]   rest: %u resting, block %s, world %s, sleep %s", u32(g_host.group.resting()),
		    block_name(g_host.group.block()), wb.empty() ? "free" : wb.c_str(),
		    g_host.group.sleeping() ? "running" : "no");
		const u64* r = g_stat.refused;
		const u64* c = g_stat.cancelled;
		::Msg("[coop]   rest: lie-downs %llu (refused place %llu, combat %llu, dead %llu, busy %llu, camp %llu), get-ups "
		      "%llu, camps set %llu; rests ended: place at the start %llu, combat %llu, dead %llu, busy %llu, left %llu; "
		      "sleeps %llu (last %u h), woke %llu; RestState sent %llu; malformed %llu, without a character %llu",
		    g_stat.lie_downs, r[size_t(Refusal::Place)], r[size_t(Refusal::Combat)], r[size_t(Refusal::Dead)],
		    r[size_t(Refusal::Busy)], r[size_t(Refusal::Camp)], g_stat.get_ups, g_stat.camps, c[size_t(Refusal::Place)],
		    c[size_t(Refusal::Combat)], c[size_t(Refusal::Dead)], c[size_t(Refusal::Busy)], g_stat.left, g_stat.sleeps,
		    u32(g_host.sleep_hours), g_stat.woke, g_stat.sent, g_stat.malformed, g_stat.not_in_world);
		for (const Entry& e : g_host.group.entries())
		{
			string256 buf;
			entry_line(e, e.character, buf);
			::Msg("[coop]   rest of %s%s", buf, g_host.group.forced(e.character) ? " (passed out)" : "");
		}
		::Msg("[coop]   rest (the host player, over the internal channel): asked %llu (held back %llu), RestState "
		      "received %llu (refusals %llu), sleeps %llu (ignored %llu)",
		    g_stat.asked, g_stat.held, g_stat.received, g_stat.refusals, g_stat.sleeps_seen, g_stat.sleeps_ignored);
	}
	else if (mgr().is_client())
	{
		::Msg("[coop]   rest: block %s; asked %llu (held back %llu), RestState received %llu (refusals %llu), sleeps %llu "
		      "(ignored %llu), malformed %llu",
		    block_name(g_mirror.block), g_stat.asked, g_stat.held, g_stat.received, g_stat.refusals, g_stat.sleeps_seen,
		    g_stat.sleeps_ignored, g_stat.malformed);
		for (const Entry& e : g_mirror.entries)
		{
			string256 buf;
			entry_line(e, join::to_host(e.character), buf);
			::Msg("[coop]   rest of %s", buf);
		}
	}
}

void register_console() { CMD1(CCC_CoopRestStatus, "coop_rest_status"); }

// Fills coop.rest (made here: the table is ours).
void register_lua(lua_State* L)
{
	static const luaL_Reg fns[] = {
		{"lie_down", &l_lie_down},
		{"get_up", &l_get_up},
		{"state_of", &l_state_of},
		{"hours_of", &l_hours_of},
		{"waiting_for", &l_waiting_for},
		{"block", &l_block},
		{"set_world_block", &l_set_world_block},
		{nullptr, nullptr},
	};
	lua_pushstring(L, "coop");
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		return; // the registrator logs it
	}
	lua_newtable(L);
	for (const luaL_Reg* f = fns; f->name; ++f)
	{
		lua_pushcfunction(L, f->func);
		lua_setfield(L, -2, f->name);
	}
	lua_setfield(L, -2, "rest"); // coop.rest = { ... }
	lua_pop(L, 1); // coop
}

// ---------------------------------------------------------------- public

State state_of(u16 local_id)
{
	if (mgr().is_host())
	{
		host_sync_world();
		return g_host.group.state_of(local_id);
	}
	if (mgr().is_client())
	{
		client_sync_world();
		const Entry* e = mirror_find(local_id);
		return e ? e->state : State::Awake;
	}
	return State::Awake;
}

u8 hours_of(u16 local_id)
{
	if (mgr().is_host())
	{
		host_sync_world();
		return g_host.group.hours_of(local_id);
	}
	if (mgr().is_client())
	{
		client_sync_world();
		const Entry* e = mirror_find(local_id);
		return e ? e->hours : u8(0);
	}
	return 0;
}

void host_cancel(u16 character, const char* reason)
{
	if (!mgr().is_host())
		return;
	host_sync_world();
	if (!g_host.group.get_up(character))
		return;
	const Refusal why = refusal_from_name(reason);
	u64& n = g_stat.cancelled[size_t(why)];
	++n;
	if (log_allowed(n))
		::Msg("[coop] rest: the rest of character %u ends (%s, host_cancel '%.32s')", character, refusal_name(why),
		    reason ? reason : "");
	tell_owner(character, why);
}
} // namespace rest
} // namespace coop
