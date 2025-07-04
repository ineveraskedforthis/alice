#pragma once

#include <vector>
#include <array>
#include "constants.hpp"
#include "dcon_generated.hpp"
#include "unordered_dense.h"
#include "container_types.hpp"
#include "constants.hpp"
#include "parsers.hpp"
#include "cyto_any.hpp"

namespace parsers {
struct building_gfx_context;
}

namespace ui {

class context_menu_window;

enum class object_type : uint8_t {
	generic_sprite = 0x00,
	bordered_rect = 0x01,
	horizontal_progress_bar = 0x02,
	vertical_progress_bar = 0x03,
	flag_mask = 0x04,
	tile_sprite = 0x05,
	text_sprite = 0x06,
	barchart = 0x07,
	piechart = 0x08,
	linegraph = 0x09
};

struct xy_pair {
	int16_t x = 0;
	int16_t y = 0;
};
static_assert(sizeof(xy_pair) == 4);
struct urect {
	xy_pair top_left;
	xy_pair size;
};

struct gfx_object {
	constexpr static uint8_t always_transparent = 0x10;
	constexpr static uint8_t flip_v = 0x20;
	constexpr static uint8_t has_click_sound = 0x40;
	constexpr static uint8_t do_transparency_check = 0x80;

	constexpr static uint8_t type_mask = 0x0F;

	xy_pair size; // 4bytes
	dcon::text_key name;
	dcon::texture_id primary_texture_handle; // 6bytes
	uint16_t type_dependent = 0; // secondary texture handle or border size -- 8bytes
	uint8_t flags = 0; // 9bytes
	uint8_t number_of_frames = 1; // 10bytes

	object_type get_object_type() const {
		return object_type(flags & type_mask);
	}
	bool is_always_transparent() const {
		return (flags & always_transparent) != 0;
	}
	bool is_vertically_flipped() const {
		return (flags & flip_v) != 0;
	}
	bool is_clicky() const {
		return (flags & has_click_sound) != 0;
	}
	bool is_partially_transparent() const {
		return (flags & do_transparency_check) != 0;
	}
};
static_assert(sizeof(gfx_object) == 16);

enum class element_type : uint8_t { // 3 bits
	button = 0x01,
	text = 0x02,
	image = 0x03,
	position = 0x04,
	overlapping = 0x05,
	listbox = 0x06,
	scrollbar = 0x07,
	window = 0x00
};

enum class alignment : uint8_t { // 2 bits
	left = 0x00,
	right = 0x01,
	centered = 0x02,
	justified = 0x03
};

inline constexpr int32_t rotation_bit_offset = 3;
enum class rotation : uint8_t { // 2 bits
	upright = (0x00 << rotation_bit_offset),
	r90_left = (0x01 << rotation_bit_offset),
	r90_right = (0x02 << rotation_bit_offset)
};

inline constexpr int32_t orientation_bit_offset = 5;
enum class orientation : uint8_t { // 3 bits
	upper_left = (0x00 << orientation_bit_offset),
	upper_right = (0x01 << orientation_bit_offset),
	lower_left = (0x02 << orientation_bit_offset),
	lower_right = (0x03 << orientation_bit_offset),
	upper_center = (0x04 << orientation_bit_offset),
	lower_center = (0x05 << orientation_bit_offset),
	center = (0x06 << orientation_bit_offset)
};

struct text_base_data {
	static constexpr uint16_t alignment_mask = 0x03;

	dcon::text_key txt; // 4bytes
	uint16_t font_handle = 0; // 6bytes
	uint16_t flags = 0; // 8bytes

	alignment get_alignment() const {
		return alignment(flags & alignment_mask);
	}
};
static_assert(sizeof(text_base_data) == 8);

inline constexpr int32_t clicksound_bit_offset = 2;
enum class clicksound : uint16_t { // 2 bits
	none = (0x00 << clicksound_bit_offset),
	click = (0x01 << clicksound_bit_offset),
	close_window = (0x02 << clicksound_bit_offset),
	start_game = (0x03 << clicksound_bit_offset)
};

inline constexpr int32_t checkbox_bit_offset = clicksound_bit_offset + 2;

inline constexpr int32_t button_scripting_bit_offset = checkbox_bit_offset + 1;
enum class button_scripting : uint16_t { // 2 bits
	none = (0x00 << button_scripting_bit_offset),
	province = (0x01 << button_scripting_bit_offset),
	nation = (0x02 << button_scripting_bit_offset)
};

struct button_data : public text_base_data {
	static constexpr uint16_t clicksound_mask = (0x03 << clicksound_bit_offset);
	static constexpr uint16_t is_checkbox_mask = (0x01 << checkbox_bit_offset);
	static constexpr uint16_t button_scripting_mask = (0x03 << button_scripting_bit_offset);

