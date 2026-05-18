#pragma once

#include <vector>

#include <SDL.h>

#include "video_output.h"

struct SwsContext;

class SDLVideoOutput : public VideoOutput {
public:
    SDLVideoOutput(SDL_Renderer *renderer, SDL_Window *window);
    ~SDLVideoOutput() override;
    SDLVideoOutput(const SDLVideoOutput &) = delete;
    SDLVideoOutput &operator=(const SDLVideoOutput &) = delete;

    int  open(int w, int h, int x, int y,
              const char *title, bool fullscreen) override;
    void display(Frame *vp, Frame *sp) override;
    void display_audio_vis(AudioVisualizer *vis, AudioOutput *dev,
                           int64_t callback_time, bool paused) override;
    void clear_subtitle_areas(Frame *sp) override;

    const std::vector<int> &supported_pix_fmts()      const override;
    const std::vector<int> &supported_color_spaces()  const override;
    const std::vector<int> &supported_alpha_modes()   const override;

    int fill_buffersink_pixel_formats(enum AVPixelFormat *out, int capacity) const override;

protected:
    int  create_resources_impl() override;
    void release_resources_impl() override;

private:
    void display_image(Frame *vp, Frame *sp);
    void draw_background();
    int  upload_texture(SDL_Texture **tex, AVFrame *frame);
    int  realloc_texture(SDL_Texture **tex, Uint32 fmt, int w, int h, SDL_BlendMode blend, int init);
    void fill_rect(int x, int y, int w, int h);
    static void get_pix_fmt_and_blendmode(int format, Uint32 *sdl_fmt, SDL_BlendMode *blend);
    static void set_yuv_conversion_mode(AVFrame *frame);

    static SDL_Rect to_sdl_rect(const Rect &r);

    // Does not own renderer_ (lifetime managed by main / do_exit).
    SDL_Renderer *const renderer_;
    SDL_Texture  *vid_texture_    = nullptr;
    SDL_Texture  *sub_texture_    = nullptr;
    SDL_Texture  *vis_texture_    = nullptr;
    SwsContext   *sub_convert_ctx_ = nullptr;
};
