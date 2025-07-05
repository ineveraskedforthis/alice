#pragma once

struct IGraphBuilder;
struct IMediaControl;
struct IBasicAudio;
struct IMediaSeeking;
struct IMediaEventEx;
typedef struct HWND__* HWND;

namespace sound {

class audio_instance {
private:
	IGraphBuilder* graph_interface = nullptr;
	IMediaControl* control_interface = nullptr;
	IBasicAudio* audio_interface = nullptr;
	IMediaSeeking* seek_interface = nullptr;
	IMediaEventEx* event_interface = nullptr;
	std::atomic<bool> interface_lock;
public:
	std::wstring filename;
	float volume_multiplier = 1.0f;

	audio_instance() { }
	audio_instance(std::wstring const& file) : filename(file) { }
	audio_instance(audio_instance const&) = delete;
	audio_instance(audio_instance&& o) noexcept
			: graph_interface(o.graph_interface), control_interface(o.control_interface),
				audio_interface(o.audio_interface), seek_interface(o.seek_interface), event_interface(o.event_interface), filename(std::move(o.filename)),
				volume_multiplier(o.volume_multiplier) {

		o.graph_interface = nullptr;
		o.control_interface = nullptr;
		o.audio_interface = nullptr;
		o.seek_interface = nullptr;
		o.event_interface = nullptr;
	}
	~audio_instance();

	void set_file(std::wstring const& file) {
		filename = file;
	}
	void play(float volume, bool as_music, void* window_handle);
	void pause() const;
	void resume() const;
	void stop() const;
	bool is_playing() const;
	void change_volume(float new_volume) const;

	friend class sound_impl;
};

class sound_impl {
private:
	audio_instance* current_effect = nullptr;
	audio_instance* current_interface_sound = nullptr;

public:
	HWND window_handle = nullptr;
	int32_t last_music = -1;
	int32_t first_music = -1;
	bool global_pause = false;

	audio_instance click_sound;
	audio_instance click_left_sound;
	audio_instance click_right_sound;
	audio_instance tab_budget_sound;
	audio_instance hover_sound;
	audio_instance checkbox_sound;
	audio_instance enact_sound;
	audio_instance subtab_sound;
	audio_instance delete_sound;
	audio_instance autochoose_sound;
	audio_instance tab_politics_sound;
	audio_instance tab_diplomacy_sound;
	audio_instance tab_military_sound;
	audio_instance tab_population_sound;
	audio_instance tab_production_sound;
	audio_instance tab_technology_sound;
	audio_instance technology_finished_sound;
	audio_instance army_move_sound;
	audio_instance army_select_sound;
	audio_instance navy_move_sound;
	audio_instance navy_select_sound;
	audio_instance declaration_of_war_sound;
	audio_instance chat_message_sound;
	audio_instance error_sound;
	audio_instance peace_sound;
	audio_instance army_built_sound;
	audio_instance navy_built_sound;
	audio_instance factory_built_sound;
	audio_instance election_sound;
	audio_instance revolt_sound;
	audio_instance fort_built_sound;
	audio_instance railroad_built_sound;
	audio_instance naval_base_built_sound;
	audio_instance minor_event_sound;
	audio_instance major_event_sound;
	audio_instance decline_sound;
	audio_instance accept_sound;
	audio_instance diplomatic_request_sound;
	audio_instance console_open_sound;
	audio_instance console_close_sound;
	audio_instance land_battle_sounds[6];
	audio_instance naval_battle_sounds[6];
	audio_instance province_select_sounds[4];
	audio_instance event_sound;
	audio_instance decision_sound;
	audio_instance pause_sound;
	audio_instance unpause_sound;
	audio_instance enemycapitulated;
	audio_instance wecapitulated;


	std::vector<audio_instance> music_list;

	void play_effect(audio_instance& s, float volume);
	void play_interface_sound(audio_instance& s, float volume);
	void play_music(int32_t track, float volume);

	void pause_effect() const;
	void pause_interface_sound() const;
	void pause_music() const;

	void resume_effect() const;
	void resume_interface_sound() const;
	void resume_music() const;

	void change_effect_volume(float v) const;
	void change_interface_volume(float v) const;
	void change_music_volume(float v) const;

	bool music_finished() const;

	void play_new_track(sys::state& ws);
	void play_next_track(sys::state& ws);
	void play_previous_track(sys::state& ws);
};

} // namespace sound
