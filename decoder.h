#pragma once

#include <functional>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

class PacketQueue;
class FrameQueue;

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Takes ownership of avctx on success. On failure, caller retains avctx.
    int init(AVCodecContext *avctx, PacketQueue *queue,
             std::function<void()> on_queue_low = {},
             int reorder_pts = -1);

    // Thread management. fq start/abort pairs with queue start/abort.
    int start(FrameQueue *fq, int (*thread_fn)(void *), const char *name, void *arg);
    void abort(FrameQueue *fq);

    // Frees avctx/pkt and clears borrow pointers. Call after abort() when
    // tearing down a stream (replaces decoder_destroy). Idempotent.
    void release_codec();

    // Returns: 1 = got frame, 0 = EOF, -1 = abort/error
    int decode_frame(AVFrame *frame, AVSubtitle *sub);

    // Accessors — read by thread functions & main thread
    int pkt_serial()  const { return pkt_serial_; }
    int finished()    const { return finished_; }
    int width()       const { return avctx_ ? avctx_->width : 0; }
    int height()      const { return avctx_ ? avctx_->height : 0; }

    void set_finished(int serial)   { finished_ = serial; }
    void set_start_pts(int64_t pts, AVRational tb);

private:
    AVCodecContext *avctx_ = nullptr;   // owning
    AVPacket       *pkt_   = nullptr;   // owning
    PacketQueue    *queue_ = nullptr;   // borrowed
    int             pkt_serial_ = -1;
    int             finished_ = 0;
    int             packet_pending_ = 0;
    int             reorder_pts_ = -1;
    int64_t         start_pts_ = AV_NOPTS_VALUE;
    AVRational      start_pts_tb_{};
    int64_t         next_pts_ = AV_NOPTS_VALUE;
    AVRational      next_pts_tb_{};
    std::thread     thread_;
    std::function<void()> on_queue_low_;
};
