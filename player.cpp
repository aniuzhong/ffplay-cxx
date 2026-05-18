#include "player.h"

#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/bprint.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
}

#include "audio_output.h"
#include "clock.h"
#include "decoder.h"
#include "demuxer.h"
#include "frame.h"
#include "frame_queue.h"
#include "packet_queue.h"
#include "video_output.h"


#include "utils/cmdutils.h"

// ==========================================================================
//  v1: read ffplay.cpp globals directly.  v2: PlayerConfig struct.
// ==========================================================================
extern AVSyncType av_sync_type;
extern int audio_disable;
extern int video_disable;
extern int subtitle_disable;
extern int find_stream_info;
extern int genpts;
extern int seek_by_bytes;
extern int64_t start_time;
extern int64_t duration; // ffplay.cpp global; ::duration in prepare()
extern int loop;
extern int autoexit;
extern int infinite_buffer;
extern int show_status;
extern int startup_volume;
extern int lowres;
extern int fast;
extern int decoder_reorder_pts;
extern const char *audio_codec_name;
extern const char *subtitle_codec_name;
extern const char *video_codec_name;
extern const char *wanted_stream_spec[AVMEDIA_TYPE_NB];
extern const char *hwaccel;
extern char *afilters;
extern AVDictionary *format_opts;
extern AVDictionary *codec_opts;
extern AudioVisualizer::ShowMode show_mode;
extern int default_width;
extern int default_height;
extern int screen_width;
extern int screen_height;
extern int screen_left;
extern int screen_top;
extern const char *input_filename;
extern const char *window_title;

constexpr int MAX_QUEUE_SIZE = 15 * 1024 * 1024;
constexpr int MIN_FRAMES = 25;
constexpr int EXTERNAL_CLOCK_MIN_FRAMES = 2;
constexpr int EXTERNAL_CLOCK_MAX_FRAMES = 10;
constexpr double EXTERNAL_CLOCK_SPEED_MIN  = 0.900;
constexpr double EXTERNAL_CLOCK_SPEED_MAX  = 1.010;
constexpr double EXTERNAL_CLOCK_SPEED_STEP = 0.001;
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
constexpr double REFRESH_RATE = 0.01;
constexpr int VIDEO_PICTURE_QUEUE_SIZE = 3;
constexpr int SUBPICTURE_QUEUE_SIZE = 16;
constexpr int SAMPLE_QUEUE_SIZE = 9;

constexpr Uint32 FF_QUIT_EVENT = SDL_USEREVENT + 2;

// AudioVisualizer::ShowMode used via fully-qualified name

// ==========================================================================
//  Player construction / destruction
// ==========================================================================

Player::Player()
{
}

Player::~Player()
{
    stop();
}

// ==========================================================================
//  Device injection
// ==========================================================================

void Player::setDataSource(const char *url, const AVInputFormat *fmt)
{
    // Lazy construction in prepare() to avoid default-constructing Demuxer.
    // Store params for prepare().
    url_   = url;
    ifmt_  = fmt;
}

void Player::setVideoDevice(VideoOutput *dev)
{
    video_dev_ = dev;
}

void Player::setAudioDevice(AudioDevice *dev)
{
    audio_dev_ = dev;
}

void Player::rollbackPrepare()
{
    if (!dmx_)
        return;
    // prepare() does not call dmx_->start() — no read thread yet.
    if (sub_.stream_index >= 0)
        closeStream(sub_.stream_index);
    if (video_.stream_index >= 0)
        closeStream(video_.stream_index);
    if (audio_.stream_index >= 0)
        closeStream(audio_.stream_index);
    dmx_->stop();
    dmx_.reset();
}

// ==========================================================================
//  prepare() — stream_open equivalent
// ==========================================================================

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    return stream_id < 0 ||
           queue->abort_request() ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets() > MIN_FRAMES &&
               (!queue->duration() || av_q2d(st->time_base) * queue->duration() > 1.0);
}

static int create_hwaccel(AVBufferRef **device_ctx, VideoOutput *vout);

