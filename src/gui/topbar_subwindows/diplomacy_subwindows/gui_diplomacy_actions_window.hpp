#pragma once

#include "gui_element_types.hpp"
#include "military.hpp"
#include "ai_campaign_values.hpp"
#include "ai_alliances.hpp"

namespace ui {

enum class diplomacy_action : uint8_t {
	ally,
	cancel_ally,
	call_ally,
	military_access,
	cancel_military_access,
	give_military_access,
	cancel_give_military_access,
	increase_relations,
	decrease_relations,
	war_subsidies,
	cancel_war_subsidies,
	declare_war,
	command_units,
	cancel_command_units,
	increase_opinion,
	add_to_sphere,
	remove_from_sphere,
	justify_war,
	make_peace,
	crisis_backdown,
	crisis_support,
	add_wargoal,
	state_transfer,
	crisis_add_wargoal
};

enum class gp_choice_actions {
	discredit,
	expel_advisors,
	ban_embassy,
	decrease_opinion
};

struct trigger_gp_choice {
	gp_choice_actions action = gp_choice_actions::discredit;
};

class diplomacy_action_btn_logic {
public:
	virtual dcon::text_key get_name(sys::state& state, dcon::nation_id target) {
		return state.lookup_key("null");
	}

	virtual bool is_available(sys::state& state, dcon::nation_id target) {
		return false;
	}

	virtual void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) {

	}

	virtual void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) {

	}
	virtual ~diplomacy_action_btn_logic() {
	};
};


class diplomacy_action_discredit_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("discredit_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(!state.world.nation_get_is_great_power(state.local_player_nation) || state.world.nation_get_is_great_power(target))
			return false;
		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		if(state.world.gp_relationship_get_influence(rel) < state.defines.discredit_influence_cost)
			return false;

		if((state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) != 0)
			return false;
		if(military::are_at_war(state, state.local_player_nation, target))
			return false;
		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		if(clevel == nations::influence::level_hostile)
			return false;

		return true;
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		send(state, parent, trigger_gp_choice{ gp_choice_actions::discredit });
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "discredit_desc", text::variable_type::days, int64_t(state.defines.discredit_days));
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(state.local_player_nation));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.discredit_influence_cost, text::variable_type::x, int64_t(state.defines.discredit_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "discredit_explain_2", clevel != nations::influence::level_hostile);
	}
};

class diplomacy_action_expel_advisors_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("expeladvisors_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(!state.world.nation_get_is_great_power(state.local_player_nation) || state.world.nation_get_is_great_power(target))
			return false;

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		if(state.world.gp_relationship_get_influence(rel) < state.defines.expeladvisors_influence_cost)
			return false;

		if((state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) != 0)
			return false;

		if(military::are_at_war(state, state.local_player_nation, target))
			return false;

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		if(clevel == nations::influence::level_hostile || clevel == nations::influence::level_opposed)
			return false;

		return true;
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		send(state, parent, trigger_gp_choice{ gp_choice_actions::expel_advisors });
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "expeladvisors_desc");
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(state.local_player_nation));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.expeladvisors_influence_cost, text::variable_type::x, int64_t(state.defines.expeladvisors_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "expel_explain_2", clevel != nations::influence::level_hostile && clevel != nations::influence::level_opposed);
	}
};

class diplomacy_action_ban_embassy_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("banembassy_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(!state.world.nation_get_is_great_power(state.local_player_nation) || state.world.nation_get_is_great_power(target))
			return false;

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		if(state.world.gp_relationship_get_influence(rel) < state.defines.banembassy_influence_cost)
			return false;

		if((state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) != 0)
			return false;

		if(military::are_at_war(state, state.local_player_nation, target))
			return false;

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		if(clevel != nations::influence::level_friendly && clevel != nations::influence::level_in_sphere)
			return false;

		return true;
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		send(state, parent, trigger_gp_choice{ gp_choice_actions::ban_embassy });
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "banembassy_desc", text::variable_type::days, int64_t(state.defines.banembassy_days));
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(state.local_player_nation));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.banembassy_influence_cost, text::variable_type::x, int64_t(state.defines.banembassy_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "ban_explain_2", clevel == nations::influence::level_friendly || clevel == nations::influence::level_in_sphere);


	}
};


class diplomacy_action_decrease_opinion_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("decreaseopinion_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(!state.world.nation_get_is_great_power(state.local_player_nation) || state.world.nation_get_is_great_power(target))
			return false;

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		if(state.world.gp_relationship_get_influence(rel) < state.defines.decreaseopinion_influence_cost)
			return false;

		if((state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) != 0)
			return false;

		if(military::are_at_war(state, state.local_player_nation, target))
			return false;

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		if(clevel == nations::influence::level_hostile)
			return false;

		return true;
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		send(state, parent, trigger_gp_choice{ gp_choice_actions::decrease_opinion });
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "decreaseopinion_desc");
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(state.local_player_nation));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.decreaseopinion_influence_cost, text::variable_type::x, int64_t(state.defines.decreaseopinion_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "dec_op_explain_3", clevel != nations::influence::level_hostile);
	}
};


class diplomacy_action_ally_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		if(nations::are_allied(state, target, state.local_player_nation)) {
			return state.lookup_key("cancelalliance_button");
		} else {
			return state.lookup_key("alliance_button");
		}
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(nations::are_allied(state, target, state.local_player_nation)) {
			return command::can_cancel_alliance(state, state.local_player_nation, target);
		} else {
			diplomatic_message::message m;
			m.type = diplomatic_message::type::alliance_request;
			m.from = state.local_player_nation;
			m.to = target;
			return command::can_ask_for_alliance(state, state.local_player_nation, target) && (state.world.nation_get_is_player_controlled(target) || diplomatic_message::ai_will_accept(state, m));
		}
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		if(nations::are_allied(state, target, state.local_player_nation)) {
			command::cancel_alliance(state, state.local_player_nation, target);
		} else {
			command::ask_for_alliance(state, state.local_player_nation, target);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		if(nations::are_allied(state, target, state.local_player_nation)) {
			text::add_line(state, contents, "cancelalliance_desc");
			text::add_line_break_to_layout(state, contents);

			if(state.local_player_nation == target) {
				text::add_line_with_condition(state, contents, "cancel_ally_explain_1", false);
			}
			if(state.defines.cancelalliance_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "cancel_ally_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.cancelalliance_diplomatic_cost, text::variable_type::x, int64_t(state.defines.cancelalliance_diplomatic_cost));
			}

			auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(target, state.local_player_nation);
			text::add_line_with_condition(state, contents, "cancel_ally_explain_3", state.world.diplomatic_relation_get_are_allied(rel));
			//text::add_line_with_condition(state, contents, "cancel_ally_explain_4", !military::are_allied_in_war(state, state.local_player_nation, content));

			auto ol = state.world.nation_get_overlord_as_subject(state.local_player_nation);
			text::add_line_with_condition(state, contents, "cancel_ally_explain_5", state.world.overlord_get_ruler(ol) != target);
		} else {
			auto asker = state.local_player_nation;

			text::add_line(state, contents, "alliance_desc");
			text::add_line_break_to_layout(state, contents);

			if(asker == target)
				text::add_line_with_condition(state, contents, "ally_explain_1", false);

			if(state.defines.alliance_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "ally_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.alliance_diplomatic_cost, text::variable_type::x, int64_t(state.defines.alliance_diplomatic_cost));
			}
			auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(target, state.local_player_nation);
			text::add_line_with_condition(state, contents, "ally_explain_3", state.world.diplomatic_relation_get_are_allied(rel) == false);
			text::add_line_with_condition(state, contents, "ally_explain_4", !state.world.nation_get_is_great_power(asker) || !state.world.nation_get_is_great_power(target) || state.current_crisis_state == sys::crisis_state::inactive);

			if(state.defines.alice_allow_subjects_declare_wars == 0) {
				auto ol = state.world.nation_get_overlord_as_subject(asker);
				text::add_line_with_condition(state, contents, "ally_explain_5", !state.world.overlord_get_ruler(ol));
				auto ol2 = state.world.nation_get_overlord_as_subject(target);
				text::add_line_with_condition(state, contents, "ally_explain_8", !state.world.overlord_get_ruler(ol2));
			}

			text::add_line_with_condition(state, contents, "ally_explain_6", !military::are_at_war(state, asker, target));

			if(!state.world.nation_get_is_player_controlled(target)) {
				diplomatic_message::message m;
				m.type = diplomatic_message::type::alliance_request;
				m.from = state.local_player_nation;
				m.to = target;

				text::add_line_with_condition(state, contents, "ally_explain_7", diplomatic_message::ai_will_accept(state, m));
				ai::explain_ai_alliance_reasons(state, target, contents, 15);
			}
		}
	}
};

