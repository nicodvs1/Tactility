#pragma once

#include "Data.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tt::app::devuelta {

/** Large buffer in external RAM */
class Buffer {
    uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t capacity = 0;

public:
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer();

    bool reserve(size_t newCapacity);
    bool append(const void* data, size_t size);
    void clear() { length = 0; }
    /** Sets the length after writing directly into data() */
    void setSize(size_t size) { length = size; }

    uint8_t* data() const { return bytes; }
    size_t size() const { return length; }
    size_t getCapacity() const { return capacity; }
};

struct Pcm {
    Buffer samples;
    uint32_t sampleRate = 16000;

    size_t getSampleCount() const { return samples.size() / sizeof(int16_t); }
    const int16_t* getSamples() const { return reinterpret_cast<const int16_t*>(samples.data()); }
};

struct Turn {
    std::string user;
    std::string model;
};

enum class Mode {
    General,
    Form,
};

struct PromptContext {
    const Config* config;
    const std::vector<Reminder>* reminders;
    const HourRequest* form;
    Mode mode;
};

struct Reply {
    std::string heard;
    std::string answer;
    std::string action;
    Reminder reminder;
    HourRequest formUpdate;
};

/**
 * Sends the spoken question (or text when there's no audio) to Gemini and parses its structured answer.
 * @return empty on success, or a message for the user
 */
std::string ask(const PromptContext& context, const std::vector<Turn>& history, const Pcm* audio, const std::string& text, Reply& reply);

/** Receives mono 16-bit samples, returns false to stop */
using AudioSink = std::function<bool(const int16_t* samples, size_t count, uint32_t sampleRate)>;

/**
 * Generates the speech and hands it over piece by piece while it arrives.
 * @return empty on success or when onAudio stopped it, or a message for the user
 */
std::string synthesizeStreaming(const Config& config, const std::string& text, const AudioSink& onAudio);

}
