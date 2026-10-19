#pragma once

#include "Data.h"
#include "Face.h"

#include <lvgl.h>

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace tt::app::devuelta {

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
    MarkTaken,
    Snooze,
    Exit,
};

struct UiFonts {
    /** 18 px */
    const lv_font_t* body = nullptr;
    const lv_font_t* bodyBold = nullptr;
    /** 22 px */
    const lv_font_t* label = nullptr;
    const lv_font_t* labelBold = nullptr;
    /** 28 px bold */
    const lv_font_t* title = nullptr;
    /** 36 px bold */
    const lv_font_t* big = nullptr;
    /** 80 px bold, digits only */
    const lv_font_t* clock = nullptr;
    /** 24 px Material Symbols */
    const lv_font_t* icons = nullptr;
};

/** What the screens show. The worker task changes it, the LVGL task reads it under the LVGL lock. */
struct UiModel {
    Screen screen = Screen::Home;
    std::string title;
    std::string text;
    std::string detail;
    /** Verito is saying the answer */
    bool speaking = false;
    int alarmIndex = -1;
    std::vector<Reminder> reminders;
    HourRequest form;
};

tm getLocalTime();
bool isClockSet(const tm& local);
std::string getToday();
int getMinutes(const std::string& time);

/** @return the index of the next reminder that isn't taken today, or -1 */
int findNextReminder(const std::vector<Reminder>& reminders);

/** The screens of the model and Verito's face, which moves between them. Needs the LVGL lock, except for setLevel(). */
class Ui {
    struct ButtonAction {
        Ui* ui;
        Command command;
        int argument;
    };

    UiFonts fonts;
    std::function<void(Command, int)> onCommand;
    const UiModel* model = nullptr;
    lv_obj_t* root = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* clockLabel = nullptr;
    lv_obj_t* dateLabel = nullptr;
    Screen shownScreen = Screen::Home;
    Face face;

    static void onButtonClicked(lv_event_t* event);
    static void onButtonDeleted(lv_event_t* event);
    static void onRootDeleted(lv_event_t* event);

    void onFaceClicked();
    void addAction(lv_obj_t* object, Command command, int argument);
    lv_obj_t* createButton(lv_obj_t* parent, const char* icon, const std::string& text, bool filled, Command command, int argument);
    void createCloseButton(Command command);
    lv_obj_t* createTitle(const std::string& text);
    lv_obj_t* createFormGrid(int32_t y, int32_t width, int32_t height);

    FacePose renderHome(int32_t width, int32_t height);
    FacePose renderConversation(int32_t width, int32_t height, const char* caption, Expression expression, Ring ring);
    FacePose renderAnswer(int32_t width, int32_t height);
    FacePose renderMessage(int32_t width, int32_t height);
    FacePose renderAlarm(int32_t width, int32_t height);
    void renderReminders(int32_t width, int32_t height);
    void renderForm(int32_t width, int32_t height);
    void renderFormReady(int32_t width, int32_t height);
    void renderEmergency(int32_t width, int32_t height);

public:
    void configure(const UiFonts& uiFonts, std::function<void(Command, int)> commandCallback);

    void create(lv_obj_t* parent, const UiModel& uiModel);

    /** Shows the screen of the model */
    void render(bool animate = true);

    void updateClock();

    /** The microphone or speaker level from 0 to 1. Can be called from any task. */
    void setLevel(float level) { face.setLevel(level); }
};

}
