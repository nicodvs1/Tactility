#include "Face.h"

#include <algorithm>
#include <cmath>

namespace tt::app::devuelta {

namespace {

constexpr uint32_t FRAME_MILLIS = 33;
constexpr uint32_t MOVE_MILLIS = 400;
constexpr uint32_t EXPRESSION_MILLIS = 250;
constexpr uint32_t BLINK_MILLIS = 300;
constexpr uint32_t BREATH_MILLIS = 4000;
constexpr uint32_t PULSE_MILLIS = 2000;
constexpr float PI = 3.14159265f;

// Measures of a face that is DESIGN_SIZE wide
constexpr float DESIGN_SIZE = 180.0f;
constexpr float RING_DIAMETER = 200.0f;
constexpr float RING_GROWTH = 28.0f;
constexpr float RING_BORDER = 6.0f;
constexpr float BROW_WIDTH = 6.0f;
constexpr float BLINK_HEIGHT = 4.0f;
constexpr float MOUTH_GROWTH = 30.0f;
constexpr float SHADOW_WIDTH = 18.0f;
constexpr float SHADOW_OFFSET = 6.0f;

const lv_color_t COLOR_SKIN = lv_color_hex(0xE9A23B);
const lv_color_t COLOR_INK = lv_color_hex(0x1B2A4A);
const lv_color_t COLOR_RING = lv_color_hex(0x0F6E6A);

struct Box {
    float x, y, width, height, radius;
};

struct Arc {
    float centerX, centerY, diameter, width;
    bool smile;
};

struct Stroke {
    float x1, y1, x2, y2;
};

struct Features {
    Box leftEye;
    Box rightEye;
    /** The mouth is the arc instead of the box */
    bool hasArc;
    Box mouth;
    Arc arc;
    bool hasBrows;
    Stroke leftBrow;
    Stroke rightBrow;
};

const Features& getFeatures(Expression expression) {
    static constexpr Features REST { { 48, 66, 20, 26, 10 }, { 112, 66, 20, 26, 10 }, false, { 70, 116, 40, 10, 5 }, {}, false, {}, {} };
    static constexpr Features ATTENTIVE { { 46, 60, 24, 34, 12 }, { 110, 60, 24, 34, 12 }, false, { 72, 118, 36, 12, 6 }, {}, false, {}, {} };
    static constexpr Features THINKING { { 62, 56, 20, 28, 10 }, { 122, 56, 20, 28, 10 }, false, { 92, 122, 28, 8, 4 }, {}, false, {}, {} };
    static constexpr Features SPEAKING { { 47, 62, 22, 30, 11 }, { 111, 62, 22, 30, 11 }, false, { 68, 112, 44, 10, 8 }, {}, false, {}, {} };
    static constexpr Features HAPPY { { 45, 70, 26, 12, 6 }, { 109, 70, 26, 12, 6 }, true, {}, { 90, 104, 64, 8, true }, false, {}, {} };
    static constexpr Features ALERT { { 46, 58, 24, 36, 12 }, { 110, 58, 24, 36, 12 }, false, { 78, 116, 24, 20, 10 }, {}, true, { 42, 46, 72, 42 }, { 108, 42, 138, 46 } };
    static constexpr Features WORRIED { { 48, 70, 20, 26, 10 }, { 112, 70, 20, 26, 10 }, true, {}, { 90, 144, 48, 7, false }, true, { 42, 56, 72, 48 }, { 108, 48, 138, 56 } };
    switch (expression) {
        case Expression::Rest: return REST;
        case Expression::Attentive: return ATTENTIVE;
        case Expression::Thinking: return THINKING;
        case Expression::Speaking: return SPEAKING;
        case Expression::Happy: return HAPPY;
        case Expression::Alert: return ALERT;
        case Expression::Worried: return WORRIED;
    }
    return REST;
}

float mix(float from, float to, float amount) {
    return from + (to - from) * amount;
}

Box mix(const Box& from, const Box& to, float amount) {
    return { mix(from.x, to.x, amount), mix(from.y, to.y, amount), mix(from.width, to.width, amount), mix(from.height, to.height, amount), mix(from.radius, to.radius, amount) };
}

Stroke mix(const Stroke& from, const Stroke& to, float amount) {
    return { mix(from.x1, to.x1, amount), mix(from.y1, to.y1, amount), mix(from.x2, to.x2, amount), mix(from.y2, to.y2, amount) };
}

float easeOut(float amount) {
    const float rest = 1.0f - amount;
    return 1.0f - rest * rest * rest * rest;
}

float easeInOut(float amount) {
    if (amount < 0.5f) {
        return 4.0f * amount * amount * amount;
    }
    const float rest = -2.0f * amount + 2.0f;
    return 1.0f - rest * rest * rest / 2.0f;
}

float getProgress(uint32_t elapsed, uint32_t duration) {
    return std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(duration));
}

/** A sine wave from 0 to 1 */
float getWave(uint32_t now, uint32_t period) {
    return 0.5f + 0.5f * std::sin(static_cast<float>(now % period) / static_cast<float>(period) * 2.0f * PI);
}

int32_t toPixels(float value, float scale) {
    return static_cast<int32_t>(std::lround(value * scale));
}

// The setters below skip unchanged values, so a still face doesn't redraw

void setOpacity(lv_obj_t* object, float opacity) {
    const auto opa = static_cast<lv_opa_t>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
    if (lv_obj_get_style_opa(object, LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(object, opa, LV_STATE_DEFAULT);
    }
    const bool hidden = opa == LV_OPA_TRANSP;
    if (lv_obj_is_hidden(object) != hidden) {
        lv_obj_set_hidden(object, hidden);
    }
}

void setRadius(lv_obj_t* object, int32_t radius) {
    if (lv_obj_get_style_radius(object, LV_PART_MAIN) != radius) {
        lv_obj_set_style_radius(object, radius, LV_STATE_DEFAULT);
    }
}

void setBox(lv_obj_t* object, const Box& box, float scale) {
    lv_obj_set_pos(object, toPixels(box.x, scale), toPixels(box.y, scale));
    lv_obj_set_size(object, std::max<int32_t>(1, toPixels(box.width, scale)), std::max<int32_t>(1, toPixels(box.height, scale)));
    setRadius(object, toPixels(box.radius, scale));
}

void setStroke(lv_obj_t* line, lv_point_precise_t* points, const Stroke& stroke, float scale) {
    const lv_point_precise_t start = { toPixels(stroke.x1, scale), toPixels(stroke.y1, scale) };
    const lv_point_precise_t end = { toPixels(stroke.x2, scale), toPixels(stroke.y2, scale) };
    if (points[0].x != start.x || points[0].y != start.y || points[1].x != end.x || points[1].y != end.y) {
        points[0] = start;
        points[1] = end;
        lv_line_set_points(line, points, 2);
    }
    const int32_t width = std::max<int32_t>(2, toPixels(BROW_WIDTH, scale));
    if (lv_obj_get_style_line_width(line, LV_PART_MAIN) != width) {
        lv_obj_set_style_line_width(line, width, LV_STATE_DEFAULT);
    }
}

void setArc(lv_obj_t* object, const Arc& arc, float scale) {
    const int32_t diameter = toPixels(arc.diameter, scale);
    lv_obj_set_pos(object, toPixels(arc.centerX, scale) - diameter / 2, toPixels(arc.centerY, scale) - diameter / 2);
    lv_obj_set_size(object, diameter, diameter);
    const int32_t width = std::max<int32_t>(2, toPixels(arc.width, scale));
    if (lv_obj_get_style_arc_width(object, LV_PART_MAIN) != width) {
        lv_obj_set_style_arc_width(object, width, LV_STATE_DEFAULT);
    }
    // LVGL angles turn clockwise from the right, so the lower half is a smile
    const lv_value_precise_t start = arc.smile ? 15 : 200;
    const lv_value_precise_t end = arc.smile ? 165 : 340;
    if (lv_arc_get_bg_angle_start(object) != start || lv_arc_get_bg_angle_end(object) != end) {
        lv_arc_set_bg_angles(object, start, end);
    }
}

lv_obj_t* createShape(lv_obj_t* parent) {
    auto* shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_set_style_bg_color(shape, COLOR_INK, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_clickable(shape, false);
    lv_obj_set_scrollable(shape, false);
    return shape;
}

lv_obj_t* createBrow(lv_obj_t* parent) {
    auto* line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_style_line_color(line, COLOR_INK, LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(line, true, LV_STATE_DEFAULT);
    lv_obj_set_size(line, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(line, false);
    return line;
}

}

void Face::create(lv_obj_t* parent, std::function<void()> onClickCallback) {
    onClick = std::move(onClickCallback);
    placed = false;
    shownLevel = 0.0f;
    level = 0.0f;

    ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ring, COLOR_RING, LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_clickable(ring, false);
    lv_obj_set_scrollable(ring, false);
    lv_obj_set_hidden(ring, true);

    head = lv_obj_create(parent);
    lv_obj_remove_style_all(head);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(head, COLOR_SKIN, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(head, COLOR_INK, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(head, LV_OPA_20, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_clickable(head, true);
    lv_obj_set_hidden(head, true);
    lv_obj_add_event_cb(head, onHeadClicked, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_add_event_cb(head, onHeadDeleted, LV_EVENT_DELETE, this);

    leftEye = createShape(head);
    rightEye = createShape(head);
    mouth = createShape(head);

    mouthArc = lv_arc_create(head);
    lv_obj_remove_style_all(mouthArc);
    lv_obj_set_style_arc_color(mouthArc, COLOR_INK, LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(mouthArc, true, LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(mouthArc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_clickable(mouthArc, false);

    leftBrow = createBrow(head);
    rightBrow = createBrow(head);
    leftBrowPoints[0] = leftBrowPoints[1] = {};
    rightBrowPoints[0] = rightBrowPoints[1] = {};

    nextBlink = lv_tick_get() + 3000;
    blinkStart = 0;
    timer = lv_timer_create(onTimer, FRAME_MILLIS, this);
}

void Face::setPose(const FacePose& pose, bool animate) {
    if (head == nullptr) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (!placed || !animate) {
        from = pose;
        placed = true;
        // Starts out finished
        transitionStart = now - MOVE_MILLIS;
    } else {
        // Continues from where an unfinished transition is now
        const float move = easeOut(getProgress(lv_tick_diff(now, transitionStart), MOVE_MILLIS));
        if (from.visible && to.visible) {
            from.centerX = static_cast<int32_t>(mix(from.centerX, to.centerX, move));
            from.centerY = static_cast<int32_t>(mix(from.centerY, to.centerY, move));
            from.size = static_cast<int32_t>(mix(from.size, to.size, move));
        } else {
            from = to;
        }
        from.expression = move < 0.5f ? from.expression : to.expression;
        from.ring = move < 0.5f ? from.ring : to.ring;
        from.visible = move < 0.5f ? from.visible : to.visible;
        transitionStart = now;
    }
    to = pose;
    update();
}

void Face::onTimer(lv_timer_t* timer) {
    static_cast<Face*>(lv_timer_get_user_data(timer))->update();
}

void Face::onHeadClicked(lv_event_t* event) {
    auto* face = static_cast<Face*>(lv_event_get_user_data(event));
    if (face->onClick) {
        face->onClick();
    }
}

void Face::onHeadDeleted(lv_event_t* event) {
    auto* face = static_cast<Face*>(lv_event_get_user_data(event));
    lv_timer_delete(face->timer);
    face->timer = nullptr;
    face->ring = nullptr;
    face->head = nullptr;
    face->leftEye = nullptr;
    face->rightEye = nullptr;
    face->mouth = nullptr;
    face->mouthArc = nullptr;
    face->leftBrow = nullptr;
    face->rightBrow = nullptr;
    face->placed = false;
}

void Face::update() {
    if (head == nullptr || !placed) {
        return;
    }
    const uint32_t now = lv_tick_get();
    const uint32_t elapsed = lv_tick_diff(now, transitionStart);
    const float move = easeOut(getProgress(elapsed, MOVE_MILLIS));
    const float change = easeInOut(getProgress(elapsed, EXPRESSION_MILLIS));

    // Appearing and disappearing happen in place
    const FacePose& start = from.visible ? from : to;
    const FacePose& end = to.visible ? to : from;
    const float opacity = mix(from.visible ? 1.0f : 0.0f, to.visible ? 1.0f : 0.0f, change);
    setOpacity(head, opacity);
    if (opacity <= 0.0f) {
        setOpacity(ring, 0.0f);
        return;
    }

    const float target = level;
    shownLevel += (target - shownLevel) * (target > shownLevel ? 0.5f : 0.2f);

    const float size = mix(start.size, end.size, move);
    const float scale = size / DESIGN_SIZE;
    const float centerX = mix(start.centerX, end.centerX, move);
    float centerY = mix(start.centerY, end.centerY, move);
    if (end.expression == Expression::Rest) {
        centerY += 2.0f * (getWave(now, BREATH_MILLIS) - 0.5f) * 2.0f;
    }

    const int32_t headSize = std::lround(size);
    lv_obj_set_pos(head, std::lround(centerX - size / 2.0f), std::lround(centerY - size / 2.0f));
    lv_obj_set_size(head, headSize, headSize);
    const int32_t shadowWidth = toPixels(SHADOW_WIDTH, scale);
    if (lv_obj_get_style_shadow_width(head, LV_PART_MAIN) != shadowWidth) {
        lv_obj_set_style_shadow_width(head, shadowWidth, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_offset_y(head, toPixels(SHADOW_OFFSET, scale), LV_STATE_DEFAULT);
    }

    const Features& a = getFeatures(start.expression);
    const Features& b = getFeatures(end.expression);

    if (static_cast<int32_t>(now - nextBlink) >= 0) {
        blinkStart = now;
        nextBlink = now + lv_rand(2500, 5500);
    }
    float blink = 0.0f;
    if (blinkStart != 0 && end.expression != Expression::Happy) {
        const uint32_t blinkElapsed = lv_tick_diff(now, blinkStart);
        if (blinkElapsed < BLINK_MILLIS) {
            blink = 1.0f - std::fabs(1.0f - 2.0f * static_cast<float>(blinkElapsed) / BLINK_MILLIS);
        } else {
            blinkStart = 0;
        }
    }
    for (auto [eye, box] : { std::pair { leftEye, mix(a.leftEye, b.leftEye, change) }, std::pair { rightEye, mix(a.rightEye, b.rightEye, change) } }) {
        const float height = mix(box.height, BLINK_HEIGHT, blink);
        box.y += (box.height - height) / 2.0f;
        box.height = height;
        box.radius = std::min(box.radius, height / 2.0f);
        setBox(eye, box, scale);
    }

    Box mouthBox = a.hasArc ? b.mouth : (b.hasArc ? a.mouth : mix(a.mouth, b.mouth, change));
    if (end.expression == Expression::Speaking) {
        mouthBox.height += MOUTH_GROWTH * shownLevel * change;
    }
    setBox(mouth, mouthBox, scale);
    setOpacity(mouth, mix(a.hasArc ? 0.0f : 1.0f, b.hasArc ? 0.0f : 1.0f, change));

    setArc(mouthArc, b.hasArc ? b.arc : a.arc, scale);
    setOpacity(mouthArc, mix(a.hasArc ? 1.0f : 0.0f, b.hasArc ? 1.0f : 0.0f, change));

    const Stroke leftStroke = a.hasBrows ? (b.hasBrows ? mix(a.leftBrow, b.leftBrow, change) : a.leftBrow) : b.leftBrow;
    const Stroke rightStroke = a.hasBrows ? (b.hasBrows ? mix(a.rightBrow, b.rightBrow, change) : a.rightBrow) : b.rightBrow;
    setStroke(leftBrow, leftBrowPoints, leftStroke, scale);
    setStroke(rightBrow, rightBrowPoints, rightStroke, scale);
    const float browOpacity = mix(a.hasBrows ? 1.0f : 0.0f, b.hasBrows ? 1.0f : 0.0f, change);
    setOpacity(leftBrow, browOpacity);
    setOpacity(rightBrow, browOpacity);

    const Ring ringMode = end.ring != Ring::None ? end.ring : start.ring;
    const float ringPresence = mix(start.ring != Ring::None ? 1.0f : 0.0f, end.ring != Ring::None ? 1.0f : 0.0f, change);
    float ringLevel = 0.0f;
    if (ringMode == Ring::Level) {
        ringLevel = shownLevel;
    } else if (ringMode == Ring::Pulse) {
        ringLevel = 0.5f * getWave(now, PULSE_MILLIS);
    }
    const int32_t ringSize = toPixels(RING_DIAMETER + RING_GROWTH * ringLevel, scale);
    lv_obj_set_pos(ring, std::lround(centerX) - ringSize / 2, std::lround(centerY) - ringSize / 2);
    lv_obj_set_size(ring, ringSize, ringSize);
    const int32_t border = std::max<int32_t>(3, toPixels(RING_BORDER, scale));
    if (lv_obj_get_style_border_width(ring, LV_PART_MAIN) != border) {
        lv_obj_set_style_border_width(ring, border, LV_STATE_DEFAULT);
    }
    setOpacity(ring, ringPresence * opacity * (0.35f + 0.65f * ringLevel));
}

}
