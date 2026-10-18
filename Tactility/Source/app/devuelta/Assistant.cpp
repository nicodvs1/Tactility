#include "Assistant.h"

#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/log.h>

#include <cJSON.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <utility>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#endif

namespace tt::app::devuelta {

constexpr auto* TAG = "DeVuelta";

// region Buffer

static void* allocateExternal(size_t size) {
#ifdef ESP_PLATFORM
    void* result = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return result != nullptr ? result : malloc(size);
#else
    return malloc(size);
#endif
}

Buffer::Buffer(Buffer&& other) noexcept :
    bytes(std::exchange(other.bytes, nullptr)),
    length(std::exchange(other.length, 0)),
    capacity(std::exchange(other.capacity, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        free(bytes);
        bytes = std::exchange(other.bytes, nullptr);
        length = std::exchange(other.length, 0);
        capacity = std::exchange(other.capacity, 0);
    }
    return *this;
}

Buffer::~Buffer() {
    free(bytes);
}

bool Buffer::reserve(size_t newCapacity) {
    if (newCapacity <= capacity) {
        return true;
    }
    auto* newBytes = static_cast<uint8_t*>(allocateExternal(newCapacity));
    if (newBytes == nullptr) {
        return false;
    }
    if (bytes != nullptr) {
        memcpy(newBytes, bytes, length);
        free(bytes);
    }
    bytes = newBytes;
    capacity = newCapacity;
    return true;
}

bool Buffer::append(const void* data, size_t size) {
    if (length + size > capacity && !reserve(std::max(capacity * 2, length + size + 4096))) {
        return false;
    }
    memcpy(bytes + length, data, size);
    length += size;
    return true;
}

// endregion

#ifdef ESP_PLATFORM

constexpr auto* API_BASE = "https://generativelanguage.googleapis.com/v1beta";
constexpr auto* AUDIO_PLACEHOLDER = "@@AUDIO@@";
constexpr auto* SPEECH_STYLE = "Voz cálida, tranquila y clara, como una persona amable que acompaña a un adulto mayor. Ritmo pausado. Acento chileno.";

constexpr const char* WEEKDAYS[] = { "domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado" };
constexpr const char* MONTHS[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" };

// region HTTP

struct Part {
    const char* data;
    size_t size;
};

/** The Google Trust Services roots of the Gemini API, RSA (R1) and ECDSA (R4) */
constexpr char GOOGLE_ROOT_CERTIFICATES[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

static int postOnce(const std::string& url, const std::string& apiKey, const std::vector<Part>& parts, Buffer& response, std::string& failure) {
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 45000;
    config.addr_type = HTTP_ADDR_TYPE_INET;
    config.cert_pem = GOOGLE_ROOT_CERTIFICATES;
    config.cert_len = sizeof(GOOGLE_ROOT_CERTIFICATES);
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        failure = "init";
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-goog-api-key", apiKey.c_str());

    size_t total = 0;
    for (const auto& part : parts) {
        total += part.size;
    }

    int status = -1;
    const esp_err_t openResult = esp_http_client_open(client, static_cast<int>(total));
    if (openResult != ESP_OK) {
        int tlsCode = 0;
        int tlsFlags = 0;
        const esp_err_t tlsError = esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
        failure = std::format("open {}, {} -0x{:X} {:X}, ram {}k/{}k", esp_err_to_name(openResult), esp_err_to_name(tlsError), -tlsCode, tlsFlags,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    } else {
        bool written = true;
        for (const auto& part : parts) {
            size_t offset = 0;
            while (written && offset < part.size) {
                const int chunk = std::min<size_t>(part.size - offset, 4096);
                const int result = esp_http_client_write(client, part.data + offset, chunk);
                if (result <= 0) {
                    failure = std::format("write {}", result);
                    written = false;
                } else {
                    offset += result;
                }
            }
        }

        const int64_t contentLength = written ? esp_http_client_fetch_headers(client) : -1;
        if (written && (contentLength >= 0 || esp_http_client_is_chunked_response(client))) {
            response.clear();
            response.reserve(contentLength > 0 ? contentLength + 1 : 65536);
            uint8_t chunk[2048];
            int count;
            bool complete = true;
            while ((count = esp_http_client_read(client, reinterpret_cast<char*>(chunk), sizeof(chunk))) > 0) {
                if (!response.append(chunk, count)) {
                    failure = "memoria";
                    complete = false;
                    break;
                }
            }
            if (complete && (count < 0 || !esp_http_client_is_complete_data_received(client))) {
                failure = std::format("read {}, {} of {} bytes", count, response.size(), contentLength);
                complete = false;
            }
            const uint8_t terminator = 0;
            if (complete && !response.append(&terminator, 1)) {
                failure = "memoria";
                complete = false;
            }
            if (complete) {
                response.setSize(response.size() - 1);
                status = esp_http_client_get_status_code(client);
            }
        } else if (written) {
            failure = std::format("headers {}", contentLength);
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return status;
}

static bool isWifiConnected() {
    Device* wifi = nullptr;
    if (device_get_first_by_type(&WIFI_TYPE, &wifi) != ERROR_NONE) {
        return false;
    }
    WifiStationState state = WIFI_STATION_STATE_DISCONNECTED;
    wifi_get_station_state(wifi, &state);
    device_put(wifi);
    return state == WIFI_STATION_STATE_CONNECTED;
}

static bool isTransientStatus(int status) {
    return status == 500 || status == 502 || status == 503 || status == 504;
}

/**
 * Retries once when the connection couldn't be opened or the service was temporarily unavailable.
 * @param[out] failure where the request stopped, when it didn't complete
 * @return the HTTP status, or -1 when the request didn't complete
 */
static int post(const std::string& url, const std::string& apiKey, const std::vector<Part>& parts, Buffer& response, std::string& failure) {
    int status = postOnce(url, apiKey, parts, response, failure);
    if ((status < 0 && failure.starts_with("open")) || isTransientStatus(status)) {
        LOG_W(TAG, "Retrying after %d %s", status, failure.c_str());
        appendLog(std::format("Reintento tras {} {}", status, failure));
        vTaskDelay(pdMS_TO_TICKS(2000));
        failure.clear();
        status = postOnce(url, apiKey, parts, response, failure);
    }
    return status;
}

static std::string getClockState() {
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    return local.tm_year + 1900 >= 2025 ? "hora ok" : "hora sin sincronizar";
}

/** @return the error message of a Gemini error response, shortened for the screen */
static std::string getServiceMessage(const Buffer& response) {
    std::string message;
    cJSON* json = cJSON_Parse(reinterpret_cast<const char*>(response.data()));
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(json, "error");
    const cJSON* detail = cJSON_GetObjectItemCaseSensitive(error, "message");
    if (cJSON_IsString(detail)) {
        message = detail->valuestring;
    } else if (response.size() > 0) {
        message = std::string(reinterpret_cast<const char*>(response.data()), std::min<size_t>(response.size(), 200));
    }
    cJSON_Delete(json);
    constexpr size_t MAX_LENGTH = 160;
    if (message.size() > MAX_LENGTH) {
        message = message.substr(0, MAX_LENGTH) + "...";
    }
    return message;
}

static std::string describeHttpError(const char* request, int status, const Buffer& response, const std::string& failure) {
    if (status < 0) {
        LOG_E(TAG, "%s failed at %s", request, failure.c_str());
        appendLog(std::format("{}: sin respuesta, {}, {}", request, failure, getClockState()));
        if (failure == "memoria") {
            return "No hay memoria suficiente para recibir la respuesta. Intente de nuevo.";
        }
        if (!isWifiConnected()) {
            return "No me pude conectar a internet. Revise que el WiFi esté funcionando.";
        }
        return std::format("El WiFi está conectado, pero no pude llegar a Gemini. Intente de nuevo. (Detalle: {}, {})", failure, getClockState());
    }
    const auto message = getServiceMessage(response);
    LOG_E(TAG, "%s HTTP %d: %s", request, status, message.c_str());
    appendLog(std::format("{}: error {}: {}", request, status, message));
    if (status == 401 || status == 403) {
        return std::format("Hay un problema con la clave de Gemini. Revise el archivo clave_gemini.txt. (Error {}: {})", status, message);
    }
    if (status == 400 || status == 404) {
        return std::format("El servicio rechazó la petición. (Error {}: {})", status, message);
    }
    if (status == 429) {
        return std::format("Se alcanzó el límite de uso de Gemini. Intente de nuevo en un rato. (Error 429: {})", message);
    }
    return std::format("El servicio no respondió bien. Intente de nuevo en un momento. (Error {}: {})", status, message);
}

// endregion

// region Prompt

static std::string formatNow() {
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    return std::format("{} {} de {} de {}, {:02}:{:02}", WEEKDAYS[local.tm_wday], local.tm_mday, MONTHS[local.tm_mon], local.tm_year + 1900, local.tm_hour, local.tm_min);
}

static std::string formatToday() {
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    return std::format("{:04}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

static std::string buildSystemPrompt(const PromptContext& context) {
    std::string prompt = R"(Eres Verito, la voz de De Vuelta: un aparato de un solo botón que acompaña a personas mayores de Chile a orientarse en el sistema público de salud (Fonasa, CESFAM, SAPU, SAR, hospitales). La persona aprieta un botón, te habla y te escucha por un parlante. También ve tu respuesta escrita en una pantalla pequeña.

Cómo hablas:
- Español de Chile, trato de usted, cálido y respetuoso, sin tratar a la persona como niño.
- Respuestas cortas: máximo tres oraciones, un solo paso a la vez. Sin listas, sin palabras técnicas y sin siglas sin explicar.
- Si hay varios pasos, da solo el primero y ofrece seguir con el siguiente.
- No uses emojis, asteriscos, viñetas ni ningún formato: todo se lee en voz alta.
- Si no entendiste el audio o está vacío, pide con amabilidad que lo repita.

Límites:
- No diagnosticas, no recetas, no cambias dosis ni opinas sobre remedios. Para eso, dile que lo vea con su médico o en el CESFAM.
- Ante una urgencia (dolor en el pecho, falta de aire, desmayo, cara chueca o no poder hablar, caída con golpe en la cabeza, sangrado abundante) dile de inmediato que llame al 131, el SAMU, o vaya a urgencias, y usa la acción "emergencia".
- Si no estás seguro de un dato (horarios o requisitos de un CESFAM específico), dilo y sugiere llamar a Salud Responde al 600 360 7777 o preguntar en el SOME del CESFAM.

Acciones (campo "accion"):
- "ninguna": respuesta normal.
- "agregar_recordatorio": pide que le recuerdes un remedio. Llena "recordatorio" con nombre, dosis y hora en formato HH:MM de 24 horas. Si falta la hora, pregúntala antes y usa "ninguna". Al agregarlo, confirma en voz alta qué y a qué hora.
- "marcar_tomado": dice que ya se tomó un remedio. Pon su nombre en "recordatorio".
- "actualizar_formulario": te dio datos para la ficha para pedir hora. Pon en "formulario" solo los datos nuevos y pregunta por el siguiente que falta, de a uno. Repite el RUT para confirmarlo.
- "subir_volumen" o "bajar_volumen": pide que hables más fuerte o más bajo.
- "emergencia": la urgencia descrita arriba.

En "dijo" escribe lo que dijo la persona, tal cual lo dijo.
)";

    prompt += std::format("\nAhora es {}.\n", formatNow());
    if (!context.config->userName.empty()) {
        prompt += std::format("La persona se llama {}.\n", context.config->userName);
    }

    const auto today = formatToday();
    prompt += "\nSus remedios con recordatorio:\n";
    if (context.reminders->empty()) {
        prompt += "- Ninguno todavía.\n";
    }
    for (const auto& reminder : *context.reminders) {
        prompt += std::format("- {} {} a las {} ({}){}\n", reminder.name, reminder.dose, reminder.time, reminder.note, reminder.takenOn == today ? ", ya se lo tomó hoy" : "");
    }

    prompt += "\nFicha para pedir hora en el CESFAM:\n";
    for (const auto& field : FORM_FIELDS) {
        const auto& value = context.form->*field.value;
        prompt += std::format("- {} ({}): {}\n", field.label, field.key, value.empty() ? "falta" : value);
    }

    if (context.mode == Mode::Form) {
        prompt += "\nAhora están completando la ficha para pedir hora. Pregunta por el siguiente dato que falta, de a uno, en orden. "
            "Cuando estén todos, dile que la ficha está lista y que puede mostrarla en el mesón del SOME o leerla por teléfono.\n";
    }
    return prompt;
}

static cJSON* createStringProperty(cJSON* properties, const char* name, const char* description) {
    cJSON* property = cJSON_AddObjectToObject(properties, name);
    cJSON_AddStringToObject(property, "type", "STRING");
    cJSON_AddStringToObject(property, "description", description);
    return property;
}

static cJSON* createResponseSchema() {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "OBJECT");
    cJSON* properties = cJSON_AddObjectToObject(schema, "properties");

    createStringProperty(properties, "dijo", "Lo que dijo la persona");
    createStringProperty(properties, "respuesta", "Lo que le vas a decir en voz alta");
    cJSON* action = createStringProperty(properties, "accion", "La acción a realizar");
    const char* actions[] = { "ninguna", "agregar_recordatorio", "marcar_tomado", "actualizar_formulario", "subir_volumen", "bajar_volumen", "emergencia" };
    cJSON_AddItemToObject(action, "enum", cJSON_CreateStringArray(actions, std::size(actions)));

    cJSON* reminder = cJSON_AddObjectToObject(properties, "recordatorio");
    cJSON_AddStringToObject(reminder, "type", "OBJECT");
    cJSON* reminderProperties = cJSON_AddObjectToObject(reminder, "properties");
    createStringProperty(reminderProperties, "nombre", "Nombre del remedio");
    createStringProperty(reminderProperties, "dosis", "Dosis, por ejemplo 50 mg");
    createStringProperty(reminderProperties, "hora", "Hora en formato HH:MM de 24 horas");

    cJSON* form = cJSON_AddObjectToObject(properties, "formulario");
    cJSON_AddStringToObject(form, "type", "OBJECT");
    cJSON* formProperties = cJSON_AddObjectToObject(form, "properties");
    for (const auto& field : FORM_FIELDS) {
        createStringProperty(formProperties, field.key, field.label);
    }

    const char* required[] = { "dijo", "respuesta", "accion" };
    cJSON_AddItemToObject(schema, "required", cJSON_CreateStringArray(required, std::size(required)));
    return schema;
}

static void addTextContent(cJSON* contents, const char* role, const std::string& text) {
    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "role", role);
    cJSON* parts = cJSON_AddArrayToObject(content, "parts");
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text.c_str());
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(contents, content);
}

static cJSON* createAskRequest(const PromptContext& context, const std::vector<Turn>& history, bool hasAudio, const std::string& text, const std::string& thinkingLevel) {
    cJSON* request = cJSON_CreateObject();

    cJSON* system = cJSON_AddObjectToObject(request, "systemInstruction");
    cJSON* systemParts = cJSON_AddArrayToObject(system, "parts");
    cJSON* systemPart = cJSON_CreateObject();
    cJSON_AddStringToObject(systemPart, "text", buildSystemPrompt(context).c_str());
    cJSON_AddItemToArray(systemParts, systemPart);

    cJSON* contents = cJSON_AddArrayToObject(request, "contents");
    for (const auto& turn : history) {
        addTextContent(contents, "user", turn.user);
        addTextContent(contents, "model", turn.model);
    }

    cJSON* content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "role", "user");
    cJSON* parts = cJSON_AddArrayToObject(content, "parts");
    cJSON* textPart = cJSON_CreateObject();
    cJSON_AddStringToObject(textPart, "text", hasAudio ? "Esto es lo que dijo la persona:" : text.c_str());
    cJSON_AddItemToArray(parts, textPart);
    if (hasAudio) {
        cJSON* audioPart = cJSON_CreateObject();
        cJSON* inlineData = cJSON_AddObjectToObject(audioPart, "inlineData");
        cJSON_AddStringToObject(inlineData, "mimeType", "audio/wav");
        cJSON_AddStringToObject(inlineData, "data", AUDIO_PLACEHOLDER);
        cJSON_AddItemToArray(parts, audioPart);
    }
    cJSON_AddItemToArray(contents, content);

    cJSON* generation = cJSON_AddObjectToObject(request, "generationConfig");
    cJSON_AddStringToObject(generation, "responseMimeType", "application/json");
    cJSON_AddItemToObject(generation, "responseSchema", createResponseSchema());
    // Room for the thinking tokens too
    cJSON_AddNumberToObject(generation, "maxOutputTokens", 4096);
    if (!thinkingLevel.empty()) {
        cJSON* thinking = cJSON_AddObjectToObject(generation, "thinkingConfig");
        cJSON_AddStringToObject(thinking, "thinkingLevel", thinkingLevel.c_str());
    }
    return request;
}

// endregion

// region Audio encoding

static bool encodeWav(const Pcm& audio, Buffer& wav) {
    const uint32_t dataSize = audio.samples.size();
    const uint32_t byteRate = audio.sampleRate * sizeof(int16_t);
    const uint8_t header[44] = {
        'R', 'I', 'F', 'F',
        static_cast<uint8_t>((36 + dataSize)), static_cast<uint8_t>((36 + dataSize) >> 8), static_cast<uint8_t>((36 + dataSize) >> 16), static_cast<uint8_t>((36 + dataSize) >> 24),
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0,
        static_cast<uint8_t>(audio.sampleRate), static_cast<uint8_t>(audio.sampleRate >> 8), static_cast<uint8_t>(audio.sampleRate >> 16), static_cast<uint8_t>(audio.sampleRate >> 24),
        static_cast<uint8_t>(byteRate), static_cast<uint8_t>(byteRate >> 8), static_cast<uint8_t>(byteRate >> 16), static_cast<uint8_t>(byteRate >> 24),
        2, 0, 16, 0,
        'd', 'a', 't', 'a',
        static_cast<uint8_t>(dataSize), static_cast<uint8_t>(dataSize >> 8), static_cast<uint8_t>(dataSize >> 16), static_cast<uint8_t>(dataSize >> 24),
    };
    return wav.reserve(sizeof(header) + dataSize) && wav.append(header, sizeof(header)) && wav.append(audio.samples.data(), dataSize);
}

static bool encodeBase64(const Buffer& input, Buffer& output) {
    size_t required = 0;
    mbedtls_base64_encode(nullptr, 0, &required, input.data(), input.size());
    if (!output.reserve(required)) {
        return false;
    }
    size_t written = 0;
    if (mbedtls_base64_encode(output.data(), output.getCapacity(), &written, input.data(), input.size()) != 0) {
        return false;
    }
    output.setSize(written);
    return true;
}

// endregion

// region Ask

static std::string getString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

/** @return why the first candidate stopped, or why the prompt was blocked */
static std::string getFinishReason(const cJSON* response) {
    const cJSON* feedback = cJSON_GetObjectItemCaseSensitive(response, "promptFeedback");
    const auto blocked = getString(feedback, "blockReason");
    if (!blocked.empty()) {
        return blocked;
    }
    const cJSON* candidate = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(response, "candidates"), 0);
    return getString(candidate, "finishReason");
}

/** @return the non-thought text of the first candidate */
static std::string getAnswerText(const cJSON* response) {
    const cJSON* candidates = cJSON_GetObjectItemCaseSensitive(response, "candidates");
    const cJSON* candidate = cJSON_GetArrayItem(candidates, 0);
    const cJSON* content = cJSON_GetObjectItemCaseSensitive(candidate, "content");
    const cJSON* parts = cJSON_GetObjectItemCaseSensitive(content, "parts");
    std::string text;
    const cJSON* part;
    cJSON_ArrayForEach(part, parts) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(part, "thought"))) {
            continue;
        }
        text += getString(part, "text");
    }
    return text;
}

