#pragma once

#include <cstdint>
#include <functional>

extern "C" {
#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
#include <SDL.h>
}

struct AudioParams {
    int freq = 0;
    AVChannelLayout ch_layout = {};
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    int frame_size = 0;
    int bytes_per_sec = 0;
};

struct AVFrame;
struct SwrContext;

class FrameQueue;
class PacketQueue;

class AudioOutput {
public:
    AudioOutput(FrameQueue *sampq, PacketQueue *audioq);
    ~AudioOutput();

    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    int open(const AVChannelLayout *ch_layout, int sample_rate,
             SDL_AudioCallback cb, void *cb_opaque);

    void read(uint8_t *stream, int len, bool paused,
              std::function<double()> sync_diff_fn,
              const std::function<void(const int16_t *, int)> *on_decode = nullptr);

    double clock_for_set_at() const;
    double clock() const { return audio_clock_; }
    int clock_serial() const { return audio_clock_serial_; }
    const AudioParams &hw_params() const { return audio_tgt_; }
    int hw_buf_size() const { return audio_hw_buf_size_; }
    int write_buf_size() const { return audio_write_buf_size_; }
    void unpause();

    int volume() const { return audio_volume_; }
    void set_volume(int vol) { audio_volume_ = vol; }
    bool muted() const { return muted_; }
    void toggle_mute() { muted_ = !muted_; }

    void close();

private:
    int decode_frame(bool paused,
                     const std::function<double()> &sync_diff_fn,
                     const std::function<void(const int16_t *, int)> *on_decode);
    int synchronize(int nb_samples, double sync_diff);

    FrameQueue *const sampq_;
    PacketQueue *const audioq_;

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