class diplomacy_action_call_ally_button : public diplomacy_action_btn_logic {
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("callally_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		for(auto war_par : state.world.nation_get_war_participant(state.local_player_nation)) {
			if(command::can_call_to_arms(state, state.local_player_nation, target, war_par.get_war())) {
				if(!state.world.nation_get_is_player_controlled(target)) {
					diplomatic_message::message m;
					m.type = diplomatic_message::type::call_ally_request;
					m.from = state.local_player_nation;
					m.to = target;
					m.data.war = war_par.get_war();
					if(diplomatic_message::ai_will_accept(state, m)) {
						return true;
						break;
					}
				} else {
					return true;
					break;
				}
			}
		}

		return false;
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		auto asker = state.local_player_nation;

		for(auto war_par : state.world.nation_get_war_participant(asker)) {
			if(command::can_call_to_arms(state, state.local_player_nation, target, war_par.get_war())) {

				if(!state.world.nation_get_is_player_controlled(target)) {
					diplomatic_message::message m;
					m.type = diplomatic_message::type::call_ally_request;
					m.from = state.local_player_nation;
					m.to = target;
					m.data.war = war_par.get_war();
					if(diplomatic_message::ai_will_accept(state, m)) {
						command::call_to_arms(state, asker, target, war_par.get_war());
					}
				} else {
					command::call_to_arms(state, asker, target, war_par.get_war());
				}
			}
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto asker = state.local_player_nation;

		text::add_line(state, contents, "remove_callally_desc");
		text::add_line_break_to_layout(state, contents);

		if(state.defines.callally_diplomatic_cost > 0) {
			text::add_line_with_condition(state, contents, "call_ally_explain_2", state.world.nation_get_diplomatic_points(asker) >= state.defines.callally_diplomatic_cost, text::variable_type::x, int64_t(state.defines.callally_diplomatic_cost));
		}
		text::add_line_with_condition(state, contents, "call_ally_explain_1", nations::are_allied(state, asker, target));
		text::add_line_with_condition(state, contents, "call_ally_explain_3", state.world.nation_get_is_at_war(asker));

		bool possible_war = false;
		bool that_ai_will_accept = false;

		for(auto war_par : state.world.nation_get_war_participant(asker)) {
			if(!military::is_civil_war(state, war_par.get_war())
				&& military::standard_war_joining_is_possible(state, war_par.get_war(), target, military::is_attacker(state, war_par.get_war(), asker))
				&& (!war_par.get_war().get_is_crisis_war() || state.military_definitions.great_wars_enabled)) {

				possible_war = true;
				if(!state.world.nation_get_is_player_controlled(target)) {
					diplomatic_message::message m;
					m.type = diplomatic_message::type::call_ally_request;
					m.from = state.local_player_nation;
					m.to = target;
					m.data.war = war_par.get_war();
					if(diplomatic_message::ai_will_accept(state, m)) {
						that_ai_will_accept = true;
					}
				}
			}
		}
		text::add_line_with_condition(state, contents, "call_ally_explain_4", possible_war);
		if(!state.world.nation_get_is_player_controlled(target)) {
			text::add_line_with_condition(state, contents, "call_ally_explain_5", that_ai_will_accept);
		}

	}
};

class diplomacy_action_military_access_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			return state.lookup_key("cancelaskmilitaryaccess_button");
		} else {
			return state.lookup_key("askmilitaryaccess_button");
		}
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			return command::can_cancel_military_access(state, state.local_player_nation, target);
		} else {
			if(command::can_ask_for_access(state, state.local_player_nation, target)) {
				if(!state.world.nation_get_is_player_controlled(target)) {
					diplomatic_message::message m;
					m.type = diplomatic_message::type::access_request;
					m.from = state.local_player_nation;
					m.to = target;
					if(diplomatic_message::ai_will_accept(state, m)) {
						return true;
					} else {
						return false;
					}
				} else {
					return true;
				}
			} else {
				return false;
			}
		}
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			command::cancel_military_access(state, state.local_player_nation, target);
		} else {
			command::ask_for_military_access(state, state.local_player_nation, target);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			text::add_line(state, contents, "cancelaskmilitaryaccess_desc");
			text::add_line_break_to_layout(state, contents);

			if(state.defines.cancelaskmilaccess_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "cancel_access_explain_1", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.cancelaskmilaccess_diplomatic_cost, text::variable_type::x, int64_t(state.defines.cancelaskmilaccess_diplomatic_cost));
			}
		} else {
			text::add_line(state, contents, "askmilitaryaccess_desc");
			text::add_line_break_to_layout(state, contents);

			if(target == state.local_player_nation) {
				text::add_line_with_condition(state, contents, "ask_access_explain_1", false);
			}
			if(state.defines.askmilaccess_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "ask_access_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.askmilaccess_diplomatic_cost, text::variable_type::x, int64_t(state.defines.askmilaccess_diplomatic_cost));
			}
			if(!state.world.nation_get_is_player_controlled(target)) {
				diplomatic_message::message m;
				m.type = diplomatic_message::type::access_request;
				m.from = state.local_player_nation;
				m.to = target;
				text::add_line_with_condition(state, contents, "ask_access_explain_3", diplomatic_message::ai_will_accept(state, m));

				ai::explain_ai_access_reasons(state, target, contents, 15);
			}
			text::add_line_with_condition(state, contents, "ask_access_explain_4", !military::are_at_war(state, state.local_player_nation, target));
		}

	}
};

class diplomacy_action_give_military_access_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(state.local_player_nation, target);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			return state.lookup_key("cancelgivemilitaryaccess_button");
		} else {
			return state.lookup_key("givemilitaryaccess_button");
		}
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(state.local_player_nation, target);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			return command::can_cancel_given_military_access(state, state.local_player_nation, target);
		} else {
			return command::can_give_military_access(state, state.local_player_nation, target);
		}
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(state.local_player_nation, target);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			command::cancel_given_military_access(state, state.local_player_nation, target);
		} else {
			command::give_military_access(state, state.local_player_nation, target);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto urid = state.world.get_unilateral_relationship_by_unilateral_pair(state.local_player_nation, target);

		if(state.world.unilateral_relationship_get_military_access(urid)) {
			text::add_line(state, contents, "cancelgivemilitaryaccess_desc");
			text::add_line_break_to_layout(state, contents);

			if(state.defines.cancelgivemilaccess_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "cancel_given_access_explain_1", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.cancelgivemilaccess_diplomatic_cost, text::variable_type::x, int64_t(state.defines.cancelgivemilaccess_diplomatic_cost));
			}
		} else {
			text::add_line(state, contents, "givemilitaryaccess_desc");
			text::add_line_break_to_layout(state, contents);

			if(target == state.local_player_nation) {
				text::add_line_with_condition(state, contents, "give_access_explain_1", false);
			}
			if(state.defines.givemilaccess_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "give_access_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.givemilaccess_diplomatic_cost, text::variable_type::x, int64_t(state.defines.givemilaccess_diplomatic_cost));
			}
			text::add_line_with_condition(state, contents, "give_access_explain_3", !military::are_at_war(state, state.local_player_nation, target));
		}
	}
};

class diplomacy_action_increase_relations_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("increaserelation_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_increase_relations(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::increase_relations(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "increase_rel_explain_1", text::variable_type::x, int64_t(state.defines.increaserelation_relation_on_accept));
		text::add_line_break_to_layout(state, contents);

		if(target == state.local_player_nation) {
			text::add_line_with_condition(state, contents, "increase_rel_explain_2", false);
		}

		text::add_line_with_condition(state, contents, "increase_rel_explain_3", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.increaserelation_diplomatic_cost, text::variable_type::x, int64_t(state.defines.increaserelation_diplomatic_cost));

		auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(state.local_player_nation, target);
		text::add_line_with_condition(state, contents, "increase_rel_explain_4", state.world.diplomatic_relation_get_value(rel) < 200.0f);

		text::add_line_with_condition(state, contents, "increase_rel_explain_5", !military::are_at_war(state, state.local_player_nation, target));
	}
};

