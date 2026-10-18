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
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <TactilityCpp/Memory.h>
#include <tactility/check.h>

/** The certificate check of the bundle, which esp_crt_bundle_attach() installs */
extern "C" int esp_crt_verify_callback(void* context, mbedtls_x509_crt* certificate, int depth, uint32_t* flags);
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

/** The Google Trust Services roots of the Gemini API: R1 (RSA), R3 and R4 (ECDSA) */
constexpr char GOOGLE_ROOT_CERTIFICATES[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G\n"
    "jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2\n"
    "4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7\n"
    "VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm\n"
    "ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X\n"
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

// region Trust

/** A certificate the server sent, as the verify callback saw it */
struct ReceivedCertificate {
    int depth;
    uint32_t flags;
    uint32_t flagsAfterBundle;
    int bundleResult;
    std::string der;
};

/** The roots above, with the certificate bundle of the firmware as a fallback */
struct Trust {
    mbedtls_x509_crt roots;
    bool parsed = false;
    /** The chain the server sent during the last handshake, from the top down */
    std::vector<ReceivedCertificate> chain;
    bool loggedWorkingChain = false;
};

static Trust& getTrust() {
    static Trust* trust = [] {
        auto* created = tt::newObject<Trust>();
        check(created != nullptr);
        mbedtls_x509_crt_init(&created->roots);
        const int result = mbedtls_x509_crt_parse(&created->roots, reinterpret_cast<const unsigned char*>(GOOGLE_ROOT_CERTIFICATES), sizeof(GOOGLE_ROOT_CERTIFICATES));
        created->parsed = result == 0;
        if (!created->parsed) {
            LOG_E(TAG, "Failed to parse the Google roots: -0x%X", -result);
        }
        return created;
    }();
    return *trust;
}

/**
 * Lets the bundle accept what the Google roots didn't, and records each certificate.
 * A rejection keeps its flags, so the handshake still fails, but the rest of the chain gets recorded too.
 */
static int verifyCertificate(void*, mbedtls_x509_crt* certificate, int depth, uint32_t* flags) {
    auto& trust = getTrust();
    const uint32_t ownFlags = *flags;
    const int result = esp_crt_verify_callback(nullptr, certificate, depth, flags);
    trust.chain.push_back({ depth, ownFlags, *flags, result, std::string(reinterpret_cast<const char*>(certificate->raw.p), certificate->raw.len) });
    return 0;
}

static esp_err_t attachTrust(void* configuration) {
    const esp_err_t result = esp_crt_bundle_attach(configuration);
    if (result != ESP_OK) {
        return result;
    }
    auto& trust = getTrust();
    auto* ssl = static_cast<mbedtls_ssl_config*>(configuration);
    if (trust.parsed) {
        mbedtls_ssl_conf_ca_chain(ssl, &trust.roots, nullptr);
    }
    mbedtls_ssl_conf_verify(ssl, verifyCertificate, nullptr);
    return ESP_OK;
}

static bool parseCertificate(const ReceivedCertificate& received, mbedtls_x509_crt& certificate) {
    mbedtls_x509_crt_init(&certificate);
    return mbedtls_x509_crt_parse_der(&certificate, reinterpret_cast<const unsigned char*>(received.der.data()), received.der.size()) == 0;
}

/**
 * Describes the recorded chain for registro.txt. Each certificate is checked against the Google roots alone,
 * and against the certificate above it, to tell a missing root apart from a signature the device can't verify.
 */
static std::string describeChain() {
    const auto& trust = getTrust();
    std::string text;
    std::string info(2048, '\0');
    for (size_t i = 0; i < trust.chain.size(); i++) {
        const auto& received = trust.chain[i];
        mbedtls_x509_crt certificate;
        if (!parseCertificate(received, certificate)) {
            text += std::format("  [{}] no se pudo leer\n", received.depth);
            mbedtls_x509_crt_free(&certificate);
            continue;
        }
        uint32_t rootFlags = 0;
        const int rootResult = trust.parsed ? mbedtls_x509_crt_verify(&certificate, const_cast<mbedtls_x509_crt*>(&trust.roots), nullptr, nullptr, &rootFlags, nullptr, nullptr) : -1;
        std::string parentCheck = "sin certificado arriba";
        mbedtls_x509_crt parent;
        if (i > 0 && parseCertificate(trust.chain[i - 1], parent)) {
            uint32_t parentFlags = 0;
            const int parentResult = mbedtls_x509_crt_verify(&certificate, &parent, nullptr, nullptr, &parentFlags, nullptr, nullptr);
            parentCheck = std::format("contra el de arriba -0x{:X} flags 0x{:X}", -parentResult, parentFlags);
            mbedtls_x509_crt_free(&parent);
        }
        const int length = mbedtls_x509_crt_info(info.data(), info.size(), "    ", &certificate);
        text += std::format("  [{}] flags 0x{:X} -> 0x{:X}, bundle -0x{:X}; contra raices Google -0x{:X} flags 0x{:X}; {}\n{}\n",
            received.depth, received.flags, received.flagsAfterBundle, -received.bundleResult, -rootResult, rootFlags, parentCheck,
            length > 0 ? info.substr(0, length) : std::format("    (sin detalle -0x{:X})", -length));
        mbedtls_x509_crt_free(&certificate);
    }
    return text;
}

// endregion

/**
 * Sends the request and reads the response headers.
 * @param[out] failure where the request stopped, when it returns nullptr
 * @return the client, ready to read the body, or nullptr
 */
static esp_http_client_handle_t startPost(const std::string& url, const std::string& apiKey, const std::vector<Part>& parts, int64_t& contentLength, std::string& failure) {
    esp_http_client_config_t config {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 60000;
    config.addr_type = HTTP_ADDR_TYPE_INET;
    config.crt_bundle_attach = attachTrust;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        failure = "init";
        return nullptr;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-goog-api-key", apiKey.c_str());

    size_t total = 0;
    for (const auto& part : parts) {
        total += part.size;
    }

    auto& trust = getTrust();
    trust.chain.clear();
    const esp_err_t openResult = esp_http_client_open(client, static_cast<int>(total));
    if (openResult == ESP_OK && !trust.loggedWorkingChain && !trust.chain.empty()) {
        trust.loggedWorkingChain = true;
        appendLog("Cadena de certificados que funcionó:\n" + describeChain());
    }
    if (openResult != ESP_OK) {
        int tlsCode = 0;
        int tlsFlags = 0;
        const esp_err_t tlsError = esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
        failure = std::format("open {}, {} -0x{:X} {:X}, ram {}k/{}k", esp_err_to_name(openResult), esp_err_to_name(tlsError), -tlsCode, tlsFlags,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
        if (!trust.chain.empty()) {
            appendLog("Cadena de certificados rechazada:\n" + describeChain());
        }
        esp_http_client_cleanup(client);
        return nullptr;
    }

    for (const auto& part : parts) {
        size_t offset = 0;
        while (offset < part.size) {
            const int chunk = std::min<size_t>(part.size - offset, 4096);
            const int result = esp_http_client_write(client, part.data + offset, chunk);
            if (result <= 0) {
                failure = std::format("write {}", result);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return nullptr;
            }
            offset += result;
        }
    }

    contentLength = esp_http_client_fetch_headers(client);
    if (contentLength < 0 && !esp_http_client_is_chunked_response(client)) {
        failure = contentLength == -ESP_ERR_HTTP_EAGAIN ? "tiempo" : std::format("headers {}", contentLength);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    return client;
}

static void endPost(esp_http_client_handle_t client) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/** Reads the whole body. @return the HTTP status, or -1 when the body didn't arrive completely */
static int readBody(esp_http_client_handle_t client, int64_t contentLength, Buffer& response, std::string& failure) {
    response.clear();
    response.reserve(contentLength > 0 ? contentLength + 1 : 65536);
    uint8_t chunk[2048];
    int count;
    while ((count = esp_http_client_read(client, reinterpret_cast<char*>(chunk), sizeof(chunk))) > 0) {
        if (!response.append(chunk, count)) {
            failure = "memoria";
            return -1;
        }
    }
    if (count < 0 || !esp_http_client_is_complete_data_received(client)) {
        failure = count == -ESP_ERR_HTTP_EAGAIN ? "tiempo" : std::format("read {}, {} of {} bytes", count, response.size(), contentLength);
        return -1;
    }
    const uint8_t terminator = 0;
    if (!response.append(&terminator, 1)) {
        failure = "memoria";
        return -1;
    }
    response.setSize(response.size() - 1);
    return esp_http_client_get_status_code(client);
}

static int postOnce(const std::string& url, const std::string& apiKey, const std::vector<Part>& parts, Buffer& response, std::string& failure) {
    int64_t contentLength = 0;
    esp_http_client_handle_t client = startPost(url, apiKey, parts, contentLength, failure);
    if (client == nullptr) {
        return -1;
    }
    const int status = readBody(client, contentLength, response, failure);
    endPost(client);
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
        if (failure == "tiempo") {
            return "Gemini se demoró demasiado en responder. Intente de nuevo en un momento.";
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

/** The thinking level that the model accepted, so a rejected one isn't sent again */
struct ThinkingMemory {
    std::string model;
    std::string configured;
    std::string accepted;
};

static ThinkingMemory& getThinkingMemory() {
    static ThinkingMemory* memory = [] {
        auto* created = tt::newObject<ThinkingMemory>();
        check(created != nullptr);
        return created;
    }();
    return *memory;
}

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
    auto& memory = getThinkingMemory();
    const bool remembered = memory.model == context.config->model && memory.configured == context.config->thinkingLevel;
    std::vector<std::string> thinkingLevels { remembered ? memory.accepted : context.config->thinkingLevel };
    for (const char* fallback : { "low", "" }) {
        if (std::find(thinkingLevels.begin(), thinkingLevels.end(), fallback) == thinkingLevels.end()) {
            thinkingLevels.push_back(fallback);
        }
    }
    const TickType_t start = xTaskGetTickCount();
    size_t attempt = 0;
    for (; attempt < thinkingLevels.size(); attempt++) {
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
    memory = { context.config->model, context.config->thinkingLevel, thinkingLevels[attempt] };

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

static cJSON* createSpeechRequest(const Config& config, const std::string& text) {
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "model", config.ttsModel.c_str());
    cJSON_AddBoolToObject(request, "store", false);
    cJSON_AddBoolToObject(request, "stream", true);
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
    // Raw 16-bit little-endian PCM, so every streamed piece can be played as it arrives
    cJSON* format = cJSON_AddObjectToObject(request, "response_format");
    cJSON_AddStringToObject(format, "type", "audio");
    cJSON_AddStringToObject(format, "mime_type", "audio/l16");
    cJSON* generation = cJSON_AddObjectToObject(request, "generation_config");
    cJSON* speechConfigs = cJSON_AddArrayToObject(generation, "speech_config");
    cJSON* speechConfig = cJSON_CreateObject();
    cJSON_AddStringToObject(speechConfig, "voice", config.voice.c_str());
    cJSON_AddItemToArray(speechConfigs, speechConfig);
    return request;
}

/** Reads the server-sent events of the speech stream and hands over the audio of each one */
class SpeechStream {
    const AudioSink& onAudio;
    Buffer line;
    Buffer decoded;

public:
    uint32_t sampleRate = 24000;
    size_t sampleCount = 0;
    TickType_t firstAudio = 0;
    bool done = false;
    bool stopped = false;
    /** A message for the user */
    std::string error;

    explicit SpeechStream(const AudioSink& onAudio) : onAudio(onAudio) {}

    bool isOpen() const { return !done && !stopped && error.empty(); }

    void feed(const uint8_t* data, size_t size) {
        size_t offset = 0;
        while (offset < size && isOpen()) {
            const auto* newline = static_cast<const uint8_t*>(memchr(data + offset, '\n', size - offset));
            const size_t end = newline != nullptr ? newline - data : size;
            if (end > offset && !line.append(data + offset, end - offset)) {
                error = "No hay memoria suficiente para la voz.";
                return;
            }
            offset = end;
            if (newline != nullptr) {
                handleLine(reinterpret_cast<const char*>(line.data()), line.size());
                line.clear();
                offset++;
            }
        }
    }

private:
    void handleLine(const char* text, size_t size) {
        if (size > 0 && text[size - 1] == '\r') {
            size--;
        }
        if (size < 5 || memcmp(text, "data:", 5) != 0) {
            return;
        }
        text += 5;
        size -= 5;
        while (size > 0 && *text == ' ') {
            text++;
            size--;
        }
        if (size == 6 && memcmp(text, "[DONE]", 6) == 0) {
            done = true;
            return;
        }
        cJSON* event = cJSON_ParseWithLength(text, size);
        if (event == nullptr) {
            return;
        }
        const cJSON* delta = cJSON_GetObjectItemCaseSensitive(event, "delta");
        const cJSON* audio = cJSON_GetObjectItemCaseSensitive(delta, "data");
        const cJSON* problem = cJSON_GetObjectItemCaseSensitive(event, "error");
        if (cJSON_IsString(audio) && audio->valuestring != nullptr) {
            handleAudio(delta, audio->valuestring);
        } else if (problem != nullptr) {
            const auto message = getString(problem, "message");
            appendLog(std::format("Voz: error del servicio durante la voz: {}", message));
            error = std::format("El servicio falló al generar la voz. ({})", message.empty() ? "sin detalle" : message);
        }
        cJSON_Delete(event);
    }

    void handleAudio(const cJSON* delta, const char* base64) {
        const cJSON* rate = cJSON_GetObjectItemCaseSensitive(delta, "sample_rate");
        if (cJSON_IsNumber(rate) && rate->valueint > 0) {
            sampleRate = rate->valueint;
        }
        const auto* source = reinterpret_cast<const uint8_t*>(base64);
        const size_t length = strlen(base64);
        size_t needed = 0;
        mbedtls_base64_decode(nullptr, 0, &needed, source, length);
        decoded.clear();
        size_t written = 0;
        if (needed == 0 || !decoded.reserve(needed) || mbedtls_base64_decode(decoded.data(), decoded.getCapacity(), &written, source, length) != 0) {
            error = "La voz llegó dañada.";
            return;
        }
        const size_t count = written / sizeof(int16_t);
        if (count == 0) {
            return;
        }
        if (firstAudio == 0) {
            firstAudio = xTaskGetTickCount();
        }
        sampleCount += count;
        if (!onAudio(reinterpret_cast<const int16_t*>(decoded.data()), count, sampleRate)) {
            stopped = true;
        }
    }
};

std::string synthesizeStreaming(const Config& config, const std::string& text, const AudioSink& onAudio) {
    if (config.apiKey.empty()) {
        return "Falta la clave de Gemini.";
    }
    cJSON* request = createSpeechRequest(config, text);
    char* json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (json == nullptr) {
        return "No hay memoria suficiente.";
    }

    const auto url = std::format("{}/interactions", API_BASE);
    const std::vector<Part> parts { { json, strlen(json) } };
    const TickType_t start = xTaskGetTickCount();
    std::string failure;
    int64_t contentLength = 0;
    int status = -1;
    esp_http_client_handle_t client = nullptr;
    // Retried once, like the other requests
    for (int attempt = 0; attempt < 2; attempt++) {
        failure.clear();
        client = startPost(url, config.apiKey, parts, contentLength, failure);
        status = client != nullptr ? esp_http_client_get_status_code(client) : -1;
        const bool retry = (client == nullptr && failure.starts_with("open")) || isTransientStatus(status);
        if (!retry || attempt == 1) {
            break;
        }
        if (client != nullptr) {
            endPost(client);
            client = nullptr;
        }
        LOG_W(TAG, "Retrying speech after %d %s", status, failure.c_str());
        appendLog(std::format("Reintento de voz tras {} {}", status, failure));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    cJSON_free(json);
    if (client == nullptr) {
        return describeHttpError("Voz", -1, Buffer(), failure);
    }
    if (status != 200) {
        Buffer response;
        readBody(client, contentLength, response, failure);
        endPost(client);
        return describeHttpError("Voz", status, response, failure);
    }

    SpeechStream stream(onAudio);
    uint8_t chunk[2048];
    while (stream.isOpen()) {
        const int count = esp_http_client_read(client, reinterpret_cast<char*>(chunk), sizeof(chunk));
        if (count < 0) {
            failure = count == -ESP_ERR_HTTP_EAGAIN ? "tiempo" : std::format("read {}", count);
            break;
        }
        if (count == 0) {
            break;
        }
        stream.feed(chunk, count);
    }
    endPost(client);

    if (stream.stopped) {
        return "";
    }
    if (!stream.error.empty()) {
        return stream.error;
    }
    if (!failure.empty()) {
        appendLog(std::format("Voz: se cortó, {}, {} ms de audio recibido", failure, stream.sampleCount * 1000 / stream.sampleRate));
        return failure == "tiempo" ? "Gemini se demoró demasiado en generar la voz." : std::format("La voz se cortó. (Detalle: {})", failure);
    }
    if (stream.sampleCount == 0) {
        appendLog("Voz: el servicio no devolvió audio");
        return "El servicio no devolvió audio.";
    }
    appendLog(std::format("Voz: primer audio en {} ms, completa en {} ms, {} ms de audio a {} Hz{}",
        pdTICKS_TO_MS(stream.firstAudio - start), pdTICKS_TO_MS(xTaskGetTickCount() - start),
        stream.sampleCount * 1000 / stream.sampleRate, stream.sampleRate, stream.done ? "" : ", sin [DONE]"));
    return "";
}

// endregion

#else

std::string ask(const PromptContext&, const std::vector<Turn>&, const Pcm*, const std::string&, Reply&) {
    return "No disponible en el simulador.";
}

std::string synthesizeStreaming(const Config&, const std::string&, const AudioSink&) {
    return "No disponible en el simulador.";
}

#endif

}
