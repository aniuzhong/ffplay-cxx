// VulkanVideoOutput — VkRenderer-backed video output implementation.
// Phase 2: Vulkan for video frames + SDL_Renderer for audio vis / subtitles.

#include "vulkan_video_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "audio_device.h"
#include "audio_visualizer.h"
#include "vulkan_video_output_impl.h"
#include "frame.h"
#include "sdl_pixfmt_table.h"

namespace {

constexpr int kBgTileSize = 64;

} // namespace

// ===========================================================================
//  Construction / destruction
// ===========================================================================

VulkanVideoOutput::VulkanVideoOutput(SDL_Window *window, VkRenderer *vk)
    : VideoOutput(window)
    , vk_(vk)
{
}

VulkanVideoOutput::~VulkanVideoOutput()
{
    close();
}

// ===========================================================================
//  open / resources
// ===========================================================================

int VulkanVideoOutput::open(int w, int h, int x, int y,
                             const char *title, bool fullscreen)
{
    close();

    if (!window_)
        return -1;

    SDL_SetWindowTitle(window_, title ? title : "ffplay");
    SDL_SetWindowSize(window_, w, h);
    SDL_SetWindowPosition(window_, x, y);
    SDL_SetWindowFullscreen(window_,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowWindow(window_);

    int ret = create_resources_impl();
    // Mirror ffplay.c video_open(): set logical size as soon as the window is
    // configured, so event_loop's `if (!videoDevice()->width()) continue` does
    // not discard seek / hotkeys until the first decoded frame (rindex_shown).
    set_layout(Rect{0, 0, w, h});
    if (ret < 0)
        return ret;
    return 0;
}

int VulkanVideoOutput::create_resources_impl()
{
    // Create an SDL_Renderer for audio visualization and subtitle overlays.
    // This is the same renderer that SDLVideoOutput uses, but Vulkan handles
    // video frame display — the renderer is only for 2D overlays.
    sdl_renderer_ = SDL_CreateRenderer(window_, -1,
                   SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer_) {
        av_log(nullptr, AV_LOG_WARNING,
               "Failed to create SDL renderer for overlays: %s\n",
               SDL_GetError());
        sdl_renderer_ = SDL_CreateRenderer(window_, -1, 0);
    }
    if (!sdl_renderer_) {
        av_log(nullptr, AV_LOG_ERROR,
               "Cannot create SDL renderer for overlays\n");
        return -1;
    }
    resources_created_ = true;
    return 0;
}

void VulkanVideoOutput::release_resources_impl()
{
    if (vis_texture_)      { SDL_DestroyTexture(vis_texture_); vis_texture_ = nullptr; }
    if (sub_texture_)      { SDL_DestroyTexture(sub_texture_); sub_texture_ = nullptr; }
    if (sub_convert_ctx_)  { sws_freeContext(sub_convert_ctx_); sub_convert_ctx_ = nullptr; }
    if (sdl_renderer_)     { SDL_DestroyRenderer(sdl_renderer_); sdl_renderer_ = nullptr; }
    resources_created_ = false;
}

void VulkanVideoOutput::on_layout_changed()
{
    if (vk_)
        vk_renderer_resize(vk_, layout_.w, layout_.h);
    // Destroy vis texture on resize — will be recreated on next display_audio_vis.
    if (vis_texture_) {
        SDL_DestroyTexture(vis_texture_);
        vis_texture_ = nullptr;
    }
}

// ===========================================================================
//  SDL helpers (from SDLVideoOutput)
// ===========================================================================

void VulkanVideoOutput::fill_rect(int x, int y, int w, int h)
{
    SDL_Rect rect = {x, y, w, h};
    if (w && h && sdl_renderer_)
        SDL_RenderFillRect(sdl_renderer_, &rect);
}

int VulkanVideoOutput::realloc_texture(SDL_Texture **texture, Uint32 new_format,
                                        int new_width, int new_height,
                                        SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture ||
        SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 ||
        new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        *texture = SDL_CreateTexture(sdl_renderer_, new_format,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      new_width, new_height);
        if (!*texture)
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, nullptr, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
    }
    return 0;
}

