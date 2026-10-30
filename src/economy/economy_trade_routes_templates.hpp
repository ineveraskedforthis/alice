#pragma once


#ifdef NDEBUG
#ifdef _MSC_VER
#define DCON_RELEASE_INLINE __forceinline
#define DCON_RESTRICT __restrict
#else
#define DCON_RELEASE_INLINE inline __attribute__((always_inline))
#define DCON_RESTRICT __restrict__
#endif
#else
#define DCON_RELEASE_INLINE inline
#ifdef _MSC_VER
#define DCON_RESTRICT __restrict
#else
#define DCON_RESTRICT __restrict__
#endif
#endif
namespace adaptive_ve {

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto trade_route_get_volume(sys::state const& state, left_t route, right_t cid) {
	return state.world.trade_route_get_volume(route, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_trade_volume<left_t, right_t> trade_route_get_volume(sys::state const& state, left_t route, right_t cid) {
	return { route, cid };
}

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_price(sys::state const& state, left_t market, right_t cid) {
	return state.world.market_get_price(market, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_price<left_t, right_t> market_get_price(sys::state const& state, left_t market, right_t cid) {
	return { market, cid };
}

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_expected_probability_to_sell(sys::state const& state, left_t market, right_t cid) {
	return state.world.market_get_expected_probability_to_sell(market, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_price<left_t, right_t> market_get_expected_probability_to_sell(sys::state const& state, left_t market, right_t cid) {
	return { market, cid };
}

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_expected_probability_to_buy(sys::state const& state, left_t market, right_t cid) {
	return state.world.market_get_expected_probability_to_buy(market, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_price<left_t, right_t> market_get_expected_probability_to_buy(sys::state const& state, left_t market, right_t cid) {
	return { market, cid };
}

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_aggregated_demand_history(sys::state const& state, left_t market, right_t cid) {
	return state.world.market_get_aggregated_demand_history(market, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_price<left_t, right_t> market_get_aggregated_demand_history(sys::state const& state, left_t market, right_t cid) {
	return { market, cid };
}

template<bool eager, typename left_t, typename right_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_price_confidence(sys::state const& state, left_t market, right_t cid) {
	return state.world.market_get_price_confidence(market, cid);
}
template<bool eager, typename left_t, typename right_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_price_confidence<left_t, right_t> market_get_price_confidence(sys::state const& state, left_t market, right_t cid) {
	return { market, cid };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_trade_house_budget_import_scale(sys::state const& state, left_t market) {
	return state.world.market_get_trade_house_budget_import_scale(market);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::trade_house_budget_import_scale<left_t> market_get_trade_house_budget_import_scale(sys::state const& state, left_t market) {
	return { market };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_naval_transportation_demand_satisfaction(sys::state const& state, left_t market) {
	return state.world.market_get_naval_transportation_demand_satisfaction(market);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::naval_transportation_demand_satisfaction<left_t> market_get_naval_transportation_demand_satisfaction(sys::state const& state, left_t market) {
	return { market };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto market_get_land_transportation_demand_satisfaction(sys::state const& state, left_t market) {
	return state.world.market_get_land_transportation_demand_satisfaction(market);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::land_transportation_demand_satisfaction<left_t> market_get_land_transportation_demand_satisfaction(sys::state const& state, left_t market) {
	return { market };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto trade_route_get_origin(sys::state const& state, left_t route) {
	return state.world.trade_route_get_origin(route);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_trade_origin<left_t> trade_route_get_origin(sys::state const& state, left_t route) {
	return { route };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto trade_route_get_target(sys::state const& state, left_t route) {
	return state.world.trade_route_get_target(route);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_trade_target<left_t> trade_route_get_target(sys::state const& state, left_t route) {
	return { route };
}

template<bool eager, typename left_t, std::enable_if_t<eager, bool> = true>
DCON_RELEASE_INLINE auto trade_route_get_owner(sys::state const& state, left_t route) {
	return state.world.trade_route_get_owner(route);
}
template<bool eager, typename left_t, std::enable_if_t<!eager, bool> = true>
constexpr term::fetch_trade_owner<left_t> trade_route_get_owner(sys::state const& state, left_t route) {
	return { route };
}

}
#undef DCON_RELEASE_INLINE
#undef DCON_RESTRICT


namespace economy {

/*
New model of trade update:
Assume that there is a segment [0, 1] of traders operating this route.
Trader 0 gets 0x of the sales.
Trader 1 gets 2x of the sales.
Traders inbetween earn a linear combination of trader 0 and trader 1 earning.
If a certain trader earns less than SPEND, they change their volume (1 - a) times - q dt.
If a certain trader earns more than SPEND, they change their volume (1 + a) times + q dt.
We want to find a proportion of traders which would earn more than they spend and update the total traded amount accordingly.

2 * EARN * x + y = 2 * EARN;
if y = SPEND
2 * EARN * x + SPEND = 2 * EARN
x = (2 * EARN - SPEND) / (2 * EARN) --- the ratio of traders with profitable trade
2 * (x - 0.5) = (EARN - SPEND) / (EARN)  --- signed amount of traders increasing/reducing their volume
*/

template<bool eager, typename scalar, typename ROUTE_ID, typename COMMODITY_ID, typename VALUE_IN, typename BOOL_IN1, typename BOOL_IN2>
inline auto trade_route_compute_next_volume(
	sys::state const& state,
	BOOL_IN1 reset_route,
	BOOL_IN2 is_sea_route,
	ROUTE_ID trade_route,
	COMMODITY_ID c,

	VALUE_IN export_tariff,
	VALUE_IN import_tariff,
	VALUE_IN transport_cost,
	VALUE_IN budget,
	VALUE_IN trade_good_loss_mult,
	VALUE_IN merchant_cut
) {
	auto origin = adaptive_ve::trade_route_get_origin<eager, ROUTE_ID>(state, trade_route);
	auto target = adaptive_ve::trade_route_get_target<eager, ROUTE_ID>(state, trade_route);
	auto owner = adaptive_ve::trade_route_get_owner<eager, ROUTE_ID>(state, trade_route);
	auto reset_route_commodity = reset_route;
	auto current_volume = adaptive_ve::trade_route_get_volume<eager>(state, trade_route, c);
	auto confidence_export = adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.5f), adaptive_ve::market_get_price_confidence<eager>(state, origin, c));
	auto confidence_import = adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.5f), adaptive_ve::market_get_price_confidence<eager>(state, target, c));
	auto price_origin = adaptive_ve::market_get_price<eager>(state, origin, c);
	auto price_target = adaptive_ve::market_get_price<eager>(state, target, c);

	auto expected_to_buy = adaptive_ve::market_get_expected_probability_to_buy<eager>(state, origin, c);
	auto expected_to_sell = adaptive_ve::market_get_expected_probability_to_sell<eager>(state, target, c);
	auto historical_demand = adaptive_ve::market_get_aggregated_demand_history<eager>(state, target, c);
	auto import_budget_scale = adaptive_ve::market_get_trade_house_budget_import_scale<eager>(state, target);
	auto naval_transport_availability = adaptive_ve::market_get_naval_transportation_demand_satisfaction<eager>(state, owner);
	auto land_transport_availability = adaptive_ve::market_get_land_transportation_demand_satisfaction<eager>(state, owner);
	auto transport_availability = adaptive_ve::select<BOOL_IN2, scalar>(is_sea_route, naval_transport_availability, land_transport_availability);


	auto price_export = price_origin / confidence_export;
	auto price_import = price_target * confidence_import;

	auto earn_per_unit = price_import * trade_good_loss_mult;
	auto pay_per_unit = price_export * (1.f + export_tariff + merchant_cut) + price_import * import_tariff + transport_cost;
	auto owner_is_exporter = owner == origin;
	auto price_sold = adaptive_ve::select<decltype(owner_is_exporter), scalar>(owner_is_exporter, price_import, price_export);
	auto budget_scale = adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.f), 1.f - import_budget_scale);
	auto budget_factor = adaptive_ve::min(adaptive_ve::constant_value<scalar>(1.f), adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.f), budget / (1.f + import_budget_scale * 5.f) / 100.f * budget_scale / (economy::price_properties::commodity::min + price_sold)));

	auto hard_limit = transport_availability;
	auto soft_limit = expected_to_sell * expected_to_buy;
	auto change_multiplier =
		adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.f), (hard_limit - 0.9f) / 0.1f)
		* adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.f), (soft_limit - 0.8f) / 0.2f)
		* budget_factor * budget_factor;
	auto decay = adaptive_ve::max(adaptive_ve::constant_value<scalar>(0.999f), adaptive_ve::min(adaptive_ve::constant_value<scalar>(1.f), 0.5f + hard_limit * soft_limit * budget_scale));
	auto diff = 2.f * (earn_per_unit - pay_per_unit) / (earn_per_unit + economy::price_properties::commodity::min);
	auto change = (current_volume * 0.001f + 0.1f) * diff;
	auto change_is_negative = change <= adaptive_ve::constant_value<scalar>(0.f);
	auto change_slow_down_expansion = adaptive_ve::select<decltype(change_is_negative), scalar>(change_is_negative, change, change * change_multiplier);
	auto next = adaptive_ve::select<BOOL_IN1, scalar>(
		reset_route_commodity,
		adaptive_ve::constant_value<scalar>(0.f),
		adaptive_ve::max(
			adaptive_ve::constant_value<scalar>(0.f),
			current_volume * decay + change_slow_down_expansion + adaptive_ve::min(
				adaptive_ve::constant_value<scalar>(0.f),
				historical_demand - 0.0001f
			)
		)
	);

	return next;
}

}
