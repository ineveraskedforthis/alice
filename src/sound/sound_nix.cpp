#include "sound.hpp"
#include "system_state.hpp"

#define MINIAUDIO_IMPLEMENTATION 1
#define MA_NO_FLAC 1
#define MA_NO_OPENSL 1
#define MA_NO_WEBAUDIO 1
#define MA_NO_SNDIO 1
#define MA_NO_AUDIO4 1
#define MA_NO_WINMM 1
#define MA_NO_WASAPI 1
#define MA_NO_WINMM 1
#define MA_NO_COREAUDIO 1
#include "miniaudio.h"

namespace sound {

sound_impl::sound_impl() {
	if(ma_engine_init(NULL, &engine) != MA_SUCCESS) {
		std::abort(); //TODO: This shouldn't be a cause for abort
	}
}

sound_impl::~sound_impl() {
	ma_engine_uninit(&engine);
}

void sound_impl::set_volume(std::optional<ma_sound>& sound, float volume) {
	if(sound.has_value()) {
		ma_sound_set_volume(&*sound, volume);
	}
}

void sound_impl::override_sound(std::optional<ma_sound>& sound, audio_instance& s, float volume) {
	if(sound.has_value()) {
		ma_sound_uninit(&*sound);
	}

	sound.reset();
	sound.emplace();
	ma_result result = ma_sound_init_from_file(&engine, s.filename.c_str(), 0, NULL, NULL, &*sound);
	if(result == MA_SUCCESS) {
		set_volume(sound, volume);
		ma_sound_start(&*sound);
	}
}

void sound_impl::play_music(int32_t track, float volume) {
	current_music = track;

	audio_instance audio{};
	audio.filename = music_list[track].filename.c_str();
	override_sound(music, audio, volume);
}

void sound_impl::play_new_track(sys::state& ws) {
	if(music_list.size() > 0) {
		int32_t result = int32_t(rand() % music_list.size()); // well aware that using rand is terrible, thanks
		for(uint32_t i = 0; i < 16 && result == last_music; i++) {
			result = int32_t(rand() % music_list.size());
		}
		play_music(result, ws.user_settings.master_volume * ws.user_settings.music_volume);
	}
}
void sound_impl::play_next_track(sys::state& ws) {
	if(music_list.size() > 0) {
		int32_t result = int32_t((last_music + 1) % music_list.size());
		play_music(result, ws.user_settings.master_volume * ws.user_settings.music_volume);
	}
}
void sound_impl::play_previous_track(sys::state& ws) {
	if(music_list.size() > 0) {
		int32_t result = int32_t((last_music - 1) % music_list.size());
		play_music(result, ws.user_settings.master_volume * ws.user_settings.music_volume);
	}
}

bool sound_impl::music_finished() {
	if(music.has_value())
		return music->atEnd;
	return true;
}

void initialize_sound_system(sys::state& state) {
	state.sound_ptr = std::make_unique<sound_impl>();

	auto root_dir = simple_fs::get_root(state.common_fs);
	auto const music_dir = simple_fs::open_directory(root_dir, NATIVE("music"));
	for(auto const& mp3_file : simple_fs::list_files(music_dir, NATIVE(".mp3"))) {
		audio_instance audio{};
		audio.set_file(simple_fs::get_full_name(mp3_file));
		state.sound_ptr->music_list.emplace_back(audio);
		auto file_name = simple_fs::get_full_name(mp3_file);
		if(parsers::native_has_fixed_suffix_ci(file_name.c_str(), file_name.c_str() + file_name.length(), NATIVE("thecoronation_titletheme.mp3")))
			state.sound_ptr->first_music = int32_t(state.sound_ptr->music_list.size()) - 1;
	}
	for(auto const& music_subdir : simple_fs::list_subdirectories(music_dir)) {
		for(auto const& mp3_file : simple_fs::list_files(music_subdir, NATIVE(".mp3"))) {
			audio_instance audio{};
			audio.set_file(simple_fs::get_full_name(mp3_file));
			state.sound_ptr->music_list.emplace_back(audio);
		}
	}

	struct {
		audio_instance* audio;
		native_string_view name;
	} vanilla_sound_table[] = {
		//{ &state.sound_ptr->click_sound, NATIVE("GI_ValidClick.wav") },
		{ &state.sound_ptr->technology_finished_sound, NATIVE("UI_TechnologyFinished.wav") },
		{ &state.sound_ptr->army_move_sound, NATIVE("GI_InfantryMove.wav") },
		{ &state.sound_ptr->army_select_sound, NATIVE("GI_InfantrySelected.wav") },
		{ &state.sound_ptr->navy_move_sound, NATIVE("UI_SailMove.wav") },
		{ &state.sound_ptr->navy_select_sound, NATIVE("UI_SailSelected.wav") },
		{ &state.sound_ptr->declaration_of_war_sound, NATIVE("DeclarationofWar.wav") },
		{ &state.sound_ptr->chat_message_sound, NATIVE("GI_ChatMessage.wav") },
		{ &state.sound_ptr->error_sound, NATIVE("GI_ErrorBlip.wav") },
		{ &state.sound_ptr->peace_sound, NATIVE("Misc_Peace.wav") },
		{ &state.sound_ptr->army_built_sound, NATIVE("UI_LandUnitFinished.wav") },
		{ &state.sound_ptr->navy_built_sound, NATIVE("UI_NavalUnitFinished.wav") },
		{ &state.sound_ptr->factory_built_sound, NATIVE("Misc_NewFactory.wav") },
		{ &state.sound_ptr->revolt_sound, NATIVE("Misc_revolt.wav") },
		{ &state.sound_ptr->fort_built_sound, NATIVE("Misc_Fortification.wav") },
		{ &state.sound_ptr->railroad_built_sound, NATIVE("Misc_Infrastructure.wav") },
		{ &state.sound_ptr->naval_base_built_sound, NATIVE("Misc_CoalingStation.wav") },
		{ &state.sound_ptr->minor_event_sound, NATIVE("GI_MinorBlip.wav") },
		{ &state.sound_ptr->major_event_sound, NATIVE("Misc_Attention.wav") },
		{ &state.sound_ptr->decline_sound, NATIVE("GI_FailureBlip.wav") },
		{ &state.sound_ptr->accept_sound, NATIVE("GI_SuccessBlip.wav") },
		{ &state.sound_ptr->diplomatic_request_sound, NATIVE("GI_MessageWindow.wav") },
		{ &state.sound_ptr->election_sound, NATIVE("Misc_ElectionHeld.wav") },
		{ &state.sound_ptr->land_battle_sounds[0], NATIVE("Combat_Cavalry_1.wav") },
		{ &state.sound_ptr->land_battle_sounds[1], NATIVE("Combat_Cavalry_2.wav") },
		{ &state.sound_ptr->land_battle_sounds[2], NATIVE("Combat_Cavalry_3.wav") },
		{ &state.sound_ptr->land_battle_sounds[3], NATIVE("Combat_Infantry_1.wav") },
		{ &state.sound_ptr->land_battle_sounds[4], NATIVE("Combat_Infantry_2.wav") },
		{ &state.sound_ptr->land_battle_sounds[5], NATIVE("Combat_Infantry_3.wav") },
		{ &state.sound_ptr->naval_battle_sounds[0], NATIVE("Combat_MajorShip_1.wav") },
		{ &state.sound_ptr->naval_battle_sounds[1], NATIVE("Combat_MajorShip_2.wav") },
		{ &state.sound_ptr->naval_battle_sounds[2], NATIVE("Combat_MajorShip_3.wav") },
		{ &state.sound_ptr->naval_battle_sounds[3], NATIVE("Combat_MinorShip_1.wav") },
		{ &state.sound_ptr->naval_battle_sounds[4], NATIVE("Combat_MinorShip_2.wav") },
		{ &state.sound_ptr->naval_battle_sounds[5], NATIVE("Combat_MinorShip_3.wav") },
	};
	auto const sound_directory = simple_fs::open_directory(root_dir, NATIVE("sound"));
	for(const auto& e : vanilla_sound_table) {
		auto file_peek = simple_fs::peek_file(sound_directory, e.name);
		e.audio->set_file(file_peek ? simple_fs::get_full_name(*file_peek) : native_string());
	}
	struct {
		audio_instance* audio;
		native_string_view name;
	} new_sound_table[] = {
		{ &state.sound_ptr->click_sound, NATIVE("NU_AltClick.wav") },
		{ &state.sound_ptr->click_left_sound, NATIVE("NU_ClickL.wav") },
		{ &state.sound_ptr->click_right_sound, NATIVE("NU_ClickR.wav") },
		{ &state.sound_ptr->console_open_sound, NATIVE("NU_OpenConsole.wav") },
		{ &state.sound_ptr->console_close_sound, NATIVE("NU_CloseConsole.wav") },
		{ &state.sound_ptr->tab_budget_sound, NATIVE("NU_TabBudget.wav") },
		{ &state.sound_ptr->hover_sound, NATIVE("NU_Hover.wav") },
		{ &state.sound_ptr->checkbox_sound, NATIVE("NU_Checkbox.wav") },
		{ &state.sound_ptr->enact_sound, NATIVE("NU_Enact.wav") },
		{ &state.sound_ptr->subtab_sound, NATIVE("NU_Subtab.wav") },
		{ &state.sound_ptr->delete_sound, NATIVE("NU_Delete.wav") },
		{ &state.sound_ptr->autochoose_sound, NATIVE("NU_Autochoose.wav") },
		{ &state.sound_ptr->tab_politics_sound, NATIVE("NU_TabPolitics.wav") },
		{ &state.sound_ptr->tab_diplomacy_sound, NATIVE("NU_TabDiplomacy.wav") },
		{ &state.sound_ptr->tab_military_sound, NATIVE("NU_TabMilitary.wav") },
		{ &state.sound_ptr->tab_population_sound, NATIVE("NU_TabPopulation.wav") },
		{ &state.sound_ptr->tab_production_sound, NATIVE("NU_TabProduction.wav") },
		{ &state.sound_ptr->tab_technology_sound, NATIVE("NU_TabTechnology.wav") },
		{ &state.sound_ptr->tab_military_sound, NATIVE("NU_TabMilitary.wav") },
		{ &state.sound_ptr->event_sound, NATIVE("NU_Event.wav") },
		{ &state.sound_ptr->decision_sound, NATIVE("NU_Decision.wav") },
		{ &state.sound_ptr->pause_sound, NATIVE("NU_Pause.wav") },
		{ &state.sound_ptr->unpause_sound, NATIVE("NU_Unpause.wav") },
		{ &state.sound_ptr->enemycapitulated, NATIVE("enemycapitulated.wav") },
		{ &state.sound_ptr->wecapitulated, NATIVE("wecapitulated.wav") },
		{ &state.sound_ptr->province_select_sounds[0], NATIVE("NU_ProvSelect1.wav") },
		{ &state.sound_ptr->province_select_sounds[1], NATIVE("NU_ProvSelect2.wav") },
		{ &state.sound_ptr->province_select_sounds[2], NATIVE("NU_ProvSelect3.wav") },
		{ &state.sound_ptr->province_select_sounds[3], NATIVE("NU_ProvSelect4.wav") },
	};
	auto const assets_directory = simple_fs::open_directory(root_dir, NATIVE("\\assets"));
	for(const auto& e : new_sound_table) {
		auto file_peek = simple_fs::peek_file(assets_directory, e.name);
		e.audio->set_file(file_peek ? simple_fs::get_full_name(*file_peek) : native_string());
	}
}
void change_effect_volume(sys::state& state, float v) {
	state.sound_ptr->set_volume(state.sound_ptr->effect_sound, v);
}
void change_interface_volume(sys::state& state, float v) {
	state.sound_ptr->set_volume(state.sound_ptr->interface_sound, v);
}
void change_music_volume(sys::state& state, float v) {
	state.sound_ptr->set_volume(state.sound_ptr->music, v);
}

void play_effect(sys::state& state, audio_instance& s, float volume) {
	if(state.sound_ptr->global_pause)
		return;
	state.sound_ptr->override_sound(state.sound_ptr->effect_sound, s, volume);
}
void play_interface_sound(sys::state& state, audio_instance& s, float volume) {
	if(state.sound_ptr->global_pause)
		return;
	state.sound_ptr->override_sound(state.sound_ptr->interface_sound, s, volume);
}

void stop_music(sys::state& state) {
	if(state.sound_ptr->music.has_value()) {
		ma_sound_stop(&*state.sound_ptr->music);
	}
}
void start_music(sys::state& state, float v) {
	if(v > 0.0f && state.sound_ptr->music_list.size() != 0) {
		if(state.sound_ptr->first_music != -1) {
			state.sound_ptr->play_music(state.sound_ptr->first_music, v);
		} else {
			if(state.sound_ptr->music.has_value()) {
				ma_sound_start(&*state.sound_ptr->music);
			}
		}
	}
}

void pause_all(sys::state& state) {
	if(state.sound_ptr.get()) {
		state.sound_ptr->global_pause = true;
		if(state.sound_ptr->effect_sound.has_value()) {
			ma_sound_stop(&*state.sound_ptr->effect_sound);
		}
		if(state.sound_ptr->interface_sound.has_value()) {
			ma_sound_stop(&*state.sound_ptr->interface_sound);
		}
		if(state.sound_ptr->music.has_value()) {
			ma_sound_stop(&*state.sound_ptr->music);
		}
	}
}
void resume_all(sys::state& state) {
	if(state.sound_ptr.get()) {
		state.sound_ptr->global_pause = false;
		if(state.sound_ptr->effect_sound.has_value()) {
			ma_sound_start(&*state.sound_ptr->effect_sound);
		}
		if(state.sound_ptr->interface_sound.has_value()) {
			ma_sound_start(&*state.sound_ptr->interface_sound);
		}
		if(state.sound_ptr->music.has_value()) {
			ma_sound_start(&*state.sound_ptr->music);
		}
	}
}

void update_music_track(sys::state& state) {
	if(state.sound_ptr->music_finished()) {
		state.sound_ptr->play_new_track(state);
	}
}

// returns the default click sound -- expect this list of functions to expand as
//    we implement more of the fixed sound effects
audio_instance& get_click_sound(sys::state& state) {
	return state.sound_ptr->click_sound;
}
audio_instance& get_click_left_sound(sys::state& state) {
	return state.sound_ptr->click_left_sound;
}
audio_instance& get_click_right_sound(sys::state& state) {
	return state.sound_ptr->click_right_sound;
}
audio_instance& get_tab_budget_sound(sys::state& state) {
	return state.sound_ptr->tab_budget_sound;
}
audio_instance& get_hover_sound(sys::state& state) {
	return state.sound_ptr->hover_sound;
}
audio_instance& get_checkbox_sound(sys::state& state) {
	return state.sound_ptr->checkbox_sound;
}
audio_instance& get_enact_sound(sys::state& state) {
	return state.sound_ptr->enact_sound;
}
audio_instance& get_subtab_sound(sys::state& state) {
	return state.sound_ptr->subtab_sound;
}
audio_instance& get_delete_sound(sys::state& state) {
	return state.sound_ptr->delete_sound;
}
audio_instance& get_autochoose_sound(sys::state& state) {
	return state.sound_ptr->autochoose_sound;
}
audio_instance& get_tab_politics_sound(sys::state& state) {
	return state.sound_ptr->tab_politics_sound;
}
audio_instance& get_tab_diplomacy_sound(sys::state& state) {
	return state.sound_ptr->tab_diplomacy_sound;
}
audio_instance& get_tab_military_sound(sys::state& state) {
	return state.sound_ptr->tab_military_sound;
}
audio_instance& get_tab_population_sound(sys::state& state) {
	return state.sound_ptr->tab_population_sound;
}
audio_instance& get_tab_production_sound(sys::state& state) {
	return state.sound_ptr->tab_production_sound;
}
audio_instance& get_tab_technology_sound(sys::state& state) {
	return state.sound_ptr->tab_technology_sound;
}
audio_instance& get_army_select_sound(sys::state& state) {
	return state.sound_ptr->army_select_sound;
}
audio_instance& get_army_move_sound(sys::state& state) {
	return state.sound_ptr->army_move_sound;
}
audio_instance& get_navy_select_sound(sys::state& state) {
	return state.sound_ptr->navy_select_sound;
}
audio_instance& get_navy_move_sound(sys::state& state) {
	return state.sound_ptr->navy_move_sound;
}
audio_instance& get_error_sound(sys::state& state) {
	return state.sound_ptr->error_sound;
}
audio_instance& get_peace_sound(sys::state& state) {
	return state.sound_ptr->peace_sound;
}
audio_instance& get_army_built_sound(sys::state& state) {
	return state.sound_ptr->army_built_sound;
}
audio_instance& get_navy_built_sound(sys::state& state) {
	return state.sound_ptr->navy_built_sound;
}
audio_instance& get_declaration_of_war_sound(sys::state& state) {
	return state.sound_ptr->declaration_of_war_sound;
}
audio_instance& get_technology_finished_sound(sys::state& state) {
	return state.sound_ptr->technology_finished_sound;
}
audio_instance& get_factory_built_sound(sys::state& state) {
	return state.sound_ptr->factory_built_sound;
}
audio_instance& get_election_sound(sys::state& state) {
	return state.sound_ptr->election_sound;
}
audio_instance& get_revolt_sound(sys::state& state) {
	return state.sound_ptr->revolt_sound;
}
audio_instance& get_fort_built_sound(sys::state& state) {
	return state.sound_ptr->fort_built_sound;
}
audio_instance& get_railroad_built_sound(sys::state& state) {
	return state.sound_ptr->railroad_built_sound;
}
audio_instance& get_naval_base_built_sound(sys::state& state) {
	return state.sound_ptr->naval_base_built_sound;
}
audio_instance& get_minor_event_sound(sys::state& state) {
	return state.sound_ptr->minor_event_sound;
}
audio_instance& get_major_event_sound(sys::state& state) {
	return state.sound_ptr->major_event_sound;
}
audio_instance& get_decline_sound(sys::state& state) {
	return state.sound_ptr->decline_sound;
}
audio_instance& get_accept_sound(sys::state& state) {
	return state.sound_ptr->accept_sound;
}
audio_instance& get_diplomatic_request_sound(sys::state& state) {
	return state.sound_ptr->diplomatic_request_sound;
}
audio_instance& get_chat_message_sound(sys::state& state) {
	return state.sound_ptr->chat_message_sound;
}
audio_instance& get_console_open_sound(sys::state& state) {
	return state.sound_ptr->console_open_sound;
}
audio_instance& get_console_close_sound(sys::state& state) {
	return state.sound_ptr->console_close_sound;
}

audio_instance& get_event_sound(sys::state& state) {
	return state.sound_ptr->event_sound;
}
audio_instance& get_decision_sound(sys::state& state) {
	return state.sound_ptr->decision_sound;
}
audio_instance& get_pause_sound(sys::state& state) {
	return state.sound_ptr->pause_sound;
}
audio_instance& get_unpause_sound(sys::state& state) {
	return state.sound_ptr->unpause_sound;
}
audio_instance& get_enemycapitulated_sound(sys::state& state) {
	return state.sound_ptr->enemycapitulated;
}
audio_instance& get_wecapitulated_sound(sys::state& state) {
	return state.sound_ptr->wecapitulated;
}

audio_instance& get_random_land_battle_sound(sys::state& state) {
	return state.sound_ptr->land_battle_sounds[int32_t(std::rand() % 6)];
}
audio_instance& get_random_naval_battle_sound(sys::state& state) {
	return state.sound_ptr->naval_battle_sounds[int32_t(std::rand() % 6)];
}
audio_instance& get_random_province_select_sound(sys::state& state) {
	return state.sound_ptr->province_select_sounds[int32_t(std::rand() % 4)];
}

void play_new_track(sys::state& state) {
	state.sound_ptr->play_new_track(state);
}
void play_next_track(sys::state& state) {
	state.sound_ptr->play_next_track(state);
}
void play_previous_track(sys::state& state) {
	state.sound_ptr->play_previous_track(state);
}

native_string get_current_track_name(sys::state& state) {
	if(state.sound_ptr->music.has_value())
		return state.sound_ptr->music_list[state.sound_ptr->current_music].filename;
	return "";
}

} // namespace sound
