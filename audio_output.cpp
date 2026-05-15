#include "audio_output.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
}

#include "frame_queue.h"
#include "packet_queue.h"

// From ffplay.cpp globals
extern int64_t audio_callback_time;

constexpr int SDL_AUDIO_MIN_BUFFER_SIZE = 512;
constexpr int SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;
constexpr int SAMPLE_CORRECTION_PERCENT_MAX = 10;
constexpr double AV_NOSYNC_THRESHOLD = 10.0;
constexpr int AUDIO_DIFF_AVG_NB = 20;

namespace {

void audio_params_clear(AudioParams *p)
{
    av_channel_layout_uninit(&p->ch_layout);
    p->freq          = 0;
    p->fmt           = AV_SAMPLE_FMT_NONE;
    p->frame_size    = 0;
    p->bytes_per_sec = 0;
}

/** Replaces dst layout with src (decoder-side mirror of hardware tgt). */
int audio_params_copy_from(const AudioParams *src, AudioParams *dst)
{
    av_channel_layout_uninit(&dst->ch_layout);
    dst->freq          = src->freq;
    dst->fmt           = src->fmt;
    dst->frame_size    = src->frame_size;
    dst->bytes_per_sec = src->bytes_per_sec;
    return av_channel_layout_copy(&dst->ch_layout, &src->ch_layout);
}

} // namespace

AudioOutput::AudioOutput(FrameQueue *sampq, PacketQueue *audioq)
    : sampq_(sampq), audioq_(audioq)
{
}

AudioOutput::~AudioOutput()
{
    close();
}

int AudioOutput::open(const AVChannelLayout *ch_layout, int sample_rate,
                       SDL_AudioCallback cb, void *cb_opaque)
{
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    if (audio_dev_) {
        av_log(nullptr, AV_LOG_ERROR, "AudioOutput::open: device already open\n");
        return AVERROR(EINVAL);
    }
    audio_params_clear(&audio_src_);
    audio_params_clear(&audio_tgt_);

    AVChannelLayout wanted_ch_layout;
    if (av_channel_layout_copy(&wanted_ch_layout, ch_layout) < 0) {
        av_channel_layout_uninit(&wanted_ch_layout);
        return -1;
    }

    int wanted_nb_channels = wanted_ch_layout.nb_channels;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(&wanted_ch_layout);
        av_channel_layout_default(&wanted_ch_layout, wanted_nb_channels);
    }
    if (wanted_ch_layout.order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(&wanted_ch_layout);
        av_channel_layout_default(&wanted_ch_layout, wanted_nb_channels);
    }
    wanted_nb_channels = wanted_ch_layout.nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        av_channel_layout_uninit(&wanted_ch_layout);
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = cb;
    wanted_spec.userdata = cb_opaque;

    while (!(audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec,
                                              SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                              SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(nullptr, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                av_channel_layout_uninit(&wanted_ch_layout);
                return -1;
            }
        }
        av_channel_layout_default(&wanted_ch_layout, wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(nullptr, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
        av_channel_layout_uninit(&wanted_ch_layout);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(&wanted_ch_layout);
        av_channel_layout_default(&wanted_ch_layout, spec.channels);
        if (wanted_ch_layout.order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(nullptr, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            SDL_CloseAudioDevice(audio_dev_);
            audio_dev_ = 0;
            av_channel_layout_uninit(&wanted_ch_layout);
            return -1;
        }
    }

    audio_tgt_.fmt = AV_SAMPLE_FMT_S16;
    audio_tgt_.freq = spec.freq;
    if (av_channel_layout_copy(&audio_tgt_.ch_layout, &wanted_ch_layout) < 0) {
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
        av_channel_layout_uninit(&wanted_ch_layout);
        audio_params_clear(&audio_tgt_);
        return -1;
    }
    audio_tgt_.frame_size = av_samples_get_buffer_size(nullptr,
        audio_tgt_.ch_layout.nb_channels, 1, audio_tgt_.fmt, 1);
    audio_tgt_.bytes_per_sec = av_samples_get_buffer_size(nullptr,
        audio_tgt_.ch_layout.nb_channels, audio_tgt_.freq, audio_tgt_.fmt, 1);
    if (audio_tgt_.bytes_per_sec <= 0 || audio_tgt_.frame_size <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
        av_channel_layout_uninit(&wanted_ch_layout);
        audio_params_clear(&audio_tgt_);
        return -1;
    }
    audio_hw_buf_size_ = spec.size;

    av_channel_layout_uninit(&wanted_ch_layout);

    if (audio_params_copy_from(&audio_tgt_, &audio_src_) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "AudioOutput::open: failed to copy hw layout to src\n");
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
        audio_params_clear(&audio_tgt_);
        return AVERROR(ENOMEM);
    }
    audio_buf_size_  = 0;
    audio_buf_index_ = 0;

    audio_diff_avg_coef_  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    audio_diff_avg_count_ = 0;
    audio_diff_threshold_ = (double)audio_hw_buf_size_ / audio_tgt_.bytes_per_sec;

    return audio_hw_buf_size_;
}

double AudioOutput::clock_for_set_at() const
{
    if (isnan(audio_clock_))
        return NAN;
    return audio_clock_
        - (double)(2 * audio_hw_buf_size_ + audio_write_buf_size_)
        / audio_tgt_.bytes_per_sec;
}

int AudioOutput::synchronize(int nb_samples, double sync_diff)
{
    if (isnan(sync_diff))
        return nb_samples;

    if (fabs(sync_diff) >= AV_NOSYNC_THRESHOLD) {
        audio_diff_avg_count_ = 0;
        audio_diff_cum_       = 0;
        return nb_samples;
    }
    audio_diff_cum_ = sync_diff + audio_diff_avg_coef_ * audio_diff_cum_;
    if (audio_diff_avg_count_ < AUDIO_DIFF_AVG_NB) {
        audio_diff_avg_count_++;
        return nb_samples;
    }
    double avg = audio_diff_cum_ * (1.0 - audio_diff_avg_coef_);
    if (fabs(avg) < audio_diff_threshold_)
        return nb_samples;
    int wanted_nb_samples = nb_samples + (int)(sync_diff * audio_src_.freq);
    int min = nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100;
    int max = nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100;
    wanted_nb_samples = av_clip(wanted_nb_samples, min, max);
    av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
           sync_diff, avg, wanted_nb_samples - nb_samples,
           audio_clock_, audio_diff_threshold_);
    return wanted_nb_samples;
}