void VulkanVideoOutput::get_pix_fmt_and_blendmode(int format,
                                                   Uint32 *sdl_fmt,
                                                   SDL_BlendMode *blend)
{
    *sdl_fmt = SDL_PIXELFORMAT_UNKNOWN;
    *blend = SDL_BLENDMODE_NONE;
    if (format == AV_PIX_FMT_RGB32   || format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   || format == AV_PIX_FMT_BGR32_1 ||
        format == AV_PIX_FMT_0RGB32  || format == AV_PIX_FMT_0BGR32  ||
        format == AV_PIX_FMT_NE(RGB0, 0BGR) ||
        format == AV_PIX_FMT_NE(BGR0, 0RGB))
        *blend = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < kTextureFormatMapSize; i++) {
        if (format == kTextureFormatMap[i].format) {
            *sdl_fmt = kTextureFormatMap[i].texture_fmt;
            return;
        }
    }
}

// ===========================================================================
//  display  (Vulkan video path — unchanged from Phase 1)
// ===========================================================================

void VulkanVideoOutput::display(Frame *vp, Frame * /* sp unused in Vulkan path */)
{
    if (!vk_ || !vp || !vp->frame)
        return;

    VideoOutput::calc_display_rect(&render_params_.target_rect,
                                   layout_.x, layout_.y,
                                   layout_.w, layout_.h,
                                   vp->width, vp->height, vp->sar);
    vk_renderer_display(vk_, vp->frame, &render_params_);
}

// ===========================================================================
//  display_audio_vis  (SDL path — from SDLVideoOutput)
// ===========================================================================