class diplomacy_action_decrease_relations_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("decreaserelation_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_increase_relations(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::decrease_relations(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "decrease_rel_explain_1", text::variable_type::x, int64_t(state.defines.decreaserelation_relation_on_accept));
		text::add_line_break_to_layout(state, contents);

		if(target == state.local_player_nation) {
			text::add_line_with_condition(state, contents, "decrease_rel_explain_2", false);
		}

		text::add_line_with_condition(state, contents, "decrease_rel_explain_3", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.decreaserelation_diplomatic_cost, text::variable_type::x, int64_t(state.defines.decreaserelation_diplomatic_cost));

		auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(state.local_player_nation, target);
		text::add_line_with_condition(state, contents, "decrease_rel_explain_4", state.world.diplomatic_relation_get_value(rel) > -200.0f);

		text::add_line_with_condition(state, contents, "decrease_rel_explain_5", !military::are_at_war(state, state.local_player_nation, target));

	}
};

class diplomacy_action_war_subisides_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		auto rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);
		bool subsidies = state.world.unilateral_relationship_get_war_subsidies(rel);

		if(subsidies) {
			return state.lookup_key("cancel_warsubsidies_button");
		} else {
			return state.lookup_key("warsubsidies_button");
		}
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		auto rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);
		bool subsidies = state.world.unilateral_relationship_get_war_subsidies(rel);

		if(subsidies) {
			return command::can_cancel_war_subsidies(state, state.local_player_nation, target);
		} else {
			return command::can_give_war_subsidies(state, state.local_player_nation, target);
		}
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		auto rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);
		bool subsidies = state.world.unilateral_relationship_get_war_subsidies(rel);
		if(subsidies) {
			command::cancel_war_subsidies(state, state.local_player_nation, target);
		} else {
			command::give_war_subsidies(state, state.local_player_nation, target);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);
		bool subsidies = state.world.unilateral_relationship_get_war_subsidies(rel);

		auto lpn_fat = dcon::fatten(state.world, state.local_player_nation);
		auto target_fat = dcon::fatten(state.world, target);
		auto subsize = economy::estimate_war_subsidies(state, target_fat, lpn_fat);

		if(subsidies) {
			text::add_line(state, contents, "cancel_w_sub_explain_1", text::variable_type::x, text::fp_currency{ subsize });
			if(state.defines.cancelwarsubsidy_diplomatic_cost > 0) {
				text::add_line_break_to_layout(state, contents);
				text::add_line_with_condition(state, contents, "cancel_w_sub_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.cancelwarsubsidy_diplomatic_cost, text::variable_type::x, int16_t(state.defines.cancelwarsubsidy_diplomatic_cost));
			}
		} else {
			text::add_line(state, contents, "warsubsidies_desc", text::variable_type::money, text::fp_one_place{ subsize });
			text::add_line_break_to_layout(state, contents);

			if(state.local_player_nation == target) {
				text::add_line_with_condition(state, contents, "w_sub_explain_1", false);
			}
			if(state.defines.warsubsidy_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "w_sub_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.warsubsidy_diplomatic_cost, text::variable_type::x, int16_t(state.defines.warsubsidy_diplomatic_cost));
			}
			text::add_line_with_condition(state, contents, "w_sub_explain_3", !military::are_at_war(state, state.local_player_nation, target));
			// text::add_line_with_condition(state, contents, "w_sub_explain_4", state.world.nation_get_is_at_war(target));
		}
	}
};

class diplomacy_action_declare_war_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		if(military::are_at_war(state, state.local_player_nation, target)) {
			return state.lookup_key("peace_button");
		} else {
			return state.lookup_key("war_button");
		}
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		if(military::are_at_war(state, state.local_player_nation, target)) {
			return command::can_start_peace_offer(state, state.local_player_nation, target,
					military::find_war_between(state, state.local_player_nation, target), true);
		} else {
			return !(state.local_player_nation == target || !military::can_use_cb_against(state, state.local_player_nation, target) || state.world.nation_get_diplomatic_points(state.local_player_nation) < state.defines.declarewar_diplomatic_cost || military::are_in_common_war(state, state.local_player_nation, target));
		}
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		if(parent) {
			dcon::nation_id content = retrieve<dcon::nation_id>(state, parent);
			Cyto::Any ac_payload = military::are_at_war(state, state.local_player_nation, content) ? diplomacy_action::make_peace : diplomacy_action::declare_war;
			parent->impl_get(state, ac_payload);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;

		if(military::are_at_war(state, state.local_player_nation, target)) {
			text::add_line(state, contents, "peace_description");
			text::add_line_break_to_layout(state, contents);
			{
				auto ol = state.world.nation_get_overlord_as_subject(source);
				text::add_line_with_condition(state, contents, "peace_explain_1", !state.world.overlord_get_ruler(ol));
			}

			{
				auto ol = state.world.nation_get_overlord_as_subject(target);
				text::add_line_with_condition(state, contents, "peace_explain_2", !state.world.overlord_get_ruler(ol));
			}

			auto war = military::find_war_between(state, source, target);

			if(state.world.war_get_is_crisis_war(war)) {
				bool both_wl = (state.world.war_get_primary_attacker(war) == source && state.world.war_get_primary_defender(war) == target) || (state.world.war_get_primary_attacker(war) == target || state.world.war_get_primary_defender(war) == source);
				text::add_line_with_condition(state, contents, "peace_explain_4", both_wl);
			} else {
				bool one_is_wl = [&]() {
					if(state.world.war_get_primary_attacker(war) == source) {
						if(military::get_role(state, war, target) == military::war_role::defender)
							return true;
					} else if(state.world.war_get_primary_defender(war) == source) {
						if(military::get_role(state, war, target) == military::war_role::attacker)
							return true;
					} else if(state.world.war_get_primary_attacker(war) == target) {
						if(military::get_role(state, war, source) == military::war_role::defender)
							return true;
					} else if(state.world.war_get_primary_defender(war) == target) {
						if(military::get_role(state, war, source) == military::war_role::attacker)
							return true;
					}
					return false;
					}();
				text::add_line_with_condition(state, contents, "peace_explain_3", one_is_wl);
			}
			text::add_line_with_condition(state, contents, "peace_explain_5", !(state.world.nation_get_peace_offer_from_pending_peace_offer(source)));

		} else {
			text::add_line(state, contents, "act_wardesc");
			text::add_line_break_to_layout(state, contents);

			if(source == target) {
				text::add_line_with_condition(state, contents, "war_explain_1", false);
			}
			if(state.defines.declarewar_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "war_explain_3", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.declarewar_diplomatic_cost, text::variable_type::x, int64_t(state.defines.declarewar_diplomatic_cost));
			}
			text::add_line_with_condition(state, contents, "war_explain_2", military::can_use_cb_against(state, state.local_player_nation, target));
			text::add_line_with_condition(state, contents, "war_explain_4", !military::are_in_common_war(state, state.local_player_nation, target));
		}

	}
};

class diplomacy_action_release_subject_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("alice_diplo_release_subject");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_release_subject(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::release_subject(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		text::add_line(state, contents, "alice_diplo_release_subject_desc");
		text::add_line_with_condition(state, contents, "alice_diplo_release_subject_0", state.world.overlord_get_ruler(state.world.nation_get_overlord_as_subject(target)) == state.local_player_nation);
	}
};

class diplomacy_action_increase_opinion_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("increaseopinion_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_increase_opinion(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::increase_opinion(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;

		text::add_line(state, contents, "increaseopinion_desc");
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(source));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, source);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.increaseopinion_influence_cost, text::variable_type::x, int64_t(state.defines.increaseopinion_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, source, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "inc_op_explain_1", clevel != nations::influence::level_friendly && clevel != nations::influence::level_in_sphere);

	}
};

class diplomacy_action_add_to_sphere_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("addtosphere_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_add_to_sphere(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::add_to_sphere(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;

		text::add_line(state, contents, "addtosphere_desc");
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(source));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, source);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.addtosphere_influence_cost, text::variable_type::x, int64_t(state.defines.addtosphere_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, source, target));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		text::add_line_with_condition(state, contents, "add_sphere_explain_1", clevel == nations::influence::level_friendly);

		text::add_line_with_condition(state, contents, "add_sphere_explain_2", !state.world.nation_get_in_sphere_of(target));

	}
};

