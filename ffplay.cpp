/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include <cmath>
#include <climits>
#include <csignal>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <SDL.h>

extern "C" {
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/dict.h>
#include <libavutil/fifo.h>
#include <libavutil/parseutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/bprint.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/tx.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include "config.h"
#include "config_components.h"
#include "packet_queue.h"
#include "clock.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "audio_output.h"
#include "audio_visualizer.h"
#include "decoder.h"
#include "video_output.h"
#include "sdl_video_output.h"
#include "utils/cmdutils.h"
#include "utils/opt_common.h"

typedef AudioVisualizer::ShowMode ShowMode;

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

constexpr int MAX_QUEUE_SIZE = 15 * 1024 * 1024;
constexpr int MIN_FRAMES = 25;
constexpr int EXTERNAL_CLOCK_MIN_FRAMES = 2;
constexpr int EXTERNAL_CLOCK_MAX_FRAMES = 10;

/* Minimum SDL audio buffer size, in samples. */
constexpr int SDL_AUDIO_MIN_BUFFER_SIZE = 512;
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
constexpr int SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;

/* Step size for volume control in dB */
constexpr double SDL_VOLUME_STEP = 0.75;

/* no AV sync correction is done if below the minimum AV sync threshold */
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
/* AV sync correction is done if above the maximum AV sync threshold */
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
/* maximum audio speed change to get correct sync */
constexpr int SAMPLE_CORRECTION_PERCENT_MAX = 10;

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
constexpr double EXTERNAL_CLOCK_SPEED_MIN  = 0.900;
constexpr double EXTERNAL_CLOCK_SPEED_MAX  = 1.010;
constexpr double EXTERNAL_CLOCK_SPEED_STEP = 0.001;

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
constexpr int AUDIO_DIFF_AVG_NB = 20;

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
constexpr double REFRESH_RATE = 0.01;

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
constexpr int64_t CURSOR_HIDE_DELAY = 1000000;

constexpr int VIDEO_PICTURE_QUEUE_SIZE = 3;
constexpr int SUBPICTURE_QUEUE_SIZE = 16;
constexpr int SAMPLE_QUEUE_SIZE = 9;

enum class AVSyncType {
    AudioMaster, /* default choice */
    VideoMaster,
    ExternalClock, /* synchronize to an external clock */
};

struct VideoState {
    std::unique_ptr<Demuxer> dmx_;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

    int audio_stream;
    AVStream *audio_st;
    PacketQueue audioq;

    AVSyncType av_sync_type;

    AudioOutput *audio_out = nullptr;
    AudioParams audio_filter_src;
    int frame_drops_early;
    int frame_drops_late;

    AudioVisualizer audio_vis;
    double last_vis_time;

    VideoOutput *video_out = nullptr;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;

    int force_refresh;
    int step;

    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph

    int last_video_stream, last_audio_stream, last_subtitle_stream;
};

/* options specified by the user */
static const AVInputFormat *file_iformat;
static const char *input_filename;
static const char *window_title;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwaysontop;
static int startup_volume = 100;
static int show_status = -1;
static AVSyncType av_sync_type = AVSyncType::AudioMaster;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static ShowMode show_mode = ShowMode::None;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
static const char **vfilters_list = nullptr;
static int nb_vfilters = 0;
static char *afilters = nullptr;
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;
static char *video_background = nullptr;
static const char *hwaccel = nullptr;

/* current context */
static int is_full_screen;
int64_t audio_callback_time;

constexpr Uint32 FF_QUIT_EVENT = SDL_USEREVENT + 2;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    int ret = GROW_ARRAY(vfilters_list, nb_vfilters);
    if (ret < 0)
        return ret;

    vfilters_list[nb_vfilters - 1] = av_strdup(arg);
    if (!vfilters_list[nb_vfilters - 1])
        return AVERROR(ENOMEM);

    return 0;
}

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->dmx_->ic();
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->auddec.abort(&is->sampq);
        is->auddec.release_codec();
        is->audio_out->close();

        is->audio_vis.destroy_rdft();
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->viddec.abort(&is->pictq);
        is->viddec.release_codec();
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subdec.abort(&is->subpq);
        is->subdec.release_codec();
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = nullptr;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = nullptr;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = nullptr;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    if (is->dmx_) {
        is->dmx_->stop();

        /* close each stream */
        if (is->audio_stream >= 0)
            stream_component_close(is, is->audio_stream);
        if (is->video_stream >= 0)
            stream_component_close(is, is->video_stream);
        if (is->subtitle_stream >= 0)
            stream_component_close(is, is->subtitle_stream);

        is->dmx_.reset();
    }

    delete is->audio_out;
    delete is->video_out;
    delete is;
}

static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
    for (int i = 0; i < nb_vfilters; i++)
        av_freep(&vfilters_list[i]);
    av_freep(&vfilters_list);
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&input_filename);
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    Rect rect;
    int max_width  = screen_width  ? screen_width  : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    VideoOutput::calc_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

