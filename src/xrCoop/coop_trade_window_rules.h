// Ядро: без проверок режима (docs/MODES.md)
// Anomaly Coop (TZ R90) - which trade window a player sees, and the deal of Anomaly's Lua window (G.A.M.M.A.'s ui_inventory).
//
// The host, single player and a server's process trade in the Lua inventory window. Its Sell, Buy and Donate move every item with
// transfer_item and the money with give_money on the machine itself; on a player's copy that moved the trader's money there only,
// so a copy used to get the engine's trade window (coop_trade.cpp lua_trade_window_allowed, CUIGameSP::StartTrade). Now a copy gets
// the Lua window too once scripts\coop_trade_window.script has taken its three buttons over: they send one TradeOp Deal to the
// decider (coop.trade_deal), which checks and moves everything or nothing as for the engine window. The decider prices such a deal
// with the window's own formula (utils_item.get_item_cost with the trader's profile, under the player's character), flag 2.
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
// lists (settle_side); a reader of 34 never meets it (Hello refuses the other number).
constexpr uint8_t kDealDonate = 1; // a gift: free, nothing bought (the stock DonateCurrentItem / Action_Donate)
constexpr uint8_t kDealLuaPrices = 2; // R90: the Lua window's deal, priced with its formula (coop_trade_window.host_price)
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
	Engine, // CTrade::GetItemPrice (the engine window's deal)
	Lua, // the Lua window's formula (coop_trade_window.host_price)
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
	Changed, // review of 2026-11-25: the decider's price is worse for the player than his window showed (settle_side)
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
// carries the decider's price of each side after the trader's money (protocol 35, coop_trade.h).
constexpr uint8_t kPriceChanged = 10;

// What coop_trade_window.host_price answered on the decider (coop_trade.cpp ask_host_price).
enum class Answer : uint8_t
{
	Priced, // two numbers
	Refused, // nil and why (an item the window does not trade, a Lua error inside), or the call itself failed
	NoFormula, // nothing at all: the decider's gamedata has no coop_trade_window.host_price (older than R90)
};

enum class Charge : uint8_t
{
	Settle, // the decider's formula, settled against the sums the player's window showed (settle_side)
	EngineOnly, // the engine window's sums as they are (CTrade): there is no other formula on the decider, as before R90
	Refuse, // no price
};

// Review of 2026-11-25: the formula is the decider's own window's (the Lua window's: the host trades in it), not the player's choice of
// flag. A Lua window's deal (flag 2) settles against the sums it sent; an engine window's deal (flag 0) against the engine formula's
// sums, which the decider computes itself and which that window showed. Flag 0 is so never a way around the Lua formula: a modified
// client gets at most the Lua price for a sale and pays at least it for a purchase, whichever flag it sends. Only a decider without
// the Lua formula keeps the engine sums alone; a Lua window's deal there has no price.
inline Charge charge_of(bool lua_window_deal, Answer a)
{
	if (a == Answer::Priced)
		return Charge::Settle;
	return (a == Answer::NoFormula && !lua_window_deal) ? Charge::EngineOnly : Charge::Refuse;
}

// The side a refusal for its price tells the player of: true the purchase (the decider asks more than shown), false the sale.
inline bool tell_purchase(bool has_buy, uint32_t decider_buy, uint32_t shown_buy) { return has_buy && decider_buy > shown_buy; }

// The decider's sum of one side of a Lua window's deal (host_price answered `raw` for it). An empty side costs 0 whatever came.
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

// The rounding a side may differ by without a refusal. The window adds its cells' prices up, the decider its items' prices, each
// floors once, and the two sums of the same prices can floor 1 RU apart (G.A.M.M.A. adds a cell's prices in pairs() order,
// utils_ui.script:1994-2013).
constexpr uint32_t kRoundingRu = 1;

// Review of 2026-11-25 (nit 1): the engine window's sums are CTrade's, which floors every item twice (trade2.cpp: iFloor(base * condition
// * factor), then iFloor(result * discount)), the Lua formula not at all: the two sums of the same list are up to 2 RU an item apart.
// Without that allowance the engine window, which cannot show the decider's price, could not buy at all.
inline uint32_t slack_of(bool lua_window_deal, size_t items)
{
	if (lua_window_deal)
		return kRoundingRu;
	const uint64_t slack = uint64_t(kRoundingRu) + 2u * uint64_t(items);
	return slack > 0xffffffffu ? 0xffffffffu : uint32_t(slack);
}

// R90 review (blocking 2): the decider's formula and the player's window may price a side differently (another discount, a mod that
// replaced the formula after the window took it). So each side is settled against the sum the window showed (sent with the deal,
// protocol 35): a sale (`trader_buys`) pays the smaller sum, a purchase costs what the window showed.
// Review of 2026-11-25: the player never pays more nor gets less than his window showed without seeing it. A side the decider prices
// worse for him than shown by more than `slack` (slack_of: rounding) is refused (Changed); the player's window then shows the decider's
// price (the Result carries it), and the next press, which sends that price as the shown sum, agrees to it. Within the slack the
// purchase costs what was shown and the sale pays the decider's price: rounding never costs the player.
// Abuse: the shown sum comes from the player's machine. A larger one claimed for a sale is refused and answered with the decider's price,
// which the client's own formula knows anyway; a smaller one for a purchase is refused the same way. Agreeing then costs the decider's
// price: a sale never pays more than it, a purchase never costs less than it minus the rounding slack.
// A side that is not empty and settles at 0 RU is refused: the window's button is off at 0 RU.
inline PriceVerdict settle_side(bool has_items, bool trader_buys, uint32_t decider, uint32_t shown, uint32_t slack, uint32_t& out)
{
	out = 0;
	if (!has_items)
		return PriceVerdict::Ok;
	if (trader_buys ? (shown > decider && shown - decider > slack) : (decider > shown && decider - shown > slack))
		return PriceVerdict::Changed;
	const uint32_t settled = trader_buys ? (decider < shown ? decider : shown) : shown;
	if (settled < 1)
		return PriceVerdict::Zero;
	out = settled;
	return PriceVerdict::Ok;
}
} // namespace trade_window
} // namespace coop
