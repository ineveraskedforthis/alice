#include "economy.hpp"
#include "economy_government.hpp"
#include "economy_stats.hpp"
#include "economy_production.hpp"
#include "economy_trade_routes.hpp"
#include "construction.hpp"
#include "demographics.hpp"
#include "demographics_templates.hpp"
#include "dcon_generated.hpp"
#include "ai_economy.hpp"
#include "system_state.hpp"
#include "prng.hpp"
#include "province_templates.hpp"
#include "triggers.hpp"
#include "advanced_province_buildings.hpp"
#include "price.hpp"
#include "economy_pops.hpp"
#include "commodities.hpp"
#include "province.hpp"
#include "money.hpp"
#include "economy_constants.hpp"
#include "economy_factory_view.hpp"
#include "events.hpp"
#include "commands.hpp"
#include <vector>
#include <algorithm>
#include "economy_pops_constants.hpp"
#include "economy_templates.hpp"
#include "economy_templates_pure.hpp"
#include "military_templates.hpp"
#include "nations_templates.hpp"
#include "national_budget.hpp"
#include "economy_trade_routes_templates.hpp"
namespace economy {

inline constexpr float free_efficiency = 0.3f;

enum class economy_update_stage {
	stage_1,
	stage_2,
	stage_3,
	stage_4,
	stage_5,
	stage_6,
	stage_7,
	stage_8
};

template<typename tag_type, typename F>
RELEASE_INLINE void execute_serial_range(uint32_t start, uint32_t count, F&& functor) {
	for(uint32_t i = 0; i < (count & ~uint32_t(3)); i += 4) {
		functor(ve::contiguous_tags<tag_type>((start + i) * ve::vector_size));
		functor(ve::contiguous_tags<tag_type>((start + i + 1) * ve::vector_size));
		functor(ve::contiguous_tags<tag_type>((start + i + 2) * ve::vector_size));
		functor(ve::contiguous_tags<tag_type>((start + i + 3) * ve::vector_size));
	}
	for(uint32_t i = (count & ~uint32_t(3)); i < count; i += 1) {
		functor(ve::contiguous_tags<tag_type>((start + i) * ve::vector_size));
	}
}
template<typename tag_type, uint32_t batch, uint32_t batch_count, typename F>
RELEASE_INLINE void execute_batch(uint32_t count, F&& f) {
	auto vectors = count / ve::vector_size;
	auto vectors_in_batch = vectors / batch_count;
	if constexpr(batch + 1 == batch_count) {
		vectors_in_batch += vectors - (vectors / batch_count) * batch_count;
	}

	execute_serial_range<tag_type>((vectors / batch_count) * batch, vectors_in_batch, std::forward<F>(f));

	if constexpr(batch + 1 == batch_count) {
		const uint32_t full_units = count & ~uint32_t(ve::vector_size - 1);
		const uint32_t remainder = count & uint32_t(ve::vector_size - 1);
		if(remainder != 0) {
			f(ve::partial_contiguous_tags<tag_type>(full_units, remainder));
		}
	}
}

template<uint32_t batch, uint32_t batch_count, typename F>
RELEASE_INLINE void for_each_controlled_market_parallel_over_nation_batch(sys::state& state, F const& func) {
	auto total = state.world.nation_size();
	auto per_batch = total / batch_count;
	auto batch_start = per_batch * batch;
	auto in_batch_count = per_batch;
	if constexpr(batch + 1 == batch_count) {
		in_batch_count += total - per_batch * batch_count;
	}
	for(uint32_t raw = (uint32_t)(batch_start); raw < (uint32_t)(batch_start + in_batch_count); ++raw) {
		dcon::nation_id nid{ (dcon::nation_id::value_base_t)(raw) };
		state.world.nation_for_each_state_control(nid, [&](dcon::state_control_id soid) {
			auto sid = state.world.state_control_get_state(soid);
			auto market = state.world.state_instance_get_market_from_local_market(sid);
			func(nid, sid, market);
		});
	}
}

template<uint32_t batch, uint32_t batch_count, typename F>
RELEASE_INLINE void for_each_index_batched(uint32_t total, F const& func) {
	auto per_batch = total / batch_count;
	auto batch_start = per_batch * batch;
	auto in_batch_count = per_batch;
	if constexpr(batch + 1 == batch_count) {
		in_batch_count += total - per_batch * batch_count;
	}
	for(uint32_t raw = (uint32_t)(batch_start); raw < (uint32_t)(batch_start + in_batch_count); ++raw) {
		func(raw);
	}
}

template<uint32_t batch, uint32_t batch_count, typename F>
RELEASE_INLINE void for_each_market_province_parallel_over_market_batched(sys::state const& state, F const& func) {
	for_each_index_batched<batch, batch_count>(state.world.market_size(), [&](auto raw_mid) {
		dcon::market_id mid{ (dcon::market_id::value_base_t)(raw_mid) };
		auto sid = state.world.market_get_zone_from_local_market(mid);
		province::for_each_province_in_state_instance(state, sid, [&](auto pid) {
			func(mid, sid, pid);
		});
	});
}

template<uint32_t batch, uint32_t batch_count, typename F>
RELEASE_INLINE void for_each_nation_owned_province_parallel_over_nation_batched(sys::state& state, F const& func) {
	for_each_index_batched<batch, batch_count>(state.world.nation_size(), [&](auto raw) {
		dcon::nation_id nid{ (dcon::nation_id::value_base_t)(raw) };
		state.world.nation_for_each_province_ownership(nid, [&](dcon::province_ownership_id poid) {
			func(nid, state.world.province_ownership_get_province(poid));
		});
	});
}

dcon::province_id construction_get_location(const sys::state& state, dcon::province_land_construction_id con) {
		return state.world.pop_get_province_from_pop_location( state.world.province_land_construction_get_pop(con));
}
dcon::province_id construction_get_location(const sys::state& state, dcon::province_naval_construction_id con) {
	return state.world.province_naval_construction_get_province(con);
}
dcon::province_id construction_get_location(const sys::state& state, dcon::province_building_construction_id con) {
	return state.world.province_building_construction_get_province(con);
}
dcon::province_id construction_get_location(const sys::state& state, dcon::factory_construction_id con) {
	return state.world.factory_construction_get_province(con);
}

dcon::unit_type_id construction_get_type(const sys::state& state, dcon::province_land_construction_id con) {
	return state.world.province_land_construction_get_type(con);
}
dcon::unit_type_id construction_get_type(const sys::state& state, dcon::province_naval_construction_id con) {
	return state.world.province_naval_construction_get_type(con);
}
uint8_t construction_get_type(const sys::state& state, dcon::province_building_construction_id con) {
	return state.world.province_building_construction_get_type(con);
}
dcon::factory_type_id construction_get_type(const sys::state& state, dcon::factory_construction_id con) {
	return state.world.factory_construction_get_type(con);
}


dcon::nation_id construction_get_controller(const sys::state& state, dcon::province_land_construction_id con) {
	return state.world.province_land_construction_get_nation(con);
}
dcon::nation_id construction_get_controller(const sys::state& state, dcon::province_naval_construction_id con) {
	return state.world.province_naval_construction_get_nation(con);
}

dcon::nation_id construction_get_controller(const sys::state& state, dcon::province_building_construction_id con) {
	return state.world.province_building_construction_get_nation(con);
}
dcon::nation_id construction_get_controller(const sys::state& state, dcon::factory_construction_id con) {
	return state.world.factory_construction_get_nation(con);
}

dcon::commodity_id unit_commodity_get_base_commodity(const sys::state& state, dcon::unit_supply_commodity_id com_id) {
	return state.world.unit_supply_commodity_get_base_commodity(com_id);
}
dcon::commodity_id unit_commodity_get_base_commodity(const sys::state& state, dcon::unit_build_commodity_id com_id) {
	return state.world.unit_build_commodity_get_base_commodity(com_id);
}
dcon::commodity_id unit_commodity_get_base_commodity(const sys::state& state, dcon::unit_supply_and_build_commodity_id com_id) {
	return state.world.unit_supply_and_build_commodity_get_base_commodity(com_id);
}



// assume that max speed is measured in ~knots which are roughly 2. of km/h
constexpr float vic2_knots_to_km_per_day = 24.f * 2.f;
constexpr float vic2_hull_to_tonn = 20.f;
// measured and provided via port capacity
constexpr float vic2_ship_crew_size = 150.f;
constexpr float cargo_ship_maintenance_reduction = 0.01f;

float pop_min_wage_factor(sys::state& state, dcon::nation_id n) {
	return state.world.nation_get_modifier_values(n, sys::national_mod_offsets::minimum_wage);
}
template<typename T>
ve::fp_vector ve_pop_min_wage_factor(sys::state& state, T n) {
	return state.world.nation_get_modifier_values(n, sys::national_mod_offsets::minimum_wage);
}
template<typename T>
ve::fp_vector ve_farmer_min_wage(sys::state& state, T markets, ve::fp_vector min_wage_factor) {
	auto life = state.world.market_get_life_needs_costs(markets, state.culture_definitions.farmers);
	auto everyday = state.world.market_get_everyday_needs_costs(markets, state.culture_definitions.farmers);
	return min_wage_factor * (life + everyday) * 1.1f;
}

template<typename T>
ve::fp_vector ve_laborer_min_wage(sys::state& state, T markets, ve::fp_vector min_wage_factor) {
	auto life = state.world.market_get_life_needs_costs(markets, state.culture_definitions.laborers);
	auto everyday = state.world.market_get_everyday_needs_costs(markets, state.culture_definitions.laborers);
	return min_wage_factor * (life + everyday) * 1.1f;
}
float farmer_min_wage(sys::state& state, dcon::market_id m, float min_wage_factor) {
	auto life = state.world.market_get_life_needs_costs(m, state.culture_definitions.farmers);
	auto everyday = state.world.market_get_everyday_needs_costs(m, state.culture_definitions.farmers);
	return min_wage_factor * (life + everyday) * 1.1f;
}
float laborer_min_wage(sys::state& state, dcon::market_id m, float min_wage_factor) {
	auto life = state.world.market_get_life_needs_costs(m, state.culture_definitions.laborers);
	auto everyday = state.world.market_get_everyday_needs_costs(m, state.culture_definitions.laborers);
	return min_wage_factor * (life + everyday) * 1.1f;
}

constexpr inline float expected_savings_per_capita = 0.0001f;

void sanity_check([[maybe_unused]] sys::state& state) {
/*
	state.world.for_each_nation([&](auto nid) {
		assert(std::isfinite(state.world.nation_get_stockpiles(nid, money)));
	});
	state.world.for_each_commodity([&](auto cid) {
		state.world.for_each_market([&](auto mid) {
			assert(state.world.market_get_price(mid, cid) > 0.f);
		});
	});
*/
#ifdef NDEBUG
#else
	/*
	ve::fp_vector total{};
	state.world.execute_serial_over_pop([&](auto ids) {
		total = total + state.world.pop_get_savings(ids);
	});
	assert(total.reduce() > 0.f);
	*/
	//assert(state.inflation > 0.1f && state.inflation < 10.f);
#endif
}

bool can_take_loans(sys::state& state, dcon::nation_id n) {
	if(!state.world.nation_get_is_player_controlled(n) || !state.world.nation_get_is_debt_spending(n))
		return false;

	/*
	A country cannot borrow if it is less than define:BANKRUPTCY_EXTERNAL_LOAN_YEARS since their last bankruptcy.
	*/
	auto last_br = state.world.nation_get_bankrupt_until(n);
	if(last_br && state.current_date < last_br)
		return false;

	return true;
}

float interest_payment(sys::state& state, dcon::nation_id n) {
	/*
	Every day, a nation must pay its creditors. It must pay national-modifier-to-loan-interest x debt-amount x interest-to-debt-holder-rate / 30
	When a nation takes a loan, the interest-to-debt-holder-rate is set at nation-taking-the-loan-technology-loan-interest-modifier + define:LOAN_BASE_INTEREST, with a minimum of 0.01.
	*/
	auto debt = state.world.nation_get_local_loan(n);
	return debt * std::max(0.01f, (state.world.nation_get_modifier_values(n, sys::national_mod_offsets::loan_interest) + 1.0f) * state.defines.loan_base_interest) / 30.0f;
}
float max_loan(sys::state& state, dcon::nation_id n) {
	/*
	There is an income cap to how much may be borrowed, namely: define:MAX_LOAN_CAP_FROM_BANKS x (national-modifier-to-max-loan-amount + 1) x national-tax-base.
	*/
	auto mod = (state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_loan_modifier) + 1.0f);
	auto total_tax_base = state.world.nation_get_total_rich_income(n) + state.world.nation_get_total_middle_income(n) + state.world.nation_get_total_poor_income(n);
	return std::max(0.0f, (total_tax_base + state.world.nation_get_national_bank(n)) * mod);
}

int32_t most_recent_price_record_index(sys::state& state, int offset_days) {
	auto offset_positive = (offset_days % (price_history_length << 4) + (price_history_length << 4));
	return ((state.current_date.value + offset_positive) >> 4) % price_history_length;
}
int32_t previous_price_record_index(sys::state& state) {
	return ((state.current_date.value >> 4) + price_history_length - 1) % price_history_length;
}

int32_t most_recent_gdp_record_index(sys::state& state, int offset_days) {
	auto date = state.current_date.to_ymd(state.start_date);
	auto quarter_offset = (offset_days / 90);
	quarter_offset = (quarter_offset % gdp_history_length + gdp_history_length);
	return (date.year * 4 + date.month / 3 + quarter_offset) % gdp_history_length;
}
int32_t previous_gdp_record_index(sys::state& state) {
	auto date = state.current_date.to_ymd(state.start_date);
	return ((date.year * 4 + date.month / 3) + gdp_history_length - 1) % gdp_history_length;
}


struct spending_cost {
	float administration;
	float construction;
	float other;
	float total;
};

spending_cost full_spending_cost(sys::state& state, dcon::nation_id n, float budget);
// Sets army demand to be purchased to govt stockpiles.Will set the demand in states which is deemed the most likely to be able to satisfy the demand locally
void populate_army_consumption(sys::state& state);
// Sets navy demand to be purchased to govt stockpiles.Will set the demand in states which is deemed the most likely to be able to satisfy the demand locally
void populate_navy_consumption(sys::state& state);

// Returns factory types for which commodity is an output good
std::vector<dcon::factory_type_id> commodity_get_factory_types_as_output(sys::state const& state, dcon::commodity_id output_good) {
	std::vector<dcon::factory_type_id> types;
	for(auto t : state.world.in_factory_type) {
		if(t.get_output() == output_good) {
			types.push_back(t);
		}
	}
	return types;
}

void initialize_artisan_distribution(sys::state& state) {
	state.world.province_resize_artisan_score(state.world.commodity_size());
	state.world.province_resize_artisan_actual_production(state.world.commodity_size());

	auto artisans = state.culture_definitions.artisans;
	auto keys = demographics::to_key(state, artisans);

	state.world.for_each_commodity([&](auto cid) {
		province::ve_for_each_land_province(state, [&](auto ids) {
			auto local_artisans = state.world.province_get_demographics(ids, keys);
			state.world.province_set_artisan_score(ids, cid, local_artisans * 0.05f);
		});
	});
}

void initialize_needs_weights(sys::state& state, dcon::market_id n) {
	auto zone = state.world.market_get_zone_from_local_market(n);
	auto nation = state.world.state_instance_get_nation_from_state_ownership(zone);
	{
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			state.world.market_set_life_needs_weights(n, c, 0.001f);
		});
	}
	{
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			state.world.market_set_everyday_needs_weights(n, c, 0.001f);
		});
	}
	{
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			state.world.market_set_luxury_needs_weights(n, c, 0.001f);
		});
	}
	state.world.for_each_consumption_category([&](auto weights_id) {
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			auto index = c.index() + weights_id.index() * state.world.commodity_size();
			state.world.market_set_local_consumption_weights(n, index, 0.01f);
		});
	});
}

/*
Need weight allows pops to not promise buying "hopeless" items and avoid generating demand on them
There is base growth to avoid being stuck at 0
It's modified by local availability and price
*/

void rebalance_needs_weights(sys::state& state) {
	static ve::vectorizable_buffer<float, dcon::commodity_id> average_life_need = ve::vectorizable_buffer<float, dcon::commodity_id>(uint32_t(1));
	static ve::vectorizable_buffer<float, dcon::commodity_id> average_everyday_need = ve::vectorizable_buffer<float, dcon::commodity_id>(uint32_t(1));
	static ve::vectorizable_buffer<float, dcon::commodity_id> average_luxury_need = ve::vectorizable_buffer<float, dcon::commodity_id>(uint32_t(1));
	static bool average_need_bool_buffer_ready = false;

	static float life_count = 1.f;
	static float everyday_count = 1.f;
	static float luxury_count = 1.f;

	if(!average_need_bool_buffer_ready) {
		average_need_bool_buffer_ready = true;
		average_life_need = state.world.commodity_make_vectorizable_float_buffer();
		average_everyday_need = state.world.commodity_make_vectorizable_float_buffer();
		average_luxury_need = state.world.commodity_make_vectorizable_float_buffer();

		state.world.for_each_commodity([&](auto c) {
			auto life_average = 0.f;
			auto everyday_average = 0.f;
			auto luxury_average = 0.f;

			state.world.for_each_pop_type([&](auto t) {
				life_average += state.world.pop_type_get_life_needs(t, c);
				everyday_average += state.world.pop_type_get_everyday_needs(t, c);
				luxury_average += state.world.pop_type_get_luxury_needs(t, c);
			});

			life_average /= float(state.world.pop_type_size());
			everyday_average /= float(state.world.pop_type_size());
			luxury_average /= float(state.world.pop_type_size());

			average_life_need.set(c, life_average);
			average_everyday_need.set(c, everyday_average);
			average_luxury_need.set(c, luxury_average);

			life_count += life_average;
			everyday_count += everyday_average;
			luxury_count += luxury_average;
		});
	}

	state.world.execute_parallel_over_market([&](auto market){
		ve::fp_vector local_cost_life { economy::price_properties::commodity::min };
		ve::fp_vector local_cost_everyday{ economy::price_properties::commodity::min };
		ve::fp_vector local_cost_luxury{ economy::price_properties::commodity::min };
		state.world.for_each_commodity([&](auto cid) {
			auto base_life = average_life_need.get(cid);
			auto base_everyday = average_everyday_need.get(cid);
			auto base_luxury = average_luxury_need.get(cid);

			auto price = state.world.market_get_price(market, cid);

			local_cost_life = local_cost_life + base_life * price;
			local_cost_everyday = local_cost_everyday + base_everyday * price;
			local_cost_luxury = local_cost_luxury + base_luxury * price;
		});

		state.world.for_each_commodity([&](dcon::commodity_id c) {
			auto price = state.world.market_get_price(market, c);
			auto available = state.world.market_get_expected_probability_to_buy(market, c);

			auto w_life = state.world.market_get_life_needs_weights(market, c);
			auto w_everyday = state.world.market_get_everyday_needs_weights(market, c);
			auto w_luxury = state.world.market_get_luxury_needs_weights(market, c);

			auto needed_life = average_life_need.get(c);
			auto needed_everyday = average_everyday_need.get(c);
			auto needed_luxury = average_luxury_need.get(c);

			ve::fp_vector dw_life = need_weight_change<ve::fp_vector, ve::fp_vector>(local_cost_life + local_cost_everyday * 0.000'000'1f, price, available, w_life, needed_life, local_cost_life, life_count);
			ve::fp_vector dw_everyday = need_weight_change<ve::fp_vector, ve::fp_vector>(local_cost_life + local_cost_everyday * 0.000'000'1f, price, available, w_everyday, needed_everyday, local_cost_everyday, everyday_count);
			ve::fp_vector dw_luxury = need_weight_change<ve::fp_vector, ve::fp_vector>(local_cost_life + local_cost_everyday * 0.000'000'1f, price, available, w_luxury, needed_luxury, local_cost_luxury, luxury_count);

			w_life = ve::max(ve::fp_vector{0.f}, ve::min(ve::fp_vector{1.f}, w_life + dw_life * 10.f * state.defines.alice_need_drift_speed));
			w_everyday = ve::max(ve::fp_vector{ 0.f }, ve::min(ve::fp_vector{ 1.f }, w_everyday + dw_everyday * 10.f * state.defines.alice_need_drift_speed));
			w_luxury = ve::max(ve::fp_vector{ 0.f }, ve::min(ve::fp_vector{ 1.f }, w_luxury + dw_luxury * 10.f * state.defines.alice_need_drift_speed));

			state.world.market_set_life_needs_weights(market, c, w_life);
			state.world.market_set_everyday_needs_weights(market, c, w_everyday);
			state.world.market_set_luxury_needs_weights(market, c, w_luxury);
		});
	});
}


void convert_commodities_into_ingredients(
	sys::state& state,
	std::vector<float>& buffer_commodities,
	std::vector<float>& buffer_ingredients,
	std::vector<float>& buffer_weights
) {
	state.world.for_each_commodity([&](dcon::commodity_id c) {
		float amount = buffer_commodities[c.index()];

		if(state.world.commodity_get_rgo_amount(c) > 0.f) {
			buffer_ingredients[c.index()] += amount;
		} else {
			//calculate input vectors weights:
			std::vector<float> weights;
			float total_weight = 0.f;
			float non_zero_count = 0.f;

			state.world.for_each_factory_type([&](dcon::factory_type_id t) {
				auto o = state.world.factory_type_get_output(t);
				if(o == c) {
					auto& inputs = state.world.factory_type_get_inputs(t);

					float weight_current = 0;

					for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
						if(inputs.commodity_type[i]) {
							float weight_input = buffer_weights[inputs.commodity_type[i].index()];
							total_weight += weight_input;
							weight_current += weight_input;
						} else {
							break;
						}
					}

					if(weight_current > 0.f)
						non_zero_count++;

					weights.push_back(weight_current);
				}
			});

			if(total_weight == 0) {
				for(size_t i = 0; i < weights.size(); i++) {
					weights[i] = 1.f;
					total_weight++;
				}
			} else {
				float average_weight = total_weight / non_zero_count;
				for(size_t i = 0; i < weights.size(); i++) {
					if(weights[i] == 0.f) {
						weights[i] = average_weight;
						total_weight += average_weight;
					}
				}
			}

			//now we have weights and can use them for transformation of output into ingredients:
			size_t index = 0;

			state.world.for_each_factory_type([&](dcon::factory_type_id t) {
				auto o = state.world.factory_type_get_output(t);
				if(o == c) {
					auto& inputs = state.world.factory_type_get_inputs(t);
					float output_power = state.world.factory_type_get_output_amount(t);

					float weight_current = weights[index] / total_weight;
					index++;

					for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
						if(inputs.commodity_type[i]) {

							buffer_ingredients[inputs.commodity_type[i].index()] += inputs.commodity_amounts[i] * amount / output_power * weight_current;

							float weight_input = buffer_weights[inputs.commodity_type[i].index()];
							total_weight += weight_input;
							weight_current += weight_input;
						} else {
							break;
						}
					}
				}
			});
		}
	});
}

float combined_government_stockpile_demand(float army, float navy, float mil_construction, float stockpile_filling) {
	return army + navy + mil_construction + stockpile_filling;
}

float combined_government_stockpile_demand(sys::state& state, dcon::market_id market, dcon::commodity_id commodity) {
	// The amount of effective demand for for all sources of stockpile purchases. Gets the highest of either army+navy demand, or stockpile targets demand
	return combined_government_stockpile_demand(state.world.market_get_army_demand(market, commodity), state.world.market_get_navy_demand(market, commodity), state.world.market_get_government_construction_demand(market, commodity), state.world.market_get_government_stockpile_demand(market, commodity));
}





void get_closest_available_market_states(sys::state& state, std::vector<dcon::state_instance_id>& out_buffer, dcon::nation_id nation_as, dcon::province_id location_from) {
	state.world.nation_for_each_state_control(nation_as, [&](dcon::state_control_id sc) {
		dcon::state_instance_id state_instance = state.world.state_control_get_state(sc);
		out_buffer.push_back(state_instance);
	});
	std::sort(out_buffer.begin(), out_buffer.end(), [&](auto state_instance_a, auto state_instance_b) {
		auto si_a_capital = state.world.state_instance_get_capital(state_instance_a);
		auto si_b_capital = state.world.state_instance_get_capital(state_instance_b);
		float si_b_direct_dist = province::sorting_distance(state, si_b_capital, location_from);
		float si_a_direct_dist = province::sorting_distance(state, si_b_capital, location_from);
		auto st_a_idx = state_instance_a.index();
		auto si_b_idx = state_instance_b.index();
		return (si_b_direct_dist != si_a_direct_dist ? si_a_direct_dist < si_b_direct_dist : st_a_idx < si_b_idx);
	});
}
void get_closest_available_market_states(sys::state& state, dcon::dcon_vv_fat_id<dcon::state_instance_id> out_buffer, dcon::nation_id nation_as, dcon::province_id location_from) {
	state.world.nation_for_each_state_control(nation_as, [&](dcon::state_control_id sc) {
		dcon::state_instance_id state_instance = state.world.state_control_get_state(sc);
		out_buffer.push_back(state_instance);
	});
	std::sort(out_buffer.begin(), out_buffer.end(), [&](auto state_instance_a, auto state_instance_b) {
		auto si_a_capital = state.world.state_instance_get_capital(state_instance_a);
		auto si_b_capital = state.world.state_instance_get_capital(state_instance_b);
		float si_b_direct_dist = province::sorting_distance(state, si_b_capital, location_from);
		float si_a_direct_dist = province::sorting_distance(state, si_a_capital, location_from);
		auto st_a_idx = state_instance_a.index();
		auto si_b_idx = state_instance_b.index();
		return (si_b_direct_dist != si_a_direct_dist ? si_a_direct_dist < si_b_direct_dist : st_a_idx < si_b_idx);
	});

}


consume_stockpile_result consume_single_government_stockpile(sys::state& state, dcon::nation_id state_controller, dcon::state_instance_id stockpile_state, dcon::commodity_id commodity, float amount) {
	auto stockpile_market = state.world.state_instance_get_market_from_local_market(stockpile_state);
	auto current_stockpile = state.world.market_get_government_stockpile(stockpile_market, commodity);
	float to_consume_amount = std::min(amount, current_stockpile); // We may not consume more than exists in the stockpile
	//float divisor = (clamped_req_amount == 0 ? 1.0f : clamped_req_amount);
	float satisfaction = (amount == 0 ? 1.0f : to_consume_amount / amount); // if amount is 0 then full satisfaction, otherwise clamp value to avoid div by zero if stockpile empty
	// Consume it
	economy::subtract_government_stockpile(state, state_controller, stockpile_market, commodity, to_consume_amount);

	return consume_stockpile_result {to_consume_amount, satisfaction};
}



float consume_from_government_stockpiles(sys::state& state, economy::commodity_set& to_consume, std::span<const dcon::state_instance_id> stockpile_states, dcon::province_id location_from, dcon::nation_id nation_as) {
	float required_commodities = 0;
	float consumed_commodities = 0;
	for(uint32_t i = 0; i < to_consume.set_size; i++) {
		if(to_consume.commodity_type[i]) {
			float& required_amount = to_consume.commodity_amounts[i];
			required_commodities += required_amount;
			dcon::commodity_id required_commodity = to_consume.commodity_type[i];
			// Iterate over each stockpile in the preferred order, and consume from them until the required supply is satisfied, or there are no more stockpiles
			for(auto stockpile_state : stockpile_states) {
			// Find out what needs to be consumed from the stockpile, and set the satisfaction. We modify the commodity set over time.
			auto result = consume_single_government_stockpile(state, nation_as, stockpile_state, required_commodity, required_amount);
			required_amount -= result.amount_consumed;
			consumed_commodities += result.amount_consumed;
				// If no more goods are required we can exit early
				if(required_amount <= 0.0f) {
					break;
				}
			}
		} else {
			break;
		}
	}
	// Guard against DBZ if no commodities are required. Must clamp end result to a max of 1.0f as rounding can make it get slightly above it
	float satisfaction = (required_commodities == 0 ? 1.0f : std::min(consumed_commodities / required_commodities, 1.0f));
	return satisfaction;

}


void update_government_stockpile_market_demand_weights(sys::state& state) {

	// This buffer is free during this time
	auto weights_buffer_get = [&](auto nation, dcon::commodity_id com_id) {
		return state.world.nation_get_commodity_float_buffer_1(nation, com_id);
	};
	auto weights_buffer_set = [&](auto nation, dcon::commodity_id com_id, auto val) {
		state.world.nation_set_commodity_float_buffer_1(nation, com_id, val);
	};

	// First, set the "raw" weight per market
	state.world.execute_parallel_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto controller_valid = (controllers != dcon::nation_id{ } && nations::exists(state, controllers));
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
			auto sat_weight = state.world.market_get_expected_probability_to_buy(markets, commodity) + 0.001f;
			auto price_weight = 1.0f / (state.world.market_get_price(markets, commodity) + 0.001f);
			auto raw_weight = state.world.market_get_supply(markets, commodity) * price_weight * sat_weight;
			state.world.market_set_government_stockpile_demand_weights(markets, commodity, ve::select(controller_valid, raw_weight, 0.0f));

		});
	});

	// Then, accumulate them into a per-nation buffer
	province::for_each_controlled_market_parallel_over_nation(state, [&](dcon::nation_id controller, dcon::state_instance_id sid, dcon::market_id market) {
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
			float amount = state.world.market_get_government_stockpile_demand_weights(market, commodity);
			weights_buffer_set(controller, commodity, weights_buffer_get(controller, commodity) + amount);
		});
	});

	// Lastly, set percentage weights, so that a nations' controlled markets' weights all add up to 1.0f.
	state.world.execute_parallel_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto controller_valid = (controllers != dcon::nation_id{ } && nations::exists(state, controllers));
		ve::fp_vector state_count = ve::apply([&](dcon::nation_id nation) {
			return float(state.world.nation_get_state_control(nation).end() - state.world.nation_get_state_control(nation).begin());
		}, controllers);
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
			auto total_nation_weights = weights_buffer_get(controllers, commodity);
			auto controller_has_weight = (controller_valid && total_nation_weights > 0.0f);
			auto raw_weight = state.world.market_get_government_stockpile_demand_weights(markets, commodity);
			auto equally_split_demand = 1.0f / state_count;
			auto percentage_weight = ve::select(controller_has_weight, raw_weight / total_nation_weights, equally_split_demand);
			percentage_weight = ve::select(controller_valid, percentage_weight, 0.0f);
			state.world.market_set_government_stockpile_demand_weights(markets, commodity, percentage_weight);

		});
	});


}


template<price_estimation price_est, concepts::any_commodity_type com_type>
float estimate_stockpile_total_purchase_cost(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, com_type>& goods) {
	auto getter = [&](com_type com_id) -> float {
		return goods.get(com_id);
	};
	return estimate_stockpile_total_purchase_cost<price_est>(state, for_nation, getter);
}

template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_supply_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_supply_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_build_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_supply_and_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_supply_and_build_commodity_id>& goods);

template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_supply_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_supply_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_build_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_supply_and_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const ve::vectorizable_buffer<float, dcon::unit_supply_and_build_commodity_id>& goods);


template<price_estimation price_est, concepts::any_commodity_type com_type>
float estimate_stockpile_total_purchase_cost(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, com_type>& goods) {
	auto getter = [&](com_type com_id) -> float {
		return goods[com_id];
	};
	return estimate_stockpile_total_purchase_cost<price_est>(state, for_nation, getter);
}

template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_supply_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_supply_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_build_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::theoretical_max, dcon::unit_supply_and_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_supply_and_build_commodity_id>& goods);

template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_supply_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_supply_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_build_commodity_id>& goods);
template float estimate_stockpile_total_purchase_cost <price_estimation::capped_by_availability, dcon::unit_supply_and_build_commodity_id>
(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, dcon::unit_supply_and_build_commodity_id>& goods);


template<price_estimation price_est, concepts::any_commodity_type com_type>
tagged_vector<float, com_type> estimate_stockpile_purchase_cost_by_commodity(const sys::state& state, dcon::nation_id for_nation, const tagged_vector<float, com_type>& goods) {

	auto for_each_func = [&]<typename F>(F && func) {
		if constexpr(std::is_same_v<com_type, dcon::unit_supply_commodity_id>) {
			state.world.for_each_unit_supply_commodity(func);
		} else if constexpr(std::is_same_v<com_type, dcon::unit_build_commodity_id>) {
			state.world.for_each_unit_build_commodity(func);
		} else if constexpr(std::is_same_v<com_type, dcon::unit_supply_and_build_commodity_id>) {
			state.world.for_each_unit_supply_and_build_commodity(func);
		} else if constexpr(std::is_same_v<com_type, dcon::commodity_id>) {
			economy::for_each_commodity_no_money(state, func);
		} else {
			static_assert(false, "Unsupported functor signature");
		}
	};


	tagged_vector<float, com_type> prices(goods.size());
	for_each_func([&](auto com_id) {
		dcon::commodity_id base_com_id = [&]() {
			if constexpr(std::is_same_v<decltype(com_id), dcon::commodity_id>) {
				return com_id; // We already have the base commodity
			} else {
				return unit_commodity_get_base_commodity(state, com_id);
			}
		}();
		assert(base_com_id);
		state.world.nation_for_each_state_control(for_nation, [&](dcon::state_control_id sc) {
			auto state_inst = state.world.state_control_get_state(sc);
			auto market = state.world.state_instance_get_market_from_local_market(state_inst);
			float goods_desired = goods[com_id];
			prices[com_id] += get_estimated_state_stockpile_purchase_price<price_est>(state, market, base_com_id, goods_desired);
		});
	});
	return prices;
}


float estimate_government_stockpile_filling_spending_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto fat_nation = fatten(state.world, nation);
	auto expected_demand = state.world.commodity_make_vectorizable_float_buffer();

	// Slow down demand from reaching stockpile targets just a bit, to keep things stable
	economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
		float desired_amount = government_stockpile_desired_commodity_amount(state, nation, com_id);
		float target = state.world.nation_get_stockpile_targets(nation, com_id);
		float demand = desired_amount * stockpile_targets_demand_mult;
		expected_demand.set(com_id, demand);
	});

	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, expected_demand);
	// Scale with how much we think we can afford, or attempt to buy everything if the budget allows
	auto can_afford_mult = (total_expected_price == 0.0f ? 1.0f : std::min(budget / total_expected_price, 1.0f));
	return total_expected_price * can_afford_mult;
}

tagged_vector<float, dcon::commodity_id> estimate_government_stockpile_filling_spending_by_commodity_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto fat_nation = fatten(state.world, nation);
	auto expected_demand = tagged_vector<float, dcon::commodity_id>(state.world.commodity_size());

	// Slow down demand from reaching stockpile targets just a bit, to keep things stable
	economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
		float desired_amount = government_stockpile_desired_commodity_amount(state, nation, com_id);
		float target = state.world.nation_get_stockpile_targets(nation, com_id);
		float demand = desired_amount * stockpile_targets_demand_mult;
		expected_demand[com_id] = demand;
	});
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, expected_demand);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::theoretical_max>(state, nation, expected_demand);
	// Scale with how much we think we can afford, or attempt to buy everything if the budget allows
	auto can_afford_mult = (total_expected_price == 0.0f ? 1.0f : std::min(budget / total_expected_price, 1.0f));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
tagged_vector<float, dcon::commodity_id> estimate_government_stockpile_filling_spending_by_commodity_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto fat_nation = fatten(state.world, nation);
	auto expected_demand = tagged_vector<float, dcon::commodity_id>(state.world.commodity_size());

	// Slow down demand from reaching stockpile targets just a bit, to keep things stable
	economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
		float desired_amount = government_stockpile_desired_commodity_amount(state, nation, com_id);
		float target = state.world.nation_get_stockpile_targets(nation, com_id);
		float demand = desired_amount * stockpile_targets_demand_mult;
		expected_demand[com_id] = demand;
	});
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, expected_demand);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, expected_demand);
	// Scale with how much we think we can afford, or attempt to buy everything if the budget allows
	auto can_afford_mult = (total_expected_price == 0.0f ? 1.0f : std::min(budget / total_expected_price, 1.0f));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
float estimate_government_stockpile_filling_spending_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto fat_nation = fatten(state.world, nation);
	auto expected_demand = tagged_vector<float, dcon::commodity_id>(state.world.commodity_size());

	// Slow down demand from reaching stockpile targets just a bit, to keep things stable
	economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
		float desired_amount = government_stockpile_desired_commodity_amount(state, nation, com_id);
		float target = state.world.nation_get_stockpile_targets(nation, com_id);
		float demand = desired_amount * stockpile_targets_demand_mult;
		expected_demand[com_id] = demand;
	});
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, expected_demand);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, expected_demand);
	// Scale with how much we think we can afford, or attempt to buy everything if the budget allows
	auto can_afford_mult = (total_expected_price == 0.0f ? 1.0f : std::min(budget / total_expected_price, 1.0f));
	float total = 0.f;
	for(auto& price : total_expected_prices) {
		total += price * can_afford_mult;
	}
	return total;
}


tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_nation_army_and_navy_consumption(const sys::state& state, dcon::nation_id nation) {
	tagged_vector<float, dcon::unit_supply_and_build_commodity_id> consumption(state.world.unit_supply_and_build_commodity_size());
	auto accumulate_func = [&](dcon::commodity_id com_id, float amount) {
		auto sup_build_com_id = state.world.commodity_get_unit_supply_and_build_commodity(com_id);
		assert(sup_build_com_id);
		consumption[sup_build_com_id] += amount;
	};
	for(auto a : state.world.nation_get_army_control(nation)) {
		for(auto r : a.get_army().get_army_membership()) {
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, r.get_regiment().id, accumulate_func);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, r.get_regiment().id, accumulate_func);
		}
	}
	for(auto a : state.world.nation_get_navy_control(nation)) {
		for(auto r : a.get_navy().get_navy_membership()) {
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, r.get_ship().id, accumulate_func);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, r.get_ship().id, accumulate_func);
		}
	}
	return consumption;
}

tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_nation_army_consumption(const sys::state& state, dcon::nation_id nation) {
	tagged_vector<float, dcon::unit_supply_and_build_commodity_id> consumption(state.world.unit_supply_and_build_commodity_size());
	auto accumulate_func = [&](dcon::commodity_id com_id, float amount) {
		auto sup_build_com_id = state.world.commodity_get_unit_supply_and_build_commodity(com_id);
		assert(sup_build_com_id);
		consumption[sup_build_com_id] += amount;
	};
	for(auto a : state.world.nation_get_army_control(nation)) {
		for(auto r : a.get_army().get_army_membership()) {
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, r.get_regiment().id, accumulate_func);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, r.get_regiment().id, accumulate_func);
		}
	}
	return consumption;
}

tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_nation_navy_consumption(const sys::state& state, dcon::nation_id nation) {
	tagged_vector<float, dcon::unit_supply_and_build_commodity_id> consumption(state.world.unit_supply_and_build_commodity_size());
	auto accumulate_func = [&](dcon::commodity_id com_id, float amount) {
		auto sup_build_com_id = state.world.commodity_get_unit_supply_and_build_commodity(com_id);
		assert(sup_build_com_id);
		consumption[sup_build_com_id] += amount;
	};
	for(auto a : state.world.nation_get_navy_control(nation)) {
		for(auto r : a.get_navy().get_navy_membership()) {
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, r.get_ship().id, accumulate_func);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, r.get_ship().id, accumulate_func);
		}
	}
	return consumption;
}
float estimate_navy_stockpile_spending_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_navy_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_navy_required_spend : std::min(budget / total_expected_price, max_navy_required_spend));
	return total_expected_price * can_afford_mult;
}
tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_navy_stockpile_spending_by_commodity_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_navy_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::theoretical_max>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_navy_required_spend : std::min(budget / total_expected_price, max_navy_required_spend));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_navy_stockpile_spending_by_commodity_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_navy_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_navy_required_spend : std::min(budget / total_expected_price, max_navy_required_spend));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
float estimate_navy_stockpile_spending_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_navy_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_navy_required_spend : std::min(budget / total_expected_price, max_navy_required_spend));
	float total = 0.f;
	for(auto& price : total_expected_prices) {
		total += price * can_afford_mult;
	}
	return total;
}

float estimate_army_stockpile_spending_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_army_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_army_required_spend : std::min(budget / total_expected_price, max_army_required_spend));
	return total_expected_price * can_afford_mult;
}
tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_army_stockpile_spending_by_commodity_before_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_army_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::theoretical_max>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_army_required_spend : std::min(budget / total_expected_price, max_army_required_spend));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
tagged_vector<float, dcon::unit_supply_and_build_commodity_id> estimate_army_stockpile_spending_by_commodity_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_army_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_army_required_spend : std::min(budget / total_expected_price, max_army_required_spend));
	for(auto& price : total_expected_prices) {
		price = price * can_afford_mult;
	}
	return total_expected_prices;
}
float estimate_army_stockpile_spending_after_refund(const sys::state& state, dcon::nation_id nation, float budget) {
	auto goods_consumption = estimate_nation_army_consumption(state, nation);
	// We are using both here since the id's we are iterating over is the union of BOTH supply and reinforcement(build) goods
	float total_expected_price = estimate_stockpile_total_purchase_cost<price_estimation::theoretical_max>(state, nation, goods_consumption);
	auto total_expected_prices = estimate_stockpile_purchase_cost_by_commodity<price_estimation::capped_by_availability>(state, nation, goods_consumption);
	float can_afford_mult = (total_expected_price == 0 ? max_army_required_spend : std::min(budget / total_expected_price, max_army_required_spend));
	float total = 0.f;
	for(auto& price : total_expected_prices) {
		total += price * can_afford_mult;
	}
	return total;
}


total_stockpile_spendings estimate_total_stockpile_spendings_after_refund(const sys::state& state, dcon::nation_id nation_as, float construction_budget, float stockpile_filling_budget, float army_supplies_budget, float navy_supplies_budget) {
	total_stockpile_spendings result{
		.army_stockpile_spendings = estimate_army_stockpile_spending_after_refund(state, nation_as, army_supplies_budget),
		.navy_stockpile_spendings = estimate_navy_stockpile_spending_after_refund(state, nation_as, navy_supplies_budget),
		.construction_spendings = estimate_construction_stockpile_spending_after_refund(state, nation_as, construction_budget),
		.stockpile_filling_spendings = estimate_government_stockpile_filling_spending_after_refund(state, nation_as, stockpile_filling_budget)
	};
	return result;
}
total_stockpile_spendings estimate_total_stockpile_spendings_before_refund(const sys::state& state, dcon::nation_id nation_as, float construction_budget, float stockpile_filling_budget, float army_supplies_budget, float navy_supplies_budget) {
	total_stockpile_spendings result{
		.army_stockpile_spendings = estimate_army_stockpile_spending_before_refund(state, nation_as, army_supplies_budget),
		.navy_stockpile_spendings = estimate_navy_stockpile_spending_before_refund(state, nation_as, navy_supplies_budget),
		.construction_spendings = estimate_construction_stockpile_spending_before_refund(state, nation_as, construction_budget),
		.stockpile_filling_spendings = estimate_government_stockpile_filling_spending_before_refund(state, nation_as, stockpile_filling_budget)
	};
	return result;
}

total_stockpile_spendings_by_commodity estimate_total_stockpile_spendings_by_commodity_after_refund(const sys::state& state, dcon::nation_id nation_as, float construction_budget, float stockpile_filling_budget, float army_supplies_budget, float navy_supplies_budget) {

	total_stockpile_spendings_by_commodity result{
		.army_stockpile_spendings = estimate_army_stockpile_spending_by_commodity_after_refund(state, nation_as, army_supplies_budget),
		.navy_stockpile_spendings = estimate_navy_stockpile_spending_by_commodity_after_refund(state, nation_as, navy_supplies_budget),
		.construction_spendings = estimate_construction_stockpile_spending_by_commodity_after_refund(state, nation_as, construction_budget),
		.stockpile_filling_spendings = estimate_government_stockpile_filling_spending_by_commodity_after_refund(state, nation_as, stockpile_filling_budget)
	};
	return result;
}
total_stockpile_spendings_by_commodity estimate_total_stockpile_spendings_by_commodity_before_refund(const sys::state& state, dcon::nation_id nation_as, float construction_budget, float stockpile_filling_budget, float army_supplies_budget, float navy_supplies_budget) {

	total_stockpile_spendings_by_commodity result{
		.army_stockpile_spendings = estimate_army_stockpile_spending_by_commodity_before_refund(state, nation_as, army_supplies_budget),
		.navy_stockpile_spendings = estimate_navy_stockpile_spending_by_commodity_before_refund(state, nation_as, navy_supplies_budget),
		.construction_spendings = estimate_construction_stockpile_spending_by_commodity_before_refund(state, nation_as, construction_budget),
		.stockpile_filling_spendings = estimate_government_stockpile_filling_spending_by_commodity_before_refund(state, nation_as, stockpile_filling_budget)
	};
	return result;
}


void populate_government_stockpile_demand(sys::state& state, ve::vectorizable_buffer<float, dcon::nation_id>& expected_total_price, ve::vectorizable_buffer<float, dcon::nation_id>& current_budget) {

	// Nobody should be using this dcon buffer at this time, so we can re-use it.
	auto demand_buffer_set = [&](auto nation, dcon::commodity_id com_id, auto val) {
		state.world.nation_set_commodity_float_buffer_1(nation, com_id, val);
	};
	auto demand_buffer_get = [&](auto nation, dcon::commodity_id com_id) {
		return state.world.nation_get_commodity_float_buffer_1(nation, com_id);
	};
	
	// Reset buffers and calculate the total stockpile demand per nation
	state.world.execute_serial_over_nation([&](auto ids) {
		auto nations_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation) && nations::exists(state, nation);
		}, ids);
		
		// Slow down demand from reaching stockpile targets just a bit, to keep things stable
		state.world.for_each_commodity([&](dcon::commodity_id com_id) {
			auto desired_amount = government_stockpile_desired_commodity_amount(state, ids, com_id);
			auto demand = desired_amount * 0.05f;
			demand_buffer_set(ids, com_id, ve::select(nations_valid, demand, ve::fp_vector{ 0.0f }));
		});
	});

	// Calculate the total expected price per nation, when taking into account demand weights in their controlled states.
	// We need to calculate this to be a modifier on demand later
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto nations_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
			auto nations_demand = demand_buffer_get(controllers, com_id);;
			auto expected_stockpile_price = get_estimated_state_stockpile_purchase_price<price_estimation::theoretical_max>(state, markets, com_id, nations_demand);
			ve::apply([&](float expected_price, dcon::nation_id nation, bool valid) {
				if(valid) {
					expected_total_price.set(nation, expected_total_price.get(nation) + expected_price);
				}
			}, expected_stockpile_price, controllers, nations_valid);
		});
	});
	// Calculate the actual amount of demand to apply on each market, depending on the nation budget and how much we are expected to be able to afford
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto controllers_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);
		auto total_expected_price = expected_total_price.get(controllers);
		auto nation_budget = current_budget.get(controllers);
		auto can_afford_mult = ve::select(total_expected_price == 0, 1.0f, ve::min(nation_budget / total_expected_price, 1.0f));
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
			auto percentage_weight = state.world.market_get_government_stockpile_demand_weights(markets, com_id);
			auto total_demand = demand_buffer_get(controllers, com_id);
			auto can_purchase = total_demand * percentage_weight * can_afford_mult;
			state.world.market_set_government_stockpile_demand(markets, com_id, ve::select(controllers_valid, can_purchase, 0.0f));
		});
	});
}

void presimulate(sys::state& state) {
	// set control to something reasonable to kickstart national economy
	state.world.execute_serial_over_province([&](auto pids){
		state.world.province_set_control_ratio(pids, 0.5f);
		state.world.province_set_control_scale(pids, state.world.province_get_demographics(pids, demographics::total) * 0.5f);
	});
	// economic updates without construction
#ifdef NDEBUG
	uint32_t steps = uint32_t(state.defines.alice_economy_presim_days);
#else
	uint32_t steps = 2;
#endif
	for(uint32_t i = 0; i < steps; i++) {
		float presim_completion = float(i) / float(steps);
		float employment_gradient_mult = 300.0f - 299.f * presim_completion;
		update_employment(state, true, employment_gradient_mult);
		daily_update(state, true, std::min(1.f, 2.f * (float)i / (float)steps), i - steps);
		ai::update_budget(state, true);
	}
}

bool has_building(sys::state const& state, dcon::state_instance_id si, dcon::factory_type_id fac) {
	auto sdef = state.world.state_instance_get_definition(si);
	auto owner = state.world.state_instance_get_nation_from_state_ownership(si);
	for(auto p : state.world.state_definition_get_abstract_state_membership(sdef)) {
		if(p.get_province().get_nation_from_province_ownership() == owner) {
			for(auto b : p.get_province().get_factory_location()) {
				if(b.get_factory().get_building_type() == fac)
					return true;
			}
		}
	}
	return false;
}

bool is_bankrupt_debtor_to(sys::state& state, dcon::nation_id debt_holder, dcon::nation_id debtor) {
	return state.world.nation_get_is_bankrupt(debt_holder) &&
		state.world.unilateral_relationship_get_owns_debt_of(
				state.world.get_unilateral_relationship_by_unilateral_pair(debtor, debt_holder)) > 0.1f;
}

bool nation_is_constructing_factories(sys::state& state, dcon::nation_id n) {
	auto rng = state.world.nation_get_factory_construction(n);
	return rng.begin() != rng.end();
}

bool factory_is_closed(sys::state const& state, dcon::factory_id f) {
	auto basic_scale = state.world.factory_get_primary_employment(f) + state.world.factory_get_unqualified_employment(f);
	if(basic_scale < factory_closed_threshold) {
		return true;
	}
	return false;
}

bool nation_has_closed_factories(sys::state& state, dcon::nation_id n) { // TODO - should be "good" now
	auto nation_fat = dcon::fatten(state.world, n);
	for(auto prov_owner : nation_fat.get_province_ownership()) {
		auto prov = prov_owner.get_province();
		for(auto factloc : prov.get_factory_location()) {
			if(factory_is_closed(state, factloc.get_factory())) {
				return true;
			}
		}
	}
	return false;
}

// Check if source gives trade rights to target. Includes derived rights from sphere/overlord
dcon::unilateral_relationship_id nation_gives_free_trade_rights(sys::state& state, dcon::nation_id source, dcon::nation_id target) {

	auto market_leader_target = nations::get_market_leader(state, target);
	auto market_leader_source = nations::get_market_leader(state, source);
	dcon::unilateral_relationship_id source_tariffs_rel;
	if(market_leader_target == market_leader_source) {
		source_tariffs_rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, source);
	}
	else {
		source_tariffs_rel = state.world.get_unilateral_relationship_by_unilateral_pair(market_leader_target, market_leader_source);
	}
	if(source_tariffs_rel) {
		auto enddt = state.world.unilateral_relationship_get_no_tariffs_until(source_tariffs_rel);
		// Enddt empty signalises revoken agreement
		// Enddt > cur_date signalises that the agreement can't be broken
		if(enddt) {
			return source_tariffs_rel;
		}
	}
	return dcon::unilateral_relationship_id{};
}
// Check if source gives trade rights to target. Only include direct relationship
dcon::unilateral_relationship_id nation_gives_direct_free_trade_rights(sys::state& state, dcon::nation_id source, dcon::nation_id target) {
	auto source_tariffs_rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, source);
	if(source_tariffs_rel) {
		auto enddt = state.world.unilateral_relationship_get_no_tariffs_until(source_tariffs_rel);
		// Enddt empty signalises revoken agreement
		// Enddt > cur_date signalises that the agreement can't be broken
		if(enddt) {
			return source_tariffs_rel;
		}
	}
	return dcon::unilateral_relationship_id{};
}

void update_yesterday_govt_stockpiles_cache(sys::state& state) {
	state.world.execute_parallel_over_nation([&](auto nations) {
		for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
			auto to_apply = state.world.nation_get_total_stockpiles(nations, com_id);
			state.world.nation_set_yesterday_total_stockpiles(nations, com_id, to_apply);
		});
	});
}


void initialize(sys::state& state) {
	initialize_artisan_distribution(state);

	state.world.for_each_commodity([&](dcon::commodity_id c) {
		state.world.execute_serial_over_market([&](auto markets) {
			state.world.market_set_price(markets, c, state.world.commodity_get_cost(c));
			state.world.market_set_price_confidence(markets, c, 1.f);

			state.world.market_set_aggregated_demand_history(markets, c, ve::fp_vector{});
			state.world.market_set_aggregated_supply_history(markets, c, ve::fp_vector{});
			state.world.market_set_demand(markets, c, ve::fp_vector{});
			state.world.market_set_supply(markets, c, ve::fp_vector{});
			state.world.market_set_consumption(markets, c, ve::fp_vector{});
		});

		auto fc = fatten(state.world, c);

		for(uint32_t i = 0; i < price_history_length; ++i) {
			fc.set_price_record(i, fc.get_cost());
		}
		// fc.set_global_market_pool();
	});

	services::reset_demand(state);
	services::reset_supply(state);
	services::reset_price(state);

	/*
	auto savings_buffer = state.world.pop_type_make_vectorizable_float_buffer();
	state.world.for_each_pop_type([&](dcon::pop_type_id t) {
		auto ft = fatten(state.world, t);
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			savings_buffer.get(t) +=
				state.world.commodity_get_is_available_from_start(c)
				?
				state.world.commodity_get_cost(c) * ft.get_life_needs(c)
				+ 0.5f * state.world.commodity_get_cost(c) * ft.get_everyday_needs(c)
				: 0.0f;
		});
		auto strata = (ft.get_strata() * 2) + 1;
		savings_buffer.get(t) *= strata;
	});
	*/


	auto expected_savings = expected_savings_per_capita;

	state.world.for_each_pop([&](dcon::pop_id p) {
		auto fp = fatten(state.world, p);
		state.world.pop_set_satisfaction(p, 0.4f);
		fp.set_savings(fp.get_size() * expected_savings);
	});

	sanity_check(state);

	auto csize = state.world.commodity_size();

	std::vector<float> rgo_efficiency_inputs_amount;
	rgo_efficiency_inputs_amount.resize(csize + 1);
	std::vector<int> rgo_efficiency_inputs_count;
	rgo_efficiency_inputs_count.resize(csize + 1);

	state.world.for_each_factory_type([&](dcon::factory_type_id ftid) {
		auto& e_inputs = state.world.factory_type_get_efficiency_inputs(ftid);
		for(uint32_t i = 0; i < e_inputs.set_size; i++) {
			if(e_inputs.commodity_type[i]) {
				rgo_efficiency_inputs_amount[e_inputs.commodity_type[i].index()] +=
					e_inputs.commodity_amounts[i];
				rgo_efficiency_inputs_count[e_inputs.commodity_type[i].index()] += 1;
			}
		}
	});

	// find the two most common efficiency inputs (usually, cement and machine parts)
	// TODO: add modding support to overwrite this generation

	int most_common_index = -1;
	int second_most_common_index = -1;
	int third_most_common_index = -1;
	int most_common_count = -1;
	int second_most_common_count = -1;
	int third_most_common_count = -1;

	for(size_t i = 0; i < rgo_efficiency_inputs_amount.size(); i++) {
		if(rgo_efficiency_inputs_count[i] >= most_common_count) {
			third_most_common_index = second_most_common_index;
			third_most_common_count = second_most_common_count;

			second_most_common_index = most_common_index;
			second_most_common_count = most_common_count;

			most_common_index = int(i);
			most_common_count = rgo_efficiency_inputs_count[i];
		} else if(rgo_efficiency_inputs_count[i] >= second_most_common_count) {
			third_most_common_index = second_most_common_index;
			third_most_common_count = second_most_common_count;

			second_most_common_index = int(i);
			second_most_common_count = rgo_efficiency_inputs_count[i];
		} else if(rgo_efficiency_inputs_count[i] >= third_most_common_count) {
			third_most_common_index = int(i);
			third_most_common_count = rgo_efficiency_inputs_count[i];
		}
	}

	// generate commodity set:
	economy::commodity_set base_rgo_e_inputs { };

	if(most_common_count > 0 && most_common_index != -1) {
		base_rgo_e_inputs.commodity_type[0] = dcon::commodity_id{ dcon::commodity_id::value_base_t(most_common_index) };
		base_rgo_e_inputs.commodity_amounts[0] =
			rgo_efficiency_inputs_amount[most_common_index]
			/ (float)most_common_count;
	}
	if(second_most_common_count > 0 && second_most_common_index != -1) {
		base_rgo_e_inputs.commodity_type[1] = dcon::commodity_id{ dcon::commodity_id::value_base_t(second_most_common_index) };
		base_rgo_e_inputs.commodity_amounts[1] =
			rgo_efficiency_inputs_amount[second_most_common_index]
			/ (float)second_most_common_count;
	}

	if(third_most_common_count > 0 && third_most_common_index != -1) {
		base_rgo_e_inputs.commodity_type[2] = dcon::commodity_id{ dcon::commodity_id::value_base_t(third_most_common_index) };
		base_rgo_e_inputs.commodity_amounts[2] =
			rgo_efficiency_inputs_amount[third_most_common_index]
			/ (float)third_most_common_count;
	}

	state.world.for_each_commodity([&](dcon::commodity_id cid) {
		if (state.world.commodity_get_rgo_efficiency_inputs_are_defined_in_content(cid)) return;
		auto & data = state.world.commodity_get_rgo_efficiency_inputs(cid);
		for(uint32_t i = 0; i < base_rgo_e_inputs.set_size; i++) {
			data.commodity_amounts[i] = base_rgo_e_inputs.commodity_amounts[i];
			data.commodity_type[i] = base_rgo_e_inputs.commodity_type[i];
		}
	});

	state.world.for_each_factory([&](dcon::factory_id f) {
		auto ff = fatten(state.world, f);
		ff.set_unqualified_employment(ff.get_size() * 0.04f);
		ff.set_primary_employment(0.f);
	});

	// learn some weights for rgo from initial territories:
	std::vector<std::vector<float>> per_climate_distribution_buffer(state.world.modifier_size() + 1, std::vector<float>(csize + 1, 0.f));
	std::vector<std::vector<float>> per_terrain_distribution_buffer(state.world.modifier_size() + 1, std::vector<float>(csize + 1, 0.f));
	std::vector<std::vector<float>> per_continent_distribution_buffer(state.world.modifier_size() + 1, std::vector<float>(csize + 1, 0.f));

	// init the map for climates
	province::for_each_land_province(state, [&](dcon::province_id p) {
		auto fp = fatten(state.world, p);
		dcon::commodity_id main_trade_good = state.world.province_get_rgo(p);
		dcon::modifier_id climate = fp.get_climate();
		dcon::modifier_id terrain = fp.get_terrain();
		dcon::modifier_id continent = fp.get_continent();
		if(main_trade_good) {
			per_climate_distribution_buffer[climate.value][main_trade_good.index()] += 1.f;
			per_terrain_distribution_buffer[terrain.value][main_trade_good.index()] += 1.f;
			per_continent_distribution_buffer[continent.value][main_trade_good.index()] += 1.f;
		}
	});

	// normalisation
	for(uint32_t i = 0; i < uint32_t(state.world.modifier_size()); i++) {
		float climate_sum = 0.f;
		float terrain_sum = 0.f;
		float continent_sum = 0.f;
		for(uint32_t j = 0; j < csize; j++) {
			climate_sum += per_climate_distribution_buffer[i][j];
			terrain_sum += per_terrain_distribution_buffer[i][j];
			continent_sum += per_continent_distribution_buffer[i][j];
		}
		for(uint32_t j = 0; j < csize; j++) {
			per_climate_distribution_buffer[i][j] *= climate_sum == 0.f ? 1.f : 1.f / climate_sum;
			per_terrain_distribution_buffer[i][j] *= terrain_sum == 0.f ? 1.f : 1.f / terrain_sum;
			per_continent_distribution_buffer[i][j] *= continent_sum == 0.f ? 1.f : 1.f / continent_sum;
		}
	}

	if(state.defines.alice_rgo_generate_distribution > 0.0f) {
		province::for_each_land_province(state, [&](dcon::province_id p) {
			/*
			Here we set up initial distribution of "mine" and "farm rgos"
			Max total potential size of "farm" rgos is limited by local province area.
			Initial size might be reduced depending on population.

			"Major mines" are defined as main rgo, they add a fixed amount of initial size
			"Secondary mines" add amount calculated from distribution and local population
			*/


			

			auto fp = fatten(state.world, p);
			//max size of exploitable land:
			auto max_agriculture_size = std::ceil(100.f * state.map_state.map_data.province_area_km2[province::to_map_id(p)]);
			// currently exploited land
			float pop_amount = 0.0f;
			for(auto pt : state.world.in_pop_type) {
				if(pt == state.culture_definitions.slaves) {
					pop_amount += state.world.province_get_demographics(p, demographics::to_key(state, state.culture_definitions.slaves));
				} else if(pt.get_is_paid_rgo_worker()) {
					pop_amount += state.world.province_get_demographics(p, demographics::to_key(state, pt));
				}
			}
			fp.set_rgo_base_size(max_agriculture_size);

			if(state.world.province_get_rgo_was_set_during_scenario_creation(p)) {
				return;
			}

			dcon::modifier_id climate = fp.get_climate();
			dcon::modifier_id terrain = fp.get_terrain();
			dcon::modifier_id continent = fp.get_continent();

			state.world.for_each_commodity([&](dcon::commodity_id c) {
				fp.set_rgo_target_employment(c, 0.f);
				state.world.province_set_rgo_base_efficiency(p, c, 1.f);
			});

			static std::vector<float> true_distribution;
			true_distribution.resize(state.world.commodity_size());

			float total = 0.f;
			state.world.for_each_commodity([&](dcon::commodity_id c) {
				float climate_d = per_climate_distribution_buffer[climate.value][c.index()];
				float terrain_d = per_terrain_distribution_buffer[terrain.value][c.index()];
				float continent_d = per_continent_distribution_buffer[continent.value][c.index()];
				float current = (climate_d + terrain_d) * (climate_d + terrain_d) * continent_d;
				true_distribution[c.index()] = current;
				total += current;
			});

			// remove continental restriction if failed:
			if(total == 0.f) {
				state.world.for_each_commodity([&](dcon::commodity_id c) {
					float climate_d = per_climate_distribution_buffer[climate.value][c.index()];
					float terrain_d = per_terrain_distribution_buffer[terrain.value][c.index()];
					float current = (climate_d + terrain_d) * (climate_d + terrain_d);
					true_distribution[c.index()] = current;
					total += current;
				});
			}

			// make it into uniform distrubution on available goods then...
			if(total == 0.f) {
				state.world.for_each_commodity([&](dcon::commodity_id c) {
					if(state.world.commodity_get_money_rgo(c)) {
						return;
					}
					if(!state.world.commodity_get_is_available_from_start(c)) {
						return;
					}
					float current = 1.f;
					true_distribution[c.index()] = current;
					total += current;
				});
			}

			auto main_rgo = state.world.province_get_rgo(p);
			if(main_rgo) {
				auto main_rgo_score = total * 0.5f;
				if(total <= 0.f)
					main_rgo_score = 1.f;
				true_distribution[main_rgo.id.index()] += main_rgo_score;
				total = total + main_rgo_score;
			}

			assert(total > 0.f);

			auto non_zero_count = 1.f;

			state.world.for_each_commodity([&](dcon::commodity_id c) {
				assert(std::isfinite(total));
				// if everything had failed for some reason, then assume 0 distribution: main rgo is still active
				if(total == 0.f) {
					true_distribution[c.index()] = 0.f;
				} else {
					true_distribution[c.index()] /= total;
					non_zero_count += 1.f;
				}
			});

			// distribution of rgo land per good
			state.world.for_each_commodity([&](dcon::commodity_id c) {
				assert(std::isfinite(true_distribution[c.index()]));
				auto major_mine_base_max_size = state.defines.alice_base_rgo_employment_bonus * 300'000.f;
				auto secondary_mine_base_max_size = state.defines.alice_base_rgo_employment_bonus * 50'000.f;

				auto max_size = 0.f;
				auto efficiency = (0.2f + 5.f * true_distribution[c.index()]);
				if(state.world.commodity_get_is_mine(c)) {
					if(main_rgo == c) {
						max_size += major_mine_base_max_size;
						efficiency += state.defines.alice_base_rgo_efficiency_bonus;
					}
					max_size += secondary_mine_base_max_size * true_distribution[c.index()];
				} else {
					max_size = max_agriculture_size;
				}

				auto actual_size = std::min(max_size, (10000.f + pop_amount * 1.f) * true_distribution[c.index()]);

				if(actual_size > state.defines.alice_secondary_rgos_min_employment || main_rgo == c) {
					state.world.province_set_rgo_size(p, c,
						state.world.province_get_rgo_size(p, c) + actual_size
					);
					state.world.province_set_rgo_potential(p, c,
						state.world.province_get_rgo_potential(p, c) + max_size
					);
					state.world.province_set_rgo_efficiency(p, c, 1.f);
					state.world.province_set_rgo_max_efficiency(p, c, 1.f);
					state.world.province_set_rgo_target_employment(p, c, main_rgo == c ? pop_amount : 0.f);
					state.world.province_set_rgo_base_efficiency(p, c, state.world.province_get_rgo_base_efficiency(p, c) + efficiency);

					auto area = state.world.province_get_state_membership(p);
					if(area) {
						// Check that it is not uncolonized and it does belong to a state instance
						auto market = state.world.state_instance_get_market_from_local_market(area);
						state.world.market_set_stockpile(market, c, state.world.market_get_stockpile(market, c) + 1.f + efficiency * actual_size / 10'000.f);
					}

				}
			});

			// add a trickle of money rgo everywhere to produce a source of inflation
			state.world.province_set_rgo_size(p, economy::money, state.world.province_get_rgo_size(p, economy::money) + 2500.f);
			state.world.province_set_rgo_potential(p, economy::money, state.world.province_get_rgo_size(p, economy::money) + 5000.f);
			state.world.province_set_rgo_efficiency(p, economy::money, 1.f);
			state.world.province_set_rgo_max_efficiency(p, economy::money, 1.f);
		});
	}

	state.world.for_each_nation([&](dcon::nation_id n) {
		auto fn = fatten(state.world, n);
		fn.set_administrative_spending(int8_t(35));
		fn.set_military_spending(int8_t(60));
		fn.set_education_spending(int8_t(100));
		fn.set_social_spending(int8_t(100));
		fn.set_land_spending(int8_t(100));
		fn.set_land_supply_consumption(int8_t(100));
		fn.set_land_reinforcement_consumption(int8_t(100));
		fn.set_army_construction_consumption(int8_t(100));
		fn.set_navy_construction_consumption(int8_t(100));
		fn.set_factory_construction_consumption(int8_t(100));
		fn.set_building_construction_consumption(int8_t(100));


		fn.set_naval_spending(int8_t(100));
		fn.set_naval_supply_consumption(int8_t(100));
		fn.set_naval_reinforcement_consumption(int8_t(100));


		fn.set_construction_spending(int8_t(100));
		fn.set_overseas_spending(int8_t(100));

		fn.set_poor_tax(int8_t(75));
		fn.set_middle_tax(int8_t(75));
		fn.set_rich_tax(int8_t(75));

		fn.set_spending_level(1.0f);
	});

	state.world.for_each_market([&](dcon::market_id n) {
		initialize_needs_weights(state, n);
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			state.world.market_set_expected_probability_to_buy(n, c, 0.0f);
			state.world.market_set_actual_probability_to_buy(n, c, 0.0f);
			state.world.market_set_expected_probability_to_sell(n, c, 0.0f);
			state.world.market_set_actual_probability_to_sell(n, c, 0.0f);
		});
	});

	province::ve_for_each_land_province(state, [&](auto ids) {
		for(int32_t i = 0; i < labor::total; i++) {
			state.world.province_set_labor_price(ids, i, 0.0001f);
		}
	});

	update_employment(state, true, 1.f);

	update_government_stockpile_market_demand_weights(state);

	populate_army_consumption(state);
	populate_navy_consumption(state);

	auto current_stockpile_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(state.world.nation_size()));
	auto expected_stockpile_total_cost = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(state.world.nation_size()));
	populate_government_stockpile_demand(state, expected_stockpile_total_cost, current_stockpile_budget);

	populate_government_construction_consumption(state);

	state.world.for_each_nation([&](dcon::nation_id n) {
		state.world.nation_set_treasury(n, 1000.f);
	});


	// civilian ports
	state.world.for_each_province([&](auto pid) {
		if(state.world.province_get_is_coast(pid)) {
			auto naval_base_level = state.world.province_get_building_level(pid, (uint8_t)(economy::province_building_type::naval_base));
			auto population = state.world.province_get_demographics(pid, demographics::total);
			state.world.province_set_advanced_province_building_max_private_size(
				pid,
				advanced_province_buildings::list::civilian_ports,
				naval_base_level * 25000.f + population * 0.0001f + 100.f
			);
		}
	});

	state.world.for_each_province([&](auto pid) {
		auto starting_urban_population_proxy =
			state.world.province_get_demographics(pid, demographics::literacy) * 0.3f
			+ state.world.province_get_demographics(pid, demographics::to_key(state, state.culture_definitions.artisans)) * 2.f;

		state.world.province_set_advanced_province_building_max_private_size(
			pid,
			advanced_province_buildings::list::local_cities_and_towns,
			starting_urban_population_proxy
		);

	});
}

float sphere_leader_share_factor(sys::state& state, dcon::nation_id sphere_leader, dcon::nation_id sphere_member) {
	/*
	Share factor : If the nation is a civ and is a secondary power start with define : SECOND_RANK_BASE_SHARE_FACTOR, and
	otherwise start with define : CIV_BASE_SHARE_FACTOR.Also calculate the sphere owner's foreign investment in the nation as a
	fraction of the total foreign investment in the nation (I believe that this is treated as zero if there is no foreign
	investment at all). The share factor is (1 - base share factor) x sphere owner investment fraction + base share factor. For
	uncivs, the share factor is simply equal to define:UNCIV_BASE_SHARE_FACTOR (so 1, by default). If a nation isn't in a sphere,
	we let the share factor be 0 if it needs to be used in any other calculation.
	*/
	if(state.world.nation_get_is_civilized(sphere_member)) {
		float base = state.world.nation_get_rank(sphere_member) <= state.defines.colonial_rank
			? state.defines.second_rank_base_share_factor
			: state.defines.civ_base_share_factor;
		auto const ul = state.world.get_unilateral_relationship_by_unilateral_pair(sphere_member, sphere_leader);
		float sl_investment = state.world.unilateral_relationship_get_foreign_investment(ul);
		float total_investment = nations::get_foreign_investment(state, sphere_member);
		float investment_fraction = total_investment > 0.0001f ? sl_investment / total_investment : 0.0f;
		return base + (1.0f - base) * investment_fraction;
	} else {
		return state.defines.unciv_base_share_factor;
	}
}


void update_factory_triggered_modifiers(sys::state& state) {
	state.world.for_each_factory([&](dcon::factory_id f) {
		auto fac_type = fatten(state.world, state.world.factory_get_building_type(f));
		float sum = 0.0f;
		auto prov = state.world.factory_get_province_from_factory_location(f);
		auto pstate = state.world.province_get_state_membership(prov);
		auto powner = state.world.province_get_nation_from_province_ownership(prov);

		if(powner && pstate) {
			for(auto bonus : fac_type.get_factory_bonuses()) {
				if(bonus.trigger && trigger::evaluate(state, bonus.trigger, trigger::to_generic(pstate), trigger::to_generic(powner), 0)) {
					sum -= bonus.amount;
				}
			}
		}

		state.world.factory_set_triggered_modifiers(f, sum);
	});
}

float subsistence_size(sys::state const& state, dcon::province_id p) {
	auto rgo_ownership = state.world.province_get_landowners_share(p) + state.world.province_get_capitalists_share(p);
	return state.world.province_get_rgo_base_size(p) * (1.f - rgo_ownership);
}

float subsistence_max_pseudoemployment(sys::state& state, dcon::province_id p) {
	return subsistence_size(state, p) * 1.1f;
}

void update_local_subsistence_factor(sys::state& state) {
	province::ve_for_each_land_province(state, [&](auto ids) {
		auto quality = (ve::to_float(state.world.province_get_life_rating(ids)) - 10.f) / 10.f;
		quality = ve::max(quality, 0.f) + 0.01f;
		auto score = (subsistence_factor * quality) + subsistence_score_life * 0.9f;
		state.world.province_set_subsistence_score(ids, score);
	});
}


void update_land_ownership(sys::state& state) {
	province::ve_for_each_land_province(state, [&](auto ids) {
		auto local_states = state.world.province_get_state_membership(ids);
		auto weight_population =
			state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.farmers))
			+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.laborers));
		auto weight_capitalists = weight_population / 50.f
			+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.capitalists)) * 200.f;
		auto weight_aristocracy = weight_population / 50.f
			+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.aristocrat)) * 200.f
			+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.slaves));
		auto total = weight_aristocracy + weight_capitalists + weight_population + 1.0f;
		state.world.province_set_landowners_share(ids, weight_aristocracy / total);
		state.world.province_set_capitalists_share(ids, weight_capitalists / total);
	});
}

int32_t factory_priority(sys::state const& state, dcon::factory_id f) {
	return (state.world.factory_get_priority_low(f) ? 1 : 0) + (state.world.factory_get_priority_high(f) ? 2 : 0);
}
void set_factory_priority(sys::state& state, dcon::factory_id f, int32_t priority) {
	state.world.factory_set_priority_high(f, priority >= 2);
	state.world.factory_set_priority_low(f, (priority & 1) != 0);
}
bool factory_is_profitable(sys::state const& state, dcon::factory_id f) {
	return state.world.factory_get_unprofitable(f) == false || state.world.factory_get_subsidized(f);
}

struct commodity_profit_holder {
	float profit = 0.0f;
	dcon::commodity_id c;
};

float factory_unqualified_employment(sys::state const& state, dcon::factory_id f) {
	auto pid = state.world.factory_get_province_from_factory_location(f);
	auto employment = state.world.factory_get_unqualified_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::no_education);
	return employment;
}
float factory_primary_employment(sys::state const& state, dcon::factory_id f) {
	auto pid = state.world.factory_get_province_from_factory_location(f);
	auto primary_employment = state.world.factory_get_primary_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::basic_education);
	return primary_employment;
}
float factory_secondary_employment(sys::state const& state, dcon::factory_id f) {
	auto pid = state.world.factory_get_province_from_factory_location(f);
	auto secondary_employment = state.world.factory_get_secondary_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::high_education);
	return secondary_employment;
}

float factory_total_employment(sys::state const& state, dcon::factory_id f) {
	auto pid = state.world.factory_get_province_from_factory_location(f);
	return (
		state.world.factory_get_unqualified_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::no_education)
		+
		state.world.factory_get_primary_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::basic_education)
		+
		state.world.factory_get_secondary_employment(f)
		* state.world.province_get_labor_demand_satisfaction(pid, labor::high_education)
	);
}





void update_pops_employment(sys::state& state) {
	province::ve_parallel_for_each_land_province(state, [&](auto pids) {
		auto nation = state.world.province_get_nation_from_province_ownership(pids);
		// calculate total pops employment:

		auto no_education = state.world.province_get_labor_supply_sold(pids, labor::no_education);
		auto basic_education = state.world.province_get_labor_supply_sold(pids, labor::basic_education); // craftsmen
		auto high_education = state.world.province_get_labor_supply_sold(pids, labor::high_education); // clerks, clergy and bureaucrats
		auto guild_education = state.world.province_get_labor_supply_sold(pids, labor::guild_education); // artisans
		auto high_education_and_accepted = state.world.province_get_labor_supply_sold(pids, labor::high_education_and_accepted); // clerks, clergy and bureaucrats of accepted culture

		auto rgo_workers_employment =
			state.world.province_get_pop_labor_distribution(pids, pop_labor::rgo_worker_no_education)
			* no_education;

		auto primary_workers_employment =
			state.world.province_get_pop_labor_distribution(pids, pop_labor::primary_no_education)
			* no_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::primary_basic_education)
			* basic_education;

		auto high_not_accepted_workers_employment =
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_not_accepted_no_education)
			* no_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_not_accepted_basic_education)
			* basic_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_not_accepted_high_education)
			* high_education;

		auto high_accepted_workers_employment =
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_accepted_no_education)
			* no_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_accepted_basic_education)
			* basic_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_accepted_high_education)
			* high_education
			+
			state.world.province_get_pop_labor_distribution(pids, pop_labor::high_education_accepted_high_education_accepted)
			* high_education_and_accepted;


		ve::apply([&](dcon::province_id p, dcon::nation_id n, float rgo, float primary, float high, float high_accepted) {
			auto subsistence_employment = 0.f;
			for(auto pop_location : state.world.province_get_pop_location(p)) {
				auto pop = pop_location.get_pop();

				auto accepted = state.world.nation_get_accepted_cultures(n, pop.get_culture())
					|| state.world.nation_get_primary_culture(n) == pop.get_culture();

				if(pop.get_poptype() == state.culture_definitions.slaves) {
					pop_demographics::set_raw_employment(state, pop, rgo);
					subsistence_employment += pop.get_size() * (1.f - rgo);
				} else if(pop.get_poptype().get_is_paid_rgo_worker()) {
					pop_demographics::set_raw_employment(state, pop, rgo);
					subsistence_employment += pop.get_size() * (1.f - rgo);
				} else if(pop.get_poptype() == state.culture_definitions.primary_factory_worker) {
					pop_demographics::set_raw_employment(state, pop, primary);
					subsistence_employment += pop.get_size() * (1.f - primary);
				} else if(pop.get_poptype() == state.culture_definitions.secondary_factory_worker) {
					if(accepted) {
						pop_demographics::set_raw_employment(state, pop, high_accepted);
						subsistence_employment += pop.get_size() * (1.f - high_accepted);
					} else {
						pop_demographics::set_raw_employment(state, pop, high);
						subsistence_employment += pop.get_size() * (1.f - high);
					}
				} else if(pop.get_poptype() == state.culture_definitions.bureaucrat) {
					if(accepted) {
						pop_demographics::set_raw_employment(state, pop, high_accepted);
						subsistence_employment += pop.get_size() * (1.f - high_accepted);
					} else {
						pop_demographics::set_raw_employment(state, pop, high);
						subsistence_employment += pop.get_size() * (1.f - high);
					}
				} else if(pop.get_poptype() == state.culture_definitions.clergy) {
					if(accepted) {
						pop_demographics::set_raw_employment(state, pop, high_accepted);
						subsistence_employment += pop.get_size() * (1.f - high_accepted);
					} else {
						pop_demographics::set_raw_employment(state, pop, high);
						subsistence_employment += pop.get_size() * (1.f - high);
					}
				}
			}
			state.world.province_set_subsistence_employment(
				p,
				std::max(
					0.f,
					std::min(
						subsistence_employment,
						subsistence_max_pseudoemployment(state, p)
					)
				)
			);
		}, pids, nation, rgo_workers_employment, primary_workers_employment, high_not_accepted_workers_employment, high_accepted_workers_employment);
	});
}

float convex_function(float x) {
	return 1.f - (1.f - x) * (1.f - x);
}



void populate_army_consumption(sys::state& state) {
	static auto current_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_total_price = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if(new_count > old_count) {
			current_budget = state.world.nation_make_vectorizable_float_buffer();
			expected_total_price = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	// Nobody should be using this dcon buffer at this time, so we can re-use it
	auto demand_buffer_set = [&](auto nation, dcon::unit_supply_and_build_commodity_id com_id, auto val) {
		state.world.nation_set_unit_supply_and_build_commodity_float_buffer_1(nation, com_id, val);
	};
	auto demand_buffer_get = [&](auto nation, dcon::unit_supply_and_build_commodity_id com_id) {
		return state.world.nation_get_unit_supply_and_build_commodity_float_buffer_1(nation, com_id);
	};
	// Set nation budget values and reset buffer
	state.world.execute_serial_over_nation([&](auto ids) {
		auto base_budget = state.world.nation_get_last_base_budget(ids);
		auto army_priority = ve::to_float(state.world.nation_get_land_spending(ids)) / 100.f;
		current_budget.set(ids, ve::max(0.f, base_budget * army_priority));
		expected_total_price.set(ids, 0.0f);
		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			demand_buffer_set(ids, com_id, ve::fp_vector{ 0.0f });
		});
	});
	// Add up total commodity demand from regiments per nation. We always want to populate demand as if we are trying to buy as many goods as it would take to fully supply all units, clamped by the budget allocated
	state.world.for_each_regiment([&](dcon::regiment_id regiment) {
		auto army = state.world.regiment_get_army_from_army_membership(regiment);
		auto nation = state.world.army_get_controller_from_army_control(army);
		if(nation) {
			auto accumulate_consumption = [&](dcon::commodity_id com_id, float amount) {
				auto union_com_id = state.world.commodity_get_unit_supply_and_build_commodity(com_id);
				demand_buffer_set(nation, union_com_id, demand_buffer_get(nation, union_com_id) + amount);
			};
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, regiment, accumulate_consumption);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, regiment, accumulate_consumption);
		}
	});
	// Calculate the total expected price per nation, when taking into account demand weights in their controlled states.
	// We need to calculate this to be a modifier on demand later
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto nations_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);

		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			auto nations_demand = demand_buffer_get(controllers, com_id);
			auto base_com_id = economy::unit_commodity_get_base_commodity(state, com_id);
			auto expected_stockpile_price = get_estimated_state_stockpile_purchase_price<price_estimation::theoretical_max>(state, markets, base_com_id, nations_demand);
			ve::apply([&](float expected_price, dcon::nation_id nation, bool valid) {
				if (valid) {
					expected_total_price.set(nation, expected_total_price.get(nation) + expected_price);
				}
			}, expected_stockpile_price, controllers, nations_valid);
		});
	});
	// Calculate the actual amount of demand to apply on each market, depending on the nation budget and how much we are expected to be able to afford
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto controllers_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);
		auto total_expected_price = expected_total_price.get(controllers);
		auto nation_budget = current_budget.get(controllers);
		auto can_afford_mult = ve::select(total_expected_price == 0, max_army_required_spend, ve::min(nation_budget / total_expected_price, max_army_required_spend));
		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			dcon::commodity_id base_commodity = economy::unit_commodity_get_base_commodity(state, com_id);
			auto percentage_weight = state.world.market_get_government_stockpile_demand_weights(markets, base_commodity);
			auto total_demand = demand_buffer_get(controllers, com_id);
			auto can_purchase = total_demand * percentage_weight * can_afford_mult;
			state.world.market_set_army_demand(markets, base_commodity, ve::select(controllers_valid, can_purchase, 0.0f));
		});
	});
}

void populate_navy_consumption(sys::state& state) {

	static auto current_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_total_price = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if (new_count > old_count) {
			current_budget = state.world.nation_make_vectorizable_float_buffer();
			expected_total_price = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	// Nobody should be using this dcon buffer at this time, so we can re-use it. Buffer 1 is already used in parallel by populating army demand...
	auto demand_buffer_set = [&](auto nation, dcon::unit_supply_and_build_commodity_id com_id, auto val) {
		state.world.nation_set_unit_supply_and_build_commodity_float_buffer_2(nation, com_id, val);
	};
	auto demand_buffer_get = [&](auto nation, dcon::unit_supply_and_build_commodity_id com_id) {
		return state.world.nation_get_unit_supply_and_build_commodity_float_buffer_2(nation, com_id);
	};
	// Set nation budget values and reset buffer
	state.world.execute_serial_over_nation([&](auto ids) {
		auto base_budget = state.world.nation_get_last_base_budget(ids);
		auto army_priority = ve::to_float(state.world.nation_get_naval_spending(ids)) / 100.f;
		current_budget.set(ids, ve::max(0.f, base_budget * army_priority));
		expected_total_price.set(ids, 0.0f);
		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			demand_buffer_set(ids, com_id, ve::fp_vector{ 0.0f });
		});
	});
	// Add up total commodity demand from ships per nation. We always want to populate demand as if we are trying to buy as many goods as it would take to fully supply all units, clamped by the budget allocated
	state.world.for_each_ship([&](dcon::ship_id ship) {
		auto navy = state.world.ship_get_navy_from_navy_membership(ship);
		auto nation = state.world.navy_get_controller_from_navy_control(navy);
		if(nation) {
			auto accumulate_consumption = [&](dcon::commodity_id com_id, float amount) {
				auto union_com_id = state.world.commodity_get_unit_supply_and_build_commodity(com_id);
				demand_buffer_set(nation, union_com_id, demand_buffer_get(nation, union_com_id) + amount);
			};
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::supply>(state, nation, ship, accumulate_consumption);
			military::accumulate_subunit_daily_goods_requirements<military::unit_consumption_type::reinforcement>(state, nation, ship, accumulate_consumption);
		}
	});
	// Calculate the total expected price per nation, when taking into account demand weights in their controlled states.
	// We need to calculate this to be a modifier on demand later
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto nations_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);
		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			auto nations_demand = demand_buffer_get(controllers, com_id);
			auto base_com_id = economy::unit_commodity_get_base_commodity(state, com_id);
			auto expected_stockpile_price = get_estimated_state_stockpile_purchase_price<price_estimation::theoretical_max>(state, markets, base_com_id, nations_demand);
			ve::apply([&](float expected_price, dcon::nation_id nation, bool valid) {
				if (valid) {
					expected_total_price.set(nation, expected_total_price.get(nation) + expected_price);
				}
			}, expected_stockpile_price, controllers, nations_valid);
		});
	});
	// Calculate the actual amount of demand to apply on each market, depending on the nation budget and how much we are expected to be able to afford
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto controllers_valid = ve::apply([&](dcon::nation_id nation) {
			return state.world.nation_is_valid(nation);
		}, controllers);
		auto total_expected_price = expected_total_price.get(controllers);
		auto nation_budget = current_budget.get(controllers);
		auto can_afford_mult = ve::select(total_expected_price == 0, max_navy_required_spend, ve::min(nation_budget / total_expected_price, max_navy_required_spend));
		state.world.for_each_unit_supply_and_build_commodity([&](dcon::unit_supply_and_build_commodity_id com_id) {
			dcon::commodity_id base_commodity = economy::unit_commodity_get_base_commodity(state, com_id);
			auto percentage_weight = state.world.market_get_government_stockpile_demand_weights(markets, base_commodity);
			auto total_demand = demand_buffer_get(controllers, com_id);
			auto can_purchase = total_demand * percentage_weight * can_afford_mult;
			state.world.market_set_navy_demand(markets, base_commodity, ve::select(controllers_valid, can_purchase, 0.0f));
		});
	});
}

void add_total_govt_stockpile(sys::state& state, dcon::nation_id controller, dcon::commodity_id commodity, float amount) {
	float current_national = state.world.nation_get_total_stockpiles(controller, commodity);
	state.world.nation_set_total_stockpiles(controller, commodity, current_national + amount);
}
void subtract_total_govt_stockpile(sys::state& state, dcon::nation_id controller, dcon::commodity_id commodity, float amount) {
	float current_national = state.world.nation_get_total_stockpiles(controller, commodity);
	float to_sub = std::min(current_national, amount);
	state.world.nation_set_total_stockpiles(controller, commodity, current_national - to_sub);
}

void add_government_stockpile(sys::state& state, dcon::nation_id controller, dcon::market_id market, dcon::commodity_id commodity, float amount) {
	assert(amount >= 0.0f);
	assert(controller);
	float current_local = state.world.market_get_government_stockpile(market, commodity);
	state.world.market_set_government_stockpile(market, commodity, current_local + amount);
	add_total_govt_stockpile(state, controller, commodity, amount);
	assert(state.world.market_get_government_stockpile(market, commodity) >= 0.0f);
	assert(state.world.nation_get_total_stockpiles(controller, commodity) >= 0.0f);
}
void subtract_government_stockpile(sys::state& state, dcon::nation_id controller, dcon::market_id market, dcon::commodity_id commodity, float amount) {
	assert(amount >= 0.0f);
	assert(controller);
	float current_local = state.world.market_get_government_stockpile(market, commodity);
	float to_sub = std::min(current_local, amount);
	state.world.market_set_government_stockpile(market, commodity, current_local - to_sub);
	subtract_total_govt_stockpile(state, controller, commodity, amount);
	assert(state.world.market_get_government_stockpile(market, commodity) >= 0.0f);
	assert(state.world.nation_get_total_stockpiles(controller, commodity) >= 0.0f);
}

void add_rebel_stockpile(sys::state& state, dcon::market_id market, dcon::commodity_id commodity, float amount) {
	assert(amount >= 0.0f);
	assert(market);
	float current_local = state.world.market_get_government_stockpile(market, commodity);
	state.world.market_set_government_stockpile(market, commodity, current_local + amount);
	assert(state.world.market_get_government_stockpile(market, commodity) >= 0.0f);
}
void subtract_rebel_stockpile(sys::state& state, dcon::market_id market, dcon::commodity_id commodity, float amount) {
	assert(amount >= 0.0f);
	assert(market);
	float current_local = state.world.market_get_government_stockpile(market, commodity);
	float to_sub = std::min(current_local, amount);
	state.world.market_set_government_stockpile(market, commodity, current_local - to_sub);
	assert(state.world.market_get_government_stockpile(market, commodity) >= 0.0f);
}



spending_cost full_spending_cost(sys::state& state, dcon::nation_id n, float base_budget) {
	spending_cost costs = {
		.administration = 0.f,
		.construction = 0.f,
		.other = 0.f,
		.total = 0.f,
	};

	float total = 0.0f;


	auto const admin_spending = full_spendings_administration_before_refund(state, n, base_budget);
	costs.administration = admin_spending;
	total += admin_spending;

	assert(std::isfinite(total) && total >= 0.0f);

	float military_total = 0.f;
	uint32_t total_commodities = state.world.commodity_size();

	float construction_budget =
		std::max(
			0.f,
			float(state.world.nation_get_construction_spending(n))
			/ 100.0f
			* base_budget
		);
	float land_budget = std::max(
			0.f,
			float(state.world.nation_get_land_spending(n))
			/ 100.0f
			* base_budget
	);
	float navy_budget = std::max(
			0.f,
			float(state.world.nation_get_naval_spending(n))
			/ 100.0f
			* base_budget
	);
	float stockpile_budget = std::max(
			0.f,
			float(state.world.nation_get_stockpile_spending(n))
			/ 100.0f
			* base_budget
	);

	float o_spending = float(state.world.nation_get_overseas_spending(n)) / 100.f;

	float total_construction_costs = 0.f;
	float total_navy_costs = 0.0f;
	float total_army_costs = 0.0f;
	float total_stockpiling_costs = 0.0f;
	// Govt stockpile demand is done in controlled states
	state.world.nation_for_each_state_control(n, [&](auto soid) {
		auto local_state = state.world.state_control_get_state(soid);
		auto local_market = state.world.state_instance_get_market_from_local_market(local_state);
		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			total_army_costs += state.world.market_get_army_demand(local_market, cid) * state.world.market_get_price(local_market, cid);
			total_navy_costs += state.world.market_get_navy_demand(local_market, cid) * state.world.market_get_price(local_market, cid);
			total_construction_costs += state.world.market_get_government_construction_demand(local_market, cid) * state.world.market_get_price(local_market, cid);
			total_stockpiling_costs += state.world.market_get_government_stockpile_demand(local_market, cid) * state.world.market_get_price(local_market, cid);
		}
	});
	//costs.construction = std::min(construction_budget, total_construction_costs);
	//total += (std::min(land_budget, total_army_costs) + std::min(navy_budget, total_navy_costs) + costs.construction + std::min(stockpile_budget, total_stockpiling_costs));
	costs.construction = total_construction_costs;
	total += (total_army_costs + total_navy_costs + costs.construction + total_stockpiling_costs);

	auto capital_state = state.world.province_get_state_membership(state.world.nation_get_capital(n));
	auto capital_market = state.world.state_instance_get_market_from_local_market(capital_state);

	assert(std::isfinite(total) && total >= 0.0f);
	// ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
	if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
		auto overseas_factor = state.defines.province_overseas_penalty * float(state.world.nation_get_owned_province_count(n) - state.world.nation_get_central_province_count(n));
		if(overseas_factor > 0) {
			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
				if(state.world.commodity_get_overseas_penalty(cid) && (state.world.commodity_get_is_available_from_start(cid) || state.world.nation_get_unlocked_commodities(n, cid))) {
					total += overseas_factor * state.world.market_get_price(capital_market, cid) * o_spending;
				}
			}
		}
	}

	assert(std::isfinite(total) && total >= 0.0f);

	// direct payments to pops
	auto capitalists_key = demographics::to_key(state, state.culture_definitions.capitalists);
	auto aristocracy_key = demographics::to_key(state, state.culture_definitions.aristocrat);
	auto capitalists = state.world.nation_get_demographics(n, capitalists_key);
	auto aristocrats = state.world.nation_get_demographics(n, aristocracy_key);

	if(capitalists + aristocrats > 0) {
		total += base_budget * float(state.world.nation_get_domestic_investment_spending(n)) / 100.f;
	}

	auto const m_spending = float(state.world.nation_get_military_spending(n)) / 100.f;

	auto const p_level = std::max(0.f, state.world.nation_get_modifier_values(n, sys::national_mod_offsets::pension_level));
	auto const unemp_level = std::max(0.f, state.world.nation_get_modifier_values(n, sys::national_mod_offsets::unemployment_benefit));

	if(p_level + unemp_level > 0.f) {
		total += base_budget * float(state.world.nation_get_social_spending(n)) / 100.0f;
	}

	total += economy::national_budget::estimate_education_spending_before_refund(state, n);

	state.world.nation_for_each_state_ownership(n, [&](auto soid) {
		auto local_state = state.world.state_ownership_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);

		state.world.for_each_pop_type([&](dcon::pop_type_id pt) {
			auto key = demographics::to_key(state, pt);
			auto adj_pop_of_type =
				state.world.state_instance_get_demographics(local_state, key)
				/ state.defines.alice_needs_scaling_factor;

			if(adj_pop_of_type <= 0)
				return;

			auto ln_type = culture::income_type(state.world.pop_type_get_life_needs_income_type(pt));
			if(ln_type == culture::income_type::military) {
				total += m_spending * adj_pop_of_type * state.world.market_get_life_needs_costs(market, pt) * payouts_spending_multiplier;
			}

			auto en_type = culture::income_type(state.world.pop_type_get_everyday_needs_income_type(pt));
			if(en_type == culture::income_type::military) {
				total += m_spending * adj_pop_of_type * state.world.market_get_everyday_needs_costs(market, pt) * payouts_spending_multiplier;
			}

			auto lx_type = culture::income_type(state.world.pop_type_get_luxury_needs_income_type(pt));
			if(lx_type == culture::income_type::military) {
				total += m_spending * adj_pop_of_type * state.world.market_get_luxury_needs_costs(market, pt) * payouts_spending_multiplier;
			}

			assert(std::isfinite(total) && total >= 0.0f);
		});
	});

	total += state.world.nation_get_subsidy_token_total(n) == 0.f ? 0.f : base_budget * float(state.world.nation_get_subsidies_spending(n)) / 100.f;

	assert(std::isfinite(total) && total >= 0.0f);

	costs.total = total;
	costs.other = total - costs.construction - costs.administration;

	return costs;
}


float estimate_overseas_penalty_spending_before_refund(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	auto capital = state.world.nation_get_capital(n);
	auto capital_state = state.world.province_get_state_membership(capital);
	auto market = state.world.state_instance_get_market_from_local_market(capital_state);
	// ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
	if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
		auto overseas_factor = state.defines.province_overseas_penalty * float(state.world.nation_get_owned_province_count(n) - state.world.nation_get_central_province_count(n));
		uint32_t total_commodities = state.world.commodity_size();
		if(overseas_factor > 0) {
			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };

				if(state.world.commodity_get_overseas_penalty(cid) && (state.world.commodity_get_is_available_from_start(cid) || state.world.nation_get_unlocked_commodities(n, cid))) {
					total +=
						overseas_factor
						* state.world.market_get_price(market, cid);
				}
			}
		}
	}
	return total;
}
float estimate_overseas_penalty_spending_after_refund(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	auto capital = state.world.nation_get_capital(n);
	auto capital_state = state.world.province_get_state_membership(capital);
	auto market = state.world.state_instance_get_market_from_local_market(capital_state);
	// ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
	if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
		auto overseas_factor = state.defines.province_overseas_penalty * float(state.world.nation_get_owned_province_count(n) - state.world.nation_get_central_province_count(n));
		uint32_t total_commodities = state.world.commodity_size();
		if(overseas_factor > 0) {
			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };

				if(state.world.commodity_get_overseas_penalty(cid) && (state.world.commodity_get_is_available_from_start(cid) || state.world.nation_get_unlocked_commodities(n, cid))) {
					total +=
						overseas_factor
						* state.world.market_get_price(market, cid)
						* state.world.market_get_actual_probability_to_buy(market, cid);
				}
			}
		}
	}
	return total;
}

float full_private_investment_cost(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	state.world.nation_for_each_state_ownership(n, [&](auto soid) {
		auto local_state = state.world.state_ownership_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);
		uint32_t total_commodities = state.world.commodity_size();
		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			total +=
				state.world.market_get_private_construction_demand(market, cid)
				* state.world.market_get_price(market, cid);
		}
	});
	return total;
}

void update_private_consumption(sys::state& state, dcon::nation_id n, float private_investment_scale) {
	uint32_t total_commodities = state.world.commodity_size();
	state.world.nation_for_each_state_ownership(n, [&](auto soid) {
		auto local_state = state.world.state_ownership_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);

		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			register_demand(
				state,
				market,
				cid,
				state.world.market_get_private_construction_demand(market, cid)
				* private_investment_scale
			);
		}
	});
}






void update_national_consumption(sys::state& state, dcon::nation_id n, float spending_scale, float base_budget) {
	uint32_t total_commodities = state.world.commodity_size();
	float o_spending = float(state.world.nation_get_overseas_spending(n)) / 100.0f;

	state.world.nation_for_each_state_control(n, [&](auto soid) {
		auto local_state = state.world.state_control_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);

		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			auto actual_stockpile_demand = economy::combined_government_stockpile_demand(state, market, cid);
			register_demand(
			state,
			market,
			cid,
			actual_stockpile_demand * spending_scale
			);

		}
	});

	auto capital = state.world.nation_get_capital(n);
	auto capital_state = state.world.province_get_state_membership(capital);
	// ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
	if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
		auto market = state.world.state_instance_get_market_from_local_market(capital_state);

		auto overseas_factor = state.defines.province_overseas_penalty * float(state.world.nation_get_owned_province_count(n) - state.world.nation_get_central_province_count(n));
		if(overseas_factor > 0.f) {
			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
				if(state.world.commodity_get_overseas_penalty(cid) && (state.world.commodity_get_is_available_from_start(cid) || state.world.nation_get_unlocked_commodities(n, cid))) {
					auto sat = state.world.market_get_expected_probability_to_buy(market, cid);
					auto sat_importance = std::min(1.f, 1.f / (state.world.market_get_price(market, cid) + 0.001f));
					auto sat_coefficient = (sat_importance + (1.f - sat_importance) * sat);

					register_demand(
						state,
						market,
						cid,
						overseas_factor
						* spending_scale
						* o_spending
						* sat_coefficient
					);
				}
			}
		}
	}
	
}

// ### Private Investment ###

const inline float courage = 1.0f;
const inline float days_prepaid = 200.f;

/* Returns number of initiated projects */
std::vector<full_construction_factory> estimate_private_investment_upgrade(sys::state& state, dcon::nation_id nid, float est_private_const_spending) {
	std::vector<full_construction_factory> res;
	auto n = dcon::fatten(state.world, nid);
	auto nation_rules = n.get_combined_issue_rules();

	if(!n.get_is_civilized()) {
		return res;
	}
	if((nation_rules & issue_rule::pop_expand_factory) == 0) {
		return res;
	}

	// check if current projects are already too expensive for capitalists to manage
	float total_cost = est_private_const_spending * days_prepaid * 40.f;
	float total_cost_added = 0.f;
	float current_inv = n.get_private_investment() * investment_pool_investment_per_day;

	if(current_inv * courage < total_cost + total_cost_added) {
		return res;
	}
	if(current_inv <= total_cost) {
		return res;
	}

	static std::vector<dcon::factory_type_id> desired_types;
	desired_types.clear();

	static std::vector<dcon::province_id> provinces_in_order;
	provinces_in_order.clear();
	for(auto si : n.get_province_ownership()) {
		if(si.get_province().get_state_membership().get_capital().get_is_colonial() == false) {
			provinces_in_order.push_back(si.get_province().id);
		}
	}

	std::sort(
		provinces_in_order.begin(),
		provinces_in_order.end(),
		[&](dcon::province_id a, dcon::province_id b
			) {
				auto a_pop = state.world.province_get_demographics(a, demographics::total);
				auto b_pop = state.world.province_get_demographics(b, demographics::total);
				if(a_pop != b_pop)
					return a_pop > b_pop;
				return a.index() < b.index(); // force total ordering
	});

	//upgrade all good targets!!!
	//upgrading only one per run is too slow and leads to massive unemployment!!!

	for(auto s : provinces_in_order) {
		auto sid = state.world.province_get_state_membership(s);
		auto market = state.world.state_instance_get_market_from_local_market(sid);

		float profit = 0.0f;
		dcon::factory_id selected_factory;

		// is there an upgrade target ?
		for(auto f : state.world.province_get_factory_location(s)) {
			if(
				factory_total_employment_score(state, f.get_factory()) >= 0.9f) {
				auto type = f.get_factory().get_building_type();
				auto ug_in_progress = false;
				for(auto c : state.world.province_get_factory_construction(s)) {
					if(c.get_type() == type) {
						ug_in_progress = true;
						break;
					}
				}

				if(ug_in_progress) {
					continue;
				}

				if(auto new_p = explain_last_factory_profit(state, f.get_factory()).profit / (f.get_factory().get_size() + 1);
					new_p > profit
				) {
					profit = new_p;
					selected_factory = f.get_factory();
				}
			}
		}
		if(selected_factory && profit > 0.f) {
			auto ft = state.world.factory_get_building_type(selected_factory);
			auto time = factory_building_construction_time(state, ft, true);
			auto cm = factory_build_cost_multiplier(state, nid, state.world.factory_get_province_from_factory_location(selected_factory), true);
			auto& costs = state.world.factory_type_get_construction_costs(ft);

			float added_cost = 0.0f;
			for(uint32_t i = 0; i < commodity_set::set_size; ++i) {
				if(costs.commodity_type[i]) {
					added_cost +=
						costs.commodity_amounts[i]
						* state.world.market_get_price(market, costs.commodity_type[i])
						* cm
						/ float(time) ;
				} else {
					break;
				}
			}
			total_cost_added += added_cost * days_prepaid;

			if(current_inv * courage < total_cost + total_cost_added) {
				break;
			}
			res.push_back({ added_cost, n, s, true, true, state.world.factory_get_building_type(selected_factory) });
		}
	}

	return res;
}

/* Returns number of initiated projects */
std::vector<full_construction_factory> estimate_private_investment_construct(sys::state& state, dcon::nation_id nid, bool craved, float est_private_const_spending, bool& potential_target_exists) {
	std::vector<full_construction_factory> res;

	auto n = dcon::fatten(state.world, nid);
	auto nation_rules = n.get_combined_issue_rules();

	if(!n.get_is_civilized()) {
		return res;
	}
	if((nation_rules & issue_rule::pop_build_factory) == 0) {
		return res;
	}

	// check if current projects are already too expensive for capitalists to manage
	float total_cost = est_private_const_spending * days_prepaid;
	float total_cost_added = 0.f;
	float current_inv = n.get_private_investment() * investment_pool_investment_per_day;

	if(current_inv * courage < total_cost + total_cost_added) {
		return res;
	}

	std::vector<dcon::factory_type_id> desired_types;
	desired_types.clear();

	std::vector<dcon::province_id> provinces_in_order;
	provinces_in_order.clear();
	for(auto si : n.get_province_ownership()) {
		if(si.get_province().get_state_membership().get_capital().get_is_colonial() == false) {
			provinces_in_order.push_back(si.get_province().id);
		}
	}

	std::sort(
		provinces_in_order.begin(),
		provinces_in_order.end(),
		[&](dcon::province_id a, dcon::province_id b
			) {
				auto a_pop = state.world.province_get_demographics(a, demographics::total);
				auto b_pop = state.world.province_get_demographics(b, demographics::total);
				if(a_pop != b_pop)
					return a_pop > b_pop;
				return a.index() < b.index(); // force total ordering
	});

	for(auto s : provinces_in_order) {
		auto existing_constructions = state.world.province_get_factory_construction(s);
		if(existing_constructions.begin() != existing_constructions.end())
			continue; // already building

		int32_t num_factories = province_factory_count(state, s);
		auto urbanisation = state.world.province_get_advanced_province_building_max_private_size(s, advanced_province_buildings::list::local_cities_and_towns);

		if(num_factories >= int32_t(state.defines.factories_per_state * urbanisation / economy::factories_per_state_required_city_size)) {
			continue;
		}

		auto sid = state.world.province_get_state_membership(s);
		auto market = state.world.state_instance_get_market_from_local_market(sid);

		// randomly try a valid (check coastal, unlocked, non existing) factory
		desired_types.clear();
		if(craved) {
			ai::get_craved_factory_types(state, n, market, s, desired_types, true);
		} else {
			ai::get_desired_factory_types(state, n, market, s, desired_types, true);
		}

		if(desired_types.empty()) {
			continue;
		}

		auto selected = desired_types[
			rng::reduce(uint32_t(rng::get_random(state, uint32_t((n.id.index() << 6) ^ s.index()))), uint32_t(desired_types.size()))
		];

		if(
			state.world.factory_type_get_is_coastal(selected)
			&& !state.world.province_get_port_to(s)
		)
			continue;

		bool already_in_progress = [&]() {
			for(auto p : state.world.province_get_factory_construction(s)) {
				if(p.get_type() == selected)
					return true;
			}
			return false;
			}();

		if(already_in_progress)
			continue;


		bool present_in_location = false;
		for(auto fac : state.world.province_get_factory_location(s)) {
			auto type = fac.get_factory().get_building_type();
			if(selected == type) {
				present_in_location = true;
				break;
			}
		}

		if(present_in_location) {
			continue;
		}

		potential_target_exists = true;

		auto& costs = state.world.factory_type_get_construction_costs(selected);
		auto time = factory_building_construction_time(state, selected, false);

		float added_cost = 0.0f;
		for(uint32_t i = 0; i < commodity_set::set_size; ++i) {
			if(costs.commodity_type[i]) {
				added_cost +=
					costs.commodity_amounts[i]
					* state.world.market_get_price(market, costs.commodity_type[i])
					/ float(time);
			} else {
				break;
			}
		}
		if((total_cost + total_cost_added > 0.f) && (current_inv * courage < total_cost + total_cost_added + added_cost * days_prepaid)) {
			break;
		}
		total_cost_added += added_cost * days_prepaid;
		res.push_back(economy::full_construction_factory{ added_cost, n, s, true, false, selected });
	}

	return res;
}

std::vector<full_construction_province> estimate_private_investment_province(sys::state& state, dcon::nation_id nid, float est_private_const_spending) {
	auto n = dcon::fatten(state.world, nid);
	auto nation_rules = n.get_combined_issue_rules();

	// check if current projects are already too expensive for capitalists to manage
	float total_cost = est_private_const_spending * days_prepaid * 40.f;
	float total_cost_added = 0.f;
	float current_inv = n.get_private_investment() * investment_pool_investment_per_day;

	std::vector<full_construction_province> res;

	if(current_inv <= total_cost) {
		return res;
	}

	if(!n.get_is_civilized()) {
		return res;
	}

	if((nation_rules & issue_rule::pop_build_factory) == 0) {
		return res;
	}

	static std::vector<std::pair<dcon::province_id, float>> provinces_in_order;
	provinces_in_order.clear();
	for(auto si : n.get_state_ownership()) {
		if(si.get_state().get_capital().get_is_colonial() == false) {
			auto s = si.get_state().id;
			auto d = state.world.state_instance_get_definition(s);
			float num_factories = 0;
			for(auto p : state.world.state_definition_get_abstract_state_membership(d)) {
				if(province::generic_can_build_railroads(state, p.get_province(), n) &&
						p.get_province().get_nation_from_province_ownership() == n) {
					for(auto f : p.get_province().get_factory_location())
						num_factories += get_factory_level(state, f.get_factory());
					provinces_in_order.emplace_back(p.get_province().id, num_factories);
				}
			}
			// The state's number of factories is intentionally given
			// to all the provinces within the state so the
			// railroads aren't just built on a single province within a state
			for(auto p : state.world.state_definition_get_abstract_state_membership(d)) {
				if(province::generic_can_build_railroads(state, p.get_province(), n) &&
						p.get_province().get_nation_from_province_ownership() == n)
					provinces_in_order.emplace_back(p.get_province().id, num_factories);
			}
		}
	}
	if(!provinces_in_order.empty()) {
		std::pair<dcon::province_id, float> best_p = provinces_in_order[0];
		for(auto e : provinces_in_order)
			if(e.second > best_p.second)
				best_p = e;

		auto sid = state.world.province_get_state_membership(best_p.first);
		auto market = state.world.state_instance_get_market_from_local_market(sid);

		auto costs = state.economy_definitions.building_definitions[int32_t(province_building_type::railroad)].cost;
		auto time = state.economy_definitions.building_definitions[int32_t(province_building_type::railroad)].time;
		float added_cost = 0.0f;

		for(uint32_t i = 0; i < commodity_set::set_size; ++i) {
			if(costs.commodity_type[i]) {
				added_cost +=
					costs.commodity_amounts[i]
					* state.world.market_get_price(market, costs.commodity_type[i])
					/ float(time);
			} else {
				break;
			}
		}

		total_cost_added += added_cost * days_prepaid;
		if(n.get_private_investment() * investment_pool_investment_per_day * courage < total_cost + total_cost_added) {
			return res;
		}

		res.push_back({ added_cost, n, best_p.first , true, province_building_type::railroad });
	}

	return res;
}

void run_private_investment(sys::state& state) {
	// make new investments
	for(auto n : state.world.in_nation) {
		auto nation_rules = n.get_combined_issue_rules();

		//we do not want to delete unciv money
		if(n.get_is_civilized()) {
			if(n.get_owned_province_count() > 0 && ((nation_rules & issue_rule::pop_build_factory) != 0 || (nation_rules & issue_rule::pop_expand_factory) != 0)) {
				float est_private_const_spending = estimate_private_construction_spendings(state, n);
				bool stop = false;
				auto craved_constructions = estimate_private_investment_construct(state, n, true, est_private_const_spending, stop);
				for(auto const& r : craved_constructions) {
					if(economy::do_resource_potentials_allow_construction(state, r.nation, r.province, r.type)) {
						auto new_up = fatten(
						state.world,
						state.world.force_create_factory_construction(r.province, r.nation)
						);

						new_up.set_is_pop_project(r.is_pop_project);
						new_up.set_is_upgrade(r.is_upgrade);
						new_up.set_type(r.type);
						const auto& base_cost = state.world.factory_type_get_construction_costs(r.type);

						est_private_const_spending += r.cost;
					}
				}

				if(stop) {
					continue;
				}

				/*
				auto upgrades = estimate_private_investment_upgrade(state, n, est_private_const_spending);

				for(auto const& r : upgrades) {
					auto new_up = fatten(
					state.world,
					state.world.force_create_factory_construction(r.province, r.nation)
					);

					new_up.set_is_pop_project(r.is_pop_project);
					new_up.set_is_upgrade(r.is_upgrade);
					new_up.set_type(r.type);
					est_private_const_spending += r.cost;
				}
				*/

				auto constructions = estimate_private_investment_construct(state, n, false, est_private_const_spending, stop);

				for(auto const& r : constructions) {
					if(economy::do_resource_potentials_allow_construction(state, r.nation, r.province, r.type)) {
						auto new_up = fatten(
						state.world,
						state.world.force_create_factory_construction(r.province, r.nation)
						);

						new_up.set_is_pop_project(r.is_pop_project);
						new_up.set_is_upgrade(r.is_upgrade);
						new_up.set_type(r.type);
						const auto& base_cost = state.world.factory_type_get_construction_costs(r.type);
						est_private_const_spending += r.cost;
					}
				}

				if(stop) {
					continue;
				}

				auto province_constr = estimate_private_investment_province(state, n, est_private_const_spending);

				for(auto const& r : province_constr) {
					auto new_rr = fatten(
						state.world,
						state.world.force_create_province_building_construction(r.province, r.nation)
					);
					const auto& base_cost = state.economy_definitions.building_definitions[uint8_t(r.type)].cost;
					new_rr.set_is_pop_project(r.is_pop_project);
					new_rr.set_type(uint8_t(r.type));
					est_private_const_spending += r.cost;
				}

				// If nowhere to invest
				if(est_private_const_spending < 1.f && craved_constructions.size() == 0 && constructions.size() == 0 && province_constr.size() == 0) {
					// If it's an overlord - prioritize distributing some private invesmtent to subjects
					// If it's a subject - transfer private investment to overlord
					auto rel = state.world.nation_get_overlord_as_subject(n);
					auto overlord = state.world.overlord_get_ruler(rel);

					auto amt = state.world.nation_get_private_investment(n) * state.defines.alice_privateinvestment_subject_transfer / 100.f;
					state.world.nation_set_private_investment(n, state.world.nation_get_private_investment(n) - amt);

					auto subjects = nations::nation_get_subjects(state, n);
					if(subjects.size() > 0) {
						auto part = amt / subjects.size();
						for(auto s : subjects) {
							state.world.nation_set_private_investment(s, state.world.nation_get_private_investment(s) + part);
						}
					} else if(overlord) {
						state.world.nation_set_private_investment(overlord, state.world.nation_get_private_investment(overlord) + amt);
					}
				}
			} else { // private investment not allowed
				state.world.nation_set_private_investment(n, 0.0f);
			}
		}
	}


}


// AWESOME ECONOMY PROFILE TOOLS
 //static auto pf = fopen("performance_record", "w");
// static inline int64_t GetTicks() {
// 	LARGE_INTEGER ticks;
// 	if(!QueryPerformanceCounter(&ticks)) {
// 		std::abort();
// 	}
// 	return ticks.QuadPart;
// }

/*
static auto pf = fopen("money_record", "w");
static int columns = 0;
static int column = 0;
static bool no_more_columns = false;
static float total_history;
static ankerl::unordered_dense::map<std::string, float> total_history_per_label;
static ankerl::unordered_dense::map<std::string, nation_monetary_breakdown> total_history_full_per_label;
static float previous_total;
*/
/*
Funnily enough, this place is great to put logging into because of the passed name.
*/
static void set_profile_point(sys::state& state, std::string name) {
	/*
	nation_monetary_breakdown data{};
	state.world.for_each_nation([&](auto n) {
		auto money_pools = economy::breakdown_nation_monetary_structure(state, n);
		data.total += money_pools.total;
		data.artisans += money_pools.artisans;
		data.bank += money_pools.bank;
		data.educators += money_pools.educators;
		data.factory += money_pools.factory;
		data.investment_pool += money_pools.investment_pool;
		data.landlords += money_pools.landlords;
		data.market += money_pools.market;
		data.nation += money_pools.nation;
		data.pops += money_pools.pops;
		data.ports += money_pools.ports;
		data.rgo += money_pools.rgo;
	});
	auto it = total_history_per_label.find(name);
	if(it == total_history_per_label.end()) {
		if(columns == 0) {
			fprintf(pf, "%s", name.c_str());
		} else {
			fprintf(pf, ";%s", name.c_str());
		}
		total_history_per_label[name] = 0.f;
		columns++;
	} else {
		if(!no_more_columns) {
			fprintf(pf, "\n");
			no_more_columns = true;
		}
		auto diff = data.total - previous_total;
		auto diff_diff = diff - it->second;
		std::string logged_data = name + "\n"
			+ std::to_string(data.total) + ","
			+ std::to_string(data.nation) + ","
			+ std::to_string(data.bank) + ","
			+ std::to_string(data.investment_pool) + ","
			+ std::to_string(data.rgo) + ","
			+ std::to_string(data.factory) + ","
			+ std::to_string(data.market) + ","
			+ std::to_string(data.artisans) + ","
			+ std::to_string(data.ports) + ","
			+ std::to_string(data.landlords) + ","
			+ std::to_string(data.educators) + ","
			+ std::to_string(data.pops) + "\n"
			+ std::to_string(int(diff_diff)) + "\n";
			//+ "RGO sales: "+ std::to_string(int(data.rgo_sales)) + "\n"
			//+ "RGO wages: "+ std::to_string(int(data.rgo_wages)) + "\n"
			//+ "POP wages: "+ std::to_string(int(data.pops_wages)) + "\n"
			//+ "POP purchase goods: "+ std::to_string(int(data.pops_spending_life + data.pops_spending_everyday + data.pops_spending_luxury)) + "\n"
			//+ "POP education: " + std::to_string(int(data.pops_cashback_education)) + "/" + std::to_string(int(data.pops_spending_education)) + "\n"
			//+ "POP housing: " + std::to_string(int(data.pops_cashback_housing)) + "/" + std::to_string(int(data.pops_spending_housing)) + "\n"
		;
		if(abs(diff_diff) > 0.5f) {
			//state.console_log(logged_data);
		} else {
			//state.console_log(name + " OK!");
		}
		previous_total = data.total;
		total_history_per_label[name] = diff;

		column = (column + 1) % columns;
		if(column == 0) {
			fprintf(pf, "%f\n", data.total);
		} else {
			fprintf(pf, "%f;", data.total);
		}
	}
	
	*/
	//total_history = data.total;

	/*
	ve::fp_vector total_nations {};
	ve::fp_vector total_markets {};
	ve::fp_vector total_pops{};
	state.world.execute_serial_over_pop([&](auto ids) {
		total_pops = total_pops + state.world.pop_get_savings(ids);
	});
	state.world.execute_serial_over_market([&](auto ids) {
		total_markets = total_markets + state.world.market_get_stockpile(ids, money);
	});
	state.world.execute_serial_over_nation([&](auto ids) {
		total_nations = total_nations + state.world.nation_get_stockpiles(ids, money);
	});
	auto total = total_nations + total_markets + total_pops;
	auto diff = total.reduce() - total_history;
	total_history = total.reduce();
	
	std::string logged_data = name + "\n" + std::to_string(total.reduce()) + "," + std::to_string(total_pops.reduce()) + ","  + std::to_string(total_markets.reduce()) + "," + std::to_string(total_nations.reduce())  + "\n" + std::to_string(int(diff / 1000.f)) + "\n";
	state.console_log(logged_data);
	*/


	//printf("%f,%f,%f\n", total_pops.reduce(), total_markets.reduce(), total_nations.reduce());

	// fprintf(pf, (name + ",%llu\n").c_str(), GetTicks());
}

void decay_government_stockpiles(sys::state& state) {
	state.world.execute_serial_over_market([&](auto markets) {
		auto states = state.world.market_get_zone_from_local_market(markets);
		auto capitals = state.world.state_instance_get_capital(states);
		auto controllers = state.world.state_instance_get_nation_from_state_control(states);
		auto capital_control = state.world.province_get_control_ratio(capitals);
		for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
			auto current_stockpiles = state.world.market_get_government_stockpile(markets, com_id);
			auto decay = current_stockpiles * government_stockpile_spoilage * (2.0f - capital_control);
			auto new_stockpiles = current_stockpiles - decay;
			ve::apply([&](dcon::nation_id controller, dcon::market_id market, float decay_amount) {
				if(controller) {
					subtract_government_stockpile(state, controller, market, com_id, decay_amount);
				}
				else {
					subtract_rebel_stockpile(state, market, com_id, decay_amount);
				}
			}, controllers, markets, decay);
		});
	});
}

//template<uint32_t batch, uint32_t batch_count, typename F>
//RELEASE_INLINE void execute_batch_over_pop(sys::state& state, F&& f) {
	//execute_batch<dcon::pop_id, batch, batch_count>(state.world.pop_size(), f);
//}

void batched_daily_update(sys::state& state, bool presimulation, float presimulation_stage) {
	static bool do_work = false;


	uint32_t total_commodities = state.world.commodity_size();

	static economy_update_stage update_stage = economy_update_stage::stage_1;

	sanity_check(state);

	set_profile_point(state, "start");

	// per nation buffers
	static auto current_stockpile_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_stockpile_total_cost = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto current_budget_navy = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_total_price_navy = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(1);
	static ve::vectorizable_buffer<float, dcon::nation_id> invention_count = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	// per market buffers
	static auto export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	static auto import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	static auto port_availability = state.world.market_make_vectorizable_float_buffer();
	static auto price_port_capacity = state.world.market_make_vectorizable_float_buffer();
	static auto port_total_weight = state.world.market_make_vectorizable_float_buffer();
	// per state instance buffers
	static auto coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
	static auto state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
	// per pop buffers
	static ve::vectorizable_buffer<float, dcon::pop_id> income_buffer(uint32_t(1));


	auto weights_buffer_get = [&](auto nation, dcon::commodity_id com_id) {
		return state.world.nation_get_commodity_float_buffer_1(nation, com_id);
	};
	auto weights_buffer_set = [&](auto nation, dcon::commodity_id com_id, auto val) {
		state.world.nation_set_commodity_float_buffer_1(nation, com_id, val);
	};

	auto batch_work = [&]<uint32_t batch, uint32_t batch_count>(sys::state& state) {
		switch(update_stage) {
		case economy_update_stage::stage_1:
		{
			auto nation_size = state.world.nation_size();
			execute_batch<dcon::nation_id, batch, batch_count>(nation_size, [&](auto nations) {
				for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
					auto to_apply = state.world.nation_get_total_stockpiles(nations, com_id);
					state.world.nation_set_yesterday_total_stockpiles(nations, com_id, to_apply);
					state.world.nation_set_commodity_float_buffer_1(nations, com_id, ve::fp_vector{ 0.0f });
				});
				state.world.nation_set_subsidy_token_total(nations, 0.f);
				auto treasury = state.world.nation_get_treasury(nations);
				state.world.nation_set_last_treasury(nations, treasury);

				auto base_budget = state.world.nation_get_last_base_budget(nations);
				auto stockpiles_priority = ve::to_float(state.world.nation_get_stockpile_spending(nations)) / 100.f;
				expected_stockpile_total_cost.set(nations, 0.0f);
				current_stockpile_budget.set(nations, ve::max(0.f, base_budget * stockpiles_priority));
				invention_count.set(nations, 0.f);
				state.world.for_each_invention([&](auto iid) {
					auto has_inv = ve::select(
						state.world.nation_get_active_inventions(nations, iid),
						ve::fp_vector(1.0f),
						ve::fp_vector(0.0f)
					);
					auto count = invention_count.get(nations) + has_inv;
					invention_count.set(nations, count);
				});
				ve::apply([&](auto nation) {
					market_leader.set(nation, nations::get_market_leader(state, nation));
				}, nations);
			}); 
			execute_batch<dcon::market_id, batch, batch_count>(state.world.market_size(), [&](auto market) {
				state.world.for_each_pop_type([&](dcon::pop_type_id t) {
					state.world.market_set_life_needs_costs(market, t, ve::fp_vector{});
					state.world.market_set_everyday_needs_costs(market, t, ve::fp_vector{});
					state.world.market_set_luxury_needs_costs(market, t, ve::fp_vector{});
				});
				economy::for_each_commodity_no_money(state, [&](dcon::commodity_id cid) {
					state.world.market_set_private_construction_demand(market, cid, 0.0f);
				});
				auto sid = state.world.market_get_zone_from_local_market(market);
				auto nids = state.world.state_instance_get_nation_from_state_ownership(sid);
				ve::apply([&](auto nid, auto mid) {
					import_tariff_buffer.set(mid, effective_tariff_import_rate(state, nid, mid));
					export_tariff_buffer.set(mid, effective_tariff_export_rate(state, nid, mid));
				}, nids, market);
			});

			for_each_market_province_parallel_over_market_batched<batch, batch_count>(state, [&](auto mid, auto sid, dcon::province_id pid) {
				state.world.province_for_each_province_building_construction(pid, [&](auto c) {
					populate_province_building_construction_private_demand(state, c);
				});
				state.world.province_for_each_factory_construction(pid, [&](auto c) {
					populate_state_construction_private_demand(state, c);
				});

				auto price = state.world.province_get_service_price(pid, services::list::port_capacity);
				auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
				auto local_weight = size / (price_properties::service::epsilon + price);
				port_total_weight.set(mid, port_total_weight.get(mid) + local_weight);
			});

			for_each_market_province_parallel_over_market_batched<batch, batch_count>(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
				auto price = state.world.province_get_service_price(pid, services::list::port_capacity);
				auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
				auto local_weight = size / (price_properties::service::epsilon + price);
				auto total_weight = port_total_weight.get(mid);
				auto local_demand_ratio = local_weight / total_weight;
				auto sat = state.world.province_get_service_satisfaction(pid, services::list::port_capacity);
				port_availability.set(mid, port_availability.get(mid) + local_demand_ratio * sat);
				price_port_capacity.set(mid, price_port_capacity.get(mid) + local_demand_ratio * price);
			});

			execute_batch<dcon::province_id, batch, batch_count>(state.province_definitions.first_sea_province.index(), [&](auto ids) {
				auto local_states = state.world.province_get_state_membership(ids);
				auto weight_population =
					state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.farmers))
					+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.laborers));
				auto weight_capitalists = weight_population / 50.f
					+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.capitalists)) * 200.f;
				auto weight_aristocracy = weight_population / 50.f
					+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.aristocrat)) * 200.f
					+ state.world.state_instance_get_demographics(local_states, demographics::to_key(state, state.culture_definitions.slaves));
				auto total = weight_aristocracy + weight_capitalists + weight_population + 1.0f;
				state.world.province_set_landowners_share(ids, weight_aristocracy / total);
				state.world.province_set_capitalists_share(ids, weight_capitalists / total);
				auto quality = (ve::to_float(state.world.province_get_life_rating(ids)) - 10.f) / 10.f;
				quality = ve::max(quality, 0.f) + 0.01f;
				auto score = (subsistence_factor * quality) + subsistence_score_life * 0.9f;
				state.world.province_set_subsistence_score(ids, score);
			});
			// First, set the "raw" weight per market
			execute_batch<dcon::market_id, batch, batch_count>(state.world.market_size(), [&](auto markets) {
				auto states = state.world.market_get_zone_from_local_market(markets);
				auto controllers = state.world.state_instance_get_nation_from_state_control(states);
				auto controller_valid = (controllers != dcon::nation_id{ } && nations::exists(state, controllers));
				economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
					auto sat_weight = state.world.market_get_expected_probability_to_buy(markets, commodity) + 0.001f;
					auto price_weight = 1.0f / (state.world.market_get_price(markets, commodity) + 0.001f);
					auto raw_weight = state.world.market_get_supply(markets, commodity) * price_weight * sat_weight;
					state.world.market_set_government_stockpile_demand_weights(markets, commodity, ve::select(controller_valid, raw_weight, 0.0f));
				});
			});

			execute_batch<dcon::state_instance_id, batch, batch_count>(state.world.state_instance_size(), [&](auto sids) {
				auto coastal_capitals = ve::apply([&](auto sid) {
					return province::state_get_coastal_capital(state, sid);
				}, sids);
				coastal_capital_buffer.set(sids, coastal_capitals);
				auto capital = state.world.state_instance_get_capital(sids);
				auto controller_capital = state.world.province_get_nation_from_province_control(capital);
				auto controller_port = state.world.province_get_nation_from_province_control(coastal_capitals);
				auto occupied = ve::apply([&](auto capital_nation, auto port_nation) {
					return military::are_at_war(state, capital_nation, port_nation);
				}, controller_capital, controller_port);
				state_naval_trade_is_blockaded.set(sids,
					ve::select(
						coastal_capitals == dcon::province_id{}
						|| occupied
						|| state.world.province_get_is_blockaded(coastal_capitals),
						ve::fp_vector{ 1.f },
						ve::fp_vector{ 0.f }
					)
				);
			});

			execute_batch<dcon::factory_id, batch, batch_count>(state.world.factory_size(), [&](auto factories) {
				ve::apply([&](dcon::factory_id factory){
					auto fac_type = fatten(state.world, state.world.factory_get_building_type(factory));
					float sum = 0.0f;
					auto prov = state.world.factory_get_province_from_factory_location(factory);
					auto pstate = state.world.province_get_state_membership(prov);
					auto powner = state.world.province_get_nation_from_province_ownership(prov);
					if(powner && pstate) {
						for(auto bonus : fac_type.get_factory_bonuses()) {
							if(bonus.trigger && trigger::evaluate(state, bonus.trigger, trigger::to_generic(pstate), trigger::to_generic(powner), 0)) {
								sum -= bonus.amount;
							}
						}
					}
					state.world.factory_set_triggered_modifiers(factory, sum);
				}, factories);
			});

			for_each_nation_owned_province_parallel_over_nation_batched<batch, batch_count>(state, [&](dcon::nation_id nation, dcon::province_id province) {
				auto area = state.world.province_get_state_membership(province);
				state.world.province_for_each_factory_location(province, [&](dcon::factory_location_id factory_location) {
					auto factory = state.world.factory_location_get_factory(factory_location);
					auto building_type = state.world.factory_get_building_type(factory);
					auto output_type = state.world.factory_type_get_output(building_type);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, output_type));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, output_type));
					if(priority || priority_local) {
						auto base_output = state.world.factory_type_get_output_amount(building_type);
						auto factory_output = state.world.factory_get_output(factory);
						auto effective_output = factory_output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_factory_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_factory_bank(province, current_money + last_token_price * effective_output);
					}
				});

				state.world.for_each_commodity([&](auto cid) {
					auto base_output = state.world.commodity_get_artisan_output_amount(cid);
					if(base_output == 0.f) return;
					auto area = state.world.province_get_state_membership(province);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));
					if(priority || priority_local) {
						auto output = state.world.province_get_artisan_actual_production(province, cid);
						auto effective_output = output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_artisan_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_artisan_bank(province, current_money + last_token_price * effective_output);
					}
				});

				state.world.for_each_commodity([&](auto cid) {
					auto base_output = state.world.commodity_get_rgo_amount(cid);
					if(base_output == 0.f) return;

					auto area = state.world.province_get_state_membership(province);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));

					if(priority || priority_local) {
						auto rgo_output = state.world.province_get_rgo_output(province, cid);
						auto effective_output = rgo_output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_rgo_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_rgo_bank(province, current_money + last_token_price * effective_output);
					}
				});
			});
			break;
		}
		case economy_update_stage::stage_2:
		{
			for_each_controlled_market_parallel_over_nation_batch<batch, batch_count>(state, [&](dcon::nation_id controller, dcon::state_instance_id sid, dcon::market_id market) {
				economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
					float amount = state.world.market_get_government_stockpile_demand_weights(market, commodity);
					weights_buffer_set(controller, commodity, weights_buffer_get(controller, commodity) + amount);
				});
			});
			execute_batch<dcon::trade_route_id, batch, batch_count>(state.world.trade_route_size(), [&](auto trade_route) {
				auto origin = state.world.trade_route_get_origin(trade_route);
				auto target = state.world.trade_route_get_target(trade_route);

				auto owner = state.world.trade_route_get_owner(trade_route);
				auto budget = 1.f + state.world.market_get_total_earn(target);

				auto s_origin = state.world.market_get_zone_from_local_market(origin);
				auto s_target = state.world.market_get_zone_from_local_market(target);

				auto capital_origin = state.world.state_instance_get_capital(s_origin);
				auto capital_target = state.world.state_instance_get_capital(s_target);

				auto controller_origin = state.world.province_get_nation_from_province_control(capital_origin);
				auto controller_target = state.world.province_get_nation_from_province_control(capital_target);

				auto formal_owner_origin = state.world.province_get_nation_from_province_ownership(capital_origin);
				auto formal_owner_target = state.world.province_get_nation_from_province_ownership(capital_target);

				controller_origin = ve::select(controller_origin != dcon::nation_id{ }, controller_origin, formal_owner_origin);
				controller_target = ve::select(controller_target != dcon::nation_id{ }, controller_target, formal_owner_target);

				auto origin_tariff_is_applied = state.world.trade_route_get_is_tariff_applied_origin(trade_route);
				auto target_tariff_is_applied = state.world.trade_route_get_is_tariff_applied_target(trade_route);

				ve::mask_vector is_origin_blockaded = state_naval_trade_is_blockaded.get(s_origin) > 0.f;
				ve::mask_vector is_target_blockaded = state_naval_trade_is_blockaded.get(s_target) > 0.f;

				auto trade_closed = state.world.trade_route_get_is_trade_forbidden(trade_route);

				auto is_origin_civ = state.world.nation_get_is_civilized(controller_origin);
				auto is_target_civ = state.world.nation_get_is_civilized(controller_target);
				auto is_sea_route = state.world.trade_route_get_is_sea_route(trade_route);
				auto blockaded = is_sea_route && (is_origin_blockaded || is_target_blockaded);
				auto same_nation = controller_origin == controller_target;

				// US3AC7 US3AC8 Ban international sea routes or international land routes based on the corresponding modifiers
				auto origin_bans_sea_trade = state.world.nation_get_modifier_values(controller_origin, sys::national_mod_offsets::disallow_naval_trade) > 0.f;
				auto target_bans_sea_trade = state.world.nation_get_modifier_values(controller_target, sys::national_mod_offsets::disallow_naval_trade) > 0.f;
				auto sea_trade_banned = origin_bans_sea_trade || target_bans_sea_trade;
				auto origin_bans_land_trade = state.world.nation_get_modifier_values(controller_origin, sys::national_mod_offsets::disallow_land_trade) > 0.f;
				auto target_bans_land_trade = state.world.nation_get_modifier_values(controller_target, sys::national_mod_offsets::disallow_land_trade) > 0.f;
				auto land_trade_banned = origin_bans_land_trade || target_bans_land_trade;
				auto trade_banned = (is_sea_route && sea_trade_banned && !same_nation) || (!is_sea_route && land_trade_banned && !same_nation);
				auto merchant_cut = ve::select(same_nation, ve::fp_vector{ economy::merchant_cut_domestic }, ve::fp_vector{ economy::merchant_cut_foreign });
				auto export_tariff = ve::select(same_nation || !origin_tariff_is_applied, ve::fp_vector{ 0.f }, export_tariff_buffer.get(origin));
				auto import_tariff = ve::select(same_nation || !target_tariff_is_applied, ve::fp_vector{ 0.f }, import_tariff_buffer.get(target));
				ve::fp_vector distance = state.world.trade_route_get_distance_km(trade_route);
				// US3AC18
				auto trade_good_loss_mult = ve::max(0.f, 1.f - trade_goods_lost_per_km * distance);
				auto transport_cost = distance * ve::select(is_sea_route, state.world.market_get_naval_transportation_price(owner), state.world.market_get_land_transportation_price(owner));

				auto reset_route =
					trade_closed || blockaded || trade_banned
					|| !ve::apply([&](auto r) { return state.world.trade_route_is_valid(r); }, trade_route);

				for(auto c : state.world.in_commodity) {
					// US3AC19
					if(state.world.commodity_get_money_rgo(c) || state.world.commodity_get_is_local(c)) {
						continue;
					}
					if(
						state.world.commodity_get_rgo_amount(c) > 0.f
						&& !state.world.commodity_get_actually_exists_in_nature(c)
					) {
						continue;
					}
					auto reset_route_commodity = reset_route;
					auto next = trade_route_compute_next_volume<true, ve::fp_vector>(
						state, reset_route, is_sea_route,
						trade_route, c,
						export_tariff, import_tariff, transport_cost, budget, trade_good_loss_mult, merchant_cut
					);
					state.world.trade_route_set_volume(trade_route, c, next);
				}
			});
			break;
		}
		case economy_update_stage::stage_3:
		{
			// Lastly, set percentage weights, so that a nations' controlled markets' weights all add up to 1.0f.
			execute_batch<dcon::market_id, batch, batch_count>(state.world.market_size(), [&](auto markets) {
				auto states = state.world.market_get_zone_from_local_market(markets);
				auto controllers = state.world.state_instance_get_nation_from_state_control(states);
				auto controller_valid = (controllers != dcon::nation_id{ } && nations::exists(state, controllers));
				ve::fp_vector state_count = ve::apply([&](dcon::nation_id nation) {
					return float(state.world.nation_get_state_control(nation).end() - state.world.nation_get_state_control(nation).begin());
				}, controllers);
				economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
					auto total_nation_weights = weights_buffer_get(controllers, commodity);
					auto controller_has_weight = (controller_valid && total_nation_weights > 0.0f);
					auto raw_weight = state.world.market_get_government_stockpile_demand_weights(markets, commodity);
					auto equally_split_demand = 1.0f / state_count;
					auto percentage_weight = ve::select(controller_has_weight, raw_weight / total_nation_weights, equally_split_demand);
					percentage_weight = ve::select(controller_valid, percentage_weight, 0.0f);
					state.world.market_set_government_stockpile_demand_weights(markets, commodity, percentage_weight);
				});
			});
			break;
		}
		case economy_update_stage::stage_4:
		{
			if constexpr(batch == 0) {
				populate_army_consumption(state);
			}
			if constexpr(batch == 1) {
				populate_navy_consumption(state);
			}
			if constexpr(batch == 2) {
				populate_government_construction_consumption(state);
			}
			if constexpr(batch == 3) {
				populate_government_stockpile_demand(state, expected_stockpile_total_cost, current_stockpile_budget);
				decay_government_stockpiles(state);
			}
			break;
		}
		case economy_update_stage::stage_5:
		{

			break;
		}
		case economy_update_stage::stage_6:
		{

			break;
		}
		case economy_update_stage::stage_7:
		{

			break;
		}
		case economy_update_stage::stage_8:
		{

			break;
		}
		}
	};


	constexpr uint32_t workers = 7;
	static std::atomic<bool> work_done[workers] {};
	static bool work_requested[workers] {};

	for(uint32_t i = 0; i < workers; i++) {
		work_done[i].store(true);
		work_requested[i] = false;
	}
	auto work_is_not_done = [&]() {
		for(uint32_t i = 0; i < workers; i++) {
			if(work_done[i].load(std::memory_order::acquire) == false) {
				return true;
			}
		}
		return false;
	};
	auto request_work = [&]() {
		for(uint32_t i = 0; i < workers; i++) {
			work_done[i].store(false, std::memory_order::release);
			//work_requested[i].store(true, std::memory_order::release);
			work_requested[i] = true;
		}
	};
	auto do_nothing = [&]() {
		std::this_thread::sleep_for(std::chrono::microseconds{ 1 });
	};

	auto batch_update = [&]<uint32_t batch, uint32_t batch_count>(sys::state& state){
		while(true) {
			if(!do_work) {
				std::this_thread::sleep_for(std::chrono::microseconds{ 2 });
				if(state.quit_signaled.load(std::memory_order::acquire) == true) {
					break;
				}
			}
			//auto requested = work_requested[batch].exchange(false, std::memory_order::acq_rel);
			if(!work_requested[batch]) {
				continue;
			}
			batch_work.template operator()<batch, batch_count>(state);
			work_requested[batch] = false;
			work_done[batch].store(true, std::memory_order_release);
		}
	};


	static bool threads_running = false;
	if(!threads_running) {
		static std::thread update_thread_0([&]() {batch_update.template operator() < 0, workers > (state); });
		static std::thread update_thread_1([&]() {batch_update.template operator() < 1, workers > (state); });
		static std::thread update_thread_2([&]() {batch_update.template operator() < 2, workers > (state); });
		static std::thread update_thread_3([&]() {batch_update.template operator() < 3, workers > (state); });
		static std::thread update_thread_4([&]() {batch_update.template operator() < 4, workers > (state); });
		static std::thread update_thread_5([&]() {batch_update.template operator() < 5, workers > (state); });
		update_thread_0.detach();
		update_thread_1.detach();
		update_thread_2.detach();
		update_thread_3.detach();
		update_thread_4.detach();
		update_thread_5.detach();
		threads_running = true;
	}

	auto set_stage = [&](economy_update_stage stage) {
		update_stage = stage;
		request_work();
		int i = 0;
		constexpr uint32_t last_batch = workers - 1;
		//work_requested[last_batch].exchange(false, std::memory_order::acq_rel);
		work_requested[last_batch] = false;
		batch_work.template operator()<last_batch, workers>(state);
		work_done[last_batch].store(true, std::memory_order_release);
		while(work_is_not_done()) {
			//do_nothing();
			i += 1;
		}
	};

	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if(new_count > old_count) {
			current_stockpile_budget = state.world.nation_make_vectorizable_float_buffer();
			expected_stockpile_total_cost = state.world.nation_make_vectorizable_float_buffer();
			current_budget_navy = state.world.nation_make_vectorizable_float_buffer();
			expected_total_price_navy = state.world.nation_make_vectorizable_float_buffer();
			market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(new_count);
			invention_count = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}

	{
		static uint32_t old_count = 1;
		auto new_count = state.world.market_size();
		if(new_count > old_count) {
			export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			port_availability = state.world.market_make_vectorizable_float_buffer();
			price_port_capacity = state.world.market_make_vectorizable_float_buffer();
			port_total_weight = state.world.market_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}

	{
		static uint32_t old_count = 1;
		auto new_count = state.world.state_instance_size();
		if(new_count > old_count) {
			coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
			state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
			old_count = new_count;
		}
	}

	{
		static uint32_t old_count = 1;
		auto new_count = state.world.pop_size();
		if(new_count > old_count) {
			income_buffer = state.world.pop_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	do_work = true;
	//set_stage(economy_update_stage::reset_buffer_size);

	//do_work = false;

	if(presimulation) {
		state.world.execute_serial_over_province([&](auto pid) {
			state.world.province_set_artisan_bank(pid, 0.f);
			state.world.province_set_factory_bank(pid, 0.f);
		});
	}
	if(state.trade_route_cached_values_out_of_date) {
		state.trade_route_cached_values_out_of_date = false;

		ankerl::unordered_dense::map<int32_t, bool> direct_block;
		ankerl::unordered_dense::map<int32_t, bool> trade_closed;
		ankerl::unordered_dense::map<int32_t, bool> direct_no_tariffs;
		ankerl::unordered_dense::map<int32_t, bool> no_tariffs;

		// US3AC9. Wartime embargoes

		state.world.for_each_war([&](auto war) {
			state.world.war_for_each_war_participant(war, [&](auto attacker_candidate) {
				if(!state.world.war_participant_get_is_attacker(attacker_candidate)) return;
				auto attacker = state.world.war_participant_get_nation(attacker_candidate);

				state.world.war_for_each_war_participant(war, [&](auto defender_candidate) {
					if(state.world.war_participant_get_is_attacker(defender_candidate)) return;
					auto defender = state.world.war_participant_get_nation(defender_candidate);

					auto index_pair = attacker.index() * state.world.nation_size() + defender.index();
					auto index_pair_T = defender.index() * state.world.nation_size() + attacker.index();
					direct_block[index_pair] = true;
					direct_block[index_pair_T] = true;
				});
			});
		});

		// US3AC10. diplomatic embargos
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_embargo(rel)) {
				dcon::nation_id source = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id target = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = source.index() * state.world.nation_size() + target.index();
				auto index_pair_T = target.index() * state.world.nation_size() + source.index();

				direct_block[index_pair] = true;
				direct_block[index_pair_T] = true;
			}
		});

		// US3AC11. US3AC12. sphere joins market leader
		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);
				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_block.find(indices[0]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_block.find(indices[1]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_block.find(indices[2]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}

				{
					if(direct_block.find(base_pair) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
			});
		});

		// US3AC15. Equal/unequal trade treaties
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_no_tariffs_until(rel)) {
				dcon::nation_id n1 = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id n2 = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = n1.index() * state.world.nation_size() + n2.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		// Reflexivity of free trade
		state.world.for_each_nation([&](auto nid) {
			auto index_pair = nid.index() * state.world.nation_size() + nid.index();
			direct_no_tariffs[index_pair] = true;
		});
		state.world.for_each_nation([&](auto nid) {
			dcon::nation_id sphere = state.world.nation_get_in_sphere_of(nid);
			if(sphere) {
				auto index_pair = nid.index() * state.world.nation_size() + sphere.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		state.world.for_each_overlord([&](auto ovid) {
			dcon::nation_id subject = state.world.overlord_get_subject(ovid);
			dcon::nation_id overlord = state.world.overlord_get_ruler(ovid);
			auto index_pair = subject.index() * state.world.nation_size() + overlord.index();
			direct_no_tariffs[index_pair] = true;
		});

		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);

				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_no_tariffs.find(indices[0]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_no_tariffs.find(indices[1]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_no_tariffs.find(indices[2]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				{
					if(direct_no_tariffs.find(base_pair) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
			});
		});

		// update cache:

		state.world.for_each_trade_route([&](auto route) {
			auto A = state.world.trade_route_get_origin(route);
			auto B = state.world.trade_route_get_target(route);
			auto s_A = state.world.market_get_zone_from_local_market(A);
			auto s_B = state.world.market_get_zone_from_local_market(B);

			auto capital_A = state.world.state_instance_get_capital(s_A);
			auto capital_B = state.world.state_instance_get_capital(s_B);

			auto controller_A = state.world.province_get_nation_from_province_control(capital_A);
			auto controller_B = state.world.province_get_nation_from_province_control(capital_B);

			auto index = controller_A.index() * state.world.nation_size() + controller_B.index();
			auto index_T = controller_B.index() * state.world.nation_size() + controller_A.index();

			if(trade_closed.find(index) != trade_closed.end()) {
				state.world.trade_route_set_is_trade_forbidden(route, true);
			} else {
				state.world.trade_route_set_is_trade_forbidden(route, false);
			}

			if(no_tariffs.find(index) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_origin(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_origin(route, true);
			}

			if(no_tariffs.find(index_T) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_target(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_target(route, true);
			}
		});
	};

	set_stage(economy_update_stage::stage_1);

	/*
	do_work = false;
	update_trade_routes_volume(
		state,
		presimulation ? true : false,
		export_tariff_buffer,
		import_tariff_buffer,
		coastal_capital_buffer,
		state_naval_trade_is_blockaded
	);
	do_work = true;
	*/

	set_stage(economy_update_stage::stage_2);
	set_stage(economy_update_stage::stage_3);
	set_stage(economy_update_stage::stage_4);
	//set_stage(economy_update_stage::stage_5);
	//set_stage(economy_update_stage::stage_6);
	//set_stage(economy_update_stage::stage_7);
	//set_stage(economy_update_stage::stage_8);
}

inline float investment_expand_factory_priority(
	const sys::state& state,
	dcon::factory_id factory
) {
	auto size = state.world.factory_get_size(factory);
	return std::clamp((factory_total_desired_employment(state, factory) - 0.8f * size) * 5.f / (size + 1.f), 0.f, 1.f);
}


constexpr float sales_optimism = 0.05f;
constexpr float purchase_optimism = 0.2f;

template<typename T>
auto artisan_input_multiplier_2(
	const sys::state& state,
	T nations
) {
	auto alice_input_base = state.defines.alice_inputs_base_factor_artisans;
	auto nation_input_mod = 1.f + state.world.nation_get_modifier_values(
		nations, sys::national_mod_offsets::artisan_input);

	if constexpr(std::is_same_v<T, dcon::nation_id>) {
		return std::max(alice_input_base * nation_input_mod, 0.01f);
	} else {
		return ve::max(alice_input_base * nation_input_mod, 0.01f);
	}
}
template<typename T>
float ve_artisan_min_wage_2(sys::state& state, T markets) {
	auto life = state.world.market_get_life_needs_costs(markets, state.culture_definitions.artisans);
	auto everyday = state.world.market_get_everyday_needs_costs(markets, state.culture_definitions.artisans);
	return (life + everyday) * 0.001f;
}
template<typename T>
auto artisan_output_multiplier_2(
	const sys::state& state,
	T nations
) {
	auto alice_output_base = state.defines.alice_output_base_factor_artisans;
	auto nation_output_mod = 1.f + state.world.nation_get_modifier_values(
		nations, sys::national_mod_offsets::artisan_output);

	if constexpr(std::is_same_v<T, dcon::nation_id>) {
		return std::max(alice_output_base * nation_output_mod, 0.01f);
	} else {
		return ve::max(alice_output_base * nation_output_mod, 0.01f);
	}
}
template<typename T>
auto artisan_throughput_multiplier_2(
	sys::state& state,
	T nations
) {
	if constexpr(std::is_same_v<T, dcon::nation_id>) {
		return std::max(
			0.01f,
			1.0f
			+ state.world.nation_get_modifier_values(nations, sys::national_mod_offsets::artisan_throughput)
		);
	} else {
		return ve::max(
			0.01f,
			1.0f
			+ state.world.nation_get_modifier_values(nations, sys::national_mod_offsets::artisan_throughput)
		);
	}
}

constexpr float factory_priority_bonus = 10.f;

float factory_investment_tokens_2(
	const sys::state& state,
	dcon::nation_id nation,
	dcon::province_id province,
	dcon::factory_id factory
) {
	auto area = state.world.province_get_state_membership(province);
	auto market = state.world.state_instance_get_market_from_local_market(area);

	auto factory_type = state.world.factory_get_building_type(factory);
	auto output_type = state.world.factory_type_get_output(factory_type);
	auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, output_type));
	auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, output_type));
	auto subsidy = 0.f;

	auto size = state.world.factory_get_size(factory);
	if(priority || priority_local) {
		auto base_output = state.world.factory_type_get_output_amount(factory_type);
		auto factory_output = state.world.factory_get_output(factory);
		auto effective_output = factory_output / base_output;
		auto tokens = state.world.nation_get_subsidy_token_total(nation);
		auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
		subsidy = last_token_price * effective_output / (size + 1.f);
	}

	auto time = state.world.factory_type_get_construction_time(factory_type);
	auto per_worker_output_cost = state.world.factory_get_output_per_worker(factory) * state.world.market_get_price(market, output_type);
	auto per_worker_input_cost = state.world.factory_get_input_cost_per_worker(factory);
	auto profit = per_worker_output_cost;

	return  factory_priority_bonus * (profit + subsidy);
}

float rgo_investment_tokens_2(
	const sys::state& state,
	dcon::nation_id nation,
	dcon::province_id province,
	dcon::commodity_id commodity
) {
	auto area = state.world.province_get_state_membership(province);
	auto market = state.world.state_instance_get_market_from_local_market(area);
	auto base_output = state.world.commodity_get_rgo_amount(commodity);
	if(base_output == 0.f) return 0.f;
	auto current_max_size = state.world.province_get_rgo_potential(province, commodity);
	if(current_max_size == 0.f) return 0.f;
	auto current_size = state.world.province_get_rgo_size(province, commodity);
	auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, commodity));
	auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, commodity));
	auto subsidy_tokens = 0.f;
	if(priority || priority_local) {
		auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
		subsidy_tokens = last_token_price / state.defines.alice_rgo_per_size_employment;
	}
	auto output_tokens = state.world.commodity_get_rgo_amount(commodity)
		* state.world.province_get_rgo_base_efficiency(province, commodity)
		* state.world.market_get_price(market, commodity)
		/ state.defines.alice_rgo_per_size_employment;
	auto base_tokens = 1.f / state.defines.alice_rgo_per_size_employment;
	auto local_tokens = subsidy_tokens + output_tokens + base_tokens;
	return local_tokens;
}

struct preconsumption_data_2 {
	inputs_data direct_inputs_data{ };

	float output_price = 0.f;
	float output_amount_per_production_unit = 0.f;
	float direct_inputs_cost_per_production_unit = 0.f;
	float direct_inputs_expected_cost_per_production_unit = 0.f;
	float direct_inputs_cost_per_production_unit_availability_adjusted = 0.f;
};

preconsumption_data_2 prepare_data_for_consumption_2(
	sys::state& state,
	dcon::market_id market,
	economy::commodity_set const& inputs,
	dcon::commodity_id output,
	float output_amount,

	float input_multiplier, float output_multiplier
) {
	auto direct_inputs_data = get_inputs_data(state, market, inputs);
	assert(direct_inputs_data.min_available >= 0.f);

	auto output_price = state.world.market_get_price(market, output);

	auto output_per_production_unit =
		output_amount
		* output_multiplier;

	float output_cost_per_production_unit =
		output_per_production_unit
		* output_price;

	preconsumption_data_2 result = {
		.direct_inputs_data = direct_inputs_data,
		.output_price = output_price,
		.output_amount_per_production_unit = output_per_production_unit,
		.direct_inputs_cost_per_production_unit = direct_inputs_data.total_cost * input_multiplier,
		.direct_inputs_expected_cost_per_production_unit = direct_inputs_data.total_expected_cost * input_multiplier,
		.direct_inputs_cost_per_production_unit_availability_adjusted = direct_inputs_data.total_cost_availability_adjusted * input_multiplier,
	};

	return result;
}

struct consumption_data_2 {
	float direct_inputs_cost;
	float direct_inputs_expected_cost;
	float output;

	float direct_inputs_scale;

	float input_cost_per_employment_unit;
	float input_expected_cost_per_employment_unit;
	float output_per_employment_unit;
};

template<typename PROV, typename SET, typename VALUE>
void save_inputs_to_buffers_2(
	sys::state& state,
	PROV provs,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_demanded,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_consumed,
	SET const& inputs,
	VALUE scale,
	VALUE min_available
) {
	for(uint32_t i = 0; i < SET::set_size; ++i) {
		if(inputs.commodity_type[i]) {
			auto b_index = inputs.commodity_type[i].index();
			auto c = inputs.commodity_type[i];
			buffer_demanded.set(c, buffer_demanded.get(c) + scale * inputs.commodity_amounts[i]);
			buffer_consumed.set(c, buffer_consumed.get(c) + scale * inputs.commodity_amounts[i] * min_available);
		} else {
			break;
		}
	}
}

consumption_data_2 consume_2(
	sys::state& state,
	dcon::province_id province,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_demanded,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_consumed,
	economy::commodity_set const& inputs,
	preconsumption_data_2& additional_data,

	float input_multiplier, float throughput_multiplier, float output_multiplier,
	float employment_units, float output_multiplier_from_workers_with_high_education,
	float max_employment,
	economy_reason reason
) {
	assert(input_multiplier >= 0.f);
	assert(throughput_multiplier >= 0.f);
	assert(output_multiplier >= 0.f);

	float production_units = employment_units * throughput_multiplier;
	assert(production_units >= 0.f);

	float input_scale =
		input_multiplier
		* production_units;
	assert(input_scale >= 0.f);

	save_inputs_to_buffers_2(state, province, buffer_demanded, buffer_consumed, inputs, input_scale, additional_data.direct_inputs_data.min_available);

	consumption_data_2 result = {
		.direct_inputs_cost = additional_data.direct_inputs_cost_per_production_unit_availability_adjusted * production_units,
		.direct_inputs_expected_cost = additional_data.direct_inputs_expected_cost_per_production_unit * production_units,

		.output =
			additional_data.output_amount_per_production_unit
			* production_units
			* additional_data.direct_inputs_data.min_available
			* output_multiplier_from_workers_with_high_education,

		.direct_inputs_scale = input_scale,

		.input_cost_per_employment_unit = additional_data.direct_inputs_cost_per_production_unit * throughput_multiplier,
		.input_expected_cost_per_employment_unit = additional_data.direct_inputs_expected_cost_per_production_unit * throughput_multiplier	,
		.output_per_employment_unit =
			additional_data.output_amount_per_production_unit
			* throughput_multiplier
			* output_multiplier_from_workers_with_high_education
	};

	return result;
}

consumption_data_2 consume_2(
	sys::state& state,
	dcon::province_id province,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_demanded,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_consumed,
	economy::commodity_set const& inputs,
	preconsumption_data_2& additional_data,
	float input_multiplier, float throughput_multiplier, float output_multiplier,
	float employment_units,
	economy_reason reason
) {
	float production_units = employment_units * throughput_multiplier;
	float input_scale = input_multiplier * production_units;

	save_inputs_to_buffers_2(state, province, buffer_demanded, buffer_consumed, inputs, input_scale, additional_data.direct_inputs_data.min_available);

	consumption_data_2 result = {
		.direct_inputs_cost = additional_data.direct_inputs_cost_per_production_unit_availability_adjusted * production_units,
		.direct_inputs_expected_cost = additional_data.direct_inputs_expected_cost_per_production_unit * production_units,
		.output = additional_data.output_amount_per_production_unit * production_units * additional_data.direct_inputs_data.min_available,

		.direct_inputs_scale = input_scale,

		.input_cost_per_employment_unit = additional_data.direct_inputs_cost_per_production_unit * throughput_multiplier,
		.input_expected_cost_per_employment_unit = additional_data.direct_inputs_expected_cost_per_production_unit * throughput_multiplier	,
		.output_per_employment_unit = additional_data.output_amount_per_production_unit * throughput_multiplier
	};

	return result;
}

float employment_units_2(
	sys::state const& state,
	dcon::province_id labor_market,
	float target_workers_no_education, float target_workers_basic_education, float target_workers_high_education, float employment_unit_size
) {
	assert(target_workers_no_education >= 0.f);
	assert(target_workers_basic_education >= 0.f);
	float workers_no_education = target_workers_no_education
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::no_education);
	float workers_basic_education = target_workers_basic_education
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::basic_education);
	float workers_high_education = target_workers_high_education
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::high_education);
	assert(workers_no_education >= 0.f);
	assert(workers_basic_education >= 0.f);
	assert(workers_high_education >= 0.f);

	auto effective_employment =
		workers_no_education * unqualified_throughput_multiplier
		+ workers_basic_education + workers_high_education;
	return effective_employment / employment_unit_size;
}

float employment_units_guild_2(
	sys::state const& state,
	dcon::province_id labor_market,
	float target_workers_guild
) {
	float guild_actual_size =
		target_workers_guild
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::guild_education);
	return guild_actual_size / artisans_per_employment_unit;
}

float high_education_power_2(
	sys::state const& state,
	dcon::nation_id nation,
	dcon::commodity_id output
) {
	return
		std::max(0.f, 1.f + state.world.nation_get_factory_goods_output(nation, output))
		* std::max(0.f, 1.f + state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::factory_output))
		* economy::secondary_employment_output_bonus;
}

float output_multiplier_from_workers_with_high_education_2(
	sys::state const& state,
	dcon::commodity_id output,
	dcon::province_id labor_market,
	float target_workers_high_education
) {
	assert(target_workers_high_education >= 0.f);
	float workers_high_education = target_workers_high_education * state.world.province_get_labor_demand_satisfaction(labor_market, labor::high_education);
	assert(workers_high_education >= 0.f);
	return (1.f + high_education_power_2(state, state.world.province_get_nation_from_province_ownership(labor_market), output) * workers_high_education);
}

// returns employment units
float consume_labor_2(
	sys::state& state,
	dcon::province_id labor_market,
	float target_workers_no_education,
	float target_workers_basic_education,
	float target_workers_high_education,
	float employment_unit_size
) {
	assert(std::isfinite(target_workers_no_education) && target_workers_no_education >= 0.f);
	assert(std::isfinite(target_workers_basic_education) && target_workers_basic_education >= 0.f);
	assert(std::isfinite(target_workers_high_education) && target_workers_high_education >= 0.f);

	// register demand for labor
	auto& no_edu_demand = state.world.province_get_labor_demand(labor_market, labor::no_education);
	state.world.province_set_labor_demand(labor_market, labor::no_education, no_edu_demand + target_workers_no_education);

	auto& basic_edu_demand = state.world.province_get_labor_demand(labor_market, labor::basic_education);
	state.world.province_set_labor_demand(labor_market, labor::basic_education, basic_edu_demand + target_workers_basic_education);

	auto& high_edu_demand = state.world.province_get_labor_demand(labor_market, labor::high_education);
	state.world.province_set_labor_demand(labor_market, labor::high_education, high_edu_demand + target_workers_high_education);

	assert(std::isfinite(state.world.province_get_labor_demand(labor_market, labor::no_education)));
	assert(std::isfinite(state.world.province_get_labor_demand(labor_market, labor::basic_education)));
	assert(std::isfinite(state.world.province_get_labor_demand(labor_market, labor::high_education)));

	return employment_units_2(
		state, labor_market,
		target_workers_no_education, target_workers_basic_education, target_workers_high_education, employment_unit_size
	);
}

template<typename NATIONS, typename PROV>
auto max_rgo_efficiency_2(sys::state& state, NATIONS n, PROV p, dcon::commodity_id c) {
	using VALUE = adaptive_ve::convert_to_float<NATIONS>;
	//using MASK = adaptive_ve::convert_to_bool<NATIONS>;

	auto is_mine = state.world.commodity_get_is_mine(c);

	auto prov_mod_offset = is_mine ? sys::provincial_mod_offsets::mine_rgo_eff : sys::provincial_mod_offsets::farm_rgo_eff;
	auto nation_mod_offset = is_mine ? sys::national_mod_offsets::mine_rgo_eff : sys::national_mod_offsets::farm_rgo_eff;

	VALUE throughput =
		1.0f
		+ state.world.province_get_modifier_values(p, sys::provincial_mod_offsets::local_rgo_throughput)
		+ state.world.nation_get_modifier_values(n, sys::national_mod_offsets::rgo_throughput)
		+ state.world.province_get_modifier_values(p, prov_mod_offset)
		+ state.world.nation_get_modifier_values(n, nation_mod_offset);

	VALUE result =
		(
			(state.world.province_get_demographics(p, demographics::literacy) + 1.f)
			/ (state.world.province_get_demographics(p, demographics::total) + 1.f)
			+ 0.01f
		)
		* adaptive_ve::max<VALUE>(0.5f, throughput)
		* state.defines.alice_rgo_boost
		* adaptive_ve::max<VALUE>(0.5f, (
			1.0f
			+ state.world.province_get_modifier_values(p, sys::provincial_mod_offsets::local_rgo_output)
			+ state.world.nation_get_modifier_values(n, sys::national_mod_offsets::rgo_output)
			+ state.world.nation_get_rgo_goods_output(n, c)
			));

	if(state.world.commodity_get_money_rgo(c)) {
		return free_efficiency + result * 0.1f;
	}

	// to compensate for smaller mine sizes, we provide additional potential efficiency, if they are able to reach it
	return free_efficiency + result * (is_mine ? 5.f : 1.f);
}

float consume_labor_guild_2(
	sys::state& state,
	dcon::province_id province,
	float target_guild_size
) {

#ifndef NDEBUG
	ve::apply([&](float value) {assert(std::isfinite(value) && value >= 0.f); }, target_guild_size);
#endif // !NDEBUG

	state.world.province_set_labor_demand(
		province,
		labor::guild_education,
		state.world.province_get_labor_demand(province, labor::guild_education) + target_guild_size
	);

#ifndef NDEBUG
	ve::apply([&](float value) {assert(std::isfinite(value)); }, state.world.province_get_labor_demand(province, labor::guild_education));
#endif // !NDEBUG

	return employment_units_guild_2(state, province, target_guild_size);
}

float get_total_wage_2(const sys::state& state, dcon::factory_id f) {
	auto labor_market = state.world.factory_get_province_from_factory_location(f);
	return state.world.province_get_labor_price(labor_market, labor::no_education)
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::no_education)
		* state.world.factory_get_unqualified_employment(f)
		+
		state.world.province_get_labor_price(labor_market, labor::basic_education)
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::basic_education)
		* state.world.factory_get_primary_employment(f)
		+
		state.world.province_get_labor_price(labor_market, labor::high_education)
		* state.world.province_get_labor_demand_satisfaction(labor_market, labor::high_education)
		* state.world.factory_get_secondary_employment(f);
}
float get_total_target_wage_2(const sys::state& state, dcon::factory_id f) {
	auto labor_market = state.world.factory_get_province_from_factory_location(f);
	return state.world.province_get_labor_price(labor_market, labor::no_education)
		* state.world.factory_get_unqualified_employment(f)
		+
		state.world.province_get_labor_price(labor_market, labor::basic_education)
		* state.world.factory_get_primary_employment(f)
		+
		state.world.province_get_labor_price(labor_market, labor::high_education)
		* state.world.factory_get_secondary_employment(f);
}

void update_single_factory_consumption_2(
	sys::state& state,
	dcon::factory_id f,
	dcon::province_id p,
	dcon::state_instance_id s,
	dcon::market_id m,
	dcon::nation_id n,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_demanded,
	ve::vectorizable_buffer<float, dcon::commodity_id>& buffer_consumed,
	float mobilization_impact,
	bool occupied
) {
	auto fac = fatten(state.world, f);
	auto fac_type = fac.get_building_type();
	assert(fac_type);
	assert(fac_type.get_output());
	assert(n);
	assert(p);
	assert(s);

	//modifiers
	float input_multiplier = factory_input_multiplier(state, fac, n, p, s);
	auto const mfactor = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::factory_maintenance) + 1.0f;
	float throughput_multiplier = factory_throughput_multiplier(state, f, n, p, s, fac.get_size());
	float output_multiplier = factory_output_multiplier_no_secondary_workers(state, fac, n, p);

	auto& direct_inputs = fac_type.get_inputs();

	auto base_data = prepare_data_for_consumption_2(
		state, m,
		direct_inputs,
		fac_type.get_output(), fac_type.get_output_amount(),
		input_multiplier, output_multiplier
	);

	auto max_employment = state.world.factory_get_size(fac);

	auto employment_units = consume_labor_2(
		state, fac.get_province_from_factory_location(),
		fac.get_unqualified_employment(), fac.get_primary_employment(), fac.get_secondary_employment(),
		float(fac_type.get_base_workforce())
	) * std::max(0.f, mobilization_impact);
	auto total_employment = fac.get_unqualified_employment() + fac.get_primary_employment() + fac.get_secondary_employment();

	auto data = consume_2(
		state, p, buffer_demanded, buffer_consumed,
		direct_inputs,
		base_data,
		input_multiplier, throughput_multiplier, output_multiplier,
		employment_units,
		output_multiplier_from_workers_with_high_education_2(
			state, fac_type.get_output(), fac.get_province_from_factory_location(), fac.get_secondary_employment()
		), max_employment, economy_reason::factory
	);

	auto current_size = state.world.factory_get_size(f);
	auto ftid = state.world.factory_get_building_type(f);
	auto base_size = state.world.factory_type_get_base_workforce(ftid);
	float actual_wages = get_total_wage_2(state, f);
	float actual_profit =
		data.output
		* base_data.output_price
		* state.world.market_get_actual_probability_to_sell(m, fac_type.get_output())
		- data.direct_inputs_cost
		- actual_wages;

	if(state.world.commodity_get_uses_potentials(fac_type.get_output())) {
		auto new_size = state.world.factory_get_size(fac);
		// A factory may be created in history but have no potentials for such factory in the province
		// Then we apply min size
		state.world.factory_set_size(fac, std::min(new_size, std::max(500.f, state.world.province_get_factory_max_size(p, fac_type.get_output()))));
		assert(std::isfinite(state.world.factory_get_size(fac)));
		assert(state.world.factory_get_size(fac) > 0.f);
	}

	fac.set_unprofitable(actual_profit <= 0.0f);

	fac.set_output(data.output);
	fac.set_output_per_worker(data.output_per_employment_unit / float(fac_type.get_base_workforce()));
	fac.set_input_cost_per_worker(data.input_cost_per_employment_unit / float(fac_type.get_base_workforce()));
	fac.set_input_expected_cost_per_worker(data.input_expected_cost_per_employment_unit / float(fac_type.get_base_workforce()));
	fac.set_input_cost(data.direct_inputs_cost);
}

void daily_update_market_parallel(sys::state& state, bool presimulation, float presimulation_stage, int date_offset) {
	uint32_t total_commodities = state.world.commodity_size();

	if(presimulation) {
		state.world.execute_serial_over_province([&](auto pid) {
			state.world.province_set_artisan_bank(pid, 0.f);
			state.world.province_set_factory_bank(pid, 0.f);
		});
	}

	// national
	auto investment_tokens = state.world.nation_make_vectorizable_float_buffer();
	static auto current_stockpile_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_stockpile_total_cost = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(1);
	static ve::vectorizable_buffer<float, dcon::nation_id> invention_count = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if(new_count > old_count) {
			current_stockpile_budget = state.world.nation_make_vectorizable_float_buffer();
			expected_stockpile_total_cost = state.world.nation_make_vectorizable_float_buffer();
			market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(new_count);
			invention_count = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	state.world.execute_parallel_over_nation([&](auto nations) {
		for_each_commodity_no_money(state, [&](dcon::commodity_id com_id) {
			auto to_apply = state.world.nation_get_total_stockpiles(nations, com_id);
			state.world.nation_set_yesterday_total_stockpiles(nations, com_id, to_apply);
		});
		state.world.nation_set_subsidy_token_total(nations, 0.f);
		auto treasury = state.world.nation_get_treasury(nations);
		state.world.nation_set_last_treasury(nations, treasury);
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
			state.world.nation_set_commodity_float_buffer_1(nations, commodity, ve::fp_vector{ 0.0f });
		});
		expected_stockpile_total_cost.set(nations, 0.0f);
		auto base_budget = state.world.nation_get_last_base_budget(nations);
		auto stockpiles_priority = ve::to_float(state.world.nation_get_stockpile_spending(nations)) / 100.f;
		current_stockpile_budget.set(nations, ve::max(0.f, base_budget * stockpiles_priority));
		invention_count.set(nations, 0.f);
		state.world.for_each_invention([&](auto iid) {
			auto count =
				invention_count.get(nations)
				+ ve::select(state.world.nation_get_active_inventions(nations, iid), ve::fp_vector(1.0f), ve::fp_vector(0.0f));
			invention_count.set(nations, count);
		});
		ve::apply([&](auto nation){
			market_leader.set(nation, nations::get_market_leader(state, nation));

			state.world.nation_for_each_province_ownership(nation, [&](dcon::province_ownership_id poid) {
				auto province = state.world.province_ownership_get_province(poid);
				auto area = state.world.province_get_state_membership(province);

				state.world.province_for_each_factory_location(province, [&](dcon::factory_location_id factory_location) {
					auto factory = state.world.factory_location_get_factory(factory_location);
					auto building_type = state.world.factory_get_building_type(factory);
					auto output_type = state.world.factory_type_get_output(building_type);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, output_type));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, output_type));
					if(priority || priority_local) {
						auto base_output = state.world.factory_type_get_output_amount(building_type);
						auto factory_output = state.world.factory_get_output(factory);
						auto effective_output = factory_output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_factory_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_factory_bank(province, current_money + last_token_price * effective_output);
					}
				});
				state.world.for_each_commodity([&](auto cid) {
					auto base_output = state.world.commodity_get_rgo_amount(cid);
					if(base_output == 0.f) return;

					auto area = state.world.province_get_state_membership(province);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));

					if(priority || priority_local) {
						auto rgo_output = state.world.province_get_rgo_output(province, cid);
						auto effective_output = rgo_output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_rgo_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_rgo_bank(province, current_money + last_token_price * effective_output);
					}
				});

				state.world.for_each_commodity([&](auto cid) {
					auto base_output = state.world.commodity_get_artisan_output_amount(cid);
					if(base_output == 0.f) return;
					auto area = state.world.province_get_state_membership(province);
					auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
					auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));
					if(priority || priority_local) {
						auto output = state.world.province_get_artisan_actual_production(province, cid);
						auto effective_output = output / base_output;
						auto tokens = state.world.nation_get_subsidy_token_total(nation);
						state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

						auto current_money = state.world.province_get_artisan_bank(province);
						auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
						state.world.province_set_artisan_bank(province, current_money + last_token_price * effective_output);
					}
				});


				// FACTORIES
				for(auto f : state.world.province_get_factory_location(province)) {
					auto factory = f.get_factory();
					auto old = investment_tokens.get(nation);
					auto factory_tokens = factory_investment_tokens_2(state, nation, province, factory);
					investment_tokens.set(nation, old + factory_tokens);
				}

				// RGO
				state.world.for_each_commodity([&](auto c) {
					auto old = investment_tokens.get(nation);
					auto rgo_tokens = rgo_investment_tokens_2(state, nation, province, c);
					investment_tokens.set(nation, old + rgo_tokens);
				});
			});
		}, nations);
	});

	if(state.trade_route_cached_values_out_of_date) {
		state.trade_route_cached_values_out_of_date = false;

		ankerl::unordered_dense::map<int32_t, bool> direct_block;
		ankerl::unordered_dense::map<int32_t, bool> trade_closed;
		ankerl::unordered_dense::map<int32_t, bool> direct_no_tariffs;
		ankerl::unordered_dense::map<int32_t, bool> no_tariffs;

		// US3AC9. Wartime embargoes

		state.world.for_each_war([&](auto war) {
			state.world.war_for_each_war_participant(war, [&](auto attacker_candidate) {
				if(!state.world.war_participant_get_is_attacker(attacker_candidate)) return;
				auto attacker = state.world.war_participant_get_nation(attacker_candidate);

				state.world.war_for_each_war_participant(war, [&](auto defender_candidate) {
					if(state.world.war_participant_get_is_attacker(defender_candidate)) return;
					auto defender = state.world.war_participant_get_nation(defender_candidate);

					auto index_pair = attacker.index() * state.world.nation_size() + defender.index();
					auto index_pair_T = defender.index() * state.world.nation_size() + attacker.index();
					direct_block[index_pair] = true;
					direct_block[index_pair_T] = true;
				});
			});
		});

		// US3AC10. diplomatic embargos
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_embargo(rel)) {
				dcon::nation_id source = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id target = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = source.index() * state.world.nation_size() + target.index();
				auto index_pair_T = target.index() * state.world.nation_size() + source.index();

				direct_block[index_pair] = true;
				direct_block[index_pair_T] = true;
			}
		});

		// US3AC11. US3AC12. sphere joins market leader
		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);
				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_block.find(indices[0]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_block.find(indices[1]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_block.find(indices[2]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}

				{
					if(direct_block.find(base_pair) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
			});
		});

		// US3AC15. Equal/unequal trade treaties
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_no_tariffs_until(rel)) {
				dcon::nation_id n1 = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id n2 = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = n1.index() * state.world.nation_size() + n2.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		// Reflexivity of free trade
		state.world.for_each_nation([&](auto nid) {
			auto index_pair = nid.index() * state.world.nation_size() + nid.index();
			direct_no_tariffs[index_pair] = true;
		});
		state.world.for_each_nation([&](auto nid) {
			dcon::nation_id sphere = state.world.nation_get_in_sphere_of(nid);
			if(sphere) {
				auto index_pair = nid.index() * state.world.nation_size() + sphere.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		state.world.for_each_overlord([&](auto ovid) {
			dcon::nation_id subject = state.world.overlord_get_subject(ovid);
			dcon::nation_id overlord = state.world.overlord_get_ruler(ovid);
			auto index_pair = subject.index() * state.world.nation_size() + overlord.index();
			direct_no_tariffs[index_pair] = true;
		});

		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);

				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_no_tariffs.find(indices[0]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_no_tariffs.find(indices[1]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_no_tariffs.find(indices[2]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				{
					if(direct_no_tariffs.find(base_pair) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
			});
		});

		// update cache:

		state.world.for_each_trade_route([&](auto route) {
			auto A = state.world.trade_route_get_origin(route);
			auto B = state.world.trade_route_get_target(route);
			auto s_A = state.world.market_get_zone_from_local_market(A);
			auto s_B = state.world.market_get_zone_from_local_market(B);

			auto capital_A = state.world.state_instance_get_capital(s_A);
			auto capital_B = state.world.state_instance_get_capital(s_B);

			auto controller_A = state.world.province_get_nation_from_province_control(capital_A);
			auto controller_B = state.world.province_get_nation_from_province_control(capital_B);

			auto index = controller_A.index() * state.world.nation_size() + controller_B.index();
			auto index_T = controller_B.index() * state.world.nation_size() + controller_A.index();

			if(trade_closed.find(index) != trade_closed.end()) {
				state.world.trade_route_set_is_trade_forbidden(route, true);
			} else {
				state.world.trade_route_set_is_trade_forbidden(route, false);
			}

			if(no_tariffs.find(index) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_origin(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_origin(route, true);
			}

			if(no_tariffs.find(index_T) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_target(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_target(route, true);
			}
		});
	};

	update_government_stockpile_market_demand_weights(state);

	concurrency::parallel_for(0, 5, [&](int32_t index) {
		switch(index) {
		case 0:
			populate_navy_consumption(state);
			break;
		case 1:
			// uses nation_commodity_float_buffer_1
			populate_government_stockpile_demand(state, expected_stockpile_total_cost, current_stockpile_budget);
			decay_government_stockpiles(state);
			break;
		case 2:
			populate_army_consumption(state);
			break;
		case 3:
			populate_private_construction_consumption(state);
			break;
		case 4:
			populate_government_construction_consumption(state);
			break;
		}
	});

	// market
	static auto export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	static auto import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.market_size();
		if(new_count > old_count) {
			export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	static auto coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
	static auto state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.state_instance_size();
		if(new_count > old_count) {
			coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
			state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
			old_count = new_count;
		}
	}

	auto port_availability = state.world.market_make_vectorizable_float_buffer();
	auto price_port_capacity = state.world.market_make_vectorizable_float_buffer();
	auto port_total_weight = state.world.market_make_vectorizable_float_buffer();
	auto market_total_investment = state.world.market_make_vectorizable_float_buffer();

	state.world.execute_parallel_over_market([&](auto markets){
		state.world.market_set_gdp(markets, 0.f);

		state.world.for_each_pop_type([&](dcon::pop_type_id t) {
			state.world.market_set_life_needs_costs(markets, t, ve::fp_vector{});
			state.world.market_set_everyday_needs_costs(markets, t, ve::fp_vector{});
			state.world.market_set_luxury_needs_costs(markets, t, ve::fp_vector{});
		});

		auto sids = state.world.market_get_zone_from_local_market(markets);
		auto owner_nids = state.world.state_instance_get_nation_from_state_ownership(sids);
		ve::apply([&](auto nation_owner, auto market, auto state_instance){
			import_tariff_buffer.set(market, effective_tariff_import_rate(state, nation_owner, market));
			export_tariff_buffer.set(market, effective_tariff_export_rate(state, nation_owner, market));
			province::for_each_province_in_state_instance(state, state_instance, [&](auto province) {
				coastal_capital_buffer.set(state_instance, province::state_get_coastal_capital(state, state_instance));
				auto quality = (float(state.world.province_get_life_rating(province)) - 10.f) / 10.f;
				quality = std::max(quality, 0.f) + 0.01f;
				auto score = (subsistence_factor * quality) + subsistence_score_life * 0.9f;
				state.world.province_set_subsistence_score(province, score);
				auto weight_population =
					state.world.state_instance_get_demographics(state_instance, demographics::to_key(state, state.culture_definitions.farmers))
					+ state.world.state_instance_get_demographics(state_instance, demographics::to_key(state, state.culture_definitions.laborers));
				auto weight_capitalists = weight_population / 50.f
					+ state.world.state_instance_get_demographics(state_instance, demographics::to_key(state, state.culture_definitions.capitalists)) * 200.f;
				auto weight_aristocracy = weight_population / 50.f
					+ state.world.state_instance_get_demographics(state_instance, demographics::to_key(state, state.culture_definitions.aristocrat)) * 200.f
					+ state.world.state_instance_get_demographics(state_instance, demographics::to_key(state, state.culture_definitions.slaves));
				auto total = weight_aristocracy + weight_capitalists + weight_population + 1.0f;
				state.world.province_set_landowners_share(province, weight_aristocracy / total);
				state.world.province_set_capitalists_share(province, weight_capitalists / total);
				state.world.province_for_each_factory_location(province, [&](auto location)  {
					auto f = state.world.factory_location_get_factory(location);
					auto fac_type = fatten(state.world, state.world.factory_get_building_type(f));
					float sum = 0.0f;
					if(nation_owner && state_instance) {
						for(auto bonus : fac_type.get_factory_bonuses()) {
							if(bonus.trigger && trigger::evaluate(state, bonus.trigger, trigger::to_generic(state_instance), trigger::to_generic(nation_owner), 0)) {
								sum -= bonus.amount;
							}
						}
					}
					state.world.factory_set_triggered_modifiers(f, sum);
				});
				{
					auto price = state.world.province_get_service_price(province, services::list::port_capacity);
					auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(province, advanced_province_buildings::list::civilian_ports);
					auto local_weight = size / (price_properties::service::epsilon + price);
					port_total_weight.set(market, port_total_weight.get(market) + local_weight);
				}
			});
			province::for_each_province_in_state_instance(state, state_instance, [&](auto province) {
				auto price = state.world.province_get_service_price(province, services::list::port_capacity);
				auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(province, advanced_province_buildings::list::civilian_ports);
				auto local_weight = size / (price_properties::service::epsilon + price);
				auto total_weight = port_total_weight.get(market);
				auto local_demand_ratio = local_weight / total_weight;
				auto sat = state.world.province_get_service_satisfaction(province, services::list::port_capacity);
				port_availability.set(market, port_availability.get(market) + local_demand_ratio * sat);
				price_port_capacity.set(market, price_port_capacity.get(market) + local_demand_ratio * price);
			});
			auto capital = state.world.state_instance_get_capital(sids);
			auto port = coastal_capital_buffer.get(sids);
			auto controller_capital = state.world.province_get_nation_from_province_control(capital);
			auto controller_port = state.world.province_get_nation_from_province_control(port);

			auto occupied = ve::apply([&](auto capital_nation, auto port_nation) {
				return military::are_at_war(state, capital_nation, port_nation);
			}, controller_capital, controller_port);

			state_naval_trade_is_blockaded.set(sids, ve::select(port == dcon::province_id{} || occupied || state.world.province_get_is_blockaded(port), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f }));
			
		}, owner_nids, markets, sids);

		auto invention_factor = invention_count.get(owner_nids) * state.defines.invention_impact_on_demand + 1.0f;
		// STEP 0: gather total demand costs:
		/*
		- Each pop strata and needs type has its own demand modifier, calculated as follows:
		- (national-modifier-to-goods-demand + define:BASE_GOODS_DEMAND) x (national-modifier-to-specific-strata-and-needs-type + 1) x
		(define:INVENTION_IMPACT_ON_DEMAND x number-of-unlocked-inventions + 1, but for non-life-needs only)
		- Each needs demand is also multiplied by  2 - the nation's administrative efficiency if the pop has education / admin /
		military income for that need category
		- We calculate an adjusted pop-size as (0.5 + pop-consciousness / define:PDEF_BASE_CON) x (for non-colonial pops: 1 +
		national-plurality (as a fraction of 100)) x pop-size
		*/

		// poor strata update

		auto nations = owner_nids;

		{
			uint8_t strata_filter = 0;
			auto offset_life = sys::national_mod_offsets::poor_life_needs;
			auto offset_everyday = sys::national_mod_offsets::poor_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::poor_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);

#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
							);
#endif
				});
			}
		}

		// middle strata update

		{
			uint8_t strata_filter = 1;
			auto offset_life = sys::national_mod_offsets::middle_life_needs;
			auto offset_everyday = sys::national_mod_offsets::middle_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::middle_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);
#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
							);
#endif
				});
			}
		}

		// rich strata update

		{
			uint8_t strata_filter = 2;
			auto offset_life = sys::national_mod_offsets::rich_life_needs;
			auto offset_everyday = sys::national_mod_offsets::rich_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::rich_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);

#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
							);
#endif
				});
			}
		}
		state.world.for_each_consumption_category([&](auto cat_id) {
			ve::fp_vector total{ 0.f };
			state.world.for_each_commodity([&](auto c) {
				auto price = state.world.market_get_price(markets, c);
				auto index = c.index() + cat_id.index() * state.world.commodity_size();
				auto consumption_weight = state.world.consumption_category_get_weights(cat_id, c);
				auto w = state.world.market_get_local_consumption_weights(markets, index);
#ifndef NDEBUG
				ve::apply(
					[](float amount) {
						assert(std::isfinite(amount) && amount >= 0.f);
					}, price * consumption_weight * w
				);
#endif
				total = total + price * consumption_weight * w;
			});
#ifndef NDEBUG
			ve::apply(
				[](float amount) {
					assert(std::isfinite(amount) && amount >= 0.f);
				}, total
			);
#endif
			state.world.market_set_cost_per_consumption_category(markets, cat_id, total);
		});

		// DEMAND STAGE
		// RESET DEMAND
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			auto old_demand = state.world.market_get_demand(markets, c);
			auto old_aggregated_demand = state.world.market_get_aggregated_demand_history(markets, c);
			if(presimulation) {
				state.world.market_set_aggregated_demand_history(markets, c, old_aggregated_demand * 0.001f + old_demand * 0.999f);
			} else {
				state.world.market_set_aggregated_demand_history(markets, c, old_aggregated_demand * 0.95f + old_demand * 0.05f);
			}

			state.world.market_set_demand(markets, c, ve::fp_vector{});
			state.world.market_set_intermediate_demand(markets, c, ve::fp_vector{});
		});
		ve::apply([&](auto nation_owner, auto market, auto state_instance) {
			province::for_each_province_in_state_instance(state, state_instance, [&](auto province) {
				for(int32_t i = 0; i < labor::total; i++) {
					state.world.province_set_labor_demand(province, i, 0.f);
				}
				for(int32_t i = 0; i < services::list::total; i++) {
					state.world.province_set_service_demand_allowed_public_supply(province, i, 0.f);
					state.world.province_set_service_demand_forbidden_public_supply(province, i, 0.f);
				}
			});
		}, owner_nids, markets, sids);
		state.world.for_each_pop_type([&](auto pt) {
			state.world.market_set_life_needs_scale(markets, pt, 0.f);
			state.world.market_set_everyday_needs_scale(markets, pt, 0.f);
			state.world.market_set_luxury_needs_scale(markets, pt, 0.f);
		});
		state.world.for_each_consumption_category([&](auto cat) {
			state.world.market_set_demand_per_consumption_category(markets, cat, 0.f);
		});
		state.world.market_set_total_port_capacity(markets, 0.f);
		state.world.market_set_land_transportation_demand(markets, 0.f);
		state.world.market_set_naval_transportation_demand(markets, 0.f);

#ifdef STUPID_EXPENSIVE
		// PRODUCTION DEMAND
		ve::apply([&](auto nation_owner, auto market, auto state_instance) {
			province::for_each_province_in_state_instance(state, state_instance, [&](auto province) {
				ve::vectorizable_buffer<float, dcon::commodity_id> buffer_demanded = state.world.commodity_make_vectorizable_float_buffer();
				ve::vectorizable_buffer<float, dcon::commodity_id> buffer_consumed_estimation = state.world.commodity_make_vectorizable_float_buffer();
				auto mobilization_impact =					
					state.world.nation_get_is_mobilized(nation_owner) ?
					military::mobilization_impact(state, nation_owner) : 1.f;
				auto const csize = state.world.commodity_size();
				{
					auto total_profit = 0.0f;
					auto input_multiplier = artisan_input_multiplier_2(state, nation_owner);
					auto throughput_multiplier = artisan_throughput_multiplier_2(state, nation_owner);
					auto output_multiplier = artisan_output_multiplier_2(state, nation_owner);

					for(uint32_t i = 1; i < csize; ++i) {
						dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
						auto output_amount = state.world.commodity_get_artisan_output_amount(cid);
						if(output_amount <= 0.f) {
							continue;
						}

						auto const& inputs = state.world.commodity_get_artisan_inputs(cid);
						state.world.province_set_artisan_actual_production(province, cid, 0.0f);
						auto valid_mask = valid_artisan_good(state, nation_owner, cid);
						auto target_workers = state.world.province_get_artisan_score(province, cid);

						if(target_workers <= 0.f) {
							continue;
						}

						auto actual_workers = valid_mask ? target_workers * mobilization_impact : 0.f;
						auto employment_units = valid_mask ? consume_labor_guild_2(state, province, actual_workers) : 0.f;

						preconsumption_data_2 prepared_data = prepare_data_for_consumption_2(
							state, market, inputs, cid, output_amount, input_multiplier, output_multiplier
						);
						consumption_data_2 consumption_data = consume_2(
							state, province, buffer_demanded, buffer_consumed_estimation,
							inputs, prepared_data,
							input_multiplier, throughput_multiplier, output_multiplier,
							employment_units, economy_reason::artisan
						);
						state.world.province_set_artisan_actual_production(
							province,
							cid,
							consumption_data.output
						);
						total_profit =
							total_profit
							+ consumption_data.output * prepared_data.output_price * state.world.market_get_actual_probability_to_sell(market, cid)
							- consumption_data.direct_inputs_cost;
					}
					state.world.province_set_artisan_profit(province, total_profit);
				}

				auto p = province;
				auto n = nation_owner;
				auto sid = state_instance;
				auto m = market;

				{

					// update efficiency and consume efficiency_inputs:
					state.world.for_each_commodity([&](dcon::commodity_id c) {
						auto size = state.world.province_get_rgo_size(p, c);
						if(size <= 0.f) {
							return;
						}

						// recalculate max efficiency here (doesn't really matter where, just do it once)
						auto max_efficiency = max_rgo_efficiency_2(state, n, p, c);
						state.world.province_set_rgo_max_efficiency(p, c, max_efficiency);

						// estimate amount of workers using previous day data
						auto estimated_amount_of_workers = state.world.province_get_rgo_target_employment(p, c)
							* state.world.province_get_labor_demand_satisfaction(p, labor::no_education)
							* mobilization_impact;

						// DEMAND ON LABOR

						auto target = state.world.province_get_rgo_target_employment(p, c);
						auto cur_labor_demand = state.world.province_get_labor_demand(p, labor::no_education);

#ifndef NDEBUG
						ve::apply([&](float value) {assert(std::isfinite(value) && value >= 0.f); }, cur_labor_demand + target);
#endif // !NDEBUG

						state.world.province_set_labor_demand(p, labor::no_education, cur_labor_demand + target);

						// SAVE ADDITIONAL DATA

						auto per_worker = state.world.commodity_get_rgo_amount(c)
							* state.world.province_get_rgo_efficiency(p, c)
							* state.world.province_get_rgo_base_efficiency(p, c)
							/ state.defines.alice_rgo_per_size_employment;
						state.world.province_set_rgo_output_per_worker(p, c, per_worker);
					});
				}

				for(auto f : state.world.province_get_factory_location(p)) {
					update_single_factory_consumption_2(
						state,
						f.get_factory(),
						p,
						sid,
						market,
						nation_owner, buffer_demanded, buffer_consumed_estimation,
						mobilization_impact,
						state.world.province_get_nation_from_province_control(p) != nation_owner // is occupied
					);
				}

				state.world.for_each_commodity([&](auto cid) {
					auto median_price = state.world.commodity_get_median_price(cid);
					auto demanded = buffer_demanded.get(cid);
					auto consumed = buffer_consumed_estimation.get(cid);
					assert(std::isfinite(demanded));
					assert(std::isfinite(consumed));
					register_intermediate_demand(state, market, cid, demanded);
					state.world.market_set_gdp(market, state.world.market_get_gdp(market) - consumed * median_price);
				});

				auto nation = nation_owner;
				auto available_investment = state.world.nation_get_private_investment(nation);
				auto actually_spent = 0.f;
				auto total_tokens = investment_tokens.get(nation);
				bool investment_spent = false;

				// FACTORY
				for(auto f : state.world.province_get_factory_location(province)) {
					auto factory = f.get_factory();
					auto factory_type = factory.get_building_type();
					auto output_type = factory_type.get_output();
					auto size = state.world.factory_get_size(factory);
					auto factory_tokens = factory_investment_tokens_2(state, nation, province, factory);

					//auto time = state.world.factory_type_get_construction_time(factory_type);
					auto base_size = state.world.factory_type_get_base_workforce(factory_type);
					auto per_worker_output_cost = state.world.factory_get_output_per_worker(factory) * state.world.market_get_price(market, output_type);
					auto per_worker_input_cost = state.world.factory_get_input_cost_per_worker(factory);
					auto profit = per_worker_output_cost; // - per_worker_input_cost - state.world.province_get_labor_price(province, economy::labor::basic_education);

					// SIZE

					float expand_priority = investment_expand_factory_priority(state, factory);

					if(expand_priority > 0.f) {
						auto investment = available_investment * factory_tokens / total_tokens * expand_priority;

						auto& costs = state.world.factory_type_get_construction_costs(factory_type);
						auto costs_data = get_inputs_data(state, market, costs);
						// expansion is cheaper than new construction:
						auto cost_per_unit = costs_data.total_cost / base_size / 2.f;
						auto expansion_scale = std::min(100.f + size * 0.05f, investment / cost_per_unit);

						for(uint32_t i = 0; i < economy::commodity_set::set_size; ++i) {
							auto cid = costs.commodity_type[i];
							if(!cid) break;
							auto amount = costs.commodity_amounts[i] / base_size / 2.f;
							economy::register_demand(state, market, costs.commodity_type[i], expansion_scale * amount);
							actually_spent = actually_spent + expansion_scale * amount * state.world.market_get_price(market, cid) * state.world.market_get_actual_probability_to_buy(market, cid);
						}
						auto actual_expansion = expansion_scale * costs_data.min_available;
						state.world.factory_set_size(factory, std::max(1.f, size * 0.99999f - 0.01f) + actual_expansion);
					} else {
						state.world.factory_set_size(factory, std::max(1.f, size * 0.99999f - 0.01f));
					}

					/*
					EFFICIENCY
					Investment logic into efficiency is pretty simple:
					Invest proportionally to the current level of efficiency investments into every category.
					- Base efficiency additive growth is 1.
					- Base efficiency additive decay is 1.
					- Base efficiency multiplicative decay is whatever number you see in the code below.
					Satisfaction of every category multiplies growth by (1 + alpha * effective scale)
					Originally, the idea was to scale costs with the level of investment.
					But it led to machine parts factories entering the vicious cycle of producing machine parts only to consume them themselves.
					So now costs are scaled only with current factory size.
					Instead of scaling the costs with efficiency, current efficiency decreases the speed at which efficiency is increased.
					Technological advances increases speed of increasing efficiency.
					Final efficiency would influence factory throughput.
					*/

					auto base_growth = 1.f;
					auto decay_mult = 0.0001f;
					auto old = state.world.factory_get_technology_scale(factory);

					auto national_t = state.world.nation_get_factory_goods_throughput(nation, output_type);
					auto nationnal_fac_t = state.world.nation_get_modifier_values(nation, sys::national_mod_offsets::factory_throughput);
					auto investment_efficiency = std::min(1.f, 1.f / (1.f + old))
						* std::max(0.f, 1.f + national_t)
						* std::max(0.f, 1.f + nationnal_fac_t);

					if(expand_priority < 1.f) {
						auto investment = available_investment * factory_tokens / total_tokens * (1.f - expand_priority);
						auto& efficiency_inputs = state.world.factory_type_get_efficiency_inputs(factory_type);
						auto total_can_afford = 0.f;
						for(uint8_t i = 0; i < economy::small_commodity_set::set_size; i++) {
							auto cid = efficiency_inputs.commodity_type[i];
							if(!cid) break;

							auto current_scale = (1.f + size) / state.world.factory_type_get_base_workforce(factory_type);

							auto amount = 0.01f * efficiency_inputs.commodity_amounts[i] * current_scale;
							auto cost = amount * state.world.market_get_price(market, cid);
							auto can_afford = std::max(0.f, investment / cost - 1.f);

							total_can_afford = total_can_afford + can_afford;
						}

						if(total_can_afford > 0.f) {
							for(uint8_t i = 0; i < economy::small_commodity_set::set_size; i++) {
								auto cid = efficiency_inputs.commodity_type[i];
								if(!cid) break;

								auto current_scale = (1.f + size) / state.world.factory_type_get_base_workforce(factory_type);

								auto amount = 0.01f * efficiency_inputs.commodity_amounts[i] * current_scale;
								auto cost = amount * state.world.market_get_price(market, cid);
								auto can_afford = std::max(0.f, investment / cost - 1.f);
								auto investment_into_category = investment * can_afford / total_can_afford;

								auto can_actually_afford = std::min(1.f, investment_into_category / cost);
								economy::register_demand(state, market, cid, can_actually_afford * amount);
								auto probability_to_buy = state.world.market_get_actual_probability_to_buy(market, cid);
								auto growth = investment_efficiency * can_actually_afford * probability_to_buy;
								base_growth = base_growth * (1.f + decay_mult * growth);

								actually_spent = actually_spent + can_actually_afford * cost;
							}
						}
					}

					//auto decay = std::max(0.f, (old - investment_efficiency) * decay_mult);
					state.world.factory_set_technology_scale(factory, std::max(0.05f, old + base_growth - 1.f));
				}

				auto total_agriculture = 0.f;

				//RGO
				state.world.for_each_commodity([&](auto c) {
					auto decay_mult = 0.0001f;
					auto base_output = state.world.commodity_get_rgo_amount(c);
					if(base_output == 0.f) return;
					auto current_max_size = state.world.province_get_rgo_potential(province, c);
					if(current_max_size == 0.f) return;
					auto current_size = state.world.province_get_rgo_size(province, c);

					auto local_tokens = rgo_investment_tokens_2(state, nation, province, c);
					auto local_investment = available_investment * local_tokens / total_tokens;

					auto current_efficiency = state.world.province_get_rgo_efficiency(province, c);
					auto investment_efficiency = state.world.province_get_rgo_max_efficiency(province, c) / (1.f + current_efficiency);

					{
						auto current_employment = state.world.province_get_rgo_target_employment(province, c);
						auto priority_from_max_size = current_max_size < 1.f ? 0.f : std::max(0.01f, 1.f - (current_size / current_max_size));
						auto priority_from_employment = current_size < 1.f ? 1.f : std::max(0.01f, (current_employment / current_size - 0.8f) * 5.f);
						auto expansion_priority = priority_from_max_size * priority_from_employment;
						auto total_local_priority = 1.f + expansion_priority;

						auto investment_rgo_expansion = local_investment * expansion_priority / total_local_priority;
						auto investment_rgo_efficiency = local_investment * 1.f / total_local_priority;

						auto size = state.world.province_get_rgo_size(province, c);

						// EFFICIENCY
						{
							auto efficiency_growth = 1.f;
							auto& efficiency_inputs = state.world.commodity_get_rgo_efficiency_inputs(c);
							auto total_can_afford = 0.f;
							for(uint8_t i = 0; i < economy::small_commodity_set::set_size; i++) {
								auto cid = efficiency_inputs.commodity_type[i];
								if(!cid) break;
								auto current_scale = (1.f + size) / state.defines.alice_rgo_per_size_employment;
								auto amount = 0.01f * efficiency_inputs.commodity_amounts[i] * current_scale;
								auto cost = amount * state.world.market_get_price(market, cid);
								auto can_afford = std::max(0.f, investment_rgo_efficiency / cost - 1.f);
								total_can_afford = total_can_afford + can_afford;
							}
							if(total_can_afford > 0.f) {
								for(uint8_t i = 0; i < economy::small_commodity_set::set_size; i++) {
									auto cid = efficiency_inputs.commodity_type[i];
									if(!cid) break;
									auto current_scale = (1.f + size) / state.defines.alice_rgo_per_size_employment;
									auto amount = 0.01f * efficiency_inputs.commodity_amounts[i] * current_scale;
									auto cost = amount * state.world.market_get_price(market, cid);
									auto can_afford = std::max(0.f, investment_rgo_efficiency / cost - 1.f);
									auto investment_into_category = investment_rgo_efficiency * can_afford / total_can_afford;
									auto can_actually_afford = std::min(1.f, investment_into_category / cost);
									economy::register_demand(state, market, cid, can_actually_afford * amount);
									auto probability_to_buy = state.world.market_get_actual_probability_to_buy(market, cid);
									auto growth = 0.05f * investment_efficiency * can_actually_afford * probability_to_buy;
									efficiency_growth = efficiency_growth * (1.f + decay_mult * growth);
									actually_spent = actually_spent + can_actually_afford * cost;
								}
							}

							auto decay = std::max(0.f, (current_efficiency - investment_efficiency) * decay_mult);
							state.world.province_set_rgo_efficiency(province, c, std::max(free_efficiency, current_efficiency + efficiency_growth - 1.f));
						}

						// SIZE

						/*
						For now expansion of rgo requires only skilled labor.
						Later it would be nice to have actual expansion costs.
						Larger rgos require more labor to expand
						*/

						float labor_to_expand_rgo = 20.f * (1.f + size / 100'000.f);

						auto expansion_cost = labor_to_expand_rgo * state.world.province_get_labor_price(province, economy::labor::basic_education);
						auto available_labor = state.world.province_get_labor_demand_satisfaction(province, economy::labor::basic_education);
						auto can_expand = std::max(0.f, std::min(500.f, investment_rgo_expansion / expansion_cost - 1.f));
						auto new_size = std::min(current_max_size, size * 0.9999f + can_expand * available_labor);
						state.world.province_set_rgo_size(province, c, new_size);

						auto labor_demand = state.world.province_get_labor_demand(province, economy::labor::basic_education);

#ifndef NDEBUG
						ve::apply([&](float value) {assert(std::isfinite(value) && value >= 0.f); }, labor_demand + labor_to_expand_rgo * can_expand);
#endif // !NDEBUG

						state.world.province_set_labor_demand(province, economy::labor::basic_education, labor_demand + labor_to_expand_rgo * can_expand);
						actually_spent = actually_spent + expansion_cost * can_expand * available_labor;
					}
					/*
					{
						auto size = state.world.province_get_rgo_size(province, c);
						auto current_efficiency = state.world.province_get_rgo_efficiency(province, c);
						auto new_efficiency = current_efficiency * 0.9999f;
						auto validated_efficiency = ve::select(size == 0.f, 0.f, ve::max(free_efficiency, new_efficiency));
						state.world.province_set_rgo_efficiency(province, c, validated_efficiency);

						auto new_size = std::min(current_max_size, size * 0.9999f);
						state.world.province_set_rgo_size(province, c, new_size);
					}
					*/
					if(!state.world.commodity_get_is_mine(c)) {
						total_agriculture += state.world.province_get_rgo_size(province, c);
					}
				});

				//respect max rgo size
				auto max_agriculture_size = state.world.province_get_rgo_base_size(province);

				if(total_agriculture > max_agriculture_size) {
					state.world.for_each_commodity([&](auto c) {
						if(state.world.commodity_get_is_mine(c)) return;
						auto size = state.world.province_get_rgo_size(province, c);
						state.world.province_set_rgo_size(province, c, size * max_agriculture_size / total_agriculture);
					});
				}

				// guard against fp errors
				state.world.nation_set_private_investment(nation, std::max(0.f, state.world.nation_get_private_investment(nation) - actually_spent));
			});

		}, owner_nids, markets, sids);
#endif
	});

	update_production_consumption(state);

	// we can't put it between demand and supply generation!
	/*
	update_trade_routes_volume(
		state,
		presimulation ? true : false,
		export_tariff_buffer,
		import_tariff_buffer,
		coastal_capital_buffer,
		state_naval_trade_is_blockaded
	);
	*/
}

void daily_update(sys::state& state, bool presimulation, float presimulation_stage, int date_offset) {
	uint32_t total_commodities = state.world.commodity_size();

	sanity_check(state);

	set_profile_point(state, "start");

	// per nation buffers
	static auto current_stockpile_budget = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto expected_stockpile_total_cost = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	static auto market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(1);
	static ve::vectorizable_buffer<float, dcon::nation_id> invention_count = ve::vectorizable_buffer<float, dcon::nation_id>(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if(new_count > old_count) {
			current_stockpile_budget = state.world.nation_make_vectorizable_float_buffer();
			expected_stockpile_total_cost = state.world.nation_make_vectorizable_float_buffer();
			market_leader = ve::vectorizable_buffer<dcon::nation_id, dcon::nation_id>(new_count);
			invention_count = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}

	// per market buffers
	static auto export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	static auto import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.market_size();
		if(new_count > old_count) {
			export_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			import_tariff_buffer = state.world.market_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}

	// per state instance buffers
	static auto coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
	static auto state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.state_instance_size();
		if(new_count > old_count) {
			coastal_capital_buffer = ve::vectorizable_buffer<dcon::province_id, dcon::state_instance_id>(state.world.state_instance_size());
			state_naval_trade_is_blockaded = ve::vectorizable_buffer<float, dcon::state_instance_id>(state.world.state_instance_size());
			old_count = new_count;
		}
	}

	// per pop buffers
	static ve::vectorizable_buffer<float, dcon::pop_id> income_buffer(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.pop_size();
		if(new_count > old_count) {
			income_buffer = state.world.pop_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}

	set_profile_point(state, "create_buffers");

	concurrency::parallel_for(0, 12, [&](int32_t index) {
		switch(index) {
		case 0:
			update_yesterday_govt_stockpiles_cache(state);
			break;
		case 1:
			state.world.for_each_pop_type([&](dcon::pop_type_id t) {
				state.world.execute_serial_over_market([&](auto nids) {
					state.world.market_set_life_needs_costs(nids, t, ve::fp_vector{});
				});
			});
			break;
		case 2:
			state.world.for_each_pop_type([&](dcon::pop_type_id t) {
				state.world.execute_serial_over_market([&](auto nids) {
					state.world.market_set_everyday_needs_costs(nids, t, ve::fp_vector{});
				});
			});
			break;
		case 3:
			state.world.for_each_pop_type([&](dcon::pop_type_id t) {
				state.world.execute_serial_over_market([&](auto nids) {
					state.world.market_set_luxury_needs_costs(nids, t, ve::fp_vector{});
				});
			});
			break;
		case 4:
			state.world.execute_serial_over_nation([&](auto id) {
				state.world.nation_set_subsidy_token_total(id, 0.f);
			});
			break;
		case 5:
			state.world.execute_serial_over_nation([&](auto ids) {
				auto treasury = state.world.nation_get_treasury(ids);
				state.world.nation_set_last_treasury(ids, treasury);
			});
			break;
		case 6:
			state.world.execute_serial_over_nation([&](auto nations) {
				economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
					state.world.nation_set_commodity_float_buffer_1(nations, commodity, ve::fp_vector{ 0.0f });
				});
			});
			break;
		case 7:
			state.world.execute_serial_over_nation([&](auto ids) {
				expected_stockpile_total_cost.set(ids, 0.0f);
			});
			break;
		case 8:
			state.world.execute_serial_over_nation([&](auto ids) {
				auto base_budget = state.world.nation_get_last_base_budget(ids);
				auto stockpiles_priority = ve::to_float(state.world.nation_get_stockpile_spending(ids)) / 100.f;
				current_stockpile_budget.set(ids, ve::max(0.f, base_budget * stockpiles_priority));
			});
			break;
		case 9:
			state.world.execute_serial_over_nation([&](auto nations) {
				invention_count.set(nations, 0.f);
			});
			break;
		case 10:
			update_land_ownership(state);
			break;
		case 11:
			update_local_subsistence_factor(state);
			break;
		}
	});

	set_profile_point(state, "reset buffers");

	// uses nation_commodity_float_buffer_1
	update_government_stockpile_market_demand_weights(state); // Functions will later will use these calculated values

	/* initialization parallel block */

	concurrency::parallel_for(0, 12, [&](int32_t index) {
		switch(index) {
		case 0:
			populate_navy_consumption(state);
			break;
		case 1:
			// uses nation_commodity_float_buffer_1
			populate_government_stockpile_demand(state, expected_stockpile_total_cost, current_stockpile_budget);
			decay_government_stockpiles(state);
			break;
		case 2:
			populate_army_consumption(state);
			break;
		case 3:
			populate_private_construction_consumption(state);			
			break;
		case 4:
			populate_government_construction_consumption(state);
			break;
		case 5:
			// done
			state.world.execute_serial_over_market([&](auto ids) {
				auto sids = state.world.market_get_zone_from_local_market(ids);
				auto nids = state.world.state_instance_get_nation_from_state_ownership(sids);
				ve::apply([&](auto nid, auto mid) {
					import_tariff_buffer.set(mid, effective_tariff_import_rate(state, nid, mid));
				}, nids, ids);
			});
			break;
		case 6:
			// done
			state.world.execute_serial_over_market([&](auto ids) {
				auto sids = state.world.market_get_zone_from_local_market(ids);
				auto nids = state.world.state_instance_get_nation_from_state_ownership(sids);
				ve::apply([&](auto nid, auto mid) {
					export_tariff_buffer.set(mid, effective_tariff_export_rate(state, nid, mid));
				}, nids, ids);
			});
			break;
		case 7:
			state.world.execute_serial_over_state_instance([&](auto ids) {
				auto capitals = ve::apply([&](auto sid) {
					return province::state_get_coastal_capital(state, sid);
				}, ids);
				coastal_capital_buffer.set(ids, capitals);
			});
			break;
		case 8:
			state.world.for_each_nation([&](auto ids) {
				market_leader.set(ids, nations::get_market_leader(state, ids));
			});
			break;
		case 9:
			update_factory_triggered_modifiers(state);
			break;
		case 10:
			state.world.execute_serial_over_state_instance([&](auto sids) {
				// US3AC17. if market capital controller is at war with market coastal controller is different
				// or it's actually blockaded
				// consider province blockaded
				// US3AC16

				auto capital = state.world.state_instance_get_capital(sids);
				auto port = coastal_capital_buffer.get(sids);
				auto controller_capital = state.world.province_get_nation_from_province_control(capital);
				auto controller_port = state.world.province_get_nation_from_province_control(port);

				auto occupied = ve::apply([&](auto capital_nation, auto port_nation) {
					return military::are_at_war(state, capital_nation, port_nation);
				}, controller_capital, controller_port);

				state_naval_trade_is_blockaded.set(sids, ve::select(port == dcon::province_id{} || occupied || state.world.province_get_is_blockaded(port), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f }));
			});
			break;
		case 11 :
			state.world.for_each_invention([&](auto iid) {
				state.world.execute_serial_over_nation([&](auto nations) {
					auto count =
						invention_count.get(nations)
						+ ve::select(state.world.nation_get_active_inventions(nations, iid), ve::fp_vector(1.0f), ve::fp_vector(0.0f));
					invention_count.set(nations, count);
				});
			});
			break;
		}
	});

	set_profile_point(state, "set expensive buffers");

	/*

	Subsidy tokens are  granted per effective produced unit to encourage actual production rather than raw employment or size.
	Here we calculate their amount and pay them out immediately.
	It might generate or delete a certain amount of money as production volume
	changes all the time but it would be the most efficient way to start and introduced leaks are not that bad.

	*/

	province::for_each_nation_owned_province_parallel_over_nation(state, [&](dcon::nation_id nation, dcon::province_id province) {
		auto area = state.world.province_get_state_membership(province);
		state.world.province_for_each_factory_location(province, [&](dcon::factory_location_id factory_location) {
			auto factory = state.world.factory_location_get_factory(factory_location);
			auto building_type = state.world.factory_get_building_type(factory);
			auto output_type = state.world.factory_type_get_output(building_type);
			auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, output_type));
			auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, output_type));
			if(priority || priority_local) {
				auto base_output = state.world.factory_type_get_output_amount(building_type);
				auto factory_output = state.world.factory_get_output(factory);
				auto effective_output = factory_output / base_output;
				auto tokens = state.world.nation_get_subsidy_token_total(nation);
				state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

				auto current_money = state.world.province_get_factory_bank(province);
				auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
				state.world.province_set_factory_bank(province, current_money + last_token_price * effective_output);
			}
		});
	});

	province::for_each_nation_owned_province_parallel_over_nation(state, [&](dcon::nation_id nation, dcon::province_id province) {
		state.world.for_each_commodity([&](auto cid) {
			auto base_output = state.world.commodity_get_rgo_amount(cid);
			if(base_output == 0.f) return;

			auto area = state.world.province_get_state_membership(province);
			auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
			auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));

			if(priority || priority_local) {
				auto rgo_output = state.world.province_get_rgo_output(province, cid);
				auto effective_output = rgo_output / base_output;
				auto tokens = state.world.nation_get_subsidy_token_total(nation);
				state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

				auto current_money = state.world.province_get_rgo_bank(province);
				auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
				state.world.province_set_rgo_bank(province, current_money + last_token_price * effective_output);
			}
		});
	});

	province::for_each_nation_owned_province_parallel_over_nation(state, [&](dcon::nation_id nation, dcon::province_id province) {
		state.world.for_each_commodity([&](auto cid) {
			auto base_output = state.world.commodity_get_artisan_output_amount(cid);
			if(base_output == 0.f) return;
			auto area = state.world.province_get_state_membership(province);
			auto priority = state.world.nation_get_production_directive(nation, production_directives::to_key(state, cid));
			auto priority_local = state.world.state_instance_get_production_directive(area, production_directives::to_key(state, cid));
			if(priority || priority_local) {
				auto output = state.world.province_get_artisan_actual_production(province, cid);
				auto effective_output = output / base_output;
				auto tokens = state.world.nation_get_subsidy_token_total(nation);
				state.world.nation_set_subsidy_token_total(nation, tokens + effective_output);

				auto current_money = state.world.province_get_artisan_bank(province);
				auto last_token_price = state.world.nation_get_subsidy_token_price(nation);
				state.world.province_set_artisan_bank(province, current_money + last_token_price * effective_output);
			}
		});
	});

	if(presimulation) {
		state.world.execute_serial_over_province([&](auto pid) {
			state.world.province_set_artisan_bank(pid, 0.f);
			state.world.province_set_factory_bank(pid, 0.f);
		});
	}

	if(state.trade_route_cached_values_out_of_date) {
		state.trade_route_cached_values_out_of_date = false;

		ankerl::unordered_dense::map<int32_t, bool> direct_block;
		ankerl::unordered_dense::map<int32_t, bool> trade_closed;
		ankerl::unordered_dense::map<int32_t, bool> direct_no_tariffs;
		ankerl::unordered_dense::map<int32_t, bool> no_tariffs;

		// US3AC9. Wartime embargoes

		state.world.for_each_war([&](auto war) {
			state.world.war_for_each_war_participant(war, [&](auto attacker_candidate) {
				if(!state.world.war_participant_get_is_attacker(attacker_candidate)) return;
				auto attacker = state.world.war_participant_get_nation(attacker_candidate);

				state.world.war_for_each_war_participant(war, [&](auto defender_candidate) {
					if(state.world.war_participant_get_is_attacker(defender_candidate)) return;
					auto defender = state.world.war_participant_get_nation(defender_candidate);

					auto index_pair = attacker.index() * state.world.nation_size() + defender.index();
					auto index_pair_T = defender.index() * state.world.nation_size() + attacker.index();
					direct_block[index_pair] = true;
					direct_block[index_pair_T] = true;
				});
			});
		});

		// US3AC10. diplomatic embargos
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_embargo(rel)) {
				dcon::nation_id source = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id target = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = source.index() * state.world.nation_size() + target.index();
				auto index_pair_T = target.index() * state.world.nation_size() + source.index();

				direct_block[index_pair] = true;
				direct_block[index_pair_T] = true;
			}
		});

		// US3AC11. US3AC12. sphere joins market leader
		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);
				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_block.find(indices[0]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_block.find(indices[1]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_block.find(indices[2]) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}

				{
					if(direct_block.find(base_pair) != direct_block.end()) {
						trade_closed[base_pair] = true;
					}
				}
			});
		});

		// US3AC15. Equal/unequal trade treaties
		state.world.for_each_unilateral_relationship([&](auto rel) {
			if(state.world.unilateral_relationship_get_no_tariffs_until(rel)) {
				dcon::nation_id n1 = state.world.unilateral_relationship_get_source(rel);
				dcon::nation_id n2 = state.world.unilateral_relationship_get_target(rel);

				auto index_pair = n1.index() * state.world.nation_size() + n2.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		// Reflexivity of free trade
		state.world.for_each_nation([&](auto nid) {
			auto index_pair = nid.index() * state.world.nation_size() + nid.index();
			direct_no_tariffs[index_pair] = true;
		});
		state.world.for_each_nation([&](auto nid) {
			dcon::nation_id sphere = state.world.nation_get_in_sphere_of(nid);
			if(sphere) {
				auto index_pair = nid.index() * state.world.nation_size() + sphere.index();
				direct_no_tariffs[index_pair] = true;
			}
		});
		state.world.for_each_overlord([&](auto ovid) {
			dcon::nation_id subject = state.world.overlord_get_subject(ovid);
			dcon::nation_id overlord = state.world.overlord_get_ruler(ovid);
			auto index_pair = subject.index() * state.world.nation_size() + overlord.index();
			direct_no_tariffs[index_pair] = true;
		});

		state.world.for_each_nation([&](auto A) {
			dcon::nation_id market_leader_A = market_leader.get(A);
			state.world.for_each_nation([&](auto B) {
				dcon::nation_id market_leader_B = market_leader.get(B);

				int32_t base_pair = A.index() * state.world.nation_size() + B.index();

				int32_t indices[3]{
					market_leader_A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
					market_leader_A.index() * int32_t(state.world.nation_size()) + B.index(),
					A.index() * int32_t(state.world.nation_size()) + market_leader_B.index(),
				};

				if(market_leader_A && market_leader_B) {
					if(direct_no_tariffs.find(indices[0]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_A) {
					if(direct_no_tariffs.find(indices[1]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				if(market_leader_B) {
					if(direct_no_tariffs.find(indices[2]) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
				{
					if(direct_no_tariffs.find(base_pair) != direct_no_tariffs.end()) {
						no_tariffs[base_pair] = true;
					}
				}
			});
		});

		// update cache:

		state.world.for_each_trade_route([&](auto route) {
			auto A = state.world.trade_route_get_origin(route);
			auto B = state.world.trade_route_get_target(route);
			auto s_A = state.world.market_get_zone_from_local_market(A);
			auto s_B = state.world.market_get_zone_from_local_market(B);

			auto capital_A = state.world.state_instance_get_capital(s_A);
			auto capital_B = state.world.state_instance_get_capital(s_B);

			auto controller_A = state.world.province_get_nation_from_province_control(capital_A);
			auto controller_B = state.world.province_get_nation_from_province_control(capital_B);

			auto index = controller_A.index() * state.world.nation_size() + controller_B.index();
			auto index_T = controller_B.index() * state.world.nation_size() + controller_A.index();

			if(trade_closed.find(index) != trade_closed.end()) {
				state.world.trade_route_set_is_trade_forbidden(route, true);
			} else {
				state.world.trade_route_set_is_trade_forbidden(route, false);
			}

			if(no_tariffs.find(index) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_origin(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_origin(route, true);
			}

			if(no_tariffs.find(index_T) != no_tariffs.end()) {
				state.world.trade_route_set_is_tariff_applied_target(route, false);
			} else {
				state.world.trade_route_set_is_tariff_applied_target(route, true);
			}
		});
	};

	set_profile_point(state, "update trade cache and buffers");

	sanity_check(state);

	/* end initialization parallel block */

	// store available port capacity ratio per market

	auto port_availability = state.world.market_make_vectorizable_float_buffer();
	auto price_port_capacity = state.world.market_make_vectorizable_float_buffer();
	auto port_total_weight = state.world.market_make_vectorizable_float_buffer();

	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
		auto price = state.world.province_get_service_price(pid, services::list::port_capacity);
		auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
		auto local_weight = size / (price_properties::service::epsilon + price);
		port_total_weight.set(mid, port_total_weight.get(mid) + local_weight);
	});
	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
		auto price = state.world.province_get_service_price(pid, services::list::port_capacity);
		auto size = 100.f + state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
		auto local_weight = size / (price_properties::service::epsilon + price);
		auto total_weight = port_total_weight.get(mid);
		auto local_demand_ratio = local_weight / total_weight;
		auto sat = state.world.province_get_service_satisfaction(pid, services::list::port_capacity);
		port_availability.set(mid, port_availability.get(mid) + local_demand_ratio * sat);
		price_port_capacity.set(mid, price_port_capacity.get(mid) + local_demand_ratio * price);
	});

	// update trade volume based on potential profits right at the start
	// we can't put it between demand and supply generation!

	/*
	update_trade_routes_volume(
		state,
		presimulation ? true : false,
		export_tariff_buffer,
		import_tariff_buffer,
		coastal_capital_buffer,
		state_naval_trade_is_blockaded
	);
	*/

	set_profile_point(state, "trade volume");

	sanity_check(state);
	
	state.world.execute_parallel_over_market([&](auto markets) {
		// reset gdp
		state.world.market_set_gdp(markets, 0.f);

		auto states = state.world.market_get_zone_from_local_market(markets);
		auto nations = state.world.state_instance_get_nation_from_state_ownership(states);

		auto invention_factor = invention_count.get(nations) * state.defines.invention_impact_on_demand + 1.0f;

		// STEP 0: gather total demand costs:
		/*
		- Each pop strata and needs type has its own demand modifier, calculated as follows:
		- (national-modifier-to-goods-demand + define:BASE_GOODS_DEMAND) x (national-modifier-to-specific-strata-and-needs-type + 1) x
		(define:INVENTION_IMPACT_ON_DEMAND x number-of-unlocked-inventions + 1, but for non-life-needs only)
		- Each needs demand is also multiplied by  2 - the nation's administrative efficiency if the pop has education / admin /
		military income for that need category
		- We calculate an adjusted pop-size as (0.5 + pop-consciousness / define:PDEF_BASE_CON) x (for non-colonial pops: 1 +
		national-plurality (as a fraction of 100)) x pop-size
		*/

		uint32_t total_commodities = state.world.commodity_size();

		// poor strata update

		{
			uint8_t strata_filter = 0;
			auto offset_life = sys::national_mod_offsets::poor_life_needs;
			auto offset_everyday = sys::national_mod_offsets::poor_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::poor_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);

#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
					);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
					);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
					);
#endif
				});
			}
		}

		// middle strata update

		{
			uint8_t strata_filter = 1;
			auto offset_life = sys::national_mod_offsets::middle_life_needs;
			auto offset_everyday = sys::national_mod_offsets::middle_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::middle_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);
#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
							);
#endif
				});
			}
		}

		// rich strata update

		{
			uint8_t strata_filter = 2;
			auto offset_life = sys::national_mod_offsets::rich_life_needs;
			auto offset_everyday = sys::national_mod_offsets::rich_everyday_needs;
			auto offset_luxury = sys::national_mod_offsets::rich_luxury_needs;

			auto life_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_life) + 1.0f)
				* state.defines.alice_lf_needs_scale;
			auto everyday_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_everyday) + 1.0f)
				* state.defines.alice_ev_needs_scale;
			auto luxury_needs_mult =
				(state.world.nation_get_modifier_values(nations, offset_luxury) + 1.0f)
				* state.defines.alice_lx_needs_scale;

			for(uint32_t i = 1; i < total_commodities; ++i) {
				dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

				auto prices = state.world.market_get_price(markets, c);

				auto is_active = state.world.nation_get_unlocked_commodities(nations, c);
				auto life_weights = state.world.market_get_life_needs_weights(markets, c);
				auto everyday_weights = state.world.market_get_everyday_needs_weights(markets, c);
				auto luxury_weights = state.world.market_get_luxury_needs_weights(markets, c);

				state.world.for_each_pop_type([&](dcon::pop_type_id pop_type) {
					if(state.world.pop_type_get_strata(pop_type) != strata_filter) {
						return;
					}

					auto life_base = state.world.pop_type_get_life_needs(pop_type, c);
					auto everyday_base = state.world.pop_type_get_everyday_needs(pop_type, c);
					auto luxury_base = state.world.pop_type_get_luxury_needs(pop_type, c);

					auto life_costs = prices * life_base * life_needs_mult * life_weights;
					auto everyday_costs = prices * everyday_base * everyday_needs_mult * everyday_weights * invention_factor;
					auto luxury_costs = prices * luxury_base * luxury_needs_mult * luxury_weights * invention_factor;

					state.world.market_set_life_needs_costs(
						markets,
						pop_type,
						state.world.market_get_life_needs_costs(markets, pop_type) + life_costs
					);
					state.world.market_set_everyday_needs_costs(
						markets,
						pop_type,
						state.world.market_get_everyday_needs_costs(markets, pop_type) + everyday_costs
					);
					state.world.market_set_luxury_needs_costs(
						markets,
						pop_type,
						state.world.market_get_luxury_needs_costs(markets, pop_type) + luxury_costs
					);

#ifndef NDEBUG
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_life_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_everyday_needs_costs(markets, pop_type)
							);
					ve::apply(
						[](float x) {
							assert(std::isfinite(x));
						}, state.world.market_get_luxury_needs_costs(markets, pop_type)
							);
#endif
				});
			}
		}
	});

	// for new needs (much more elegant...)
	state.world.execute_parallel_over_market([&](auto markets) {
		state.world.for_each_consumption_category([&](auto cat_id) {
			ve::fp_vector total {0.f};
			state.world.for_each_commodity([&](auto c) {
				auto price = state.world.market_get_price(markets, c);
				auto index = c.index() + cat_id.index() * state.world.commodity_size();
				auto consumption_weight = state.world.consumption_category_get_weights(cat_id, c);
				auto w = state.world.market_get_local_consumption_weights(markets, index);
#ifndef NDEBUG
				ve::apply(
					[](float amount) {
							assert(std::isfinite(amount) && amount >= 0.f);
					}, price* consumption_weight* w
				);
#endif
				total = total + price * consumption_weight * w;
			});
#ifndef NDEBUG
			ve::apply(
				[](float amount) {
					assert(std::isfinite(amount) && amount >= 0.f);
				}, total
			);
#endif
			state.world.market_set_cost_per_consumption_category(markets, cat_id, total);
		});
	});

	// PROFILE
	set_profile_point(state, "needs_costs");

	// CLEAR ALL DEMAND

	concurrency::parallel_for(0, 12, [&](int32_t index) {
		switch(index) {
		case 0:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.for_each_commodity([&](dcon::commodity_id c) {
					auto old_demand = state.world.market_get_demand(markets, c);
					auto old_aggregated_demand = state.world.market_get_aggregated_demand_history(markets, c);
					if(presimulation) {
						state.world.market_set_aggregated_demand_history(markets, c, old_aggregated_demand * 0.001f + old_demand * 0.999f);
					} else {
						state.world.market_set_aggregated_demand_history(markets, c, old_aggregated_demand * 0.95f + old_demand * 0.05f);
					}

					state.world.market_set_demand(markets, c, ve::fp_vector{});
				});
			});
			break;
		case 2:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.for_each_commodity([&](dcon::commodity_id c) {
					state.world.market_set_intermediate_demand(markets, c, ve::fp_vector{});
				});
			});
			break;
		case 3:
			for(int32_t i = 0; i < labor::total; i++) {
				province::ve_for_each_land_province(state, [&](auto ids) {
					state.world.province_set_labor_demand(ids, i, 0.f);
				});
			}
			break;
		case 4:
			services::reset_demand(state);
			break;
		case 5:
			state.world.for_each_pop_type([&](auto pt) {
				state.world.execute_serial_over_market([&](auto ids) {
					state.world.market_set_life_needs_scale(ids, pt, 0.f);
				});
			});
			break;
		case 6:
			state.world.for_each_pop_type([&](auto pt) {
				state.world.execute_serial_over_market([&](auto ids) {
					state.world.market_set_everyday_needs_scale(ids, pt, 0.f);
				});
			});
			break;
		case 7:
			state.world.for_each_pop_type([&](auto pt) {
				state.world.execute_serial_over_market([&](auto ids) {
					state.world.market_set_luxury_needs_scale(ids, pt, 0.f);
				});
			});
			break;
		case 8:
			state.world.execute_serial_over_market([&](auto ids) {
				state.world.for_each_consumption_category([&](auto cat) {
					state.world.market_set_demand_per_consumption_category(ids, cat, 0.f);
				});
			});
			break;
		case 9:
			state.world.execute_serial_over_market([&](auto ids) {
				state.world.market_set_total_port_capacity(ids, 0.f);
			});
			break;
		case 10:
			state.world.execute_serial_over_market([&](auto ids) {
				state.world.market_set_land_transportation_demand(ids, 0.f);
			});
			break;
		case 11:
			state.world.execute_serial_over_market([&](auto ids) {
				state.world.market_set_naval_transportation_demand(ids, 0.f);
			});
			break;
		}
	});

	// rgo/factories/artisans consumption
	update_production_consumption(state);

	set_profile_point(state, "production_consumption");

	state.world.for_each_commodity([&](auto cid) {
		bool is_potential_rgo = state.world.commodity_get_rgo_amount(cid) > 0.f;
		bool already_known_to_exist = state.world.commodity_get_actually_exists_in_nature(cid);
		if(is_potential_rgo && !already_known_to_exist) {
			for(auto pid : state.world.in_province) {
				auto potential = state.world.province_get_rgo_size(pid, cid);
				if(potential > 0) {
					state.world.commodity_set_actually_exists_in_nature(cid, true);
					break;
				}
			}
		}
	});

	sanity_check(state);

#ifdef NOT_DEFINED
	// STEP 3 update pops consumption:

	auto potential_ratio_life = state.world.pop_make_vectorizable_float_buffer();
	auto potential_ratio_housing= state.world.pop_make_vectorizable_float_buffer();
	auto potential_ratio_everyday = state.world.pop_make_vectorizable_float_buffer();
	auto potential_ratio_luxury = state.world.pop_make_vectorizable_float_buffer();
	auto potential_ratio_education_public = state.world.pop_make_vectorizable_float_buffer();
	auto potential_ratio_education_private = state.world.pop_make_vectorizable_float_buffer();
	auto demand_life = state.world.pop_make_vectorizable_float_buffer();
	auto demand_housing = state.world.pop_make_vectorizable_float_buffer();
	auto demand_everyday = state.world.pop_make_vectorizable_float_buffer();
	auto demand_luxury = state.world.pop_make_vectorizable_float_buffer();
	auto demand_paid_education = state.world.pop_make_vectorizable_float_buffer();
	std::vector<ve::vectorizable_buffer<float, dcon::pop_id>> demand_consumption_category {};
	state.world.for_each_consumption_category([&](auto cat) {
		demand_consumption_category.emplace_back(ve::vectorizable_buffer<float, dcon::pop_id>(state.world.pop_size()));
	});
	auto satisfaction_from_subsistence = state.world.pop_make_vectorizable_float_buffer();

	pops::update_consumption(
		state,
		invention_count,
		potential_ratio_life,
		potential_ratio_housing,
		potential_ratio_everyday,
		potential_ratio_luxury,
		potential_ratio_education_public,
		potential_ratio_education_private,
		demand_life,
		demand_housing,
		demand_everyday,
		demand_luxury,
		demand_paid_education,
		satisfaction_from_subsistence,
		demand_consumption_category
	);

	set_profile_point(state, "pops_consumption");

	sanity_check(state);


	// national income is handled at the end,
	// so we have money stockpile from previous day
	// and can safely use it to calculate spendings and generate demand

	// national spendings ideally should follow the priority:
	// 1) loans interest (otherwise interest gets diluted and can be abused with high admin spending)
	// 2) international treaties (currently it is handled somewhere else, requires investigation)
	// 3) admin spending (can't pay for stuff without taxes)
	// 4) everything else

	// if nation is unable to pay interest it is considered bankrupt
	// money stockpiles after interest are considered BASEBUDGET
	// admin spendings take a ratio of BASEBUDGET to pay for bureaucracy
	// denote them as ADMIN
	// the rest of spending is divided into two groups:
	// 1) LEGACY: these expenses depend on some predefined value
	// 2) RELATIVE: these spendings are equal to ratio of BASEBUDGET
	// if LEGACY and RELATIVE - ADMIN expenses are larger than BASEBUDGET - ADMIN
	// then scale them down

	// AI is considered bankrupt when money stockpiles are lower than 0,
	// which should happen basically never

	// we have to recalculate loan related variables every new round, because they depend on themselves

	static auto spent_on_construction_buffer = ve::vectorizable_buffer<float,dcon::nation_id>(uint32_t(1));
	{
		static uint32_t old_count = 1;
		auto new_count = state.world.nation_size();
		if(new_count > old_count) {
			spent_on_construction_buffer = state.world.nation_make_vectorizable_float_buffer();
			old_count = new_count;
		}
	}
	
	concurrency::combinable<std::vector<dcon::nation_id>> bankrupt_nations;
	state.world.execute_serial_over_nation([&](auto nations) { spent_on_construction_buffer.set(nations, ve::fp_vector{ 0.0f }); });
	nations::parallel_for_each_existing_nation(state, [&](dcon::nation_id n) {

		// handle loans
		bool is_bankrupt = false;
		{
			auto current_money = state.world.nation_get_treasury(n);
			if(state.world.nation_get_is_player_controlled(n)) {
				auto max_loan_amount = max_loan(state, n);
				auto current_loan = state.world.nation_get_local_loan(n);
				auto current_interest = interest_payment(state, n);
				auto required_additional_loan = 0.f;
				auto current_bank_money = state.world.nation_get_national_bank(n);

				if(current_money < current_interest) {
					required_additional_loan = required_additional_loan - current_money;
				}

				if(current_money < current_interest && current_loan + required_additional_loan > max_loan_amount) {
					is_bankrupt = true;
				} else if(current_money > current_interest) {
					// can pay interest without new loans
					state.world.nation_set_treasury(n, current_money - current_interest);
					state.world.nation_set_national_bank(n, current_bank_money + current_interest);
				} else {
					// we have to take additional loan to pay interest and we are able to do it
					state.world.nation_set_local_loan(n, current_loan + required_additional_loan);
					state.world.nation_set_treasury(n, 0);
				}
			} else {
				if(current_money < 0) {
					is_bankrupt = true;
				}
			}
		}

		if(is_bankrupt) {
			bankrupt_nations.local().push_back(n);
		} else {
			float spending_scale = 1.0f;

			// interest is paid and we are not bankrupt,
			// now we can assume that money stockpile is equal to BASE_BUDGET
			auto base_budget = state.world.nation_get_treasury(n);
			auto additional_funding = 0.f;
			auto costs = full_spending_cost(state, n, base_budget);
			auto admin = costs.administration;
			auto required_additional_funding = std::max(0.f, costs.total - base_budget);
			auto current_loan = state.world.nation_get_local_loan(n);

			assert(costs.total >= 0.f);

			// if loan is required, then take as much as you can

			if(can_take_loans(state, n)) {
				auto available_loan = std::max(0.f, max_loan(state, n) - current_loan);
				additional_funding = std::min(required_additional_funding, available_loan);
			}

			// by definition, ADMIN must be lower or equal than BASE_BUDGET, so we can always pay for admin budget

			if(base_budget + additional_funding >= costs.total) {
				spending_scale = 1.f;
			} else {
				spending_scale =
					(costs.total - admin < 0.001f)
					? 1.f
					: (base_budget + additional_funding - admin) / (costs.total - admin);
			}

			assert(spending_scale >= 0);
			assert(std::isfinite(spending_scale));

			// spend money
			state.world.nation_set_treasury(
				n, base_budget - std::min(base_budget, (costs.total - admin) * spending_scale + admin)
			);
			state.world.nation_set_spending_level(n, spending_scale);
			state.world.nation_set_last_base_budget(n, base_budget);

			

			if (additional_funding > 0.f) {
				// take the loan
				state.world.nation_set_local_loan(n, current_loan + additional_funding);
				state.world.nation_set_treasury(n, 0);
			} else {
				// repay the loan
				auto money_before = state.world.nation_get_treasury(n);
				auto paid_loan = std::min(money_before, current_loan);
				auto remaining_loan_after = std::max(0.f, current_loan - paid_loan);
				auto money_after = std::max(0.f, money_before - paid_loan);

				state.world.nation_set_local_loan(n, remaining_loan_after);
				state.world.nation_set_treasury(n, money_after);
				// we do not increase national bank
				// because it stores the sum of loaned money and money available for a loan
			}

			spent_on_construction_buffer.set(n, spending_scale * costs.construction);

			// use calculated values to perform actual consumption

			auto total_tokens = state.world.nation_get_subsidy_token_total(n);
			auto total_token_budget = base_budget * spending_scale * float(state.world.nation_get_subsidies_spending(n)) / 100.f;
			auto token_price = total_tokens == 0.f ? 0.f : total_token_budget / (total_tokens + economy::numerical::commodity_unit::epsilon);
			assert(std::isfinite(token_price));
			state.world.nation_set_subsidy_token_price(n, token_price);
			update_national_consumption(state, n, spending_scale, base_budget);
			update_consumption_administration(state, n, base_budget);
		}
	
	});


	// private budget. Must be run in a seperate loop as it iterates over nation owned states, whereas the prev national consumption loop iterates over nation controlled states
	nations::parallel_for_each_existing_nation(state, [&](dcon::nation_id n) {
		float private_spending_scale = 1.0f;
		float pi_total = full_private_investment_cost(state, n);
		float perceived_spending = pi_total;
		float pi_budget = state.world.nation_get_private_investment(n) * investment_pool_investment_per_day;
		private_spending_scale = perceived_spending <= pi_budget ? 1.0f : pi_budget / perceived_spending;
		state.world.nation_set_private_investment_effective_fraction(n, private_spending_scale);
		state.world.nation_set_private_investment(
			n,
			std::max(0.0f, state.world.nation_get_private_investment(n) - pi_total * private_spending_scale)
		);

		update_private_consumption(state, n, private_spending_scale);
	});

	{
		auto total_vector = bankrupt_nations.combine([](auto& a, auto& b) {
			std::vector<dcon::nation_id> result(a.begin(), a.end());
			result.insert(result.end(), b.begin(), b.end());
			return result;
		});
		std::sort(total_vector.begin(), total_vector.end(), [](auto a, auto b) { return a.value < b.value; });
		for(auto& n : total_vector) {		
			go_bankrupt(state, n);
			state.world.nation_set_spending_level(n, 0.f);
			state.world.nation_set_last_base_budget(n, 0.f);
			update_national_consumption(state, n, 0.f, 0.f);
			update_consumption_administration(state, n, 0.f);
		}
	}

	sanity_check(state);

	set_profile_point(state, "national_consumption");

	concurrency::parallel_invoke(
		[&]() {
			advanced_province_buildings::update_national_size(state);
		},
		[&]() {
			advanced_province_buildings::update_private_size(state);
		}
	);

	advanced_province_buildings::update_consumption(state);

	set_profile_point(state, "apb_consumption");

	sanity_check(state);

	update_trade_routes_consumption(state);

	set_profile_point(state, "routes_consumption");

	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
		auto local_price = state.world.province_get_service_price(pid, services::list::port_capacity);
		auto local_size = state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
		auto local_weight = local_size / (price_properties::service::epsilon + local_price);
		auto current_weight = state.world.market_get_total_port_capacity_weight(mid);
		state.world.market_set_total_port_capacity_weight(mid, current_weight + local_weight);
		auto current_size = state.world.market_get_total_port_capacity(mid);
		state.world.market_set_total_port_capacity(mid, current_size + local_size);
	});
	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
		auto local_price = state.world.province_get_service_price(pid, services::list::port_capacity);
		auto local_size = state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
		auto local_weight = local_size / (price_properties::service::epsilon + local_price);
		auto current_weight = state.world.market_get_total_port_capacity_weight(mid);
		state.world.market_set_weighted_port_price_cost(mid, current_weight == 0.f ? 0.f : local_price * local_weight / current_weight);
	});

	state.world.execute_parallel_over_market([&](auto market){
		auto area = state.world.market_get_zone_from_local_market(market);
		auto nation = state.world.state_instance_get_nation_from_state_ownership(area);

		auto budget = state.world.market_get_stockpile(market, economy::money);

		auto arbitrage = state.world.market_get_arbitrage(market);
		auto investments = state.world.market_get_pop_investments(market);
		state.world.market_set_pop_investments(market, 0.f);
		auto unused_dividends = state.world.market_get_pop_dividends(market);
		auto sales = state.world.market_get_sales(market);
		state.world.market_set_sales(market, 0.f);
		auto purchases = state.world.market_get_purchases(market);
		auto export_gains = state.world.market_get_export_cut(market);
		auto import_spent = state.world.market_get_import_spending(market);

		auto reinvestment = ve::max(0.f, budget * 0.05f);
		auto debt_payment = ve::max(0.f, -budget * 0.5f);
		budget = budget - reinvestment + debt_payment;


		auto earn = ve::select(arbitrage > 0.f, arbitrage, 0.f) + export_gains + investments + unused_dividends + sales + reinvestment;

		auto next_dividends = earn * economy::pops::trade_dividends_rate;
		state.world.market_set_pop_dividends(market, next_dividends);
		state.world.market_set_last_pop_dividends(market, next_dividends);


		//auto total_spend = state.world.market_get_total_spend(market);

		auto supposed_construction_spending = 0.f;
		auto shipping_satisfaction = state.world.market_get_naval_transportation_demand_satisfaction(market);
		auto ship_building_spend = earn * ve::min(0.1f, (1.f - shipping_satisfaction));

		auto total_shipbuilding_costs = ve::fp_vector{ 0.f };

		auto current_size = state.world.market_get_total_port_capacity(market);
		auto ship_building_speed = current_size / 10000.f;		

		/*
		Ships scoring:
		Proportional to speed * hull / cost.
		*/
		ve::fp_vector total_score = 0.f;
		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if (def.is_land) continue;
			if (def.type != military::unit_type::transport) continue;
			auto researched = ve::select(def.active || state.world.nation_get_active_unit(nation, uid), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f });
			ve::fp_vector ship_cost = 0.f;
			auto& costs = def.build_cost;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				ship_cost = ship_cost + state.world.market_get_price(market, cid) * amount;
			}
			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed;}, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull;}, nation);
			auto score = researched * speed * hull / (price_properties::commodity::min + ship_cost);
			total_score = total_score + score;
		}

		ve::fp_vector total_abstract_cargo_potential{ 0.f };
		ve::fp_vector total_maintenance_costs{ 0.f };
		ve::fp_vector total_port_services_required{ 0.f };

		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if(def.is_land) continue;
			if(def.type != military::unit_type::transport) continue;
			auto researched = ve::select(def.active || state.world.nation_get_active_unit(nation, uid), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f });
			ve::fp_vector ship_cost = 0.f;
			auto& costs = def.build_cost;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				ship_cost = ship_cost + state.world.market_get_price(market, cid) * amount;
			}

			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed;}, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull;}, nation);
			auto cargo_abstract_per_day = researched * speed * hull;

			auto score = cargo_abstract_per_day / (price_properties::commodity::min + ship_cost);
			auto weight = ve::select(total_score == 0.f, 0.f, score / total_score);

			auto maintenance_per_ship = vic2_ship_crew_size * state.world.market_get_port_services_weighted_price(market);
			auto& supply_cost = def.supply_cost;
			for(uint8_t i = 0; i < supply_cost.set_size; i++) {
				auto cid = supply_cost.commodity_type[i];
				if(!cid) break;
				auto amount = supply_cost.commodity_amounts[i] * cargo_ship_maintenance_reduction;
				maintenance_per_ship = maintenance_per_ship + state.world.market_get_price(market, cid) * amount;
			}
			auto existing_count = state.world.market_get_owned_ships(market, uid);
			auto estimated_maintenance_cost = maintenance_per_ship * existing_count;

			total_port_services_required = existing_count + vic2_ship_crew_size * existing_count;

			total_maintenance_costs = total_maintenance_costs + estimated_maintenance_cost;
			total_abstract_cargo_potential = total_abstract_cargo_potential + cargo_abstract_per_day;

			total_shipbuilding_costs = total_shipbuilding_costs + ship_building_speed * weight * ship_cost;
		}

		ship_building_spend = ve::min(ship_building_spend, total_shipbuilding_costs);
		auto ship_maintenance_spend = ve::min(total_maintenance_costs, earn * 0.1f);
		
		auto maintenance_costs_per_abstract_unit_of_cargo = ve::select(total_abstract_cargo_potential == 0.f, 0.f, total_maintenance_costs / (total_abstract_cargo_potential));
		state.world.market_set_naval_transportation_price(market, maintenance_costs_per_abstract_unit_of_cargo / vic2_hull_to_tonn / vic2_knots_to_km_per_day);

		auto spend = ve::select(arbitrage > 0.f, 0.f, -arbitrage) + import_spent + purchases + next_dividends + debt_payment + ship_building_spend + ship_maintenance_spend;

		auto spend_on_ships_maintenance_ratio = ve::select(total_maintenance_costs == 0.f, 0.f, ship_maintenance_spend / total_maintenance_costs);
		auto spend_on_ships_ratio = ve::select(total_shipbuilding_costs == 0.f, 0.f, ship_building_spend / total_shipbuilding_costs);

		state.world.market_set_land_transportation_price(market, 0.f);
		state.world.market_set_naval_transportation_demand_satisfaction(market, 1.f);
		state.world.market_set_land_transportation_demand_satisfaction(market, 1.f);

		state.world.market_set_port_services_demand(market, 0.f);

		auto spending_ability_ratio = ve::min(1.f, ve::max(0.f, ve::select(spend == 0.f, 1.f, earn / spend)));
		// We don't want to spend a lot more than we earn on imports
		auto import_ratio = ve::min(1.f, ve::max(0.f, ve::select(earn == 0.f, 1.f, import_spent / earn)));

		state.world.market_set_trade_house_budget_scale(market, spending_ability_ratio);
		state.world.market_set_trade_house_budget_import_scale(market, import_ratio);

		auto refund = state.world.market_get_shipbuilding_refund(market);

		state.world.market_set_stockpile(market, economy::money, budget + refund + earn - spend);

		state.world.market_set_total_earn(market, earn);
		state.world.market_set_total_paid(market, spend);

		state.world.market_set_shipbuilding_spending_ratio(market, spend_on_ships_ratio);
		state.world.market_set_ships_maintenance_spending_ratio(market, spend_on_ships_maintenance_ratio);

		// register shipbuilding demand
		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if(def.is_land) continue;
			if(def.type != military::unit_type::transport) continue;
			auto researched = ve::select(def.active || state.world.nation_get_active_unit(nation, uid), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f });
			ve::fp_vector ship_cost = 0.f;
			auto& costs = def.build_cost;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				ship_cost = ship_cost + state.world.market_get_price(market, cid) * amount;
			}
			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed; }, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull; }, nation);
			auto score = researched * speed * hull / (price_properties::commodity::min + ship_cost);
			auto weight = ve::select(total_score == 0.f, 0.f, score / total_score);
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				auto demanded_amount = ship_building_speed * amount * weight * spend_on_ships_ratio;
				register_demand(state, market, cid, demanded_amount);
			}

			auto existing_count = state.world.market_get_owned_ships(market, uid);
			auto& supply_cost = def.supply_cost;
			for(uint8_t i = 0; i < supply_cost.set_size; i++) {
				auto cid = supply_cost.commodity_type[i];
				if(!cid) break;
				auto amount = supply_cost.commodity_amounts[i] * cargo_ship_maintenance_reduction;
				auto demanded_amount = existing_count * amount * spend_on_ships_maintenance_ratio;
				register_demand(state, market, cid, demanded_amount);
			}

			auto old_port_demand = state.world.market_get_port_services_demand(market);
			auto demanded_port_services = existing_count * vic2_ship_crew_size;
			state.world.market_set_port_services_demand(market, old_port_demand + demanded_port_services);
		}
	});

	set_profile_point(state, "trade_centers_consumption");

	// Convert trade transportation demand
	concurrency::parallel_for((size_t)(0), (size_t)(state.world.market_size()), [&](auto raw_mid) {
		dcon::market_id mid{ (dcon::market_id::value_base_t)(raw_mid) };
		auto sid = state.world.market_get_zone_from_local_market(mid);
		auto demand_to_distribute = state.world.market_get_port_services_demand(mid);
		auto total_weight = state.world.market_get_total_port_capacity_weight(mid);
		if(demand_to_distribute == 0.f || total_weight == 0.f) {
			return;
		}
		province::for_each_province_in_state_instance(state, sid, [&](auto pid) {
			auto port_size = state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
			auto local_weight = port_size / (price_properties::service::epsilon + state.world.province_get_service_price(pid, services::list::port_capacity));
			auto local_demand_add = demand_to_distribute * local_weight / total_weight;
			auto local_demand = state.world.province_get_service_demand_forbidden_public_supply(pid, services::list::port_capacity);
			state.world.province_set_service_demand_forbidden_public_supply(pid, services::list::port_capacity, local_demand + local_demand_add);
		});
	});

	set_profile_point(state, "port_demand_conversion");

	sanity_check(state);

	// ###################
	// # MARKET CLEARING #
	// ###################

	/*
	perform actual consumption / purchasing subject to availability at markets:
	*/

#ifndef NDEBUG
	ve::fp_vector total_markets_income = 0.f;

	state.world.execute_serial_over_market([&](auto markets) {
		total_markets_income = total_markets_income - state.world.market_get_stockpile(markets, economy::money);
	});
#endif

	// labor
	province::ve_parallel_for_each_land_province(state, [&](auto ids) {
		for(auto j = int32_t(0); j < int32_t(labor::total); j++) {
			auto supply_labor = state.world.province_get_labor_supply(ids, j);
			auto demand_labor = state.world.province_get_labor_demand(ids, j);

			auto satisfaction = ve::select(demand_labor > 0.f, ve::min(1.f, supply_labor / demand_labor), 1.f);
			auto sold = ve::select(supply_labor > 0.f, ve::min(1.f, demand_labor / supply_labor), 1.f);

#ifndef NDEBUG
			ve::apply([&](auto value) { assert(std::isfinite(value) && value >= 0.f); }, sold);
			ve::apply([&](auto value) { assert(std::isfinite(value) && value >= 0.f); }, satisfaction);
#endif

			state.world.province_set_labor_demand_satisfaction(ids, j, satisfaction);
			state.world.province_set_labor_supply_sold(ids, j, sold);
		}
	});

	services::match_supply_and_demand(state);

	state.world.execute_parallel_over_market([&](auto ids) {
		auto zones = state.world.market_get_zone_from_local_market(ids);
		auto market_capitals = state.world.state_instance_get_capital(zones);
		auto nations = state.world.state_instance_get_nation_from_state_ownership(zones);
		auto capital = state.world.nation_get_capital(nations);
		auto capital_states = state.world.province_get_state_membership(capital);
		auto capital_mask = capital_states == zones;

		// market transportation section
		auto naval_momentum = state.world.market_get_naval_transportation_supply(ids) * vic2_knots_to_km_per_day * vic2_hull_to_tonn;
		auto required_naval_momentum = state.world.market_get_naval_transportation_demand(ids);
		auto naval_momentum_satisfaction = ve::select(required_naval_momentum == 0.f, 1.f, ve::min(ve::fp_vector{1.f}, naval_momentum / required_naval_momentum));
		state.world.market_set_naval_transportation_demand_satisfaction(ids, naval_momentum_satisfaction);
#ifndef NDEBUG
		ve::apply([&](auto value) { assert(value >= 0.f && value <= 1.f); }, naval_momentum_satisfaction);
#endif

		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

			// we do not actually consume/purchase money rgos
			if(state.world.commodity_get_money_rgo(c)) {
				continue;
			}

			auto draw_from_stockpile = state.world.nation_get_drawing_on_stockpiles(nations, c) == true;

			/*

			Currently, merhants don't want to stockpile goods.
			Instead, they purchase goods elsewhere or store unsold items to sold them locally.
			At certain point incoming items are balanced with sold items.

			*/

			auto stockpiles = state.world.market_get_stockpile(ids, c);
			auto merchants_supply = state.world.market_get_stockpile_sales(ids, c);

			auto production_and_merchants_supply = state.world.market_get_supply(ids, c);
			// we draw from stockpile in capital
			// This has been disabled due to govt stockpiles work on a market-to-market basis. Drawing from stockpiles in this way has not yet been implemented
			/*auto national_stockpile = ve::select(
				capital_mask && draw_from_stockpile,
				state.world.nation_get_stockpiles(nations, c),
				0.f
			);*/
			auto total_supply = production_and_merchants_supply;
			//auto supply_from_nation_ratio = ve::select(total_supply == 0.f, 0.f, national_stockpile / total_supply);
			auto total_demand = state.world.market_get_demand(ids, c);

			auto new_actual_probability_to_buy = ve::min(1.f, ve::select(total_demand == 0.f, 0.f, total_supply / total_demand));
			auto new_actual_probability_to_sell = ve::min(1.f, ve::select(total_supply == 0.f, 0.f, total_demand / total_supply));

			auto aggregated_demand = state.world.market_get_aggregated_demand_history(ids, c);
			auto aggregated_supply = state.world.market_get_aggregated_supply_history(ids, c);

			auto new_expected_probability_to_buy = ve::min(1.f, (aggregated_supply + 0.25f) / (aggregated_demand + 0.25f));
			auto new_expected_probability_to_sell = ve::min(1.f, (aggregated_demand + 0.25f) / (aggregated_supply + 0.25f));

			auto old_expected_probability_to_buy = state.world.market_get_expected_probability_to_buy(ids, c);
			auto old_expected_probability_to_sell = state.world.market_get_expected_probability_to_sell(ids, c);

			auto expected_probability_to_buy =
				old_expected_probability_to_buy * state.defines.alice_sat_delay_factor
				+ new_expected_probability_to_buy * (1.f - state.defines.alice_sat_delay_factor);
			auto expected_probability_to_sell =
				old_expected_probability_to_sell * state.defines.alice_sat_delay_factor
				+ new_expected_probability_to_sell * (1.f - state.defines.alice_sat_delay_factor);
#ifndef NDEBUG
			ve::apply([&](auto value) { assert(value >= 0.f && value <= 1.f); }, expected_probability_to_buy);
			ve::apply([&](auto value) { assert(value >= 0.f && value <= 1.f); }, new_expected_probability_to_sell);
#endif

			state.world.market_set_expected_probability_to_buy(ids, c, expected_probability_to_buy);
			state.world.market_set_expected_probability_to_sell(ids, c, expected_probability_to_sell);

			state.world.market_set_actual_probability_to_buy(ids, c, new_actual_probability_to_buy);
			state.world.market_set_actual_probability_to_sell(ids, c, new_actual_probability_to_sell);

			state.world.market_set_consumption(ids, c, new_actual_probability_to_buy * total_demand);

			// we bought something locally
			// transaction
			// local money stockpile might go negative:
			// traders can take loans after all
			// our balance goal is to make sure that money trajectory is sane
			// there should be no +- inf in money stockpiles
			// decay stockpiles and ""gift"" the unsold supply to merchants


			state.world.market_set_stockpile(
				ids, c,
				ve::max(0.f, (
					state.world.market_get_stockpile(ids, c)
					// we sold a part of stockpile
					- merchants_supply * new_actual_probability_to_sell
					// we got unsold part of non-stockpiled supply
					+ (production_and_merchants_supply - merchants_supply) * (1.f - new_actual_probability_to_sell)
				))
			);

			state.world.market_set_stockpile(ids, c, state.world.market_get_stockpile(ids, c) *(1.f - stockpile_spoilage));

			//if(presimulation) {
				//state.world.market_set_stockpile(ids, c, state.world.market_get_stockpile(ids, c) + 5.f * production_and_merchants_supply);
			//}

			auto sales = state.world.market_get_sales(ids);
			state.world.market_set_sales(ids, sales + merchants_supply * new_actual_probability_to_sell * ve_price(state, ids, c));

#ifndef NDEBUG
			ve::apply([&](auto value) { assert(std::isfinite(value)); }, state.world.market_get_stockpile(ids, c));
#endif


			// then we siphon from national stockpile:
			// there is only one capital in a country!,
			// which means that we can safely pay back for siphoned stockpile
			// and change national stockpile at the same time
			/*ve::apply([&](bool do_it, float bought_from_nation, float national_stockpile_i, dcon::nation_id nations_i, dcon::market_id ids_i) {
				if(do_it) {
					auto bought_from_nation_cost =
						bought_from_nation
						* state.world.market_get_price(ids_i, c);
					state.world.nation_set_stockpiles(nations_i, c, national_stockpile_i - bought_from_nation);
					auto treasury = state.world.nation_get_stockpiles(nations_i, economy::money);
					state.world.nation_set_stockpiles(nations_i, economy::money, treasury + bought_from_nation_cost);
				}
			}, capital_mask && draw_from_stockpile, national_stockpile * new_actual_probability_to_sell, national_stockpile, nations, ids);*/
		}

		state.world.market_set_stockpile(
			ids, economy::money,
			state.world.market_get_stockpile(ids, economy::money) * state.inflation
		);
	});

	set_profile_point(state, "clear_market");

#ifndef NDEBUG
	state.world.execute_serial_over_market([&](auto markets) {
		total_markets_income = total_markets_income + state.world.market_get_stockpile(markets, economy::money);
	});
	state.console_log("Total markets income: " + std::to_string(total_markets_income.reduce()));
#endif // !NDEBUG

	// recalculate wealth
	state.world.execute_parallel_over_market([&](auto market){
		ve::fp_vector total;
		state.world.for_each_commodity([&](auto cid) {
			if(cid != economy::money) {
				total = total + state.world.market_get_stockpile(market, cid) * state.world.market_get_price(market, cid);
			}
		});
		state.world.market_set_wealth(market, total);
	});

	sanity_check(state);

	// ################################################################################
	// # ADJUST ACTUALLY SATISFIED DEMAND DEPENDING ON THE RESULTS OF MARKET CLEARING #
	// ################################################################################

	// updates of national purchases:
	concurrency::parallel_for(uint32_t(0), state.world.nation_size(), [&](uint32_t i) {
		auto n = dcon::nation_id{ dcon::nation_id::value_base_t(i) };
		if(state.world.nation_get_owned_province_count(n) == 0)
			return;

		auto capital = state.world.nation_get_capital(n);
		auto capital_state = state.world.province_get_state_membership(capital);
		auto capital_market = state.world.state_instance_get_market_from_local_market(capital_state);

		// refund national employment:
		refund_demand_administration(state, n);

		/*
		determine effective spending levels
		we iterate over all markets to gather contruction data
		*/
		auto nations_commodity_spending = state.world.nation_get_spending_level(n);
		float refund = 0.0f;

		for(uint32_t k = 1; k < total_commodities; ++k) {
			dcon::commodity_id c{ dcon::commodity_id::value_base_t(k) };
			state.world.nation_for_each_state_control(n, [&](dcon::state_control_id soid) {
				auto local_state = state.world.state_control_get_state(soid);
				auto market = state.world.state_instance_get_market_from_local_market(local_state);
				auto sat = state.world.market_get_actual_probability_to_buy(market, c);
				auto combined_stockpile_demand = economy::combined_government_stockpile_demand(state, market, c);
				if(combined_stockpile_demand > 0.f) {
					add_government_stockpile(state, n, market, c, combined_stockpile_demand * nations_commodity_spending * sat);
					auto delta =
						combined_stockpile_demand
						* (1.0f - sat)
						* nations_commodity_spending
						* state.world.market_get_price(market, c);
					assert(delta >= 0.f);
					refund += delta;
				}
			});
			
		}

		/*
		calculate overseas penalty:
		ideally these goods would be bought in colonies
		but limit to capital for now.
		ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
		*/

		if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
			auto overseas_factor = state.defines.province_overseas_penalty
				* float(
					state.world.nation_get_owned_province_count(n)
					- state.world.nation_get_central_province_count(n)
				);
			auto overseas_budget = float(state.world.nation_get_overseas_spending(n)) / 100.f;
			auto overseas_budget_satisfaction = 1.f;

			if(overseas_factor > 0) {
				for(uint32_t k = 1; k < total_commodities; ++k) {
					dcon::commodity_id c{ dcon::commodity_id::value_base_t(k) };
					if(state.world.commodity_get_overseas_penalty(c) && valid_need(state, n, c)) {
						auto sat = state.world.market_get_actual_probability_to_buy(capital_market, c);
						overseas_budget_satisfaction = std::min(sat, overseas_budget_satisfaction);
						auto price_of = state.world.market_get_price(capital_market, c);
						auto delta = overseas_factor
							* (1.0f - sat) * nations_commodity_spending * price_of * overseas_budget;
						assert(delta >= 0.f);
						refund += delta;
					}
				}

				state.world.nation_set_overseas_penalty(n, overseas_budget
					* overseas_budget_satisfaction);
			} else {
				state.world.nation_set_overseas_penalty(n, 1.0f);
			}
		}
		else {
			state.world.nation_set_overseas_penalty(n, 0.0f);
		}

		// finally, pay back refund:
		assert(std::isfinite(refund) && std::isfinite(state.world.nation_get_treasury(n) + refund) && refund >= 0.0f);
		state.world.nation_set_treasury(n, state.world.nation_get_treasury(n) + refund);
	});

	set_profile_point(state, "refund_nations");

	concurrency::parallel_for((size_t)(0), (size_t)(state.world.market_size()), [&](auto raw_mid) {
		dcon::market_id mid{ (dcon::market_id::value_base_t)(raw_mid) };
		auto sid = state.world.market_get_zone_from_local_market(mid);
		auto total_sat = 0.f;
		province::for_each_province_in_state_instance(state, sid, [&](auto pid) {
			auto local_price = state.world.province_get_service_price(pid, services::list::port_capacity);
			auto local_size = state.world.province_get_advanced_province_building_max_private_size(pid, advanced_province_buildings::list::civilian_ports);
			auto local_weight = local_size / (price_properties::service::epsilon + local_price);
			auto current_weight = state.world.market_get_total_port_capacity_weight(mid);

			auto bought_locally = current_weight == 0.f ? 0.f : local_weight / current_weight;
			auto local_satisfied = state.world.province_get_service_satisfaction(pid, services::list::port_capacity);

			auto sat_contribution = bought_locally * local_satisfied;
			total_sat = total_sat + sat_contribution;
		});
		state.world.market_set_port_services_demand_satisfaction(mid, total_sat);
	});


	set_profile_point(state, "recalculate_refund_for_services");

	state.world.execute_parallel_over_market([&](auto market) {
		auto area = state.world.market_get_zone_from_local_market(market);
		auto nation = state.world.state_instance_get_nation_from_state_ownership(area);
		auto current_size = state.world.market_get_total_port_capacity(market);

		auto ships_ratio = state.world.market_get_shipbuilding_spending_ratio(market);
		auto ships_maintenance_ratio = state.world.market_get_ships_maintenance_spending_ratio(market);
		//auto income_scale = state.world.market_get_trade_house_budget_scale(market);
		auto shipping_satisfaction = state.world.market_get_naval_transportation_demand_satisfaction(market);
		auto ship_building_priority = 1.f - shipping_satisfaction;
		auto ship_building_speed = current_size / 10000.f * ship_building_priority;

		auto port_refund = 1.f - state.world.market_get_port_services_demand_satisfaction(market);

		ve::fp_vector shipbuilding_refund {0.f};
		ve::fp_vector total_port_services_demanded{ 0.f };

		ve::fp_vector total_score = 0.f;
		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if(def.is_land) continue;
			if(def.type != military::unit_type::transport) continue;
			auto researched = ve::select(def.active || state.world.nation_get_active_unit(nation, uid), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f });
			ve::fp_vector ship_cost = 0.f;
			auto& costs = def.build_cost;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				ship_cost = ship_cost + state.world.market_get_price(market, cid) * amount;
			}
			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed; }, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull; }, nation);
			auto score = researched * speed * hull / (price_properties::commodity::min + ship_cost);
			total_score = total_score + score;
		}
		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if(def.is_land) continue;
			if(def.type != military::unit_type::transport) continue;
			auto researched = ve::select(def.active || state.world.nation_get_active_unit(nation, uid), ve::fp_vector{ 1.f }, ve::fp_vector{ 0.f });
			ve::fp_vector ship_cost = 0.f;
			auto& costs = def.build_cost;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				ship_cost = ship_cost + state.world.market_get_price(market, cid) * amount;
			}
			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed; }, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull; }, nation);
			auto score = researched * speed * hull / (price_properties::commodity::min + ship_cost);
			auto weight = ve::select(total_score == 0.f, 0.f, score / total_score);
			ve::fp_vector min_satisfied = 1.f;
			for(uint8_t i = 0; i < costs.set_size; i++) {
				auto cid = costs.commodity_type[i];
				if(!cid) break;
				auto amount = costs.commodity_amounts[i];
				auto demanded_amount = ship_building_speed * amount * weight * ships_ratio;
				auto actually_bought = state.world.market_get_actual_probability_to_buy(market, cid);
				auto price = state.world.market_get_price(market, cid);
				auto refunded = demanded_amount * price * (1.f - actually_bought);
				shipbuilding_refund = shipbuilding_refund + refunded;
				min_satisfied = ve::min(min_satisfied, actually_bought);
			}
			auto ready_ships = state.world.market_get_owned_ships(market, uid);
			auto& supply_cost = def.supply_cost;
			ve::fp_vector min_satisfied_maintenance = 1.f;
			for(uint8_t i = 0; i < supply_cost.set_size; i++) {
				auto cid = supply_cost.commodity_type[i];
				if(!cid) break;
				auto amount = supply_cost.commodity_amounts[i] * cargo_ship_maintenance_reduction;
				auto actually_bought = state.world.market_get_actual_probability_to_buy(market, cid);
				auto demanded_amount = ready_ships * amount * ships_maintenance_ratio;
				auto price = state.world.market_get_price(market, cid);
				auto refunded = demanded_amount * price * (1.f - actually_bought);
				shipbuilding_refund = shipbuilding_refund + refunded;
				min_satisfied_maintenance = ve::min(min_satisfied_maintenance, actually_bought);
			}

			if(presimulation) {
				min_satisfied = 1.f;
				ship_building_speed = ship_building_speed * 10.f;
			}

			auto advance = min_satisfied * weight * ship_building_speed * ships_ratio;
			auto port_services = ready_ships * vic2_ship_crew_size;
			total_port_services_demanded = total_port_services_demanded + port_services;

			state.world.market_set_owned_ships(
				market,
				uid,
				ready_ships
				* ve::max(0.99999f, ve::min(1.f, 0.5f + min_satisfied_maintenance * ships_maintenance_ratio))
				+ advance
			);
		}


		auto total_cost_port_services = total_port_services_demanded * state.world.market_get_port_services_weighted_price(market);
		shipbuilding_refund = shipbuilding_refund + total_cost_port_services * port_refund;
		state.world.market_set_shipbuilding_refund(market, shipbuilding_refund);
	});

	set_profile_point(state, "refund_markets");

	sanity_check(state);

	advanced_province_buildings::update_profit_and_refund(state);

	set_profile_point(state, "refund and profit of advanced province buildings");

	sanity_check(state);

	/* now we know demand satisfaction and can set actual satifaction of pops and pay profit to producers */

	update_rgo_profit(state);
	set_profile_point(state, "rgo profit");

	/* prepare needs satisfaction caps */
	if(state.world.consumption_category_size() == 0) {
		state.world.execute_parallel_over_market([&](auto ids) {
			auto states = state.world.market_get_zone_from_local_market(ids);
			auto nations = state.world.state_instance_get_nation_from_state_ownership(states);

			ve::fp_vector life_mul[3] = {
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::poor_life_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::middle_life_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::rich_life_needs) + 1.0f
			};
			ve::fp_vector everyday_mul[3] = {
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::poor_everyday_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::middle_everyday_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::rich_everyday_needs) + 1.0f
			};
			ve::fp_vector luxury_mul[3] = {
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::poor_luxury_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::middle_luxury_needs) + 1.0f,
				state.world.nation_get_modifier_values(
					nations, sys::national_mod_offsets::rich_luxury_needs) + 1.0f,
			};

			auto invention_factor = invention_count.get(nations) * state.defines.invention_impact_on_demand + 1.0f;

			state.world.for_each_pop_type([&](auto pt) {			
				auto strata = state.world.pop_type_get_strata(pt);

				/*

				Pops have spent money on needs assuming that they have managed to buy everything
				Now we have to adjust their savings to account for the fact that not everything is possible to buy.
				And calculate how well they actually fulfill their needs.

				The logic is fairly simple:
				W - weights
				B - base
				P - price
				S - probability to buy

				Pops needed B package of goods
				Pop have attempted to purchase W * B package of goods.
				Pop have managed to purchase S * W * B package of goods.
				It means that sum(S * W * B) / sum(B) provides us
				with a crude but usable metric of satisfaction ratio.
				The shortcoming of this metric is that sometimes certain goods
				could be required in high amount.
				For example, in vanilla content, pops require 4 units of tea.
				This volume overshadows all other needs and makes available range of satisfaction
				much less expressive among the nations with low access to tea.
				It means that sometimes we want to ignore the actual amount of  needed goods
				when calculating satisfaction.
				For these cases sum(S * W) / N might be more appropriate.
				Another, more subtle, issue with summing up weights is that
				if we want to allow pops to prioritise certain goods over other by setting the weights to be higher than 1
				then simple sum would allow to consume only one kind of goods and be fully satisfied.
				So we need a way to adjust the sum which would promote consumption of diverse goods.
				One of opportunities to do it is to divide the result by sum of squares of weights (after normalisation by the sum).
				It could work because if we consider a simplex of weights, the norm is minimal
				at the cetral point of this simplex which would correspond to all weights being equal
				which represents the situtation we want to encourage

				In the result we arrive to the following calculation of "satisfaction":
				sum(S * W) / N / sum(N * W / sum(W) * W / sum(W))
				where N is the amount of types of goods in the basket.

				Or, alternatively:
				sum(W) * sum(W) * sum(S * W) / sum(W * W) / N / N

				But as W could be shifting around, it's possible for them to become equal to zero.
				It means that we have to resolve the singularity in some way.

				As zero weights represent the situation when pop purchases nothing, it's meaningful to just set the value to zero.
				(Also it makes sense from limit W->0 viewpoint)

				Sum over W * B * P provides us with cost of 1 package of required goods
				It's precisely the price of package the pop was trying to pay.
				But only sum(S * W * B * P) was actually paid.
				So to calculate refund we have to calculate
				sum(S * W * B * P) / sum(W * B * P) ratio.

				*/

				ve::fp_vector ln_w_summed = 0.0f;
				ve::fp_vector en_w_summed = 0.0f;
				ve::fp_vector lx_w_summed = 0.0f;

				ve::fp_vector ln_ww_summed = 0.0f;
				ve::fp_vector en_ww_summed = 0.0f;
				ve::fp_vector lx_ww_summed = 0.0f;

				ve::fp_vector ln_sw = 0.0f;
				ve::fp_vector en_sw = 0.0f;
				ve::fp_vector lx_sw = 0.0f;

				//ve::fp_vector ln_wbp = 0.0f;
				//ve::fp_vector en_wbp = 0.0f;
				//ve::fp_vector lx_wbp = 0.0f;

				ve::fp_vector ln_swbp = 0.0f;
				ve::fp_vector en_swbp = 0.0f;
				ve::fp_vector lx_swbp = 0.0f;

				ve::fp_vector ln_N = 0.00f;
				ve::fp_vector en_N = 0.00f;
				ve::fp_vector lx_N = 0.00f;

				for(uint32_t i = 1; i < total_commodities; ++i) {
					dcon::commodity_id c{ dcon::commodity_id::value_base_t(i) };

					auto s = state.world.market_get_actual_probability_to_buy(ids, c);
					auto p = state.world.market_get_price(ids, c);


					auto ln_b = state.world.pop_type_get_life_needs(pt, c) * life_mul[strata];
					auto en_b = state.world.pop_type_get_everyday_needs(pt, c) * invention_factor * everyday_mul[strata];
					auto lx_b = state.world.pop_type_get_luxury_needs(pt, c) * invention_factor * luxury_mul[strata];

					ln_N = ln_N + ve::select(ln_b > 0.f, ve::fp_vector(1.f), ve::fp_vector(0.f));
					en_N = en_N + ve::select(en_b > 0.f, ve::fp_vector(1.f), ve::fp_vector(0.f));
					lx_N = lx_N + ve::select(lx_b > 0.f, ve::fp_vector(1.f), ve::fp_vector(0.f));

					auto ln_w = ve::select(ln_b > 0.f, state.world.market_get_life_needs_weights(ids, c), 0.f);
					auto en_w = ve::select(en_b > 0.f, state.world.market_get_everyday_needs_weights(ids, c), 0.f);
					auto lx_w = ve::select(lx_b > 0.f, state.world.market_get_luxury_needs_weights(ids, c), 0.f);

					ln_w_summed = ln_w_summed + ln_w;
					en_w_summed = en_w_summed + en_w;
					lx_w_summed = lx_w_summed + lx_w;

					ln_ww_summed = ln_ww_summed + ln_w * ln_w;
					en_ww_summed = en_ww_summed + en_w * en_w;
					lx_ww_summed = lx_ww_summed + lx_w * lx_w;

					ln_sw = ln_sw + s *  ln_w;
					en_sw = en_sw + s * en_w;
					lx_sw = lx_sw + s * lx_w;

					//ln_wbp = ln_wbp + ln_w * ln_b * p;
					//en_wbp = en_wbp + en_w * en_b * p;
					//lx_wbp = lx_wbp + lx_w * lx_b * p;

					ln_swbp = ln_swbp + s * ln_w * ln_b * p;
					en_swbp = en_swbp + s * en_w * en_b * p;
					lx_swbp = lx_swbp + s * lx_w * lx_b * p;
				}

				auto ideal_ln_wbp = state.world.market_get_life_needs_costs(ids, pt);
				auto ideal_en_wbp = state.world.market_get_everyday_needs_costs(ids, pt);
				auto ideal_lx_wbp = state.world.market_get_luxury_needs_costs(ids, pt);

				ln_swbp = ve::select(ideal_ln_wbp > 0.f, ln_swbp / ideal_ln_wbp, 1.f);
				en_swbp = ve::select(ideal_en_wbp > 0.f, en_swbp / ideal_en_wbp, 1.f);
				lx_swbp = ve::select(ideal_lx_wbp > 0.f, lx_swbp / ideal_lx_wbp, 1.f);

				auto ln_q = ve::min( ve::select(ln_N == 0.f, 1.f, ve::select(ln_ww_summed > 0.f, ln_w_summed * ln_w_summed * ln_sw / ln_ww_summed / ln_N / ln_N, 0.f)), 1.0f);
				auto en_q = ve::min( ve::select(en_N == 0.f, 1.f, ve::select(en_ww_summed > 0.f, en_w_summed * en_w_summed *  en_sw / en_ww_summed / en_N / en_N, 0.f)), 1.0f);
				auto lx_q = ve::min( ve::select(lx_N == 0.f, 1.f, ve::select(lx_ww_summed > 0.f, lx_w_summed * lx_w_summed *  lx_sw / lx_ww_summed / lx_N / lx_N, 0.f)), 1.0f);

	#ifndef NDEBUG
				ve::apply([](float life, float everyday, float luxury) {
					assert(life >= 0.f /* && life <= 1.f */);
					assert(everyday >= 0.f /*&& everyday <= 1.f */ );
					assert(luxury >= 0.f /*&& luxury <= 1.f */ );
				}, ln_swbp, en_swbp, lx_swbp);
				ve::apply([](float life, float everyday, float luxury) {
					assert(life >= 0.f && life <= 1.f);
					assert(everyday >= 0.f && everyday <= 1.f);
					assert(luxury >= 0.f && luxury <= 1.f);
				}, ln_q, en_q, lx_q);
				/*
				The tests showed that these values are really close, so it would be nice to test using them instead of recalculating them
				*/
				//ve::apply([](float A, float B) { assert(abs(A - B) < 1.f); }, ideal_ln_wbp, ln_wbp);
				//ve::apply([](float A, float B) { assert(abs(A - B) < 1.f); }, ideal_en_wbp, en_wbp);
				//ve::apply([](float A, float B) { assert(abs(A - B) < 1.f); }, ideal_lx_wbp, lx_wbp);
	#endif // !NDEBUG

				// probability to increase corresponding satisfaction
				state.world.market_set_satisfied_ratio_of_max_life_needs(ids, pt, ln_q);
				state.world.market_set_satisfied_ratio_of_max_everyday_needs(ids, pt, en_q);
				state.world.market_set_satisfied_ratio_of_max_luxury_needs(ids, pt, lx_q);

				// ratio of actually spent money to calculate cashback
				state.world.market_set_satisfied_ratio_of_demanded_life_needs(ids, pt, ve::min(1.f, ln_swbp));
				state.world.market_set_satisfied_ratio_of_demanded_everyday_needs(ids, pt, ve::min(1.f, en_swbp));
				state.world.market_set_satisfied_ratio_of_demanded_luxury_needs(ids, pt, ve::min(1.f, lx_swbp));
			});
		});
	}


	/*
	Here we do a much more simple and self-explanatory computation.
	*/
	if(state.world.consumption_category_size() != 0) {
		state.world.execute_parallel_over_market([&](auto ids) {
			state.world.for_each_consumption_category([&](auto cat_id) {
				ve::fp_vector total_demanded{ 0.f };
				ve::fp_vector total_bought{ 0.f };
				state.world.for_each_commodity([&](auto c) {
					auto price = state.world.market_get_price(ids, c);
					auto index = c.index() + cat_id.index() * state.world.commodity_size();
					auto consumption_weight = state.world.consumption_category_get_weights(cat_id, c);
					auto w = state.world.market_get_local_consumption_weights(ids, index);
					auto satisfied_demand = state.world.market_get_actual_probability_to_buy(ids, c);
					total_demanded = total_demanded + price * consumption_weight * w;
					total_bought = total_bought + satisfied_demand * price * consumption_weight * w;
				});
				state.world.market_set_satisfied_demand_ratio_per_consumption_category(ids, cat_id,
					ve::select(total_demanded == 0.f, 0.f, total_bought / total_demanded)
				);
			});
		});
	}

	sanity_check(state);

	// update needs satisfaction depending on actually available goods and services:

	state.world.execute_parallel_over_pop([&](auto ids) {
		auto province = state.world.pop_get_province_from_pop_location(ids);
		auto local_state = state.world.province_get_state_membership(province);
		auto local_market = state.world.state_instance_get_market_from_local_market(local_state);
		auto local_nation = state.world.province_get_nation_from_province_ownership(province);
		auto invalid = local_nation == dcon::nation_id {};
		auto pop_type = state.world.pop_get_poptype(ids);

		if (state.world.consumption_category_size() == 0) {
			auto ln_satisfaction = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_max_life_needs(market, pt);
			}, local_market, pop_type);
			auto en_satisfaction = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_max_everyday_needs(market, pt);
			}, local_market, pop_type);
			auto lx_satisfaction = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_max_luxury_needs(market, pt);
			}, local_market, pop_type);

			auto ln_spent = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_demanded_life_needs(market, pt);
			}, local_market, pop_type);
			auto en_spent = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_demanded_everyday_needs(market, pt);
			}, local_market, pop_type);
			auto lx_spent = ve::apply([&](auto market, auto pt) {
				return state.world.market_get_satisfied_ratio_of_demanded_luxury_needs(market, pt);
			}, local_market, pop_type);

			// return money which were not actually spent
			{
				auto ln_cost = ve::apply([&](auto market, auto pt) {
					return state.world.market_get_life_needs_costs(market, pt);
				}, local_market, pop_type);
				auto en_cost = ve::apply([&](auto market, auto pt) {
					return state.world.market_get_everyday_needs_costs(market, pt);
				}, local_market, pop_type);
				auto lx_cost = ve::apply([&](auto market, auto pt) {
					return state.world.market_get_luxury_needs_costs(market, pt);
				}, local_market, pop_type);

				auto pop_size = state.world.pop_get_size(ids);
				auto unspent_money =
					ln_cost * (1.f - ln_spent) * demand_life.get(ids)
					+ en_cost * (1.f - en_spent) * demand_everyday.get(ids)
					+ lx_cost * (1.f - lx_spent) * demand_luxury.get(ids);

				auto savings = state.world.pop_get_savings(ids);
				state.world.pop_set_savings(ids, savings + unspent_money);
			}
			auto satisfaction = state.world.pop_get_satisfaction(ids);
			auto satisfaction_gain =
				ve::min(1.f, potential_ratio_life.get(ids) * ln_satisfaction)
				+ ve::min(1.f, potential_ratio_everyday.get(ids) * en_satisfaction)
				+ ve::min(1.f, potential_ratio_luxury.get(ids) * lx_satisfaction);
			satisfaction = satisfaction * 0.999f + satisfaction_gain * 0.001f / 3.f;

			state.world.pop_set_satisfaction(ids, satisfaction);
		} else {
			ve::fp_vector total_satisfaction_from_categories{ 0.f };
			state.world.for_each_consumption_category([&](auto cat_id) {
				auto cost = state.world.market_get_cost_per_consumption_category(local_market, cat_id);
				auto actually_bought = state.world.market_get_satisfied_demand_ratio_per_consumption_category(local_market, cat_id);
				auto demanded = demand_consumption_category[cat_id.index()].get(ids);
				auto refund = (1.f - actually_bought) * cost * demanded;
				auto savings = state.world.pop_get_savings(ids);
				state.world.pop_set_savings(ids, savings + refund);
				total_satisfaction_from_categories = total_satisfaction_from_categories + actually_bought * demanded * state.world.consumption_category_get_satisfaction_score(cat_id);
			});
			auto satisfaction = state.world.pop_get_satisfaction(ids);
			satisfaction = satisfaction * 0.999f + total_satisfaction_from_categories * 0.001f;
			state.world.pop_set_satisfaction(ids, satisfaction);
		}

		{
			auto literacy = pop_demographics::get_literacy(state, ids);
			auto old_literacy = literacy;

			auto literacy_sat_paid = state.world.province_get_service_satisfaction(province, services::list::education);
			auto literacy_sat_public = state.world.province_get_service_satisfaction_for_free(province, services::list::education);
			auto housing_sat = state.world.province_get_service_satisfaction(province, services::list::urban_housing);

			literacy =
				literacy
				+ (
					potential_ratio_education_public.get(ids)
					* literacy_sat_public
					+ potential_ratio_education_private.get(ids)
					* literacy_sat_paid
					- 0.9f
				)
				* pop_demographics::pop_u16_scaling;
			pop_demographics::set_literacy(state, ids, ve::select(invalid, old_literacy, ve::min(1.f, ve::max(0.f, literacy))));

			/*

			Return back money:
			Calculate desired

			*/

			{
				// education
				auto paid_back = demand_paid_education.get(ids) * (1.f - literacy_sat_paid) * state.world.province_get_service_price(province, services::list::education);
				auto savings = state.world.pop_get_savings(ids);
				state.world.pop_set_savings(ids, savings + paid_back);
			}

			{
				// housing
				auto paid_back = demand_housing.get(ids) * (1.f - housing_sat) * state.world.province_get_service_price(province, services::list::urban_housing);
				auto savings = state.world.pop_get_savings(ids);
				state.world.pop_set_savings(ids, savings + paid_back);
			}
		}
	});

	set_profile_point(state, "satisfaction_update");

	sanity_check(state);

	// finally we can move to production:

	// ##########
	// # SUPPLY #
	// ##########


	// RESET SUPPLY

	state.world.execute_parallel_over_market([&](auto markets) {
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			auto old_supply = state.world.market_get_supply(markets, c);
			auto old_aggregated_supply = state.world.market_get_aggregated_supply_history(markets, c);
			if(presimulation) {
				state.world.market_set_aggregated_supply_history(markets, c, old_aggregated_supply * 0.001f + old_supply * 0.999f);
			} else {
				state.world.market_set_aggregated_supply_history(markets, c, old_aggregated_supply * 0.95f + old_supply * 0.05f);
			}

			state.world.market_set_supply(markets, c, ve::fp_vector{});
			state.world.market_set_import(markets, c, ve::fp_vector{});
			state.world.market_set_export(markets, c, ve::fp_vector{});
		});
	});

	concurrency::parallel_for(0, 8, [&](int32_t index) {
		switch(index) {
		case 0:
			services::reset_supply(state);
			break;
		case 1:
			province::ve_for_each_land_province(state, [&](auto markets) {
				for(int32_t i = 0; i < labor::total; i++) {
					state.world.province_set_labor_supply(markets, i, 0.f);
				}
			});
			break;
		case 2:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_arbitrage(markets, 0.f);
			});
			break;
		case 3:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_export_cut(markets, 0.f);
			});
			break;
		case 4:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_import_spending(markets, 0.f);
			});
			break;
		case 5:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_tariff_collected(markets, 0.f);
			});
			break;
		case 6:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_naval_transportation_supply(markets, 0.f);
			});
			break;
		case 7:
			state.world.execute_serial_over_market([&](auto markets) {
				state.world.market_set_land_transportation_supply(markets, 0.f);
			});
			break;
		}
	});

	// at this stage we record supply
	// old supply is invalidated

	// we can't handle each trade good separately: they do influence common markets
	// todo: split the logic so some part of it could be done in parallel
	//
	// register trade supply

#ifndef NDEBUG
	float total_trade_paid = 0.f;
	float total_merchant_cut = 0.f;
#endif // !NDEBUG

	set_profile_point(state, "create trade buffers");

	fill_trade_buffers(state,
		export_tariff_buffer,
		import_tariff_buffer,
		presimulation
	);

	set_profile_point(state, "set and sum up trade buffers into per market data");

	// we bought something: register supply from stockpiles:

	state.world.execute_parallel_over_market([&](auto markets) {
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			if(!state.world.commodity_get_money_rgo(c)) {
				auto stockpiles = state.world.market_get_stockpile(markets, c);
				auto states = state.world.market_get_zone_from_local_market(markets);
				auto capitals = state.world.state_instance_get_capital(states);
				auto price = ve_price(state, markets, c);
				auto median_price = state.world.commodity_get_median_price(c);
				auto confidence = state.world.market_get_price_confidence(markets, c);
				auto current_merchants_supply = state.world.market_get_stockpile_sales(markets, c);

				auto wealth = state.world.market_get_wealth(markets);
				auto accumulated_commodity_wealth = stockpiles * price;
				auto wealth_ratio = (1.f + accumulated_commodity_wealth) / (1.f + wealth);

				auto funds = state.world.market_get_stockpile(markets, economy::money);
				auto desired_funds = wealth;
				auto desired_sales_worth = ve::max(ve::fp_vector{ 0.f }, desired_funds - funds) * wealth_ratio;
				auto desired_sales = desired_sales_worth / (price + price_properties::commodity::min);

				auto historical_demand = state.world.market_get_aggregated_demand_history(markets, c);
				auto historical_supply = state.world.market_get_aggregated_supply_history(markets, c);
				auto historical_chance_to_sell = state.world.market_get_expected_probability_to_sell(markets, c);
				auto historical_chance_to_buy = state.world.market_get_expected_probability_to_buy(markets, c);

				auto stockpile_safety = ve::select(
					current_merchants_supply == 0.f,
					ve::fp_vector{ 1.f },
					ve::min(ve::fp_vector{ 1.f }, stockpiles * stockpile_to_supply / current_merchants_supply)
				);
				auto stockpile_filled = ve::select(
					current_merchants_supply == 0.f,
					ve::fp_vector{ 1.f },
					ve::max(ve::fp_vector{ 1.f }, stockpiles * stockpile_to_supply / current_merchants_supply) - 1.f
				);

				stockpile_safety = ve::min(ve::fp_vector{ 1.f }, stockpile_safety * stockpile_safety + (1.f - confidence));

				auto historical_balance =
					historical_demand * (2.f - historical_chance_to_buy)
					- (historical_supply - current_merchants_supply) * (2.f - historical_chance_to_sell);
				auto get_rid_of_stockpiles = 10.f * (1.f + wealth_ratio) * (historical_chance_to_sell + 0.01f) * stockpiles * stockpile_to_supply;
				auto preserve_stockpiles_for_future = - 10.f * current_merchants_supply;

				auto market_is_unbalanced = (1.1f - confidence * historical_chance_to_buy * historical_chance_to_sell) * 10.f;
				auto stockpile_is_running_out = ve::max(ve::fp_vector{ 0.f }, current_merchants_supply / (0.01f + stockpiles * stockpile_to_supply) - 1.f);
				auto stockpile_is_too_big = ve::max(ve::fp_vector{ 0.f }, stockpiles * stockpile_to_supply / (0.01f + current_merchants_supply) - 1.f);

				auto total_score = market_is_unbalanced + stockpile_is_running_out + stockpile_is_too_big;

				auto target = (
					get_rid_of_stockpiles * stockpile_is_too_big
					+ preserve_stockpiles_for_future * stockpile_is_running_out
				) / total_score;

				auto next_merchants_supply = current_merchants_supply * 0.5f + historical_balance * 0.4f + target * 0.10f; //+ desired_sales * 0.000002f ;

				auto bounded_next_merchants_supply = ve::min(stockpiles, ve::max(0.f, next_merchants_supply));
				state.world.market_set_stockpile_sales(markets, c, bounded_next_merchants_supply);
				state.world.market_set_supply(markets, c, state.world.market_get_supply(markets, c) + bounded_next_merchants_supply);

				if(
					presimulation
					&& state.world.commodity_get_actually_exists_in_nature(c)
					&& state.world.commodity_get_rgo_amount(c) > 0.f
				) {
					state.world.market_set_stockpile(markets, c, ve::max(
						ve::min(ve::fp_vector{3650.f}, (2.f - confidence) * presimulation_stage * historical_demand / stockpile_to_supply),
						state.world.market_get_stockpile(markets, c)
					));
				}
			}
		});
	});

	set_profile_point(state, "stockpile trade");

	sanity_check(state);

	// labor supply
	{
		// very simple labor distribution for now:
		// just follow wages proportionally and reduce supply when wages are small

		state.world.execute_parallel_over_province([&](auto ids) {
			auto states = state.world.province_get_state_membership(ids);
			auto markets = state.world.state_instance_get_market_from_local_market(states);
			auto target_wage =
				state.world.market_get_life_needs_costs(markets, state.culture_definitions.primary_factory_worker)
				/ state.defines.alice_needs_scaling_factor
				* labor_greed_life
				+ economy::price_properties::labor::min;

			auto no_education = state.world.province_get_labor_price(ids, labor::no_education);
			target_wage = ve::min(target_wage, no_education * 0.05f);
			auto basic_education = state.world.province_get_labor_price(ids, labor::basic_education);
			auto high_education = state.world.province_get_labor_price(ids, labor::high_education);
			auto high_education_and_accepted = state.world.province_get_labor_price(ids, labor::high_education_and_accepted);

			target_wage = target_wage * target_wage;
			no_education = no_education * no_education;
			basic_education = basic_education * basic_education;
			high_education = high_education * high_education;
			high_education_and_accepted = high_education_and_accepted * high_education_and_accepted;

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::rgo_worker_no_education,
				no_education / (no_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::rgo_worker_no_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::primary_no_education,
				no_education / (no_education + basic_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::primary_no_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_accepted_no_education,
				no_education / (no_education + basic_education + high_education + high_education_and_accepted + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_no_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_not_accepted_no_education,
				no_education / (no_education + basic_education + high_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_no_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::primary_basic_education,
				basic_education / (no_education + basic_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::primary_basic_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_accepted_basic_education,
				basic_education / (no_education + basic_education + high_education + high_education_and_accepted + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_basic_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_not_accepted_basic_education,
				basic_education / (no_education + basic_education + high_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_basic_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_accepted_high_education,
				high_education / (no_education + basic_education + high_education + high_education_and_accepted + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_high_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_not_accepted_high_education,
				high_education / (no_education + basic_education + high_education + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_high_education) * 0.9f
			);

			state.world.province_set_pop_labor_distribution(
				ids,
				pop_labor::high_education_accepted_high_education_accepted,
				high_education_and_accepted / (no_education + basic_education + high_education + high_education_and_accepted + target_wage) * 0.1f
				+ state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_high_education_accepted) * 0.9f
			);

			auto n = state.world.province_get_nation_from_province_ownership(ids);

			auto total_rgo_workers = ve::fp_vector{ 0.f };
			state.world.for_each_pop_type([&](dcon::pop_type_id ptid) {
				if(
					state.world.pop_type_get_is_paid_rgo_worker(ptid)
					|| ptid == state.culture_definitions.slaves
				) {
					total_rgo_workers = total_rgo_workers + state.world.province_get_demographics(ids, demographics::to_key(state, ptid));
				}
			});

			auto total_primary = state.world.province_get_demographics(ids, demographics::to_key(state, state.culture_definitions.primary_factory_worker));
			auto total_guild_education = state.world.province_get_demographics(ids, demographics::to_key(state, state.culture_definitions.artisans));


			auto total_high_education = ve::apply([&](auto p, auto n) {
				auto total = 0.f;
				for(auto pl : state.world.province_get_pop_location(p)) {
					auto not_accepted = !state.world.nation_get_accepted_cultures(n, pl.get_pop().get_culture())
						&& state.world.nation_get_primary_culture(n) != pl.get_pop().get_culture().id;

					if(state.culture_definitions.secondary_factory_worker == pl.get_pop().get_poptype()) {
						if(not_accepted)
							total += pl.get_pop().get_size();
					} else if(pl.get_pop().get_poptype() == state.culture_definitions.bureaucrat) {
						if(not_accepted)
							total += pl.get_pop().get_size();
					} else if(pl.get_pop().get_poptype() == state.culture_definitions.clergy) {
						if(not_accepted)
							total += pl.get_pop().get_size();
					}
				}
				return total;
			}, ids, n);

			auto total_high_education_and_accepted = ve::apply([&](auto p, auto n) {
				auto total = 0.f;
				for(auto pl : state.world.province_get_pop_location(p)) {
					auto accepted = state.world.nation_get_accepted_cultures(n, pl.get_pop().get_culture())
						|| state.world.nation_get_primary_culture(n) == pl.get_pop().get_culture().id;

					if(state.culture_definitions.secondary_factory_worker == pl.get_pop().get_poptype()) {
						if(accepted)
							total += pl.get_pop().get_size();
					} else if(pl.get_pop().get_poptype() == state.culture_definitions.bureaucrat) {
						if(accepted)
							total += pl.get_pop().get_size();
					} else if(pl.get_pop().get_poptype() == state.culture_definitions.clergy) {
						if(accepted)
							total += pl.get_pop().get_size();
					}
				}
				return total;
			}, ids, n);

			state.world.province_set_labor_supply(
				ids, labor::no_education,
				total_rgo_workers * state.world.province_get_pop_labor_distribution(ids, pop_labor::rgo_worker_no_education)
				+ total_primary * state.world.province_get_pop_labor_distribution(ids, pop_labor::primary_no_education)
				+ total_high_education * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_no_education)
				+ total_high_education_and_accepted * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_no_education)
			);
			state.world.province_set_labor_supply(
				ids, labor::basic_education,
				total_primary * state.world.province_get_pop_labor_distribution(ids, pop_labor::primary_basic_education)
				+ total_high_education * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_basic_education)
				+ total_high_education_and_accepted * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_basic_education)
			);
			state.world.province_set_labor_supply(
				ids, labor::high_education,
				total_high_education * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_not_accepted_high_education)
				+ total_high_education_and_accepted * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_high_education)
			);
			state.world.province_set_labor_supply(
				ids, labor::guild_education,
				total_guild_education
			);
			state.world.province_set_labor_supply(
				ids, labor::high_education_and_accepted,
				total_high_education_and_accepted * state.world.province_get_pop_labor_distribution(ids, pop_labor::high_education_accepted_high_education_accepted)
			);
		});
	}

	set_profile_point(state, "labor supply");

	update_pops_employment(state);

	set_profile_point(state, "employment");

	sanity_check(state);

	// produce goods and services

	update_artisan_production(state);

	set_profile_point(state, "artisans production");

	advanced_province_buildings::update_production(state);

	set_profile_point(state, "apb production");

	update_factories_production(state);

	set_profile_point(state, "factories production");

	update_rgo_production(state);

	set_profile_point(state, "rgo production");

	for(auto n : state.world.in_nation) {
		update_production_administration(state, n);
	}

	set_profile_point(state, "admin production");

	state.world.execute_parallel_over_market([&](auto market) {
		auto area = state.world.market_get_zone_from_local_market(market);
		auto nation = state.world.state_instance_get_nation_from_state_ownership(area);
		/*
			A little bit of free shipping to avoid deadlocks.
		*/
		state.world.market_set_naval_transportation_supply(market, 1.f);
		for(uint32_t u = 0; u < state.military_definitions.unit_base_definitions.size(); ++u) {
			dcon::unit_type_id uid = dcon::unit_type_id{ dcon::unit_type_id::value_base_t(u) };
			military::unit_definition& def = state.military_definitions.unit_base_definitions[uid];
			if(def.is_land) continue;
			if(def.type != military::unit_type::transport) continue;

			auto speed = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).maximum_speed; }, nation);
			auto hull = ve::apply([&](dcon::nation_id n) { return state.world.nation_get_unit_stats(n, uid).defence_or_hull; }, nation);
			auto momentum_per_ship = speed * hull;
			auto ready_ships = ve::floor(state.world.market_get_owned_ships(market, uid));

			auto existing_momentum = state.world.market_get_naval_transportation_supply(market);
			state.world.market_set_naval_transportation_supply(market, existing_momentum + momentum_per_ship * ready_ships);
		}
	});

	set_profile_point(state, "transportation production");

	// ####################
	// # PAYMENTS TO POPS #
	// ####################

	state.world.execute_serial_over_pop([&](auto p) {
		income_buffer.set(p, -state.world.pop_get_savings(p));
	});

	/*
	Start with updating rgo/factory "banks"
	*/

	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid){
		{
			auto total_factory_profit = 0.f;
			// FACTORIES
			for(auto f : state.world.province_get_factory_location(pid)) {
				auto fac = f.get_factory();
				auto profit = explain_last_factory_profit(state, fac);
				state.world.factory_set_profit(fac.id, profit.profit);
				total_factory_profit += profit.profit;
			}

			auto current_bank = state.world.province_get_factory_bank(pid);
			state.world.province_set_factory_bank(pid, current_bank + total_factory_profit);
		}

		{
			auto total_rgo_profit = state.world.province_get_rgo_profit(pid);
			auto current_bank = state.world.province_get_rgo_bank(pid);
			state.world.province_set_rgo_bank(pid, current_bank + total_rgo_profit);
		}
	});

	set_profile_point(state, "rgo/factory banks");

	pops::update_income_national_subsidy(state);

	set_profile_point(state, "pops subsidy");

	pops::update_income_non_labor(state);

	set_profile_point(state, "non labor income");

	pops::update_income_artisans(state);

	set_profile_point(state, "artisans");

	pops::update_income_wages(state);

	set_profile_point(state, "wages");

	state.world.execute_serial_over_pop([&](auto p) {
		income_buffer.set(p, ve::max(state.world.pop_get_savings(p) + income_buffer.get(p), 0.f));
	});

	set_profile_point(state, "update income buffer");

	// #####################
	// # TAXES AND TARIFFS #
	// #####################


	concurrency::parallel_for(int32_t(0), int32_t(state.world.nation_size()), [&](int32_t index) {
		auto n = dcon::nation_id{ dcon::nation_id::value_base_t(index) };
		/* advance construction */
		advance_nation_private_constructions(state, n, spent_on_construction_buffer.get(n));
		if(presimulation) {
			emulate_construction_demand(state, n);
		}
	});

	collect_taxes(state, income_buffer);

	/*
	There is also a notion of "market tax"
	Consumers have to pay additional money to local traders.
	The main reason to do it is to provide markets with cash.
	But it could actually represent the fact that local companies
	exercise certain power over local population and are able
	to charge extra money above "fair" price, essentially collecting the "tax"
	*/

	province::for_each_market_province_parallel_over_market(state, [&](dcon::market_id mid, dcon::state_instance_id sid, dcon::province_id pid) {
		float total = 0.f;
		state.world.province_for_each_pop_location(pid, [&](auto location) {
			auto pop = state.world.pop_location_get_pop(location);
			auto savings = state.world.pop_get_savings(pop);
			total += savings;
			state.world.pop_set_savings(pop, savings * (1.f - economy::pops::market_tax));
		});
		auto tax = total * economy::pops::market_tax;
		auto current =state.world.market_get_pop_investments(mid);
		state.world.market_set_pop_investments(mid, current + tax);
	});


	set_profile_point(state, "taxes");

	auto collected_tariff_buffer = state.world.nation_make_vectorizable_float_buffer();
	for(auto mid : state.world.in_market) {
		auto controller = mid.get_zone_from_local_market().get_capital().get_nation_from_province_control();
		// skip if rebel controlled
		if(!controller) {
			continue;
		}
		auto old_value = collected_tariff_buffer.get(controller);
		auto collected = mid.get_tariff_collected();
		collected_tariff_buffer.set(controller, old_value + collected);
		mid.set_tariff_collected(0.f);
	};
	state.world.execute_serial_over_nation([&](auto nid) {
		auto old = state.world.nation_get_treasury(nid);
		state.world.nation_set_treasury(nid, old + collected_tariff_buffer.get(nid));
	});

	set_profile_point(state, "tariffs");

	

	rebalance_needs_weights(state);
	

	// rebalance new needs
	if(state.world.consumption_category_size() > 0) {
		state.world.execute_parallel_over_market([&](auto market) {
			state.world.for_each_consumption_category([&](auto cat_id){
				auto price_w = state.world.consumption_category_get_price_pressure(cat_id);
				auto availability_w = state.world.consumption_category_get_availability_pressure(cat_id);
				auto base_w = state.world.consumption_category_get_base_pressure(cat_id);
				auto depends_on = state.world.consumption_category_get_scale_with(cat_id);

				ve::fp_vector max_cost = {0.f};
				state.world.for_each_commodity([&](auto cid) {
					auto price = state.world.market_get_price(market, cid);
					auto consumption_weight = state.world.consumption_category_get_weights(cat_id, cid);
					//if (consumption_weight == 0.f) return;
					//total_cost = total_cost + price * consumption_weight;
					max_cost = ve::max(max_cost, price * consumption_weight);
				});
				//auto average_cost = total_cost / float(state.world.commodity_size());

				ve::fp_vector total_weight = 0.f;
				state.world.for_each_commodity([&](auto cid) {
					auto index = cid.index() + cat_id.index() * state.world.commodity_size();
					auto consumption_weight = state.world.consumption_category_get_weights(cat_id, cid);
					auto current = state.world.market_get_local_consumption_weights(market, index);
					auto price = state.world.market_get_price(market, cid);
					auto availability = state.world.market_get_expected_probability_to_buy(market, cid);
					auto change =
						base_w
						+ availability_w * availability
						+ price_w * price * consumption_weight / max_cost;
					auto next = ve::select(
						max_cost == 0.f || consumption_weight == 0.f,
						0.f,
						ve::max(0.f, current + change * state.defines.alice_need_drift_speed)
					);
#ifndef NDEBUG
					ve::apply(
						[](dcon::market_id m, float amount) {
							assert(!m || (std::isfinite(amount) && amount >= 0.f));
						}, market, next
					);
#endif
					state.world.market_set_local_consumption_weights(market, index, next);
					total_weight = total_weight + next;
				});

				state.world.for_each_commodity([&](auto cid) {
					auto index = cid.index() + cat_id.index() * state.world.commodity_size();
					auto current = state.world.market_get_local_consumption_weights(market, index);

					auto next = ve::select(total_weight == 0.f, 0.01f, current / total_weight);
#ifndef NDEBUG
					ve::apply(
						[](dcon::market_id m, float amount) {
							assert(!m || (std::isfinite(amount) && amount >= 0.f));
						}, market, next
					);
#endif
					state.world.market_set_local_consumption_weights(market, index, next);
				});
			});


			ve::fp_vector max_satisfaction_per_pound = 0.f;
			state.world.for_each_consumption_category([&](auto cat_id) {
				ve::fp_vector cost = 0.f;
				state.world.for_each_commodity([&](auto cid) {
					auto price = state.world.market_get_price(market, cid);
					auto index = cid.index() + cat_id.index() * state.world.commodity_size();
					auto current = state.world.market_get_local_consumption_weights(market, index);
					auto consumption_weight = state.world.consumption_category_get_weights(cat_id, cid);
					cost = cost + current * price * consumption_weight;
				});
				auto satisfaction = state.world.consumption_category_get_satisfaction_score(cat_id);
				max_satisfaction_per_pound = ve::max(max_satisfaction_per_pound, satisfaction / (price_properties::commodity::min + cost));
			});
#ifndef NDEBUG
			ve::apply(
				[](dcon::market_id m, float amount) {
					assert(!m || (std::isfinite(amount) && amount >= 0.f));
				}, market, max_satisfaction_per_pound
			);
#endif
			// close to max satisfaction per pound -> 1
			state.world.for_each_consumption_category([&](auto cat_id) {
				ve::fp_vector cost = 0.f;
				state.world.for_each_commodity([&](auto cid) {
					auto price = state.world.market_get_price(market, cid);
					auto index = cid.index() + cat_id.index() * state.world.commodity_size();
					auto current = state.world.market_get_local_consumption_weights(market, index);
					auto consumption_weight = state.world.consumption_category_get_weights(cat_id, cid);
					cost = cost + current * price * consumption_weight;
				});
				auto satisfaction = state.world.consumption_category_get_satisfaction_score(cat_id);
				auto satisfaction_per_pound = satisfaction / (price_properties::commodity::min + cost);
				auto true_weight = ve::min(ve::fp_vector{1.f}, satisfaction_per_pound * 1.5f / max_satisfaction_per_pound);
				state.world.for_each_commodity([&](auto cid) {
					auto price = state.world.market_get_price(market, cid);
					auto index = cid.index() + cat_id.index() * state.world.commodity_size();
					auto current = state.world.market_get_local_consumption_weights(market, index);
#ifndef NDEBUG
					ve::apply(
						[](dcon::market_id m, float amount) {
							assert(!m || (std::isfinite(amount) && amount >= 0.f));
						}, market, current* true_weight
					);
#endif
					state.world.market_set_local_consumption_weights(market, index, current * true_weight);
				});
			});
		});
	}

	set_profile_point(state, "need weights");

	sanity_check(state);

	// #################
	// # PRICE UPDATES #
	// #################

	// at this point we already know supply and demand
	// so we can update prices

	/*
	adjust prices based on global production & consumption
	*/
	state.world.execute_serial_over_market([&](auto ids) {
		//auto costs = state.world.market_get_everyday_needs_costs(ids, state.culture_definitions.laborers);
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			if(!state.world.commodity_get_money_rgo(c))
				return;
			state.world.market_set_price(ids, c, state.world.commodity_get_cost(c) * 0.05f);
		});
	});

	set_profile_point(state, "reset prices");

	// price of labor

	concurrency::parallel_for(int32_t(0), int32_t(labor::total), [&](int32_t i) {
		province::ve_for_each_land_province(state, [&](auto ids) {
			ve::fp_vector supply = state.world.province_get_labor_supply(ids, i);
			ve::fp_vector demand = state.world.province_get_labor_demand(ids, i);
			auto current_price = state.world.province_get_labor_price(ids, i);
			auto old_price = current_price;

			if(presimulation) {
				current_price = current_price + price_properties::labor::change_fast<ve::fp_vector>(current_price, supply, demand) / (1.f + 100.f * presimulation_stage);
			} else {
				current_price = current_price + price_properties::labor::change<ve::fp_vector>(current_price, supply, demand);
			}

			auto nids = state.world.province_get_nation_from_province_ownership(ids);
			auto sids = state.world.province_get_state_membership(ids);
			auto mids = state.world.state_instance_get_market_from_local_market(sids);
			auto min_wage_factor = ve_pop_min_wage_factor(state, nids);

			// labor price control

			ve::fp_vector price_control = ve::fp_vector{ 0.f };
			price_control = price_control + state.world.market_get_life_needs_costs(mids, state.culture_definitions.secondary_factory_worker) * 1.0f;
			price_control = price_control + state.world.market_get_everyday_needs_costs(mids, state.culture_definitions.secondary_factory_worker) * 1.0f;

			// we reduce min wage if unemployment is too high
			// base min wage is decided by national multipliers
			// then we apply administrative efficiency

			price_control =
				price_control
				* ve::max(0.f, state.world.province_get_labor_supply_sold(ids, i) - 0.8f) * 5.f
				* state.world.province_get_control_ratio(ids)
				/ state.defines.alice_needs_scaling_factor
				* min_wage_factor;

			/*
			Rapid price changes are bad for simulation, instead we will slowly shift toward state mandated price
			*/

			price_control = ve::max(current_price, ve::max(price_properties::labor::min, price_control));

#ifndef NDEBUG
			ve::apply([&](auto value) { assert(std::isfinite(value)); }, current_price);
#endif
			current_price = ve::max(price_properties::labor::min, ve::min(price_control * 0.0001f + current_price * 0.9999f, price_properties::labor::max));
#ifndef NDEBUG
			ve::apply([&](auto value) { assert(value > 0.f); }, current_price);
#endif
			state.world.province_set_labor_price(ids, i, ve::select(nids == dcon::nation_id{}, old_price, current_price));
		});
	});

	set_profile_point(state, "update labor prices");

	concurrency::parallel_for(uint32_t(1), total_commodities, [&](uint32_t k) {
		dcon::commodity_id cid{ dcon::commodity_id::value_base_t(k) };
		//handling gold cost separetely
		if(state.world.commodity_get_money_rgo(cid)) {
			return;
		}
		state.world.execute_serial_over_market([&](auto ids) {
			ve::fp_vector supply = state.world.market_get_aggregated_supply_history(ids, cid);
			ve::fp_vector demand = state.world.market_get_aggregated_demand_history(ids, cid);
			auto current_price = ve_price(state, ids, cid);
			auto next_price = current_price;
			if(presimulation) {
				next_price = current_price + price_properties::commodity::change_fast<ve::fp_vector>(current_price, supply, demand) / (1.f + 100.f * presimulation_stage) ;				
				next_price = ve::select(demand <= supply, next_price * (demand + 0.000'001f) / (supply + 0.000'002f), next_price);
			} else {
				next_price = current_price + price_properties::commodity::change<ve::fp_vector>(current_price, supply, demand);
			}
#ifndef NDEBUG
			ve::apply([&](auto value) { assert(std::isfinite(value)); }, next_price);
#endif
			auto next_price_clamped = ve::min(ve::max(next_price, price_properties::commodity::min), price_properties::commodity::max);

			auto current_confidence = state.world.market_get_price_confidence(ids, cid);
			auto price_ratio = current_price / next_price_clamped;
			auto base_confidence_increase = 1.f / 365.f;
			if(presimulation) {
				base_confidence_increase = 1.f / 365.f * 3.f;
			}
			auto next_confidence = ve::max(ve::fp_vector{0.0001f}, ve::min(ve::fp_vector{ 1.f }, current_confidence * ve::min(price_ratio, 1.f / price_ratio) + base_confidence_increase));
#ifndef NDEBUG
			ve::apply([&](auto value) { assert(std::isfinite(value)); }, next_confidence);
#endif
			state.world.market_set_price(ids, cid, next_price_clamped);
			state.world.market_set_price_confidence(ids, cid, next_confidence);
		});
	});

	set_profile_point(state, "update commodity prices");

	services::update_price(state);

	set_profile_point(state, "update services prices");


	sanity_check(state);

	/*
	if(state.cheat_data.ecodump) {
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			float states_count = 0.f;
			float total_price = 0.f;
			float total_production = 0.f;
			float total_demand = 0.f;

			state.world.for_each_market([&](auto id) {
				states_count++;
				total_price += state.world.market_get_price(id, c);
				total_production += state.world.market_get_supply(id, c);
				total_demand += state.world.market_get_demand(id, c);
			});

			state.cheat_data.prices_dump_buffer += std::to_string(total_price / states_count) + ",";
			state.cheat_data.supply_dump_buffer += std::to_string(total_production) + ",";
			state.cheat_data.demand_dump_buffer += std::to_string(total_demand) + ",";
		});

		state.cheat_data.prices_dump_buffer += "\n";
		state.cheat_data.supply_dump_buffer += "\n";
		state.cheat_data.demand_dump_buffer += "\n";
	}
	*/

	/*
	DIPLOMATIC EXPENSES
	*/

	for(auto n : state.world.in_nation) {
		// Subject money transfers
		auto rel = state.world.nation_get_overlord_as_subject(n);
		auto overlord = state.world.overlord_get_ruler(rel);

		if(overlord) {
			auto transferamt = estimate_subject_payments_paid(state, n);
			state.world.nation_set_treasury(n,state.world.nation_get_treasury(n) - transferamt);
			state.world.nation_set_treasury(overlord, state.world.nation_get_treasury(overlord) + transferamt);
		}

		for(auto uni : n.get_unilateral_relationship_as_source()) {
			if(uni.get_war_subsidies()) {
				auto sub_size = estimate_war_subsidies(state, uni.get_target(), uni.get_source());

				if(sub_size <= n.get_treasury()) {
					n.set_treasury(n.get_treasury() - sub_size);
					auto& current = uni.get_target().get_treasury();
					uni.get_target().set_treasury(current + sub_size);
				} else {
					uni.set_war_subsidies(false);

					notification::post(state, notification::message{
						[source = n.id, target = uni.get_target().id](sys::state& state, text::layout_base& contents) {
							text::add_line(state, contents, "msg_wsub_end_1", text::variable_type::x, source, text::variable_type::y, target);
						},
						"msg_wsub_end_title",
						n.id, uni.get_target().id, dcon::nation_id{},
						sys::message_base_type::war_subsidies_end,
						dcon::province_id{ }
					});
				}
			}
			if(uni.get_reparations() && state.current_date < n.get_reparations_until()) {
				auto const tax_eff = nations::tribute_efficiency(state, n);
				auto total_tax_base = n.get_total_rich_income() + n.get_total_middle_income() + n.get_total_poor_income();

				auto payout = total_tax_base * tax_eff * state.defines.reparations_tax_hit;
				auto capped_payout = std::min(n.get_treasury(), payout);
				assert(capped_payout >= 0.0f);
				n.set_treasury(n.get_treasury() - capped_payout);
				auto& current = uni.get_target().get_treasury();
				uni.get_target().set_treasury(current + capped_payout);
			}
		}
	}

	// essentially upper bound on wealth in the system
	state.inflation = 1.f;
	if(presimulation) {
		state.inflation = presimulation_stage * 0.99f + 0.01f;
	}

	sanity_check(state);

	if(!presimulation) {
		run_private_investment(state);
	}

	sanity_check(state);

	if(((state.current_date.value + 16 + (date_offset % 16)) % 16) == 0) {
		auto index = economy::most_recent_price_record_index(state, date_offset);
		concurrency::parallel_for(uint32_t(1), total_commodities, [&](uint32_t k) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(k) };
			auto median_price = economy::median_price_slow(state, cid);
			state.world.commodity_set_median_price(cid, median_price);
			state.world.commodity_set_price_record(cid, index, median_price);
		});
	}

	auto ymd_date = state.current_date.to_ymd(state.start_date + 365 + (date_offset % 365));
	if(((ymd_date.month % 3) == 0) && (ymd_date.day == 1)) {
		auto index = economy::most_recent_gdp_record_index(state, date_offset);
		for(auto n : state.world.in_nation) {
			n.set_gdp_record(index, economy::gdp::value_nation_adjusted(state, n));
		}
	}

	// ####################
	// # STATS COLLECTION #
	// ####################

	//write gdp and total savings to file
	if(state.cheat_data.ecodump) {
		nation_monetary_breakdown total_money {};
		state.world.for_each_nation([&](auto n){
			auto money_pools = economy::breakdown_nation_monetary_structure(state, n);
			total_money.total += money_pools.total;
			total_money.artisans += money_pools.artisans;
			total_money.bank += money_pools.bank;
			total_money.educators += money_pools.educators;
			total_money.factory += money_pools.factory;
			total_money.investment_pool += money_pools.investment_pool;
			total_money.landlords += money_pools.landlords;
			total_money.market += money_pools.market;
			total_money.nation += money_pools.nation;
			total_money.pops += money_pools.pops;
			total_money.ports += money_pools.ports;
			total_money.rgo += money_pools.rgo;
		});

		state.cheat_data.savings_buffer += std::to_string(total_money.total) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.artisans) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.bank) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.educators) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.factory) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.investment_pool) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.landlords) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.market) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.nation) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.pops) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.ports) + ";";
		state.cheat_data.savings_buffer += std::to_string(total_money.rgo) + "\n";

		/*
		float total_savings_pops[20] = { };

		for(int i = 0; i < 20; i++) {
			total_savings_pops[i] = 0.f;
		}
		state.world.for_each_pop([&](auto pop) {
			total_savings_pops[state.world.pop_get_poptype(pop).id.index()] += state.world.pop_get_savings(pop);
		});

		float total_savings_markets = 0.f;
		state.world.for_each_market([&](auto market) {
			total_savings_markets += state.world.market_get_stockpile(market, economy::money);
		});

		float total_savings_nations = 0.f;
		float total_investment_pool = 0.f;
		state.world.for_each_nation([&](auto nation) {
			total_savings_nations += state.world.nation_get_treasury(nation);
			total_investment_pool += state.world.nation_get_private_investment(nation);
		});

		if(state.cheat_data.savings_buffer.size() == 0) {
			state.world.for_each_pop_type([&](auto pop_type) {
				state.cheat_data.savings_buffer += std::to_string(total_savings_pops[pop_type.index()]);
				state.cheat_data.savings_buffer += ";";
			});
			state.cheat_data.savings_buffer += std::to_string(total_savings_markets);
			state.cheat_data.savings_buffer += ";";
			state.cheat_data.savings_buffer += std::to_string(total_savings_nations);
			state.cheat_data.savings_buffer += ";";
			state.cheat_data.savings_buffer += std::to_string(total_investment_pool);
			state.cheat_data.savings_buffer += "\n";
		}
		*/
	}

	set_profile_point(state, "random data");

	sanity_check(state);
#endif

}

void regenerate_unsaved_values(sys::state& state) {
	state.culture_definitions.rgo_workers.clear();
	for(auto pt : state.world.in_pop_type) {
		if(pt.get_is_paid_rgo_worker())
			state.culture_definitions.rgo_workers.push_back(pt);
	}

	auto const total_commodities = state.world.commodity_size();
	for(uint32_t k = 1; k < total_commodities; ++k) {
		dcon::commodity_id cid{ dcon::commodity_id::value_base_t(k) };
		for(auto pt : state.world.in_pop_type) {
			if(pt != state.culture_definitions.slaves) {
				if(pt.get_life_needs(cid) > 0.0f)
					state.world.commodity_set_is_life_need(cid, true);
				if(pt.get_everyday_needs(cid) > 0.0f)
					state.world.commodity_set_is_everyday_need(cid, true);
				if(pt.get_luxury_needs(cid) > 0.0f)
					state.world.commodity_set_is_luxury_need(cid, true);
			}
		}
	}

	state.world.market_resize_intermediate_demand(state.world.commodity_size());

	state.world.market_resize_life_needs_costs(state.world.pop_type_size());
	state.world.market_resize_everyday_needs_costs(state.world.pop_type_size());
	state.world.market_resize_luxury_needs_costs(state.world.pop_type_size());

	state.world.market_resize_life_needs_scale(state.world.pop_type_size());
	state.world.market_resize_everyday_needs_scale(state.world.pop_type_size());
	state.world.market_resize_luxury_needs_scale(state.world.pop_type_size());

	state.world.market_resize_satisfied_ratio_of_max_life_needs(state.world.pop_type_size());
	state.world.market_resize_satisfied_ratio_of_max_everyday_needs(state.world.pop_type_size());
	state.world.market_resize_satisfied_ratio_of_max_luxury_needs(state.world.pop_type_size());
	state.world.market_resize_satisfied_ratio_of_demanded_life_needs(state.world.pop_type_size());
	state.world.market_resize_satisfied_ratio_of_demanded_everyday_needs(state.world.pop_type_size());
	state.world.market_resize_satisfied_ratio_of_demanded_luxury_needs(state.world.pop_type_size());

	state.world.nation_resize_total_stockpiles(state.world.commodity_size());

	recreate_total_government_stockpiles(state);
}

float government_consumption(sys::state& state, dcon::nation_id n, dcon::commodity_id c) {

	auto capital = state.world.nation_get_capital(n);
	auto capital_state = state.world.province_get_state_membership(capital);
	float o_adjust = 0.0f;
	// ONLY attempt to consume the goods if we control the capital state. If we do not, then we get the maximum overseas penality. This isn't ideal, but the best solution until it can be reworked
	if(state.world.state_instance_get_nation_from_state_control(capital_state) == n) {
		auto overseas_factor =
			state.defines.province_overseas_penalty *
			float(
				state.world.nation_get_owned_province_count(n)
				- state.world.nation_get_central_province_count(n)
			);
		if(overseas_factor > 0) {
			if(
				state.world.commodity_get_overseas_penalty(c)
				&& (
					state.world.commodity_get_is_available_from_start(c)
					|| state.world.nation_get_unlocked_commodities(n, c)
					)
			) {
				o_adjust = overseas_factor;
			}
		}
	}

	auto total = 0.f;

	state.world.nation_for_each_state_control_as_nation(n, [&](auto soid) {
		auto market =
			state.world.state_instance_get_market_from_local_market(
				state.world.state_control_get_state(soid)
			);
		total = total + economy::combined_government_stockpile_demand(state, market, c);
	});

	return total + o_adjust;
}

float nation_pop_consumption(sys::state& state, dcon::nation_id n, dcon::commodity_id c) {
	auto amount = 0.f;
	if(state.world.commodity_get_is_available_from_start(c) || state.world.nation_get_unlocked_commodities(n, c)) {
		state.world.nation_for_each_state_ownership(n, [&](auto soid) {
			auto si = state.world.state_ownership_get_state(soid);
			auto m = state.world.state_instance_get_market_from_local_market(si);
			state.world.for_each_pop_type([&](dcon::pop_type_id pt) {
				amount += (
						state.world.pop_type_get_life_needs(pt, c)
							* state.world.market_get_life_needs_weights(m, c)
						+ state.world.pop_type_get_everyday_needs(pt, c)
							* state.world.market_get_everyday_needs_weights(m, c)
						+ state.world.pop_type_get_luxury_needs(pt, c)
							* state.world.market_get_luxury_needs_weights(m, c)
					)
					* state.world.nation_get_demographics(n, demographics::to_key(state, pt))

					/ state.defines.alice_needs_scaling_factor;
			});
		});
	}
	return amount;
}

float nation_total_imports(sys::state& state, dcon::nation_id n) {
	float t_total = 0.0f;

	auto const total_commodities = state.world.commodity_size();

	for(uint32_t k = 1; k < total_commodities; ++k) {
		dcon::commodity_id cid{ dcon::commodity_id::value_base_t(k) };
		state.world.nation_for_each_state_ownership(n, [&](auto soid) {
			auto local_state = state.world.state_ownership_get_state(soid);
			auto market = state.world.state_instance_get_market_from_local_market(local_state);
			t_total += state.world.market_get_price(market, cid) * state.world.market_get_import(market, cid);
		});
	}

	return t_total;
}

float nation_total_exports(sys::state& state, dcon::nation_id n) {
	float t_total = 0.0f;

	auto const total_commodities = state.world.commodity_size();

	for(uint32_t k = 1; k < total_commodities; ++k) {
		dcon::commodity_id cid{ dcon::commodity_id::value_base_t(k) };
		state.world.nation_for_each_state_ownership(n, [&](auto soid) {
			auto local_state = state.world.state_ownership_get_state(soid);
			auto market = state.world.state_instance_get_market_from_local_market(local_state);
			t_total += state.world.market_get_price(market, cid) * state.world.market_get_export(market, cid);
		});
	}

	return t_total;
}

float estimate_gold_income(sys::state& state, dcon::nation_id n) {
	auto amount = 0.f;
	for(auto poid : state.world.nation_get_province_ownership_as_nation(n)) {
		auto prov = poid.get_province();
		state.world.for_each_commodity([&](dcon::commodity_id c) {
			if(state.world.commodity_get_money_rgo(c)) {
				amount +=
					state.world.province_get_rgo_output(prov, c)
					* state.world.commodity_get_cost(c);
			}
		});
	}
	return amount * state.defines.gold_to_cash_rate;
}

float estimate_tariff_import_income(sys::state& state, dcon::nation_id n) {
	float result = 0.f;
	state.world.for_each_commodity([&](dcon::commodity_id cid) {
		state.world.for_each_trade_route([&](auto route) {
			if(!economy::is_trade_route_relevant(state, route, n)) return;
			trade_and_tariff route_data = explain_trade_route_commodity(state, route, cid);
			if(route_data.target_nation == n) {
				result += route_data.tariff_target;
			}
		});
	});
	return result;
}

float estimate_tariff_export_income(sys::state& state, dcon::nation_id n) {
	float result = 0.f;
	state.world.for_each_commodity([&](dcon::commodity_id cid) {
		state.world.for_each_trade_route([&](auto route) {
			if(!economy::is_trade_route_relevant(state, route, n)) return;
			trade_and_tariff route_data = explain_trade_route_commodity(state, route, cid);
			if(route_data.origin_nation == n) {
				result += route_data.tariff_origin;
			}
		});
	});
	return result;
}

float estimate_war_subsidies_income(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;

	for(auto uni : state.world.nation_get_unilateral_relationship_as_target(n)) {
		if(uni.get_war_subsidies()) {
			total += estimate_war_subsidies(state, uni.get_target(), uni.get_source());
		}
	}
	return total;
}
float estimate_reparations_income(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	for(auto uni : state.world.nation_get_unilateral_relationship_as_target(n)) {
		if(uni.get_reparations() && state.current_date < uni.get_source().get_reparations_until()) {
			auto source = uni.get_source();
			auto const tax_eff = nations::tribute_efficiency(state, n);
			auto total_tax_base = state.world.nation_get_total_rich_income(source) +
				state.world.nation_get_total_middle_income(source) +
				state.world.nation_get_total_poor_income(source);
			auto payout = total_tax_base * tax_eff * state.defines.reparations_tax_hit;
			total += payout;
		}
	}
	return total;
}

float estimate_war_subsidies_spending(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;

	for(auto uni : state.world.nation_get_unilateral_relationship_as_source(n)) {
		if(uni.get_war_subsidies()) {
			total += estimate_war_subsidies(state, uni.get_target(), uni.get_source());
		}
	}

	return total;
}

float estimate_reparations_spending(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	if(state.current_date < state.world.nation_get_reparations_until(n)) {
		for(auto uni : state.world.nation_get_unilateral_relationship_as_source(n)) {
			if(uni.get_reparations()) {
				auto const tax_eff = nations::tribute_efficiency(state, n);
				auto total_tax_base = state.world.nation_get_total_rich_income(n) +
					state.world.nation_get_total_middle_income(n) +
					state.world.nation_get_total_poor_income(n);
				auto payout = total_tax_base * tax_eff * state.defines.reparations_tax_hit;
				total += payout;
			}
		}
	}
	return total;
}

float estimate_diplomatic_balance(sys::state& state, dcon::nation_id n) {
	float w_sub = estimate_war_subsidies_income(state, n) - estimate_war_subsidies_spending(state, n);
	float w_reps = estimate_reparations_income(state, n) - estimate_reparations_spending(state, n);
	float subject_payments = estimate_subject_payments_received(state, n) - estimate_subject_payments_paid(state, n);
	return w_sub + w_reps + subject_payments;
}
float estimate_diplomatic_income(sys::state& state, dcon::nation_id n) {
	// potential OOS in subject_payments (if parallelizing over nations while changing tax levels)
	float w_sub = estimate_war_subsidies_income(state, n);
	float w_reps = estimate_reparations_income(state, n);
	float subject_payments = estimate_subject_payments_received(state, n);
	return w_sub + w_reps + subject_payments;
}
float estimate_diplomatic_expenses(sys::state& state, dcon::nation_id n) {
	float w_sub = estimate_war_subsidies_spending(state, n);
	float w_reps = estimate_reparations_spending(state, n);
	float subject_payments = estimate_subject_payments_paid(state, n);
	return w_sub + w_reps + subject_payments;
}



float estimate_max_domestic_investment(sys::state& state, dcon::nation_id n) {
	return economy::estimate_next_budget(state, n);
}

float estimate_current_domestic_investment(sys::state& state, dcon::nation_id n) {
	return estimate_max_domestic_investment(state, n) * float(state.world.nation_get_domestic_investment_spending(n)) / 100.0f;
}

float estimate_today_land_spending(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	uint32_t total_commodities = state.world.commodity_size();
	state.world.nation_for_each_state_control(n, [&](auto soid) {
		auto local_state = state.world.state_control_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);
		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			total +=
				state.world.market_get_army_demand(market, cid)
				* state.world.market_get_price(market, cid);
				//* state.world.market_get_actual_probability_to_buy(market, cid);
		}
	});
	return total;
}

float estimate_today_naval_spending(sys::state& state, dcon::nation_id n) {
	float total = 0.0f;
	uint32_t total_commodities = state.world.commodity_size();
	state.world.nation_for_each_state_control(n, [&](auto soid) {
		auto local_state = state.world.state_control_get_state(soid);
		auto market = state.world.state_instance_get_market_from_local_market(local_state);
		for(uint32_t i = 1; i < total_commodities; ++i) {
			dcon::commodity_id cid{ dcon::commodity_id::value_base_t(i) };
			total += state.world.market_get_navy_demand(market, cid)
				* state.world.market_get_price(market, cid);
				//* state.world.market_get_actual_probability_to_buy(market, cid);
		}
	});
	return total;
}

float estimate_war_subsidies(sys::state& state, dcon::nation_id target, dcon::nation_id source) {
	/* total-nation-tax-base x defines:WARSUBSIDIES_PERCENT */

	auto target_m_costs = (state.world.nation_get_total_rich_income(target) + state.world.nation_get_total_middle_income(target) + state.world.nation_get_total_poor_income(target)) * state.defines.warsubsidies_percent;
	auto source_m_costs = (state.world.nation_get_total_rich_income(source) + state.world.nation_get_total_middle_income(source) + state.world.nation_get_total_poor_income(source)) * state.defines.warsubsidies_percent;
	return std::min(target_m_costs, source_m_costs);
}

float estimate_subject_payments_paid(sys::state& state, dcon::nation_id n) {
	auto tax = explain_tax_income(state, n);
	auto collected_tax = tax.mid + tax.poor + tax.rich;

	auto rel = state.world.nation_get_overlord_as_subject(n);
	auto overlord = state.world.overlord_get_ruler(rel);

	if(overlord) {
		auto transferamt = collected_tax;

		if(state.world.nation_get_is_substate(n)) {
			transferamt *= state.defines.alice_substate_subject_money_transfer / 100.f;
		} else {
			transferamt *= state.defines.alice_puppet_subject_money_transfer / 100.f;
		}

		return std::max(0.f, std::min(state.world.nation_get_treasury(n), transferamt));
	}

	return 0;
}

float estimate_subject_payments_received(sys::state& state, dcon::nation_id o) {
	auto res = 0.0f;
	for(auto n : state.world.in_nation) {
		auto rel = state.world.nation_get_overlord_as_subject(n);
		auto overlord = state.world.overlord_get_ruler(rel);

		if(overlord == o) {
			auto tax = explain_tax_income(state, n);
			auto const collected_tax = tax.poor + tax.mid + tax.rich;
			auto transferamt = collected_tax;

			if(state.world.nation_get_is_substate(n)) {
				transferamt *= state.defines.alice_substate_subject_money_transfer / 100.f;
			} else {
				transferamt *= state.defines.alice_puppet_subject_money_transfer / 100.f;
			}

			res += transferamt;
		}
	}

	return res;
}

construction_status province_building_construction(sys::state& state, dcon::province_id p, province_building_type t) {
	assert(0 <= int32_t(t) && int32_t(t) < int32_t(economy::max_building_types));
	for(auto pb_con : state.world.province_get_province_building_construction(p)) {
		if(pb_con.get_type() == uint8_t(t)) {
			float modifier = construction_build_cost_multiplier(state, pb_con);
			float total = 0.0f;
			float purchased = 0.0f;
			for(uint32_t i = 0; i < commodity_set::set_size; ++i) {
				auto current = pb_con.get_purchased_goods()[i];
				auto required = state.economy_definitions.building_definitions[int32_t(t)].cost.commodity_amounts[i]
					* modifier;

				total += required;
				purchased += std::min(current, required);
			}
			return construction_status{ total > 0.0f ? purchased / total : 0.0f, true };
		}
	}
	return construction_status{ 0.0f, false };
}

construction_status factory_upgrade(sys::state& state, dcon::factory_id f) {
	auto in_prov = state.world.factory_get_province_from_factory_location(f);
	auto fac_type = state.world.factory_get_building_type(f);

	for(auto st_con : state.world.province_get_factory_construction(in_prov)) {
		if(st_con.get_type() == fac_type) {
			float modifier = factory_build_cost_multiplier(state, st_con.get_nation(), st_con.get_province(), st_con.get_is_pop_project());
			float total = 0.0f;
			float purchased = 0.0f;
			auto& goods = state.world.factory_type_get_construction_costs(fac_type);
			for(uint32_t i = 0; i < commodity_set::set_size; ++i) {
				total += goods.commodity_amounts[i] * modifier;
				purchased += st_con.get_purchased_goods()[i];
			}
			return construction_status{ total > 0.0f ? purchased / total : 0.0f, true };
		}
	}

	return construction_status{ 0.0f, false };
}


void add_factory_level_to_province(sys::state& state, dcon::province_id p, dcon::factory_type_id t) {
	float base_size = float(state.world.factory_type_get_base_workforce(t));

	// addition of factory level also represents construction of a small factory town or factory-adjacent housing

	auto town_size = state.world.province_get_advanced_province_building_max_private_size(p, advanced_province_buildings::list::local_cities_and_towns);
	state.world.province_set_advanced_province_building_max_private_size(p, advanced_province_buildings::list::local_cities_and_towns, town_size + base_size * 0.1f);

	// Since construction may be queued together with refit, check if there is factory to add level to
	for(auto f : state.world.province_get_factory_location(p)) {
		auto fac = f.get_factory();
		auto fac_type = fac.get_building_type();
		if(f.get_factory().get_building_type() == t) {
			f.get_factory().set_size(f.get_factory().get_size() + fac_type.get_base_workforce());
			return;
		}
	}

	// Only then create new factory
	auto new_fac = fatten(state.world, state.world.create_factory());
	new_fac.set_building_type(t);
	new_fac.set_size(base_size);
	new_fac.set_unqualified_employment(base_size * 0.1f);
	new_fac.set_primary_employment(0.f);
	new_fac.set_secondary_employment(0.f);
	auto n = state.world.province_get_nation_from_province_ownership(p);
	auto output =  state.world.factory_type_get_output(t);
	auto national_t = state.world.nation_get_factory_goods_throughput(n, output);
	auto nationnal_fac_t = state.world.nation_get_modifier_values(n, sys::national_mod_offsets::factory_throughput);
	new_fac.set_technology_scale(0.05f + std::max(0.f, national_t) + std::max(0.f, nationnal_fac_t));
	state.world.try_create_factory_location(new_fac, p);
	set_initial_factory_values(state, new_fac);
}

void change_factory_type_in_province(sys::state& state, dcon::province_id p, dcon::factory_type_id t, dcon::factory_type_id refit_target) {
	assert(refit_target);
	dcon::factory_id factory_target;

	// Find factory in question
	for(auto f : state.world.province_get_factory_location(p)) {
		if(f.get_factory().get_building_type() == t) {
			factory_target = f.get_factory();
		}
	}

	// Find existing factories to sum
	for(auto f : state.world.province_get_factory_location(p))
		if(f.get_factory().get_building_type() == refit_target) {
			auto factory_size_1 = f.get_factory().get_size();
			auto factory_size_2 = state.world.factory_get_size(factory_target);
			f.get_factory().set_size(factory_size_1 + factory_size_2);

			state.world.delete_factory(factory_target);
			return;
		}

	// Change type of the given factory
	for(auto f : state.world.province_get_factory_location(p)) {
		if(f.get_factory().get_building_type() == t) {
			f.get_factory().set_building_type(refit_target);
			return;
		}
	}
}

// This is used specifically in AI calculations, and omits subject income calculation because that requires iterating over all subjects and calculating their tax income seperately, will will cause OOS when parallelized over nations in ai::update_budget
float estimate_daily_income_ai(sys::state& state, dcon::nation_id n) {
	auto tax = explain_tax_income(state, n);
	auto tariff = estimate_tariff_export_income(state, n) + estimate_tariff_import_income(state, n);
	auto gold = estimate_gold_income(state, n);
	auto diplomacy = estimate_war_subsidies_income(state, n) + estimate_reparations_income(state, n);
	return tax.mid + tax.poor + tax.rich + tariff + gold + diplomacy;
}

/* TODO -
 * This should return what we think the income will be next day, and as a result wont account for any unprecedented actions
 * return value is passed directly into text::fp_currency{} without adulteration.
 */
float estimate_daily_income(sys::state& state, dcon::nation_id n) {
	auto tax = explain_tax_income(state, n);
	auto tariff = estimate_tariff_export_income(state, n) + estimate_tariff_import_income(state, n);
	auto gold = estimate_gold_income(state, n);
	auto diplomacy = estimate_diplomatic_income(state, n);
	return tax.mid + tax.poor + tax.rich + tariff + gold + diplomacy;
}

void try_add_factory_to_state(sys::state& state, dcon::state_instance_id s, dcon::factory_type_id t) {
	auto province = state.world.state_instance_get_capital(s);

	if(state.world.factory_type_get_is_coastal(t)) {
		if(!state.world.province_get_port_to(province))
			return; // requires coast to build coastal factory
	}

	auto existing_constructions = state.world.province_get_factory_construction(province);
	int32_t num_factories = 0;
	for(auto prj : existing_constructions) {
		if(!prj.get_is_upgrade())
			++num_factories;
		if(prj.get_type() == t)
			return; // can't duplicate type
	}

	// is there an upgrade target ?
	for(auto f : state.world.province_get_factory_location(province)) {
		++num_factories;
		if(f.get_factory().get_building_type() == t)
			return; // can't build another of this type
	}

	// do not limit it here because it feels bad to get factory reward from event not appearing at all

	add_factory_level_to_province(state, province, t);
}

command::budget_settings_data budget_minimums(sys::state& state, dcon::nation_id n) {
	command::budget_settings_data result;
	result.education_spending = 0;
	result.military_spending = 0;
	result.administrative_spending = 0;
	result.social_spending = 0;
	result.land_spending = 0;
	result.naval_spending = 0;
	result.construction_spending = 0;
	result.stockpile_spending = 0;
	result.poor_tax = 0;
	result.middle_tax = 0;
	result.rich_tax = 0;
	result.tariffs_import = 0;
	result.tariffs_export = 0;
	result.domestic_investment = 0;
	result.overseas = 0;
	{
		auto min_tariff = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tariff));
		result.tariffs_import = int8_t(std::clamp(min_tariff, 0, 100));
		result.tariffs_export = int8_t(std::clamp(min_tariff, 0, 100));
	}
	{
		auto min_tax = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tax));
		result.poor_tax = int8_t(std::clamp(min_tax, 0, 100));
		result.middle_tax = int8_t(std::clamp(min_tax, 0, 100));
		result.rich_tax = int8_t(std::clamp(min_tax, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_military_spending));
		result.military_spending = int8_t(std::clamp(min_spend, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_social_spending));
		result.social_spending = int8_t(std::clamp(min_spend, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_domestic_investment));
		result.domestic_investment = int8_t(std::clamp(min_spend, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_land_upkeep));
		result.land_spending = int8_t(std::clamp(min_spend, 0, 100));
	}
	return result;
}
command::budget_settings_data budget_maximums(sys::state& state, dcon::nation_id n) {
	command::budget_settings_data result;
	result.education_spending = 100;
	result.military_spending = 100;
	result.administrative_spending = 100;
	result.social_spending = 100;
	result.land_spending = 100;
	result.naval_spending = 100;
	result.construction_spending = 100;
	result.stockpile_spending = 100;
	result.poor_tax = 100;
	result.middle_tax = 100;
	result.rich_tax = 100;
	result.tariffs_import = 100;
	result.tariffs_export = 100;
	result.domestic_investment = 100;
	result.overseas = 100;
	result.subsidies = 100;

	{
		auto min_tariff = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tariff));
		auto max_tariff = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_tariff));
		max_tariff = std::max(min_tariff, max_tariff);

		result.tariffs_import = int8_t(std::clamp(max_tariff, 0, 100));
		result.tariffs_export = int8_t(std::clamp(max_tariff, 0, 100));
	}
	{
		auto min_tax = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tax));
		auto max_tax = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_tax));
		if(max_tax <= 0)
			max_tax = 100;
		max_tax = std::max(min_tax, max_tax);

		result.poor_tax = int8_t(std::clamp(max_tax, 0, 100));
		result.middle_tax = int8_t(std::clamp(max_tax, 0, 100));
		result.rich_tax = int8_t(std::clamp(max_tax, 0, 100));
	}
	{
		auto min_spend =
			int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_military_spending));
		auto max_spend =
			int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_military_spending));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		result.military_spending = int8_t(std::clamp(max_spend, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_social_spending));
		auto max_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_social_spending));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		result.social_spending = int8_t(std::clamp(max_spend, 0, 100));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_domestic_investment));
		auto max_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_domestic_investment));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		result.domestic_investment = int8_t(std::clamp(max_spend, 0, 100));
	}
	return result;
}
void bound_budget_settings(sys::state& state, dcon::nation_id n) {
	{
		auto min_tariff = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tariff));
		auto max_tariff = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_tariff));
		max_tariff = std::max(min_tariff, max_tariff);

		{
			auto& tariff = state.world.nation_get_tariffs_import(n);
			state.world.nation_set_tariffs_import(n, int8_t(std::clamp(std::clamp(int32_t(tariff), min_tariff, max_tariff), 0, 100)));
		}

		{
			auto& tariff = state.world.nation_get_tariffs_export(n);
			state.world.nation_set_tariffs_export(n, int8_t(std::clamp(std::clamp(int32_t(tariff), min_tariff, max_tariff), 0, 100)));
		}
	}
	{
		auto min_tax = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_tax));
		auto max_tax = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_tax));
		if(max_tax <= 0)
			max_tax = 100;
		max_tax = std::max(min_tax, max_tax);

		auto& ptax = state.world.nation_get_poor_tax(n);
		state.world.nation_set_poor_tax(n, int8_t(std::clamp(std::clamp(int32_t(ptax), min_tax, max_tax), 0, 100)));
		auto& mtax = state.world.nation_get_middle_tax(n);
		state.world.nation_set_middle_tax(n, int8_t(std::clamp(std::clamp(int32_t(mtax), min_tax, max_tax), 0, 100)));
		auto& rtax = state.world.nation_get_rich_tax(n);
		state.world.nation_set_rich_tax(n, int8_t(std::clamp(std::clamp(int32_t(rtax), min_tax, max_tax), 0, 100)));
	}
	{
		auto min_spend =
			int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_military_spending));
		auto max_spend =
			int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_military_spending));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		auto& v = state.world.nation_get_military_spending(n);
		state.world.nation_set_military_spending(n, int8_t(std::clamp(std::clamp(int32_t(v), min_spend, max_spend), 0, 100)));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_social_spending));
		auto max_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_social_spending));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		auto& v = state.world.nation_get_social_spending(n);
		state.world.nation_set_social_spending(n, int8_t(std::clamp(std::clamp(int32_t(v), min_spend, max_spend), 0, 100)));
	}
	{
		auto min_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_domestic_investment));
		auto max_spend = int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::max_domestic_investment));
		if(max_spend <= 0)
			max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		auto& v = state.world.nation_get_domestic_investment_spending(n);
		state.world.nation_set_domestic_investment_spending(n, int8_t(std::clamp(std::clamp(int32_t(v), min_spend, max_spend), 0, 100)));
	}
	{
		auto min_spend =
			int32_t(100.0f * state.world.nation_get_modifier_values(n, sys::national_mod_offsets::min_land_upkeep));
		auto max_spend = 100;
		max_spend = std::max(min_spend, max_spend);

		auto& v = state.world.nation_get_land_spending(n);
		state.world.nation_set_land_spending(n, int8_t(std::clamp(std::clamp(int32_t(v), min_spend, max_spend), 0, 100)));
	}
}

void prune_factories(sys::state& state) {
	for(auto pid : state.world.in_province) {
		auto owner = pid.get_nation_from_province_ownership();
		auto rules = owner.get_combined_issue_rules();

		if(owner.get_is_player_controlled() && (rules & issue_rule::destroy_factory) != 0) // not for players who can manually destroy
			continue;

		dcon::factory_id deletion_choice;
		int32_t factory_count = 0;

		for(auto f : state.world.province_get_factory_location(pid)) {
			++factory_count;
			auto desired_employment = factory_total_desired_employment(state, f.get_factory());
			bool unprofitable = f.get_factory().get_unprofitable();
			if(
				(desired_employment < 50.f && unprofitable)
				&&
				(!deletion_choice || state.world.factory_get_size(deletion_choice) > f.get_factory().get_size())
			) {
				deletion_choice = f.get_factory();
			}
		}

		// aggressive pruning
		// to help building more healthy economy instead of 1 profitable giant factory with 6 small 0 scale factories
		if(deletion_choice) {
			auto production_type = state.world.factory_get_building_type(deletion_choice);
			state.world.delete_factory(deletion_choice);

			for(auto proj : pid.get_factory_construction()) {
				if(proj.get_type() == production_type) {
					state.world.delete_factory_construction(proj);
					break;
				}
			}
		}
	}
}

dcon::modifier_id get_province_selector_modifier(sys::state& state) {
	return state.economy_definitions.selector_modifier;
}

dcon::modifier_id get_province_immigrator_modifier(sys::state& state) {
	return state.economy_definitions.immigrator_modifier;
}

void go_bankrupt(sys::state& state, dcon::nation_id n) {
	auto& debt = state.world.nation_get_local_loan(n);

	/*
	 If a nation cannot pay and the amount it owes is less than define:SMALL_DEBT_LIMIT, the nation it owes money to gets an on_debtor_default_small event (with the nation defaulting in the from slot). Otherwise, the event is pulled from on_debtor_default. The nation then goes bankrupt. It receives the bad_debter modifier for define:BANKRUPCY_EXTERNAL_LOAN_YEARS years (if it goes bankrupt again within this period, creditors receive an on_debtor_default_second event). It receives the in_bankrupcy modifier for define:BANKRUPCY_DURATION days. Its prestige is reduced by a factor of define:BANKRUPCY_FACTOR, and each of its pops has their militancy increase by 2.
	*/
	auto existing_br = state.world.nation_get_bankrupt_until(n);
	if(existing_br && state.current_date < existing_br) {
		for(auto gn : state.great_nations) {
			if(gn.nation && gn.nation != n) {
				event::fire_fixed_event(state, state.national_definitions.on_debtor_default_second, trigger::to_generic(gn.nation), event::slot_type::nation, gn.nation, trigger::to_generic(n), event::slot_type::nation);
			}
		}
	} else if(debt >= -state.defines.small_debt_limit) {
		for(auto gn : state.great_nations) {
			if(gn.nation && gn.nation != n) {
				event::fire_fixed_event(state, state.national_definitions.on_debtor_default_small, trigger::to_generic(gn.nation), event::slot_type::nation, gn.nation, trigger::to_generic(n), event::slot_type::nation);
			}
		}
	} else {
		for(auto gn : state.great_nations) {
			if(gn.nation && gn.nation != n) {
				event::fire_fixed_event(state, state.national_definitions.on_debtor_default, trigger::to_generic(gn.nation), event::slot_type::nation, gn.nation, trigger::to_generic(n), event::slot_type::nation);
			}
		}
	}

	// RESET MONEY: POTENTIAL MERGE CONFLICT WITH SNEAKBUG'S FUTURE CHANGES
	state.world.nation_set_treasury(n, 0.f);

	sys::add_modifier_to_nation(state, n, state.national_definitions.in_bankrupcy, state.current_date + int32_t(state.defines.bankrupcy_duration * 365));
	sys::add_modifier_to_nation(state, n, state.national_definitions.bad_debter, state.current_date + int32_t(state.defines.bankruptcy_external_loan_years * 365));

	state.world.nation_set_local_loan(n, 0.0f);
	state.world.nation_set_is_debt_spending(n, false);
	state.world.nation_set_bankrupt_until(n, state.current_date + int32_t(state.defines.bankrupcy_duration * 365));

	notification::post(state, notification::message{
		[n](sys::state& state, text::layout_base& contents) {
			text::add_line(state, contents, "msg_bankruptcy_1", text::variable_type::x, n);
		},
		"msg_bankruptcy_title",
		n, dcon::nation_id{}, dcon::nation_id{},
		sys::message_base_type::bankruptcy,
		dcon::province_id{ }
	});
}

float estimate_investment_pool_daily_loss(sys::state& state, dcon::nation_id n) {
	return state.world.nation_get_private_investment(n) * 0.001f;
}

// Does this commodity has any factory using potentials mechanic
// Since in vanilla there are no such factories, it will return false.
bool get_commodity_uses_potentials(sys::state& state, dcon::commodity_id c) {
	for(auto type : state.world.in_factory_type) {
		auto output = type.get_output();
		if(output == c && output.get_uses_potentials()) {
			return true;
		}
	}
	return false;
}

float calculate_province_factory_limit(sys::state& state, dcon::province_id pid, dcon::commodity_id c) {
	return state.world.province_get_factory_max_size(pid, c);
}

float calculate_state_factory_limit(sys::state& state, dcon::state_instance_id sid, dcon::commodity_id c) {
	float result = 0.f;
	province::for_each_province_in_state_instance(state, sid, [&](dcon::province_id pid) {
		result += calculate_province_factory_limit(state, pid, c);
	});
	return result;
}

float calculate_nation_factory_limit(sys::state& state, dcon::nation_id nid, dcon::commodity_id c) {
	float res = 0;
	for(auto p : state.world.in_province) {
		if(p.get_nation_from_province_ownership() != nid) {
			continue;
		}

		if(p.get_factory_limit_was_set_during_scenario_creation()) {
			res += calculate_province_factory_limit(state, p, c);
		}
	}

	return res;
}

bool do_resource_potentials_allow_construction(sys::state& state, [[maybe_unused]] dcon::nation_id source, dcon::province_id location, dcon::factory_type_id type) {
	/* If mod uses Factory Province limits */
	auto output = state.world.factory_type_get_output(type);
	auto limit = economy::calculate_province_factory_limit(state, location, output);

	if(!output.get_uses_potentials()) {
		return true;
	}

	// Is there a potential for this commodity limit?
	if(limit <= 0) {
		return false;
	}

	return true;
}

bool do_resource_potentials_allow_upgrade(sys::state& state, [[maybe_unused]] dcon::nation_id source, dcon::province_id location, dcon::factory_type_id type) {
	/* If mod uses Factory Province limits */
	auto output = state.world.factory_type_get_output(type);

	if(!output.get_uses_potentials()) {
		return true;
	}

	auto limit = economy::calculate_province_factory_limit(state, location, output);

	// Will upgrade put us over the limit?
	auto existing_levels = 0.f;
	for(auto f : state.world.province_get_factory_location(location)) {
		if(f.get_factory().get_building_type() == type) {
			existing_levels += f.get_factory().get_size();
		}
		if(existing_levels + state.world.factory_type_get_base_workforce(type) > limit) {
			return false;
		}
	}

	return true;
}

void recreate_total_government_stockpiles(sys::state& state) {
	nations::parallel_for_each_existing_nation(state, [&](dcon::nation_id nation) {
		economy::for_each_commodity_no_money(state, [&](dcon::commodity_id commodity) {
			float total_stockpile = 0;
			state.world.nation_for_each_state_control(nation, [&](dcon::state_control_id sc) {
				dcon::state_instance_id state_instance = state.world.state_control_get_state(sc);
				auto market = state.world.state_instance_get_market_from_local_market(state_instance);
				total_stockpile += state.world.market_get_government_stockpile(market, commodity);
			});
			state.world.nation_set_total_stockpiles(nation, commodity, total_stockpile);
		});
	});
}

bool do_resource_potentials_allow_refit(sys::state& state, [[maybe_unused]] dcon::nation_id source, dcon::province_id location, dcon::factory_type_id from, dcon::factory_type_id refit_target) {
	/* If mod uses Factory Province limits */
	auto output = state.world.factory_type_get_output(from);
	auto limit = economy::calculate_province_factory_limit(state, location, output);

	if(!output.get_uses_potentials()) {
		return true;
	}

	auto refit_levels = 0.f;
	// How many levels changed factory has
	for(auto f : state.world.province_get_factory_location(location)) {
		if(f.get_factory().get_building_type() == from) {
			refit_levels = f.get_factory().get_size();
		}
	}
	// Will that put us over the limit?
	auto existing_levels = 0.f;
	for(auto f : state.world.province_get_factory_location(location)) {
		if(f.get_factory().get_building_type() == refit_target) {
			existing_levels += f.get_factory().get_size();
		}
		if(existing_levels + refit_levels > limit) {
			return false;
		}
	}

	return true;
}

} // namespace economy
