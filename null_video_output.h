#pragma once

#include "video_output.h"

class NullVideoOutput : public VideoOutput {
public:
    NullVideoOutput();

    int  open(int w, int h, int x, int y,
              const char *title, bool fullscreen) override;
    void display(Frame *vp, Frame *sp) override;
    void display_audio_vis(AudioVisualizer *vis, AudioOutput *dev,
                           int64_t callback_time, bool paused) override;
    void clear_subtitle_areas(Frame *sp) override;

    const std::vector<int> &supported_pix_fmts()      const override;
    const std::vector<int> &supported_color_spaces()  const override;
    const std::vector<int> &supported_alpha_modes()   const override;

    int fill_buffersink_pixel_formats(enum AVPixelFormat *out,
                                      int capacity) const override;

protected:
    int  create_resources_impl() override;
    void release_resources_impl() override;
};
