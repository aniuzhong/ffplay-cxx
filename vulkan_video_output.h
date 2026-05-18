// VulkanVideoOutput — VkRenderer-backed video output for ffplay-cpp.
//
// Phase 2: Vulkan swapchain for video frames + SDL_Renderer for audio
// visualization and subtitle clearing (matching upstream ffplay.c behavior).
// Audio vis and video frame rendering are mutually exclusive per show_mode;
// SDL_RenderClear in display_audio_vis() therefore cannot erase Vulkan content.
//
// Ownership:  does NOT own vk_ (owned by main, destroyed in do_exit).
// Does NOT own window_ (managed by base class).  Owns the internal
// SDL_Renderer created in open().

#pragma once

#include <vector>

#include <SDL.h>

#include "video_output.h"

extern "C" {
struct AVBufferRef;
struct SwsContext;
}

struct VkRenderer;

class VulkanVideoOutput : public VideoOutput {
public:
    VulkanVideoOutput(SDL_Window *window, VkRenderer *vk);
    ~VulkanVideoOutput() override;

    VulkanVideoOutput(const VulkanVideoOutput &) = delete;
    VulkanVideoOutput &operator=(const VulkanVideoOutput &) = delete;

    int  open(int w, int h, int x, int y,
              const char *title, bool fullscreen) override;
    void display(Frame *vp, Frame *sp) override;
    void display_audio_vis(AudioVisualizer *vis, AudioDevice *dev,
                           int64_t callback_time, bool paused) override;
    void clear_subtitle_areas(Frame *sp) override;

    const std::vector<int> &supported_pix_fmts()      const override;
    const std::vector<int> &supported_color_spaces()  const override;
    const std::vector<int> &supported_alpha_modes()   const override;

    int fill_buffersink_pixel_formats(enum AVPixelFormat *out, int capacity) const override;

    AVBufferRef *hw_device_ref() const override;

    void invalidatePixFmtsCache() { pix_fmts_cached_ = false; }

protected:
    int  create_resources_impl() override;
    void release_resources_impl() override;
    void on_layout_changed() override;

private:
    void ensurePixFmtsCached() const;

    // SDL helpers (identical to SDLVideoOutput)
    void fill_rect(int x, int y, int w, int h);
    int  realloc_texture(SDL_Texture **tex, Uint32 fmt, int w, int h,
                          SDL_BlendMode blend, int init);
    static void get_pix_fmt_and_blendmode(int format, Uint32 *sdl_fmt,
                                          SDL_BlendMode *blend);

    VkRenderer *const vk_;

    // SDL resources owned by this object (created in open())
    SDL_Renderer *sdl_renderer_ = nullptr;
    SDL_Texture  *vis_texture_  = nullptr;
    SDL_Texture  *sub_texture_  = nullptr;
    SwsContext   *sub_convert_ctx_ = nullptr;

    mutable bool pix_fmts_cached_ = false;
    mutable std::vector<int> pix_fmts_cache_;
};
