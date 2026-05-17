#include "audio_pipeline.h"

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include "decoder.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "player.h"

// v1 globals
extern char *afilters;
extern int filter_nbthreads;
extern "C" AVDictionary *swr_opts;    // defined in utils/cmdutils.c (C linkage)

static inline int cmp_audio_fmts(AVSampleFormat fmt1, int64_t ch_count1,
                                 AVSampleFormat fmt2, int64_t ch_count2)
{
    if (ch_count1 == 1 && ch_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return ch_count1 != ch_count2 || fmt1 != fmt2;
}

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx);

static int configure_audio_filters(AudioPipeline *ap,
                                    const char *af,
                                    int force_output_format,
                                    const AudioParams *audio_tgt);

AudioPipeline::~AudioPipeline()
{
    abort();
}

int AudioPipeline::init(AVCodecContext *avctx, PacketQueue *audioq,
                         FrameQueue *sampq, Demuxer *dmx,
                         AVStream *audio_st, int reorder_pts)
{
    if (!avctx || !audioq || !sampq || !dmx || !audio_st)
        return AVERROR(EINVAL);

    stream       = audio_st;
    this->pktq   = audioq;
    this->sampq  = sampq;
    dmx_         = dmx;

    return decoder.init(avctx, audioq, [dmx] { dmx->wake(); }, reorder_pts);
}

void AudioPipeline::start(Player *player)
{
    player_ = player;
    pktq->start();
    sampq->start();
    thread_ = std::thread(&AudioPipeline::run, this);
}

void AudioPipeline::abort()
{
    if (pktq) pktq->abort();
    if (sampq) sampq->abort();
    if (thread_.joinable())
        thread_.join();
    if (pktq) pktq->flush();
}

void AudioPipeline::releaseCodec()
{
    abort();
    decoder.release_codec();
    avfilter_graph_free(&agraph);
    swr_free(&swr_ctx);
    av_freep(&audio_buf1);
    audio_buf       = nullptr;
    audio_buf_size  = 0;
    audio_buf1_size = 0;
    audio_buf_index = 0;
    audio_write_buf_size = 0;
    in_filter       = nullptr;
    out_filter      = nullptr;
    player_         = nullptr;
    dmx_            = nullptr;
    pktq            = nullptr;
    sampq           = nullptr;
    stream          = nullptr;
}

