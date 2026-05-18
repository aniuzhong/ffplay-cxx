#pragma once

#include <vector>

#include <SDL.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include "video_types.h"

struct AVFrame;
struct AVBufferRef;
struct Frame;
class AudioVisualizer;
class AudioOutput;

class VideoOutput {
public:
    virtual ~VideoOutput() = default;
    VideoOutput(const VideoOutput &) = delete;
    VideoOutput &operator=(const VideoOutput &) = delete;

    // Idempotent — safe to call multiple times.
    virtual int open(int w, int h, int x, int y,
                     const char *title, bool fullscreen) = 0;

    void close();

    // Update layout (e.g. after window resize).
    void set_layout(const Rect &layout);

    // Each implementation is responsible for its own Clear + Present.
    virtual void display(Frame *vp, Frame *sp) = 0;
    virtual void display_audio_vis(AudioVisualizer *vis, AudioOutput *dev,
                                   int64_t callback_time, bool paused) = 0;

    // Clear expired subtitle texture areas.
    virtual void clear_subtitle_areas(Frame *sp) = 0;

    virtual const std::vector<int> &supported_pix_fmts()      const = 0;
    virtual const std::vector<int> &supported_color_spaces()  const = 0;
    virtual const std::vector<int> &supported_alpha_modes()   const = 0;

    // Intersection of decoder pixel formats the *this* backend can upload to
    // its display path. Writes at most `capacity` entries into `out`; returns
    // count written, or a negative AVERROR on failure.
    virtual int fill_buffersink_pixel_formats(enum AVPixelFormat *out,
                                              int capacity) const = 0;

    static void calc_display_rect(Rect *out,
                                  int scr_x, int scr_y,
                                  int scr_w, int scr_h,
                                  int pic_w, int pic_h,
                                  AVRational pic_sar);

    RenderParams &render_params()             { return render_params_; }
    const RenderParams &render_params() const { return render_params_; }
    const Rect &layout() const                { return layout_; }
    int width()  const { return layout_.w; }
    int height() const { return layout_.h; }

    // HW device reference for derived hwaccel contexts (Vulkan path).
    // Returns nullptr for SDL path (standalone hwaccel).
    virtual AVBufferRef *hw_device_ref() const { return nullptr; }

protected:
    explicit VideoOutput(SDL_Window *window);
    SDL_Window *const window_;

    virtual int create_resources_impl() = 0;
    virtual void release_resources_impl() = 0;

    // Hook called after layout_ is updated. Subclasses override to sync
    // GPU resources (e.g. Vulkan swapchain resize).
    virtual void on_layout_changed() {}

    bool resources_created_ = false;

    Rect          layout_;
    RenderParams  render_params_;
};