int Player::prepare()
{
    if (dmx_)
        return 0; // already prepared

    // -- Init frame queues --
    if (pictq_.init(VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        return -1;
    if (subpq_.init(SUBPICTURE_QUEUE_SIZE, 0) < 0)
        return -1;
    if (sampq_.init(SAMPLE_QUEUE_SIZE, 1) < 0)
        return -1;

    if (!videoq_.valid() || !audioq_.valid() || !subtitleq_.valid())
        return -1;

    av_sync_type_ = av_sync_type;

    // -- Demuxer setup --
    {
        DemuxerOptions opts;
        opts.find_stream_info = find_stream_info;
        opts.genpts           = genpts;
        opts.seek_by_bytes    = seek_by_bytes;
        opts.start_time       = start_time;
        opts.duration         = ::duration;
        opts.loop             = loop;
        opts.autoexit         = autoexit;
        opts.infinite_buffer  = infinite_buffer;
        opts.show_status      = show_status;
        opts.video_disable    = video_disable;
        opts.audio_disable    = audio_disable;
        opts.subtitle_disable = subtitle_disable;
        for (int i = 0; i < AVMEDIA_TYPE_NB; i++)
            opts.wanted_stream_spec[i] = wanted_stream_spec[i];

        dmx_ = std::make_unique<Demuxer>(url_.c_str(), ifmt_, opts);
    }

    if (dmx_->init(&format_opts, codec_opts) < 0) {
        dmx_.reset(); // allow retry; Demuxer leaves ic_ == nullptr on failure
        return -1;
    }

    /* read_thread: default window title from container metadata (ffplay.c ~2974). */
    if (!window_title) {
        const AVDictionaryEntry *t =
            av_dict_get(dmx_->ic()->metadata, "title", nullptr, 0);
        if (t) {
            char *w = av_asprintf("%s - %s", t->value, input_filename);
            if (w)
                window_title = w;
        }
    }

    seek_by_bytes = dmx_->seek_by_bytes();

    // -- Open streams --
    if (dmx_->audio_index() >= 0) {
        if (openStream(dmx_->audio_index()) < 0) {
            if (dmx_->video_index() < 0) {
                rollbackPrepare();
                return -1;
            }
        }
    }

    int video_open_ret = -1;
    if (dmx_->video_index() >= 0)
        video_open_ret = openStream(dmx_->video_index());

    if (dmx_->subtitle_index() >= 0) {
        if (openStream(dmx_->subtitle_index()) < 0) {
            av_log(nullptr, AV_LOG_WARNING,
                   "Could not open subtitle stream #%d\n",
                   dmx_->subtitle_index());
        }
    }

    if (video_.stream_index < 0 && audio_.stream_index < 0) {
        av_log(nullptr, AV_LOG_FATAL,
               "Failed to open file '%s' or configure filtergraph\n",
               url_.c_str());
        rollbackPrepare();
        return -1;
    }

    // Pure video file: no audio stream; if video decoder failed, both indices
    // can still be -1 only when openStream returned error before binding — if
    // video_.stream_index was set then init failed, we must not continue.
    if (video_open_ret < 0 && audio_.stream_index < 0) {
        rollbackPrepare();
        return -1;
    }

    // Match ffplay read_thread: ret from stream_component_open(video) drives default.
    if (audio_vis_.mode() == AudioVisualizer::ShowMode::None) {
        audio_vis_.set_mode(video_open_ret >= 0 ? AudioVisualizer::ShowMode::Video
                                                  : AudioVisualizer::ShowMode::Rdft);
    }
    show_mode = audio_vis_.mode();

    // -- Wire Demuxer callbacks --
    wireDemuxerCallbacks();

    return 0;
}

// ==========================================================================
//  openStream — stream_component_open equivalent
// ==========================================================================

int Player::openStream(int stream_index)
{
    AVFormatContext *ic = dmx_->ic();
    AVCodecContext *avctx;
    const AVCodec *codec;
    const char *forced_codec_name = nullptr;
    AVDictionary *opts = nullptr;
    int sample_rate;
    AVChannelLayout ch_layout = {};
    int ret = 0;
    int stream_lowres = lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0) goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        last_audio_stream_ = stream_index;
        forced_codec_name = audio_codec_name;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        last_subtitle_stream_ = stream_index;
        forced_codec_name = subtitle_codec_name;
        break;
    case AVMEDIA_TYPE_VIDEO:
        last_video_stream_ = stream_index;
        forced_codec_name = video_codec_name;
        break;
    default: break;
    }

    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(nullptr, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n", forced_codec_name);
        else
            av_log(nullptr, AV_LOG_WARNING,
                   "No decoder could be found for codec %s\n",
                   avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING,
               "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    ret = filter_codec_opts(codec_opts, avctx->codec_id, ic,
                            ic->streams[stream_index], codec, &opts, nullptr);
    if (ret < 0) goto fail;

    if (!av_dict_get(opts, "threads", nullptr, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        ret = create_hwaccel(&avctx->hw_device_ctx, video_dev_);
        if (ret < 0) goto fail;
    }

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0)
        goto fail;
    ret = check_avoptions(opts);
    if (ret < 0) goto fail;

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
        audio_.filter_src.freq = avctx->sample_rate;
        ret = av_channel_layout_copy(&audio_.filter_src.ch_layout, &avctx->ch_layout);
        if (ret < 0) goto fail;
        audio_.filter_src.fmt = avctx->sample_fmt;

        audio_.stream_index = stream_index;
        audio_.stream = ic->streams[stream_index];
        dmx_->set_audio_stream(stream_index, audio_.stream);

        if ((ret = audio_.init(avctx, &audioq_, &sampq_, dmx_.get(),
                               ic->streams[stream_index],
                               decoder_reorder_pts)) < 0)
            goto fail;
        if (dmx_->ic()->iformat->flags & AVFMT_NOTIMESTAMPS) {
            audio_.decoder.set_start_pts(audio_.stream->start_time,
                                          audio_.stream->time_base);
        }

        // Pre-configure audio filters so we know the correct output format
        // for the audio device.
        if (audio_dev_) {
            AudioParams audio_tgt;
            ret = audio_.configureFilters(afilters, 0, &audio_tgt,
                                           &sample_rate, &ch_layout);
        } else {
            ret = audio_.configureFilters(afilters, 0, nullptr,
                                           &sample_rate, &ch_layout);
        }
        if (ret < 0) goto fail;

        if (audio_dev_) {
            if ((ret = audio_dev_->open(&ch_layout, sample_rate,
                                        sdlAudioCallback, this)) < 0)
                goto fail;
        }
        break;
    }
    case AVMEDIA_TYPE_VIDEO:
        video_.stream_index = stream_index;
        video_.stream = ic->streams[stream_index];
        dmx_->set_video_stream(stream_index, video_.stream);

        if ((ret = video_.init(avctx, &videoq_, &pictq_, dmx_.get(),
                               ic->streams[stream_index],
                               decoder_reorder_pts)) < 0)
            goto fail;
        break;

    case AVMEDIA_TYPE_SUBTITLE:
        sub_.stream_index = stream_index;
        sub_.stream = ic->streams[stream_index];
        dmx_->set_subtitle_stream(stream_index, sub_.stream);

        if ((ret = sub_.init(avctx, &subtitleq_, &subpq_, dmx_.get(),
                             decoder_reorder_pts)) < 0)
            goto fail;
        break;

    default:
        ret = AVERROR(ENOTSUP);
        goto fail;
    }

    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);
    return ret;
}

