#include "null_video_output.h"

NullVideoOutput::NullVideoOutput()
    : VideoOutput(nullptr)
{
}

int NullVideoOutput::open(int, int, int, int, const char *, bool)
{
    close();
    int ret = create_resources_impl();
    if (ret < 0)
        return ret;
    resources_created_ = true;
    return 0;
}

void NullVideoOutput::display(Frame *, Frame *)              {}
void NullVideoOutput::display_audio_vis(AudioVisualizer *, AudioDevice *,
                                         int64_t, bool)      {}
void NullVideoOutput::clear_subtitle_areas(Frame *)          {}

const std::vector<int> &NullVideoOutput::supported_pix_fmts() const
{
    static std::vector<int> v;
    return v;
}
const std::vector<int> &NullVideoOutput::supported_color_spaces() const
{
    static std::vector<int> v;
    return v;
}
const std::vector<int> &NullVideoOutput::supported_alpha_modes() const
{
    static std::vector<int> v;
    return v;
}

int NullVideoOutput::fill_buffersink_pixel_formats(AVPixelFormat *, int) const
{
    return 0;
}

int  NullVideoOutput::create_resources_impl()  { return 0; }
void NullVideoOutput::release_resources_impl() {}
