// SDLVideoOutput — SDL_Renderer backed video output for ffplay-cpp.
//
// Ownership:  does NOT own renderer_ / window_ (window_ is managed by the
// base class, created in main(), destroyed in do_exit()).  close() only
// frees textures and sws context.

#include "sdl_video_output.h"

#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "audio_output.h"
#include "audio_visualizer.h"
#include "frame.h"
#include "sdl_pixfmt_table.h"

#ifndef USE_ONEPASS_SUBTITLE_RENDER
#define USE_ONEPASS_SUBTITLE_RENDER 1
#endif

namespace {

constexpr int kBgTileSize = 64;

int kColorSpaces[] = {
    AVCOL_SPC_BT709,
    AVCOL_SPC_BT470BG,
    AVCOL_SPC_SMPTE170M,
};

#if LIBAVUTIL_VERSION_MAJOR >= 61
int kAlphaModes[] = {
    AVALPHA_MODE_UNSPECIFIED,
    AVALPHA_MODE_STRAIGHT,
};
#endif

} // namespace

SDL_Rect SDLVideoOutput::to_sdl_rect(const Rect &r)
{
    return {r.x, r.y, r.w, r.h};
}

int SDLVideoOutput::fill_buffersink_pixel_formats(enum AVPixelFormat *out,
                                                  int capacity) const
{
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(renderer_, &ri) != 0)
        return AVERROR(EINVAL);
    if (!out || capacity <= 0)
        return AVERROR(EINVAL);

    int n = 0;
    for (Uint32 i = 0; i < ri.num_texture_formats; i++) {
        for (int j = 0; j < kTextureFormatMapSize; j++) {
            if (ri.texture_formats[i] == kTextureFormatMap[j].texture_fmt) {
                if (n < capacity)
                    out[n++] = static_cast<enum AVPixelFormat>(kTextureFormatMap[j].format);
                break;
            }
        }
    }
    return n;
}

const std::vector<int> &SDLVideoOutput::supported_pix_fmts() const
{
    static std::vector<int> v = [] {
        std::vector<int> f;
        for (int i = 0; i < kTextureFormatMapSize; i++)
            f.push_back(kTextureFormatMap[i].format);
        return f;
    }();
    return v;
}

const std::vector<int> &SDLVideoOutput::supported_color_spaces() const
{
    static std::vector<int> v;
    if (v.empty()) {
        for (auto cs : kColorSpaces)
            v.push_back(cs);
    }
    return v;
}

const std::vector<int> &SDLVideoOutput::supported_alpha_modes() const
{
    static std::vector<int> v;
#if LIBAVUTIL_VERSION_MAJOR >= 61
    if (v.empty()) {
        for (auto am : kAlphaModes)
            v.push_back(am);
    }
#endif
    return v;
}

void SDLVideoOutput::get_pix_fmt_and_blendmode(int format, Uint32 *sdl_fmt,
                                               SDL_BlendMode *blend)
{
    *blend = SDL_BLENDMODE_NONE;
    *sdl_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *blend = SDL_BLENDMODE_BLEND;
    for (int i = 0; i < kTextureFormatMapSize; i++) {
        if (format == kTextureFormatMap[i].format) {
            *sdl_fmt = kTextureFormatMap[i].texture_fmt;
            return;
        }
    }
}

void SDLVideoOutput::set_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P ||
                  frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG ||
                 frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

SDLVideoOutput::SDLVideoOutput(SDL_Renderer *renderer, SDL_Window *window)
    : VideoOutput(window), renderer_(renderer)
{
}

SDLVideoOutput::~SDLVideoOutput()
{
    close();
}