// ==========================================================================
//  closeStream — stream_component_close equivalent
// ==========================================================================

void Player::closeStream(int stream_index)
{
    AVFormatContext *ic = dmx_->ic();
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    AVCodecParameters *codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        audio_.abort();
        audio_.releaseCodec();
        if (audio_dev_) audio_dev_->close();
        audio_vis_.destroy_rdft();
        break;
    case AVMEDIA_TYPE_VIDEO:
        video_.abort();
        video_.releaseCodec();
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        sub_.abort();
        sub_.releaseCodec();
        break;
    default: break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        audio_.stream = nullptr;
        audio_.stream_index = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        video_.stream = nullptr;
        video_.stream_index = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        sub_.stream = nullptr;
        sub_.stream_index = -1;
        break;
    default: break;
    }
}

// ==========================================================================
//  wireDemuxerCallbacks
// ==========================================================================

void Player::wireDemuxerCallbacks()
{
    dmx_->set_packet_handler([this](AVPacket *pkt, int stream_index) {
        if (!pkt) {
            if (stream_index == video_.stream_index && video_.stream_index >= 0)
                videoq_.put_null_packet(stream_index);
            else if (stream_index == audio_.stream_index && audio_.stream_index >= 0)
                audioq_.put_null_packet(stream_index);
            else if (stream_index == sub_.stream_index && sub_.stream_index >= 0)
                subtitleq_.put_null_packet(stream_index);
            return;
        }
        if (stream_index == audio_.stream_index)
            audioq_.put(pkt);
        else if (stream_index == video_.stream_index &&
                 !(video_.stream && video_.stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            videoq_.put(pkt);
        else if (stream_index == sub_.stream_index)
            subtitleq_.put(pkt);
        else
            av_packet_unref(pkt);
    });

    dmx_->set_seek_done_handler([this](int64_t target, int flags) {
        if (audio_.stream_index >= 0) audioq_.flush();
        if (sub_.stream_index >= 0) subtitleq_.flush();
        if (video_.stream_index >= 0) videoq_.flush();
        if (flags & AVSEEK_FLAG_BYTE)
            extclk_.set(NAN, 0);
        else
            extclk_.set(target / (double)AV_TIME_BASE, 0);
    });

    dmx_->set_seek_complete_handler([this]() {
        if (dmx_->is_paused())
            stepToNextFrame();
    });

    dmx_->set_queues_full_handler([this]() -> bool {
        if (dmx_->infinite_buffer() >= 1)
            return false;
        if (audioq_.size() + videoq_.size() + subtitleq_.size() > MAX_QUEUE_SIZE)
            return true;
        return stream_has_enough_packets(audio_.stream, audio_.stream_index, &audioq_) &&
               stream_has_enough_packets(video_.stream, video_.stream_index, &videoq_) &&
               stream_has_enough_packets(sub_.stream, sub_.stream_index, &subtitleq_);
    });

    dmx_->set_decoders_done_handler([this]() -> bool {
        if (dmx_->is_paused()) return false;
        bool audio_done = !audio_.stream ||
            (audio_.decoder.finished() == audioq_.serial() && sampq_.nb_remaining() == 0);
        bool video_done = !video_.stream ||
            (video_.decoder.finished() == videoq_.serial() && pictq_.nb_remaining() == 0);
        return audio_done && video_done;
    });

    dmx_->set_read_fatal_handler([this]() {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = this;
        SDL_PushEvent(&event);
    });
}

// ==========================================================================
//  start / pause / stop
// ==========================================================================

int Player::start()
{
    if (!dmx_)
        return -1;
    if (started_)
        return 0;

    if (audio_.stream_index >= 0 && audio_.stream)
        audio_.start(this);
    if (video_.stream_index >= 0 && video_.stream)
        video_.start(this);
    if (sub_.stream_index >= 0 && sub_.stream)
        sub_.start();

    if (audio_dev_)
        audio_dev_->unpause();

    dmx_->start();
    started_ = true;
    paused_ = false;
    return 0;
}

void Player::pause()
{
    if (!dmx_) return;

    bool was_paused = dmx_->is_paused();
    if (was_paused) {
        frame_timer_ += av_gettime_relative() / 1000000.0 - vidclk_.last_updated();
        if (dmx_->pause_supported())
            vidclk_.set_paused(false);
        vidclk_.set(vidclk_.get(videoq_.serial()), vidclk_.serial());
    }
    extclk_.set(extclk_.get(extclk_.serial()), extclk_.serial());

    bool now_paused = !was_paused;
    extclk_.set_paused(now_paused);
    vidclk_.set_paused(now_paused);
    audclk_.set_paused(now_paused);
    dmx_->set_paused(now_paused);
    paused_ = now_paused;
    step_ = 0;
}

void Player::stop()
{
    // Fixed release order (see header comment):
    // 1. demuxer / read thread
    // 2. decoder threads
    // 3. audio device
    // 4. codecs

    if (dmx_) {
        dmx_->stop();
    }

    if (audio_.stream_index >= 0)
        audio_.abort();
    if (video_.stream_index >= 0)
        video_.abort();
    if (sub_.stream_index >= 0)
        sub_.abort();

    if (audio_dev_)
        audio_dev_->close();

    audio_.releaseCodec();
    video_.releaseCodec();
    sub_.releaseCodec();

    dmx_.reset();
    started_ = false;
    paused_ = false;
}

// ==========================================================================
//  Query
// ==========================================================================

double Player::currentPosition() const
{
    return masterClock();
}

double Player::duration() const
{
    if (!dmx_) return 0;
    return dmx_->ic()->duration / (double)AV_TIME_BASE;
}

// ==========================================================================
//  seekTo / stepToNextFrame
// ==========================================================================

void Player::seekTo(double sec, double rel_sec)
{
    if (!dmx_) return;
    stream_seek(sec, rel_sec);
}

void Player::stream_seek(double pos_sec, double incr_sec)
{
    dmx_->seek(static_cast<int64_t>(pos_sec * AV_TIME_BASE),
               static_cast<int64_t>(incr_sec * AV_TIME_BASE), 0);
}

void Player::stepToNextFrame()
{
    if (!dmx_) return;
    if (dmx_->is_paused())
        pause(); // toggle unpause
    step_ = 1;
}

// ==========================================================================
//  cycleChannel
// ==========================================================================

void Player::cycleChannel(int codec_type)
{
    if (!dmx_) return;
    AVFormatContext *ic = dmx_->ic();

    int start_index, stream_index, old_index;
    AVStream *st;
    AVProgram *p = nullptr;
    int nb_streams = ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = last_video_stream_;
        old_index = video_.stream_index;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = last_audio_stream_;
        old_index = audio_.stream_index;
    } else {
        start_index = last_subtitle_stream_;
        old_index = sub_.stream_index;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && video_.stream_index != -1) {
        p = av_find_program_from_stream(ic, nullptr, video_.stream_index);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams) start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                last_subtitle_stream_ = -1;
                goto the_end;
            }
            if (start_index == -1) return;
            stream_index = 0;
        }
        if (stream_index == start_index) return;
        st = ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default: break;
            }
        }
    }
 the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(nullptr, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(static_cast<AVMediaType>(codec_type)),
           old_index, stream_index);

    closeStream(old_index);
    if (openStream(stream_index) < 0) {
        av_log(nullptr, AV_LOG_ERROR,
               "Could not open %s stream #%d; reverting to #%d\n",
               av_get_media_type_string(static_cast<AVMediaType>(codec_type)),
               stream_index, old_index);
        if (openStream(old_index) < 0) {
            av_log(nullptr, AV_LOG_FATAL,
                   "Failed to restore %s stream #%d after switch error.\n",
                   av_get_media_type_string(static_cast<AVMediaType>(codec_type)),
                   old_index);
        }
    }
}

