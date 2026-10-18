#pragma once

#include "Assistant.h"

#include <functional>

struct Device;

namespace tt::app::devuelta {

enum class RecordResult {
    Spoken,
    /** Nothing was said before the timeout */
    Silence,
    /** Stopped by the button before anything was said */
    Cancelled,
    Failed,
};

/**
 * Records until the person stops talking, the button is pressed or the maximum duration is reached.
 * @param[in] onLevel receives the input level (0 to 1) of each frame
 * @param[in] isStopRequested polled every frame
 */
RecordResult record(Device* stream, Pcm& audio, const std::function<void(float)>& onLevel, const std::function<bool()>& isStopRequested);

enum class PlayResult {
    Completed,
    /** Stopped by isStopRequested */
    Interrupted,
    Failed,
};

PlayResult play(Device* stream, const Pcm& audio, const std::function<bool()>& isStopRequested);

enum class Tone {
    /** Starts listening */
    Listen,
    /** Stops listening */
    Done,
    /** Medication reminder */
    Alarm,
};

void playTone(Device* stream, Tone tone);

/** Switches the speaker amplifier while nothing plays: off in the app, on when other apps may play sound */
void setIdleAmp(bool enabled);

/** The physical button, wired between the GPIO and ground */
class Button {
    int pin;
    int pressedCount = 0;
    bool reported = false;

public:
    explicit Button(int pin);

    /** Call every ~20 ms. @return true once per press */
    bool poll();
};

}
