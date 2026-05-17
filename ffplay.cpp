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

#include "audio_output.h"
#include "audio_visualizer.h"
#include "avsync_type.h"
#include "clock.h"
#include "decoder.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "video_output.h"
#include "sdl_video_output.h"
#include "packet_queue.h"
#include "player.h"
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

/* options specified by the user */
static const AVInputFormat *file_iformat;
const char *input_filename;
const char *window_title;
       int default_width  = 640;
       int default_height = 480;
       int screen_width   = 0;
       int screen_height  = 0;
       int screen_left = SDL_WINDOWPOS_CENTERED;
       int screen_top = SDL_WINDOWPOS_CENTERED;
       int audio_disable;
       int video_disable;
       int subtitle_disable;
       const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
       int seek_by_bytes = -1;
static float seek_interval = 10;
       int display_disable;
static int borderless;
static int alwaysontop;
       int startup_volume = 100;
       int show_status = -1;
       AVSyncType av_sync_type = AVSyncType::AudioMaster;
       int64_t start_time = AV_NOPTS_VALUE;
       int64_t duration = AV_NOPTS_VALUE;
       int fast = 0;
       int genpts = 0;
       int lowres = 0;
       int decoder_reorder_pts = -1;
       int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
       int loop = 1;
       int framedrop = -1;
       int infinite_buffer = -1;
       ShowMode show_mode = ShowMode::None;
       const char *audio_codec_name;
       const char *subtitle_codec_name;
       const char *video_codec_name;
       double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
       const char **vfilters_list = nullptr;
       int nb_vfilters = 0;
       char *afilters = nullptr;
       int autorotate = 1;
       int find_stream_info = 1;
       int filter_nbthreads = 0;
static char *video_background = nullptr;
       const char *hwaccel = nullptr;

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

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    double num;
    int ret = parse_number(opt, arg, OPT_TYPE_INT64, 1, INT_MAX, &num);
    if (ret < 0)
        return ret;

    screen_width = num;
    return 0;
}

