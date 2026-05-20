#include "subtitle_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#include "decoder.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "packet_queue.h"

SubtitlePipeline::~SubtitlePipeline()
{
    abort();
}

int SubtitlePipeline::init(AVCodecContext *avctx, PacketQueue *subq,
                            FrameQueue *subpq_in, Demuxer *dmx, int reorder_pts)
{
    if (!avctx || !subq || !subpq_in || !dmx)
        return AVERROR(EINVAL);

    this->pktq   = subq;
    this->subpq  = subpq_in;
    dmx_         = dmx;

    return decoder.init(avctx, subq,
        [dmx] { dmx->wake(); }, reorder_pts);
}

void SubtitlePipeline::start()
{
    pktq->start();
    subpq->start();
    thread_ = std::thread(&SubtitlePipeline::run, this);
}

void SubtitlePipeline::abort()
{
    if (pktq) pktq->abort();
    if (subpq) subpq->abort();
    if (thread_.joinable())
        thread_.join();
    if (pktq) pktq->flush();
}

void SubtitlePipeline::releaseCodec()
{
    abort();
    decoder.release_codec();
    dmx_       = nullptr;
    pktq       = nullptr;
    subpq      = nullptr;
    stream     = nullptr;
}

void SubtitlePipeline::run()
{
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = subpq->peek_writable()))
            return;

        if ((got_subtitle = decoder.decode_frame(nullptr, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts      = pts;
            sp->serial   = decoder.pkt_serial();
            sp->width    = decoder.width();
            sp->height   = decoder.height();
            sp->uploaded = 0;

            subpq->push();
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
}
