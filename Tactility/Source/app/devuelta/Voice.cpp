#include "Voice.h"

#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <tactility/freertos/freertos.h>
#include <tactility/freertos/semphr.h>
#include <tactility/freertos/task.h>

#include <atomic>

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <freertos/stream_buffer.h>
#include <sdkconfig.h>
#else
#include <stream_buffer.h>
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
constexpr float MIN_SPEECH_RMS = 120.0f;
constexpr float SPEECH_FACTOR = 2.0f;
/** Below the speech threshold, but enough to send the audio anyway when nothing clearer was heard */
constexpr float SOFT_SPEECH_FACTOR = 1.5f;
/** Frames this quiet are digital silence from the codec starting up, not room noise */
constexpr float DIGITAL_SILENCE_RMS = 2.0f;
constexpr float NORMALIZED_PEAK = 22000.0f;
constexpr float MAX_GAIN = 8.0f;
constexpr size_t PLAY_CHUNK_SAMPLES = 1024;
constexpr uint32_t PADDING_MILLIS = 40;
constexpr uint32_t FADE_IN_SAMPLES = SAMPLE_RATE / 200;

/** RMS without the DC offset of the microphone */
static float getRms(const int16_t* samples, size_t count) {
    if (count == 0) {
        return 0.0f;
    }
    double mean = 0;
    for (size_t i = 0; i < count; i++) {
        mean += samples[i];
    }
    mean /= count;
    double sum = 0;
    for (size_t i = 0; i < count; i++) {
        const double value = samples[i] - mean;
        sum += value * value;
    }
    return static_cast<float>(std::sqrt(sum / count));
}

/** Amplifies quiet recordings so Gemini hears them clearly */
static void normalize(Pcm& audio) {
    auto* samples = reinterpret_cast<int16_t*>(audio.samples.data());
    const size_t count = audio.getSampleCount();
    int peak = 1;
    for (size_t i = 0; i < count; i++) {
        peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    }
    const float gain = std::min(MAX_GAIN, NORMALIZED_PEAK / peak);
    if (gain <= 1.0f) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        samples[i] = static_cast<int16_t>(std::clamp(samples[i] * gain, -32767.0f, 32767.0f));
    }
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
    // The quietest frame, so talking right away doesn't raise the threshold
    float noiseFloor = 1e9f;
    float peak = 0;
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
        if (rms > DIGITAL_SILENCE_RMS) {
            noiseFloor = std::min(noiseFloor, rms);
        }
        onLevel(std::min(1.0f, rms / std::max(noiseFloor * 8.0f, 1000.0f)));

        if (index < CALIBRATION_FRAMES) {
            continue;
        }
        peak = std::max(peak, rms);

        const float floor = noiseFloor < 1e8f ? noiseFloor : 0.0f;
        const float threshold = std::max(floor * SPEECH_FACTOR, MIN_SPEECH_RMS);
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
            const bool heardSomething = peak > floor * SOFT_SPEECH_FACTOR && peak > MIN_SPEECH_RMS / 2;
            result = heardSomething ? RecordResult::Spoken : RecordResult::Silence;
            break;
        }
        if (isStopRequested()) {
            result = speaking || peak > floor * SOFT_SPEECH_FACTOR ? RecordResult::Spoken : RecordResult::Cancelled;
            break;
        }
    }

    LOG_I(TAG, "Recorded %u ms, noise %.0f, peak %.0f, speech %d", static_cast<unsigned>(audio.getSampleCount() * 1000 / SAMPLE_RATE), noiseFloor, peak, speaking);
    audio_stream_close(handle);
    onLevel(0.0f);
    if (result == RecordResult::Spoken) {
        normalize(audio);
    }
    return result;
}

// region Speaker amplifier

/** The ES3C35P amplifier pops when the audio starts and stops, so it's only enabled while sound plays */
constexpr int AMP_PIN = 1;

static bool hasSwitchedAmp() {
#if defined(ESP_PLATFORM) && defined(CONFIG_TT_DEVICE_ID)
    return strcmp(CONFIG_TT_DEVICE_ID, "es3c35p") == 0;
#else
    return false;
#endif
}

static void setAmpEnabled(bool enabled) {
#ifdef ESP_PLATFORM
    if (hasSwitchedAmp()) {
        // Active low
        gpio_set_level(static_cast<gpio_num_t>(AMP_PIN), enabled ? 0 : 1);
    }
#endif
}

void setIdleAmp(bool enabled) {
    setAmpEnabled(enabled);
}

// endregion

