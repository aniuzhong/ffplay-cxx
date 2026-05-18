#pragma once

#include <cstdint>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <SDL.h>
}

struct AudioParams {
    int freq = 0;
    AVChannelLayout ch_layout = {};
    AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
    int frame_size = 0;
    int bytes_per_sec = 0;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual int  open(const AVChannelLayout *ch_layout, int sample_rate,
                      SDL_AudioCallback cb, void *opaque) = 0;
    virtual void close() = 0;
    virtual void unpause() = 0;

    virtual int  volume() const = 0;
    virtual void set_volume(int vol) = 0;
    virtual bool muted() const = 0;
    virtual void toggle_mute() = 0;

    virtual int  hw_buf_size() const = 0;
    virtual int  write_buf_size() const = 0;
    virtual const AudioParams &hw_params() const = 0;
};