class diplomacy_action_remove_from_sphere_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("removefromsphere_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_remove_from_sphere(state, state.local_player_nation, target, state.world.nation_get_in_sphere_of(target));
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::remove_from_sphere(state, state.local_player_nation, target, state.world.nation_get_in_sphere_of(target));
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;
		bool in_players_sphere = state.world.nation_get_in_sphere_of(target) == source;

		text::add_line(state, contents, "removefromsphere_desc");
		if(in_players_sphere) {
			if(state.defines.removefromsphere_infamy_cost != 0) {
				text::add_line(state, contents, "rem_sphere_explain_4", text::variable_type::x, text::fp_one_place{ state.defines.removefromsphere_infamy_cost });
			}
			if(state.defines.removefromsphere_prestige_cost != 0) {
				text::add_line(state, contents, "rem_sphere_explain_5", text::variable_type::x, text::fp_one_place{ state.defines.removefromsphere_prestige_cost });
			}
		}
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "iaction_explain_5", state.world.nation_get_is_great_power(state.local_player_nation));

		text::add_line_with_condition(state, contents, "iaction_explain_6", !state.world.nation_get_is_great_power(target));

		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);
		text::add_line_with_condition(state, contents, "iaction_explain_1", state.world.gp_relationship_get_influence(rel) >= state.defines.removefromsphere_influence_cost, text::variable_type::x, int64_t(state.defines.removefromsphere_influence_cost));

		text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
		text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

		text::add_line_with_condition(state, contents, "rem_sphere_explain_1", bool(state.world.nation_get_in_sphere_of(target)));

		auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
		if(!in_players_sphere) {
			text::add_line_with_condition(state, contents, "rem_sphere_explain_2", clevel == nations::influence::level_friendly);
		} else {
			text::add_line_with_condition(state, contents, "rem_sphere_explain_3", true);
		}

	}
};

class diplomacy_action_ask_free_trade_agreement : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		if(economy::nation_gives_free_trade_rights(state, state.local_player_nation, target)) {
			return state.lookup_key("revoke_trade_rights");
		}
		
		return state.lookup_key("ask_free_trade_agreement");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		// We are obliged to them
		if(economy::nation_gives_free_trade_rights(state, target, state.local_player_nation)) {
			return command::can_revoke_trade_rights(state, state.local_player_nation, target);
		}
		return command::can_ask_for_free_trade_agreement(state, state.local_player_nation, target);
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		// We are obliged to them
		if(economy::nation_gives_free_trade_rights(state, target, state.local_player_nation)) {
			command::revoke_trade_rights(state, state.local_player_nation, target);
			return;
		}
		command::ask_for_free_trade_agreement(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto asker = state.local_player_nation;

		if(economy::nation_gives_free_trade_rights(state, state.local_player_nation, target)) {
			text::add_line(state, contents, "trade_rights_desc", text::variable_type::x, text::fp_one_place{ state.defines.alice_free_trade_agreement_years });
			text::add_line_break_to_layout(state, contents);

			// Rel source if obliged towards target
			auto we_have_free_trade = economy::nation_gives_free_trade_rights(state, asker, target);
			auto they_have_free_trade = economy::nation_gives_free_trade_rights(state, target, asker);
			if(we_have_free_trade) {
				auto enddt1 = state.world.unilateral_relationship_get_no_tariffs_until(we_have_free_trade);
				if(state.current_date < enddt1) {
					text::add_line(state, contents, "free_trade_explain_1", text::variable_type::date, enddt1);
				}
			}
			if(they_have_free_trade) {
				auto enddt2 = state.world.unilateral_relationship_get_no_tariffs_until(they_have_free_trade);
				if(state.current_date < enddt2) {
					text::add_line(state, contents, "free_trade_explain_2", text::variable_type::date, enddt2);
				}
			}

			text::add_line_break_to_layout(state, contents);

			text::add_line_with_condition(state, contents, "ally_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.alliance_diplomatic_cost, text::variable_type::x, int64_t(state.defines.alliance_diplomatic_cost));

			auto ol = state.world.nation_get_overlord_as_subject(asker);
			text::add_line_with_condition(state, contents, "ally_explain_5", !state.world.overlord_get_ruler(ol));
			auto ol2 = state.world.nation_get_overlord_as_subject(target);
			text::add_line_with_condition(state, contents, "ally_explain_8", !state.world.overlord_get_ruler(ol2));

			auto rights = economy::nation_gives_free_trade_rights(state, asker, target);
			auto enddt = state.world.unilateral_relationship_get_no_tariffs_until(rights);

			text::add_line_with_condition(state, contents, "revoke_trade_rights_explain_1", state.current_date > enddt);
		}
		else {

			text::add_line(state, contents, "free_trade_desc", text::variable_type::x, text::fp_one_place{ state.defines.alice_free_trade_agreement_years });
			text::add_line_break_to_layout(state, contents);

			// Rel source if obliged towards target
			auto we_have_free_trade = economy::nation_gives_free_trade_rights(state, asker, target);
			auto they_have_free_trade = economy::nation_gives_free_trade_rights(state, target, asker);
			if(we_have_free_trade) {
				auto enddt1 = state.world.unilateral_relationship_get_no_tariffs_until(we_have_free_trade);
				if(state.current_date < enddt1) {
					text::add_line(state, contents, "free_trade_explain_1", text::variable_type::date, enddt1);
				}
			}
			if(they_have_free_trade) {
				auto enddt2 = state.world.unilateral_relationship_get_no_tariffs_until(they_have_free_trade);
				if(state.current_date < enddt2) {
					text::add_line(state, contents, "free_trade_explain_2", text::variable_type::date, enddt2);
				}
			}

			text::add_line_break_to_layout(state, contents);

			if(asker == target)
				text::add_line_with_condition(state, contents, "ally_explain_1", false);

			if(state.defines.alliance_diplomatic_cost > 0) {
				text::add_line_with_condition(state, contents, "ally_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.alliance_diplomatic_cost, text::variable_type::x, int64_t(state.defines.alliance_diplomatic_cost));
			}
			auto rel = state.world.get_diplomatic_relation_by_diplomatic_pair(target, state.local_player_nation);
			text::add_line_with_condition(state, contents, "ally_explain_4", !state.world.nation_get_is_great_power(asker) || !state.world.nation_get_is_great_power(target) || state.current_crisis_state == sys::crisis_state::inactive);

			auto ol = state.world.nation_get_overlord_as_subject(asker);
			text::add_line_with_condition(state, contents, "ally_explain_5", !state.world.overlord_get_ruler(ol));
			auto ol2 = state.world.nation_get_overlord_as_subject(target);
			text::add_line_with_condition(state, contents, "ally_explain_8", !state.world.overlord_get_ruler(ol2));

			text::add_line_with_condition(state, contents, "ally_explain_6", !military::are_at_war(state, asker, target));

			// Can't free trade if embargo is imposed
			auto we_embargo_them = false;
			auto they_embargo_us = false;
			auto source_tariffs_rel = state.world.get_unilateral_relationship_by_unilateral_pair(target, asker);
			auto target_tariffs_rel = state.world.get_unilateral_relationship_by_unilateral_pair(asker, target);
			if(source_tariffs_rel) {
				we_embargo_them = true;
			}
			if(target_tariffs_rel) {
				they_embargo_us = true;
			}
			text::add_line_with_condition(state, contents, "free_trade_explain_3", !we_embargo_them);
			text::add_line_with_condition(state, contents, "free_trade_explain_4", !they_embargo_us);

			if(!state.world.nation_get_is_player_controlled(target)) {
				diplomatic_message::message m;
				m.type = diplomatic_message::type::alliance_request;
				m.from = state.local_player_nation;
				m.to = target;

				text::add_line_with_condition(state, contents, "ally_explain_7", diplomatic_message::ai_will_accept(state, m));
				ai::explain_ai_trade_agreement_reasons(state, target, contents, 15);
			}
		}
	}
};

class diplomacy_action_embargo : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		auto rel_1 = state.world.get_unilateral_relationship_by_unilateral_pair(target, state.local_player_nation);
		if(rel_1) {
			if(state.world.unilateral_relationship_get_embargo(rel_1)) {
				return state.lookup_key("lift_embargo");
			}
		}

		return state.lookup_key("issue_embargo");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		return command::can_switch_embargo_status(state, state.local_player_nation, target);
	}


	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		command::switch_embargo_status(state, state.local_player_nation, target);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto asker = state.local_player_nation;

		text::add_line(state, contents, "embargo_desc", text::variable_type::x, text::fp_one_place{ state.defines.alice_free_trade_agreement_years });
		text::add_line_break_to_layout(state, contents);

		// Show existing embargoes
		// We embargo them
		if(economy::has_active_embargo(state, asker, target)) {
			text::add_line(state, contents, "embargo_explain_1", text::variable_type::x, asker, text::variable_type::y, target);
		}
		// They embargo us
		if(economy::has_active_embargo(state, target, asker)) {
				text::add_line(state, contents, "embargo_explain_2", text::variable_type::x, target, text::variable_type::y, asker);
		}

		text::add_line_break_to_layout(state, contents);

		if(state.defines.alliance_diplomatic_cost > 0) {
			text::add_line_with_condition(state, contents, "ally_explain_2", state.world.nation_get_diplomatic_points(state.local_player_nation) >= state.defines.alliance_diplomatic_cost, text::variable_type::x, int64_t(state.defines.alliance_diplomatic_cost));
		}
		// Subjects can embargo or be embargoed
		auto ol = state.world.nation_get_overlord_as_subject(asker);
		text::add_line_with_condition(state, contents, "ally_explain_5", !state.world.overlord_get_ruler(ol));
		auto ol2 = state.world.nation_get_overlord_as_subject(target);
		text::add_line_with_condition(state, contents, "ally_explain_8", !state.world.overlord_get_ruler(ol2));

		auto sl = state.world.nation_get_in_sphere_of(asker);
		auto sl2 = state.world.nation_get_in_sphere_of(asker);
		text::add_line_with_condition(state, contents, "embargo_explain_5", !sl);
		text::add_line_with_condition(state, contents, "embargo_explain_6", !sl2);

		// Can't embargo if free trade is in place
		auto we_have_free_trade = state.world.get_unilateral_relationship_by_unilateral_pair(asker, target);
		auto they_have_free_trade = state.world.get_unilateral_relationship_by_unilateral_pair(target, asker);
		if(we_have_free_trade) {
			auto enddt1 = state.world.unilateral_relationship_get_no_tariffs_until(we_have_free_trade);
			text::add_line_with_condition(state, contents, "embargo_explain_3", state.current_date >= enddt1);
		}
		if(they_have_free_trade) {
			auto enddt2 = state.world.unilateral_relationship_get_no_tariffs_until(they_have_free_trade);
			text::add_line_with_condition(state, contents, "embargo_explain_4", state.current_date >= enddt2);
		}

		text::add_line_break_to_layout(state, contents);
	}
};