	//8bytes
	dcon::gfx_object_id button_image; // 8+2bytes
	dcon::trigger_key scriptable_enable; // 8 + 4 bytes
	dcon::effect_key scriptable_effect; // 8 + 6 bytes
	sys::virtual_key shortcut = sys::virtual_key::NONE; // 8+7 bytes

	clicksound get_clicksound() const {
		return clicksound(text_base_data::flags & clicksound_mask);
	}
	bool is_checkbox() const {
		return (text_base_data::flags & is_checkbox_mask) != 0;
	}
	button_scripting get_button_scripting() const {
		return button_scripting(text_base_data::flags & button_scripting_mask);
	}
};
static_assert(sizeof(button_data) == sizeof(text_base_data) + 8);

inline constexpr int32_t text_background_bit_offset = 2;
enum class text_background : uint8_t { // 2 bits
	none = (0x00 << text_background_bit_offset),
	tiles_dialog = (0x01 << text_background_bit_offset),
	transparency = (0x02 << text_background_bit_offset),
	small_tiles_dialog = (0x03 << text_background_bit_offset)
};

struct text_data : public text_base_data {
	static constexpr uint8_t background_mask = (0x03 << text_background_bit_offset);
	static constexpr uint8_t is_fixed_size_mask = (0x01 << (text_background_bit_offset + 2));
	static constexpr uint8_t is_instant_mask = (0x01 << (text_background_bit_offset + 3));
	static constexpr uint8_t is_edit_mask = (0x01 << (text_background_bit_offset + 4));

	xy_pair border_size; // 4bytes

	text_background get_text_background() const {
		return text_background(text_base_data::flags & background_mask);
	}
	bool is_fixed_size() const {
		return (text_base_data::flags & is_fixed_size_mask) != 0;
	}
	bool is_instant() const {
		return (text_base_data::flags & is_instant_mask) != 0;
	}
	bool is_edit() const {
		return (text_base_data::flags & is_edit_mask) != 0;
	}
};
static_assert(sizeof(text_data) == sizeof(text_base_data) + 4);

struct image_data {
	static constexpr uint8_t frame_mask = 0x7F;
	static constexpr uint8_t is_mask_mask = 0x80;

	float scale = 1.0f; // 4bytes
	dcon::gfx_object_id gfx_object; // 6bytes
	uint8_t flags = 0; // 7bytes

	bool is_mask() const {
		return (flags & is_mask_mask) != 0;
	}
	uint8_t frame() const {
		return (flags & frame_mask);
	}
};

struct overlapping_data {
	float spacing = 1.0f; // 4bytes
	alignment image_alignment = alignment::left; // 5bytes
};

struct list_box_data {
	xy_pair border_size; // 4bytes
	xy_pair offset; // 8bytes
	dcon::gfx_object_id background_image; // 10bytes
	uint8_t spacing = 0; // 11bytes
};

enum class step_size : uint8_t { // 2 bits
	one = 0x00,
	two = 0x01,
	one_tenth = 0x02,
	one_hundredth = 0x03,
	one_thousandth = 0x04,
	// Non-vanilla
	twenty_five = 0x40
};

struct scrollbar_data {
	static constexpr uint8_t step_size_mask = 0x47;
	static constexpr uint8_t is_range_limited_mask = 0x08;
	static constexpr uint8_t is_lockable_mask = 0x10;
	static constexpr uint8_t is_horizontal_mask = 0x20;

	xy_pair border_size; // 4bytes
	uint16_t max_value = 1; // 6bytes
	dcon::gui_def_id first_child; // 8bytes
	uint8_t num_children = 0; // 9bytes
	uint8_t flags = 0; // 10bytes

