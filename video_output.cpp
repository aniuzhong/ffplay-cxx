#include "video_output.h"

#include <cmath>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
}

VideoOutput::VideoOutput(SDL_Window *window)
    : window_(window)
{
}

void VideoOutput::close()
{
    if (!resources_created_)
        return;
    release_resources_impl();
    resources_created_ = false;
}

void VideoOutput::calc_display_rect(Rect *out,
                                    int scr_x, int scr_y,
                                    int scr_w, int scr_h,
                                    int pic_w, int pic_h,
                                    AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_w, pic_h));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_h;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_w) {
        width = scr_w;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_w - width) / 2;
    y = (scr_h - height) / 2;
    out->x = scr_x + (int)x;
    out->y = scr_y + (int)y;
    out->w = FFMAX((int)width, 1);
    out->h = FFMAX((int)height, 1);
}
