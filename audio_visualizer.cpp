#include "audio_visualizer.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
#include <libavutil/tx.h>
}

AudioVisualizer::AudioVisualizer()
    : display_ring_(SAMPLE_ARRAY_SIZE, 0)
{
}

void AudioVisualizer::feed(const int16_t *samples, int count)
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    while (count > 0) {
        int len = SAMPLE_ARRAY_SIZE - sample_array_index_;
        if (len > count)
            len = count;
        memcpy(sample_array_ + sample_array_index_, samples, len * sizeof(int16_t));
        samples += len;
        sample_array_index_ += len;
        if (sample_array_index_ >= SAMPLE_ARRAY_SIZE)
            sample_array_index_ = 0;
        count -= len;
    }
}

void AudioVisualizer::sync_display_ring()
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    memcpy(display_ring_.data(), sample_array_,
           static_cast<size_t>(SAMPLE_ARRAY_SIZE) * sizeof(int16_t));
    display_ring_index_ = sample_array_index_;
}

int16_t AudioVisualizer::sample_at(int index) const
{
    const int i = compute_mod(index, SAMPLE_ARRAY_SIZE);
    return display_ring_[static_cast<size_t>(i)];
}

AudioVisualizer::ShowMode AudioVisualizer::cycle_mode(ShowMode current, bool has_video, bool has_audio)
{
    ShowMode next = current;
    int nb_modes = static_cast<int>(ShowMode::Nb);
    do {
        next = static_cast<ShowMode>((static_cast<int>(next) + 1) % nb_modes);
    } while (next != current &&
             (next == ShowMode::Video && !has_video ||
              next != ShowMode::Video && !has_audio));
    return next;
}

bool AudioVisualizer::is_visible() const
{
    return show_mode_ == ShowMode::Waves || show_mode_ == ShowMode::Rdft;
}

int AudioVisualizer::compute_start_index(int channels, int audio_write_buf_size,
                                          int64_t audio_callback_time, int sample_rate,
                                          bool paused, int data_used) const
{
    if (paused)
        return last_i_start_;

    int n = 2 * channels;
    int64_t delay = audio_write_buf_size;
    delay /= n;

    if (audio_callback_time) {
        int64_t time_diff = av_gettime_relative() - audio_callback_time;
        delay -= (time_diff * sample_rate) / 1000000;
    }

    delay += 2 * data_used;
    if (delay < data_used)
        delay = data_used;

    return compute_mod(display_ring_index_ - delay * channels, SAMPLE_ARRAY_SIZE);
}

int AudioVisualizer::prepare_waveform(int channels, int audio_write_buf_size,
                                       int64_t audio_callback_time, int sample_rate,
                                       bool paused, int display_width)
{
    int i_start = compute_start_index(channels, audio_write_buf_size,
                                       audio_callback_time, sample_rate,
                                       paused, display_width);

    if (!paused) {
        /* Peak detection: refine i_start by looking for a zero-crossing */
        int x = i_start;
        int h = INT_MIN;
        for (int i = 0; i < 1000; i += channels) {
            int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
            int a = display_ring_[static_cast<size_t>(idx)];
            int b = display_ring_[static_cast<size_t>((idx + 4 * channels) % SAMPLE_ARRAY_SIZE)];
            int c = display_ring_[static_cast<size_t>((idx + 5 * channels) % SAMPLE_ARRAY_SIZE)];
            int d = display_ring_[static_cast<size_t>((idx + 9 * channels) % SAMPLE_ARRAY_SIZE)];
            int score = a - d;
            if (h < score && (b ^ c) < 0) {
                h = score;
                i_start = idx;
            }
        }
    }

    last_i_start_ = i_start;
    return i_start;
}

int AudioVisualizer::prepare_spectrum_column(int channels, int audio_write_buf_size,
                                              int64_t audio_callback_time, int sample_rate,
                                              bool paused, int display_width, int display_height)
{
    /* Compute RDFT size from display height */
    int rdft_bits = 0;
    for (rdft_bits = 1; (1 << rdft_bits) < 2 * display_height; rdft_bits++)
        ;
    int nb_freq = 1 << (rdft_bits - 1);

    nb_display_channels_ = FFMIN(channels, 2);

    int i_start = compute_start_index(channels, audio_write_buf_size,
                                       audio_callback_time, sample_rate,
                                       paused, 2 * nb_freq);
    last_i_start_ = i_start;

    /* Scroll position */
    if (xpos_ >= display_width)
        xpos_ = 0;

    /* Reinitialize RDFT context if size changed — only commit rdft_bits_ after full success */
    int err = 0;
    if (rdft_bits != rdft_bits_) {
        const float rdft_scale = 1.0f;
        av_tx_uninit(&rdft_);
        rdft_fn_ = nullptr;
        av_freep(&real_data_);
        av_freep(&rdft_data_);

        real_data_ = static_cast<float *>(av_malloc_array(nb_freq, 4 * sizeof(*real_data_)));
        rdft_data_ = static_cast<AVComplexFloat *>(av_malloc_array(nb_freq + 1, 2 * sizeof(*rdft_data_)));
        if (!real_data_ || !rdft_data_) {
            err = AVERROR(ENOMEM);
        } else {
            err = av_tx_init(&rdft_, &rdft_fn_, AV_TX_FLOAT_RDFT,
                             0, 1 << rdft_bits, &rdft_scale, 0);
        }
        if (err < 0 || !rdft_fn_) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Failed to allocate buffers for RDFT, switching to waves display\n");
            av_tx_uninit(&rdft_);
            rdft_fn_ = nullptr;
            av_freep(&real_data_);
            av_freep(&rdft_data_);
            rdft_bits_ = 0;
            return -1;
        }
        rdft_bits_ = rdft_bits;
    }

    if (!rdft_data_ || !rdft_fn_) {
        av_log(nullptr, AV_LOG_ERROR,
               "Failed to allocate buffers for RDFT, switching to waves display\n");
        return -1;
    }

    nb_freq_ = nb_freq;

    /* Window samples and run RDFT */
    float *data_in[2];
    AVComplexFloat *data[2];
    for (int ch = 0; ch < nb_display_channels_; ch++) {
        data_in[ch] = real_data_ + 2 * nb_freq * ch;
        data[ch] = rdft_data_ + nb_freq * ch;
        int i = i_start + ch;
        for (int x = 0; x < 2 * nb_freq; x++) {
            double w = (x - nb_freq) * (1.0 / nb_freq);
            data_in[ch][x] = display_ring_[static_cast<size_t>(i)] * (1.0 - w * w);
            i += channels;
            if (i >= SAMPLE_ARRAY_SIZE)
                i -= SAMPLE_ARRAY_SIZE;
        }
        rdft_fn_(rdft_, data[ch], data_in[ch], sizeof(float));
        data[ch][0].im = data[ch][nb_freq].re;
        data[ch][nb_freq].re = 0;
    }

    if (!paused)
        xpos_++;

    return 0;
}

const AVComplexFloat *AudioVisualizer::spectrum_data(int ch) const
{
    return rdft_data_ + nb_freq_ * ch;
}

void AudioVisualizer::destroy_rdft()
{
    if (rdft_) {
        av_tx_uninit(&rdft_);
    }
    av_freep(&real_data_);
    av_freep(&rdft_data_);
    rdft_ = nullptr;
    rdft_bits_ = 0;
}

void AudioVisualizer::destroy()
{
    destroy_rdft();
}

AudioVisualizer::~AudioVisualizer()
{
    destroy();
}
