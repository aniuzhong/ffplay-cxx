#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <SDL.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include "audio_pipeline.h"
#include "audio_output.h"
#include "audio_visualizer.h"
#include "avsync_type.h"
#include "clock.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "subtitle_pipeline.h"
#include "video_pipeline.h"

class Demuxer;
class VideoOutput;

/*
 * Thread model and ownership
 * -------------------------
 * Main thread (SDL event loop):
 *   · Owns Player::pause / seekTo / stop
 *   · Sole caller of videoRefresh
 *   · Reads playback state (currentPosition, isPaused, etc.)
 *
 * SDL audio callback thread:
 *   · sdlAudioCallback → AudioPipeline::fill() → updates audclk_
 *   · Uses function pointers + opaque (zero heap allocation per callback)
 *
 * Demuxer internal thread:
 *   · Reads packets → dispatches to PacketQueues
 *   · Notifies via callbacks (seek_done, read_error)
 *   · Never calls Player::pause / stop directly
 *   · Communicates to main thread via SDL_PushEvent (same as FF_QUIT_EVENT today)
 *
 * Decoder threads (one per Pipeline):
 *   · PacketQueue → decode → FrameQueue
 *   · Reads Demuxer abort/paused flags, never writes Player state
 *
 * Seek protocol
 * -------------
 * 1. Player::seekTo(sec, rel_sec) → Demuxer::seek(pos, rel, flags)
 * 2. Demuxer callback → Player lambda → flush audioq/videoq/subtitleq
 * 3. Decoder threads detect PacketQueue flush → avcodec_flush_buffers
 *    (triggered automatically by serial change in Decoder::decode_frame)
 * 4. Demuxer dispatches packets from new position (new serial)
 * 5. Decoder threads receive new serial → begin decoding new frames
 * 6. Main thread videoRefresh detects serial change → resets frame_timer
 */
class Player {
public:
    Player();
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    // -- Data source --
    void setDataSource(const char *url,
                       const AVInputFormat *fmt = nullptr);

    // -- Device injection (may be nullptr for headless / no-audio) --
    void setVideoDevice(VideoOutput *dev);
    void setAudioDevice(AudioOutput *dev);

    // -- Lifecycle --
    int  prepare();       // v1: reads ffplay.cpp globals for options
    int  start();
    void pause();
    void stop();          // Idempotent. Must release (in order):
                          //   1. demuxer/read thread
                          //   2. decoder threads
                          //   3. audio device
                          //   4. video/output devices
                          //   5. codecs

    // -- Query --
    double currentPosition() const;
    double duration() const;
    bool   isPaused() const { return paused_; }

    // -- Control --
    /** @param rel_sec relative jump in seconds (ffplay incr); forwarded as seek_rel in AV_TIME_BASE. */
    void seekTo(double sec, double rel_sec = 0.0);
    void stepToNextFrame();
    void cycleChannel(int codecType);
    void toggleMute();
    void setVolume(int v);
    void setVideoFilter(int idx);
    void toggleAudioDisplay();

    // -- Display refresh (main thread) --
    double videoRefresh(double *remaining_time);
    void   forceRefresh() { force_refresh_ = 1; }

    // -- SDL audio callback entry point --
    static void sdlAudioCallback(void *opaque, Uint8 *stream, int len);

    // -- Callbacks (called from main / event_loop thread only) --
    //     Demuxer thread only SDL_PushEvent — never calls these directly.
    void setOnCompletion(std::function<void()> cb) { on_completion_ = std::move(cb); }
    void setOnError(std::function<void(int, const char *)> cb) { on_error_ = std::move(cb); }

    // -- Accessors for Pipeline friend access --
    Demuxer      *dmx()   const { return dmx_.get(); }
    VideoOutput  *videoDevice() const { return video_dev_; }
    AudioOutput  *audioDevice() const { return audio_dev_; }
    int           videoFilterIndex() const { return vfilter_idx_; }
    bool          hasVideoStreamOpen() const { return video_.stream_index >= 0 && video_.decoder.has_codec(); }
    bool          hasAudioStreamOpen() const { return audio_.stream_index >= 0 && audio_.decoder.has_codec(); }
    int          &frameDropsEarly()    { return frame_drops_early_; }
    int          &frameDropsLate()     { return frame_drops_late_; }
    double       &frameLastFilterDelay() { return frame_last_filter_delay_; }
    double       &frameLastReturnedTime() { return frame_last_returned_time_; }
    int           step() const { return step_; }

    AVSyncType    masterSyncType() const;
    double        masterClock() const;
    const Clock  &vidclk() const { return vidclk_; }
    const Clock  &audclk() const { return audclk_; }
    Clock        &vidclk()       { return vidclk_; }
    Clock        &audclk()       { return audclk_; }
    Clock        &extclk()       { return extclk_; }
    const Clock  &extclk() const { return extclk_; }

    // FrameQueues — borrowed by Pipelines, owned by Player
    FrameQueue  &pictq() { return pictq_; }
    FrameQueue  &sampq() { return sampq_; }
    FrameQueue  &subpq() { return subpq_; }

    PacketQueue &videoq() { return videoq_; }
    PacketQueue &audioq() { return audioq_; }
    PacketQueue &subtitleq() { return subtitleq_; }

    // Audio visualization
    AudioVisualizer &audioVis() { return audio_vis_; }
    double          &lastVisTime() { return last_vis_time_; }

    int            &forceRefreshRef() { return force_refresh_; }

private:
    std::unique_ptr<Demuxer> dmx_;

    // Queues — Player owns
    PacketQueue videoq_;
    PacketQueue audioq_;
    PacketQueue subtitleq_;
    FrameQueue  pictq_;
    FrameQueue  sampq_;
    FrameQueue  subpq_;

    AudioPipeline    audio_;
    VideoPipeline    video_;
    SubtitlePipeline sub_;

    VideoOutput *video_dev_ = nullptr;   // borrowed
    AudioOutput *audio_dev_ = nullptr;   // borrowed

    Clock audclk_;
    Clock vidclk_;
    Clock extclk_;
    AVSyncType av_sync_type_{};

    // Playback state
    bool        paused_ = false;
    bool        started_ = false;
    int         force_refresh_ = 0;
    int         step_ = 0;
    int         frame_drops_early_ = 0;
    int         frame_drops_late_ = 0;
    int         vfilter_idx_ = 0;

    double      frame_timer_ = 0;
    double      frame_last_filter_delay_ = 0;
    double      frame_last_returned_time_ = 0;

    // Audio visualization
    AudioVisualizer audio_vis_;
    double          last_vis_time_ = 0;

    // Stream indices (for cycleChannel)
    int last_video_stream_ = -1;
    int last_audio_stream_ = -1;
    int last_subtitle_stream_ = -1;

    // Data source (set before prepare())
    std::string url_;
    const AVInputFormat *ifmt_ = nullptr;

    // Callbacks
    std::function<void()>                on_completion_;
    std::function<void(int, const char *)> on_error_;

    // Private helpers
    int  openStream(int stream_index);
    void closeStream(int stream_index);
    void rollbackPrepare();
    void wireDemuxerCallbacks();
    void stream_seek(double pos_sec, double incr_sec = 0.0);
};
