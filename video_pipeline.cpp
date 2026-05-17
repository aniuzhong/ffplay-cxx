#include "video_pipeline.h"

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

#include "clock.h"
#include "decoder.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "player.h"
#include "video_output.h"



// v1 globals — v2 moves to PlayerConfig
extern int framedrop;
extern int filter_nbthreads;
extern int autorotate;
extern const char **vfilters_list;
extern int nb_vfilters;
extern "C" AVDictionary *sws_dict;    // defined in utils/cmdutils.c (C linkage)
extern int screen_width;
extern int screen_height;
extern int default_width;
extern int default_height;

static double get_rotation(const int32_t *displaymatrix);

VideoPipeline::~VideoPipeline()
{
    abort();
}

int VideoPipeline::init(AVCodecContext *avctx, PacketQueue *videoq,
                         FrameQueue *pictq, Demuxer *dmx,
                         AVStream *video_st, int reorder_pts)
{
    if (!avctx || !videoq || !pictq || !dmx || !video_st)
        return AVERROR(EINVAL);

    stream       = video_st;
    this->pktq   = videoq;
    this->pictq  = pictq;
    dmx_         = dmx;

    return decoder.init(avctx, videoq, [dmx] { dmx->wake(); }, reorder_pts);
}

void VideoPipeline::start(Player *player)
{
    player_ = player;
    pktq->start();
    pictq->start();
    thread_ = std::thread(&VideoPipeline::run, this);
}

void VideoPipeline::abort()
{
    if (pktq) pktq->abort();
    if (pictq) pictq->abort();
    if (thread_.joinable())
        thread_.join();
    if (pktq) pktq->flush();
}

void VideoPipeline::releaseCodec()
{
    abort();
    decoder.release_codec();
    avfilter_graph_free(&graph);
    in_filter  = nullptr;
    out_filter = nullptr;
    player_    = nullptr;
    dmx_       = nullptr;
    pktq       = nullptr;
    pictq      = nullptr;
    stream     = nullptr;
}

