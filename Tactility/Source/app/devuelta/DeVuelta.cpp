#ifdef ESP_PLATFORM

#include "Assistant.h"
#include "Data.h"
#include "Ui.h"
#include "Voice.h"

#include <Tactility/lvgl/FontCache.h>
#include <Tactility/lvgl/FontVersions.h>
#include <Tactility/service/audio/Audio.h>
#include <Tactility/settings/DisplaySettings.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <binfont/binfont.h>
#include <lvgl/binfont.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/names.h>
#include <lvgl/lvgl.h>
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
constexpr size_t FONT_COUNT = 8;

struct Message {
    Command command;
    int argument;
};

struct Fonts {
    BinFont* binFonts[FONT_COUNT] = {};
    lv_font_t* created[FONT_COUNT] = {};
};

struct Context {
    uint32_t appInstanceId = 0;
    Device* stream = nullptr;
    QueueHandle_t queue = nullptr;
    SemaphoreHandle_t workerDone = nullptr;
    volatile bool running = true;
    volatile bool stopRequested = false;

    Fonts fonts;
    Ui ui;

    // Owned by the worker task, read by the LVGL task under the LVGL lock
    Config config;
    UiModel model;

    // Worker task only
    std::vector<Turn> history;
    time_t lastTurnTime = 0;
    time_t screenTime = 0;
    std::vector<std::string> firedAlarms;
    int snoozeIndex = -1;
    time_t snoozeUntil = 0;
    time_t lastAlarmRing = 0;
    int alarmRings = 0;

    /** The display rotation before the app turned it to landscape */
    lv_display_rotation_t previousRotation = LV_DISPLAY_ROTATION_0;
};

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

/** close, medication, calendar_month, check_circle, schedule, wifi_off, refresh and check */
std::vector<uint32_t> getIconCodepoints() {
    return { 0xE5CD, 0xF033, 0xEBCC, 0xF0BE, 0xEFD6, 0xE648, 0xE5D5, 0xE668 };
}

/** Generates the fonts once and caches them, which takes a few seconds on first start */
UiFonts loadFonts(Fonts& fonts) {
    const auto regular = lvgl::getSystemFontPath("AtkinsonRegular.ttf");
    const auto bold = lvgl::getSystemFontPath("AtkinsonBold.ttf");
    const auto icons = lvgl::getSystemFontPath("MaterialSymbolsRounded.ttf");
    const uint8_t bpp = lvgl::getGeneratedFontBpp();
    UiFonts uiFonts;
    struct Request {
        const char* name;
        const std::string& ttf;
        uint32_t version;
        uint16_t size;
        std::function<std::vector<uint32_t>()> getCodepoints;
        const lv_font_t** target;
    };
    const Request requests[FONT_COUNT] = {
        { "body", regular, TT_ATKINSON_REGULAR_FONT_VERSION, 18, getLatinCodepoints, &uiFonts.body },
        { "body_bold", bold, TT_ATKINSON_BOLD_FONT_VERSION, 18, getLatinCodepoints, &uiFonts.bodyBold },
        { "label", regular, TT_ATKINSON_REGULAR_FONT_VERSION, 22, getLatinCodepoints, &uiFonts.label },
        { "label_bold", bold, TT_ATKINSON_BOLD_FONT_VERSION, 22, getLatinCodepoints, &uiFonts.labelBold },
        { "title", bold, TT_ATKINSON_BOLD_FONT_VERSION, 28, getLatinCodepoints, &uiFonts.title },
        { "big", bold, TT_ATKINSON_BOLD_FONT_VERSION, 36, getLatinCodepoints, &uiFonts.big },
        { "clock", bold, TT_ATKINSON_BOLD_FONT_VERSION, 80, getClockCodepoints, &uiFonts.clock },
        { "icons", icons, LVGL_ICON_FONT_VERSION, 24, getIconCodepoints, &uiFonts.icons },
    };
    std::vector<std::string> fileNames;
    const lv_font_t* fallback = lvgl_get_text_font(FONT_SIZE_LARGE);
    for (size_t i = 0; i < FONT_COUNT; i++) {
        const auto& request = requests[i];
        const auto fileName = lvgl::getCachedFontFileName("devuelta_", request.name, request.size, bpp, request.version);
        fileNames.push_back(fileName);
        fonts.binFonts[i] = lvgl::loadOrGenerateFont(fileName, request.ttf, request.size, bpp, request.getCodepoints);
        if (fonts.binFonts[i] != nullptr) {
            lvgl_lock();
            fonts.created[i] = lvgl_binfont_create(fonts.binFonts[i]);
            lvgl_unlock();
            if (fonts.created[i] == nullptr) {
                binfont_close(fonts.binFonts[i]);
                fonts.binFonts[i] = nullptr;
            }
        }
        if (fonts.created[i] != nullptr) {
            // Characters outside the Spanish subset come from the system font
            fonts.created[i]->fallback = fallback;
            *request.target = fonts.created[i];
        } else {
            LOG_W(TAG, "Using the system font for %s", request.name);
            *request.target = fallback;
        }
    }
    lvgl::deleteStaleCachedFonts("devuelta_", fileNames);
    return uiFonts;
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

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->ui.create(parent, ctx->model);
}