	step_size get_step_size() const {
		return step_size(step_size_mask & flags);
	}
	bool is_range_limited() const {
		return (flags & is_range_limited_mask) != 0;
	}
	bool is_lockable() const {
		return (flags & is_lockable_mask) != 0;
	}
	bool is_horizontal() const {
		return (flags & is_horizontal_mask) != 0;
	}
};

struct window_data {
	static constexpr uint8_t is_dialog_mask = 0x01;
	static constexpr uint8_t is_fullscreen_mask = 0x02;
	static constexpr uint8_t is_moveable_mask = 0x04;

	dcon::gui_def_id first_child; // 2bytes
	uint8_t num_children = 0; // 3bytes
	uint8_t flags = 0; // 4bytes

	bool is_dialog() const {
		return (flags & is_dialog_mask) != 0;
	}
	bool is_fullscreen() const {
		return (flags & is_fullscreen_mask) != 0;
	}
	bool is_moveable() const {
		return (flags & is_moveable_mask) != 0;
	}
};

struct position_data { };

struct element_data {
	static constexpr uint8_t type_mask = 0x07;
	static constexpr uint8_t rotation_mask = (0x03 << rotation_bit_offset);
	static constexpr uint8_t orientation_mask = (0x07 << orientation_bit_offset);

	static constexpr uint8_t ex_is_top_level = 0x01;

	xy_pair position; // 4bytes
	xy_pair size; // 8bytes
	dcon::text_key name; // 12bytes

	union alignas(4) internal_data {
		text_base_data text_common; // +5
		button_data button; // +5 + ? +3
		text_data text; // +5 + ? +4
		image_data image; // +6
		overlapping_data overlapping; //+5
		list_box_data list_box; // +11
		scrollbar_data scrollbar; //+10
		window_data window; // +4
		position_data position; //+0

		internal_data() {
			std::memset(this, 0, sizeof(internal_data));
			position = position_data{};
		}
	} data; // +16 = 28
	static_assert(sizeof(internal_data) == 16);

	uint8_t flags = 0; // 29
	uint8_t ex_flags = 0; // 30
	uint8_t padding[2] = { 0, 0 }; // 32

	element_data() {
		std::memset(this, 0, sizeof(element_data));
	}

	element_type get_element_type() const {
		return element_type(flags & type_mask);
	}
	rotation get_rotation() const {
		return rotation(flags & rotation_mask);
	}
	orientation get_orientation() const {
		return orientation(flags & orientation_mask);
	}
	bool is_top_level() const {
		return (ex_flags & ex_is_top_level) != 0;
	}
};
static_assert(sizeof(element_data) == 32);

struct window_extension {
	dcon::text_key window;
	dcon::gui_def_id child;
};

class definitions {
public:
	static constexpr dcon::texture_id small_tiles_dialog = dcon::texture_id(0);
	static constexpr dcon::texture_id tiles_dialog = dcon::texture_id(1);
	static constexpr dcon::texture_id transparency = dcon::texture_id(2);

	tagged_vector<gfx_object, dcon::gfx_object_id> gfx;
	tagged_vector<dcon::text_key, dcon::texture_id> textures;
	tagged_vector<element_data, dcon::gui_def_id> gui;
	std::vector<window_extension> extensions;

};

void load_text_gui_definitions(sys::state& state, parsers::building_gfx_context& context, parsers::error_handler& err);

enum class message_result { unseen, seen, consumed };
enum class focus_result { ignored, accepted };
enum class tooltip_behavior { tooltip, variable_tooltip, position_sensitive_tooltip, no_tooltip };

class element_base;

xy_pair child_relative_location(sys::state& state, element_base const& parent, element_base const& child);
xy_pair get_absolute_location(sys::state& state, element_base const& node);

xy_pair child_relative_non_mirror_location(sys::state& state, element_base const& parent, element_base const& child);
xy_pair get_absolute_non_mirror_location(sys::state& state, element_base const& node);

using ui_hook_fn = std::unique_ptr<element_base> (*)(sys::state&, dcon::gui_def_id);

struct element_target {
	ui_hook_fn generator = nullptr;
	dcon::gui_def_id definition;
};

class tool_tip;
class grid_box;

template<class T>
class unit_details_window;

struct chat_message {
	dcon::nation_id source{};
	dcon::nation_id target{};
	std::string body;
	// the reason the sender name is a unique_ptr and not a string or simple array is cause the Cyto:Any has a space limit of 64 bytes which it becomes encapsulated in later, and together with the body the struct will overflow with a array of size 24.
	std::unique_ptr<uint8_t> sender_name;