// ==========================================================================
//  Static helpers: configure_video_filters, get_video_frame, queue_picture,
//  configure_filtergraph, set_default_window_size
// ==========================================================================

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) { ret = AVERROR(ENOMEM); goto fail; }

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

    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext *, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, nullptr);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph,
                                    VideoOutput *vout,
                                    AVFormatContext *ic,
                                    AVStream *vst,
                                    AVFrame *frame,
                                    int vfilter_idx,
                                    AVFilterContext **filt_src,
                                    AVFilterContext **filt_out)
{
    enum AVPixelFormat pix_fmts[32];
    char sws_flags_str[512] = "";
    int ret;
    AVFilterContext *last_filter = nullptr;
    AVFilterContext *fsrc = nullptr;
    AVFilterContext *fout = nullptr;
    AVCodecParameters *codecpar = vst->codecpar;
    AVRational fr = av_guess_frame_rate(ic, vst, nullptr);
    const AVDictionaryEntry *e = nullptr;
    int nb_pix_fmts;
    int i;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    if (!par) return AVERROR(ENOMEM);
    if (!vout) { ret = AVERROR(EINVAL); goto fail; }

    nb_pix_fmts = vout->fill_buffersink_pixel_formats(
        pix_fmts, (int)FF_ARRAY_ELEMS(pix_fmts));
    if (nb_pix_fmts < 0) { ret = nb_pix_fmts; goto fail; }

    while ((e = av_dict_iterate(sws_dict, e))) {
        if (!strcmp(e->key, "sws_flags"))
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    fsrc = avfilter_graph_alloc_filter(graph,
        avfilter_get_by_name("buffer"), "ffplay_buffer");
    if (!fsrc) { ret = AVERROR(ENOMEM); goto fail; }

    par->format              = frame->format;
    par->time_base           = vst->time_base;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = codecpar->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
#if LIBAVFILTER_VERSION_MAJOR >= 12
    par->alpha_mode          = frame->alpha_mode;
#endif
    par->frame_rate          = fr;
    par->hw_frames_ctx       = frame->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(fsrc, par);
    if (ret < 0) goto fail;
    ret = avfilter_init_dict(fsrc, nullptr);
    if (ret < 0) goto fail;

    fout = avfilter_graph_alloc_filter(graph,
        avfilter_get_by_name("buffersink"), "ffplay_buffersink");
    if (!fout) { ret = AVERROR(ENOMEM); goto fail; }

    if ((ret = av_opt_set_array(fout, "pixel_formats", AV_OPT_SEARCH_CHILDREN,
                                0, nb_pix_fmts, AV_OPT_TYPE_PIXEL_FMT, pix_fmts)) < 0)
        goto fail;
    {
        const auto &cs = vout->supported_color_spaces();
        if ((ret = av_opt_set_array(fout, "colorspaces", AV_OPT_SEARCH_CHILDREN,
                                    0, (int)cs.size(),
                                    AV_OPT_TYPE_INT, cs.data())) < 0)
            goto fail;
    }
#if LIBAVUTIL_VERSION_MAJOR >= 61
    {
        const auto &am = vout->supported_alpha_modes();
        if ((ret = av_opt_set_array(fout, "alphamodes", AV_OPT_SEARCH_CHILDREN,
                                    0, (int)am.size(),
                                    AV_OPT_TYPE_INT, am.data())) < 0)
            goto fail;
    }
#endif

    ret = avfilter_init_dict(fout, nullptr);
    if (ret < 0) goto fail;

    last_filter = fout;

#define INSERT_FILT(name_, arg_) do {                               \
    AVFilterContext *filt_ctx;                                        \
    ret = avfilter_graph_create_filter(&filt_ctx,                     \
        avfilter_get_by_name(name_), "ffplay_" name_, arg_,           \
        nullptr, graph);                                               \
    if (ret < 0) goto fail;                                           \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                \
    if (ret < 0) goto fail;                                           \
    last_filter = filt_ctx;                                            \
} while (0)

    if (autorotate) {
        double theta = 0.0;
        int32_t *displaymatrix = nullptr;
        AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
        if (sd) displaymatrix = (int32_t *)sd->data;
        if (!displaymatrix) {
            const AVPacketSideData *psd = av_packet_side_data_get(
                vst->codecpar->coded_side_data,
                vst->codecpar->nb_coded_side_data,
                AV_PKT_DATA_DISPLAYMATRIX);
            if (psd) displaymatrix = (int32_t *)psd->data;
        }
        theta = get_rotation(displaymatrix);

        if (fabs(theta - 90) < 1.0)
            INSERT_FILT("transpose", displaymatrix[3] > 0 ? "cclock_flip" : "clock");
        else if (fabs(theta - 180) < 1.0) {
            if (displaymatrix[0] < 0) INSERT_FILT("hflip", nullptr);
            if (displaymatrix[4] < 0) INSERT_FILT("vflip", nullptr);
        } else if (fabs(theta - 270) < 1.0)
            INSERT_FILT("transpose", displaymatrix[3] < 0 ? "clock_flip" : "cclock");
        else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        } else {
            if (displaymatrix && displaymatrix[4] < 0)
                INSERT_FILT("vflip", nullptr);
        }
    }

    if ((ret = configure_filtergraph(graph,
            vfilters_list ? vfilters_list[vfilter_idx] : nullptr,
            fsrc, last_filter)) < 0)
        goto fail;

    *filt_src = fsrc;
    *filt_out = fout;

fail:
    av_freep(&par);
    return ret;
}