// endregion

// region Worker

void show(Context* ctx, Screen screen, const std::string& title = "", const std::string& text = "", const std::string& detail = "", bool speaking = false) {
    lvgl_lock();
    ctx->model.screen = screen;
    ctx->model.title = title;
    ctx->model.text = text;
    ctx->model.detail = detail;
    ctx->model.speaking = speaking;
    ctx->ui.render();
    lvgl_unlock();
    ctx->screenTime = time(nullptr);
}

void showMessage(Context* ctx, const std::string& title, const std::string& text) {
    show(ctx, Screen::Message, title, text);
}

void updateClock(Context* ctx) {
    lvgl_lock();
    ctx->ui.updateClock();
    lvgl_unlock();
}

void changeVolume(float delta) {
    const float volume = std::clamp(service::audio::getOutputVolume() + delta, 10.0f, 100.0f);
    service::audio::setOutputVolume(volume);
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

void markTaken(Context* ctx, int index) {
    if (index < 0 || index >= static_cast<int>(ctx->model.reminders.size())) {
        return;
    }
    ctx->model.reminders[index].takenOn = getToday();
    saveReminders(ctx->model.reminders);
    if (ctx->snoozeIndex == index) {
        ctx->snoozeIndex = -1;
    }
}

/** Finds the reminder that the person most likely means, preferring the next one not taken today */
int findReminderByName(Context* ctx, const std::string& name) {
    const auto wanted = toLower(name);
    const int next = findNextReminder(ctx->model.reminders);
    int found = -1;
    for (size_t i = 0; i < ctx->model.reminders.size(); i++) {
        const auto candidate = toLower(ctx->model.reminders[i].name);
        if (wanted.empty() || candidate.find(wanted) != std::string::npos || wanted.find(candidate) != std::string::npos) {
            if (static_cast<int>(i) == next) {
                return next;
            }
            if (found < 0 && ctx->model.reminders[i].takenOn != getToday()) {
                found = static_cast<int>(i);
            }
        }
    }
    return found;
}

void applyAction(Context* ctx, const Reply& reply) {
    if (reply.action == "agregar_recordatorio") {
        if (!reply.reminder.name.empty() && getMinutes(reply.reminder.time) >= 0) {
            ctx->model.reminders.push_back({ reply.reminder.name, reply.reminder.dose, reply.reminder.time, "", "" });
            std::sort(ctx->model.reminders.begin(), ctx->model.reminders.end(), [](const Reminder& left, const Reminder& right) {
                return getMinutes(left.time) < getMinutes(right.time);
            });
            saveReminders(ctx->model.reminders);
        }
    } else if (reply.action == "marcar_tomado") {
        markTaken(ctx, findReminderByName(ctx, reply.reminder.name));
    } else if (reply.action == "actualizar_formulario") {
        bool changed = false;
        for (const auto& field : FORM_FIELDS) {
            const auto& value = reply.formUpdate.*field.value;
            if (!value.empty()) {
                ctx->model.form.*field.value = value;
                changed = true;
            }
        }
        if (changed) {
            saveForm(ctx->model.form);
        }
    } else if (reply.action == "subir_volumen") {
        changeVolume(VOLUME_STEP);
    } else if (reply.action == "bajar_volumen") {
        changeVolume(-VOLUME_STEP);
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

enum class SpeakResult {
    Completed,
    /** The button was pressed while speaking */
    Interrupted,
    Failed,
};

/** @param[out] error why the speech failed */
SpeakResult speak(Context* ctx, Button& button, const std::string& text, std::string& error) {
    ctx->stopRequested = false;
    StreamPlayer player(
        ctx->stream,
        [ctx, &button] {
            pollStop(ctx, button);
            drainStopCommands(ctx);
            return ctx->stopRequested || !ctx->running;
        },
        [ctx](float level) { ctx->ui.setLevel(level); }
    );
    error = synthesizeStreaming(ctx->config, text, [&player](const int16_t* samples, size_t count, uint32_t sampleRate) {
        return player.write(samples, count, sampleRate);
    });
    if (player.isStopRequested()) {
        player.stop();
        return ctx->running ? SpeakResult::Interrupted : SpeakResult::Completed;
    }
    if (!error.empty() && !player.hasAudio()) {
        LOG_W(TAG, "Speech failed: %s", error.c_str());
        player.stop();
        return SpeakResult::Failed;
    }
    // Plays the rest, also when the speech broke off halfway
    const auto result = player.finish();
    if (result == PlayResult::Interrupted) {
        return ctx->running ? SpeakResult::Interrupted : SpeakResult::Completed;
    }
    if (result == PlayResult::Failed) {
        error = "No pude usar el parlante.";
        appendLog("Parlante: falló la reproducción");
        return SpeakResult::Failed;
    }
    return error.empty() ? SpeakResult::Completed : SpeakResult::Failed;
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
            const auto result = record(ctx->stream, audio, [ctx](float level) { ctx->ui.setLevel(level); }, [ctx, &button] {
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
        const PromptContext context { &ctx->config, &ctx->model.reminders, &ctx->model.form, mode };
        Reply reply;
        const auto error = ask(context, ctx->history, listen ? &audio : nullptr, pendingText, reply);
        pendingText.clear();
        // Frees the recording before the speech arrives
        audio = Pcm();
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

        std::string speechError;
        if (reply.action == "emergencia") {
            show(ctx, Screen::Emergency, "", reply.answer);
            speak(ctx, button, reply.answer, speechError);
            return;
        }

        show(ctx, Screen::Answer, "", reply.answer, listen ? reply.heard : "", true);
        const auto spoken = speak(ctx, button, reply.answer, speechError);
        if (spoken == SpeakResult::Failed) {
            // The answer stays readable on the screen
            show(ctx, Screen::Answer, std::format("No pude decirlo en voz alta. {}", speechError), reply.answer, listen ? reply.heard : "");
            return;
        }
        const bool formDone = mode == Mode::Form && isFormComplete(ctx->model.form);
        if (formDone) {
            show(ctx, Screen::FormReady);
            return;
        }
        if (spoken == SpeakResult::Completed && mode != Mode::Form) {
            show(ctx, Screen::Answer, "", reply.answer, listen ? reply.heard : "");
            return;
        }
    }
    show(ctx, mode == Mode::Form ? Screen::Form : Screen::Home);
}

void ringAlarm(Context* ctx, Button& button, int index) {
    ctx->model.alarmIndex = index;
    ctx->alarmRings++;
    ctx->lastAlarmRing = time(nullptr);
    show(ctx, Screen::Alarm);
    if (ctx->stream == nullptr) {
        return;
    }
    playTone(ctx->stream, Tone::Alarm);
    const auto& reminder = ctx->model.reminders[index];
    const auto greeting = ctx->config.userName.empty() ? std::string("Hola") : std::format("Hola, {}", ctx->config.userName);
    std::string speechError;
    const auto spoken = speak(ctx, button, std::format("{}. Es hora de tomar su {} de {}. {}", greeting, reminder.name, reminder.dose, reminder.note), speechError);
    // The button means "ya me lo tomé" on the alarm screen
    if (spoken == SpeakResult::Interrupted) {
        markTaken(ctx, index);
        ctx->model.alarmIndex = -1;
        show(ctx, Screen::Home);
    }
}

void checkAlarms(Context* ctx, Button& button) {
    const tm local = getLocalTime();
    if (!isClockSet(local)) {
        return;
    }
    const time_t now = time(nullptr);

    if (ctx->model.screen == Screen::Alarm) {
        if (ctx->alarmRings < ALARM_MAX_REPEATS && now - ctx->lastAlarmRing >= ALARM_REPEAT_SECONDS) {
            ringAlarm(ctx, button, ctx->model.alarmIndex);
        }
        return;
    }
    if (ctx->model.screen != Screen::Home && ctx->model.screen != Screen::Answer && ctx->model.screen != Screen::Message && ctx->model.screen != Screen::Reminders) {
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
    for (size_t i = 0; i < ctx->model.reminders.size(); i++) {
        const auto& reminder = ctx->model.reminders[i];
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
            ctx->model.form = {};
            saveForm(ctx->model.form);
            show(ctx, Screen::Form);
            break;
        case Command::MarkTaken: {
            const bool fromAlarm = ctx->model.screen == Screen::Alarm;
            markTaken(ctx, message.argument);
            ctx->model.alarmIndex = -1;
            show(ctx, fromAlarm ? Screen::Home : Screen::Reminders);
            break;
        }
        case Command::Snooze:
            ctx->snoozeIndex = message.argument;
            ctx->snoozeUntil = time(nullptr) + SNOOZE_SECONDS;
            ctx->model.alarmIndex = -1;
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
            if (ctx->model.screen == Screen::Alarm) {
                handle(ctx, button, { Command::MarkTaken, ctx->model.alarmIndex });
            } else {
                converse(ctx, button, ctx->model.screen == Screen::Form ? Mode::Form : Mode::General, "");
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
            if (ctx->model.screen == Screen::Home) {
                // Re-render to refresh the next reminder too
                show(ctx, Screen::Home);
            } else {
                updateClock(ctx);
            }
        }
        checkAlarms(ctx, button);

        const bool idleScreen = ctx->model.screen == Screen::Answer || ctx->model.screen == Screen::Message;
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

/**
 * The screens are laid out for landscape, so a portrait display turns while the app runs.
 * @return the rotation to restore
 */
lv_display_rotation_t rotateToLandscape() {
    using settings::display::Orientation;
    lvgl_lock();
    auto* display = lv_display_get_default();
    const auto previous = lv_display_get_rotation(display);
    const auto orientation = settings::display::fromLvglDisplayRotation(previous);
    if (orientation == Orientation::Portrait || orientation == Orientation::PortraitFlipped) {
        lv_display_set_rotation(display, settings::display::toLvglDisplayRotation(Orientation::Landscape));
    }
    lvgl_unlock();
    return previous;
}

void restoreRotation(lv_display_rotation_t rotation) {
    lvgl_lock();
    lv_display_set_rotation(lv_display_get_default(), rotation);
    lvgl_unlock();
}

// endregion

int32_t appMain(int, char*[]) {
    auto* ctx = new Context();
    ctx->appInstanceId = app_scheduler_current_app_id();
    ctx->stream = findAudioStream();
    ctx->config = loadConfig();
    ctx->model.reminders = loadReminders();
    ctx->model.form = loadForm();
    enableAutoStart();
    ctx->ui.configure(loadFonts(ctx->fonts), [ctx](Command command, int argument) { send(ctx, command, argument); });

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

    ctx->previousRotation = rotateToLandscape();
    WindowId window = window_manager_create(ctx->appInstanceId, createWidgets, ctx);

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
    restoreRotation(ctx->previousRotation);
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