void AudioPipeline::run()
{
    AVFrame *frame = av_frame_alloc();
    Frame *af;
    int last_serial = -1;
    int reconfigure;
    int got_frame = 0;
    AVRational tb;
    int ret = 0;

    if (!frame) return;

    do {
        if ((got_frame = decoder.decode_frame(frame, nullptr)) < 0)
            goto the_end;

        if (got_frame) {
            tb = AVRational{1, frame->sample_rate};

            reconfigure =
                cmp_audio_fmts(filter_src.fmt,
                               filter_src.ch_layout.nb_channels,
                               static_cast<AVSampleFormat>(frame->format),
                               frame->ch_layout.nb_channels) ||
                av_channel_layout_compare(&filter_src.ch_layout, &frame->ch_layout) ||
                filter_src.freq != frame->sample_rate ||
                decoder.pkt_serial() != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_channel_layout_describe(&filter_src.ch_layout, buf1, sizeof(buf1));
                av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                av_log(nullptr, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d "
                       "to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       filter_src.freq, filter_src.ch_layout.nb_channels,
                       av_get_sample_fmt_name(filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->ch_layout.nb_channels,
                       av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)),
                       buf2, decoder.pkt_serial());

                filter_src.fmt = static_cast<AVSampleFormat>(frame->format);
                if (av_channel_layout_copy(&filter_src.ch_layout, &frame->ch_layout) < 0)
                    goto the_end;
                filter_src.freq = frame->sample_rate;
                last_serial = decoder.pkt_serial();

                if (player_->audioDevice()) {
                    if ((ret = configure_audio_filters(this, afilters, 1,
                             &player_->audioDevice()->hw_params())) < 0)
                        goto the_end;
                }
            }

            if ((ret = av_buffersrc_add_frame(in_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(out_filter, frame, 0)) >= 0) {
                int64_t *fd = frame->opaque_ref
                                  ? (int64_t *)frame->opaque_ref->data : nullptr;
                tb = av_buffersink_get_time_base(out_filter);
                if (!(af = sampq->peek_writable()))
                    goto the_end;

                af->pts      = (frame->pts == AV_NOPTS_VALUE) ? NAN
                                                               : frame->pts * av_q2d(tb);
                af->pos      = fd ? *fd : -1;
                af->serial   = decoder.pkt_serial();
                af->duration = av_q2d(AVRational{frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                sampq->push();

                if (player_->audioq().serial() != decoder.pkt_serial())
                    break;
            }
            if (ret == AVERROR_EOF)
                decoder.set_finished(decoder.pkt_serial());
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);

 the_end:
    avfilter_graph_free(&agraph);
    in_filter  = nullptr;
    out_filter = nullptr;
    av_frame_free(&frame);
}

// ==========================================================================
//  configureFilters — initial audio filter graph setup
// ==========================================================================

int AudioPipeline::configureFilters(const char *af, bool force_output,
                                     const AudioParams *audio_tgt,
                                     int *out_sample_rate,
                                     AVChannelLayout *out_ch_layout)
{
    int ret = configure_audio_filters(this, af, force_output ? 1 : 0, audio_tgt);
    if (ret < 0)
        return ret;

    AVFilterContext *sink = out_filter;
    if (out_sample_rate) {
        *out_sample_rate = av_buffersink_get_sample_rate(sink);
    }
    if (out_ch_layout) {
        ret = av_buffersink_get_ch_layout(sink, out_ch_layout);
        if (ret < 0)
            return ret;
    }
    return 0;
}

// ==========================================================================
//  fill() — stub for now (AudioOutput::read() still handles decode+sync)
// ==========================================================================

int AudioPipeline::fill(uint8_t *, int, bool,
                         double (*)(void *), void *,
                         Frame *(*)(void *), void *,
                         const std::function<void(const int16_t *, int)> *)
{
    // TODO: move decode+sync logic from AudioOutput into this method
    return -1;
}

// ==========================================================================
//  Static helpers
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

static int configure_audio_filters(AudioPipeline *ap,
                                    const char *af,
                                    int force_output_format,
                                    const AudioParams *audio_tgt)
{
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    char aresample_swr_opts[512] = "";
    const AVDictionaryEntry *e = nullptr;
    AVBPrint bp;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&ap->agraph);
    if (!(ap->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    ap->agraph->nb_threads = filter_nbthreads;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

    while ((e = av_dict_iterate(swr_opts, e)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts),
                    "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    av_opt_set(ap->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    av_channel_layout_describe_bprint(&ap->filter_src.ch_layout, &bp);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                   ap->filter_src.freq, av_get_sample_fmt_name(ap->filter_src.fmt),
                   1, ap->filter_src.freq, bp.str);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, nullptr, ap->agraph);
    if (ret < 0) goto end;

    filt_asink = avfilter_graph_alloc_filter(ap->agraph,
                                              avfilter_get_by_name("abuffersink"),
                                              "ffplay_abuffersink");
    if (!filt_asink) { ret = AVERROR(ENOMEM); goto end; }

    if ((ret = av_opt_set(filt_asink, "sample_formats", "s16",
                          AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        if ((ret = av_opt_set_array(filt_asink, "channel_layouts",
                                    AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_CHLAYOUT,
                                    &audio_tgt->ch_layout)) < 0)
            goto end;
        if ((ret = av_opt_set_array(filt_asink, "samplerates",
                                    AV_OPT_SEARCH_CHILDREN,
                                    0, 1, AV_OPT_TYPE_INT,
                                    &audio_tgt->freq)) < 0)
            goto end;
    }

    ret = avfilter_init_dict(filt_asink, nullptr);
    if (ret < 0) goto end;

    if ((ret = configure_filtergraph(ap->agraph, af, filt_asrc, filt_asink)) < 0)
        goto end;

    ap->in_filter  = filt_asrc;
    ap->out_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&ap->agraph);
    av_bprint_finalize(&bp, nullptr);

    return ret;
}