static bool parseReply(const std::string& text, Reply& reply) {
    cJSON* json = cJSON_Parse(text.c_str());
    if (json == nullptr) {
        return false;
    }
    reply.heard = getString(json, "dijo");
    reply.answer = getString(json, "respuesta");
    reply.action = getString(json, "accion");
    const cJSON* reminder = cJSON_GetObjectItemCaseSensitive(json, "recordatorio");
    reply.reminder.name = getString(reminder, "nombre");
    reply.reminder.dose = getString(reminder, "dosis");
    reply.reminder.time = getString(reminder, "hora");
    const cJSON* form = cJSON_GetObjectItemCaseSensitive(json, "formulario");
    for (const auto& field : FORM_FIELDS) {
        reply.formUpdate.*field.value = getString(form, field.key);
    }
    cJSON_Delete(json);
    return !reply.answer.empty();
}

std::string ask(const PromptContext& context, const std::vector<Turn>& history, const Pcm* audio, const std::string& text, Reply& reply) {
    if (!context.config->problem.empty()) {
        return context.config->problem;
    }
    if (context.config->apiKey.empty()) {
        return "Falta la clave de Gemini. Péguela en el archivo clave_gemini.txt de la tarjeta SD.";
    }

    Buffer audioBase64;
    if (audio != nullptr) {
        Buffer wav;
        if (!encodeWav(*audio, wav) || !encodeBase64(wav, audioBase64)) {
            return "No hay memoria suficiente para enviar el audio.";
        }
    }

    const auto url = std::format("{}/models/{}:generateContent", API_BASE, context.config->model);
    Buffer response;
    std::string failure;
    int status = -1;
    // A model that rejects the configured thinking level is retried with "low", and then with its default
    std::vector<std::string> thinkingLevels { context.config->thinkingLevel };
    for (const char* fallback : { "low", "" }) {
        if (std::find(thinkingLevels.begin(), thinkingLevels.end(), fallback) == thinkingLevels.end()) {
            thinkingLevels.push_back(fallback);
        }
    }
    const TickType_t start = xTaskGetTickCount();
    for (size_t attempt = 0; attempt < thinkingLevels.size(); attempt++) {
        cJSON* request = createAskRequest(context, history, audio != nullptr, text, thinkingLevels[attempt]);
        char* json = cJSON_PrintUnformatted(request);
        cJSON_Delete(request);
        if (json == nullptr) {
            return "No hay memoria suficiente para preparar la pregunta.";
        }

        std::vector<Part> parts;
        const char* placeholder = audio != nullptr ? strstr(json, AUDIO_PLACEHOLDER) : nullptr;
        if (placeholder != nullptr) {
            parts.push_back({ json, static_cast<size_t>(placeholder - json) });
            parts.push_back({ reinterpret_cast<const char*>(audioBase64.data()), audioBase64.size() });
            const char* rest = placeholder + strlen(AUDIO_PLACEHOLDER);
            parts.push_back({ rest, strlen(rest) });
        } else {
            parts.push_back({ json, strlen(json) });
        }

        status = post(url, context.config->apiKey, parts, response, failure);
        cJSON_free(json);
        if (status != 400 || attempt + 1 == thinkingLevels.size()) {
            break;
        }
        LOG_W(TAG, "Retrying with thinking level '%s'", thinkingLevels[attempt + 1].c_str());
        appendLog(std::format("Pregunta: error 400 con razonamiento '{}': {}", thinkingLevels[attempt], getServiceMessage(response)));
    }

    if (status != 200) {
        return describeHttpError("Pregunta", status, response, failure);
    }

    cJSON* json = cJSON_Parse(reinterpret_cast<const char*>(response.data()));
    const auto answerText = getAnswerText(json);
    const auto finishReason = getFinishReason(json);
    cJSON_Delete(json);
    if (!parseReply(answerText, reply)) {
        LOG_E(TAG, "Unexpected answer (%s): %s", finishReason.c_str(), answerText.c_str());
        appendLog(std::format("Pregunta: respuesta inesperada ({}): {}", finishReason, answerText.substr(0, 200)));
        return std::format("No entendí la respuesta del servicio. Intente de nuevo. (Detalle: {})", finishReason.empty() ? "respuesta vacía" : finishReason);
    }
    appendLog(std::format("Pregunta respondida en {} ms", pdTICKS_TO_MS(xTaskGetTickCount() - start)));
    return "";
}

