#include "decoder.h"
#include "frame_queue.h"
#include "packet_queue.h"

#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

Decoder::Decoder() = default;

Decoder::~Decoder()
{
    if (thread_.joinable())
        std::terminate();
    release_codec();
}

void Decoder::release_codec()
{
    if (pkt_) {
        av_packet_free(&pkt_);
        pkt_ = nullptr;
    }
    if (avctx_) {
        avcodec_free_context(&avctx_);
        avctx_ = nullptr;
    }
    queue_          = nullptr;
    on_queue_low_   = nullptr;
    pkt_serial_     = -1;
    finished_       = 0;
    packet_pending_ = 0;
    start_pts_      = AV_NOPTS_VALUE;
    start_pts_tb_   = {};
    next_pts_       = AV_NOPTS_VALUE;
    next_pts_tb_    = {};
}

int Decoder::init(AVCodecContext *avctx, PacketQueue *queue,
                  std::function<void()> on_queue_low, int reorder_pts)
{
    if (!avctx || !queue)
        return AVERROR(EINVAL);
    if (thread_.joinable())
        return AVERROR(EINVAL);

    /* Re-init after failed start / tests: drop previous codec without joining. */
    if (pkt_)
        av_packet_free(&pkt_);
    pkt_ = nullptr;
    if (avctx_)
        avcodec_free_context(&avctx_);
    avctx_ = nullptr;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    pkt_            = pkt;
    queue_          = queue;
    on_queue_low_   = std::move(on_queue_low);
    reorder_pts_    = reorder_pts;
    start_pts_      = AV_NOPTS_VALUE;
    pkt_serial_     = -1;
    finished_       = 0;
    packet_pending_ = 0;
    next_pts_       = AV_NOPTS_VALUE;
    next_pts_tb_    = {};
    avctx_          = avctx;
    return 0;
}

int Decoder::start(FrameQueue *fq, int (*thread_fn)(void *),
                   const char * /*name*/, void *arg)
{
    if (!queue_ || !fq || !thread_fn)
        return AVERROR(EINVAL);

    queue_->start();
    fq->start();
    thread_ = std::thread(thread_fn, arg);
    return 0;
}

void Decoder::abort(FrameQueue *fq)
{
    if (!queue_ || !fq)
        return;
    queue_->abort();
    fq->abort();
    if (thread_.joinable())
        thread_.join();
    queue_->flush();
}

int Decoder::decode_frame(AVFrame *frame, AVSubtitle *sub)
{
    if (!avctx_ || !queue_ || !pkt_)
        return -1;

    int ret = AVERROR(EAGAIN);

    for (;;) {
        if (queue_->serial() == pkt_serial_) {
            do {
                if (queue_->abort_request())
                    return -1;

                switch (avctx_->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(avctx_, frame);
                        if (ret >= 0) {
                            if (reorder_pts_ == -1)
                                frame->pts = frame->best_effort_timestamp;
                            else if (!reorder_pts_)
                                frame->pts = frame->pkt_dts;
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(avctx_, frame);
                        if (ret >= 0) {
                            AVRational tb{1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, avctx_->pkt_timebase, tb);
                            else if (next_pts_ != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(next_pts_, next_pts_tb_, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                next_pts_    = frame->pts + frame->nb_samples;
                                next_pts_tb_ = tb;
                            }
                        }
                        break;
                    default: break;
                }
                if (ret == AVERROR_EOF) {
                    finished_ = pkt_serial_;
                    avcodec_flush_buffers(avctx_);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (queue_->nb_packets() == 0 && on_queue_low_)
                on_queue_low_();

            if (packet_pending_) {
                packet_pending_ = 0;
            } else {
                int old_serial = pkt_serial_;
                if (queue_->get(pkt_, 1, &pkt_serial_) < 0)
                    return -1;
                if (old_serial != pkt_serial_) {
                    avcodec_flush_buffers(avctx_);
                    finished_    = 0;
                    next_pts_    = start_pts_;
                    next_pts_tb_ = start_pts_tb_;
                }
            }
            if (queue_->serial() == pkt_serial_)
                break;
            av_packet_unref(pkt_);
        } while (1);

        if (avctx_->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            int got_frame = 0;
            ret = avcodec_decode_subtitle2(avctx_, sub, &got_frame, pkt_);
            if (ret < 0) {
                ret = AVERROR(EAGAIN);
            } else {
                if (got_frame && !pkt_->data)
                    packet_pending_ = 1;
                ret = got_frame ? 0 : (pkt_->data ? AVERROR(EAGAIN) : AVERROR_EOF);
            }
            av_packet_unref(pkt_);
        } else {
            if (pkt_->buf && !pkt_->opaque_ref) {
                pkt_->opaque_ref = av_buffer_allocz(sizeof(int64_t));
                if (!pkt_->opaque_ref)
                    return AVERROR(ENOMEM);
                *reinterpret_cast<int64_t *>(pkt_->opaque_ref->data) = pkt_->pos;
            }

            if (avcodec_send_packet(avctx_, pkt_) == AVERROR(EAGAIN)) {
                av_log(avctx_, AV_LOG_ERROR,
                       "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                packet_pending_ = 1;
            } else {
                av_packet_unref(pkt_);
            }
        }
    }
}

void Decoder::set_start_pts(int64_t pts, AVRational tb)
{
    start_pts_    = pts;
    start_pts_tb_ = tb;
}