// ==========================================================================
//  toggleMute / setVolume / setVideoFilter / toggleAudioDisplay
// ==========================================================================

void Player::toggleMute()
{
    if (audio_dev_)
        audio_dev_->toggle_mute();
}

void Player::setVolume(int v)
{
    if (audio_dev_)
        audio_dev_->set_volume(v);
}

void Player::setVideoFilter(int idx)
{
    vfilter_idx_ = idx;
    force_refresh_ = 1;
}

void Player::toggleAudioDisplay()
{
    AudioVisualizer::ShowMode next = AudioVisualizer::cycle_mode(
        audio_vis_.mode(),
        hasVideoStreamOpen(),
        hasAudioStreamOpen());
    if (audio_vis_.mode() != next) {
        force_refresh_ = 1;
        audio_vis_.set_mode(next);
        show_mode = audio_vis_.mode();
    }
}

// ==========================================================================
//  AV sync helpers
// ==========================================================================

AVSyncType Player::masterSyncType() const
{
    if (av_sync_type_ == AVSyncType::VideoMaster) {
        if (video_.stream) return AVSyncType::VideoMaster;
        else               return AVSyncType::AudioMaster;
    } else if (av_sync_type_ == AVSyncType::AudioMaster) {
        if (audio_.stream) return AVSyncType::AudioMaster;
        else               return AVSyncType::ExternalClock;
    } else {
        return AVSyncType::ExternalClock;
    }
}