static bool writeSilence(AudioStreamHandle handle, uint32_t sampleRate, uint32_t millis) {
    int16_t silence[256] = {};
    size_t remaining = sampleRate * millis / 1000;
    while (remaining > 0) {
        const size_t count = std::min(remaining, std::size(silence));
        size_t bytesWritten = 0;
        if (audio_stream_write(handle, silence, count * sizeof(int16_t), &bytesWritten, pdMS_TO_TICKS(1000)) != ERROR_NONE) {
            return false;
        }
        remaining -= count;
    }
    return true;
}

PlayResult play(Device* stream, const Pcm& audio, const std::function<bool()>& isStopRequested) {
    const AudioStreamConfig config { audio.sampleRate, 16, 1 };
    AudioStreamHandle handle = nullptr;
    if (audio_stream_open_output(stream, &config, &handle) != ERROR_NONE) {
        LOG_E(TAG, "Failed to open the speaker at %u Hz", static_cast<unsigned>(audio.sampleRate));
        return PlayResult::Failed;
    }

    // The amplifier turns on after the codec settled on silence, which avoids the pop
    writeSilence(handle, audio.sampleRate, PADDING_MILLIS);
    setAmpEnabled(true);
    writeSilence(handle, audio.sampleRate, PADDING_MILLIS);

    auto result = PlayResult::Completed;
    const int16_t* samples = audio.getSamples();
    const size_t total = audio.getSampleCount();
    int16_t chunk[PLAY_CHUNK_SAMPLES];
    for (size_t offset = 0; offset < total; offset += PLAY_CHUNK_SAMPLES) {
        if (isStopRequested()) {
            result = PlayResult::Interrupted;
            break;
        }
        const size_t count = std::min(PLAY_CHUNK_SAMPLES, total - offset);
        for (size_t i = 0; i < count; i++) {
            const size_t position = offset + i;
            chunk[i] = position < FADE_IN_SAMPLES ? static_cast<int16_t>(samples[position] * position / FADE_IN_SAMPLES) : samples[position];
        }
        size_t bytesWritten = 0;
        if (audio_stream_write(handle, chunk, count * sizeof(int16_t), &bytesWritten, pdMS_TO_TICKS(1000)) != ERROR_NONE) {
            LOG_E(TAG, "Failed to write to the speaker");
            result = PlayResult::Failed;
            break;
        }
    }

    writeSilence(handle, audio.sampleRate, PADDING_MILLIS * 2);
    setAmpEnabled(false);
    writeSilence(handle, audio.sampleRate, PADDING_MILLIS);
    audio_stream_close(handle);
    return result;
}

// region Stream player

/** Seconds of speech the queue holds while the network is faster than the speaker */
constexpr size_t STREAM_QUEUE_SECONDS = 8;
/** Played once this much audio is queued, so short network hiccups don't cut the speech */
constexpr uint32_t STREAM_PREBUFFER_MILLIS = 1000;
constexpr size_t STREAM_CHUNK_SAMPLES = 512;
constexpr uint32_t STREAM_TASK_STACK = 6144;

struct StreamPlayer::State {
    Device* stream = nullptr;
    std::function<bool()> isStopRequested;
    StreamBufferHandle_t queue = nullptr;
#ifdef ESP_PLATFORM
    StaticStreamBuffer_t queueStruct {};
    uint8_t* queueStorage = nullptr;
#endif
    SemaphoreHandle_t done = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<uint32_t> sampleRate = 0;
    std::atomic<bool> finishing = false;
    std::atomic<bool> stopping = false;
    std::atomic<bool> stopRequested = false;
    std::atomic<bool> receivedAudio = false;
    PlayResult result = PlayResult::Completed;
};

