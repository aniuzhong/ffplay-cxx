#pragma once

#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "decoder.h"

class PacketQueue;
class FrameQueue;
class Demuxer;

class SubtitlePipeline {
public:
    SubtitlePipeline() = default;
    ~SubtitlePipeline();
    SubtitlePipeline(const SubtitlePipeline &) = delete;
    SubtitlePipeline &operator=(const SubtitlePipeline &) = delete;

    int  init(AVCodecContext *avctx, PacketQueue *subq,
              FrameQueue *subpq_in, Demuxer *dmx, int reorder_pts);
    void start();
    void abort();
    void releaseCodec();

    Decoder       decoder;
    PacketQueue  *pktq = nullptr;     // borrowed
    FrameQueue   *subpq = nullptr;    // borrowed
    int           stream_index = -1;
    AVStream     *stream = nullptr;   // borrowed

private:
    std::thread thread_;
    Demuxer    *dmx_ = nullptr;

    void run();
};
