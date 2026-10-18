#include "Voice.h"

#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <algorithm>
#include <cmath>

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#endif

namespace tt::app::devuelta {

constexpr auto* TAG = "DeVuelta";

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t FRAME_SAMPLES = SAMPLE_RATE / 50;
constexpr int MAX_RECORD_FRAMES = 20 * 50;
constexpr int CALIBRATION_FRAMES = 10;
constexpr int SPEECH_START_FRAMES = 3;
constexpr int SPEECH_END_FRAMES = 60;
constexpr int NO_SPEECH_TIMEOUT_FRAMES = 7 * 50;
constexpr float MIN_SPEECH_RMS = 300.0f;
constexpr size_t PLAY_CHUNK_SAMPLES = 1024;

static float getRms(const int16_t* samples, size_t count) {
    double sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return count > 0 ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
}

RecordResult record(Device* stream, Pcm& audio, const std::function<void(float)>& onLevel, const std::function<bool()>& isStopRequested) {
    const AudioStreamConfig config { SAMPLE_RATE, 16, 1 };
    AudioStreamHandle handle = nullptr;
    if (audio_stream_open_input(stream, &config, &handle) != ERROR_NONE) {
        LOG_E(TAG, "Failed to open the microphone");
        return RecordResult::Failed;
    }

    audio.sampleRate = SAMPLE_RATE;
    audio.samples.clear();
    if (!audio.samples.reserve(MAX_RECORD_FRAMES * FRAME_SAMPLES * sizeof(int16_t))) {
        audio_stream_close(handle);
        return RecordResult::Failed;
    }

    int16_t frame[FRAME_SAMPLES];
    float noiseFloor = 0;
    int loudFrames = 0;
    int quietFrames = 0;
    bool speaking = false;
    RecordResult result = RecordResult::Spoken;

    for (int index = 0; index < MAX_RECORD_FRAMES; index++) {
        size_t bytesRead = 0;
        if (audio_stream_read(handle, frame, sizeof(frame), &bytesRead, pdMS_TO_TICKS(200)) != ERROR_NONE || bytesRead == 0) {
            result = RecordResult::Failed;
            break;
        }
        const size_t count = bytesRead / sizeof(int16_t);
        audio.samples.append(frame, count * sizeof(int16_t));

        const float rms = getRms(frame, count);
        onLevel(std::min(1.0f, rms / 4000.0f));

        if (index < CALIBRATION_FRAMES) {
            noiseFloor += rms / CALIBRATION_FRAMES;
            continue;
        }

        const float threshold = std::max(noiseFloor * 2.5f, MIN_SPEECH_RMS);
        if (rms > threshold) {
            loudFrames++;
            quietFrames = 0;
            if (loudFrames >= SPEECH_START_FRAMES) {
                speaking = true;
            }
        } else {
            loudFrames = 0;
            quietFrames++;
        }

        if (speaking && quietFrames >= SPEECH_END_FRAMES) {
            break;
        }
        if (!speaking && index >= NO_SPEECH_TIMEOUT_FRAMES) {
            result = RecordResult::Silence;
            break;
        }
        if (isStopRequested()) {
            result = speaking ? RecordResult::Spoken : RecordResult::Cancelled;
            break;
        }
    }

    audio_stream_close(handle);
    onLevel(0.0f);
    return result;
}

bool play(Device* stream, const Pcm& audio, const std::function<bool()>& isStopRequested) {
    const AudioStreamConfig config { audio.sampleRate, 16, 1 };
    AudioStreamHandle handle = nullptr;
    if (audio_stream_open_output(stream, &config, &handle) != ERROR_NONE) {
        LOG_E(TAG, "Failed to open the speaker");
        return false;
    }

    bool completed = true;
    const int16_t* samples = audio.getSamples();
    const size_t total = audio.getSampleCount();
    for (size_t offset = 0; offset < total; offset += PLAY_CHUNK_SAMPLES) {
        if (isStopRequested()) {
            completed = false;
            break;
        }
        const size_t count = std::min(PLAY_CHUNK_SAMPLES, total - offset);
        size_t bytesWritten = 0;
        if (audio_stream_write(handle, samples + offset, count * sizeof(int16_t), &bytesWritten, pdMS_TO_TICKS(1000)) != ERROR_NONE) {
            completed = false;
            break;
        }
    }

    audio_stream_close(handle);
    return completed;
}

struct Note {
    float frequency;
    int milliseconds;
};

static void appendNote(Pcm& pcm, const Note& note) {
    const size_t count = SAMPLE_RATE * note.milliseconds / 1000;
    const size_t fade = SAMPLE_RATE / 100;
    for (size_t i = 0; i < count; i++) {
        float envelope = 1.0f;
        if (i < fade) {
            envelope = static_cast<float>(i) / fade;
        } else if (count - i < fade) {
            envelope = static_cast<float>(count - i) / fade;
        }
        const float value = note.frequency > 0 ? std::sin(2.0f * static_cast<float>(M_PI) * note.frequency * i / SAMPLE_RATE) : 0.0f;
        const auto sample = static_cast<int16_t>(value * envelope * 9000.0f);
        pcm.samples.append(&sample, sizeof(sample));
    }
}

void playTone(Device* stream, Tone tone) {
    Pcm pcm;
    pcm.sampleRate = SAMPLE_RATE;
    switch (tone) {
        case Tone::Listen:
            appendNote(pcm, { 660, 90 });
            appendNote(pcm, { 990, 130 });
            break;
        case Tone::Done:
            appendNote(pcm, { 990, 90 });
            appendNote(pcm, { 660, 130 });
            break;
        case Tone::Alarm:
            for (int i = 0; i < 3; i++) {
                appendNote(pcm, { 880, 180 });
                appendNote(pcm, { 0, 120 });
            }
            break;
    }
    play(stream, pcm, [] { return false; });
}

Button::Button(int pin) : pin(pin) {
#ifdef ESP_PLATFORM
    gpio_config_t config {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&config);
#endif
}

bool Button::poll() {
#ifdef ESP_PLATFORM
    const bool pressed = gpio_get_level(static_cast<gpio_num_t>(pin)) == 0;
#else
    const bool pressed = false;
#endif
    if (!pressed) {
        pressedCount = 0;
        reported = false;
        return false;
    }
    pressedCount++;
    if (pressedCount >= 2 && !reported) {
        reported = true;
        return true;
    }
    return false;
}

}