int AudioOutput::decode_frame(bool paused,
                               const std::function<double()> &sync_diff_fn,
                               const std::function<void(const int16_t *, int)> *on_decode)
{
    if (paused)
        return -1;

    int data_size, resampled_data_size;
    Frame *af;

    do {
#if defined(_WIN32)
        while (sampq_->nb_remaining() == 0) {
            if ((av_gettime_relative() - audio_callback_time)
                > 1000000LL * audio_hw_buf_size_ / audio_tgt_.bytes_per_sec / 2)
                return -1;
            av_usleep(1000);
        }
#endif
        if (!(af = sampq_->peek_readable()))
            return -1;
        sampq_->next();
    } while (af->serial != audioq_->serial());

    data_size = av_samples_get_buffer_size(nullptr, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           static_cast<AVSampleFormat>(af->frame->format), 1);

    double sync_diff = sync_diff_fn();
    int wanted_nb_samples = synchronize(af->frame->nb_samples, sync_diff);

    if (af->frame->format        != audio_src_.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &audio_src_.ch_layout) ||
        af->frame->sample_rate   != audio_src_.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !swr_ctx_)) {
        swr_free(&swr_ctx_);
        int ret = swr_alloc_set_opts2(&swr_ctx_,
                         &audio_tgt_.ch_layout, audio_tgt_.fmt, audio_tgt_.freq,
                         &af->frame->ch_layout,
                         static_cast<AVSampleFormat>(af->frame->format),
                         af->frame->sample_rate,
                         0, nullptr);
        if (ret < 0 || swr_init(swr_ctx_) < 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion "
                   "of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate,
                   av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->frame->format)),
                   af->frame->ch_layout.nb_channels,
                   audio_tgt_.freq, av_get_sample_fmt_name(audio_tgt_.fmt),
                   audio_tgt_.ch_layout.nb_channels);
            swr_free(&swr_ctx_);
            return -1;
        }
        if (av_channel_layout_copy(&audio_src_.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        audio_src_.freq = af->frame->sample_rate;
        audio_src_.fmt = static_cast<AVSampleFormat>(af->frame->format);
    }

    if (swr_ctx_) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &audio_buf1_;
        int out_count = (int64_t)wanted_nb_samples * audio_tgt_.freq
                        / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(nullptr,
                            audio_tgt_.ch_layout.nb_channels,
                            out_count, audio_tgt_.fmt, 0);
        if (out_size < 0) {
            av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(swr_ctx_,
                    (wanted_nb_samples - af->frame->nb_samples)
                        * audio_tgt_.freq / af->frame->sample_rate,
                    wanted_nb_samples * audio_tgt_.freq
                        / af->frame->sample_rate) < 0) {
                av_log(nullptr, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&audio_buf1_, &audio_buf1_size_, out_size);
        if (!audio_buf1_)
            return AVERROR(ENOMEM);
        int len2 = swr_convert(swr_ctx_, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(nullptr, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(nullptr, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(swr_ctx_) < 0)
                swr_free(&swr_ctx_);
        }
        audio_buf_ = audio_buf1_;
        resampled_data_size = len2 * audio_tgt_.ch_layout.nb_channels
                              * av_get_bytes_per_sample(audio_tgt_.fmt);
    } else {
        audio_buf_ = af->frame->data[0];
        resampled_data_size = data_size;
    }

    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        audio_clock_ = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
    else
        audio_clock_ = NAN;
    audio_clock_serial_ = af->serial;

    if (on_decode && resampled_data_size > 0)
        (*on_decode)(reinterpret_cast<const int16_t *>(audio_buf_),
                     resampled_data_size / sizeof(int16_t));

    return resampled_data_size;
}

void AudioOutput::read(uint8_t *stream, int len, bool paused,
                        std::function<double()> sync_diff_fn,
                        const std::function<void(const int16_t *, int)> *on_decode)
{
    while (len > 0) {
        if (audio_buf_index_ >= (int)audio_buf_size_) {
            int audio_size = decode_frame(paused, sync_diff_fn, on_decode);
            if (audio_size < 0) {
                /* if error, just output silence */
                audio_buf_ = nullptr;
                audio_buf_size_ = SDL_AUDIO_MIN_BUFFER_SIZE
                    / audio_tgt_.frame_size * audio_tgt_.frame_size;
            } else {
                audio_buf_size_ = audio_size;
            }
            audio_buf_index_ = 0;
        }
        int len1 = audio_buf_size_ - audio_buf_index_;
        if (len1 > len)
            len1 = len;
        if (!muted_ && audio_buf_ && audio_volume_ == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)audio_buf_ + audio_buf_index_, len1);
        else {
            memset(stream, 0, len1);
            if (!muted_ && audio_buf_)
                SDL_MixAudioFormat(stream,
                                   (uint8_t *)audio_buf_ + audio_buf_index_,
                                   AUDIO_S16SYS, len1, audio_volume_);
        }
        len -= len1;
        stream += len1;
        audio_buf_index_ += len1;
    }
    audio_write_buf_size_ = audio_buf_size_ - audio_buf_index_;
}

void AudioOutput::unpause()
{
    if (audio_dev_)
        SDL_PauseAudioDevice(audio_dev_, 0);
}

void AudioOutput::close()
{
    if (audio_dev_) {
        SDL_CloseAudioDevice(audio_dev_);
        audio_dev_ = 0;
    }
    swr_free(&swr_ctx_);
    av_freep(&audio_buf1_);
    audio_buf_           = nullptr;
    audio_buf_size_      = 0;
    audio_buf1_size_     = 0;
    audio_buf_index_     = 0;
    audio_write_buf_size_ = 0;
    audio_params_clear(&audio_src_);
    audio_params_clear(&audio_tgt_);
}