// endregion

// region Speech

static uint32_t readLittleEndian16(const uint8_t* bytes) {
    return bytes[0] | (bytes[1] << 8);
}

static uint32_t readLittleEndian32(const uint8_t* bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

/** Appends 16-bit PCM, mixed down to mono */
static bool appendSamples(Pcm& speech, const uint8_t* bytes, size_t size, uint32_t channels) {
    if (channels <= 1) {
        return speech.samples.append(bytes, size & ~static_cast<size_t>(1));
    }
    const size_t frameSize = channels * sizeof(int16_t);
    if (!speech.samples.reserve(speech.samples.size() + size / channels)) {
        return false;
    }
    for (size_t offset = 0; offset + frameSize <= size; offset += frameSize) {
        int32_t sum = 0;
        for (uint32_t channel = 0; channel < channels; channel++) {
            sum += static_cast<int16_t>(readLittleEndian16(bytes + offset + channel * sizeof(int16_t)));
        }
        const auto sample = static_cast<int16_t>(sum / static_cast<int32_t>(channels));
        speech.samples.append(&sample, sizeof(sample));
    }
    return true;
}

/**
 * Appends one decoded audio piece: a WAV file, or raw 16-bit little-endian PCM.
 * @return empty on success, or a message for the user
 */
static std::string appendAudioPiece(Pcm& speech, const Buffer& decoded, uint32_t sampleRate, uint32_t channels) {
    const uint8_t* bytes = decoded.data();
    const size_t size = decoded.size();
    if (size < 12 || memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
        speech.sampleRate = sampleRate;
        return appendSamples(speech, bytes, size, channels) ? "" : "No hay memoria suficiente para el audio.";
    }
    size_t offset = 12;
    while (offset + 8 <= size) {
        const uint32_t chunkSize = readLittleEndian32(bytes + offset + 4);
        const size_t remaining = size - offset - 8;
        if (memcmp(bytes + offset, "fmt ", 4) == 0 && remaining >= 16) {
            const uint32_t format = readLittleEndian16(bytes + offset + 8);
            channels = readLittleEndian16(bytes + offset + 10);
            sampleRate = readLittleEndian32(bytes + offset + 12);
            const uint32_t bits = readLittleEndian16(bytes + offset + 22);
            if (format != 1 || bits != 16 || channels == 0) {
                LOG_E(TAG, "Unsupported WAV: format %u, %u bits, %u channels", static_cast<unsigned>(format), static_cast<unsigned>(bits), static_cast<unsigned>(channels));
                return std::format("El audio llegó en un formato que no puedo reproducir ({} bits).", bits);
            }
        } else if (memcmp(bytes + offset, "data", 4) == 0) {
            // Streaming encoders may leave the size at 0 or at the maximum
            const size_t dataSize = chunkSize == 0 || chunkSize > remaining ? remaining : chunkSize;
            speech.sampleRate = sampleRate;
            return appendSamples(speech, bytes + offset + 8, dataSize, channels) ? "" : "No hay memoria suficiente para el audio.";
        }
        offset += 8 + chunkSize + (chunkSize & 1);
    }
    return "El audio llegó incompleto.";
}

/** Collects the objects that hold audio data anywhere in the response, in the order they were generated */
static void findAudioItems(const cJSON* node, std::vector<const cJSON*>& items) {
    if (cJSON_IsObject(node)) {
        const bool isAudio = getString(node, "type") == "audio" || getString(node, "mime_type").starts_with("audio/") || getString(node, "mimeType").starts_with("audio/");
        if (isAudio && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(node, "data"))) {
            items.push_back(node);
            return;
        }
    }
    const cJSON* child;
    cJSON_ArrayForEach(child, node) {
        findAudioItems(child, items);
    }
}