void VulkanVideoOutput::display_audio_vis(AudioVisualizer *vis, AudioDevice *dev,
                                           int64_t callback_time, bool paused)
{
    if (!sdl_renderer_)
        return;

    SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer_);

    if (!vis || !dev) {
        SDL_RenderPresent(sdl_renderer_);
        return;
    }

    int channels = dev->hw_params().ch_layout.nb_channels;
    vis->sync_display_ring();

    typedef AudioVisualizer::ShowMode ShowMode;
    if (vis->mode() == ShowMode::Waves) {
        int i_start = vis->prepare_waveform(
            channels, dev->write_buf_size(), callback_time,
            dev->hw_params().freq, paused, layout_.w);

        int nb_display_channels = channels;
        int h = layout_.h / nb_display_channels;
        int h2 = (h * 9) / 20;

        SDL_SetRenderDrawColor(sdl_renderer_, 255, 255, 255, 255);
        for (int ch = 0; ch < nb_display_channels; ch++) {
            int i = AudioVisualizer::compute_mod(
                i_start + ch, AudioVisualizer::SAMPLE_ARRAY_SIZE);
            int y1 = layout_.y + ch * h + (h / 2);
            for (int x = 0; x < layout_.w; x++) {
                int y = (vis->sample_at(i) * h2) >> 15;
                int ys;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rect(layout_.x + x, ys, 1, y);
                i += channels;
                if (i >= AudioVisualizer::SAMPLE_ARRAY_SIZE)
                    i -= AudioVisualizer::SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(sdl_renderer_, 0, 0, 255, 255);
        for (int ch = 1; ch < nb_display_channels; ch++) {
            int y = layout_.y + ch * h;
            fill_rect(layout_.x, y, layout_.w, 1);
        }
    } else {
        if (vis->prepare_spectrum_column(
                channels, dev->write_buf_size(), callback_time,
                dev->hw_params().freq, paused,
                layout_.w, layout_.h) < 0) {
            vis->set_mode(ShowMode::Waves);
            SDL_RenderPresent(sdl_renderer_);
            return;
        }

        if (realloc_texture(&vis_texture_, SDL_PIXELFORMAT_ARGB8888,
                            layout_.w, layout_.h,
                            SDL_BLENDMODE_NONE, 1) < 0) {
            SDL_RenderPresent(sdl_renderer_);
            return;
        }

        int nb_display_channels = FFMIN(vis->nb_display_channels(), 2);
        int nb_freq = vis->nb_freq();
        SDL_Rect rect = {vis->xpos(), 0, 1, layout_.h};
        uint32_t *pixels;
        int pitch;
        if (SDL_LockTexture(vis_texture_, &rect, (void **)&pixels, &pitch) == 0) {
            pitch >>= 2;
            pixels += pitch * layout_.h;
            for (int y = 0; y < layout_.h; y++) {
                double w = 1 / sqrt(nb_freq);
                const AVComplexFloat *d0 = vis->spectrum_data(0);
                const AVComplexFloat *d1 = vis->spectrum_data(1);
                int a = (int)sqrt(w * sqrt(d0[y].re * d0[y].re +
                                           d0[y].im * d0[y].im));
                int b = (nb_display_channels == 2)
                    ? (int)sqrt(w * hypot(d1[y].re, d1[y].im)) : a;
                a = FFMIN(a, 255);
                b = FFMIN(b, 255);
                pixels -= pitch;
                *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
            }
            SDL_UnlockTexture(vis_texture_);
        }
        SDL_RenderCopy(sdl_renderer_, vis_texture_, nullptr, nullptr);
    }

    SDL_RenderPresent(sdl_renderer_);
}

// ===========================================================================
//  clear_subtitle_areas  (SDL path — from SDLVideoOutput)
// ===========================================================================

void VulkanVideoOutput::clear_subtitle_areas(Frame *sp)
{
    if (!sp || !sp->uploaded || !sub_texture_ || !sdl_renderer_)
        return;
    for (int i = 0; i < sp->sub.num_rects; i++) {
        AVSubtitleRect *sub_rect = sp->sub.rects[i];
        uint8_t *pixels;
        int pitch;
        if (SDL_LockTexture(sub_texture_, (SDL_Rect *)sub_rect,
                            (void **)&pixels, &pitch) == 0) {
            for (int j = 0; j < sub_rect->h; j++, pixels += pitch)
                memset(pixels, 0, sub_rect->w << 2);
            SDL_UnlockTexture(sub_texture_);
        }
    }
}

// ===========================================================================
//  HW device reference
// ===========================================================================

AVBufferRef *VulkanVideoOutput::hw_device_ref() const
{
    if (!vk_)
        return nullptr;

    AVBufferRef *dev = nullptr;
    vk_renderer_get_hw_dev(const_cast<VkRenderer *>(vk_), &dev);
    return dev;
}

// ===========================================================================
//  Pixel format / colorspace / alpha queries
// ===========================================================================

void VulkanVideoOutput::ensurePixFmtsCached() const
{
    if (pix_fmts_cached_)
        return;
    pix_fmts_cached_ = true;

    for (int i = 0; i < kTextureFormatMapSize; i++)
        pix_fmts_cache_.push_back(kTextureFormatMap[i].format);

    pix_fmts_cache_.push_back(AV_PIX_FMT_VULKAN);
    pix_fmts_cache_.push_back(AV_PIX_FMT_D3D11);
    pix_fmts_cache_.push_back(AV_PIX_FMT_CUDA);
    pix_fmts_cache_.push_back(AV_PIX_FMT_DXVA2_VLD);

    std::sort(pix_fmts_cache_.begin(), pix_fmts_cache_.end());
    pix_fmts_cache_.erase(
        std::unique(pix_fmts_cache_.begin(), pix_fmts_cache_.end()),
        pix_fmts_cache_.end());
}

const std::vector<int> &VulkanVideoOutput::supported_pix_fmts() const
{
    ensurePixFmtsCached();
    return pix_fmts_cache_;
}

const std::vector<int> &VulkanVideoOutput::supported_color_spaces() const
{
    static const std::vector<int> empty;
    return empty;
}

const std::vector<int> &VulkanVideoOutput::supported_alpha_modes() const
{
    static const std::vector<int> empty;
    return empty;
}

int VulkanVideoOutput::fill_buffersink_pixel_formats(AVPixelFormat *out,
                                                      int capacity) const
{
    const auto &fmts = supported_pix_fmts();
    int n = std::min(static_cast<int>(fmts.size()), capacity);
    for (int i = 0; i < n; i++)
        out[i] = static_cast<AVPixelFormat>(fmts[i]);
    return n;
}