	chat_message() {
		sender_name = std::unique_ptr<uint8_t>(new uint8_t[24]);
	}
	chat_message(const chat_message& m) {
		sender_name = std::unique_ptr<uint8_t>(new uint8_t[24]);
		source = m.source;
		target = m.target;
		body = m.body;
		memcpy(sender_name.get(), m.sender_name.get(), 24);
	}
	chat_message(chat_message&&) = default;
	chat_message& operator=(const chat_message& m) {
		if(this == &m)
			return *this;
		source = m.source;
		target = m.target;
		body = m.body;
		memcpy(sender_name.get(), m.sender_name.get(), 24);
		return *this;
	}
	chat_message& operator=(chat_message&&) = default;
	~chat_message() { }

	bool operator==(chat_message const& o) const {
		return source == o.source && target == o.target && body == o.body;
	}
	bool operator!=(chat_message const& o) const {
		return !(*this == o);
	}
	void set_sender_name(const std::array<uint8_t, 24>& name) {
		for(uint16_t i = 0; i < 24; i++) {
			sender_name.get()[i] = name[i];
		}
	}
	std::array<uint8_t, 24> get_sender_name() {
		std::array<uint8_t, 24> result;
		memcpy(&result, sender_name.get(), sizeof(result));
		return result;
	}
};

struct hash_text_key {
	using is_avalanching = void;
	using is_transparent = void;

	hash_text_key() { }

	auto operator()(dcon::text_key sv) const noexcept -> uint64_t {
		return ankerl::unordered_dense::detail::wyhash::hash(&sv, sizeof(sv));
	}
};

struct state {
	element_base* under_mouse = nullptr;
	element_base* left_mouse_hold_target = nullptr;
	uint16_t scrollbar_timer = 0;
	uint16_t fps_timer = 0;
	std::chrono::time_point<std::chrono::steady_clock> last_render_time{};
	bool scrollbar_continuous_movement = false;
	float last_fps = 0.f;
	bool lazy_load_in_game = false;
	element_base* scroll_target = nullptr;
	element_base* drag_target = nullptr;
	element_base* edit_target = nullptr;
	element_base* last_tooltip = nullptr;
	element_base* mouse_sensitive_target = nullptr;
	xy_pair target_ul_bounds = xy_pair{ 0, 0 };
	xy_pair target_lr_bounds = xy_pair{ 0, 0 };
	int32_t last_tooltip_sub_index = -1;
	
	uint32_t cursor_size = 16;

	xy_pair relative_mouse_location = xy_pair{0, 0};
	std::unique_ptr<element_base> units_root;

	std::unique_ptr<element_base> rgos_root;
	std::unique_ptr<element_base> province_details_root;
	std::unique_ptr<element_base> root;
	std::unique_ptr<element_base> military_root;
	std::unique_ptr<element_base> nation_picker;
	std::unique_ptr<element_base> end_screen;
	std::unique_ptr<element_base> select_states_legend;

	std::unique_ptr<element_base> army_group_selector_root;
	std::unique_ptr<element_base> army_group_deselector_root;

	std::unique_ptr<element_base> economy_viewer_root;

	std::unique_ptr<tool_tip> tooltip;
	std::unique_ptr<grid_box> unit_details_box;
	ankerl::unordered_dense::map<dcon::text_key, element_target, hash_text_key> defs_by_name;
	ankerl::unordered_dense::map<dcon::text_key, dcon::gfx_object_id, hash_text_key> gfx_by_name;
	ankerl::unordered_dense::map<std::string, sys::aui_pending_bytes> new_ui_windows;
	std::vector<simple_fs::file> held_open_ui_files;

	std::vector<uint32_t> rebel_flags;

