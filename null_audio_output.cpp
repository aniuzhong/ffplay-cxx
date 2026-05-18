#include "null_audio_output.h"

int NullAudioOutput::open(const AVChannelLayout *, int, SDL_AudioCallback, void *)
{
    return 0;
}

void NullAudioOutput::close() {}
void NullAudioOutput::unpause() {}

int  NullAudioOutput::volume() const      { return 0; }
void NullAudioOutput::set_volume(int)     {}
bool NullAudioOutput::muted() const       { return true; }
void NullAudioOutput::toggle_mute()       {}

int  NullAudioOutput::hw_buf_size() const  { return 0; }
int  NullAudioOutput::write_buf_size() const { return 0; }
const AudioParams &NullAudioOutput::hw_params() const
{
    static AudioParams p;
    return p;
}