static AVSyncType get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AVSyncType::VideoMaster) {
        if (is->video_st)
            return AVSyncType::VideoMaster;
        else
            return AVSyncType::AudioMaster;
    } else if (is->av_sync_type == AVSyncType::AudioMaster) {
        if (is->audio_st)
            return AVSyncType::AudioMaster;
        else
            return AVSyncType::ExternalClock;
    } else {
        return AVSyncType::ExternalClock;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AVSyncType::VideoMaster:
            val = is->vidclk.get(is->videoq.serial());
            break;
        case AVSyncType::AudioMaster:
            val = is->audclk.get(is->audioq.serial());
            break;
        default:
            val = is->extclk.get(is->extclk.serial());
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
   if (is->video_stream >= 0 && is->videoq.nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES ||
       is->audio_stream >= 0 && is->audioq.nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES) {
       is->extclk.set_speed( FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed() - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES)) {
       is->extclk.set_speed( FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed() + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed();
       if (speed != 1.0)
           is->extclk.set_speed( speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    int flags = by_bytes ? AVSEEK_FLAG_BYTE : 0;
    is->dmx_->seek(pos, rel, flags);
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    bool was_paused = is->dmx_->is_paused();
    if (was_paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated();
        if (is->dmx_->pause_supported()) {
            is->vidclk.set_paused(false);
        }
        is->vidclk.set( is->vidclk.get(is->videoq.serial()), is->vidclk.serial());
    }
    is->extclk.set( is->extclk.get(is->extclk.serial()), is->extclk.serial());
    bool now_paused = !was_paused;
    is->extclk.set_paused(now_paused);
    is->vidclk.set_paused(now_paused);
    is->audclk.set_paused(now_paused);
    is->dmx_->set_paused(now_paused);
}

static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is)
{
    is->audio_out->toggle_mute();
}

static void update_volume(VideoState *is, int sign, double step)
{
    AudioOutput *a = is->audio_out;
    double volume_level = a->volume() ? (20 * log(a->volume() / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    a->set_volume(av_clip(a->volume() == new_volume ? (a->volume() + sign) : new_volume, 0, SDL_MIX_MAXVOLUME));
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step */
    if (is->dmx_->is_paused())
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AVSyncType::VideoMaster) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = is->vidclk.get(is->videoq.serial()) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->dmx_->max_frame_duration()) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->dmx_->max_frame_duration())
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int serial)
{
    /* update current video pts */
    is->vidclk.set( pts, serial);
    sync_clock_to_slave(&is->extclk, is->extclk.serial(), &is->vidclk, is->videoq.serial());
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = static_cast<VideoState*>(opaque);
    double time;

    Frame *sp, *sp2;

    if (!is->dmx_->is_paused() && get_master_sync_type(is) == AVSyncType::ExternalClock && is->dmx_->realtime())
        check_external_clock_speed(is);

    if (!display_disable && is->audio_vis.is_visible() && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            is->video_out->display_audio_vis(&is->audio_vis, is->audio_out,
                                              audio_callback_time, is->dmx_->is_paused());
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (is->pictq.nb_remaining() == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = is->pictq.peek_last();
            vp = is->pictq.peek();

            if (vp->serial != is->videoq.serial()) {
                is->pictq.next();
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->dmx_->is_paused())
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            {
                std::lock_guard lock(is->pictq.mutex_ref());
                if (!isnan(vp->pts))
                    update_video_pts(is, vp->pts, vp->serial);
            }

            if (is->pictq.nb_remaining() > 1) {
                Frame *nextvp = is->pictq.peek_next();
                duration = vp_duration(is, vp, nextvp);
                if(!is->step && (framedrop>0 || (framedrop && get_master_sync_type(is) != AVSyncType::VideoMaster)) && time > is->frame_timer + duration){
                    is->frame_drops_late++;
                    is->pictq.next();
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (is->subpq.nb_remaining() > 0) {
                    sp = is->subpq.peek();

                    if (is->subpq.nb_remaining() > 1)
                        sp2 = is->subpq.peek_next();
                    else
                        sp2 = nullptr;

                    if (sp->serial != is->subtitleq.serial()
                            || (is->vidclk.pts() > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && is->vidclk.pts() > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        is->video_out->clear_subtitle_areas(sp);
                        is->subpq.next();
                    } else {
                        break;
                    }
                }
            }

            is->pictq.next();
            is->force_refresh = 1;

            if (is->step && !is->dmx_->is_paused())
                stream_toggle_pause(is);
        }
display:
        /* display picture */
        if (!display_disable && is->force_refresh && is->audio_vis.mode() == ShowMode::Video && is->pictq.rindex_shown()) {
            if (!is->video_out->width()) {
                int w = screen_width ? screen_width : default_width;
                int h = screen_height ? screen_height : default_height;
                if (!window_title)
                    window_title = input_filename;
                is->video_out->open(w, h, screen_left, screen_top,
                                    window_title, is_full_screen);
                is->video_out->set_layout(Rect{0, 0, w, h});
            }
            Frame *vp = is->pictq.peek_last();
            Frame *sp = is->subtitle_st && is->subpq.nb_remaining() > 0
                            ? is->subpq.peek() : nullptr;
            is->video_out->display(vp, sp);
        }
    }
    is->force_refresh = 0;
    if (show_status) {
        AVBPrint buf;
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size();
            if (is->video_st)
                vqsize = is->videoq.size();
            if (is->subtitle_st)
                sqsize = is->subtitleq.size();
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = is->audclk.get(is->audioq.serial()) - is->vidclk.get(is->videoq.serial());
            else if (is->video_st)
                av_diff = get_master_clock(is) - is->vidclk.get(is->videoq.serial());
            else if (is->audio_st)
                av_diff = get_master_clock(is) - is->audclk.get(is->audioq.serial());

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                      "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
                      get_master_clock(is),
                      (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                      av_diff,
                      is->frame_drops_early + is->frame_drops_late,
                      aqsize / 1024,
                      vqsize / 1024,
                      sqsize);

            if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(nullptr, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, nullptr);

            last_time = cur_time;
        }
    }
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = is->pictq.peek_writable()))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    is->pictq.push();
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = is->viddec.decode_frame(frame, nullptr)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->dmx_->ic(), is->video_st, frame);

        if (framedrop>0 || (framedrop && get_master_sync_type(is) != AVSyncType::VideoMaster)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial() == is->vidclk.serial() &&
                    is->videoq.nb_packets()) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = nullptr;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = nullptr;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, nullptr)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, nullptr);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[32];
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *filt_src = nullptr, *filt_out = nullptr, *last_filter = nullptr;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->dmx_->ic(), is->video_st, nullptr);
    const AVDictionaryEntry *e = nullptr;
    int nb_pix_fmts;
    int i;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par)
        return AVERROR(ENOMEM);

    if (!is->video_out) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    nb_pix_fmts = is->video_out->fill_buffersink_pixel_formats(
        pix_fmts, (int)FF_ARRAY_ELEMS(pix_fmts));
    if (nb_pix_fmts < 0) {
        ret = nb_pix_fmts;
        goto fail;
    }

    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);


    filt_src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                           "ffplay_buffer");
    if (!filt_src) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    par->format              = frame->format;
    par->time_base           = is->video_st->time_base;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
    par->alpha_mode          = frame->alpha_mode;
    par->frame_rate          = fr;
    par->hw_frames_ctx = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(filt_src, par);
    if (ret < 0)
        goto fail;

    ret = avfilter_init_dict(filt_src, nullptr);
    if (ret < 0)
        goto fail;

    filt_out = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                           "ffplay_buffersink");
    if (!filt_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = av_opt_set_array(filt_out, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
        goto fail;
    {
        const auto &cs = is->video_out->supported_color_spaces();
        if ((ret = av_opt_set_array(filt_out, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                    0, (int)cs.size(),
                                    AV_OPT_TYPE_INT, cs.data())) < 0)
            goto fail;
    }

    {
        const auto &am = is->video_out->supported_alpha_modes();
        if ((ret = av_opt_set_array(filt_out, "alphamodes", AV_OPT_SEARCH_CHILDREN,
                                    0, (int)am.size(),
                                    AV_OPT_TYPE_INT, am.data())) < 0)
            goto fail;
    }

    ret = avfilter_init_dict(filt_out, nullptr);
    if (ret < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, nullptr, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (autorotate) {
        double theta = 0.0;
        int32_t *displaymatrix = nullptr;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd)
            displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(is->video_st->codecpar->coded_side_data,
                                                                  is->video_st->codecpar->nb_coded_side_data,
                                                                  AV_PKT_DATA_DISPLAYMATRIX);
            if (psd)
                displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        } else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0)
                INSERT_FILT("hflip", nullptr);
            if (displaymatrix[4] < 0)
                INSERT_FILT("vflip", nullptr);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0)
                INSERT_FILT("vflip", nullptr);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    av_freep(&par);
    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format, AudioParams *audio_tgt)
{
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = nullptr;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   1, is->audio_filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    filt_asink = avfilter_graph_alloc_filter(is->agraph, avfilter_get_by_name("abuffersink"),
                                             "ffplay_abuffersink");
    if (!filt_asink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16", AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT, &audio_tgt->ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates", AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT, &audio_tgt->freq)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, nullptr);
    if (ret < 0)
        goto end;

    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    av_bprint_finalize(&bp, nullptr);

    return ret;
}

