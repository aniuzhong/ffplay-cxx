#include "null_audio_device.h"

int NullAudioDevice::open(const AVChannelLayout *, int, SDL_AudioCallback, void *)
{
    return 0;
}

void NullAudioDevice::close() {}
void NullAudioDevice::unpause() {}

int  NullAudioDevice::volume() const      { return 0; }
void NullAudioDevice::set_volume(int)     {}
bool NullAudioDevice::muted() const       { return true; }
void NullAudioDevice::toggle_mute()       {}

int  NullAudioDevice::hw_buf_size() const  { return 0; }
int  NullAudioDevice::write_buf_size() const { return 0; }
const AudioParams &NullAudioDevice::hw_params() const
{
    static AudioParams p;
    return p;
}
