#pragma once

#include <cstdint>
#include <functional>

extern "C" {
#include <SDL.h>
}

#include "audio_output.h"
#include "frame.h"

struct AVFrame;
struct SwrContext;

using NextAudioFrameFn = std::function<Frame *()>;

class SDLAudioOutput : public AudioOutput {
public:
    SDLAudioOutput();
    ~SDLAudioOutput();

    SDLAudioOutput(const SDLAudioOutput &) = delete;
    SDLAudioOutput &operator=(const SDLAudioOutput &) = delete;

    // -- AudioOutput overrides --
    int  open(const AVChannelLayout *ch_layout, int sample_rate,
              SDL_AudioCallback cb, void *cb_opaque) override;
    void close() override;
    void unpause() override;

    int  volume()       const override { return audio_volume_; }
    void set_volume(int vol) override { audio_volume_ = vol; }
    bool muted()        const override { return muted_; }
    void toggle_mute()        override { muted_ = !muted_; }

    int  hw_buf_size()  const override { return audio_hw_buf_size_; }
    int  write_buf_size() const override { return audio_write_buf_size_; }
    const AudioParams &hw_params() const override { return audio_tgt_; }

    // -- Methods used by sdl_audio_callback (to be moved into AudioPipeline later) --
    void read(uint8_t *stream, int len, bool paused,
              NextAudioFrameFn next_frame,
              std::function<double()> sync_diff_fn,
              const std::function<void(const int16_t *, int)> *on_decode = nullptr);

    double clock_for_set_at() const;
    double clock() const { return audio_clock_; }
    int clock_serial() const { return audio_clock_serial_; }

private:
    int decode_frame(bool paused,
                     NextAudioFrameFn next_frame,
                     const std::function<double()> &sync_diff_fn,
                     const std::function<void(const int16_t *, int)> *on_decode);
    int synchronize(int nb_samples, double sync_diff);

    SDL_AudioDeviceID audio_dev_ = 0;

    AudioParams audio_src_;
    AudioParams audio_tgt_;
    SwrContext *swr_ctx_ = nullptr;

    uint8_t *audio_buf_ = nullptr;
    uint8_t *audio_buf1_ = nullptr;
    unsigned audio_buf_size_ = 0;
    unsigned audio_buf1_size_ = 0;
    int audio_buf_index_ = 0;
    int audio_write_buf_size_ = 0;
    int audio_hw_buf_size_ = 0;

    int audio_volume_ = 0;
    int muted_ = 0;

    double audio_clock_ = NAN;
    int audio_clock_serial_ = -1;
    double audio_diff_cum_ = 0;
    double audio_diff_avg_coef_ = 0;
    double audio_diff_threshold_ = 0;
    int audio_diff_avg_count_ = 0;
};
