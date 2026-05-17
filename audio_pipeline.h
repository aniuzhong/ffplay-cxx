#pragma once

#include <cstdint>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
}

#include "audio_device.h"
#include "decoder.h"
#include "frame.h"

class PacketQueue;
class FrameQueue;
class Demuxer;
class Player;

class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();
    AudioPipeline(const AudioPipeline &) = delete;
    AudioPipeline &operator=(const AudioPipeline &) = delete;

    int  init(AVCodecContext *avctx, PacketQueue *audioq,
              FrameQueue *sampq, Demuxer *dmx,
              AVStream *audio_st, int reorder_pts);
    void start(Player *player);
    void abort();
    void releaseCodec();

    Decoder       decoder;
    PacketQueue  *pktq = nullptr;     // borrowed
    FrameQueue   *sampq = nullptr;    // borrowed
    AVStream     *stream = nullptr;   // borrowed
    int           stream_index = -1;

    // Audio filter graph
    AVFilterGraph   *agraph = nullptr;
    AVFilterContext *in_filter = nullptr;
    AVFilterContext *out_filter = nullptr;

    // Filter source params — tracks incoming frame format
    AudioParams   filter_src;

    // Resampling
    SwrContext   *swr_ctx = nullptr;
    uint8_t      *audio_buf = nullptr;
    uint8_t      *audio_buf1 = nullptr;
    unsigned      audio_buf_size = 0;
    unsigned      audio_buf1_size = 0;
    int           audio_buf_index = 0;
    int           audio_write_buf_size = 0;

    // Sync statistics
    double        diff_cum = 0;
    double        diff_avg_coef = 0;
    double        diff_threshold = 0;
    int           diff_avg_count = 0;

    // clock updated during decode
    double        audio_clock = NAN;
    int           audio_clock_serial = -1;

    // "fill" the SDL audio buffer — decode, resample, synchronize, mix.
    // Returns the number of bytes consumed from stream, or 0.
    // Initial audio filter graph setup (called from prepare(), before start()).
    // Must be called before the audio device is opened so that the output
    // sample rate / channel layout are known.
    int  configureFilters(const char *af, bool force_output,
                          const AudioParams *audio_tgt,
                          int *out_sample_rate,
                          AVChannelLayout *out_ch_layout);

    int  fill(uint8_t *stream, int len, bool paused,
              double (*sync_diff_fn)(void *), void *sync_diff_ctx,
              Frame *(*peek_fn)(void *), void *peek_ctx,
              const std::function<void(const int16_t *, int)> *on_decode);

private:
    std::thread thread_;
    Demuxer    *dmx_ = nullptr;       // borrowed
    Player     *player_ = nullptr;    // borrowed

    void run();

    int  decode_one(bool paused,
                    Frame *(*peek_fn)(void *), void *peek_ctx,
                    double (*sync_diff_fn)(void *), void *sync_diff_ctx,
                    const std::function<void(const int16_t *, int)> *on_decode);
    int  sync_samples(int nb_samples, double sync_diff);
};