double Player::masterClock() const
{
    switch (masterSyncType()) {
    case AVSyncType::VideoMaster:
        return vidclk_.get(videoq_.serial());
    case AVSyncType::AudioMaster:
        return audclk_.get(audioq_.serial());
    default:
        return extclk_.get(extclk_.serial());
    }
}

static void check_external_clock_speed(Player *p)
{
    const bool has_video = p->hasVideoStreamOpen();
    const bool has_audio = p->hasAudioStreamOpen();

    if ((has_video && p->videoq().nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES) ||
        (has_audio && p->audioq().nb_packets() <= EXTERNAL_CLOCK_MIN_FRAMES)) {
        p->extclk().set_speed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN,
                                    p->extclk().speed() - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((!has_video || p->videoq().nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (!has_audio || p->audioq().nb_packets() > EXTERNAL_CLOCK_MAX_FRAMES)) {
        p->extclk().set_speed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX,
                                    p->extclk().speed() + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        const double speed = p->extclk().speed();
        if (speed != 1.0) {
            p->extclk().set_speed(
                speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

// ==========================================================================
//  compute_target_delay
// ==========================================================================

static double compute_target_delay(double delay, Player *p)
{
    double sync_threshold, diff = 0;

    if (p->masterSyncType() != AVSyncType::VideoMaster) {
        diff = p->vidclk().get(p->videoq().serial()) - p->masterClock();

        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN,
                               FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < p->dmx()->max_frame_duration()) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
    return delay;
}

static double vp_duration(Player *p, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) {
        double d = nextvp->pts - vp->pts;
        if (isnan(d) || d <= 0 || d > p->dmx()->max_frame_duration())
            return vp->duration;
        else
            return d;
    }
    return 0.0;
}

static void update_video_pts(Player *p, double pts, int serial)
{
    p->vidclk().set(pts, serial);
    sync_clock_to_slave(&p->extclk(), p->extclk().serial(),
                        &p->vidclk(), p->videoq().serial());
}

// ==========================================================================
//  videoRefresh
// ==========================================================================

extern int display_disable;
extern int framedrop;
extern double rdftspeed;
extern int64_t audio_callback_time;

double Player::videoRefresh(double *remaining_time)
{
    double time;
    Frame *sp, *sp2;

    if (!dmx_->is_paused() && masterSyncType() == AVSyncType::ExternalClock &&
        dmx_->realtime())
        check_external_clock_speed(this);

    if (!display_disable && audio_vis_.is_visible() && audio_.stream) {
        time = av_gettime_relative() / 1000000.0;
        if (force_refresh_ || last_vis_time_ + rdftspeed < time) {
            if (video_dev_) {
                /* ffplay.c video_display: if (!is->width) video_open(is); — audio-only
                 * (-vn) never hits the video branch that opens the window / layout. */
                if (!video_dev_->width()) {
                    const int w = screen_width ? screen_width : default_width;
                    const int h = screen_height ? screen_height : default_height;
                    const char *title = window_title ? window_title : input_filename;
                    if (video_dev_->open(w, h, screen_left, screen_top,
                                         title ? title : "ffplay", 0) >= 0)
                        video_dev_->set_layout(Rect{0, 0, w, h});
                }
                video_dev_->display_audio_vis(&audio_vis_, audio_dev_,
                                               audio_callback_time,
                                               dmx_->is_paused());
            }
            last_vis_time_ = time;
        }
        *remaining_time = FFMIN(*remaining_time, last_vis_time_ + rdftspeed - time);
    }

    if (video_.stream) {
retry:
        if (pictq_.nb_remaining() == 0) {
            // nothing to do
        } else {
            double last_duration, dur, delay;
            Frame *vp, *lastvp;

            lastvp = pictq_.peek_last();
            vp = pictq_.peek();

            if (vp->serial != videoq_.serial()) {
                pictq_.next();
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                frame_timer_ = av_gettime_relative() / 1000000.0;

            if (dmx_->is_paused())
                goto display;

            last_duration = vp_duration(this, lastvp, vp);
            delay = compute_target_delay(last_duration, this);

            time = av_gettime_relative() / 1000000.0;
            if (time < frame_timer_ + delay) {
                *remaining_time = FFMIN(frame_timer_ + delay - time, *remaining_time);
                goto display;
            }

            frame_timer_ += delay;
            if (delay > 0 && time - frame_timer_ > AV_SYNC_THRESHOLD_MAX)
                frame_timer_ = time;

            {
                std::lock_guard lock(pictq_.mutex_ref());
                if (!isnan(vp->pts))
                    update_video_pts(this, vp->pts, vp->serial);
            }

            if (pictq_.nb_remaining() > 1) {
                Frame *nextvp = pictq_.peek_next();
                dur = vp_duration(this, vp, nextvp);
                if (!step_ &&
                    (framedrop > 0 || (framedrop && masterSyncType() != AVSyncType::VideoMaster)) &&
                    time > frame_timer_ + dur) {
                    frame_drops_late_++;
                    pictq_.next();
                    goto retry;
                }
            }

            if (sub_.stream) {
                while (subpq_.nb_remaining() > 0) {
                    sp = subpq_.peek();
                    sp2 = subpq_.nb_remaining() > 1 ? subpq_.peek_next() : nullptr;

                    if (sp->serial != subtitleq_.serial() ||
                        (vidclk_.pts() > (sp->pts + ((float)sp->sub.end_display_time / 1000))) ||
                        (sp2 && vidclk_.pts() > (sp2->pts + ((float)sp2->sub.start_display_time / 1000)))) {
                        if (video_dev_)
                            video_dev_->clear_subtitle_areas(sp);
                        subpq_.next();
                    } else {
                        break;
                    }
                }
            }

            pictq_.next();
            force_refresh_ = 1;

            if (step_ && !dmx_->is_paused())
                pause();
        }
display:
        if (!display_disable && force_refresh_ &&
            audio_vis_.mode() == AudioVisualizer::ShowMode::Video && pictq_.rindex_shown()) {
            if (video_dev_ && !video_dev_->width()) {
                // Match ffplay.c video_open: use default_* updated by first
                // queue_picture (set_default_window_size), screen_* overrides.
                const int w = screen_width ? screen_width : default_width;
                const int h = screen_height ? screen_height : default_height;
                const char *title = window_title ? window_title : input_filename;
                video_dev_->open(w, h, screen_left, screen_top,
                                 title ? title : "ffplay", 0);
                video_dev_->set_layout(Rect{0, 0, w, h});
            }
            if (video_dev_) {
                Frame *vp = pictq_.peek_last();
                Frame *sp_sub = sub_.stream && subpq_.nb_remaining() > 0
                                    ? subpq_.peek() : nullptr;
                video_dev_->display(vp, sp_sub);
            }
        }
    }
    force_refresh_ = 0;

    if (show_status) {
        AVBPrint buf;
        static int64_t last_status_time;
        const int64_t cur_time = av_gettime_relative();
        if (!last_status_time || (cur_time - last_status_time) >= 30000) {
            int aqsize = 0, vqsize = 0, sqsize = 0;
            double av_diff = 0;

            if (audio_.stream)
                aqsize = audioq_.size();
            if (video_.stream)
                vqsize = videoq_.size();
            if (sub_.stream)
                sqsize = subtitleq_.size();

            if (audio_.stream && video_.stream)
                av_diff = audclk_.get(audioq_.serial()) - vidclk_.get(videoq_.serial());
            else if (video_.stream)
                av_diff = masterClock() - vidclk_.get(videoq_.serial());
            else if (audio_.stream)
                av_diff = masterClock() - audclk_.get(audioq_.serial());

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                       "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB \r",
                       masterClock(),
                       (audio_.stream && video_.stream)
                           ? "A-V"
                           : (video_.stream ? "M-V" : (audio_.stream ? "M-A" : "   ")),
                       av_diff,
                       frame_drops_early_ + frame_drops_late_,
                       aqsize / 1024,
                       vqsize / 1024,
                       sqsize);

            if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(nullptr, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, nullptr);
            last_status_time = cur_time;
        }
    }

    return 0;
}

// ==========================================================================
//  SDL audio callback
// ==========================================================================

extern int64_t audio_callback_time; // defined in ffplay.cpp

void Player::sdlAudioCallback(void *opaque, Uint8 *stream, int len)
{
    Player *p = static_cast<Player *>(opaque);
    audio_callback_time = av_gettime_relative();

    if (!p->audio_dev_) return;

    // For now, delegate to AudioOutput::read() directly.
    // TODO: replace with AudioPipeline::fill() once decode+sync migrate.
    AudioDevice *dev = p->audio_dev_;
    if (auto *aout = dynamic_cast<AudioOutput *>(dev)) {
        // Use the existing read path
        auto next_frame = [&]() -> Frame * {
            Frame *af = nullptr;
            do {
                // ... peeking logic from ffplay.cpp ...
                af = p->sampq_.peek_readable();
                if (!af) return (Frame *)nullptr;
                p->sampq_.next();
            } while (af->serial != p->audioq_.serial());
            return af;
        };

        auto sync_diff = [&]() -> double {
            if (p->masterSyncType() == AVSyncType::AudioMaster)
                return NAN;
            return p->audclk_.get(p->audioq_.serial()) - p->masterClock();
        };

        std::function<void(const int16_t *, int)> on_decode =
            [p](const int16_t *samples, int count) {
                p->audio_vis_.feed(samples, count);
            };

        aout->read(stream, len, p->dmx_->is_paused(),
                   next_frame, sync_diff, &on_decode);

        if (!isnan(aout->clock())) {
            p->audclk_.set_at(aout->clock_for_set_at(),
                              aout->clock_serial(),
                              audio_callback_time / 1000000.0);
            sync_clock_to_slave(&p->extclk_, p->extclk_.serial(),
                                &p->audclk_, p->audioq_.serial());
        }
    }
}

// ==========================================================================
//  create_hwaccel
// ==========================================================================

static int create_hwaccel(AVBufferRef **device_ctx, VideoOutput *vout)
{
    *device_ctx = nullptr;
    if (!hwaccel) return 0;

    AVHWDeviceType type = av_hwdevice_find_type_by_name(hwaccel);
    if (type == AV_HWDEVICE_TYPE_NONE)
        return AVERROR(ENOTSUP);

    // Vulkan-derived path: share the renderer's Vulkan device for zero-copy.
    AVBufferRef *vk_dev = vout ? vout->hw_device_ref() : nullptr;
    if (vk_dev) {
        int ret = av_hwdevice_ctx_create_derived(device_ctx, type, vk_dev, 0);
        if (!ret) return 0;
        if (ret != AVERROR(ENOSYS)) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Failed to create derived hwaccel for %s: %s\n",
                   hwaccel, av_err2str(ret));
            return ret;
        }
        // ENOSYS: this hwaccel doesn't support Vulkan derivation,
        // fall back to standalone device.
        av_log(nullptr, AV_LOG_WARNING,
               "Derive %s from vulkan not supported, "
               "falling back to standalone device.\n", hwaccel);
    }

    return av_hwdevice_ctx_create(device_ctx, type, nullptr, nullptr, 0);
}
