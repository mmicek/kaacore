#pragma once

#include <utility>
#include <vector>

#include <SDL.h>
#include <SDL_mixer.h>

#include "kaacore/resources.h"
#include "kaacore/utils.h"

namespace kaacore {

typedef uint16_t ChannelId;
typedef uint64_t PlaybackUid;

enum struct AudioState {
    stopped = 1,
    paused = 2,
    playing = 3,
};

struct SoundData {
    Mix_Chunk* _raw_sound;

    SoundData(Mix_Chunk* raw_sound);
    ~SoundData();

    static Resource<SoundData> load(const char* path);
};

class Sound {
    friend class AudioManager;

    Resource<SoundData> _sound_data;
    double _volume;

    Sound(Resource<SoundData> sound_data, double volume = 1.);

  public:
    Sound();
    static Sound load(const char* path, double volume = 1.);

    operator bool() const;
    bool operator==(const Sound& other) const;

    double volume() const;

    void play(double volume_factor = 1.);
};

class SoundPlayback {
    Sound _sound;
    double _volume = 1.;
    ChannelId _channel_id;
    uint64_t _playback_uid;

  public:
    SoundPlayback(const Sound& sound, const double volume = 1.);
    ~SoundPlayback() = default;
    SoundPlayback(const SoundPlayback&) = delete;
    SoundPlayback(SoundPlayback&&) = delete;

    SoundPlayback& operator=(const SoundPlayback&) = delete;
    SoundPlayback& operator=(SoundPlayback&&) = delete;

    Sound sound() const;

    double volume() const;
    void volume(const double vol);

    AudioState state() const;
    bool is_playing() const;
    void play(const int loops = 1);

    bool is_paused() const;
    bool pause();
    bool resume();
    bool stop();
};

struct MusicData {
    Mix_Music* _raw_music;

    MusicData(Mix_Music* raw_music);
    ~MusicData();

    static Resource<MusicData> load(const char* path);
};

class Music {
    friend class AudioManager;

    Resource<MusicData> _music_data;
    double _volume;

    Music(Resource<MusicData> music_data, double volume = 1.);

  public:
    Music();
    static Music load(const char* path, double volume = 1.);
    static Music get_current();
    static AudioState get_state();

    operator bool() const;
    bool operator==(const Music& other) const;

    double volume() const;

    bool is_playing() const;
    void play(double volume_factor = 1.);

    bool is_paused() const;
    bool pause();
    bool resume();
    bool stop();
};

struct _AudioState {
    double requested_volume;
    Music current_music;
};

struct _ChannelState {
    double requested_volume;
    Sound current_sound;
    PlaybackUid playback_uid;
    bool paused;

    // we keep track if channel was stopped manually, since
    // there is a possibility that manually stopped channel
    // will be immediately reused, and later cleared up
    // by the channel hook
    bool _manually_stopped;

    void reset();
};

class AudioManager {
    friend class Engine;
    friend class Sound;
    friend class SoundPlayback;
    friend struct SoundData;
    friend class Music;
    friend struct MusicData;

    double _master_volume;
    double _master_sound_volume;
    double _master_music_volume;

    _AudioState _music_state;
    std::vector<_ChannelState> _channels_state;

    Mix_Chunk* load_raw_sound(const char* path);
    Mix_Music* load_raw_music(const char* path);

    std::pair<ChannelId, PlaybackUid> play_sound(
        const Sound& sound, const double volume_factor = 1.,
        const int loops = 1);
    void play_music(const Music& music, const double volume_factor = 1.);
    AudioState music_state();

    AudioState _check_playback(
        const ChannelId& channel_id, const PlaybackUid& playback_uid);

    void _pause_channel(const ChannelId& channel_id);
    void _resume_channel(const ChannelId& channel_id);
    void _stop_channel(const ChannelId& channel_id);
    void _update_channel_volume(
        const ChannelId& channel_id, const double volume);

    void _pause_music();
    void _resume_music();
    void _stop_music();

    void _recalc_music_volume();
    void _recalc_channels_volume();
    void _recalc_channel_volume(ChannelId channel_id);

    void _handle_music_finished();
    void _handle_channel_finished(ChannelId channel_id);

  public:
    AudioManager();
    ~AudioManager();

    double master_volume() const;
    void master_volume(const double vol);

    double master_sound_volume() const;
    void master_sound_volume(const double vol);

    double master_music_volume() const;
    void master_music_volume(const double vol);

    uint16_t mixing_channels() const;
    void mixing_channels(const uint16_t channels);
};

} // namespace kaacore