class diplomacy_action_justify_war_button : public diplomacy_action_btn_logic {
	bool has_any_justifiable_cb(sys::state& state, dcon::nation_id source, dcon::nation_id target) noexcept {
		auto res = false;
		state.world.for_each_cb_type([&](dcon::cb_type_id cb) {
			if(command::can_fabricate_cb(state, source, target, cb))
				res = true;
		});

		return res;
	}
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("make_cb_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;

		auto can_subjects_declare = state.defines.alice_allow_subjects_declare_wars;

		if(source == target)
			return false;
		if(state.world.nation_get_constructing_cb_type(source))
			return false;
		auto ol = state.world.nation_get_overlord_as_subject(source);
		if(state.defines.alice_allow_subjects_declare_wars < 1 && state.world.overlord_get_ruler(ol) && state.world.overlord_get_ruler(ol) != target)
			return false;
		if(state.world.nation_get_in_sphere_of(target) == source)
			return false;
		if(state.world.nation_get_diplomatic_points(source) < state.defines.make_cb_diplomatic_cost)
			return false;
		if(military::are_at_war(state, target, source))
			return false;
		if(!has_any_justifiable_cb(state, source, target))
			return false;

		return true;
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {
		if(parent) {
			Cyto::Any payload = diplomacy_action::justify_war;
			parent->impl_get(state, payload);
		}
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;
		text::add_line(state, contents, "make_cb_desc");
		text::add_line_break_to_layout(state, contents);
		if(source == target) {
			text::add_line_with_condition(state, contents, "fab_explain_1", false);
		}
		if(state.defines.make_cb_diplomatic_cost > 0) {
			text::add_line_with_condition(state, contents, "fab_explain_2", state.world.nation_get_diplomatic_points(source) >= state.defines.make_cb_diplomatic_cost, text::variable_type::x, int16_t(state.defines.make_cb_diplomatic_cost));
		}
		text::add_line_with_condition(state, contents, "fab_explain_3", !state.world.nation_get_constructing_cb_type(source));
		if(state.defines.alice_allow_subjects_declare_wars < 1) {
			auto ol = state.world.nation_get_overlord_as_subject(source);
			text::add_line_with_condition(state, contents, "fab_explain_4", !state.world.overlord_get_ruler(ol) || state.world.overlord_get_ruler(ol) == target);
		}
		text::add_line_with_condition(state, contents, "fab_explain_5", state.world.nation_get_in_sphere_of(target) != source);
		text::add_line_with_condition(state, contents, "fab_explain_6", !military::are_at_war(state, target, source));
		text::add_line_with_condition(state, contents, "fab_explain_7", has_any_justifiable_cb(state, source, target));
		text::add_line_with_condition(state, contents, "fab_explain_8", !military::has_truce_with(state, target, source));
	}
};

class diplomacy_action_dialog_title_text : public generic_settable_element<simple_text_element_base, diplomacy_action> {
	static std::string_view get_title_key(diplomacy_action v) noexcept {
		switch(v) {
		case diplomacy_action::ally:
			return "alliancetitle";
		case diplomacy_action::cancel_ally:
			return "cancelalliancetitle";
		case diplomacy_action::call_ally:
			return "callallytitle";
		case diplomacy_action::declare_war:
			return "wartitle";
		case diplomacy_action::military_access:
			return "askmilitaryaccesstitle";
		case diplomacy_action::cancel_military_access:
			return "cancelaskmilitaryaccesstitle";
		case diplomacy_action::give_military_access:
			return "givemilitaryaccesstitle";
		case diplomacy_action::cancel_give_military_access:
			return "cancelgivemilitaryaccesstitle";
		case diplomacy_action::increase_relations:
			return "increaserelationtitle";
		case diplomacy_action::decrease_relations:
			return "decreaserelationtitle";
		case diplomacy_action::war_subsidies:
			return "warsubsidiestitle";
		case diplomacy_action::cancel_war_subsidies:
			return "cancel_warsubsidiestitle";
		case diplomacy_action::increase_opinion:
			return "increaseopiniontitle";
		case diplomacy_action::add_to_sphere:
			return "addtospheretitle";
		case diplomacy_action::remove_from_sphere:
			return "removefromspheretitle";
		case diplomacy_action::justify_war:
			return "make_cbtitle";
		case diplomacy_action::command_units:
			return "give_unit_commandtitle";
		case diplomacy_action::cancel_command_units:
			return "cancel_unit_commandtitle";
		case diplomacy_action::make_peace:
			return "make_peacetitle";
		default:
			return "";
		}
	}

public:
	void on_update(sys::state& state) noexcept override {
		set_text(state, text::produce_simple_string(state, get_title_key(content)));
	}
};
class diplomacy_action_dialog_description_text : public generic_settable_element<multiline_text_element_base, diplomacy_action> {
	static std::string_view get_title_key(diplomacy_action v) noexcept {
		switch(v) {
		case diplomacy_action::ally:
			return "alliancenewdesc";
		case diplomacy_action::cancel_ally:
			return "breakalliancenewdesc";
		case diplomacy_action::call_ally:
			return "callally_desc";
		case diplomacy_action::declare_war:
			return "war_desc";
		case diplomacy_action::military_access:
			return "askmilitaryaccess_desc";
		case diplomacy_action::cancel_military_access:
			return "cancelaskmilitaryaccess_desc";
		case diplomacy_action::give_military_access:
			return "givemilitaryaccess_desc";
		case diplomacy_action::cancel_give_military_access:
			return "cancelgivemilitaryaccess_desc";
		case diplomacy_action::increase_relations:
			return "increaserelation_desc";
		case diplomacy_action::decrease_relations:
			return "decreaserelation_desc";
		case diplomacy_action::war_subsidies:
			return "warsubsidies_desc";
		case diplomacy_action::cancel_war_subsidies:
			return "cancel_warsubsidies_desc";
		case diplomacy_action::increase_opinion:
			return "increaseopinion_desc";
		case diplomacy_action::add_to_sphere:
			return "addtosphere_desc";
		case diplomacy_action::remove_from_sphere:
			return "removefromsphere_desc";
		case diplomacy_action::justify_war:
			return "make_cb_desc";
		case diplomacy_action::command_units:
			return "give_unit_command_desc";
		case diplomacy_action::cancel_command_units:
			return "cancel_unit_command_desc";
		case diplomacy_action::make_peace:
			return "make_peace_desc";
		default:
			return "";
		}
	}

public:
	void on_update(sys::state& state) noexcept override {
		auto contents = text::create_endless_layout(state, internal_layout,
				text::layout_parameters{ 0, 0, static_cast<int16_t>(base_data.size.x), static_cast<int16_t>(base_data.size.y),
						base_data.data.text.font_handle, 0, text::alignment::left, text::text_color::white, false });
		auto box = text::open_layout_box(contents);
		text::localised_format_box(state, contents, box, get_title_key(content));
		text::close_layout_box(contents, box);
	}
};

class diplomacy_action_dialog_agree_button : public generic_settable_element<button_element_base, diplomacy_action> {
	bool get_can_perform(sys::state& state) noexcept {
		auto target = retrieve<dcon::nation_id>(state, parent);
		switch(content) {
		case diplomacy_action::crisis_add_wargoal:
			return true;
		case diplomacy_action::ally:
			return command::can_ask_for_alliance(state, state.local_player_nation, target);
		case diplomacy_action::cancel_ally:
			return command::can_cancel_alliance(state, state.local_player_nation, target);
		case diplomacy_action::call_ally:
		{
			for(auto wp : dcon::fatten(state.world, state.local_player_nation).get_war_participant())
				if(command::can_call_to_arms(state, state.local_player_nation, target, dcon::fatten(state.world, wp).get_war().id))
					return true;
			return false;
		}
		case diplomacy_action::declare_war:
			return false;
		case diplomacy_action::military_access:
			return command::can_ask_for_access(state, state.local_player_nation, target);
		case diplomacy_action::cancel_military_access:
			return command::can_cancel_military_access(state, state.local_player_nation, target);
		case diplomacy_action::give_military_access:
			return false;
		case diplomacy_action::cancel_give_military_access:
			return command::can_cancel_given_military_access(state, state.local_player_nation, target);
		case diplomacy_action::increase_relations:
			return command::can_increase_relations(state, state.local_player_nation, target);
		case diplomacy_action::decrease_relations:
			return command::can_decrease_relations(state, state.local_player_nation, target);
		case diplomacy_action::war_subsidies:
			return command::can_give_war_subsidies(state, state.local_player_nation, target);
		case diplomacy_action::cancel_war_subsidies:
			return command::can_cancel_war_subsidies(state, state.local_player_nation, target);
		case diplomacy_action::increase_opinion:
			return command::can_increase_opinion(state, state.local_player_nation, target);
		case diplomacy_action::add_to_sphere:
			return command::can_add_to_sphere(state, state.local_player_nation, target);
		case diplomacy_action::remove_from_sphere:
			return command::can_remove_from_sphere(state, state.local_player_nation, target, state.world.nation_get_in_sphere_of(target));
		case diplomacy_action::justify_war:
			return false;
		case diplomacy_action::command_units:
			return false;
		case diplomacy_action::cancel_command_units:
			return false;
		case diplomacy_action::make_peace:
			return false;
		case diplomacy_action::crisis_backdown:
			return false;
		case diplomacy_action::crisis_support:
			return false;
		case diplomacy_action::add_wargoal:
			return false;
		case diplomacy_action::state_transfer:
			return false;
		}
		return false;
	}

public:
	void on_create(sys::state& state) noexcept override {
		base_data.data.button.txt = state.lookup_key("agree");
		button_element_base::on_create(state);
	}

