#include "demuxer.h"

#include <cstdio>

extern "C" {
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

#include "cmdutils.h"

constexpr int MAX_QUEUE_SIZE = 15 * 1024 * 1024;

static int is_realtime(AVFormatContext *s)
{
    if (!strcmp(s->iformat->name, "rtp") ||
        !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp"))
        return 1;
    if (s->pb && (!strncmp(s->url, "rtp:", 4) ||
                  !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}

static int demuxer_decode_interrupt_cb(void *ctx)
{
    auto *d = static_cast<Demuxer *>(ctx);
    return d->abort_request();
}

Demuxer::Demuxer(const char *filename, const AVInputFormat *iformat,
                 const DemuxerOptions &opts)
    : filename_(av_strdup(filename))
    , iformat_(iformat)
    , options_(opts)
{
}

Demuxer::~Demuxer()
{
    stop();
    if (ic_) {
        avformat_close_input(&ic_);
        ic_ = nullptr;
    }
    av_free(filename_);
}

int Demuxer::init(AVDictionary **fmt_opts, AVDictionary *cdc_opts)
{
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = nullptr;
    int scan_all_pmts_set = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ic_ = avformat_alloc_context();
    if (!ic_) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic_->interrupt_callback.callback = demuxer_decode_interrupt_cb;
    ic_->interrupt_callback.opaque = this;

    if (!av_dict_get(*fmt_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE)) {
        av_dict_set(fmt_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    err = avformat_open_input(&ic_, filename_, iformat_, fmt_opts);
    if (err < 0) {
        print_error(filename_, err);
        ret = -1;
        goto fail;
    }
    if (scan_all_pmts_set)
        av_dict_set(fmt_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE);
    remove_avoptions(fmt_opts, cdc_opts);

    ret = check_avoptions(*fmt_opts);
    if (ret < 0)
        goto fail;

    if (options_.genpts)
        ic_->flags |= AVFMT_FLAG_GENPTS;

    if (options_.find_stream_info) {
        AVDictionary **opts;
        int orig_nb_streams = ic_->nb_streams;

        err = setup_find_stream_info_opts(ic_, cdc_opts, &opts);
        if (err < 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Error setting up avformat_find_stream_info() options\n");
            ret = err;
            goto fail;
        }

        err = avformat_find_stream_info(ic_, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(nullptr, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", filename_);
            ret = -1;
            goto fail;
        }
    }

    if (ic_->pb)
        ic_->pb->eof_reached = 0;

    if (options_.seek_by_bytes < 0)
        options_.seek_by_bytes = !(ic_->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                                 !!(ic_->iformat->flags & AVFMT_TS_DISCONT) &&
                                 strcmp("ogg", ic_->iformat->name);

    max_frame_duration_ = (ic_->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (options_.start_time != AV_NOPTS_VALUE) {
        int64_t timestamp = options_.start_time;
        if (ic_->start_time != AV_NOPTS_VALUE)
            timestamp += ic_->start_time;
        ret = avformat_seek_file(ic_, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   filename_, (double)timestamp / AV_TIME_BASE);
        }
    }

    realtime_ = is_realtime(ic_);

    if (options_.show_status) {
        fprintf(stderr, "\x1b[2K\r");
        av_dump_format(ic_, 0, filename_, 0);
    }

    // select streams
    memset(st_index, -1, sizeof(st_index));
    for (unsigned k = 0; k < ic_->nb_streams; k++) {
        AVStream *st = ic_->streams[k];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && options_.wanted_stream_spec[type] &&
            st_index[type] == -1)
            if (avformat_match_stream_specifier(ic_, st,
                    options_.wanted_stream_spec[type]) > 0)
                st_index[type] = k;
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (options_.wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Stream specifier %s does not match any %s stream\n",
                   options_.wanted_stream_spec[i],
                   av_get_media_type_string(static_cast<AVMediaType>(i)));
            st_index[i] = INT_MAX;
        }
    }

    if (!options_.video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic_, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
    if (!options_.audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic_, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                nullptr, 0);
    if (!options_.video_disable && !options_.subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic_, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                nullptr, 0);

    audio_idx_    = st_index[AVMEDIA_TYPE_AUDIO];
    video_idx_    = st_index[AVMEDIA_TYPE_VIDEO];
    subtitle_idx_ = st_index[AVMEDIA_TYPE_SUBTITLE];

    ret = 0;
fail:
    av_packet_free(&pkt);
    // Free fmt_opts: ownership was transferred.
    av_dict_free(fmt_opts);
    if (ret < 0 && ic_) {
        avformat_close_input(&ic_);
        ic_ = nullptr;
    }
    return ret;
}

void Demuxer::start()
{
    if (!on_packet_) {
        av_log(nullptr, AV_LOG_FATAL, "Demuxer::start: packet handler not set\n");
        return;
    }
    if (!on_seek_done_) {
        av_log(nullptr, AV_LOG_FATAL, "Demuxer::start: seek_done handler not set\n");
        return;
    }
    thread_ = std::thread(&Demuxer::read_loop, this);
}

void Demuxer::stop()
{
    abort_request_.store(1, std::memory_order_release);
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
    // Intentionally do not close ic_ here: VideoState::stream_component_close
    // runs after stop() and still needs a valid AVFormatContext. ic_ is closed
    // in ~Demuxer().
}

void Demuxer::seek(int64_t pos, int64_t rel, int flags)
{
    seek_pos_   = pos;
    seek_rel_   = rel;
    seek_flags_ = flags;
    seek_req_.store(1, std::memory_order_release);
    cv_.notify_one();
}

void Demuxer::toggle_pause()
{
    int cur = paused_.load(std::memory_order_relaxed);
    paused_.store(cur ? 0 : 1, std::memory_order_release);
    cv_.notify_one();
}

void Demuxer::set_paused(bool v)
{
    paused_.store(v ? 1 : 0, std::memory_order_release);
    cv_.notify_one();
}

bool Demuxer::is_paused() const
{
    return paused_.load(std::memory_order_acquire) != 0;
}

int Demuxer::abort_request() const
{
    return abort_request_.load(std::memory_order_acquire);
}

void Demuxer::abort()
{
    abort_request_.store(1, std::memory_order_release);
    cv_.notify_one();
}

void Demuxer::wake()
{
    cv_.notify_one();
}

bool Demuxer::pause_supported() const
{
    // -1 = not yet called; treat as "supported" (optimistic).
    return read_pause_return_.load(std::memory_order_acquire) != AVERROR(ENOSYS);
}

void Demuxer::read_loop()
{
    AVFormatContext *ic = ic_;
    int ret;
    AVPacket *pkt = nullptr;
    int64_t stream_start_time;
    char metadata_description[96];
    int pkt_in_play_range = 0;
    int64_t pkt_ts;
    std::mutex wait_mutex;

    eof_ = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate packet.\n");
        if (on_read_fatal_)
            on_read_fatal_();
        return;
    }

    if (options_.infinite_buffer < 0 && realtime_)
        options_.infinite_buffer = 1;

    for (;;) {
        if (abort_request_.load(std::memory_order_acquire))
            break;

        // -- pause edge detection --
        int cur_paused = paused_.load(std::memory_order_acquire);
        if (cur_paused != last_paused_) {
            last_paused_ = cur_paused;
            if (cur_paused)
                read_pause_return_.store(av_read_pause(ic), std::memory_order_release);
            else
                av_read_play(ic);
        }

#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (cur_paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(filename_, "mmsh:", 5)))) {
            av_usleep(10000);
            continue;
        }
#endif

        // -- seek handling --
        // Copy params under acquire, then clear flag before the slow
        // seek so the main thread can post another request.
        bool did_seek = false;
        while (seek_req_.load(std::memory_order_acquire)) {
            did_seek = true;
            int64_t target = seek_pos_;
            int64_t min    = seek_rel_ > 0 ? target - seek_rel_ + 2 : INT64_MIN;
            int64_t max    = seek_rel_ < 0 ? target - seek_rel_ - 2 : INT64_MAX;
            int flags      = seek_flags_;

            seek_req_.store(0, std::memory_order_relaxed);

            ret = avformat_seek_file(ic, -1, min, target, max, flags);
            if (ret < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "%s: error while seeking\n", ic->url);
                if (on_seek_failed_)
                    on_seek_failed_();
            } else {
                on_seek_done_(target, flags);
            }

            queue_attachments_req_ = 1;
            eof_ = 0;
        }
        // Match ffplay: step_to_next_frame only after a seek was handled in
        // this read-loop iteration (not on every outer-loop tick).
        if (did_seek && on_seek_complete_)
            on_seek_complete_();

        // -- attachments --
        if (queue_attachments_req_) {
            if (active_video_st_ &&
                active_video_st_->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &active_video_st_->attached_pic)) < 0)
                    goto demux_fail;
                on_packet_(pkt, active_video_idx_);
                // Signal EOF to the video decoder.
                on_packet_(nullptr, active_video_idx_);
            }
            queue_attachments_req_ = 0;
        }

        // -- queue-full check --
        if (on_queues_full_ && options_.infinite_buffer < 1 && on_queues_full_()) {
            std::unique_lock lock(wait_mutex);
            cv_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }

        // -- EOF / looping --
        if (!cur_paused &&
            on_decoders_done_ && on_decoders_done_()) {
            if (options_.loop != 1 && (!options_.loop || --options_.loop)) {
                seek(options_.start_time != AV_NOPTS_VALUE ?
                     options_.start_time : 0, 0, 0);
            } else if (options_.autoexit) {
                ret = AVERROR_EOF;
                goto demux_fail;
            }
        }

        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !eof_) {
                // Signal EOF to all active decoders via null packets.
                if (active_video_idx_ >= 0)
                    on_packet_(nullptr, active_video_idx_);
                if (active_audio_idx_ >= 0)
                    on_packet_(nullptr, active_audio_idx_);
                if (active_subtitle_idx_ >= 0)
                    on_packet_(nullptr, active_subtitle_idx_);
                eof_ = 1;
            }
            if (ic->pb && ic->pb->error) {
                if (options_.autoexit)
                    goto demux_fail;
                else
                    break;
            }
            {
                std::unique_lock lock(wait_mutex);
                cv_.wait_for(lock, std::chrono::milliseconds(10));
            }
            continue;
        } else {
            eof_ = 0;
        }

        // -- metadata update display --
        if (options_.show_status &&
            ic->streams[pkt->stream_index]->event_flags &
                AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
            fprintf(stderr, "\x1b[2K\r");
            snprintf(metadata_description,
                     sizeof(metadata_description),
                     "\r  New metadata for stream %d",
                     pkt->stream_index);
            dump_dictionary(nullptr, ic->streams[pkt->stream_index]->metadata,
                            metadata_description, "    ", AV_LOG_INFO);
        }
        ic->streams[pkt->stream_index]->event_flags &=
            ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

        // -- play-range check & dispatch --
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = options_.duration == AV_NOPTS_VALUE ||
            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
            av_q2d(ic->streams[pkt->stream_index]->time_base) -
            (double)(options_.start_time != AV_NOPTS_VALUE ? options_.start_time : 0) / 1000000
            <= ((double)options_.duration / 1000000);
        if (pkt_in_play_range) {
            on_packet_(pkt, pkt->stream_index);
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
    return;

demux_fail:
    av_packet_free(&pkt);
    if (on_read_fatal_)
        on_read_fatal_();
}
