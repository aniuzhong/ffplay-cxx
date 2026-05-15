#pragma once

#include <cstdint>

extern "C" {
#include <libavutil/tx.h>
}

class AudioVisualizer {
public:
    enum class ShowMode {
        None = -1, Video = 0, Waves, Rdft, Nb
    };

    AudioVisualizer() = default;
    ~AudioVisualizer();

    AudioVisualizer(const AudioVisualizer &) = delete;
    AudioVisualizer &operator=(const AudioVisualizer &) = delete;

    // Feed PCM samples from audio callback (replaces update_sample_display)
    void feed(const int16_t *samples, int count);

    // Mode management
    ShowMode mode() const { return show_mode_; }
    void set_mode(ShowMode m) { show_mode_ = m; }
    static ShowMode cycle_mode(ShowMode current, bool has_video, bool has_audio);
    bool is_visible() const;

    // Prepare for waveform rendering. Returns start index into ring buffer.
    int prepare_waveform(int channels, int audio_write_buf_size,
                         int64_t audio_callback_time, int sample_rate,
                         bool paused, int display_width);

    // Prepare one column of spectrum. Returns 0 on success, negative on error.
    int prepare_spectrum_column(int channels, int audio_write_buf_size,
                                int64_t audio_callback_time, int sample_rate,
                                bool paused, int display_width, int display_height);

    // Ring buffer access (for waveform rendering)
    static constexpr int SAMPLE_ARRAY_SIZE = 8 * 65536;
    static int compute_mod(int a, int b) { return a < 0 ? a % b + b : a % b; }
    int16_t sample_at(int index) const { return sample_array_[index]; }

    // Spectrum output (after prepare_spectrum_column)
    int nb_freq() const { return nb_freq_; }
    int nb_display_channels() const { return nb_display_channels_; }
    const AVComplexFloat *spectrum_data(int ch) const;

    // Scroll position for RDFT texture
    int xpos() const { return xpos_; }

    // Cleanup
    void destroy_rdft();
    void destroy();

private:
    int compute_start_index(int channels, int audio_write_buf_size,
                            int64_t audio_callback_time, int sample_rate,
                            bool paused, int data_used) const;

    int16_t sample_array_[SAMPLE_ARRAY_SIZE] = {};
    int sample_array_index_ = 0;

    ShowMode show_mode_ = ShowMode::None;

    int last_i_start_ = 0;

    AVTXContext *rdft_ = nullptr;
    av_tx_fn rdft_fn_ = nullptr;
    int rdft_bits_ = 0;
    float *real_data_ = nullptr;
    AVComplexFloat *rdft_data_ = nullptr;
    int nb_freq_ = 0;
    int nb_display_channels_ = 0;

    int xpos_ = 0;
};