static void sigterm_handler(int sig)
{
    exit(123);
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

// =========================================================================
//  Player-based helpers (new code path)
// =========================================================================

static void do_exit_player(Player *p)
{
    if (p) delete p;
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    uninit_opts();
    for (int i = 0; i < nb_vfilters; i++)
        av_freep(&vfilters_list[i]);
    av_freep(&vfilters_list);
    av_freep(&video_codec_name);
    av_freep(&audio_codec_name);
    av_freep(&subtitle_codec_name);
    av_freep(&input_filename);
    avformat_network_deinit();
    if (show_status) printf("\n");
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void refresh_loop_wait_player(Player *p, SDL_Event *event)
{
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
        if (p->audioVis().mode() != ShowMode::None &&
            (!p->isPaused() || p->forceRefreshRef()))
            p->videoRefresh(&remaining_time);
        SDL_PumpEvents();
    }
}

static void seek_chapter_player(Player *p, int incr)
{
    if (!p || !p->dmx() || !p->dmx()->ic()->nb_chapters) return;
    double pos_d = p->currentPosition();
    int64_t pos = (int64_t)(pos_d * AV_TIME_BASE);
    int i;
    for (i = 0; i < p->dmx()->ic()->nb_chapters; i++) {
        AVChapter *ch = p->dmx()->ic()->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }
    i += incr;
    i = FFMAX(i, 0);
    if (i >= p->dmx()->ic()->nb_chapters) return;
    av_log(nullptr, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    p->seekTo(av_rescale_q(p->dmx()->ic()->chapters[i]->start,
                           p->dmx()->ic()->chapters[i]->time_base,
                           AV_TIME_BASE_Q) / (double)AV_TIME_BASE);
}

static void event_loop_player(Player *p)
{
    SDL_Event event;
    double incr;

    for (;;) {
        double x;
        refresh_loop_wait_player(p, &event);
        switch (event.type) {
        case SDL_KEYDOWN:
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE ||
                event.key.keysym.sym == SDLK_q) {
                do_exit_player(p);
                break;
            }
            if (!p->videoDevice() || !p->videoDevice()->width())
                continue;
            switch (event.key.keysym.sym) {
            case SDLK_f:
                is_full_screen = !is_full_screen;
                SDL_SetWindowFullscreen(window, is_full_screen
                    ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                p->forceRefresh();
                break;
            case SDLK_p: case SDLK_SPACE:
                p->pause();
                break;
            case SDLK_m:
                p->toggleMute();
                break;
            case SDLK_KP_MULTIPLY: case SDLK_0:
                {
                    auto *a = dynamic_cast<AudioOutput*>(p->audioDevice());
                    if (!a) break;
                    double vol_lvl = a->volume()
                        ? (20 * log(a->volume() / (double)SDL_MIX_MAXVOLUME) / log(10))
                        : -1000.0;
                    int nv = lrint(SDL_MIX_MAXVOLUME
                        * pow(10.0, (vol_lvl + SDL_VOLUME_STEP) / 20.0));
                    a->set_volume(
                        av_clip(a->volume() == nv ? (a->volume() + 1) : nv,
                                0, SDL_MIX_MAXVOLUME));
                }
                break;
            case SDLK_KP_DIVIDE: case SDLK_9:
                {
                    auto *a = dynamic_cast<AudioOutput*>(p->audioDevice());
                    if (!a) break;
                    double vol_lvl = a->volume()
                        ? (20 * log(a->volume() / (double)SDL_MIX_MAXVOLUME) / log(10))
                        : -1000.0;
                    int nv = lrint(SDL_MIX_MAXVOLUME
                        * pow(10.0, (vol_lvl - SDL_VOLUME_STEP) / 20.0));
                    a->set_volume(
                        av_clip(a->volume() == nv ? (a->volume() - 1) : nv,
                                0, SDL_MIX_MAXVOLUME));
                }
                break;
            case SDLK_s:
                p->stepToNextFrame();
                break;
            case SDLK_a:
                p->cycleChannel(AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                p->cycleChannel(AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                p->cycleChannel(AVMEDIA_TYPE_VIDEO);
                p->cycleChannel(AVMEDIA_TYPE_AUDIO);
                p->cycleChannel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                p->cycleChannel(AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
                if (p->audioVis().mode() == ShowMode::Video &&
                    p->videoFilterIndex() < nb_vfilters - 1) {
                    p->setVideoFilter(p->videoFilterIndex() + 1);
                } else {
                    p->setVideoFilter(0);
                    p->toggleAudioDisplay();
                }
                break;
            case SDLK_PAGEUP:
                if (p->dmx()->ic()->nb_chapters <= 1) {
                    incr = 600.0; goto do_seek_p;
                }
                seek_chapter_player(p, 1);
                break;
            case SDLK_PAGEDOWN:
                if (p->dmx()->ic()->nb_chapters <= 1) {
                    incr = -600.0; goto do_seek_p;
                }
                seek_chapter_player(p, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -10.0; goto do_seek_p;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0; goto do_seek_p;
            case SDLK_UP:
                incr = 60.0; goto do_seek_p;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek_p:
                if (seek_by_bytes > 0) {
                    int64_t bpos = -1;
                    if (bpos < 0)
                        bpos = p->pictq().last_pos(p->videoq().serial());
                    if (bpos < 0)
                        bpos = p->sampq().last_pos(p->audioq().serial());
                    if (bpos < 0)
                        bpos = avio_tell(p->dmx()->ic()->pb);
                    if (p->dmx()->ic()->bit_rate)
                        incr *= p->dmx()->ic()->bit_rate / 8.0;
                    else
                        incr *= 180000.0;
                    bpos += (int64_t)incr;
                    p->dmx()->seek(bpos, (int64_t)incr, AVSEEK_FLAG_BYTE);
                } else {
                    double pos_p = p->currentPosition();
                    if (isnan(pos_p))
                        pos_p = (double)p->dmx()->seek_target_units() / AV_TIME_BASE;
                    pos_p += incr;
                    AVFormatContext *ic = p->dmx()->ic();
                    if (ic->start_time != AV_NOPTS_VALUE &&
                        pos_p < ic->start_time / (double)AV_TIME_BASE)
                        pos_p = ic->start_time / (double)AV_TIME_BASE;
                    p->seekTo(pos_p, incr);
                }
                break;
            default: break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (exit_on_mousedown) { do_exit_player(p); break; }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_click = 0;
                if (av_gettime_relative() - last_click <= 500000) {
                    is_full_screen = !is_full_screen;
                    SDL_SetWindowFullscreen(window, is_full_screen
                        ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    p->forceRefresh();
                    last_click = 0;
                } else {
                    last_click = av_gettime_relative();
                }
            }
        case SDL_MOUSEMOTION:
            if (cursor_hidden) { SDL_ShowCursor(1); cursor_hidden = 0; }
            cursor_last_shown = av_gettime_relative();
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT) break;
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK)) break;
                x = event.motion.x;
            }
            if (!p->videoDevice()) break;
            if (seek_by_bytes > 0 || p->dmx()->ic()->duration <= 0) {
                uint64_t size = avio_size(p->dmx()->ic()->pb);
                p->dmx()->seek((int64_t)(size * x / p->videoDevice()->width()),
                               0, AVSEEK_FLAG_BYTE);
            } else {
                AVFormatContext *ic = p->dmx()->ic();
                double frac = x / (double)p->videoDevice()->width();
                int64_t tns = ic->duration / 1000000LL;
                int thh = (int)(tns / 3600);
                int tmm = (int)((tns % 3600) / 60);
                int tss = (int)(tns % 60);
                int ns = (int)(frac * tns);
                int hh = ns / 3600;
                int mm = (ns % 3600) / 60;
                int ss = ns % 60;
                av_log(nullptr, AV_LOG_INFO,
                       "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n",
                       frac * 100, hh, mm, ss, thh, tmm, tss);
                int64_t ts = (int64_t)(frac * ic->duration);
                if (ic->start_time != AV_NOPTS_VALUE)
                    ts += ic->start_time;
                p->seekTo(ts / (double)AV_TIME_BASE);
            }
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                screen_width  = event.window.data1;
                screen_height = event.window.data2;
                if (p->videoDevice())
                    p->videoDevice()->set_layout(
                        Rect{0, 0, event.window.data1, event.window.data2});
                p->forceRefresh();
                break;
            case SDL_WINDOWEVENT_EXPOSED:
                p->forceRefresh();
                break;
            default: break;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit_player(p);
            break;
        default: break;
        }
    }
}

/* Called from the main */
int main(int argc, char **argv)
{
    int flags;
    Player *p = nullptr;

    init_dynload();

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    signal(SIGINT , sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    show_banner(argc, argv, options);

    int ret = parse_options(nullptr, argc, argv, options, opt_input_file);
    if (ret < 0)
        exit(ret == AVERROR_EXIT ? 0 : 1);

    if (!input_filename) {
        show_usage();
        av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(nullptr, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    if (display_disable) video_disable = 1;

    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)  flags &= ~SDL_INIT_AUDIO;
    else {
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (display_disable) flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init(flags)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    if (!display_disable) {
        int wflags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2,0,5)
            wflags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(nullptr, AV_LOG_WARNING,
                   "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP.\n");
#endif
        if (borderless) wflags |= SDL_WINDOW_BORDERLESS;
        else            wflags |= SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
        window = SDL_CreateWindow(program_name,
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            default_width, default_height, wflags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (!window) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to create window: %s", SDL_GetError());
            do_exit_player(nullptr);
        }
        renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            av_log(nullptr, AV_LOG_WARNING,
                   "Failed to initialize a hardware accelerated renderer: %s\n",
                   SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer) {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(nullptr, AV_LOG_VERBOSE,
                       "Initialized %s renderer.\n", renderer_info.name);
        }
        if (!renderer || !renderer_info.num_texture_formats) {
            av_log(nullptr, AV_LOG_FATAL,
                   "Failed to create window or renderer: %s", SDL_GetError());
            do_exit_player(nullptr);
        }
    }

    // -- Player path --
    p = new Player();
    p->setDataSource(input_filename, file_iformat);

    auto *video_dev = new SDLVideoOutput(renderer, window);
    auto *audio_dev = new AudioOutput();

    if (display_disable)
        p->setVideoDevice(nullptr);
    else
        p->setVideoDevice(video_dev);

    if (audio_disable)
        p->setAudioDevice(nullptr);
    else {
        if (startup_volume < 0 || startup_volume > 100)
            av_log(nullptr, AV_LOG_WARNING,
                   "-volume=%d out of range, clamping\n", startup_volume);
        int vol = av_clip(startup_volume, 0, 100);
        vol = av_clip(SDL_MIX_MAXVOLUME * vol / 100, 0, SDL_MIX_MAXVOLUME);
        audio_dev->set_volume(vol);
        p->setAudioDevice(audio_dev);
    }

    // Audio visualizer
    p->audioVis().set_mode(show_mode);

    if (p->prepare() < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to prepare player\n");
        delete audio_dev;
        delete video_dev;
        delete p;
        do_exit_player(nullptr);
    }

    p->start();

    event_loop_player(p);

    /* never returns */
    return 0;
}
