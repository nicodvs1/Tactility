#pragma once

#include <string>
#include <vector>

namespace tt::app::devuelta {

constexpr auto* APP_ID = "cl.devuelta";

struct Config {
    std::string apiKey;
    std::string model = "gemini-3.8-flash";
    std::string thinkingLevel = "minimal";
    std::string ttsModel = "gemini-3.8-flash-tts";
    std::string voice = "Kore";
    std::string userName;
    /** Empty, or what's wrong with the configuration files */
    std::string problem;
};

struct Reminder {
    std::string name;
    std::string dose;
    /** "HH:MM", 24 hour clock */
    std::string time;
    std::string note;
    /** "YYYY-MM-DD" of the last day it was marked as taken, empty if never */
    std::string takenOn;
};

/** The "pedir hora en el CESFAM" form. Empty fields are still missing. */
struct HourRequest {
    std::string nombre;
    std::string rut;
    std::string telefono;
    std::string cesfam;
    std::string motivo;
    std::string desde;
    std::string preferencia;
};

struct FormField {
    const char* key;
    const char* label;
    std::string HourRequest::* value;
};

constexpr FormField FORM_FIELDS[] = {
    { "nombre", "Nombre", &HourRequest::nombre },
    { "rut", "RUT", &HourRequest::rut },
    { "telefono", "Teléfono", &HourRequest::telefono },
    { "cesfam", "CESFAM", &HourRequest::cesfam },
    { "motivo", "Motivo", &HourRequest::motivo },
    { "desde", "Desde cuándo", &HourRequest::desde },
    { "preferencia", "Horario preferido", &HourRequest::preferencia },
};

bool isFormComplete(const HourRequest& form);

std::string getStoragePath();

/**
 * Reads the API key from clave_gemini.txt, or else from config.json.
 * Creates both files when they don't exist, so they can be edited on the SD card.
 */
Config loadConfig();

/** Falls back to the sample reminders when none are stored */
std::vector<Reminder> loadReminders();
bool saveReminders(const std::vector<Reminder>& reminders);

HourRequest loadForm();
bool saveForm(const HourRequest& form);

/** Makes the launcher start De Vuelta at boot, unless another app was configured */
void enableAutoStart();

/** Adds a timestamped line to registro.txt on the SD card, which keeps the latest problems and timings */
void appendLog(const std::string& line);

}