std::string synthesize(const Config& config, const std::string& text, Pcm& speech) {
    if (config.apiKey.empty()) {
        return "Falta la clave de Gemini.";
    }

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "model", config.ttsModel.c_str());
    cJSON_AddBoolToObject(request, "store", false);
    cJSON* input = cJSON_AddArrayToObject(request, "input");
    cJSON* textInput = cJSON_CreateObject();
    cJSON_AddStringToObject(textInput, "type", "text");
    cJSON_AddStringToObject(textInput, "text", text.c_str());
    cJSON* annotations = cJSON_AddArrayToObject(textInput, "annotations");
    cJSON* annotation = cJSON_CreateObject();
    cJSON_AddStringToObject(annotation, "type", "speech_metadata");
    cJSON_AddStringToObject(annotation, "style", SPEECH_STYLE);
    cJSON_AddItemToArray(annotations, annotation);
    cJSON_AddItemToArray(input, textInput);
    const char* modalities[] = { "audio" };
    cJSON_AddItemToObject(request, "response_modalities", cJSON_CreateStringArray(modalities, 1));
    cJSON* format = cJSON_AddObjectToObject(request, "response_format");
    cJSON_AddStringToObject(format, "type", "audio");
    cJSON_AddStringToObject(format, "mime_type", "audio/wav");
    cJSON_AddNumberToObject(format, "sample_rate", 16000);
    cJSON* generation = cJSON_AddObjectToObject(request, "generation_config");
    cJSON* speechConfigs = cJSON_AddArrayToObject(generation, "speech_config");
    cJSON* speechConfig = cJSON_CreateObject();
    cJSON_AddStringToObject(speechConfig, "voice", config.voice.c_str());
    cJSON_AddItemToArray(speechConfigs, speechConfig);

    char* json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (json == nullptr) {
        return "No hay memoria suficiente.";
    }
    const TickType_t start = xTaskGetTickCount();
    Buffer response;
    std::string failure;
    const int status = post(std::format("{}/interactions", API_BASE), config.apiKey, { { json, strlen(json) } }, response, failure);
    cJSON_free(json);
    if (status != 200) {
        return describeHttpError("Voz", status, response, failure);
    }

    cJSON* parsed = cJSON_Parse(reinterpret_cast<const char*>(response.data()));
    // The parsed copy holds the audio from here on
    response = Buffer();
    if (parsed == nullptr) {
        appendLog("Voz: la respuesta no es JSON válido");
        return "La voz llegó dañada.";
    }
    std::vector<const cJSON*> items;
    findAudioItems(parsed, items);
    speech.samples.clear();
    std::string error = items.empty() ? "El servicio no devolvió audio." : "";
    for (const cJSON* item : items) {
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(item, "data");
        if (!cJSON_IsString(data) || data->valuestring == nullptr) {
            continue;
        }
        const auto* base64 = reinterpret_cast<const uint8_t*>(data->valuestring);
        const size_t length = strlen(data->valuestring);
        size_t decodedSize = 0;
        mbedtls_base64_decode(nullptr, 0, &decodedSize, base64, length);
        Buffer decoded;
        size_t written = 0;
        if (!decoded.reserve(decodedSize + 1)) {
            error = "No hay memoria suficiente para el audio.";
            break;
        }
        if (mbedtls_base64_decode(decoded.data(), decoded.getCapacity(), &written, base64, length) != 0) {
            error = "La voz llegó dañada.";
            break;
        }
        decoded.setSize(written);
        const cJSON* rate = cJSON_GetObjectItemCaseSensitive(item, "sample_rate");
        const cJSON* channels = cJSON_GetObjectItemCaseSensitive(item, "channels");
        error = appendAudioPiece(speech, decoded, cJSON_IsNumber(rate) && rate->valueint > 0 ? rate->valueint : 24000, cJSON_IsNumber(channels) && channels->valueint > 0 ? channels->valueint : 1);
        if (!error.empty()) {
            appendLog(std::format("Voz: pieza de audio {} ({})", error, getString(item, "mime_type")));
            break;
        }
    }
    cJSON_Delete(parsed);
    if (error.empty() && speech.samples.size() == 0) {
        error = "El servicio no devolvió audio.";
    }
    if (!error.empty()) {
        return error;
    }
    appendLog(std::format("Voz generada en {} ms, {} ms de audio a {} Hz", pdTICKS_TO_MS(xTaskGetTickCount() - start), speech.getSampleCount() * 1000 / speech.sampleRate, speech.sampleRate));
    return "";
}

// endregion

#else

std::string ask(const PromptContext&, const std::vector<Turn>&, const Pcm*, const std::string&, Reply&) {
    return "No disponible en el simulador.";
}

std::string synthesize(const Config&, const std::string&, Pcm&) {
    return "No disponible en el simulador.";
}

#endif

}