void StreamPlayer::run(void* parameter) {
    auto* state = static_cast<State*>(parameter);
    // Waits for the first audio, and then for a small reserve
    while (!state->stopping && !state->finishing) {
        const uint32_t rate = state->sampleRate;
        if (rate != 0 && xStreamBufferBytesAvailable(state->queue) >= rate * sizeof(int16_t) * STREAM_PREBUFFER_MILLIS / 1000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!state->stopping && state->sampleRate != 0) {
        const uint32_t rate = state->sampleRate;
        const AudioStreamConfig config { rate, 16, 1 };
        AudioStreamHandle handle = nullptr;
        if (audio_stream_open_output(state->stream, &config, &handle) != ERROR_NONE) {
            LOG_E(TAG, "Failed to open the speaker at %u Hz", static_cast<unsigned>(rate));
            state->result = PlayResult::Failed;
        } else {
            writeSilence(handle, rate, PADDING_MILLIS);
            setAmpEnabled(true);
            writeSilence(handle, rate, PADDING_MILLIS);
            int16_t chunk[STREAM_CHUNK_SAMPLES];
            size_t played = 0;
            const size_t fadeSamples = rate / 200;
            while (true) {
                if (state->stopping) {
                    state->result = PlayResult::Interrupted;
                    break;
                }
                const size_t bytes = xStreamBufferReceive(state->queue, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
                if (bytes == 0) {
                    if (state->finishing && xStreamBufferIsEmpty(state->queue)) {
                        break;
                    }
                    // Without new samples the I2S DMA repeats its last buffer, which sounds like a stuck vowel
                    writeSilence(handle, rate, 20);
                    continue;
                }
                const size_t count = bytes / sizeof(int16_t);
                for (size_t i = 0; i < count && played + i < fadeSamples; i++) {
                    chunk[i] = static_cast<int16_t>(chunk[i] * (played + i) / fadeSamples);
                }
                played += count;
                size_t written = 0;
                if (audio_stream_write(handle, chunk, count * sizeof(int16_t), &written, pdMS_TO_TICKS(1000)) != ERROR_NONE) {
                    LOG_E(TAG, "Failed to write to the speaker");
                    state->result = PlayResult::Failed;
                    break;
                }
            }
            writeSilence(handle, rate, PADDING_MILLIS * 2);
            setAmpEnabled(false);
            writeSilence(handle, rate, PADDING_MILLIS);
            audio_stream_close(handle);
        }
    } else if (state->stopping) {
        state->result = PlayResult::Interrupted;
    }
    state->stopping = true;
    xSemaphoreGive(state->done);
    vTaskDelete(nullptr);
}

StreamPlayer::StreamPlayer(Device* stream, std::function<bool()> isStopRequested) : state(new State()) {
    state->stream = stream;
    state->isStopRequested = std::move(isStopRequested);
    // 24 kHz is the rate of the Gemini speech
    constexpr size_t queueSize = STREAM_QUEUE_SECONDS * 24000 * sizeof(int16_t);
#ifdef ESP_PLATFORM
    // Only the storage goes to PSRAM, the control structure stays in this object
    state->queueStorage = static_cast<uint8_t*>(heap_caps_malloc(queueSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state->queueStorage != nullptr) {
        state->queue = xStreamBufferCreateStatic(queueSize, 1, state->queueStorage, &state->queueStruct);
    }
#else
    state->queue = xStreamBufferCreate(queueSize, 1);
#endif
    state->done = xSemaphoreCreateBinary();
    if (state->queue == nullptr || state->done == nullptr ||
        xTaskCreate(run, "devuelta_play", STREAM_TASK_STACK, state, 6, &state->task) != pdPASS) {
        LOG_E(TAG, "Failed to start the stream player");
        state->task = nullptr;
        state->result = PlayResult::Failed;
        state->stopping = true;
    }
}

StreamPlayer::~StreamPlayer() {
    if (state->task != nullptr) {
        state->stopping = true;
        xSemaphoreTake(state->done, portMAX_DELAY);
    }
    if (state->queue != nullptr) {
        vStreamBufferDelete(state->queue);
    }
#ifdef ESP_PLATFORM
    heap_caps_free(state->queueStorage);
#endif
    if (state->done != nullptr) {
        vSemaphoreDelete(state->done);
    }
    delete state;
}

bool StreamPlayer::isStopRequested() const {
    return state->stopRequested;
}

bool StreamPlayer::hasAudio() const {
    return state->receivedAudio;
}

bool StreamPlayer::write(const int16_t* samples, size_t count, uint32_t sampleRate) {
    if (state->sampleRate == 0) {
        state->sampleRate = sampleRate;
    }
    state->receivedAudio = true;
    const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
    size_t remaining = count * sizeof(int16_t);
    while (remaining > 0) {
        if (state->stopping) {
            return false;
        }
        if (state->isStopRequested()) {
            state->stopRequested = true;
            return false;
        }
        const size_t sent = xStreamBufferSend(state->queue, bytes, remaining, pdMS_TO_TICKS(50));
        bytes += sent;
        remaining -= sent;
    }
    return true;
}

PlayResult StreamPlayer::finish() {
    if (state->task == nullptr) {
        return state->result;
    }
    state->finishing = true;
    while (xSemaphoreTake(state->done, pdMS_TO_TICKS(20)) != pdTRUE) {
        if (!state->stopping && state->isStopRequested()) {
            state->stopRequested = true;
            state->stopping = true;
        }
    }
    state->task = nullptr;
    return state->result;
}

void StreamPlayer::stop() {
    if (state->task == nullptr) {
        return;
    }
    state->stopping = true;
    xSemaphoreTake(state->done, portMAX_DELAY);
    state->task = nullptr;
}

// endregion

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
