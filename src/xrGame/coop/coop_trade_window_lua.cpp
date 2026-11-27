// Anomaly Coop — trade: Anomaly's Lua trade window and the trading computer's permits. See coop_trade.h.
#include "stdafx.h"
#include "pch_script.h"
#include "coop_trade_impl.h"
#include "coop_join.h"
#include "coop_manager.h"
#include "coop_net.h"
#include "xrCoop/coop_mode.h"
#include "xrCoop/coop_lua_fn.h"

#include "GameObject.h"
#include "ai_space.h"
#include "script_engine.h"

#include <limits>

namespace coop
{
namespace trade
{
namespace
{
u32 g_permit_world = 0; // join::world_generation() of the permits
u32 g_permit_errors = 0;
u32 g_window_errors = 0;

// <module>.<fn> under the protected call of coop_lua_fn.h: push(L) pushes the arguments and returns their number, read(L) reads the
// `nres` results at the stack top. A Lua error is logged the first five times (`errors` counts them), `consequence` after "failed".
template <class Push, class Read>
luafn::Fail call_lua(const char* module, const char* fn, const char* consequence, u32& errors, Push push, int nres, Read read)
{
	std::string error;
	const luafn::Fail f = luafn::call_named(ai().script_engine().lua(), module, fn, nullptr, push, nres, read, error);
	if (f == luafn::Fail::LuaError && errors++ < 5)
		::Msg("! [coop] %s.%s failed%s: %s", module, fn, consequence, error.c_str());
	return f;
}

// coop_mods_trade_computer.<fn>(a, b). out: the three values it returned, NaN where it gave no number, all NaN when the script or the
// function is missing (no trading computer: no grants); the caller bounds them with trade_permit::object_of / ttl_of / reach_of.
// False: no Lua here, or a Lua error.
bool call_trade_computer(const char* fn, double a, double b, double out[3])
{
	out[0] = out[1] = out[2] = std::numeric_limits<double>::quiet_NaN();
	const auto push = [&](lua_State* L)
	{
		lua_pushnumber(L, lua_Number(a));
		lua_pushnumber(L, lua_Number(b));
		return 2;
	};
	const auto read = [&](lua_State* L)
	{
		for (int i = 0; i < 3; ++i)
			if (lua_type(L, i - 3) == LUA_TNUMBER)
				out[i] = double(lua_tonumber(L, i - 3));
	};
	const luafn::Fail f = call_lua("coop_mods_trade_computer", fn, " (the stock rules stand)", g_permit_errors, push, 3, read);
	return f == luafn::Fail::None || f == luafn::Fail::Missing;
}

// coop_trade_window.<fn>(...): push(L) pushes the arguments and returns their number, read(L, at) reads the two results at `at` and
// `at + 1`. Missing: no such script or function (an older gamedata), nothing read.
template <class Push, class Read>
luafn::Fail call_trade_window(const char* fn, Push push, Read read)
{
	return call_lua("coop_trade_window", fn, "", g_window_errors, push, 2, [&](lua_State* L) { read(L, lua_gettop(L) - 1); });
}

// A Lua array of ids.
void push_ids(lua_State* L, const xr_vector<u16>& ids)
{
	lua_createtable(L, int(ids.size()), 0);
	for (size_t i = 0; i < ids.size(); ++i)
	{
		lua_pushnumber(L, lua_Number(ids[i]));
		lua_rawseti(L, -2, int(i + 1));
	}
}
} // namespace

// ---------------------------------------------------------------- remote trade through a trading computer
// A trading computer lists traders far away and opens the stock trade. The decider's permits (xrCoop/coop_trade_permit.h), one per
// player, for the world they were issued in. Its Lua issues a grant (coop_mods_trade_computer.script: a real computer, the player
// at it, a trader it lists); the player's trade window with that trader opening here makes it his permit, its close or a new window
// drops it. Without one a deal keeps the stock rules (the talk and kTradeReach): remote trade with any NPC stays impossible.
trade_permit::Book g_permits;
u64 g_permits_issued = 0, g_permits_dropped = 0, g_permit_deals = 0;

trade_permit::Book& permits()
{
	const u32 gen = join::world_generation();
	if (gen != g_permit_world)
	{
		g_permit_world = gen; // a new world: the computers and the traders of the old one are gone
		g_permits.clear();
	}
	return g_permits;
}

float distance_to(CGameObject* who, CObject* what)
{
	if (!who || !what || what->getDestroy())
		return -1.f;
	return who->Position().distance_to(what->Position());
}

// Player `from`'s trade window with `trader_id` opened here: the grant the decider's Lua kept for it (take_grant, taken once)
// becomes his permit when the computer it names is here and his character stands at it. Any older permit of his goes: one
// window, one permit.
void host_take_permit(PeerId from, u16 trader_id)
{
	trade_permit::Book& book = permits();
	book.revoke(from);
	double g[3];
	if (!call_trade_computer("take_grant", double(from), double(trader_id), g))
		return;
	const u16 computer = trade_permit::object_of(g[0]);
	if (computer == trade_permit::kNoObject)
		return; // no grant for this window: the stock rules
	trade_permit::Facts f;
	f.player = from;
	f.trader = trader_id;
	f.now = mgr().now_ms();
	f.to_computer = distance_to(smart_cast<CGameObject*>(find_object(join::proxy_of_peer(from))), find_object(computer));
	trade_permit::Verdict v = trade_permit::Verdict::None;
	if (!world::is_player_proxy(computer) && !world::is_player_proxy(trader_id) &&
	    book.issue(from, computer, trader_id, f.now, trade_permit::ttl_of(g[1]), trade_permit::reach_of(g[2])))
		v = book.judge(f);
	if (!trade_permit::granted(v))
	{
		book.revoke(from);
		++g_permits_dropped;
		::Msg("! [coop] trade: player #%u's computer grant for %u through %u is not kept (%s)", from, u32(trader_id),
		    u32(computer), trade_permit::verdict_name(v));
		return;
	}
	++g_permits_issued;
	::Msg("[coop] trade: player #%u may trade with %u through computer %u", from, u32(trader_id), u32(computer));
}

// The deal of player `from` with `trader_id` through his permit: Granted, or why not (None: he holds none).
trade_permit::Verdict permit_verdict(PeerId from, CInventoryOwner* proxy, u16 trader_id)
{
	trade_permit::Book& book = permits();
	const trade_permit::Permit* p = book.find(from);
	if (!p)
		return trade_permit::Verdict::None;
	trade_permit::Facts f;
	f.player = from;
	f.trader = trader_id;
	f.now = mgr().now_ms();
	f.to_computer = distance_to(smart_cast<CGameObject*>(proxy), find_object(p->computer));
	return book.judge(f);
}

// A trade window with `partner` opens on this machine: the grant its Lua got for that trader through a trading computer
// (coop_mods_trade_computer.window_grant, taken once), else none (the stock window). The co-op host's own and a single player's
// trade never ask, so there is none there.
void client_take_window(CInventoryOwner* partner)
{
	g_view.remote = trade_permit::Window();
	const u16 trader = id_of(partner);
	if (trader == kNoId)
		return;
	double g[3];
	if (call_trade_computer("window_grant", double(trader), 0.0, g))
		g_view.remote = trade_permit::window_of(trader, g[0], g[1], mgr().now_ms());
}

// ---------------------------------------------------------------- Anomaly's Lua trade window (scripts\coop_trade_window.script)

// coop_trade_window.host_price here: the sums of both lists of a player's deal by the Lua window's formula, the host's own window's
// formula, run as for the decider itself, no player character put in (the trader's lists and discounts do not depend on the
// character); the player's items are held by his character here, and the adapters treat any player as the holder their checks look
// for. Priced: two numbers. Refused: nil and why (`why`: what the script said), or a Lua error, or no Lua. NoFormula: nothing at
// all, the decider's gamedata has no such function, or it returned nothing.
trade_window::Answer ask_host_price(u16 proxy_id, u16 trader_id, const Deal& d, double raw[2], std::string& why)
{
	trade_window::Answer answer = trade_window::Answer::Refused;
	const luafn::Fail f = call_trade_window("host_price", [&](lua_State* L)
	{
		lua_pushnumber(L, lua_Number(proxy_id));
		lua_pushnumber(L, lua_Number(trader_id));
		push_ids(L, d.sell);
		push_ids(L, d.buy);
		return 4;
	}, [&](lua_State* L, int at)
	{
		if (lua_type(L, at) == LUA_TNUMBER && lua_type(L, at + 1) == LUA_TNUMBER)
		{
			answer = trade_window::Answer::Priced;
			raw[0] = double(lua_tonumber(L, at));
			raw[1] = double(lua_tonumber(L, at + 1));
		}
		else if (lua_isnoneornil(L, at) && lua_isnoneornil(L, at + 1))
			answer = trade_window::Answer::NoFormula;
		else if (lua_type(L, at + 1) == LUA_TSTRING)
			why = lua_tostring(L, at + 1);
	});
	if (f == luafn::Fail::Missing)
		return trade_window::Answer::NoFormula;
	return f == luafn::Fail::None ? answer : trade_window::Answer::Refused;
}

void window_on_result(u32 seq, u8 code, s32 delta, const char* why, u32 decider_sell, u32 decider_buy)
{
	call_trade_window("on_result", [&](lua_State* L)
	{
		lua_pushnumber(L, lua_Number(seq));
		lua_pushnumber(L, lua_Number(code));
		lua_pushnumber(L, lua_Number(delta));
		lua_pushstring(L, why);
		lua_pushnumber(L, lua_Number(decider_sell)); // PriceChanged only, else 0
		lua_pushnumber(L, lua_Number(decider_buy));
		return 6;
	}, [](lua_State*, int) {});
}

bool lua_trade_window_allowed()
{
	// A copy gets the host's window once the Lua adapter has its Sell, Buy and Donate (coop_trade_window.ready()).
	const bool mirror = is_mirror();
	bool adapter = false;
	std::string said;
	if (mirror)
		call_trade_window("ready", [](lua_State*) { return 0; }, [&](lua_State* L, int at)
		{
			adapter = lua_toboolean(L, at) != 0;
			if (lua_type(L, at + 1) == LUA_TSTRING)
				said = lua_tostring(L, at + 1);
		});
	const bool lua = trade_window::choose(mirror, adapter) == trade_window::Window::Lua;
	if (net::live()) // which window a player saw, and why: the log answers "a different window than the host's"
	{
		const std::string tail = said.empty() ? std::string() : " (" + said + ")";
		::Msg("[coop] trade window: %s - %s%s", lua ? "Anomaly's Lua window, as the host's" : "the engine window",
		    trade_window::why(mirror, adapter), tail.c_str());
	}
	return lua;
}
} // namespace trade
} // namespace coop
