#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

extern "C" {
#include <libavutil/avutil.h>
}

struct AVFormatContext;
struct AVInputFormat;
struct AVPacket;
struct AVDictionary;
struct AVStream;

// DemuxerOptions: pure value type
struct DemuxerOptions {
    // Stream discovery
    bool find_stream_info = true;
    bool genpts           = false;
    int  seek_by_bytes    = -1; // -1 = auto

    // Playback range
    int64_t start_time    = AV_NOPTS_VALUE;
    int64_t duration      = AV_NOPTS_VALUE;

    // EOF behaviour
    int  loop             = 1;
    bool autoexit         = false;

    // Buffer control
    int  infinite_buffer  = -1;

    // Display
    bool show_status      = false;

    // Stream selection
    const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {};
    bool video_disable    = false;
    bool audio_disable    = false;
    bool subtitle_disable = false;
};

// on_packet:  called for every demuxed packet.  pkt may be nullptr to
//             signal end-of-stream (null packet).  The handler must take
//             ownership (e.g. PacketQueue::put moves ref); Demuxer does not
//             unref after a successful handler call.
using PacketHandler  = std::function<void(AVPacket *pkt, int stream_index)>;

// on_seek_done:   avformat_seek_file succeeded.
using SeekDoneHandler = std::function<void(int64_t target, int flags)>;

// on_seek_failed: avformat_seek_file returned an error.
using SeekFailedHandler = std::function<void()>;

// on_seek_complete: called once after a seek burst in this read-loop
//                   iteration (only if at least one seek was processed).
using SeekCompleteHandler = std::function<void()>;

// on_queues_full: returns true if downstream queues have enough packets;
//                 the read thread will sleep until woken.
using QueuesFullHandler = std::function<bool()>;

// on_decoders_done: returns true when all active decoders have finished
//                   and their frame queues are empty.  Demuxer uses this
//                   to trigger looping / autoexit policy.
using DecodersDoneHandler = std::function<bool()>;

// on_read_fatal: invoked when read_loop exits due to fatal error (same
//                 role as read_thread pushing FF_QUIT_EVENT in ffplay).
using ReadFatalHandler = std::function<void()>;

class Demuxer {
public:
    Demuxer(const char *filename, const AVInputFormat *iformat,
            const DemuxerOptions &opts);
    ~Demuxer();

    Demuxer(const Demuxer &) = delete;
    Demuxer &operator=(const Demuxer &) = delete;

    // init:  format_opts — ownership transferred; *format_opts becomes
    //        nullptr on return (success or failure).
    //        codec_opts — read-only; caller retains ownership and must
    //        guarantee the pointer stays valid for the entire call.
    int  init(AVDictionary **format_opts, AVDictionary *codec_opts);

    // All handlers must be set before start().
    void set_packet_handler(PacketHandler h)           { on_packet_ = std::move(h); }
    void set_seek_done_handler(SeekDoneHandler h)      { on_seek_done_ = std::move(h); }
    void set_seek_failed_handler(SeekFailedHandler h)  { on_seek_failed_ = std::move(h); }
    void set_seek_complete_handler(SeekCompleteHandler h) { on_seek_complete_ = std::move(h); }
    void set_queues_full_handler(QueuesFullHandler h)  { on_queues_full_ = std::move(h); }
    void set_decoders_done_handler(DecodersDoneHandler h) { on_decoders_done_ = std::move(h); }
    void set_read_fatal_handler(ReadFatalHandler h)     { on_read_fatal_ = std::move(h); }

    void start();
    void stop();

    // Control (thread-safe, may be called during read loop)
    void seek(int64_t pos, int64_t rel, int flags);
    void toggle_pause(); // flips paused_, does NOT touch clocks
    void set_paused(bool v);
    bool is_paused() const;
    int  abort_request() const;
    void abort();
    void wake();

    AVFormatContext *ic()            const { return ic_; }
    int  audio_index()               const { return audio_idx_; } // init snapshot
    int  video_index()               const { return video_idx_; }
    int  subtitle_index()            const { return subtitle_idx_; }
    bool pause_supported()           const;
    bool realtime()                  const { return realtime_; }
    double max_frame_duration()      const { return max_frame_duration_; }
    // Resolved after init() (auto -1 → 0/1 like classic read_thread).
    int  seek_by_bytes()             const { return options_.seek_by_bytes; }
    // May change at read_loop start when -1 and realtime (same as ffplay).
    int  infinite_buffer()           const { return options_.infinite_buffer; }

    // Set active streams (called after stream_component_open / close).
    void set_audio_stream(int idx, AVStream *st)    { active_audio_idx_    = idx; active_audio_st_    = st; }
    void set_video_stream(int idx, AVStream *st)    { active_video_idx_    = idx; active_video_st_    = st; }
    void set_subtitle_stream(int idx, AVStream *st) { active_subtitle_idx_ = idx; active_subtitle_st_ = st; }

private:
    void read_loop();

    // ══════════════════════════════════════════════════════════════
    //  Resources (owned)
    // ══════════════════════════════════════════════════════════════
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    AVFormatContext *ic_ = nullptr;
    char *filename_ = nullptr;
    const AVInputFormat *iformat_ = nullptr;
    DemuxerOptions options_;

    // ══════════════════════════════════════════════════════════════
    //  Handlers (set before start, immutable thereafter)
    // ══════════════════════════════════════════════════════════════
    PacketHandler       on_packet_;
    SeekDoneHandler     on_seek_done_;
    SeekFailedHandler   on_seek_failed_;
    SeekCompleteHandler on_seek_complete_;
    QueuesFullHandler   on_queues_full_;
    DecodersDoneHandler on_decoders_done_;
    ReadFatalHandler    on_read_fatal_;

    // ══════════════════════════════════════════════════════════════
    //  Stream snapshots — immutable after init()
    // ══════════════════════════════════════════════════════════════
    int audio_idx_             = -1;
    int video_idx_             = -1;
    int subtitle_idx_          = -1;
    int realtime_              = 0;
    double max_frame_duration_ = 0;

    // ══════════════════════════════════════════════════════════════
    //  Active stream references — set after stream_component_open,
    //  updated on stream switch.  Written by main thread, read by
    //  read_thread — same relaxed model as classic ffplay (possible
    //  data race if switching streams while demux runs; unchanged).
    // ══════════════════════════════════════════════════════════════
    int active_audio_idx_    = -1;
    int active_video_idx_    = -1;
    int active_subtitle_idx_ = -1;
    AVStream *active_audio_st_    = nullptr;
    AVStream *active_video_st_    = nullptr;
    AVStream *active_subtitle_st_ = nullptr;

    // ══════════════════════════════════════════════════════════════
    //  Control messages — release / acquire paired
    // ══════════════════════════════════════════════════════════════
    std::atomic<int> seek_req_{0};
    int64_t seek_pos_   = 0;
    int64_t seek_rel_   = 0;
    int seek_flags_     = 0;

    std::atomic<int> abort_request_{0};

    // paused: single writer (main thread), multiple readers.
    std::atomic<int> paused_{0};

    // read_pause_return_: written by read_thread (release),
    //                     read by main thread (acquire via pause_supported()).
    //                     -1 = av_read_pause not yet called.
    std::atomic<int> read_pause_return_{-1};

    // ══════════════════════════════════════════════════════════════
    //  read_thread only — no concurrent access
    // ══════════════════════════════════════════════════════════════
    int last_paused_           = 0;
    int queue_attachments_req_ = 0;
    int eof_                   = 0;
};