	void on_update(sys::state& state) noexcept override {
		disabled = !get_can_perform(state);
	}

	void button_action(sys::state& state) noexcept override {
		auto target = retrieve<dcon::nation_id>(state, parent);
		switch(content) {
		case diplomacy_action::crisis_add_wargoal:
			break;
		case diplomacy_action::ally:
			command::ask_for_alliance(state, state.local_player_nation, target);
			break;
		case diplomacy_action::cancel_ally:
			command::cancel_alliance(state, state.local_player_nation, target);
			break;
		case diplomacy_action::call_ally:
			for(auto wp : dcon::fatten(state.world, state.local_player_nation).get_war_participant())
				command::call_to_arms(state, state.local_player_nation, target, dcon::fatten(state.world, wp).get_war().id);
			break;
		case diplomacy_action::declare_war:
			break;
		case diplomacy_action::military_access:
			command::ask_for_military_access(state, state.local_player_nation, target);
			break;
		case diplomacy_action::cancel_military_access:
			command::cancel_military_access(state, state.local_player_nation, target);
			break;
		case diplomacy_action::give_military_access:
			break;
		case diplomacy_action::cancel_give_military_access:
			command::cancel_given_military_access(state, state.local_player_nation, target);
			break;
		case diplomacy_action::increase_relations:
			command::increase_relations(state, state.local_player_nation, target);
			break;
		case diplomacy_action::decrease_relations:
			command::decrease_relations(state, state.local_player_nation, target);
			break;
		case diplomacy_action::war_subsidies:
			command::give_war_subsidies(state, state.local_player_nation, target);
			break;
		case diplomacy_action::cancel_war_subsidies:
			command::cancel_war_subsidies(state, state.local_player_nation, target);
			break;
		case diplomacy_action::increase_opinion:
			command::increase_opinion(state, state.local_player_nation, target);
			break;
		case diplomacy_action::add_to_sphere:
			command::add_to_sphere(state, state.local_player_nation, target);
			break;
		case diplomacy_action::remove_from_sphere:
			command::remove_from_sphere(state, state.local_player_nation, target, state.world.nation_get_in_sphere_of(target));
			break;
		case diplomacy_action::justify_war:
			break;
		case diplomacy_action::command_units:
			break;
		case diplomacy_action::cancel_command_units:
			break;
		case diplomacy_action::make_peace:
			break;
		case diplomacy_action::crisis_backdown:
			break;
		case diplomacy_action::crisis_support:
			break;
		case diplomacy_action::add_wargoal:
			break;
		case diplomacy_action::state_transfer:
			break;
		}
		if(parent) {
			parent->set_visible(state, false);
		}
	}
};
class diplomacy_action_dialog_decline_button : public generic_close_button {
public:
	void on_create(sys::state& state) noexcept override {
		base_data.data.button.txt = state.lookup_key("decline");
		button_element_base::on_create(state);
	}
};

class diplomacy_action_dialog_window : public window_element_base {
public:
	std::unique_ptr<element_base> make_child(sys::state& state, std::string_view name, dcon::gui_def_id id) noexcept override {
		if(name == "title") {
			return make_element_by_type<diplomacy_action_dialog_title_text>(state, id);
		} else if(name == "description") {
			return make_element_by_type<diplomacy_action_dialog_description_text>(state, id);
		} else if(name == "chance_string") {
			return make_element_by_type<simple_text_element_base>(state, id);
		} else if(name == "agreebutton") {
			return make_element_by_type<diplomacy_action_dialog_agree_button>(state, id);
		} else if(name == "declinebutton") {
			return make_element_by_type<diplomacy_action_dialog_decline_button>(state, id);
		} else if(name == "leftshield") {
			return make_element_by_type<nation_player_flag>(state, id);
		} else if(name == "rightshield") {
			return make_element_by_type<flag_button>(state, id);
		} else if(name == "background") {
			auto ptr = make_element_by_type<draggable_target>(state, id);
			ptr->base_data.size = base_data.size;
			return ptr;
		} else {
			return nullptr;
		}
	}
};


struct gp_choice_num {
	int32_t value = 0;
};

struct gp_choice_select {
	int32_t value = 0;
};

struct gp_choice_get_selection {
	int32_t value = 0;
};

struct select_gp_choice_button : public flag_button {
public:
	dcon::national_identity_id get_current_nation(sys::state& state) noexcept override {
		auto n = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		return state.world.nation_get_identity_from_identity_holder(n);
	}
	void button_action(sys::state& state) noexcept override {
		auto this_selection = retrieve<gp_choice_num>(state, parent);
		send(state, parent, gp_choice_select{ this_selection.value });
	}
};

class select_gp_name : public simple_text_element_base {
public:
	void on_update(sys::state& state) noexcept override {
		auto n = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		set_text(state, text::produce_simple_string(state, text::get_name(state, n)));
	}
};

struct select_gp_selection_icon : public image_element_base {
public:
	bool show = false;
	void on_update(sys::state& state) noexcept override {
		show = retrieve<gp_choice_num>(state, parent).value == retrieve<gp_choice_get_selection>(state, parent).value;
	}
	void render(sys::state& state, int32_t x, int32_t y) noexcept override {
		if(show) {
			image_element_base::render(state, x, y);
		}
	}
};

class select_gp_banned : public image_element_base {
public:
	bool show = false;
	void on_update(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);
		show = (state.world.gp_relationship_get_status(state.world.get_gp_relationship_by_gp_influence_pair(target, gp)) & nations::influence::is_banned) != 0;
	}
	tooltip_behavior has_tooltip(sys::state& state) noexcept override {
		return show ? tooltip_behavior::variable_tooltip : tooltip_behavior::no_tooltip;
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);
		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, gp);

		text::add_line(state, contents, "dp_inf_tooltip_ban", text::variable_type::x, state.world.gp_relationship_get_penalty_expires_date(rel));
	}
	void render(sys::state& state, int32_t x, int32_t y) noexcept override {
		if(show)
			image_element_base::render(state, x, y);
	}
};

