#pragma once

#include <lvgl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

namespace tt::app::devuelta {

enum class Expression {
    Rest,
    Attentive,
    Thinking,
    Speaking,
    Happy,
    Alert,
    Worried,
};

enum class Ring {
    None,
    /** Follows the sound level */
    Level,
    /** Breathes slowly while waiting */
    Pulse,
};

struct FacePose {
    int32_t centerX = 0;
    int32_t centerY = 0;
    int32_t size = 0;
    Expression expression = Expression::Rest;
    Ring ring = Ring::None;
    bool visible = true;
};

/** Verito's face: a few shapes that move between poses, blink, breathe and follow the sound level */
class Face {
    lv_obj_t* ring = nullptr;
    lv_obj_t* head = nullptr;
    lv_obj_t* leftEye = nullptr;
    lv_obj_t* rightEye = nullptr;
    lv_obj_t* mouth = nullptr;
    lv_obj_t* mouthArc = nullptr;
    lv_obj_t* leftBrow = nullptr;
    lv_obj_t* rightBrow = nullptr;
    lv_point_precise_t leftBrowPoints[2] = {};
    lv_point_precise_t rightBrowPoints[2] = {};
    lv_timer_t* timer = nullptr;
    std::function<void()> onClick;

    FacePose from;
    FacePose to;
    bool placed = false;
    uint32_t transitionStart = 0;
    uint32_t nextBlink = 0;
    uint32_t blinkStart = 0;
    float shownLevel = 0.0f;
    std::atomic<float> level = 0.0f;

    static void onTimer(lv_timer_t* timer);
    static void onHeadClicked(lv_event_t* event);
    static void onHeadDeleted(lv_event_t* event);

    void update();

public:
    /** Creates the face on top of the parent's other children, hidden until the first pose */
    void create(lv_obj_t* parent, std::function<void()> onClick);

    /** Moves the face to a pose, animated unless it's the first pose or animate is false */
    void setPose(const FacePose& pose, bool animate);

    /** The microphone or speaker level from 0 to 1. Can be called from any task. */
    void setLevel(float value) { level = std::sqrt(std::clamp(value, 0.0f, 1.0f)); }
};

}
