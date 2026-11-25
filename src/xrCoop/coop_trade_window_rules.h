// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop (TZ R90) - which trade window a player sees, and the deal of Anomaly's Lua window (G.A.M.M.A.'s ui_inventory).
//
// The host, single player and a server's process trade in the Lua inventory window. Its Sell, Buy and Donate move every item with
// transfer_item and the money with give_money on the machine itself; on a player's copy that moved the trader's money there only,
// so a copy used to get the engine's trade window (coop_trade.cpp lua_trade_window_allowed, CUIGameSP::StartTrade). Now a copy gets
// the Lua window too once scripts\coop_trade_window.script has taken its three buttons over: they send one TradeOp Deal to the
// decider (coop.trade_deal), which checks and moves everything or nothing as for the engine window. Such a deal (flag 2) carries the
// sums the window showed. TZ R92: they are its price, as in single player; the decider computes the same G.A.M.M.A. formula as for
// itself (coop_trade_window.host_price, no player character put in), compares each side within a tolerance (settle_side) and refuses
// a side beyond it (PriceChanged).
// Pure C++17, header-only: used by the engine (coop_trade.cpp) and the unit tests (tests/test_trade_window_rules.cpp).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace coop
{
namespace trade_window
{
// TradeOp Deal flags (coop_trade.h). Bit 2 came with protocol 35: a deal with it carries the two sums its window showed after the
// lists, the deal's price (TZ R92, settle_side); a reader of 34 never meets it (Hello refuses the other number).
constexpr uint8_t kDealDonate = 1; // a gift: free, nothing bought (the stock DonateCurrentItem / Action_Donate)
constexpr uint8_t kDealLuaPrices = 2; // R90: the Lua window's deal; TZ R92: its shown sums are the price, checked by host_price
constexpr uint8_t kDealFlags = kDealDonate | kDealLuaPrices;

enum class Window : uint8_t
{
	Lua, // Anomaly's inventory window, as the host has it
	Engine, // CUIActorMenu in mmTrade
};

// `mirror`: this process's objects are copies (a co-op client, a server's player); `adapter`: on this machine the Lua window's trade
// buttons send their deal to the decider (coop_trade_window.ready()). Only a copy without the adapter keeps the engine window: there
// the Lua window would trade on its own.
inline Window choose(bool mirror, bool adapter) { return (!mirror || adapter) ? Window::Lua : Window::Engine; }

// For the log line of StartTrade.
inline const char* why(bool mirror, bool adapter)
{
	if (!mirror)
		return "this process trades for itself";
	return adapter ? "its Sell, Buy and Donate send one deal to the decider"
	               : "the Lua window cannot send its deals to the decider here";
}

// A whole number of Lua below `none`, as an object id.
inline bool id_of(double v, uint16_t none, uint16_t& out)
{
	if (!(v >= 0.0) || v >= double(none) || std::floor(v) != v)
		return false;
	out = uint16_t(v);
	return true;
}

enum class ListCheck : uint8_t
{
	Ok,
	Empty, // nothing to trade
	TooMany, // more items than one deal carries
	Repeated, // an item twice, on one side or on both
	BadId, // the id that means "none"
	GiftBuys, // a gift with something bought
};

inline const char* list_check_name(ListCheck c)
{
	switch (c)
	{
	case ListCheck::Ok: return "ok";
	case ListCheck::Empty: return "nothing to trade";
	case ListCheck::TooMany: return "too many items for one deal";
	case ListCheck::Repeated: return "an item is listed twice";
	case ListCheck::BadId: return "an item id is no id";
	case ListCheck::GiftBuys: return "a gift buys nothing";
	default: return "?";
	}
}

// The lists of one deal before it leaves (the decider checks them again, host_apply).
inline ListCheck check_lists(const std::vector<uint16_t>& sell, const std::vector<uint16_t>& buy, bool donate, size_t max_items,
    uint16_t none)
{
	const size_t n = sell.size() + buy.size();
	if (!n)
		return ListCheck::Empty;
	if (n > max_items)
		return ListCheck::TooMany;
	if (donate && !buy.empty())
		return ListCheck::GiftBuys;
	std::vector<uint16_t> seen;
	seen.reserve(n);
	for (const std::vector<uint16_t>* list : {&sell, &buy})
		for (uint16_t id : *list)
		{
			if (id == none)
				return ListCheck::BadId;
			for (uint16_t s : seen)
				if (s == id)
					return ListCheck::Repeated;
			seen.push_back(id);
		}
	return ListCheck::Ok;
}

// One item's price as the window computes it (a float): whole roubles for the actor_on_trade callback; no number or below 0 -> 0.
inline uint32_t item_price(double p)
{
	if (!(p > 0.0))
		return 0;
	if (p >= 4294967295.0)
		return 0xffffffffu;
	return uint32_t(std::floor(p));
}

// The sum of one side as the window shows it (TMode_UpdatePrice, TMode_Sell, TMode_Buy): the item prices added up, one floor at the
// end. False: a price that is no number or below 0, or a sum over `max`.
inline bool total_of(const std::vector<double>& prices, uint32_t max, uint32_t& out)
{
	double sum = 0.0;
	for (double p : prices)
	{
		if (!std::isfinite(p) || p < 0.0)
			return false;
		sum += p;
	}
	const double floored = std::floor(sum);
	if (!std::isfinite(floored) || floored > double(max))
		return false;
	out = uint32_t(floored);
	return true;
}

enum class Pricing : uint8_t
{
	Free, // a gift
	Engine, // the engine window's deal: CTrade::GetItemPrice on the decider is what that window showed (R4 2)
	Lua, // the Lua window's deal: the sums it showed (TZ R92)
};

inline Pricing pricing_of(bool donate, bool lua_prices)
{
	if (donate)
		return Pricing::Free;
	return lua_prices ? Pricing::Lua : Pricing::Engine;
}

enum class PriceVerdict : uint8_t
{
	Ok,
	NoAnswer, // no Lua here, no such function, a Lua error, or the script refused (an item it does not trade)
	BadNumber, // no number, or below 0
	Zero, // a list that is not empty and costs nothing: the window's button is off at 0 RU
	TooMuch, // more than money can be
	Changed, // TZ R92: the decider's formula and the window's sum of a side differ beyond the tolerance (settle_side)
};

inline const char* price_verdict_name(PriceVerdict v)
{
	switch (v)
	{
	case PriceVerdict::Ok: return "ok";
	case PriceVerdict::NoAnswer: return "no price from the Lua window's formula";
	case PriceVerdict::BadNumber: return "the Lua window's price is no number";
	case PriceVerdict::Zero: return "the list costs nothing";
	case PriceVerdict::TooMuch: return "the price is over the most money there is";
	case PriceVerdict::Changed: return "the decider's price is not the one the window showed";
	default: return "?";
	}
}

// TradeResult PriceChanged (coop_trade.cpp), RESULT_PRICE in scripts\coop_trade_window.script: a deal refused for its price. Its Result
// carries the decider's price of each side after the trader's money (protocol 35, coop_trade.h); TZ R92: for the log and the player's
// text only, nothing agrees to it. The player's window takes the trader's data again.
constexpr uint8_t kPriceChanged = 10;

// What coop_trade_window.host_price answered on the decider (coop_trade.cpp ask_host_price).
enum class Answer : uint8_t
{
	Priced, // two numbers
	Refused, // nil and why (an item the window does not trade, a Lua error inside), or the call itself failed
	NoFormula, // nothing at all: the decider's gamedata has no coop_trade_window.host_price (older than R90)
};

// TZ R92: what the decider does with the shown sums of a deal, by what host_price answered.
enum class Check : uint8_t
{
	Compare, // the decider's G.A.M.M.A. formula meets the shown sums (settle_side); the deal goes at the shown sums
	EngineOnly, // the engine window's CTrade sums as they are: there is no other formula on the decider, as before R90
	Refuse, // no price
};

// Review of 2026-11-25, TZ R92: the formula is the decider's own window's (the Lua window's: the host trades in it), not the player's
// choice of flag. A Lua window's deal (flag 2) is compared with the sums it sent; an engine window's deal (flag 0) with the CTrade sums
// the decider computes for it, which that window showed. Flag 0 is so never a way around the G.A.M.M.A. formula (Weapon Parts Overhaul's
// busted parts, review of 2026-11-24): what host_price refuses is refused whichever flag came. Only a decider without the formula keeps
// the engine sums alone; a Lua window's deal there has no price.
inline Check check_of(bool lua_window_deal, Answer a)
{
	if (a == Answer::Priced)
		return Check::Compare;
	return (a == Answer::NoFormula && !lua_window_deal) ? Check::EngineOnly : Check::Refuse;
}

// The decider's sum of one side of a deal (host_price answered `raw` for it). An empty side costs 0 whatever came.
// There is no fallback price: any other verdict refuses the deal.
inline PriceVerdict host_total(bool has_items, bool answered, double raw, uint32_t max, uint32_t& out)
{
	out = 0;
	if (!has_items)
		return PriceVerdict::Ok;
	if (!answered)
		return PriceVerdict::NoAnswer;
	if (!std::isfinite(raw) || raw < 0.0)
		return PriceVerdict::BadNumber;
	const double floored = std::floor(raw);
	if (floored < 1.0)
		return PriceVerdict::Zero;
	if (floored > double(max))
		return PriceVerdict::TooMuch;
	out = uint32_t(floored);
	return PriceVerdict::Ok;
}

// The rounding of a side's sum: the window and the decider each add their prices up and floor once, and the two sums of the same prices
// can floor 1 RU apart (G.A.M.M.A. adds a cell's prices in pairs() order, utils_ui.script:1994-2013).
constexpr uint32_t kRoundingRu = 1;

// TZ R92: how far the decider's sum of a side may be from the shown one, either way, without a refusal. A Lua window's deal: the
// rounding of the sum, and 1 RU an item for its condition, which travels as a u16 each way (coop_items.cpp:308-309) and so may differ
// by a step between the machines. An engine window's deal: CTrade floors every item twice (trade2.cpp: iFloor(base * condition * factor),
// then iFloor(result * discount)) where the formula floors a side once, 2 RU an item more. Saturates at the most a u32 holds.
inline uint32_t tolerance_of(bool lua_window_deal, size_t items)
{
	const uint64_t per_item = lua_window_deal ? 1u : 3u;
	const uint64_t n = uint64_t(items);
	if (n > (0xffffffffull - kRoundingRu) / per_item)
		return 0xffffffffu;
	return uint32_t(kRoundingRu + per_item * n);
}

// TZ R92 (the owner's choice of 2026-11-25, "as in single player"): the player pays and gets exactly what his window showed. The decider
// computes the same G.A.M.M.A. formula as for itself and compares it with the shown sum of each side, the sale and the purchase alike:
// within `tolerance` (tolerance_of) the side goes at the shown sum; beyond it, either way, the deal is refused (Changed: a modified client,
// or the inputs of the price out of sync between the machines), and the player's window takes the trader's data again. A side that is not
// empty and goes at 0 RU is refused: the window's button is off at 0 RU.
inline PriceVerdict settle_side(bool has_items, uint32_t decider, uint32_t shown, uint32_t tolerance, uint32_t& out)
{
	out = 0;
	if (!has_items)
		return PriceVerdict::Ok;
	if ((shown > decider ? shown - decider : decider - shown) > tolerance)
		return PriceVerdict::Changed;
	if (shown < 1)
		return PriceVerdict::Zero;
	out = shown;
	return PriceVerdict::Ok;
}

// TZ R92: which line a refusal for its price tells the player of the engine window (the Lua window tells its own, coop_trade_window
// on_result), from the decider's sums the Result carries and the window's own: the purchase the trader asks more for than shown, the
// sale he pays less for, or else that the window's prices are out of date (a side beyond the tolerance the other way). The side beyond
// the tolerance is told, the purchase first.
enum class PriceTell : uint8_t
{
	Purchase, // the trader asks more than the window showed
	Sale, // the trader pays less than the window showed
	Stale, // the window's prices are out of date
};

inline PriceTell price_tell(bool lua_window_deal, size_t sell_items, size_t buy_items, uint32_t decider_sell, uint32_t decider_buy,
    uint32_t shown_sell, uint32_t shown_buy)
{
	uint32_t out = 0;
	if (settle_side(buy_items != 0, decider_buy, shown_buy, tolerance_of(lua_window_deal, buy_items), out) == PriceVerdict::Changed)
		return decider_buy > shown_buy ? PriceTell::Purchase : PriceTell::Stale;
	if (settle_side(sell_items != 0, decider_sell, shown_sell, tolerance_of(lua_window_deal, sell_items), out) == PriceVerdict::Changed)
		return decider_sell < shown_sell ? PriceTell::Sale : PriceTell::Stale;
	return PriceTell::Stale;
}
} // namespace trade_window
} // namespace coop