int SDLVideoOutput::open(int w, int h, int x, int y,
                         const char *title, bool fullscreen)
{
    close();

    SDL_SetWindowTitle(window_, title);
    SDL_SetWindowSize(window_, w, h);
    SDL_SetWindowPosition(window_, x, y);
    SDL_SetWindowFullscreen(window_,
        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowWindow(window_);

    int ret = create_resources_impl();
    // Same as ffplay.c video_open / VulkanVideoOutput::open — layout size for
    // keyboard guard in ffplay.cpp (seek, w, a, …) before first video frame.
    set_layout(Rect{0, 0, w, h});
    if (ret < 0)
        return ret;
    resources_created_ = true;
    return 0;
}

int SDLVideoOutput::create_resources_impl()
{
    // renderer_ is injected from main() — nothing to create here.
    return 0;
}

void SDLVideoOutput::release_resources_impl()
{
    if (vid_texture_) { SDL_DestroyTexture(vid_texture_); vid_texture_ = nullptr; }
    if (sub_texture_) { SDL_DestroyTexture(sub_texture_); sub_texture_ = nullptr; }
    if (vis_texture_) { SDL_DestroyTexture(vis_texture_); vis_texture_ = nullptr; }
    if (sub_convert_ctx_) {
        sws_freeContext(sub_convert_ctx_);
        sub_convert_ctx_ = nullptr;
    }
}

void SDLVideoOutput::fill_rect(int x, int y, int w, int h)
{
    SDL_Rect rect = {x, y, w, h};
    if (w && h)
        SDL_RenderFillRect(renderer_, &rect);
}

int SDLVideoOutput::realloc_texture(SDL_Texture **texture, Uint32 new_format,
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
        *texture = SDL_CreateTexture(renderer_, new_format,
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
        av_log(nullptr, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n",
               new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

int SDLVideoOutput::upload_texture(SDL_Texture **tex, AVFrame *frame)
{
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex,
            sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN
                ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
            frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;

    int ret = 0;
    switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 &&
            frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(*tex, nullptr,
                                       frame->data[0], frame->linesize[0],
                                       frame->data[1], frame->linesize[1],
                                       frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 &&
                   frame->linesize[2] < 0) {
            ret = SDL_UpdateYUVTexture(*tex, nullptr,
                frame->data[0] + frame->linesize[0] * (frame->height - 1),
                -frame->linesize[0],
                frame->data[1] + frame->linesize[1] *
                    (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[1],
                frame->data[2] + frame->linesize[2] *
                    (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                -frame->linesize[2]);
        } else {
            av_log(nullptr, AV_LOG_ERROR,
                   "Mixed positive and negative linesizes are not supported.\n");
            return -1;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, nullptr,
                frame->data[0] + frame->linesize[0] * (frame->height - 1),
                -frame->linesize[0]);
        } else {
            ret = SDL_UpdateTexture(*tex, nullptr,
                                     frame->data[0], frame->linesize[0]);
        }
        break;
    }
    return ret;
}

void SDLVideoOutput::draw_background()
{
    SDL_Rect rect = to_sdl_rect(render_params_.target_rect);
    const int tile_size = kBgTileSize;
    SDL_BlendMode blendMode;

    if (vid_texture_ &&
        SDL_GetTextureBlendMode(vid_texture_, &blendMode) == 0 &&
        blendMode == SDL_BLENDMODE_BLEND) {
        switch (render_params_.video_background_type) {
        case VideoBackgroundType::Tiles:
            SDL_SetRenderDrawColor(renderer_, 237, 237, 237, 255);
            fill_rect(rect.x, rect.y, rect.w, rect.h);
            SDL_SetRenderDrawColor(renderer_, 222, 222, 222, 255);
            for (int x = 0; x < rect.w; x += tile_size * 2)
                fill_rect(rect.x + x, rect.y,
                          FFMIN(tile_size, rect.w - x), rect.h);
            for (int y = 0; y < rect.h; y += tile_size * 2)
                fill_rect(rect.x, rect.y + y,
                          rect.w, FFMIN(tile_size, rect.h - y));
            SDL_SetRenderDrawColor(renderer_, 237, 237, 237, 255);
            for (int y = 0; y < rect.h; y += tile_size * 2) {
                int h = FFMIN(tile_size, rect.h - y);
                for (int x = 0; x < rect.w; x += tile_size * 2)
                    fill_rect(x + rect.x, y + rect.y,
                              FFMIN(tile_size, rect.w - x), h);
            }
            break;
        case VideoBackgroundType::Color: {
            const uint8_t *c = render_params_.video_background_color;
            SDL_SetRenderDrawColor(renderer_, c[0], c[1], c[2], c[3]);
            fill_rect(rect.x, rect.y, rect.w, rect.h);
            break;
        }
        case VideoBackgroundType::None:
            SDL_SetTextureBlendMode(vid_texture_, SDL_BLENDMODE_NONE);
            break;
        }
    }
}

void SDLVideoOutput::clear_subtitle_areas(Frame *sp)
{
    if (!sp || !sp->uploaded || !sub_texture_)
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

void SDLVideoOutput::display_image(Frame *vp, Frame *sp)
{
    Rect *rect = &render_params_.target_rect;

    calc_display_rect(rect, layout_.x, layout_.y,
                      layout_.w, layout_.h,
                      vp->width, vp->height, vp->sar);

    if (sp) {
        if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
            if (!sp->uploaded) {
                if (!sp->width || !sp->height) {
                    sp->width  = vp->width;
                    sp->height = vp->height;
                }
                if (realloc_texture(&sub_texture_, SDL_PIXELFORMAT_ARGB8888,
                                    sp->width, sp->height,
                                    SDL_BLENDMODE_BLEND, 1) < 0)
                    return;

                for (int i = 0; i < sp->sub.num_rects; i++) {
                    AVSubtitleRect *sub_rect = sp->sub.rects[i];
                    sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                    sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                    sub_rect->w = av_clip(sub_rect->w, 0,
                                          sp->width  - sub_rect->x);
                    sub_rect->h = av_clip(sub_rect->h, 0,
                                          sp->height - sub_rect->y);

                    sub_convert_ctx_ = sws_getCachedContext(
                        sub_convert_ctx_,
                        sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                        sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                        0, nullptr, nullptr, nullptr);
                    if (!sub_convert_ctx_) {
                        av_log(nullptr, AV_LOG_FATAL,
                               "Cannot initialize the conversion context\n");
                        return;
                    }
                    uint8_t *pixels;
                    int pitch;
                    if (SDL_LockTexture(sub_texture_,
                                        (SDL_Rect *)sub_rect,
                                        (void **)&pixels, &pitch) == 0) {
                        sws_scale(sub_convert_ctx_,
                                  (const uint8_t * const *)sub_rect->data,
                                  sub_rect->linesize,
                                  0, sub_rect->h, &pixels, &pitch);
                        SDL_UnlockTexture(sub_texture_);
                    }
                }
                sp->uploaded = 1;
            }
        } else {
            sp = nullptr;
        }
    }

    set_yuv_conversion_mode(vp->frame);

    if (!vp->uploaded) {
        if (upload_texture(&vid_texture_, vp->frame) < 0) {
            set_yuv_conversion_mode(nullptr);
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    draw_background();

    SDL_Rect sdl_rect = to_sdl_rect(*rect);
    SDL_RenderCopyEx(renderer_, vid_texture_, nullptr, &sdl_rect,
                     0, nullptr, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
    set_yuv_conversion_mode(nullptr);

    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer_, sub_texture_, nullptr, &sdl_rect);
#else
        double xratio = (double)sdl_rect.w / (double)sp->width;
        double yratio = (double)sdl_rect.h / (double)sp->height;
        for (int i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect *)sp->sub.rects[i];
            SDL_Rect target = {
                sdl_rect.x + (int)(sub_rect->x * xratio),
                sdl_rect.y + (int)(sub_rect->y * yratio),
                (int)(sub_rect->w * xratio),
                (int)(sub_rect->h * yratio),
            };
            SDL_RenderCopy(renderer_, sub_texture_, sub_rect, &target);
        }
#endif
    }
}

void SDLVideoOutput::display_audio_vis(AudioVisualizer *vis, AudioOutput *dev,
                                       int64_t callback_time, bool paused)
{
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);

    if (!vis || !dev) {
        SDL_RenderPresent(renderer_);
        return;
    }

    int channels = dev->hw_params().ch_layout.nb_channels;

    /* Snapshot ring under mutex; feed() runs on SDL audio thread. */
    vis->sync_display_ring();

    typedef AudioVisualizer::ShowMode ShowMode;
    if (vis->mode() == ShowMode::Waves) {
        int i_start = vis->prepare_waveform(
            channels, dev->write_buf_size(), callback_time,
            dev->hw_params().freq, paused, layout_.w);

        int nb_display_channels = channels;
        int h = layout_.h / nb_display_channels;
        int h2 = (h * 9) / 20;

        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
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

        SDL_SetRenderDrawColor(renderer_, 0, 0, 255, 255);
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
            SDL_RenderPresent(renderer_);
            return;
        }

        if (realloc_texture(&vis_texture_, SDL_PIXELFORMAT_ARGB8888,
                            layout_.w, layout_.h,
                            SDL_BLENDMODE_NONE, 1) < 0) {
            SDL_RenderPresent(renderer_);
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
        SDL_RenderCopy(renderer_, vis_texture_, nullptr, nullptr);
    }

    SDL_RenderPresent(renderer_);
}

void SDLVideoOutput::display(Frame *vp, Frame *sp)
{
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (vp && vp->frame)
        display_image(vp, sp);
    SDL_RenderPresent(renderer_);
}