class select_gp_discredited : public image_element_base {
public:
	bool show = false;
	void on_update(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);
		show = (state.world.gp_relationship_get_status(state.world.get_gp_relationship_by_gp_influence_pair(target, gp)) & nations::influence::is_discredited) != 0;
	}
	tooltip_behavior has_tooltip(sys::state& state) noexcept override {
		return show ? tooltip_behavior::variable_tooltip : tooltip_behavior::no_tooltip;
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);
		auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, gp);

		text::add_line(state, contents, "dp_inf_tooltip_discredit", text::variable_type::x, state.world.gp_relationship_get_penalty_expires_date(rel));
	}
	void render(sys::state& state, int32_t x, int32_t y) noexcept override {
		if(show)
			image_element_base::render(state, x, y);
	}
};

class select_gp_opinion_detail : public simple_text_element_base {
public:
	void on_update(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);

		set_text(state, text::get_influence_level_name(state, state.world.gp_relationship_get_status(state.world.get_gp_relationship_by_gp_influence_pair(target, gp))));
	}
};
class select_gp_influence_detail : public simple_text_element_base {
public:
	void on_update(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_num>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);

		set_text(state, text::format_float(state.world.gp_relationship_get_influence(state.world.get_gp_relationship_by_gp_influence_pair(target, gp)), 1));
	}
};

class diplomacy_action_gp_dialog_select_window : public window_element_base {
public:
	uint8_t rank = 0;

	std::unique_ptr<element_base> make_child(sys::state& state, std::string_view name, dcon::gui_def_id id) noexcept override {
		if(name == "country_name") {
			return make_element_by_type<select_gp_name>(state, id);
		} else if(name == "country_flag") {
			return make_element_by_type<select_gp_choice_button>(state, id);
		} else if(name == "country_selected") {
			return make_element_by_type<select_gp_selection_icon>(state, id);
		} else if(name == "country_discredited") {
			return make_element_by_type<select_gp_discredited>(state, id);
		} else if(name == "country_banned_embassy") {
			return make_element_by_type<select_gp_banned>(state, id);
		} else if(name == "country_opinion") {
			return make_element_by_type<select_gp_opinion_detail>(state, id);
		} else if(name == "country_influence") {
			return make_element_by_type<select_gp_influence_detail>(state, id);
		} else {
			return nullptr;
		}
	}

	message_result get(sys::state& state, Cyto::Any& payload) noexcept override {
		if(payload.holds_type<gp_choice_num>()) {
			payload.emplace<gp_choice_num>(gp_choice_num{ rank });
			return message_result::consumed;
		}
		return window_element_base::get(state, payload);
	}
};


class gp_action_choice_title : public generic_settable_element<simple_text_element_base, diplomacy_action> {
public:
	void on_update(sys::state& state) noexcept override {
		switch(retrieve<gp_choice_actions>(state, parent)) {
		case gp_choice_actions::discredit:
			set_text(state, text::produce_simple_string(state, "discredittitle"));
			break;
		case gp_choice_actions::expel_advisors:
			set_text(state, text::produce_simple_string(state, "expeladvisorstitle"));
			break;
		case gp_choice_actions::ban_embassy:
			set_text(state, text::produce_simple_string(state, "banembassytitle"));
			break;
		case gp_choice_actions::decrease_opinion:
			set_text(state, text::produce_simple_string(state, "decreaseopiniontitle"));
			break;
		default:
			break;
		}

	}
};

class gp_action_choice_desc : public generic_settable_element<simple_text_element_base, diplomacy_action> {
public:
	void on_update(sys::state& state) noexcept override {
		text::substitution_map m;
		switch(retrieve<gp_choice_actions>(state, parent)) {
		case gp_choice_actions::discredit:
			text::add_to_substitution_map(m, text::variable_type::days, int64_t(state.defines.discredit_days));
			set_text(state, text::resolve_string_substitution(state, "discredit_desc", m));
			break;
		case gp_choice_actions::expel_advisors:
			set_text(state, text::produce_simple_string(state, "expeladvisors_desc"));
			break;
		case gp_choice_actions::ban_embassy:
			text::add_to_substitution_map(m, text::variable_type::days, int64_t(state.defines.banembassy_days));
			set_text(state, text::resolve_string_substitution(state, "banembassy_desc", m));
			break;
		case gp_choice_actions::decrease_opinion:
			set_text(state, text::produce_simple_string(state, "decreaseopinion_desc"));
			break;
		default:
			break;
		}

	}
};

class gp_action_choice_cancel_button : public generic_close_button {
public:
	void on_create(sys::state& state) noexcept override {
		base_data.data.button.txt = state.lookup_key("cancel");
		button_element_base::on_create(state);
	}
};

class gp_action_choice_accept_button : public button_element_base {
public:
	void on_create(sys::state& state) noexcept override {
		base_data.data.button.txt = state.lookup_key("ok");
		button_element_base::on_create(state);
	}

	void on_update(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_get_selection>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);

		switch(retrieve<gp_choice_actions>(state, parent)) {
		case gp_choice_actions::discredit:
			disabled = !command::can_discredit_advisors(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::expel_advisors:
			disabled = !command::can_expel_advisors(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::ban_embassy:
			disabled = !command::can_ban_embassy(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::decrease_opinion:
			disabled = !command::can_decrease_opinion(state, state.local_player_nation, target, gp);
			break;
		}
	}
	void button_action(sys::state& state) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_get_selection>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);

		switch(retrieve<gp_choice_actions>(state, parent)) {
		case gp_choice_actions::discredit:
			command::discredit_advisors(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::expel_advisors:
			command::expel_advisors(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::ban_embassy:
			command::ban_embassy(state, state.local_player_nation, target, gp);
			break;
		case gp_choice_actions::decrease_opinion:
			command::decrease_opinion(state, state.local_player_nation, target, gp);
			break;
		}
		parent->set_visible(state, false);
	}
	tooltip_behavior has_tooltip(sys::state& state) noexcept override {
		return tooltip_behavior::variable_tooltip;
	}
	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents) noexcept override {
		auto gp = nations::get_nth_great_power(state, uint16_t(retrieve<gp_choice_get_selection>(state, parent).value));
		auto target = retrieve<dcon::nation_id>(state, parent);

		switch(retrieve<gp_choice_actions>(state, parent)) {
		case gp_choice_actions::discredit:
		{
			if(state.local_player_nation == gp) {
				text::add_line_with_condition(state, contents, "discredit_explain_1", false);
			}

			auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);

			text::add_line_with_condition(state, contents, "iaction_explain_1", rel && state.world.gp_relationship_get_influence(rel) >= state.defines.discredit_influence_cost, text::variable_type::x, int64_t(state.defines.discredit_influence_cost));

			text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);
			text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

			auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
			text::add_line_with_condition(state, contents, "discredit_explain_2", clevel != nations::influence::level_hostile);


			auto orel = state.world.get_gp_relationship_by_gp_influence_pair(target, gp);
			text::add_line_with_condition(state, contents, "discredit_explain_3", (state.world.gp_relationship_get_status(orel) & nations::influence::is_banned) == 0);

			text::add_line_with_condition(state, contents, "iaction_explain_4", nations::influence::is_influence_level_greater_or_equal(clevel, nations::influence::get_level(state, gp, target)));
		}
		break;
		case gp_choice_actions::expel_advisors:
		{
			if(state.local_player_nation == gp) {
				text::add_line_with_condition(state, contents, "expel_explain_1", false);
			}

			auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);

			text::add_line_with_condition(state, contents, "iaction_explain_1", rel && state.world.gp_relationship_get_influence(rel) >= state.defines.expeladvisors_influence_cost, text::variable_type::x, int64_t(state.defines.expeladvisors_influence_cost));

			text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);

			text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

			auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));

			text::add_line_with_condition(state, contents, "expel_explain_2", clevel != nations::influence::level_hostile && clevel != nations::influence::level_opposed);

			text::add_line_with_condition(state, contents, "iaction_explain_4", nations::influence::is_influence_level_greater_or_equal(clevel, nations::influence::get_level(state, gp, target)));
		}
		break;
		case gp_choice_actions::ban_embassy:
		{
			if(state.local_player_nation == gp) {
				text::add_line_with_condition(state, contents, "ban_explain_1", false);
			}

			auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);

			text::add_line_with_condition(state, contents, "iaction_explain_1", rel && state.world.gp_relationship_get_influence(rel) >= state.defines.banembassy_influence_cost, text::variable_type::x, int64_t(state.defines.banembassy_influence_cost));

			text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);

			text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

			auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
			text::add_line_with_condition(state, contents, "ban_explain_2", clevel == nations::influence::level_friendly || clevel == nations::influence::level_in_sphere);

			text::add_line_with_condition(state, contents, "iaction_explain_5", nations::influence::is_influence_level_greater_or_equal(clevel, nations::influence::get_level(state, gp, target)));
		}
		break;
		case gp_choice_actions::decrease_opinion:
		{
			if(state.local_player_nation == gp) {
				text::add_line_with_condition(state, contents, "dec_op_explain_1", false);
			}

			if(state.world.nation_get_in_sphere_of(target) == gp) {
				text::add_line_with_condition(state, contents, "dec_op_explain_2", false);
			}

			auto rel = state.world.get_gp_relationship_by_gp_influence_pair(target, state.local_player_nation);

			text::add_line_with_condition(state, contents, "iaction_explain_1", rel && state.world.gp_relationship_get_influence(rel) >= state.defines.decreaseopinion_influence_cost, text::variable_type::x, int64_t(state.defines.decreaseopinion_influence_cost));

			text::add_line_with_condition(state, contents, "iaction_explain_2", (state.world.gp_relationship_get_status(rel) & nations::influence::is_banned) == 0);

			text::add_line_with_condition(state, contents, "iaction_explain_3", !military::are_at_war(state, state.local_player_nation, target));

			auto clevel = (nations::influence::level_mask & state.world.gp_relationship_get_status(rel));
			text::add_line_with_condition(state, contents, "dec_op_explain_3", clevel != nations::influence::level_hostile);


			if((nations::influence::level_mask &
				state.world.gp_relationship_get_status(state.world.get_gp_relationship_by_gp_influence_pair(target,
					gp))) == nations::influence::level_hostile) {
				text::add_line_with_condition(state, contents, "dec_op_explain_4", false);
			}

			text::add_line_with_condition(state, contents, "iaction_explain_5", nations::influence::is_influence_level_greater_or_equal(clevel, nations::influence::get_level(state, gp, target)));

		}
		break;
		default:
			break;
		}
	}
};

class diplomacy_gp_action_dialog_window : public window_element_base {
public:
	int32_t selected_gp = 0;
	dcon::nation_id action_target{ 0 };
	gp_choice_actions current_action = gp_choice_actions::discredit;

	void on_create(sys::state& state) noexcept override {
		window_element_base::on_create(state);

		xy_pair gp_base_select_offset =
			state.ui_defs.gui[state.ui_state.defs_by_name.find(state.lookup_key("gpselectoptionpos"))->second.definition].position;
		xy_pair gp_select_offset = gp_base_select_offset;
		for(uint8_t i = 0; i < uint8_t(state.defines.great_nations_count); i++) {
			auto ptr = make_element_by_type<diplomacy_action_gp_dialog_select_window>(state,
					state.ui_state.defs_by_name.find(state.lookup_key("gpselectoption"))->second.definition);
			ptr->base_data.position = gp_select_offset;
			ptr->rank = i;
			// Arrange in columns of 2 elements each...
			gp_select_offset.y += ptr->base_data.size.y;
			if(i != 0 && i % 2 == 1) {
				gp_select_offset.x += ptr->base_data.size.x;
				gp_select_offset.y = gp_base_select_offset.y;
			}
			add_child_to_front(std::move(ptr));
		}
	}

	std::unique_ptr<element_base> make_child(sys::state& state, std::string_view name, dcon::gui_def_id id) noexcept override {
		if(name == "title") {
			return make_element_by_type<gp_action_choice_title>(state, id);
		} else if(name == "description") {
			return make_element_by_type<gp_action_choice_desc>(state, id);
		} else if(name == "agreebutton") {
			return make_element_by_type<gp_action_choice_accept_button>(state, id);
		} else if(name == "declinebutton") {
			return make_element_by_type<gp_action_choice_cancel_button>(state, id);
		} else if(name == "leftshield") {
			return make_element_by_type<nation_player_flag>(state, id);
		} else if(name == "rightshield") {
			return make_element_by_type<flag_button>(state, id);
		} else if(name == "background") {
			auto ptr = make_element_by_type<draggable_target>(state, id);
			ptr->base_data.size = base_data.size;
			return ptr;
		} else {
			return nullptr;
		}
	}

	message_result get(sys::state& state, Cyto::Any& payload) noexcept override {
		if(payload.holds_type<gp_choice_select>()) {
			selected_gp = any_cast<gp_choice_select>(payload).value;
			impl_on_update(state);
			return message_result::consumed;
		} else if(payload.holds_type<dcon::nation_id>()) {
			payload.emplace<dcon::nation_id>(action_target);
			return message_result::consumed;
		} else if(payload.holds_type<gp_choice_get_selection>()) {
			payload.emplace<gp_choice_get_selection>(gp_choice_get_selection{ selected_gp });
			return message_result::consumed;
		} else if(payload.holds_type<gp_choice_actions>()) {
			payload.emplace<gp_choice_actions>(current_action);
			return message_result::consumed;
		}
		return window_element_base::get(state, payload);
	}
};

class diplomacy_action_state_transfer_button : public diplomacy_action_btn_logic {
public:
	dcon::text_key get_name(sys::state& state, dcon::nation_id target) noexcept override {
		return state.lookup_key("state_transfer_button");
	}

	bool is_available(sys::state& state, dcon::nation_id target) noexcept override {
		for(const auto s : state.world.nation_get_state_ownership(target)) {
			if(command::can_state_transfer(state, state.local_player_nation, target, s.get_state().get_definition())) {
				return true;
			}
		}

		return false;
	}

	void button_action(sys::state& state, dcon::nation_id target, ui::element_base* parent) noexcept override {

		sys::state_selection_data seldata;
		seldata.single_state_select = true;
		for(const auto s : state.world.nation_get_state_ownership(state.local_player_nation)) {
			if(command::can_state_transfer(state, state.local_player_nation, target, s.get_state().get_definition())) {
				seldata.selectable_states.push_back(s.get_state().get_definition());
			}
		}
		seldata.on_select = [parent, target](sys::state& state, dcon::state_definition_id sdef) {
			command::state_transfer(state, state.local_player_nation, target, sdef);
			parent->impl_on_update(state);
			};
		seldata.on_cancel = [parent](sys::state& state) {
			parent->impl_on_update(state);
			};
		state.start_state_selection(seldata);
	}

	void update_tooltip(sys::state& state, int32_t x, int32_t y, text::columnar_layout& contents, dcon::nation_id target) noexcept override {
		auto source = state.local_player_nation;

		text::add_line(state, contents, "state_transfer_desc");
		text::add_line_break_to_layout(state, contents);

		text::add_line_with_condition(state, contents, "state_transfer_explain_2", state.current_crisis_state == sys::crisis_state::inactive);
		text::add_line_with_condition(state, contents, "state_transfer_explain_3", !state.world.overlord_get_ruler(state.world.nation_get_overlord_as_subject(source)));
		text::add_line_with_condition(state, contents, "state_transfer_explain_5", !(state.world.nation_get_is_at_war(source) || state.world.nation_get_is_at_war(target)));
		text::add_line_with_condition(state, contents, "state_transfer_explain_6", state.world.nation_get_owned_state_count(source) > 1);

		// Asker and target must be in a subject relation
		if(state.defines.alice_state_transfer_limits > 0.0f) {
			auto ol = state.world.nation_get_overlord_as_subject(source);
			auto ol2 = state.world.nation_get_overlord_as_subject(target);

			text::add_line_with_condition(state, contents, "state_transfer_explain_7", state.world.overlord_get_ruler(ol) == target || state.world.overlord_get_ruler(ol2) == source);

		}
	}
};

template<typename T>
class diplomacy_action_window : public window_element_base {
public:
	std::unique_ptr<element_base> make_child(sys::state& state, std::string_view name, dcon::gui_def_id id) noexcept override {
		if(name == "action_option") {
			return make_element_by_type<T>(state, id);
		} else {
			return nullptr;
		}
	}
};

} // namespace ui