static int audio_thread(void *arg)
{
    VideoState *is = static_cast<VideoState*>(arg);
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = is->auddec.decode_frame(frame, nullptr)) < 0)
            goto the_end;

        if (got_frame) {
                tb = AVRational{1, frame->sample_rate};

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                                   static_cast<AVSampleFormat>(frame->format), frame->ch_layout.nb_channels)    ||
                    av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial()               != last_serial;

                if (reconfigure) {
                    char buf1[1024], buf2[1024];
                    av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                    av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                    av_log(nullptr, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)), buf2, is->auddec.pkt_serial());

                    is->audio_filter_src.fmt            = static_cast<AVSampleFormat>(frame->format);
                    ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                    if (ret < 0)
                        goto the_end;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial();

                    if ((ret = configure_audio_filters(is, afilters, 1,
                             const_cast<AudioParams*>(&is->audio_out->hw_params()))) < 0)
                        goto the_end;
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                int64_t *fd = frame->opaque_ref ? (int64_t*)frame->opaque_ref->data : nullptr;
                tb = av_buffersink_get_time_base(is->out_audio_filter);
                if (!(af = is->sampq.peek_writable()))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = fd ? *fd : -1;
                af->serial = is->auddec.pkt_serial();
                af->duration = av_q2d(AVRational{frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                is->sampq.push();

                if (is->audioq.serial() != is->auddec.pkt_serial())
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.set_finished(is->auddec.pkt_serial());
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
    avfilter_graph_free(&is->agraph);
    av_frame_free(&frame);
    return ret;
}

static int video_thread(void *arg)
{
    VideoState *is = static_cast<VideoState*>(arg);
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->dmx_->ic(), is->video_st, nullptr);

    AVFilterGraph *graph = nullptr;
    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = static_cast<AVPixelFormat>(-2);
    int last_serial = -1;
    int last_vfilter_idx = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial()
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(nullptr, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(static_cast<AVPixelFormat>(last_format)), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)), "none"), is->viddec.pkt_serial());
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : nullptr, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = static_cast<AVPixelFormat>(frame->format);
            last_serial = is->viddec.pkt_serial();
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            int64_t *fd;

            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.set_finished(is->viddec.pkt_serial());
                ret = 0;
                break;
            }

            fd = frame->opaque_ref ? (int64_t*)frame->opaque_ref->data : nullptr;

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? *fd : -1, is->viddec.pkt_serial());
            av_frame_unref(frame);
            if (is->videoq.serial() != is->viddec.pkt_serial())
                break;
        }

        if (ret < 0)
            goto the_end;
    }
 the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = static_cast<VideoState*>(arg);
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = is->subpq.peek_writable()))
            return 0;

        if ((got_subtitle = is->subdec.decode_frame(nullptr, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial();
            sp->width = is->subdec.width();
            sp->height = is->subdec.height();
            sp->uploaded = 0;

            /* now we can update the picture count */
            is->subpq.push();
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = static_cast<VideoState*>(opaque);
    audio_callback_time = av_gettime_relative();

    auto next_frame = [&]() -> Frame * {
        Frame *af = nullptr;
        do {
#if defined(_WIN32)
            while (is->sampq.nb_remaining() == 0) {
                if ((av_gettime_relative() - audio_callback_time)
                    > 1000000LL * is->audio_out->hw_buf_size()
                      / is->audio_out->hw_params().bytes_per_sec / 2)
                    return nullptr;
                av_usleep(1000);
            }
#endif
            af = is->sampq.peek_readable();
            if (!af)
                return nullptr;
            is->sampq.next();
        } while (af->serial != is->audioq.serial());
        return af;
    };

    auto sd = [&]() -> double {
        if (get_master_sync_type(is) == AVSyncType::AudioMaster)
            return NAN;
        return is->audclk.get(is->audioq.serial()) - get_master_clock(is);
    };

    std::function<void(const int16_t *, int)> on_decode =
        [&](const int16_t *samples, int count) {
            if (is->audio_vis.mode() != ShowMode::Video)
                is->audio_vis.feed(samples, count);
        };

    is->audio_out->read(stream, len, is->dmx_->is_paused(), next_frame, sd, &on_decode);

    if (!isnan(is->audio_out->clock())) {
        is->audclk.set_at(is->audio_out->clock_for_set_at(),
                          is->audio_out->clock_serial(),
                          audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, is->extclk.serial(),
                            &is->audclk, is->audioq.serial());
    }
}

static int create_hwaccel(AVBufferRef **device_ctx)
{
    enum AVHWDeviceType type;

    *device_ctx = nullptr;

    if (!hwaccel)
        return 0;

    type = av_hwdevice_find_type_by_name(hwaccel);
    if (type == AV_HWDEVICE_TYPE_NONE)
        return AVERROR(ENOTSUP);

    return av_hwdevice_ctx_create(device_ctx, type, nullptr, nullptr, 0);
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->dmx_->ic();
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = nullptr;
    AVDictionary *opts = nullptr;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    AudioParams audio_tgt;
    int ret = 0;
    int stream_lowres = lowres;
    auto wake_cb = [dmx = is->dmx_.get()] { dmx->wake(); };

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type){
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name =    audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name =    video_codec_name; break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(nullptr, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(nullptr, AV_LOG_WARNING,
                                      "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = filter_codec_opts(codec_opts, avctx->codec_id, ic,
                            ic->streams[stream_index], codec, &opts, nullptr);
    if (ret < 0)
        goto fail;

    if (!av_dict_get(opts, "threads", nullptr, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(&avctx->hw_device_ctx);
        if (ret < 0)
            goto fail;
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    ret = check_avoptions(opts);
    if (ret < 0)
        goto fail;

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
            if (ret < 0)
                goto fail;
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, afilters, 0, &audio_tgt)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            ret = av_buffersink_get_ch_layout(sink, &ch_layout);
            if (ret < 0)
                goto fail;
        }

        /* prepare audio output */
        if ((ret = is->audio_out->open(&ch_layout, sample_rate,
                                        sdl_audio_callback, is)) < 0)
            goto fail;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];
        is->dmx_->set_audio_stream(stream_index, is->audio_st);

        if ((ret = is->auddec.init(avctx, &is->audioq, wake_cb, decoder_reorder_pts)) < 0)
            goto fail;
        if (is->dmx_->ic()->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.set_start_pts(is->audio_st->start_time, is->audio_st->time_base);
        }
        if ((ret = is->auddec.start(&is->sampq, audio_thread, "audio_decoder", is)) < 0) {
            is->auddec.release_codec();
            goto out;
        }
        is->audio_out->unpause();
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];
        is->dmx_->set_video_stream(stream_index, is->video_st);

        if ((ret = is->viddec.init(avctx, &is->videoq, wake_cb, decoder_reorder_pts)) < 0)
            goto fail;
        if ((ret = is->viddec.start(&is->pictq, video_thread, "video_decoder", is)) < 0) {
            is->viddec.release_codec();
            goto out;
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];
        is->dmx_->set_subtitle_stream(stream_index, is->subtitle_st);

        if ((ret = is->subdec.init(avctx, &is->subtitleq, wake_cb, decoder_reorder_pts)) < 0)
            goto fail;
        if ((ret = is->subdec.start(&is->subpq, subtitle_thread, "subtitle_decoder", is)) < 0) {
            is->subdec.release_codec();
            goto out;
        }
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    auto *is = static_cast<VideoState*>(ctx);
    return is->dmx_->abort_request();
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request() ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets() > MIN_FRAMES && (!queue->duration() || av_q2d(st->time_base) * queue->duration() > 1.0);
}


static VideoState *stream_open(const char *filename,
                               const AVInputFormat *iformat)
{
    VideoState *is = new VideoState{};
    int ret = -1;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;

    /* start video display */
    if (is->pictq.init(VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (is->subpq.init(SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (is->sampq.init(SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (!is->videoq.valid() || !is->audioq.valid() || !is->subtitleq.valid())
        goto fail;

    is->audio_out  = new AudioOutput();
    is->video_out  = new SDLVideoOutput(renderer, window);
    if (startup_volume < 0)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    if (video_background) {
        if (!strcmp(video_background, "none")) {
            is->video_out->render_params().video_background_type = VideoBackgroundType::None;
        } else if (strcmp(video_background, "tiles")) {
            if (av_parse_color(is->video_out->render_params().video_background_color, video_background, -1, nullptr) >= 0)
                is->video_out->render_params().video_background_type = VideoBackgroundType::Color;
            else
                goto fail;
        }
    }
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_out->set_volume(startup_volume);
    is->av_sync_type = av_sync_type;

    // -- Demuxer setup --
    {
        DemuxerOptions opts;
        opts.find_stream_info = find_stream_info;
        opts.genpts           = genpts;
        opts.seek_by_bytes    = seek_by_bytes;
        opts.start_time       = start_time;
        opts.duration         = duration;
        opts.loop             = loop;
        opts.autoexit         = autoexit;
        opts.infinite_buffer  = infinite_buffer;
        opts.show_status      = show_status;
        opts.video_disable    = video_disable;
        opts.audio_disable    = audio_disable;
        opts.subtitle_disable = subtitle_disable;
        for (int i = 0; i < AVMEDIA_TYPE_NB; i++)
            opts.wanted_stream_spec[i] = wanted_stream_spec[i];

        is->dmx_ = std::make_unique<Demuxer>(filename, iformat, opts);
    }

    if (is->dmx_->init(&format_opts, codec_opts) < 0)
        goto fail;
    // format_opts is now nullptr (ownership transferred).
    // Demuxer::init resolves seek_by_bytes when it was -1 (auto); keep the
    // global in sync for UI / options display. Use seek_by_bytes > 0 in hot
    // paths so -1 is never treated as byte-seek.
    seek_by_bytes = is->dmx_->seek_by_bytes();

    // Set window title from metadata if not explicitly set.
    if (!window_title) {
        const AVDictionaryEntry *t =
            av_dict_get(is->dmx_->ic()->metadata, "title", nullptr, 0);
        if (t)
            window_title = av_asprintf("%s - %s", t->value, input_filename);
    }

    // Set initial show_mode and default window size.
    is->audio_vis.set_mode(show_mode);
    if (is->dmx_->video_index() >= 0) {
        AVStream *st = is->dmx_->ic()->streams[is->dmx_->video_index()];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(is->dmx_->ic(), st, nullptr);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }

    // Open streams.
    if (is->dmx_->audio_index() >= 0)
        stream_component_open(is, is->dmx_->audio_index());

    ret = -1;
    if (is->dmx_->video_index() >= 0)
        ret = stream_component_open(is, is->dmx_->video_index());

    if (is->audio_vis.mode() == ShowMode::None)
        is->audio_vis.set_mode(ret >= 0 ? ShowMode::Video : ShowMode::Rdft);
    show_mode = is->audio_vis.mode();

    if (is->dmx_->subtitle_index() >= 0)
        stream_component_open(is, is->dmx_->subtitle_index());

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               filename);
        goto fail;
    }

    // -- Wire callbacks --
    is->dmx_->set_packet_handler([is](AVPacket *pkt, int stream_index) {
        if (!pkt) {
            // null packet → flush signal
            if (stream_index == is->video_stream && is->video_stream >= 0)
                is->videoq.put_null_packet(stream_index);
            else if (stream_index == is->audio_stream && is->audio_stream >= 0)
                is->audioq.put_null_packet(stream_index);
            else if (stream_index == is->subtitle_stream && is->subtitle_stream >= 0)
                is->subtitleq.put_null_packet(stream_index);
            return;
        }
        if (stream_index == is->audio_stream)
            is->audioq.put(pkt);
        else if (stream_index == is->video_stream &&
                 !(is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            is->videoq.put(pkt);
        else if (stream_index == is->subtitle_stream)
            is->subtitleq.put(pkt);
        else
            av_packet_unref(pkt);
    });

    is->dmx_->set_seek_done_handler([is](int64_t target, int flags) {
        if (is->audio_stream >= 0)
            is->audioq.flush();
        if (is->subtitle_stream >= 0)
            is->subtitleq.flush();
        if (is->video_stream >= 0)
            is->videoq.flush();
        if (flags & AVSEEK_FLAG_BYTE)
            is->extclk.set(NAN, 0);
        else
            is->extclk.set(target / (double)AV_TIME_BASE, 0);
    });

    is->dmx_->set_seek_complete_handler([is]() {
        if (is->dmx_->is_paused())
            step_to_next_frame(is);
    });

    is->dmx_->set_queues_full_handler([is]() -> bool {
        if (is->dmx_->infinite_buffer() >= 1)
            return false;
        if (is->audioq.size() + is->videoq.size() + is->subtitleq.size() > MAX_QUEUE_SIZE)
            return true;
        return stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
               stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
               stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq);
    });

    is->dmx_->set_decoders_done_handler([is]() -> bool {
        if (is->dmx_->is_paused())
            return false;
        bool audio_done = !is->audio_st ||
            (is->auddec.finished() == is->audioq.serial() && is->sampq.nb_remaining() == 0);
        bool video_done = !is->video_st ||
            (is->viddec.finished() == is->videoq.serial() && is->pictq.nb_remaining() == 0);
        return audio_done && video_done;
    });

    is->dmx_->set_read_fatal_handler([is]() {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    });

    is->dmx_->start();
    return is;

fail:
    stream_close(is);
    return nullptr;
}

static void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->dmx_->ic();
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = nullptr;
    int nb_streams = is->dmx_->ic()->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, nullptr, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->dmx_->ic()->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
 the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(nullptr, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(static_cast<AVMediaType>(codec_type)),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}


static void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *is)
{
    ShowMode next = AudioVisualizer::cycle_mode(
        is->audio_vis.mode(),
        is->video_st != nullptr,
        is->audio_st != nullptr);
    if (is->audio_vis.mode() != next) {
        is->force_refresh = 1;
        is->audio_vis.set_mode(next);
        show_mode = next;
    }
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->audio_vis.mode() != ShowMode::None && (!is->dmx_->is_paused() || is->force_refresh))
            video_refresh(is, &remaining_time);
        SDL_PumpEvents();
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->dmx_->ic()->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->dmx_->ic()->nb_chapters; i++) {
        AVChapter *ch = is->dmx_->ic()->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->dmx_->ic()->nb_chapters)
        return;

    av_log(nullptr, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->dmx_->ic()->chapters[i]->start, is->dmx_->ic()->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        refresh_loop_wait_event(cur_stream, &event);
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                do_exit(cur_stream);
                break;
            }
            // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
            if (!cur_stream->video_out || !cur_stream->video_out->width())
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_p:
            case SDLK_SPACE:
                toggle_pause(cur_stream);
                break;
            case SDLK_m:
                toggle_mute(cur_stream);
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // S: Step to next frame
                step_to_next_frame(cur_stream);
                break;
            case SDLK_a:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (cur_stream->audio_vis.mode() == ShowMode::Video && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters)
                        cur_stream->vfilter_idx = 0;
                } else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
                break;
            case SDLK_PAGEUP:
                if (cur_stream->dmx_->ic()->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->dmx_->ic()->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                    if (seek_by_bytes > 0) {
                        pos = -1;
                        if (pos < 0 && cur_stream->video_stream >= 0)
                            pos = cur_stream->pictq.last_pos(cur_stream->videoq.serial());
                        if (pos < 0 && cur_stream->audio_stream >= 0)
                            pos = cur_stream->sampq.last_pos(cur_stream->audioq.serial());
                        if (pos < 0)
                            pos = avio_tell(cur_stream->dmx_->ic()->pb);
                        if (cur_stream->dmx_->ic()->bit_rate)
                            incr *= cur_stream->dmx_->ic()->bit_rate / 8.0;
                        else
                            incr *= 180000.0;
                        pos += incr;
                        stream_seek(cur_stream, pos, incr, 1);
                    } else {
                        pos = get_master_clock(cur_stream);
                        if (isnan(pos))
                            pos = 0.0;
                        pos += incr;
                        if (cur_stream->dmx_->ic()->start_time != AV_NOPTS_VALUE && pos < cur_stream->dmx_->ic()->start_time / (double)AV_TIME_BASE)
                            pos = cur_stream->dmx_->ic()->start_time / (double)AV_TIME_BASE;
                        stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                    }
                break;
            default:
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    toggle_full_screen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))
                    break;
                x = event.motion.x;
            }
                if (seek_by_bytes > 0 || cur_stream->dmx_->ic()->duration <= 0) {
                    uint64_t size =  avio_size(cur_stream->dmx_->ic()->pb);
                    stream_seek(cur_stream, size*x/cur_stream->video_out->width(), 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns  = cur_stream->dmx_->ic()->duration / 1000000LL;
                    thh  = tns / 3600;
                    tmm  = (tns % 3600) / 60;
                    tss  = (tns % 60);
                    frac = x / cur_stream->video_out->width();
                    ns   = frac * tns;
                    hh   = ns / 3600;
                    mm   = (ns % 3600) / 60;
                    ss   = (ns % 60);
                    av_log(nullptr, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                            hh, mm, ss, thh, tmm, tss);
                    ts = frac * cur_stream->dmx_->ic()->duration;
                    if (cur_stream->dmx_->ic()->start_time != AV_NOPTS_VALUE)
                        ts += cur_stream->dmx_->ic()->start_time;
                    stream_seek(cur_stream, ts, 0, 0);
                }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    screen_width  = event.window.data1;
                    screen_height = event.window.data2;
                    if (cur_stream->video_out)
                        cur_stream->video_out->set_layout(
                            Rect{0, 0, event.window.data1, event.window.data2});
                    cur_stream->force_refresh = 1;
                    break;
                case SDL_WINDOWEVENT_EXPOSED:
                    cur_stream->force_refresh = 1;
                    break;
                default:
                    break;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit(cur_stream);
            break;
        default:
            break;
        }
    }
}

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    screen_width = num;
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg)
{
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    screen_height = num;
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(nullptr, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_sync(void *optctx, const char *opt, const char *arg)
{
    if (!strcmp(arg, "audio"))
        av_sync_type = AVSyncType::AudioMaster;
    else if (!strcmp(arg, "video"))
        av_sync_type = AVSyncType::VideoMaster;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AVSyncType::ExternalClock;
    else {
        av_log(nullptr, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
    show_mode = !strcmp(arg, "video") ? ShowMode::Video :
                !strcmp(arg, "waves") ? ShowMode::Waves :
                !strcmp(arg, "rdft" ) ? ShowMode::Rdft  : ShowMode::None;

    if (show_mode == ShowMode::None) {
        double num;
        int ret = parse_number(opt, arg, OPT_TYPE_INT, 0, static_cast<int>(ShowMode::Nb)-1, &num);
        if (ret < 0)
            return ret;
        show_mode = static_cast<ShowMode>(num);
    }
    return 0;
}

static int opt_input_file(void *optctx, const char *filename)
{
    if (input_filename) {
        av_log(nullptr, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
                filename, input_filename);
        return AVERROR(EINVAL);
    }
    if (!strcmp(filename, "-"))
        filename = "fd:";
    input_filename = av_strdup(filename);
    if (!input_filename)
        return AVERROR(ENOMEM);

    return 0;
}

static int opt_codec(void *optctx, const char *opt, const char *arg)
{
   const char *spec = strchr(opt, ':');
   const char **name;
   if (!spec) {
       av_log(nullptr, AV_LOG_ERROR,
              "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
       return AVERROR(EINVAL);
   }
   spec++;

   switch (spec[0]) {
   case 'a' : name = &audio_codec_name;    break;
   case 's' : name = &subtitle_codec_name; break;
   case 'v' : name = &video_codec_name;    break;
   default:
       av_log(nullptr, AV_LOG_ERROR,
              "Invalid media specifier '%s' in option '%s'\n", spec, opt);
       return AVERROR(EINVAL);
   }

   av_freep(name);
   *name = av_strdup(arg);
   return *name ? 0 : AVERROR(ENOMEM);
}

static int dummy;

static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "x",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
    { "y",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
    { "fs",                 OPT_TYPE_BOOL,            0, { &is_full_screen }, "force full screen" },
    { "an",                 OPT_TYPE_BOOL,            0, { &audio_disable }, "disable audio" },
    { "vn",                 OPT_TYPE_BOOL,            0, { &video_disable }, "disable video" },
    { "sn",                 OPT_TYPE_BOOL,            0, { &subtitle_disable }, "disable subtitling" },
    { "ast",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
    { "vst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
    { "sst",                OPT_TYPE_STRING, OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
    { "ss",                 OPT_TYPE_TIME,            0, { &start_time }, "seek to a given position in seconds", "pos" },
    { "t",                  OPT_TYPE_TIME,            0, { &duration }, "play  \"duration\" seconds of audio/video", "duration" },
    { "bytes",              OPT_TYPE_INT,             0, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
    { "seek_interval",      OPT_TYPE_FLOAT,           0, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
    { "nodisp",             OPT_TYPE_BOOL,            0, { &display_disable }, "disable graphical display" },
    { "noborder",           OPT_TYPE_BOOL,            0, { &borderless }, "borderless window" },
    { "alwaysontop",        OPT_TYPE_BOOL,            0, { &alwaysontop }, "window always on top" },
    { "volume",             OPT_TYPE_INT,             0, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },
    { "f",                  OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_format }, "force format", "fmt" },
    { "stats",              OPT_TYPE_BOOL,   OPT_EXPERT, { &show_status }, "show status", "" },
    { "fast",               OPT_TYPE_BOOL,   OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },
    { "genpts",             OPT_TYPE_BOOL,   OPT_EXPERT, { &genpts }, "generate pts", "" },
    { "drp",                OPT_TYPE_INT,    OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres",             OPT_TYPE_INT,    OPT_EXPERT, { &lowres }, "", "" },
    { "sync",               OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit",           OPT_TYPE_BOOL,   OPT_EXPERT, { &autoexit }, "exit at the end", "" },
    { "exitonkeydown",      OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
    { "exitonmousedown",    OPT_TYPE_BOOL,   OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
    { "loop",               OPT_TYPE_INT,    OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
    { "framedrop",          OPT_TYPE_BOOL,   OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
    { "infbuf",             OPT_TYPE_BOOL,   OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
    { "window_title",       OPT_TYPE_STRING,          0, { &window_title }, "set window title", "window title" },
    { "left",               OPT_TYPE_INT,    OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
    { "top",                OPT_TYPE_INT,    OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
    { "vf",                 OPT_TYPE_FUNC, OPT_FUNC_ARG | OPT_EXPERT, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af",                 OPT_TYPE_STRING,          0, { &afilters }, "set audio filters", "filter_graph" },
    { "rdftspeed",          OPT_TYPE_INT, OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
    { "showmode",           OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "i",                  OPT_TYPE_BOOL,            0, { &dummy}, "read specified file", "input_file"},
    { "codec",              OPT_TYPE_FUNC, OPT_FUNC_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
    { "acodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
    { "scodec",             OPT_TYPE_STRING, OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
    { "vcodec",             OPT_TYPE_STRING, OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
    { "autorotate",         OPT_TYPE_BOOL,            0, { &autorotate }, "automatically rotate video", "" },
    { "find_stream_info",   OPT_TYPE_BOOL, OPT_INPUT | OPT_EXPERT, { &find_stream_info },
        "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads",     OPT_TYPE_INT,    OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },
    { "video_bg",           OPT_TYPE_STRING, OPT_EXPERT, { &video_background }, "set video background for transparent videos" },
    { "hwaccel",            OPT_TYPE_STRING, OPT_EXPERT, { &hwaccel }, "use HW accelerated decoding" },
    { nullptr, },
};

static void show_usage(void)
{
    av_log(nullptr, AV_LOG_INFO, "Simple media player\n");
    av_log(nullptr, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(nullptr, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward by 10 seconds or a custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek to previous/next chapter or backward/forward 10 minutes if no chapters\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
           );
}

/* Called from the main */
int main(int argc, char **argv)
{
    int flags, ret;
    VideoState *is;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);

    ret = parse_options(nullptr, argc, argv, options, opt_input_file);
    if (ret < 0)
        exit(ret == AVERROR_EXIT ? 0 : 1);

    if (!input_filename) {
        show_usage();
        av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(nullptr, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    if (display_disable) {
        video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (display_disable)
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init (flags)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    if (!display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2,0,5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(nullptr, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (!window) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
            do_exit(nullptr);
        }

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer) {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
        if (!renderer || !renderer_info.num_texture_formats) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(nullptr);
        }
    }

    is = stream_open(input_filename, file_iformat);
    if (!is) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(nullptr);
    }

    event_loop(is);

    /* never returns */

    return 0;
}
