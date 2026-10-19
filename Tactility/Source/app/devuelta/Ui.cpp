#include "Ui.h"

#include <lvgl/icons/shared.h>

#include <cmath>
#include <format>

namespace tt::app::devuelta {

namespace {

constexpr int32_t MARGIN = 16;
constexpr int32_t GAP = 12;
constexpr int32_t BUTTON_HEIGHT = 56;
constexpr int32_t CLOSE_SIZE = 44;
/** Left edge of the home screen's clock, date and card */
constexpr int32_t HOME_LEFT = 24;
/** Width of the home screen's face and button column */
constexpr int32_t SIDE_WIDTH = 168;
/** Width of the face column next to an answer, message or alarm */
constexpr int32_t FACE_COLUMN = 152;
constexpr int32_t SMALL_FACE = 112;
constexpr int32_t LIST_TOP = 58;
constexpr uint32_t FADE_MILLIS = 250;
/** How far the ring reaches from the center of the face, relative to the face size */
constexpr float RING_REACH = 228.0f / 360.0f;

const lv_color_t COLOR_INK = lv_color_hex(0x1B2A4A);
const lv_color_t COLOR_MUTED = lv_color_hex(0x55607A);
const lv_color_t COLOR_TEAL = lv_color_hex(0x0F6E6A);
const lv_color_t COLOR_TEAL_DARK = lv_color_hex(0x0A514E);
const lv_color_t COLOR_TEAL_SOFT = lv_color_hex(0xE1F0EE);
const lv_color_t COLOR_SAND = lv_color_hex(0xF4E6D0);
const lv_color_t COLOR_PAPER = lv_color_hex(0xFFFBF5);
const lv_color_t COLOR_GREEN = lv_color_hex(0x2E7D32);
const lv_color_t COLOR_GREEN_SOFT = lv_color_hex(0xE5F2E6);
const lv_color_t COLOR_AMBER_INK = lv_color_hex(0x8A5A12);
const lv_color_t COLOR_RED = lv_color_hex(0xB3261E);
const lv_color_t COLOR_WHITE = lv_color_hex(0xFFFFFF);

constexpr const char* WEEKDAYS[] = { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" };
constexpr const char* MONTHS[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };

std::string getClockText(const tm& local) {
    return isClockSet(local) ? std::format("{:02}:{:02}", local.tm_hour, local.tm_min) : "--:--";
}

std::string getDateText(const tm& local) {
    if (!isClockSet(local)) {
        return "Conecte el WiFi para ver la hora";
    }
    return std::format("{} {} de {}", WEEKDAYS[local.tm_wday], local.tm_mday, MONTHS[local.tm_mon]);
}

lv_obj_t* createPanel(lv_obj_t* parent) {
    auto* panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_clickable(panel, false);
    return panel;
}

lv_obj_t* createColumn(lv_obj_t* parent, int32_t gap) {
    auto* column = createPanel(parent);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, gap, LV_STATE_DEFAULT);
    return column;
}

lv_obj_t* createRow(lv_obj_t* parent, int32_t gap) {
    auto* row = createPanel(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, LV_STATE_DEFAULT);
    return row;
}

lv_obj_t* createCard(lv_obj_t* parent, int32_t radius) {
    auto* card = createPanel(parent);
    lv_obj_set_style_bg_color(card, COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(card, radius, LV_STATE_DEFAULT);
    return card;
}

/** Lets a panel scroll its content by dragging */
void makeScrollable(lv_obj_t* panel) {
    lv_obj_set_clickable(panel, true);
    lv_obj_set_scrollable(panel, true);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
}

lv_obj_t* createLabel(lv_obj_t* parent, const std::string& text, const lv_font_t* font, lv_color_t color, int32_t width = LV_SIZE_CONTENT) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, color, LV_STATE_DEFAULT);
    if (width != LV_SIZE_CONTENT) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    }
    return label;
}

}

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

int getMinutes(const std::string& time) {
    if (time.size() != 5 || time[2] != ':') {
        return -1;
    }
    return std::atoi(time.substr(0, 2).c_str()) * 60 + std::atoi(time.substr(3, 2).c_str());
}

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