static void set_default_window_size(int width, int height, AVRational sar)
{
    Rect rect;
    int max_w = screen_width  ? screen_width  : INT_MAX;
    int max_h = screen_height ? screen_height : INT_MAX;
    if (max_w == INT_MAX && max_h == INT_MAX)
        max_h = height;
    VideoOutput::calc_display_rect(&rect, 0, 0, max_w, max_h, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

static int queue_picture(VideoOutput *vout, FrameQueue *pictq,
                         AVFrame *src_frame, double pts, double duration,
                         int64_t pos, int serial)
{
    Frame *vp;

    if (!(vp = pictq->peek_writable()))
        return -1;

    vp->sar      = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;
    vp->width    = src_frame->width;
    vp->height   = src_frame->height;
    vp->format   = src_frame->format;
    vp->pts      = pts;
    vp->duration = duration;
    vp->pos      = pos;
    vp->serial   = serial;

    if (vout)
        set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    pictq->push();
    return 0;
}

static int get_video_frame(VideoPipeline *vp, Player *p, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = vp->decoder.decode_frame(frame, nullptr)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(vp->stream->time_base) * frame->pts;

        frame->sample_aspect_ratio =
            av_guess_sample_aspect_ratio(vp->dmx()->ic(), vp->stream, frame);

        if (framedrop > 0 ||
            (framedrop && p->masterSyncType() != AVSyncType::VideoMaster)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - p->masterClock();
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - p->frameLastFilterDelay() < 0 &&
                    vp->decoder.pkt_serial() == p->vidclk().serial() &&
                    p->videoq().nb_packets()) {
                    p->frameDropsEarly()++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

// ==========================================================================
//  run() — the old video_thread()
// ==========================================================================

void VideoPipeline::run()
{
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = stream->time_base;
    AVFormatContext *ic = dmx_->ic();
    AVRational frame_rate = av_guess_frame_rate(ic, stream, nullptr);

    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    int last_w = 0, last_h = 0;
    enum AVPixelFormat last_format = static_cast<AVPixelFormat>(-2);
    int last_serial = -1;
    int last_vfilter_idx = 0;

    if (!frame) return;

    for (;;) {
        ret = get_video_frame(this, player_, frame);
        if (ret < 0) goto the_end;
        if (!ret) continue;

        if (last_w != frame->width || last_h != frame->height ||
            last_format != frame->format ||
            last_serial != decoder.pkt_serial() ||
            last_vfilter_idx != player_->videoFilterIndex())
        {
            av_log(nullptr, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d "
                   "to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(
                       av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(
                       av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)), "none"),
                   decoder.pkt_serial());

            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) { ret = AVERROR(ENOMEM); goto the_end; }
            graph->nb_threads = filter_nbthreads;

            if ((ret = configure_video_filters(graph, player_->videoDevice(),
                                               ic, stream, frame,
                                               player_->videoFilterIndex(),
                                               &filt_in, &filt_out)) < 0) {
                SDL_Event event;
                event.type = SDL_USEREVENT + 2;
                event.user.data1 = player_;
                SDL_PushEvent(&event);
                goto the_end;
            }
            in_filter  = filt_in;
            out_filter = filt_out;
            last_w     = frame->width;
            last_h     = frame->height;
            last_format= static_cast<AVPixelFormat>(frame->format);
            last_serial= decoder.pkt_serial();
            last_vfilter_idx = player_->videoFilterIndex();
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0) goto the_end;

        while (ret >= 0) {
            player_->frameLastReturnedTime() = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    decoder.set_finished(decoder.pkt_serial());
                ret = 0;
                break;
            }

            int64_t *fd = frame->opaque_ref
                              ? (int64_t *)frame->opaque_ref->data : nullptr;

            player_->frameLastFilterDelay() =
                av_gettime_relative() / 1000000.0 - player_->frameLastReturnedTime();
            if (fabs(player_->frameLastFilterDelay()) > AV_NOSYNC_THRESHOLD / 10.0)
                player_->frameLastFilterDelay() = 0;
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den
                            ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(player_->videoDevice(), &player_->pictq(),
                                frame, pts, duration,
                                fd ? *fd : -1, decoder.pkt_serial());
            av_frame_unref(frame);
            if (player_->videoq().serial() != decoder.pkt_serial())
                break;
        }

        if (ret < 0) goto the_end;
    }
 the_end:
    avfilter_graph_free(&graph);
    in_filter  = nullptr;
    out_filter = nullptr;
    av_frame_free(&frame);
}

// get_rotation — moved from ffplay.cpp utils path
static double get_rotation(const int32_t *displaymatrix)
{
    double theta = 0.0;
    if (displaymatrix) {
        theta = -round(av_display_rotation_get(displaymatrix));
        theta -= 360.0 * floor(theta / 360.0 + 0.9 / 360.0);
    }
    return theta;
}