	// elements we are keeping track of
	element_base* main_menu = nullptr; // Settings window
	element_base* r_main_menu = nullptr; // Settings window for non-in-game modes
	element_base* fps_counter = nullptr;
	element_base* console_window = nullptr; // console window
	element_base* console_window_r = nullptr;
	element_base* topbar_window = nullptr;
	element_base* topbar_subwindow = nullptr; // current tab window
	element_base* province_window = nullptr;
	element_base* search_window = nullptr;
	element_base* ledger_window = nullptr;
	element_base* r_ledger_window = nullptr; // end screen ledger window
	element_base* diplomacy_subwindow = nullptr;
	element_base* politics_subwindow = nullptr;
	element_base* population_subwindow = nullptr;
	element_base* production_subwindow = nullptr;
	element_base* trade_subwindow = nullptr;
	element_base* unit_window_army = nullptr;
	element_base* unit_window_navy = nullptr;
	element_base* build_unit_window = nullptr;
	element_base* msg_filters_window = nullptr;
	element_base* outliner_window = nullptr;
	element_base* technology_subwindow = nullptr;
	element_base* military_subwindow = nullptr;
	element_base* request_window = nullptr;
	unit_details_window<dcon::army_id>* army_status_window = nullptr;
	unit_details_window<dcon::navy_id>* navy_status_window = nullptr;
	element_base* army_group_window_land = nullptr;
	element_base* multi_unit_selection_window = nullptr;
	element_base* msg_log_window = nullptr;
	element_base* msg_window = nullptr;
	element_base* menubar_window = nullptr;
	element_base* main_menu_win = nullptr; // The actual main menu
	element_base* chat_window = nullptr;
	element_base* r_chat_window = nullptr;
	element_base* map_gradient_legend = nullptr;
	element_base* map_civ_level_legend = nullptr;
	element_base* map_col_legend = nullptr;
	element_base* map_dip_legend = nullptr;
	element_base* map_rr_legend = nullptr;
	element_base* map_nav_legend = nullptr;
	element_base* map_rank_legend = nullptr;
	element_base* map_rec_legend = nullptr;
	element_base* tl_chat_list = nullptr;
	element_base* error_win = nullptr;
	element_base* naval_combat_window = nullptr;
	element_base* army_combat_window = nullptr;
	element_base* change_leader_window = nullptr;
	element_base* macro_builder_window = nullptr;
	element_base* request_topbar_listbox = nullptr;
	element_base* build_province_unit_window = nullptr;
	element_base* disband_unit_window = nullptr;

	context_menu_window* context_menu = nullptr;

	std::array<chat_message, 32> chat_messages;
	std::vector<dcon::technology_id> tech_queue;
	uint8_t chat_messages_index = 0;

	dcon::gfx_object_id bg_gfx_id{};
	dcon::gfx_object_id load_screens_gfx_id[8];

	std::vector<std::unique_ptr<element_base>> endof_landcombat_windows;
	std::vector<std::unique_ptr<element_base>> endof_navalcombat_windows;

	int32_t held_game_speed = 1; // used to keep track of speed while paused
	int32_t current_template = -1; // used as the currently edited template
	std::vector<sys::macro_builder_template> templates;
	uint16_t tooltip_font = 0;
	uint16_t default_header_font = 0;
	uint16_t default_body_font = 0;
	bool ctrl_held_down = false;
	bool shift_held_down = false;

	float last_tick_investment_pool_change;

	state();
	~state();
};

struct mouse_probe {
	element_base* under_mouse;
	xy_pair relative_location;
};

template<typename T>
constexpr ui_hook_fn hook() {
	return +[](sys::state&, dcon::gui_def_id) { return std::make_unique<T>(); };
}

void populate_definitions_map(sys::state& state);
void make_size_from_graphics(sys::state& state, ui::element_data& dat);
std::unique_ptr<element_base> make_element(sys::state& state, std::string_view name);
std::unique_ptr<element_base> make_element_immediate(sys::state& state, dcon::gui_def_id id); // bypasses global map

void show_main_menu_nation_basic(sys::state& state);
void show_main_menu_nation_picker(sys::state& state);

int32_t ui_width(sys::state const& state);
int32_t ui_height(sys::state const& state);

void create_in_game_windows(sys::state& state);

} // namespace ui
