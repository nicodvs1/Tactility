#include "Data.h"

#include <Tactility/DeprecatedPaths.h>
#include <Tactility/file/File.h>
#include <Tactility/file/PropertiesFile.h>

#include <tactility/log.h>

#include <cJSON.h>

#include <sys/stat.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <map>

namespace tt::app::devuelta {

constexpr auto* TAG = "DeVuelta";

constexpr auto* CONFIG_FILE = "config.json";
constexpr auto* KEY_FILE = "clave_gemini.txt";
constexpr auto* REMINDERS_FILE = "recordatorios.json";
constexpr auto* FORM_FILE = "ficha_hora.json";
constexpr auto* LOG_FILE = "registro.txt";
constexpr size_t MAX_LOG_SIZE = 64 * 1024;

bool isFormComplete(const HourRequest& form) {
    for (const auto& field : FORM_FIELDS) {
        if ((form.*field.value).empty()) {
            return false;
        }
    }
    return true;
}

std::string getStoragePath() {
    return getAppUserPath(APP_ID);
}

static std::string getFilePath(const char* fileName) {
    return file::getChildPath(getStoragePath(), fileName);
}

/** Skips the byte order mark that some editors write at the start of UTF-8 files */
static const char* skipBom(const char* text) {
    return strncmp(text, "\xEF\xBB\xBF", 3) == 0 ? text + 3 : text;
}

static cJSON* readJson(const char* fileName) {
    auto content = file::readString(getFilePath(fileName));
    if (content == nullptr) {
        return nullptr;
    }
    return cJSON_Parse(skipBom(reinterpret_cast<const char*>(content.get())));
}

/** Removes the whitespace and quotes that easily end up around a pasted key */
static std::string trimKey(const std::string& text) {
    const auto isNoise = [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\''; };
    size_t start = 0;
    size_t end = text.size();
    while (start < end && isNoise(text[start])) start++;
    while (end > start && isNoise(text[end - 1])) end--;
    return text.substr(start, end - start);
}

/** @return the first line of the key file, empty when it's missing or blank */
static std::string readKeyFile() {
    const auto path = getFilePath(KEY_FILE);
    auto content = file::readString(path);
    if (content == nullptr) {
        file::findOrCreateDirectory(getStoragePath(), 0777);
        file::writeString(path, "");
        return "";
    }
    std::string text = skipBom(reinterpret_cast<const char*>(content.get()));
    const auto lineEnd = text.find_first_of("\r\n");
    return trimKey(lineEnd == std::string::npos ? text : text.substr(0, lineEnd));
}

static bool writeJson(const char* fileName, cJSON* json) {
    if (!file::findOrCreateDirectory(getStoragePath(), 0777)) {
        LOG_E(TAG, "Failed to create %s", getStoragePath().c_str());
        return false;
    }
    char* text = cJSON_Print(json);
    if (text == nullptr) {
        return false;
    }
    const bool result = file::writeString(getFilePath(fileName), text);
    cJSON_free(text);
    return result;
}

static std::string getString(const cJSON* object, const char* key, const std::string& fallback = "") {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : fallback;
}

Config loadConfig() {
    Config config;
    config.apiKey = readKeyFile();
    const bool exists = file::isFile(getFilePath(CONFIG_FILE));
    cJSON* json = readJson(CONFIG_FILE);
    if (json == nullptr && exists) {
        // Never overwrite a file the person edited, it may hold their key
        LOG_E(TAG, "config.json is not valid JSON");
        config.problem = "El archivo config.json tiene un error de formato. Revise las comillas y las comas, o borre el archivo para crearlo de nuevo.";
        return config;
    }
    if (json == nullptr) {
        cJSON* created = cJSON_CreateObject();
        cJSON_AddStringToObject(created, "api_key", "");
        cJSON_AddStringToObject(created, "modelo", config.model.c_str());
        cJSON_AddStringToObject(created, "razonamiento", config.thinkingLevel.c_str());
        cJSON_AddStringToObject(created, "modelo_voz", config.ttsModel.c_str());
        cJSON_AddStringToObject(created, "voz", config.voice.c_str());
        cJSON_AddStringToObject(created, "nombre_usuario", "");
        writeJson(CONFIG_FILE, created);
        cJSON_Delete(created);
        return config;
    }
    if (config.apiKey.empty()) {
        config.apiKey = trimKey(getString(json, "api_key"));
    }
    config.model = getString(json, "modelo", config.model);
    config.thinkingLevel = getString(json, "razonamiento", config.thinkingLevel);
    config.ttsModel = getString(json, "modelo_voz", config.ttsModel);
    config.voice = getString(json, "voz", config.voice);
    config.userName = getString(json, "nombre_usuario");
    cJSON_Delete(json);
    return config;
}

static std::vector<Reminder> getSampleReminders() {
    return {
        { "Losartán", "50 mg", "08:00", "Un comprimido con el desayuno", "" },
        { "Metformina", "850 mg", "13:30", "Con el almuerzo", "" },
        { "Losartán", "50 mg", "20:00", "Un comprimido con la once", "" },
        { "Atorvastatina", "20 mg", "22:00", "Antes de dormir", "" },
    };
}

std::vector<Reminder> loadReminders() {
    cJSON* json = readJson(REMINDERS_FILE);
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        auto samples = getSampleReminders();
        saveReminders(samples);
        return samples;
    }
    std::vector<Reminder> reminders;
    const cJSON* item;
    cJSON_ArrayForEach(item, json) {
        Reminder reminder {
            getString(item, "nombre"),
            getString(item, "dosis"),
            getString(item, "hora"),
            getString(item, "nota"),
            getString(item, "tomado_el"),
        };
        if (!reminder.name.empty() && reminder.time.size() == 5) {
            reminders.push_back(reminder);
        }
    }
    cJSON_Delete(json);
    return reminders;
}

