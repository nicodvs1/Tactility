#ifdef ESP_PLATFORM

#include "Assistant.h"
#include "Data.h"
#include "Voice.h"

#include <Tactility/lvgl/FontCache.h>
#include <Tactility/lvgl/FontVersions.h>
#include <Tactility/service/audio/Audio.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <binfont/binfont.h>
#include <lvgl/binfont.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/spinner.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <format>

namespace tt::app::devuelta {

extern const ::AppManifest manifest;

namespace {

constexpr auto* TAG = "DeVuelta";

/** The talk button on top of the case, wired between this GPIO and ground */
constexpr int BUTTON_PIN = 45;

constexpr size_t MAX_HISTORY_TURNS = 6;
constexpr time_t HISTORY_EXPIRY_SECONDS = 180;
constexpr time_t IDLE_RETURN_SECONDS = 60;
constexpr time_t SNOOZE_SECONDS = 600;
constexpr time_t ALARM_REPEAT_SECONDS = 120;
constexpr int ALARM_MAX_REPEATS = 5;
constexpr int MAX_FORM_TURNS = 12;
constexpr float VOLUME_STEP = 15.0f;
constexpr size_t LEVEL_BAR_COUNT = 4;
#define LEVEL_BAR_WIDTH LV_DPX(26)
#define LEVEL_BAR_MAX_HEIGHT LV_DPX(150)

// Colors of the De Vuelta website
const lv_color_t COLOR_BACKGROUND = lv_color_hex(0xFBF7F0);
const lv_color_t COLOR_TEXT = lv_color_hex(0x1F2A2E);
const lv_color_t COLOR_MUTED = lv_color_hex(0x5A6468);
const lv_color_t COLOR_CARD = lv_color_hex(0xFFFFFF);
const lv_color_t COLOR_BORDER = lv_color_hex(0xE6DCCD);
const lv_color_t COLOR_PRIMARY = lv_color_hex(0xA8482A);
const lv_color_t COLOR_SECONDARY = lv_color_hex(0xF3ECE1);
const lv_color_t COLOR_SAGE = lv_color_hex(0x3F5C4A);
const lv_color_t COLOR_SAGE_SOFT = lv_color_hex(0xE3EBE5);
const lv_color_t COLOR_AMBER_SOFT = lv_color_hex(0xF7EAD0);
const lv_color_t COLOR_AMBER_INK = lv_color_hex(0x6B4A0C);
const lv_color_t COLOR_EMERGENCY = lv_color_hex(0xB3261E);
const lv_color_t COLOR_WHITE = lv_color_hex(0xFFFFFF);

constexpr const char* WEEKDAYS[] = { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" };
constexpr const char* MONTHS[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };

enum class Screen {
    Home,
    Listening,
    Thinking,
    Answer,
    Message,
    Reminders,
    Form,
    FormReady,
    Alarm,
    Emergency,
};

enum class Command {
    Talk,
    StopTalk,
    Home,
    Reminders,
    Form,
    FormReady,
    FillForm,
    ClearForm,
    VolumeUp,
    VolumeDown,
    MarkTaken,
    Snooze,
    Exit,
};

struct Message {
    Command command;
    int argument;
};

struct Fonts {
    BinFont* binFonts[3] = {};
    lv_font_t* created[3] = {};
    lv_font_t* body = nullptr;
    lv_font_t* title = nullptr;
    lv_font_t* clock = nullptr;
};

struct Context {
    uint32_t appInstanceId = 0;
    Device* stream = nullptr;
    QueueHandle_t queue = nullptr;
    SemaphoreHandle_t workerDone = nullptr;
    volatile bool running = true;
    volatile bool stopRequested = false;

    Fonts fonts;

    // Owned by the worker task, read by the LVGL task under the LVGL lock
    Config config;
    std::vector<Reminder> reminders;
    HourRequest form;
    Screen screen = Screen::Home;
    std::string title;
    std::string text;
    std::string detail;
    int alarmIndex = -1;

    // Worker task only
    std::vector<Turn> history;
    time_t lastTurnTime = 0;
    time_t screenTime = 0;
    std::vector<std::string> firedAlarms;
    int snoozeIndex = -1;
    time_t snoozeUntil = 0;
    time_t lastAlarmRing = 0;
    int alarmRings = 0;

    // Widgets, only valid while the window has live widgets
    lv_obj_t* root = nullptr;
    lv_obj_t* clockLabel = nullptr;
    lv_obj_t* dateLabel = nullptr;
    lv_obj_t* volumeLabel = nullptr;
    lv_obj_t* levelBars[LEVEL_BAR_COUNT] = {};
    float barLevels[LEVEL_BAR_COUNT] = {};
};

// region Time

tm getLocalTime() {
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    return local;
}

bool isClockSet(const tm& local) {
    return local.tm_year + 1900 >= 2025;
}

std::string getToday() {
    const tm local = getLocalTime();
    return std::format("{:04}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

std::string getClockText(const tm& local) {
    return isClockSet(local) ? std::format("{:02}:{:02}", local.tm_hour, local.tm_min) : "--:--";
}

std::string getDateText(const tm& local) {
    if (!isClockSet(local)) {
        return "Conecte el WiFi para ver la hora";
    }
    return std::format("{} {} de {}", WEEKDAYS[local.tm_wday], local.tm_mday, MONTHS[local.tm_mon]);
}

int getMinutes(const std::string& time) {
    if (time.size() != 5 || time[2] != ':') {
        return -1;
    }
    return std::atoi(time.substr(0, 2).c_str()) * 60 + std::atoi(time.substr(3, 2).c_str());
}

/** @return the index of the next reminder that isn't taken today, or -1 */
int findNextReminder(const std::vector<Reminder>& reminders) {
    const tm local = getLocalTime();
    const int now = local.tm_hour * 60 + local.tm_min;
    const auto today = getToday();
    int best = -1;
    int bestDistance = 24 * 60 + 1;
    for (size_t i = 0; i < reminders.size(); i++) {
        if (reminders[i].takenOn == today) {
            continue;
        }
        const int minutes = getMinutes(reminders[i].time);
        if (minutes < 0) {
            continue;
        }
        // Overdue reminders of today come first
        const int distance = minutes >= now - 120 ? minutes - now + 120 : minutes + 24 * 60 - now + 120;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// endregion

// region Fonts

std::vector<uint32_t> getLatinCodepoints() {
    std::vector<uint32_t> codepoints;
    for (uint32_t codepoint = 0x20; codepoint <= 0x7E; codepoint++) {
        codepoints.push_back(codepoint);
    }
    for (uint32_t codepoint = 0xA0; codepoint <= 0xFF; codepoint++) {
        codepoints.push_back(codepoint);
    }
    for (uint32_t codepoint : { 0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2026u }) {
        codepoints.push_back(codepoint);
    }
    return codepoints;
}

std::vector<uint32_t> getClockCodepoints() {
    return { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', '-' };
}

/** Generates the large fonts once and caches them, which takes a few seconds on first start */
void loadFonts(Fonts& fonts) {
    const auto ttf = lvgl::getSystemFontPath("AdwaitaSans.ttf");
    const uint8_t bpp = lvgl::getGeneratedFontBpp();
    struct Request {
        const char* name;
        uint16_t size;
        std::function<std::vector<uint32_t>()> getCodepoints;
        lv_font_t** target;
    };
    const Request requests[] = {
        { "body", 22, getLatinCodepoints, &fonts.body },
        { "title", 30, getLatinCodepoints, &fonts.title },
        { "clock", 64, getClockCodepoints, &fonts.clock },
    };
    for (size_t i = 0; i < std::size(requests); i++) {
        const auto& request = requests[i];
        const auto fileName = lvgl::getCachedFontFileName("devuelta_", request.name, request.size, bpp, TT_TEXT_FONT_VERSION);
        fonts.binFonts[i] = lvgl::loadOrGenerateFont(fileName, ttf, request.size, bpp, request.getCodepoints);
        if (fonts.binFonts[i] != nullptr) {
            lvgl_lock();
            fonts.created[i] = lvgl_binfont_create(fonts.binFonts[i]);
            lvgl_unlock();
            if (fonts.created[i] == nullptr) {
                binfont_close(fonts.binFonts[i]);
                fonts.binFonts[i] = nullptr;
            }
        }
        *request.target = fonts.created[i];
    }
    const lv_font_t* fallback = lvgl_get_text_font(FONT_SIZE_LARGE);
    for (auto* target : { &fonts.body, &fonts.title, &fonts.clock }) {
        if (*target == nullptr) {
            LOG_W(TAG, "Using the system font as fallback");
            *target = const_cast<lv_font_t*>(fallback);
        }
    }
}

void unloadFonts(Fonts& fonts) {
    lvgl_lock();
    for (size_t i = 0; i < std::size(fonts.binFonts); i++) {
        if (fonts.created[i] != nullptr) {
            lvgl_binfont_destroy(fonts.created[i]);
            binfont_close(fonts.binFonts[i]);
        }
    }
    lvgl_unlock();
}

// endregion

// region Widgets

void send(Context* ctx, Command command, int argument = 0) {
    const Message message { command, argument };
    xQueueSend(ctx->queue, &message, 0);
}

struct ButtonAction {
    Context* ctx;
    Command command;
    int argument;
};

void onButtonClicked(lv_event_t* event) {
    auto* action = static_cast<ButtonAction*>(lv_event_get_user_data(event));
    send(action->ctx, action->command, action->argument);
}

void onButtonDeleted(lv_event_t* event) {
    delete static_cast<ButtonAction*>(lv_event_get_user_data(event));
}

lv_obj_t* createLabel(lv_obj_t* parent, const std::string& text, const lv_font_t* font, lv_color_t color) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

lv_obj_t* createColumn(lv_obj_t* parent) {
    auto* column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, LV_DPX(6), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(column, false);
    return column;
}

lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    return row;
}

lv_obj_t* createCard(lv_obj_t* parent, lv_color_t background) {
    auto* card = createColumn(parent);
    lv_obj_set_style_bg_color(card, background, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, LV_DPX(16), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, COLOR_BORDER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, LV_DPX(12), LV_STATE_DEFAULT);
    return card;
}

lv_obj_t* createButton(Context* ctx, lv_obj_t* parent, const std::string& text, Command command, int argument, lv_color_t background, lv_color_t foreground) {
    const bool outlined = lv_color_eq(background, COLOR_CARD);
    auto* button = lv_button_create(parent);
    lv_obj_set_height(button, LV_DPX(58));
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, background, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(button, LV_DPX(14), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, COLOR_BORDER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, outlined ? 1 : 0, LV_STATE_DEFAULT);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, ctx->fonts.body, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, foreground, LV_STATE_DEFAULT);
    lv_obj_center(label);
    auto* action = new ButtonAction { ctx, command, argument };
    lv_obj_add_event_cb(button, onButtonClicked, LV_EVENT_SHORT_CLICKED, action);
    lv_obj_add_event_cb(button, onButtonDeleted, LV_EVENT_DELETE, action);
    return button;
}

lv_obj_t* createFullWidthButton(Context* ctx, lv_obj_t* parent, const std::string& text, Command command, int argument, lv_color_t background, lv_color_t foreground) {
    auto* button = createButton(ctx, parent, text, command, argument, background, foreground);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_flex_grow(button, 0);
    return button;
}

/** The round talk button that mirrors the physical one */
lv_obj_t* createTalkCircle(Context* ctx, lv_obj_t* parent, int32_t size, const std::string& text, lv_color_t color, Command command) {
    auto* circle = lv_button_create(parent);
    lv_obj_set_size(circle, size, size);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(circle, color, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(circle, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(circle, COLOR_SECONDARY, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(circle, LV_DPX(6), LV_STATE_DEFAULT);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, 0);
    auto* label = lv_label_create(circle);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, ctx->fonts.title, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_center(label);
    auto* action = new ButtonAction { ctx, command, 0 };
    lv_obj_add_event_cb(circle, onButtonClicked, LV_EVENT_SHORT_CLICKED, action);
    lv_obj_add_event_cb(circle, onButtonDeleted, LV_EVENT_DELETE, action);
    return circle;
}

/** Fixed-height area that centers its child, so screens keep their layout */
lv_obj_t* createCenterArea(lv_obj_t* parent, int32_t height) {
    auto* area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, LV_PCT(100), height);
    lv_obj_set_scrollable(area, false);
    return area;
}

/** Small exit button in the top left corner, outside the flex layout */
void createExitButton(Context* ctx, lv_obj_t* parent) {
    auto* button = lv_button_create(parent);
    lv_obj_set_floating(button, true);
    lv_obj_set_size(button, LV_DPX(40), LV_DPX(40));
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, COLOR_SECONDARY, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, COLOR_BORDER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 1, LV_STATE_DEFAULT);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, "\u00D7");
    lv_obj_set_style_text_font(label, ctx->fonts.body, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, COLOR_MUTED, LV_STATE_DEFAULT);
    lv_obj_center(label);
    auto* action = new ButtonAction { ctx, Command::Exit, 0 };
    lv_obj_add_event_cb(button, onButtonClicked, LV_EVENT_SHORT_CLICKED, action);
    lv_obj_add_event_cb(button, onButtonDeleted, LV_EVENT_DELETE, action);
}

std::string getVolumeText() {
    return std::format("Volumen {}%", static_cast<int>(service::audio::getOutputVolume() + 0.5f));
}

// endregion

// region Screens

void renderHome(Context* ctx, lv_obj_t* root) {
    const tm local = getLocalTime();

    auto* header = createColumn(root);
    ctx->clockLabel = createLabel(header, getClockText(local), ctx->fonts.clock, COLOR_TEXT);
    lv_obj_set_style_text_align(ctx->clockLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    ctx->dateLabel = createLabel(header, getDateText(local), ctx->fonts.body, COLOR_MUTED);
    lv_obj_set_style_text_align(ctx->dateLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);

    const int next = findNextReminder(ctx->reminders);
    auto* card = createCard(root, COLOR_AMBER_SOFT);
    lv_obj_set_style_border_width(card, 0, LV_STATE_DEFAULT);
    if (next >= 0) {
        const auto& reminder = ctx->reminders[next];
        createLabel(card, std::format("Próximo remedio, {}", reminder.time), ctx->fonts.body, COLOR_AMBER_INK);
        createLabel(card, std::format("{} {}", reminder.name, reminder.dose), ctx->fonts.title, COLOR_TEXT);
    } else {
        createLabel(card, "Remedios de hoy", ctx->fonts.body, COLOR_AMBER_INK);
        createLabel(card, "Todos tomados", ctx->fonts.title, COLOR_TEXT);
    }

    auto* area = createCenterArea(root, LV_DPX(132));
    createTalkCircle(ctx, area, LV_DPX(124), "Hablar", COLOR_PRIMARY, Command::Talk);

    auto* hint = createLabel(root, "Apriete el botón de arriba y pregúnteme", ctx->fonts.body, COLOR_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);

    auto* row = createRow(root);
    createButton(ctx, row, "Remedios", Command::Reminders, 0, COLOR_CARD, COLOR_TEXT);
    createButton(ctx, row, "Pedir hora", Command::Form, 0, COLOR_CARD, COLOR_TEXT);

    auto* volumeRow = createRow(root);
    auto* down = createButton(ctx, volumeRow, "-", Command::VolumeDown, 0, COLOR_SECONDARY, COLOR_TEXT);
    lv_obj_set_flex_grow(down, 0);
    lv_obj_set_width(down, LV_DPX(64));
    ctx->volumeLabel = createLabel(volumeRow, getVolumeText(), ctx->fonts.body, COLOR_TEXT);
    lv_obj_set_flex_grow(ctx->volumeLabel, 1);
    lv_obj_set_width(ctx->volumeLabel, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(ctx->volumeLabel, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    auto* up = createButton(ctx, volumeRow, "+", Command::VolumeUp, 0, COLOR_SECONDARY, COLOR_TEXT);
    lv_obj_set_flex_grow(up, 0);
    lv_obj_set_width(up, LV_DPX(64));

    createExitButton(ctx, root);
}

void renderListening(Context* ctx, lv_obj_t* root) {
    auto* title = createLabel(root, "Le escucho...", ctx->fonts.title, COLOR_SAGE);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    auto* area = createCenterArea(root, LV_DPX(190));
    auto* bars = lv_obj_create(area);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bars, LV_DPX(14), LV_STATE_DEFAULT);
    lv_obj_set_scrollable(bars, false);
    lv_obj_center(bars);
    for (size_t i = 0; i < LEVEL_BAR_COUNT; i++) {
        auto* bar = lv_obj_create(bars);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LEVEL_BAR_WIDTH, LEVEL_BAR_WIDTH);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(bar, COLOR_SAGE, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_STATE_DEFAULT);
        ctx->levelBars[i] = bar;
        ctx->barLevels[i] = 0;
    }
    auto* text = createLabel(root, ctx->text.empty() ? "Hable con calma. Cuando termine, espere un momento." : ctx->text, ctx->fonts.body, COLOR_TEXT);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    createFullWidthButton(ctx, root, "Terminar", Command::StopTalk, 0, COLOR_SAGE, COLOR_WHITE);
}

void renderThinking(Context* ctx, lv_obj_t* root) {
    auto* area = createCenterArea(root, LV_DPX(200));
    auto* spinner = lvgl_spinner_create(area);
    lv_obj_center(spinner);
    auto* title = createLabel(root, "Un momento, estoy pensando", ctx->fonts.title, COLOR_TEXT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
}

void renderAnswer(Context* ctx, lv_obj_t* root) {
    if (!ctx->detail.empty()) {
        createLabel(root, std::format("Usted dijo: “{}”", ctx->detail), ctx->fonts.body, COLOR_MUTED);
    }
    auto* card = createCard(root, COLOR_CARD);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_scrollable(card, true);
    createLabel(card, ctx->text, ctx->fonts.title, COLOR_TEXT);
    if (!ctx->title.empty()) {
        createLabel(root, ctx->title, ctx->fonts.body, COLOR_MUTED);
    }
    auto* row = createRow(root);
    createButton(ctx, row, "Volver", Command::Home, 0, COLOR_CARD, COLOR_TEXT);
    createButton(ctx, row, "Preguntar", Command::Talk, 0, COLOR_PRIMARY, COLOR_WHITE);
}

void renderMessage(Context* ctx, lv_obj_t* root) {
    createLabel(root, ctx->title, ctx->fonts.title, COLOR_PRIMARY);
    auto* card = createCard(root, COLOR_CARD);
    lv_obj_set_flex_grow(card, 1);
    createLabel(card, ctx->text, ctx->fonts.body, COLOR_TEXT);
    createFullWidthButton(ctx, root, "Volver", Command::Home, 0, COLOR_PRIMARY, COLOR_WHITE);
}

void renderReminders(Context* ctx, lv_obj_t* root) {
    createLabel(root, "Mis remedios", ctx->fonts.title, COLOR_TEXT);
    auto* list = createColumn(root);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_scrollable(list, true);
    const auto today = getToday();
    if (ctx->reminders.empty()) {
        createLabel(list, "No hay remedios guardados.", ctx->fonts.body, COLOR_MUTED);
    }
    for (size_t i = 0; i < ctx->reminders.size(); i++) {
        const auto& reminder = ctx->reminders[i];
        const bool taken = reminder.takenOn == today;
        auto* card = createCard(list, taken ? COLOR_SAGE_SOFT : COLOR_CARD);
        auto* row = createRow(card);
        auto* timeLabel = createLabel(row, reminder.time, ctx->fonts.title, COLOR_TEXT);
        lv_obj_set_width(timeLabel, LV_SIZE_CONTENT);
        auto* name = createLabel(row, std::format("{} {}", reminder.name, reminder.dose), ctx->fonts.body, COLOR_TEXT);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_width(name, LV_SIZE_CONTENT);
        if (!reminder.note.empty()) {
            createLabel(card, reminder.note, ctx->fonts.body, COLOR_MUTED);
        }
        if (taken) {
            createLabel(card, "Tomado hoy", ctx->fonts.body, COLOR_SAGE);
        } else {
            createFullWidthButton(ctx, card, "Ya me lo tomé", Command::MarkTaken, static_cast<int>(i), COLOR_SAGE, COLOR_WHITE);
        }
    }
    createLabel(list, "Para agregar uno, dígale: “recuérdeme tomar el remedio a las ocho”.", ctx->fonts.body, COLOR_MUTED);
    createFullWidthButton(ctx, root, "Volver", Command::Home, 0, COLOR_PRIMARY, COLOR_WHITE);
}

void renderForm(Context* ctx, lv_obj_t* root) {
    createLabel(root, "Pedir hora en el CESFAM", ctx->fonts.title, COLOR_TEXT);
    auto* list = createCard(root, COLOR_CARD);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_scrollable(list, true);
    for (const auto& field : FORM_FIELDS) {
        const auto& value = ctx->form.*field.value;
        createLabel(list, field.label, ctx->fonts.body, COLOR_MUTED);
        createLabel(list, value.empty() ? "Falta" : value, ctx->fonts.body, value.empty() ? COLOR_PRIMARY : COLOR_TEXT);
    }
    if (isFormComplete(ctx->form)) {
        createFullWidthButton(ctx, root, "Ver ficha lista", Command::FormReady, 0, COLOR_SAGE, COLOR_WHITE);
    } else {
        createFullWidthButton(ctx, root, "Completar hablando", Command::FillForm, 0, COLOR_PRIMARY, COLOR_WHITE);
    }
    auto* row = createRow(root);
    createButton(ctx, row, "Volver", Command::Home, 0, COLOR_CARD, COLOR_TEXT);
    createButton(ctx, row, "Borrar", Command::ClearForm, 0, COLOR_SECONDARY, COLOR_TEXT);
}

void renderFormReady(Context* ctx, lv_obj_t* root) {
    createLabel(root, "Ficha para pedir hora", ctx->fonts.title, COLOR_SAGE);
    auto* card = createCard(root, COLOR_CARD);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_scrollable(card, true);
    for (const auto& field : FORM_FIELDS) {
        createLabel(card, std::format("{}: {}", field.label, ctx->form.*field.value), ctx->fonts.body, COLOR_TEXT);
    }
    createLabel(root, "Muestre esta pantalla en el mesón del SOME.", ctx->fonts.body, COLOR_MUTED);
    createFullWidthButton(ctx, root, "Listo", Command::Home, 0, COLOR_PRIMARY, COLOR_WHITE);
}

void renderAlarm(Context* ctx, lv_obj_t* root) {
    lv_obj_set_style_bg_color(root, COLOR_AMBER_SOFT, LV_STATE_DEFAULT);
    if (ctx->alarmIndex < 0 || ctx->alarmIndex >= static_cast<int>(ctx->reminders.size())) {
        return;
    }
    const auto& reminder = ctx->reminders[ctx->alarmIndex];
    createLabel(root, "Hora de su remedio", ctx->fonts.title, COLOR_AMBER_INK);
    auto* card = createCard(root, COLOR_CARD);
    lv_obj_set_flex_grow(card, 1);
    createLabel(card, reminder.time, ctx->fonts.clock, COLOR_TEXT);
    createLabel(card, std::format("{} {}", reminder.name, reminder.dose), ctx->fonts.title, COLOR_TEXT);
    if (!reminder.note.empty()) {
        createLabel(card, reminder.note, ctx->fonts.body, COLOR_MUTED);
    }
    createFullWidthButton(ctx, root, "Ya me lo tomé", Command::MarkTaken, ctx->alarmIndex, COLOR_SAGE, COLOR_WHITE);
    createFullWidthButton(ctx, root, "Recordar en 10 minutos", Command::Snooze, ctx->alarmIndex, COLOR_CARD, COLOR_TEXT);
}

void renderEmergency(Context* ctx, lv_obj_t* root) {
    lv_obj_set_style_bg_color(root, COLOR_EMERGENCY, LV_STATE_DEFAULT);
    createLabel(root, "Llame al 131", ctx->fonts.clock, COLOR_WHITE);
    createLabel(root, "SAMU, ambulancia", ctx->fonts.title, COLOR_WHITE);
    auto* text = createLabel(root, ctx->text, ctx->fonts.body, COLOR_WHITE);
    lv_obj_set_flex_grow(text, 1);
    createFullWidthButton(ctx, root, "Volver", Command::Home, 0, COLOR_WHITE, COLOR_EMERGENCY);
}

/** Rebuilds the current screen. Must be called with the LVGL lock held. */
void render(Context* ctx) {
    if (ctx->root == nullptr) {
        return;
    }
    lv_obj_clean(ctx->root);
    ctx->clockLabel = nullptr;
    ctx->dateLabel = nullptr;
    ctx->volumeLabel = nullptr;
    std::fill(std::begin(ctx->levelBars), std::end(ctx->levelBars), nullptr);
    lv_obj_set_style_bg_color(ctx->root, COLOR_BACKGROUND, LV_STATE_DEFAULT);
    switch (ctx->screen) {
        case Screen::Home: renderHome(ctx, ctx->root); break;
        case Screen::Listening: renderListening(ctx, ctx->root); break;
        case Screen::Thinking: renderThinking(ctx, ctx->root); break;
        case Screen::Answer: renderAnswer(ctx, ctx->root); break;
        case Screen::Message: renderMessage(ctx, ctx->root); break;
        case Screen::Reminders: renderReminders(ctx, ctx->root); break;
        case Screen::Form: renderForm(ctx, ctx->root); break;
        case Screen::FormReady: renderFormReady(ctx, ctx->root); break;
        case Screen::Alarm: renderAlarm(ctx, ctx->root); break;
        case Screen::Emergency: renderEmergency(ctx, ctx->root); break;
    }
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    auto* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, LV_DPX(10), LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(root, LV_DPX(8), LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);
    ctx->root = root;
    render(ctx);
}

void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->root = nullptr;
    ctx->clockLabel = nullptr;
    ctx->dateLabel = nullptr;
    ctx->volumeLabel = nullptr;
    std::fill(std::begin(ctx->levelBars), std::end(ctx->levelBars), nullptr);
}

// endregion

// region Worker

void show(Context* ctx, Screen screen, const std::string& title = "", const std::string& text = "", const std::string& detail = "") {
    lvgl_lock();
    ctx->screen = screen;
    ctx->title = title;
    ctx->text = text;
    ctx->detail = detail;
    render(ctx);
    lvgl_unlock();
    ctx->screenTime = time(nullptr);
}

void showMessage(Context* ctx, const std::string& title, const std::string& text) {
    show(ctx, Screen::Message, title, text);
}

/** Each bar wobbles at its own pace, so they move out of sync while the person talks */
void updateLevel(Context* ctx, float level) {
    constexpr float SPEEDS[LEVEL_BAR_COUNT] = { 7.1f, 11.3f, 8.7f, 13.9f };
    constexpr float PHASES[LEVEL_BAR_COUNT] = { 0.0f, 1.9f, 3.7f, 5.3f };
    if (!lvgl_try_lock(10)) {
        return;
    }
    const float seconds = static_cast<float>(lv_tick_get()) / 1000.0f;
    // Quiet speech still shows clear movement
    const float loudness = std::sqrt(std::clamp(level, 0.0f, 1.0f));
    for (size_t i = 0; i < LEVEL_BAR_COUNT; i++) {
        if (ctx->levelBars[i] == nullptr) {
            continue;
        }
        const float wobble = 0.55f + 0.45f * std::sin(seconds * SPEEDS[i] + PHASES[i]);
        const float target = loudness * wobble;
        // Fast rise, slower fall
        const float smoothing = target > ctx->barLevels[i] ? 0.6f : 0.25f;
        ctx->barLevels[i] += (target - ctx->barLevels[i]) * smoothing;
        const auto height = LEVEL_BAR_WIDTH + static_cast<int32_t>(ctx->barLevels[i] * (LEVEL_BAR_MAX_HEIGHT - LEVEL_BAR_WIDTH));
        lv_obj_set_height(ctx->levelBars[i], height);
    }
    lvgl_unlock();
}

void updateClock(Context* ctx) {
    const tm local = getLocalTime();
    lvgl_lock();
    if (ctx->clockLabel != nullptr) {
        lv_label_set_text(ctx->clockLabel, getClockText(local).c_str());
    }
    if (ctx->dateLabel != nullptr) {
        lv_label_set_text(ctx->dateLabel, getDateText(local).c_str());
    }
    lvgl_unlock();
}

void changeVolume(Context* ctx, float delta) {
    const float volume = std::clamp(service::audio::getOutputVolume() + delta, 10.0f, 100.0f);
    service::audio::setOutputVolume(volume);
    lvgl_lock();
    if (ctx->volumeLabel != nullptr) {
        lv_label_set_text(ctx->volumeLabel, getVolumeText().c_str());
    }
    lvgl_unlock();
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

void markTaken(Context* ctx, int index) {
    if (index < 0 || index >= static_cast<int>(ctx->reminders.size())) {
        return;
    }
    ctx->reminders[index].takenOn = getToday();
    saveReminders(ctx->reminders);
    if (ctx->snoozeIndex == index) {
        ctx->snoozeIndex = -1;
    }
}

/** Finds the reminder that the person most likely means, preferring the next one not taken today */
int findReminderByName(Context* ctx, const std::string& name) {
    const auto wanted = toLower(name);
    const int next = findNextReminder(ctx->reminders);
    int found = -1;
    for (size_t i = 0; i < ctx->reminders.size(); i++) {
        const auto candidate = toLower(ctx->reminders[i].name);
        if (wanted.empty() || candidate.find(wanted) != std::string::npos || wanted.find(candidate) != std::string::npos) {
            if (static_cast<int>(i) == next) {
                return next;
            }
            if (found < 0 && ctx->reminders[i].takenOn != getToday()) {
                found = static_cast<int>(i);
            }
        }
    }
    return found;
}

void applyAction(Context* ctx, const Reply& reply) {
    if (reply.action == "agregar_recordatorio") {
        if (!reply.reminder.name.empty() && getMinutes(reply.reminder.time) >= 0) {
            ctx->reminders.push_back({ reply.reminder.name, reply.reminder.dose, reply.reminder.time, "", "" });
            std::sort(ctx->reminders.begin(), ctx->reminders.end(), [](const Reminder& left, const Reminder& right) {
                return getMinutes(left.time) < getMinutes(right.time);
            });
            saveReminders(ctx->reminders);
        }
    } else if (reply.action == "marcar_tomado") {
        markTaken(ctx, findReminderByName(ctx, reply.reminder.name));
    } else if (reply.action == "actualizar_formulario") {
        bool changed = false;
        for (const auto& field : FORM_FIELDS) {
            const auto& value = reply.formUpdate.*field.value;
            if (!value.empty()) {
                ctx->form.*field.value = value;
                changed = true;
            }
        }
        if (changed) {
            saveForm(ctx->form);
        }
    } else if (reply.action == "subir_volumen") {
        changeVolume(ctx, VOLUME_STEP);
    } else if (reply.action == "bajar_volumen") {
        changeVolume(ctx, -VOLUME_STEP);
    }
}

void pollStop(Context* ctx, Button& button) {
    if (button.poll()) {
        ctx->stopRequested = true;
    }
}

void drainStopCommands(Context* ctx) {
    Message message {};
    while (xQueuePeek(ctx->queue, &message, 0) == pdTRUE && message.command == Command::StopTalk) {
        xQueueReceive(ctx->queue, &message, 0);
        ctx->stopRequested = true;
    }
}

/** @return true when the speech was interrupted by the button */
bool speak(Context* ctx, Button& button, const std::string& text) {
    Pcm speech;
    const auto error = synthesize(ctx->config, text, speech);
    if (!error.empty()) {
        LOG_W(TAG, "Speech failed: %s", error.c_str());
        return false;
    }
    ctx->stopRequested = false;
    const bool completed = play(ctx->stream, speech, [ctx, &button] {
        pollStop(ctx, button);
        drainStopCommands(ctx);
        return ctx->stopRequested || !ctx->running;
    });
    return !completed && ctx->running;
}

/**
 * Runs one or more spoken turns. In form mode it keeps the conversation going until the form is complete.
 * @param[in] openingText when not empty, Gemini answers this text first instead of listening
 */
void converse(Context* ctx, Button& button, Mode mode, const std::string& openingText) {
    if (ctx->stream == nullptr) {
        showMessage(ctx, "Sin audio", "No encontré el micrófono ni el parlante.");
        return;
    }
    if (time(nullptr) - ctx->lastTurnTime > HISTORY_EXPIRY_SECONDS) {
        ctx->history.clear();
    }

    std::string pendingText = openingText;
    for (int turn = 0; turn < MAX_FORM_TURNS && ctx->running; turn++) {
        Pcm audio;
        const bool listen = pendingText.empty();
        if (listen) {
            show(ctx, Screen::Listening, "", mode == Mode::Form ? "Responda la pregunta con calma." : "");
            playTone(ctx->stream, Tone::Listen);
            ctx->stopRequested = false;
            const auto result = record(ctx->stream, audio, [ctx](float level) { updateLevel(ctx, level); }, [ctx, &button] {
                pollStop(ctx, button);
                drainStopCommands(ctx);
                return ctx->stopRequested || !ctx->running;
            });
            playTone(ctx->stream, Tone::Done);
            if (result == RecordResult::Cancelled) {
                show(ctx, mode == Mode::Form ? Screen::Form : Screen::Home);
                return;
            }
            if (result == RecordResult::Silence) {
                showMessage(ctx, "No le escuché", "Apriete el botón y hábleme cuando escuche el sonido.");
                return;
            }
            if (result == RecordResult::Failed) {
                showMessage(ctx, "Problema con el micrófono", "No pude grabar su voz. Intente de nuevo.");
                return;
            }
        }

        show(ctx, Screen::Thinking);
        const PromptContext context { &ctx->config, &ctx->reminders, &ctx->form, mode };
        Reply reply;
        const auto error = ask(context, ctx->history, listen ? &audio : nullptr, pendingText, reply);
        pendingText.clear();
        if (!error.empty()) {
            showMessage(ctx, "No pude responder", error);
            return;
        }

        applyAction(ctx, reply);
        ctx->history.push_back({ reply.heard.empty() ? "(sin palabras)" : reply.heard, reply.answer });
        if (ctx->history.size() > MAX_HISTORY_TURNS) {
            ctx->history.erase(ctx->history.begin());
        }
        ctx->lastTurnTime = time(nullptr);

        if (reply.action == "emergencia") {
            show(ctx, Screen::Emergency, "", reply.answer);
            speak(ctx, button, reply.answer);
            return;
        }

        show(ctx, Screen::Answer, "Hablando... apriete el botón para interrumpir", reply.answer, listen ? reply.heard : "");
        const bool interrupted = speak(ctx, button, reply.answer);
        const bool formDone = mode == Mode::Form && isFormComplete(ctx->form);
        if (formDone) {
            show(ctx, Screen::FormReady);
            return;
        }
        if (!interrupted && mode != Mode::Form) {
            show(ctx, Screen::Answer, "", reply.answer, listen ? reply.heard : "");
            return;
        }
    }
    show(ctx, mode == Mode::Form ? Screen::Form : Screen::Home);
}

void ringAlarm(Context* ctx, Button& button, int index) {
    ctx->alarmIndex = index;
    ctx->alarmRings++;
    ctx->lastAlarmRing = time(nullptr);
    show(ctx, Screen::Alarm);
    if (ctx->stream == nullptr) {
        return;
    }
    playTone(ctx->stream, Tone::Alarm);
    const auto& reminder = ctx->reminders[index];
    const auto greeting = ctx->config.userName.empty() ? std::string("Hola") : std::format("Hola, {}", ctx->config.userName);
    const bool interrupted = speak(ctx, button, std::format("{}. Es hora de tomar su {} de {}. {}", greeting, reminder.name, reminder.dose, reminder.note));
    // The button means "ya me lo tomé" on the alarm screen
    if (interrupted) {
        markTaken(ctx, index);
        ctx->alarmIndex = -1;
        show(ctx, Screen::Home);
    }
}

void checkAlarms(Context* ctx, Button& button) {
    const tm local = getLocalTime();
    if (!isClockSet(local)) {
        return;
    }
    const time_t now = time(nullptr);

    if (ctx->screen == Screen::Alarm) {
        if (ctx->alarmRings < ALARM_MAX_REPEATS && now - ctx->lastAlarmRing >= ALARM_REPEAT_SECONDS) {
            ringAlarm(ctx, button, ctx->alarmIndex);
        }
        return;
    }
    if (ctx->screen != Screen::Home && ctx->screen != Screen::Answer && ctx->screen != Screen::Message && ctx->screen != Screen::Reminders) {
        return;
    }

    if (ctx->snoozeIndex >= 0 && now >= ctx->snoozeUntil) {
        const int index = ctx->snoozeIndex;
        ctx->snoozeIndex = -1;
        ctx->alarmRings = 0;
        ringAlarm(ctx, button, index);
        return;
    }

    const auto clock = std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    const auto today = getToday();
    for (size_t i = 0; i < ctx->reminders.size(); i++) {
        const auto& reminder = ctx->reminders[i];
        const auto key = std::format("{} {}", today, i);
        if (reminder.time != clock || reminder.takenOn == today || std::find(ctx->firedAlarms.begin(), ctx->firedAlarms.end(), key) != ctx->firedAlarms.end()) {
            continue;
        }
        ctx->firedAlarms.push_back(key);
        if (ctx->firedAlarms.size() > 32) {
            ctx->firedAlarms.erase(ctx->firedAlarms.begin());
        }
        ctx->alarmRings = 0;
        ringAlarm(ctx, button, static_cast<int>(i));
        return;
    }
}

void handle(Context* ctx, Button& button, const Message& message) {
    switch (message.command) {
        case Command::Talk:
            converse(ctx, button, Mode::General, "");
            break;
        case Command::StopTalk:
            break;
        case Command::Home:
            show(ctx, Screen::Home);
            break;
        case Command::Reminders:
            show(ctx, Screen::Reminders);
            break;
        case Command::Form:
            show(ctx, Screen::Form);
            break;
        case Command::FormReady:
            show(ctx, Screen::FormReady);
            break;
        case Command::FillForm:
            converse(ctx, button, Mode::Form, "Quiero completar la ficha para pedir hora en el CESFAM.");
            break;
        case Command::ClearForm:
            ctx->form = {};
            saveForm(ctx->form);
            show(ctx, Screen::Form);
            break;
        case Command::VolumeUp:
            changeVolume(ctx, VOLUME_STEP);
            break;
        case Command::VolumeDown:
            changeVolume(ctx, -VOLUME_STEP);
            break;
        case Command::MarkTaken: {
            const bool fromAlarm = ctx->screen == Screen::Alarm;
            markTaken(ctx, message.argument);
            ctx->alarmIndex = -1;
            show(ctx, fromAlarm ? Screen::Home : Screen::Reminders);
            break;
        }
        case Command::Snooze:
            ctx->snoozeIndex = message.argument;
            ctx->snoozeUntil = time(nullptr) + SNOOZE_SECONDS;
            ctx->alarmIndex = -1;
            show(ctx, Screen::Home);
            break;
        case Command::Exit:
            app_event_emit_close(ctx->appInstanceId);
            break;
    }
}

void workerMain(void* parameter) {
    auto* ctx = static_cast<Context*>(parameter);
    Button button(BUTTON_PIN);
    int lastMinute = -1;
    TickType_t lastSecond = 0;

    while (ctx->running) {
        Message message {};
        if (xQueueReceive(ctx->queue, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
            handle(ctx, button, message);
        }
        if (!ctx->running) {
            break;
        }
        if (button.poll()) {
            if (ctx->screen == Screen::Alarm) {
                handle(ctx, button, { Command::MarkTaken, ctx->alarmIndex });
            } else {
                converse(ctx, button, ctx->screen == Screen::Form ? Mode::Form : Mode::General, "");
            }
        }

        const TickType_t ticks = xTaskGetTickCount();
        if (ticks - lastSecond < pdMS_TO_TICKS(1000)) {
            continue;
        }
        lastSecond = ticks;

        const tm local = getLocalTime();
        if (local.tm_min != lastMinute) {
            lastMinute = local.tm_min;
            if (ctx->screen == Screen::Home) {
                // Re-render to refresh the next reminder too
                show(ctx, Screen::Home);
            } else {
                updateClock(ctx);
            }
        }
        checkAlarms(ctx, button);

        const bool idleScreen = ctx->screen == Screen::Answer || ctx->screen == Screen::Message;
        if (idleScreen && time(nullptr) - ctx->screenTime > IDLE_RETURN_SECONDS) {
            show(ctx, Screen::Home);
        }
    }
    xSemaphoreGive(ctx->workerDone);
    vTaskDelete(nullptr);
}

Device* findAudioStream() {
    Device* found = nullptr;
    device_for_each_of_type(&AUDIO_STREAM_TYPE, &found, [](Device* device, void* context) -> bool {
        if (device_is_ready(device)) {
            *static_cast<Device**>(context) = device;
            return false;
        }
        return true;
    });
    return found;
}

// endregion

int32_t appMain(int, char*[]) {
    auto* ctx = new Context();
    ctx->appInstanceId = app_scheduler_current_app_id();
    ctx->stream = findAudioStream();
    ctx->config = loadConfig();
    ctx->reminders = loadReminders();
    ctx->form = loadForm();
    enableAutoStart();
    loadFonts(ctx->fonts);

    if (ctx->stream != nullptr && service::audio::getOutputVolume() < 30.0f) {
        service::audio::setOutputVolume(70.0f);
    }
    setIdleAmp(false);

    ctx->queue = xQueueCreate(8, sizeof(Message));
    ctx->workerDone = xSemaphoreCreateBinary();
    check(ctx->queue != nullptr && ctx->workerDone != nullptr);

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    WindowId window = window_manager_create_ext(ctx->appInstanceId, createWidgets, destroyWidgets, ctx);

    // TLS needs a large stack
    check(xTaskCreate(workerMain, "devuelta", 16384, ctx, 5, nullptr) == pdPASS);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
            }
        }
    }

    ctx->running = false;
    xSemaphoreTake(ctx->workerDone, portMAX_DELAY);
    setIdleAmp(true);

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    unloadFonts(ctx->fonts);
    vQueueDelete(ctx->queue);
    vSemaphoreDelete(ctx->workerDone);
    delete ctx;
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "cl.devuelta",
    .name = "De Vuelta",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
    .flags = 0,
    .stack = {}
};

} // namespace tt::app::devuelta

#endif
