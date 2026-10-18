#include "Assistant.h"

#include <tactility/log.h>

#include <cJSON.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <utility>

#ifdef ESP_PLATFORM
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
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

/** @return the HTTP status, or -1 when the request didn't complete */
static int post(const std::string& url, const std::string& apiKey, const std::vector<Part>& parts, Buffer& response) {
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 45000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-goog-api-key", apiKey.c_str());

    size_t total = 0;
    for (const auto& part : parts) {
        total += part.size;
    }

    int status = -1;
    if (esp_http_client_open(client, static_cast<int>(total)) == ESP_OK) {
        bool written = true;
        for (const auto& part : parts) {
            size_t offset = 0;
            while (written && offset < part.size) {
                const int chunk = std::min<size_t>(part.size - offset, 4096);
                const int result = esp_http_client_write(client, part.data + offset, chunk);
                if (result <= 0) {
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
            while ((count = esp_http_client_read(client, reinterpret_cast<char*>(chunk), sizeof(chunk))) > 0) {
                if (!response.append(chunk, count)) {
                    LOG_E(TAG, "Out of memory for the response");
                    break;
                }
            }
            const uint8_t terminator = 0;
            response.append(&terminator, 1);
            response.setSize(response.size() - 1);
            status = esp_http_client_get_status_code(client);
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return status;
}

static std::string describeHttpError(int status, const Buffer& response) {
    if (status < 0) {
        return "No me pude conectar a internet. Revise que el WiFi esté funcionando.";
    }
    std::string message;
    cJSON* json = cJSON_Parse(reinterpret_cast<const char*>(response.data()));
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(json, "error");
    const cJSON* detail = cJSON_GetObjectItemCaseSensitive(error, "message");
    if (cJSON_IsString(detail)) {
        message = detail->valuestring;
    }
    cJSON_Delete(json);
    LOG_E(TAG, "HTTP %d: %s", status, message.c_str());
    if (status == 401 || status == 403) {
        return "Hay un problema con la clave de Gemini. Revise el archivo config.json.";
    }
    if (status == 400 || status == 404) {
        return "El servicio rechazó la petición. Revise los modelos en el archivo config.json.";
    }
    if (status == 429) {
        return "Se alcanzó el límite de uso de Gemini. Intente de nuevo en un rato.";
    }
    return "El servicio no respondió bien. Intente de nuevo en un momento.";
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

static cJSON* createAskRequest(const PromptContext& context, const std::vector<Turn>& history, bool hasAudio, const std::string& text, bool withThinking) {
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
    cJSON_AddNumberToObject(generation, "temperature", 0.4);
    cJSON_AddNumberToObject(generation, "maxOutputTokens", 1024);
    if (withThinking) {
        cJSON* thinking = cJSON_AddObjectToObject(generation, "thinkingConfig");
        cJSON_AddStringToObject(thinking, "thinkingLevel", context.config->thinkingLevel.c_str());
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
    if (context.config->apiKey.empty()) {
        return "Falta la clave de Gemini. Escríbala en el archivo config.json de la tarjeta SD.";
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
    int status = -1;
    // Retried without the thinking level, for models that don't support it
    for (const bool withThinking : { !context.config->thinkingLevel.empty(), false }) {
        cJSON* request = createAskRequest(context, history, audio != nullptr, text, withThinking);
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

        status = post(url, context.config->apiKey, parts, response);
        cJSON_free(json);
        if (status != 400 || !withThinking) {
            break;
        }
        LOG_W(TAG, "Retrying without thinking level");
    }

    if (status != 200) {
        return describeHttpError(status, response);
    }

    cJSON* json = cJSON_Parse(reinterpret_cast<const char*>(response.data()));
    const auto answerText = getAnswerText(json);
    cJSON_Delete(json);
    if (!parseReply(answerText, reply)) {
        LOG_E(TAG, "Unexpected answer: %s", answerText.c_str());
        return "No entendí la respuesta del servicio. Intente de nuevo.";
    }
    return "";
}

// endregion

// region Speech

/** Finds the longest JSON string value of a "data" key, which holds the base64 audio */
static bool findAudioData(const Buffer& response, const char*& start, size_t& length) {
    const char* text = reinterpret_cast<const char*>(response.data());
    const char* end = text + response.size();
    length = 0;
    const char* cursor = text;
    while ((cursor = strstr(cursor, "\"data\"")) != nullptr && cursor < end) {
        cursor += 6;
        while (cursor < end && (*cursor == ' ' || *cursor == ':')) {
            cursor++;
        }
        if (cursor >= end || *cursor != '"') {
            continue;
        }
        const char* valueStart = ++cursor;
        while (cursor < end && *cursor != '"') {
            cursor++;
        }
        const size_t valueLength = cursor - valueStart;
        if (valueLength > length) {
            start = valueStart;
            length = valueLength;
        }
    }
    return length > 0;
}

static uint32_t readLittleEndian32(const uint8_t* bytes) {
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

/** Turns a decoded WAV file into its PCM samples, in place */
static bool unwrapWav(Pcm& speech) {
    uint8_t* bytes = speech.samples.data();
    const size_t size = speech.samples.size();
    if (size < 12 || memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
        return true;
    }
    size_t offset = 12;
    while (offset + 8 <= size) {
        const uint32_t chunkSize = readLittleEndian32(bytes + offset + 4);
        if (memcmp(bytes + offset, "fmt ", 4) == 0 && offset + 16 <= size) {
            speech.sampleRate = readLittleEndian32(bytes + offset + 12);
        } else if (memcmp(bytes + offset, "data", 4) == 0) {
            const size_t dataSize = std::min<size_t>(chunkSize, size - offset - 8);
            memmove(bytes, bytes + offset + 8, dataSize);
            speech.samples.setSize(dataSize);
            return true;
        }
        offset += 8 + chunkSize + (chunkSize & 1);
    }
    return false;
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
    Buffer response;
    const int status = post(std::format("{}/interactions", API_BASE), config.apiKey, { { json, strlen(json) } }, response);
    cJSON_free(json);
    if (status != 200) {
        return describeHttpError(status, response);
    }

    const char* data = nullptr;
    size_t length = 0;
    if (!findAudioData(response, data, length)) {
        return "El servicio no devolvió audio.";
    }
    // JSON may escape the slashes of the base64 alphabet
    char* compacted = const_cast<char*>(data);
    size_t compactedLength = 0;
    for (size_t i = 0; i < length; i++) {
        if (data[i] != '\\') {
            compacted[compactedLength++] = data[i];
        }
    }
    length = compactedLength;
    size_t decodedSize = 0;
    mbedtls_base64_decode(nullptr, 0, &decodedSize, reinterpret_cast<const uint8_t*>(data), length);
    speech.samples.clear();
    if (!speech.samples.reserve(decodedSize)) {
        return "No hay memoria suficiente para el audio.";
    }
    size_t written = 0;
    if (mbedtls_base64_decode(speech.samples.data(), speech.samples.getCapacity(), &written, reinterpret_cast<const uint8_t*>(data), length) != 0) {
        return "El audio llegó dañado.";
    }
    speech.samples.setSize(written);
    speech.sampleRate = 16000;
    if (!unwrapWav(speech)) {
        return "El audio llegó en un formato desconocido.";
    }
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