// region Widgets

void Ui::onButtonClicked(lv_event_t* event) {
    const auto* action = static_cast<ButtonAction*>(lv_event_get_user_data(event));
    action->ui->onCommand(action->command, action->argument);
}

void Ui::onButtonDeleted(lv_event_t* event) {
    delete static_cast<ButtonAction*>(lv_event_get_user_data(event));
}

void Ui::onRootDeleted(lv_event_t* event) {
    auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
    ui->root = nullptr;
    ui->content = nullptr;
    ui->clockLabel = nullptr;
    ui->dateLabel = nullptr;
}

void Ui::addAction(lv_obj_t* object, Command command, int argument) {
    auto* action = new ButtonAction { this, command, argument };
    lv_obj_add_event_cb(object, onButtonClicked, LV_EVENT_SHORT_CLICKED, action);
    lv_obj_add_event_cb(object, onButtonDeleted, LV_EVENT_DELETE, action);
}

lv_obj_t* Ui::createButton(lv_obj_t* parent, const char* icon, const std::string& text, bool filled, Command command, int argument) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_height(button, BUTTON_HEIGHT);
    lv_obj_set_style_radius(button, 16, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, filled ? COLOR_TEAL : COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, filled ? COLOR_TEAL_DARK : COLOR_TEAL_SOFT, LV_STATE_PRESSED);
    if (!filled) {
        lv_obj_set_style_border_width(button, 3, LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(button, COLOR_TEAL, LV_STATE_DEFAULT);
    }
    lv_obj_set_style_pad_hor(button, GAP, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(button, 8, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const lv_color_t color = filled ? COLOR_WHITE : COLOR_TEAL;
    if (icon != nullptr) {
        createLabel(button, icon, fonts.icons, color);
    }
    createLabel(button, text, fonts.labelBold, color);
    addAction(button, command, argument);
    return button;
}

void Ui::createCloseButton(Command command) {
    auto* button = lv_button_create(content);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, CLOSE_SIZE, CLOSE_SIZE);
    lv_obj_set_pos(button, 4, 4);
    lv_obj_set_ext_click_area(button, 8);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, COLOR_SAND, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    auto* icon = createLabel(button, LVGL_ICON_SHARED_CLOSE, fonts.icons, COLOR_INK);
    lv_obj_center(icon);
    addAction(button, command, 0);
}

lv_obj_t* Ui::createTitle(const std::string& text) {
    auto* title = createLabel(content, text, fonts.title, COLOR_INK);
    lv_obj_set_pos(title, CLOSE_SIZE + 12, 10);
    return title;
}

lv_obj_t* Ui::createFormGrid(int32_t y, int32_t width, int32_t height) {
    auto* grid = createPanel(content);
    lv_obj_set_pos(grid, MARGIN, y);
    lv_obj_set_size(grid, width, height);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(grid, 8, LV_STATE_DEFAULT);
    makeScrollable(grid);
    const int32_t cellWidth = (width - 8) / 2;
    for (const auto& field : FORM_FIELDS) {
        const auto& value = model->form.*field.value;
        auto* cell = createCard(grid, 12);
        lv_obj_set_size(cell, cellWidth, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(cell, GAP, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_ver(cell, 6, LV_STATE_DEFAULT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        createLabel(cell, field.label, fonts.body, COLOR_MUTED, LV_PCT(100));
        if (value.empty()) {
            createLabel(cell, "Falta", fonts.bodyBold, COLOR_AMBER_INK, LV_PCT(100));
        } else {
            createLabel(cell, value, fonts.labelBold, COLOR_INK, LV_PCT(100));
        }
    }
    return grid;
}

// endregion

// region Screens

FacePose Ui::renderHome(int32_t width, int32_t height) {
    const tm local = getLocalTime();
    const int32_t sideX = width - MARGIN - SIDE_WIDTH;
    const int32_t bottomTop = height - MARGIN - 2 * BUTTON_HEIGHT - GAP;
    const int32_t textWidth = sideX - HOME_LEFT - MARGIN;

    createCloseButton(Command::Exit);

    clockLabel = createLabel(content, getClockText(local), fonts.clock, COLOR_INK);
    lv_obj_set_pos(clockLabel, HOME_LEFT - 4, CLOSE_SIZE);
    dateLabel = createLabel(content, getDateText(local), fonts.label, COLOR_INK);
    lv_obj_update_layout(dateLabel);
    if (lv_obj_get_width(dateLabel) > textWidth) {
        lv_obj_set_style_text_font(dateLabel, fonts.body, LV_STATE_DEFAULT);
    }
    lv_obj_align_to(dateLabel, clockLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 4);

    auto* card = createCard(content, 24);
    lv_obj_set_pos(card, HOME_LEFT, bottomTop);
    lv_obj_set_size(card, textWidth, 2 * BUTTON_HEIGHT + GAP);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, COLOR_TEAL, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(card, GAP, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(card, GAP, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 2, LV_STATE_DEFAULT);
    const int next = findNextReminder(model->reminders);
    auto* header = createRow(card, 6);
    createLabel(header, LVGL_ICON_SHARED_MEDICATION, fonts.icons, COLOR_TEAL);
    createLabel(header, next >= 0 ? "Próximo remedio" : "Remedios de hoy", fonts.body, COLOR_TEAL);
    if (next >= 0) {
        const auto& reminder = model->reminders[next];
        auto* name = createLabel(card, reminder.name, fonts.title, COLOR_INK, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        auto* details = createRow(card, 6);
        if (!reminder.dose.empty()) {
            createLabel(details, std::format("{} ·", reminder.dose), fonts.label, COLOR_INK);
        }
        createLabel(details, reminder.time, fonts.labelBold, COLOR_INK);
    } else {
        createLabel(card, "Todos tomados", fonts.title, COLOR_INK, LV_PCT(100));
    }

    auto* side = createColumn(content, GAP);
    lv_obj_set_pos(side, sideX, bottomTop);
    lv_obj_set_size(side, SIDE_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_width(createButton(side, nullptr, "Mis remedios", false, Command::Reminders, 0), LV_PCT(100));
    lv_obj_set_width(createButton(side, nullptr, "Pedir hora", false, Command::Form, 0), LV_PCT(100));

    const int32_t faceSize = bottomTop >= 172 ? 88 : 72;
    const int32_t faceTop = 10;
    auto* hint = createLabel(content, "Apriete el botón para hablar", fonts.bodyBold, COLOR_INK, SIDE_WIDTH + 12);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_pos(hint, sideX - 6, faceTop + faceSize + 8);

    return { sideX + SIDE_WIDTH / 2, faceTop + faceSize / 2, faceSize, Expression::Rest, Ring::None, true };
}

FacePose Ui::renderConversation(int32_t width, int32_t height, const char* caption, Expression expression, Ring ring) {
    const int32_t faceSize = std::clamp<int32_t>(height - 150, 96, 160);
    const auto reach = static_cast<int32_t>(std::lround(faceSize * RING_REACH));
    const int32_t centerY = reach + 10;

    auto* label = createLabel(content, caption, fonts.title, COLOR_INK, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_pos(label, 0, centerY + reach + 10);
    if (!model->text.empty()) {
        auto* text = createLabel(content, model->text, fonts.body, COLOR_MUTED, width - 2 * MARGIN);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
        lv_obj_align_to(text, label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    return { width / 2, centerY, faceSize, expression, ring, true };
}

FacePose Ui::renderAnswer(int32_t width, int32_t height) {
    const int32_t centerY = model->speaking ? height / 2 : height / 2 - 28;

    auto* card = createCard(content, 24);
    lv_obj_set_pos(card, FACE_COLUMN, MARGIN);
    lv_obj_set_size(card, width - FACE_COLUMN - MARGIN, height - 2 * MARGIN);
    lv_obj_set_style_pad_all(card, 18, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, LV_STATE_DEFAULT);
    makeScrollable(card);
    if (!model->detail.empty()) {
        createLabel(card, std::format("Usted dijo: «{}»", model->detail), fonts.body, COLOR_MUTED, LV_PCT(100));
    }
    createLabel(card, model->text, fonts.labelBold, COLOR_INK, LV_PCT(100));
    if (!model->title.empty()) {
        createLabel(card, model->title, fonts.body, COLOR_RED, LV_PCT(100));
    }

    if (!model->speaking) {
        createCloseButton(Command::Home);
        auto* hint = createLabel(content, "Apriete el botón para seguir", fonts.bodyBold, COLOR_INK, FACE_COLUMN - 16);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
        lv_obj_set_pos(hint, 8, centerY + SMALL_FACE / 2 + 14);
    }
    return { FACE_COLUMN / 2, centerY, SMALL_FACE, model->speaking ? Expression::Speaking : Expression::Happy, Ring::None, true };
}

FacePose Ui::renderMessage(int32_t width, int32_t height) {
    const bool offline = model->text.find("internet") != std::string::npos;
    const int32_t centerY = offline ? height / 2 - 20 : height / 2;
    const int32_t columnWidth = width - FACE_COLUMN - MARGIN;

    createCloseButton(Command::Home);
    auto* column = createColumn(content, 8);
    lv_obj_set_pos(column, FACE_COLUMN, MARGIN);
    lv_obj_set_size(column, columnWidth, height - 2 * MARGIN - BUTTON_HEIGHT - GAP);
    makeScrollable(column);
    createLabel(column, model->title, fonts.labelBold, COLOR_INK, LV_PCT(100));
    createLabel(column, model->text, fonts.body, COLOR_INK, LV_PCT(100));

    auto* retry = createButton(content, LVGL_ICON_SHARED_REFRESH, "Intentar de nuevo", true, Command::Talk, 0);
    lv_obj_set_pos(retry, FACE_COLUMN, height - MARGIN - BUTTON_HEIGHT);
    lv_obj_set_width(retry, columnWidth);

    if (offline) {
        auto* icon = createLabel(content, LVGL_ICON_SHARED_WIFI_OFF, fonts.icons, COLOR_RED, FACE_COLUMN);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
        lv_obj_set_pos(icon, 0, centerY + SMALL_FACE / 2 + 18);
    }
    return { FACE_COLUMN / 2, centerY, SMALL_FACE, Expression::Worried, Ring::None, true };
}

FacePose Ui::renderAlarm(int32_t width, int32_t height) {
    const int index = model->alarmIndex;
    if (index < 0 || index >= static_cast<int>(model->reminders.size())) {
        return { 0, 0, 0, Expression::Alert, Ring::None, false };
    }
    const auto& reminder = model->reminders[index];
    const int32_t columnWidth = width - FACE_COLUMN - MARGIN;

    auto* column = createColumn(content, 2);
    lv_obj_set_pos(column, FACE_COLUMN, MARGIN + 4);
    lv_obj_set_size(column, columnWidth, LV_SIZE_CONTENT);
    createLabel(column, "Es hora de su remedio", fonts.body, COLOR_TEAL, LV_PCT(100));
    auto* name = createLabel(column, reminder.name, fonts.big, COLOR_INK, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    if (!reminder.dose.empty()) {
        createLabel(column, reminder.dose, fonts.label, COLOR_INK, LV_PCT(100));
    }
    if (!reminder.note.empty()) {
        createLabel(column, reminder.note, fonts.body, COLOR_MUTED, LV_PCT(100));
    }

    auto* taken = createButton(content, LVGL_ICON_SHARED_CHECK_CIRCLE, "Ya me la tomé", true, Command::MarkTaken, index);
    lv_obj_set_pos(taken, FACE_COLUMN, height - MARGIN - 2 * BUTTON_HEIGHT - GAP);
    lv_obj_set_width(taken, columnWidth);
    auto* snooze = createButton(content, LVGL_ICON_SHARED_SCHEDULE, "Recordar en 10 min", false, Command::Snooze, index);
    lv_obj_set_pos(snooze, FACE_COLUMN, height - MARGIN - BUTTON_HEIGHT);
    lv_obj_set_width(snooze, columnWidth);

    return { FACE_COLUMN / 2, MARGIN + SMALL_FACE / 2 + 20, SMALL_FACE, Expression::Alert, Ring::None, true };
}

void Ui::renderReminders(int32_t width, int32_t height) {
    createCloseButton(Command::Home);
    createTitle("Mis remedios");

    auto* list = createColumn(content, 8);
    lv_obj_set_pos(list, MARGIN, LIST_TOP);
    lv_obj_set_size(list, width - 2 * MARGIN, height - LIST_TOP);
    lv_obj_set_style_pad_bottom(list, MARGIN, LV_STATE_DEFAULT);
    makeScrollable(list);
    if (model->reminders.empty()) {
        createLabel(list, "No hay remedios guardados.", fonts.label, COLOR_MUTED, LV_PCT(100));
    }
    const auto today = getToday();
    for (size_t i = 0; i < model->reminders.size(); i++) {
        const auto& reminder = model->reminders[i];
        auto* row = createCard(list, 16);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(row, 14, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_ver(row, 10, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_column(row, GAP, LV_STATE_DEFAULT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        createLabel(row, reminder.time, fonts.title, COLOR_INK);
        auto* info = createColumn(row, 0);
        lv_obj_set_height(info, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(info, 1);
        createLabel(info, reminder.name, fonts.labelBold, COLOR_INK, LV_PCT(100));
        auto details = reminder.dose;
        if (!reminder.note.empty()) {
            details += details.empty() ? reminder.note : std::format(" · {}", reminder.note);
        }
        if (!details.empty()) {
            createLabel(info, details, fonts.body, COLOR_MUTED, LV_PCT(100));
        }
        if (reminder.takenOn == today) {
            auto* chip = createRow(row, 4);
            lv_obj_set_style_bg_color(chip, COLOR_GREEN_SOFT, LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_DEFAULT);
            lv_obj_set_style_radius(chip, 12, LV_STATE_DEFAULT);
            lv_obj_set_style_pad_hor(chip, 10, LV_STATE_DEFAULT);
            lv_obj_set_style_pad_ver(chip, 6, LV_STATE_DEFAULT);
            createLabel(chip, LVGL_ICON_SHARED_CHECK, fonts.icons, COLOR_GREEN);
            createLabel(chip, "Tomado hoy", fonts.bodyBold, COLOR_GREEN);
        } else {
            auto* button = createButton(row, nullptr, "Ya la tomé", false, Command::MarkTaken, static_cast<int>(i));
            lv_obj_set_height(button, 44);
        }
    }
    createLabel(list, "Para agregar uno, dígale: «recuérdeme tomar el remedio a las ocho».", fonts.body, COLOR_MUTED, LV_PCT(100));
}

void Ui::renderForm(int32_t width, int32_t height) {
    createCloseButton(Command::Home);
    createTitle("Pedir hora en el CESFAM");
    createFormGrid(LIST_TOP, width - 2 * MARGIN, height - LIST_TOP - MARGIN - BUTTON_HEIGHT - 10);

    auto* row = createRow(content, GAP);
    lv_obj_set_pos(row, MARGIN, height - MARGIN - BUTTON_HEIGHT);
    lv_obj_set_width(row, width - 2 * MARGIN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* action = isFormComplete(model->form)
        ? createButton(row, LVGL_ICON_SHARED_CHECK_CIRCLE, "Ver ficha lista", true, Command::FormReady, 0)
        : createButton(row, nullptr, "Completar hablando", true, Command::FillForm, 0);
    lv_obj_set_flex_grow(action, 1);
    lv_obj_set_width(createButton(row, nullptr, "Borrar", false, Command::ClearForm, 0), 132);
}

void Ui::renderFormReady(int32_t width, int32_t height) {
    createCloseButton(Command::Home);
    createTitle("Ficha para pedir hora");
    createFormGrid(LIST_TOP, width - 2 * MARGIN, height - LIST_TOP - MARGIN - BUTTON_HEIGHT - 10);

    auto* row = createRow(content, GAP);
    lv_obj_set_pos(row, MARGIN, height - MARGIN - BUTTON_HEIGHT);
    lv_obj_set_width(row, width - 2 * MARGIN);
    auto* note = createLabel(row, "Muestre esta pantalla en el mesón del SOME.", fonts.body, COLOR_MUTED, 1);
    lv_obj_set_flex_grow(note, 1);
    lv_obj_set_width(createButton(row, nullptr, "Listo", true, Command::Home, 0), 132);
}

void Ui::renderEmergency(int32_t width, int32_t height) {
    auto* column = createColumn(content, 6);
    lv_obj_set_pos(column, MARGIN + 8, MARGIN);
    lv_obj_set_size(column, width - 2 * MARGIN - 16, height - 2 * MARGIN - BUTTON_HEIGHT - GAP);
    makeScrollable(column);
    createLabel(column, "Llame al 131", fonts.big, COLOR_WHITE, LV_PCT(100));
    createLabel(column, "SAMU, ambulancia", fonts.title, COLOR_WHITE, LV_PCT(100));
    createLabel(column, model->text, fonts.label, COLOR_WHITE, LV_PCT(100));

    auto* back = createButton(content, nullptr, "Volver", false, Command::Home, 0);
    lv_obj_set_pos(back, MARGIN, height - MARGIN - BUTTON_HEIGHT);
    lv_obj_set_width(back, width - 2 * MARGIN);
    lv_obj_set_style_border_color(back, COLOR_WHITE, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lv_obj_get_child(back, 0), COLOR_RED, LV_STATE_DEFAULT);
}

// endregion

void Ui::configure(const UiFonts& uiFonts, std::function<void(Command, int)> commandCallback) {
    fonts = uiFonts;
    onCommand = std::move(commandCallback);
}

void Ui::create(lv_obj_t* parent, const UiModel& uiModel) {
    model = &uiModel;
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, this);

    content = createPanel(root);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    // Created after the content, so it stays on top
    face.create(root, [this] { onFaceClicked(); });

    shownScreen = model->screen;
    render(false);
}

void Ui::onFaceClicked() {
    switch (model->screen) {
        case Screen::Home:
        case Screen::Message:
            onCommand(Command::Talk, 0);
            break;
        case Screen::Answer:
            onCommand(model->speaking ? Command::StopTalk : Command::Talk, 0);
            break;
        case Screen::Listening:
            onCommand(Command::StopTalk, 0);
            break;
        default:
            break;
    }
}

void Ui::render(bool animate) {
    if (root == nullptr) {
        return;
    }
    lv_obj_clean(content);
    clockLabel = nullptr;
    dateLabel = nullptr;
    lv_obj_update_layout(root);
    const int32_t width = lv_obj_get_width(root);
    const int32_t height = lv_obj_get_height(root);

    if (model->screen == Screen::Emergency) {
        lv_obj_set_style_bg_color(root, COLOR_RED, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_NONE, LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_bg_color(root, COLOR_SAND, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_color(root, COLOR_PAPER, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, LV_STATE_DEFAULT);
    }

    FacePose pose { 0, 0, 0, Expression::Rest, Ring::None, false };
    switch (model->screen) {
        case Screen::Home: pose = renderHome(width, height); break;
        case Screen::Listening: pose = renderConversation(width, height, "Le escucho…", Expression::Attentive, Ring::Level); break;
        case Screen::Thinking: pose = renderConversation(width, height, "Déjeme ver…", Expression::Thinking, Ring::Pulse); break;
        case Screen::Answer: pose = renderAnswer(width, height); break;
        case Screen::Message: pose = renderMessage(width, height); break;
        case Screen::Alarm: pose = renderAlarm(width, height); break;
        case Screen::Reminders: renderReminders(width, height); break;
        case Screen::Form: renderForm(width, height); break;
        case Screen::FormReady: renderFormReady(width, height); break;
        case Screen::Emergency: renderEmergency(width, height); break;
    }
    face.setPose(pose, animate);
    if (animate && model->screen != shownScreen) {
        lv_obj_fade_in(content, FADE_MILLIS, 0);
    }
    shownScreen = model->screen;
}

void Ui::updateClock() {
    if (clockLabel == nullptr || dateLabel == nullptr) {
        return;
    }
    const tm local = getLocalTime();
    lv_label_set_text(clockLabel, getClockText(local).c_str());
    lv_label_set_text(dateLabel, getDateText(local).c_str());
}

}
