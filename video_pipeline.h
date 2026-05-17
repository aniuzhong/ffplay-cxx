#pragma once

#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include "decoder.h"

class PacketQueue;
class FrameQueue;
class Demuxer;
class Player;

class VideoPipeline {
public:
    VideoPipeline() = default;
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline &) = delete;
    VideoPipeline &operator=(const VideoPipeline &) = delete;

    int  init(AVCodecContext *avctx, PacketQueue *videoq,
              FrameQueue *pictq, Demuxer *dmx,
              AVStream *video_st, int reorder_pts);
    void start(Player *player);
    void abort();
    void releaseCodec();

    Decoder       decoder;
    PacketQueue  *pktq = nullptr;     // borrowed
    FrameQueue   *pictq = nullptr;    // borrowed
    AVStream     *stream = nullptr;   // borrowed
    int           stream_index = -1;

    // Video filter graph
    AVFilterGraph   *graph = nullptr;
    AVFilterContext *in_filter = nullptr;
    AVFilterContext *out_filter = nullptr;

    Demuxer    *dmx() const { return dmx_; }

private:
    std::thread thread_;
    Demuxer    *dmx_ = nullptr;       // borrowed
    Player     *player_ = nullptr;    // borrowed

    void run();
};
