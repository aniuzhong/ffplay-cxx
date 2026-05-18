#pragma once

#include "audio_output.h"

class NullAudioOutput : public AudioOutput {
public:
    int  open(const AVChannelLayout *, int, SDL_AudioCallback, void *) override;
    void close() override;
    void unpause() override;

    int  volume() const override;
    void set_volume(int) override;
    bool muted() const override;
    void toggle_mute() override;

    int  hw_buf_size() const override;
    int  write_buf_size() const override;
    const AudioParams &hw_params() const override;
};