bool saveReminders(const std::vector<Reminder>& reminders) {
    cJSON* json = cJSON_CreateArray();
    for (const auto& reminder : reminders) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "nombre", reminder.name.c_str());
        cJSON_AddStringToObject(item, "dosis", reminder.dose.c_str());
        cJSON_AddStringToObject(item, "hora", reminder.time.c_str());
        cJSON_AddStringToObject(item, "nota", reminder.note.c_str());
        cJSON_AddStringToObject(item, "tomado_el", reminder.takenOn.c_str());
        cJSON_AddItemToArray(json, item);
    }
    const bool result = writeJson(REMINDERS_FILE, json);
    cJSON_Delete(json);
    return result;
}

HourRequest loadForm() {
    HourRequest form;
    cJSON* json = readJson(FORM_FILE);
    if (json == nullptr) {
        return form;
    }
    for (const auto& field : FORM_FIELDS) {
        form.*field.value = getString(json, field.key);
    }
    cJSON_Delete(json);
    return form;
}

bool saveForm(const HourRequest& form) {
    cJSON* json = cJSON_CreateObject();
    for (const auto& field : FORM_FIELDS) {
        cJSON_AddStringToObject(json, field.key, (form.*field.value).c_str());
    }
    const bool result = writeJson(FORM_FILE, json);
    cJSON_Delete(json);
    return result;
}

void enableAutoStart() {
    const auto path = std::format("{}/settings/boot.properties", getDataPath());
    std::map<std::string, std::string> properties;
    if (file::isFile(path)) {
        file::loadPropertiesFile(path, properties);
    }
    if (!properties["autoStartAppId"].empty()) {
        return;
    }
    properties["autoStartAppId"] = APP_ID;
    file::findOrCreateParentDirectory(path, 0777);
    if (!file::savePropertiesFile(path, properties)) {
        LOG_W(TAG, "Failed to enable auto start");
    }
}

void appendLog(const std::string& line) {
    const auto path = getFilePath(LOG_FILE);
    struct stat fileStat {};
    const bool full = stat(path.c_str(), &fileStat) == 0 && static_cast<size_t>(fileStat.st_size) > MAX_LOG_SIZE;
    if (!file::findOrCreateDirectory(getStoragePath(), 0777)) {
        return;
    }
    std::ofstream stream(path, full ? std::ios::trunc : std::ios::app);
    if (!stream.is_open()) {
        return;
    }
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    stream << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {}\n", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, line);
}

}
