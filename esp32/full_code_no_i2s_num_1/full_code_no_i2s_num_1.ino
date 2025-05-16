#include "Arduino.h"
#include "driver/i2s.h"
#include "WiFi.h"
#include <WebSocketsClient.h>
#include <limits.h>

// --- Button Pin Definition ---
#define BUTTON_PIN 26
// --- I2S Pin Definitions (ALL on I2S_NUM_0) ---
#define I2S_BCLK_PIN      18  // Master clock output (shared by Mic and DAC)
#define I2S_WS_PIN        15  // Master WS output (shared by Mic and DAC)
#define I2S_MIC_SD_PIN    13  // Mic Data INPUT
#define I2S_DAC_SD_PIN    25  // DAC Data OUTPUT
#define I2S_PORT          I2S_NUM_0 // Use ONLY Port 0

// --- I2S Configuration (Shared & Specific) ---
#define I2S_SAMPLE_BUFFER_SIZE 1024 // Buffer size for reading samples
#define I2S_SAMPLE_RATE    (16000)
#define I2S_BITS_PER_SAMPLE_CONFIG I2S_BITS_PER_SAMPLE_16BIT // Configure port for 16-bit (for DAC output)
#define I2S_BITS_PER_SAMPLE_ACTUAL (16)   // Actual data width we process/send/receive
#define I2S_NUM_CHANNELS   (1)     // Mono

// --- I2S Buffering ---
#define I2S_DMA_BUFFER_COUNT    8
#define I2S_DMA_BUFFER_LENGTH    256 // In samples (driver might interpret differently based on bits)
#define I2S_WRITE_TIMEOUT_MS 100

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan";
const char* password = "40290678";

// --- WebSocket Server Details ---
const char* websockets_server_host = "192.168.0.101";
const uint16_t websocket_server_port = 8765;
const char* websocket_path = "/";

// --- Buffers ---
// Buffer for reading. Size depends on how driver handles 32-bit read on 16-bit config.
// Let's try reading 16-bit samples directly now.
int16_t i2s_read_buffer16[I2S_SAMPLE_BUFFER_SIZE];
// Buffer for sending (already 16-bit)
uint8_t i2s_send_buffer_bytes[I2S_SAMPLE_BUFFER_SIZE * sizeof(int16_t)];

// --- Global Objects & State ---
WebSocketsClient webSocket;
bool wsConnected = false;
bool isSendingAudio = false;

// --- Button Debounce Variables ---
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 200;

// --- Volume Control ---
const float FIXED_VOLUME = 3.0f; // Start with 1.0f for testing

// --- Function Prototypes ---
void connectWiFi();
void i2s_full_duplex_setup(); // Renamed setup function
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendAudioChunk();
void playAudioChunk(uint8_t *payload, size_t length);
void handleButton();
void printHeapStats(const char* location);

// --- Main Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial);
    Serial.println("\n\nESP32 Full-Duplex Audio Test");
    Serial.printf("Fixed Playback Volume: %.2f\n", FIXED_VOLUME);
    printHeapStats("Start of setup");

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("Button configured on GPIO %d (INPUT_PULLUP)\n", BUTTON_PIN);

    connectWiFi();
    printHeapStats("After WiFi connect");

    // --- Setup I2S (Single Port Full-Duplex) ---
    Serial.println("Configuring I2S Port 0 for Full-Duplex (Master TX/RX)...");
    i2s_full_duplex_setup(); // Call the new setup function
    printHeapStats("After I2S setup");

    // --- Configure WebSocket Client ---
    Serial.printf("Configuring WebSocket. Connecting to ws://%s:%d%s\n", websockets_server_host, websocket_server_port, websocket_path);
    webSocket.begin(websockets_server_host, websocket_server_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    Serial.println("WebSocket client configured. Waiting for connection...");
    printHeapStats("End of setup");

    Serial.println("Setup complete. Press button to start/stop recording.");
}

// --- Main Loop ---
void loop() {
    webSocket.loop();
    handleButton();
    if (wsConnected && isSendingAudio) {
        sendAudioChunk();
    }
    delay(1);
}

// --- WiFi Connection --- (Keep your existing function)
void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", ssid);
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting...");
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++connect_tries > 30) { // ~15 second timeout
             Serial.println("\nWiFi connection failed! Check credentials/network. Halting.");
             while(1) { delay(1000); }
        }
    }
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}


/**
 * @brief Configure SINGLE I2S port (I2S_NUM_0) for Full-Duplex Master operation
 */
void i2s_full_duplex_setup() {
    // Uninstall driver first
    i2s_driver_uninstall(I2S_PORT);

    i2s_config_t i2s_config = {
        // Set mode to Master, TX, and RX
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        // Configure port for 16 bits primarily for TX. RX might adapt or need handling.
        .bits_per_sample = I2S_BITS_PER_SAMPLE_CONFIG,
        // Mono data in buffers, driver handles duplication if needed for stereo slot internally
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mic usually uses Left, DAC often uses Right. Pick one, might need swap.
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DMA_BUFFER_COUNT,
        .dma_buf_len = I2S_DMA_BUFFER_LENGTH, // Should be samples
        .use_apll = false, // Use APLL for potentially better stability if needed
        .tx_desc_auto_clear = true, // Auto clear for TX
        .fixed_mclk = 0
    };

    // Define ALL pins for the single port
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,   // GPIO 18
        .ws_io_num = I2S_WS_PIN,      // GPIO 15
        .data_out_num = I2S_DAC_SD_PIN, // GPIO 25 (DAC Data Out)
        .data_in_num = I2S_MIC_SD_PIN   // GPIO 13 (Mic Data In)
    };

    esp_err_t err;
    // Install driver
    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to install I2S driver (Port %d): %s\n", I2S_PORT, esp_err_to_name(err));
        while (true);
    }
    // Set pins
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to set I2S pins (Port %d): %s\n", I2S_PORT, esp_err_to_name(err));
        while (true);
    }
     // Start the I2S driver
    err = i2s_start(I2S_PORT);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to start I2S Port (%d): %s\n", I2S_PORT, esp_err_to_name(err));
        while(true);
    }

    // Optional: Zero DMA buffer
    // i2s_zero_dma_buffer(I2S_PORT);

    Serial.printf("I2S Port %d configured for Full-Duplex.\n", I2S_PORT);
    Serial.printf("  BCLK: %d, WS: %d, DOUT: %d, DIN: %d\n", I2S_BCLK_PIN, I2S_WS_PIN, I2S_DAC_SD_PIN, I2S_MIC_SD_PIN);
}


// --- WebSocket Event Handler --- (Keep your existing function, but check playAudioChunk call)
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WSc] Disconnected!\n");
            wsConnected = false;
            isSendingAudio = false;
            // Clear buffer on disconnect
            i2s_zero_dma_buffer(I2S_PORT); // Use I2S_PORT
            Serial.println("I2S buffer cleared on disconnect.");
            break;

        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to server: %s\n", (char*)payload);
            wsConnected = true;
            Serial.println("WebSocket connected. Ready for button press.");
            // i2s_zero_dma_buffer(I2S_PORT); // Optional clear on connect
            break;

        case WStype_TEXT:
             Serial.printf("[WSc] Received text: %s\n", payload);
             if (strstr((const char*)payload, "ERROR:") != NULL) {
                Serial.printf("!!! Server reported error: %s\n", payload);
             }
             break;

        case WStype_BIN:
            Serial.printf("[WSc] BIN received %d bytes\n", length);
            if (length > 0 && wsConnected) {
                playAudioChunk(payload, length); // Calls the function to play on I2S_PORT
            }
            break;

        case WStype_ERROR:
            Serial.printf("[WSc] Error: %s\n", payload);
            // i2s_zero_dma_buffer(I2S_PORT); // Optional clear on error
            break;

        // Other cases...
        case WStype_PING: break;
        case WStype_PONG: break;
        default: Serial.printf("[WSc] Received unknown event type: %d\n", type); break;
    }
}

// --- Handle Button --- (Keep your existing function)
// --- Button Debounce Variables ---
// Убедитесь, что debounceDelay определен где-то глобально или в начале файла
// unsigned long debounceDelay = 50; // или 100, если 50 было мало

// Статическая переменная для хранения предыдущего стабильного состояния кнопки
static int lastStableButtonState = HIGH;

void handleButton() {
    // --- Локальные статические переменные для debounce ---
    static int lastStableButtonState = HIGH; // Предыдущее стабильное состояние
    static int prevRawReading = HIGH;        // Предыдущее сырое значение для таймера
    static int stableButtonState = HIGH;     // Текущее стабильное состояние
    static unsigned long lastDebounceTime = 0; // Время последнего дребезга

    // --- Чтение пина ---
    int rawReading = digitalRead(BUTTON_PIN);
    static int lastPrintedRaw = -1; 
    if (rawReading != lastPrintedRaw) {
        Serial.printf("Raw Read GPIO %d: %d\n", BUTTON_PIN, rawReading);
        lastPrintedRaw = rawReading;
    }

    // --- Логика подавления дребезга (без изменений) ---
    if (rawReading != prevRawReading) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (rawReading != stableButtonState) {
            stableButtonState = rawReading;
        }
    }
    // --- Конец логики подавления дребезга ---


    // --- Новая Логика Действия: Срабатывание при НАЖАТИИ ---
    // Проверяем переход из HIGH в LOW
    if (stableButtonState == LOW && lastStableButtonState == HIGH) {
        Serial.printf("Button Pressed! wsConnected = %s\n", wsConnected ? "true" : "false"); // Отладочный вывод

        if (wsConnected) {
            // --- Действия при НАЖАТИИ и WebSocket ПОДКЛЮЧЕН ---

            // 1. Инвертируем состояние записи
            isSendingAudio = !isSendingAudio;

            if (isSendingAudio) {
                // 2.а Если новое состояние - ЗАПИСЬ:
                Serial.println(">>> Sending START_RECORDING <<<");
                if (!webSocket.sendTXT("START_RECORDING")) {
                    Serial.println("Send START failed!");
                    isSendingAudio = false; // Вернуть состояние обратно при ошибке
                } else {
                    // Очистка буфера перед началом
                    size_t bytes_discarded = 0;
                    i2s_read(I2S_PORT, i2s_read_buffer16, sizeof(i2s_read_buffer16), &bytes_discarded, pdMS_TO_TICKS(50));
                    Serial.println("Recording started...");
                }
            } else {
                // 2.б Если новое состояние - НЕ ЗАПИСЬ:
                Serial.println("\n>>> Sending STOP_RECORDING <<<");
                if (!webSocket.sendTXT("STOP_RECORDING")) {
                    Serial.println("Send STOP failed!");
                }
                Serial.println("Recording stopped.");
            }
         // --- Конец действий при НАЖАТИИ и WebSocket ПОДКЛЮЧЕН ---

        } else {
             // --- Действия при НАЖАТИИ, но WebSocket НЕ ПОДКЛЮЧЕН ---
            Serial.println("Button pressed, but WebSocket not connected.");
            isSendingAudio = false; // Убедимся, что запись выключена
            // --- Конец действий при НАЖАТИИ и WebSocket НЕ ПОДКЛЮЧЕН ---
        }
    } // --- Конец блока if (stableButtonState == LOW && lastStableButtonState == HIGH) ---


    // --- Обновляем переменные для следующего вызова функции (В КОНЦЕ ФУНКЦИИ!) ---
    lastStableButtonState = stableButtonState; // Запоминаем текущее стабильное для следующего раза
    prevRawReading = rawReading;               // Запоминаем текущее сырое для следующего раза
} // --- Конец функции handleButton ---
/**
 * @brief Reads audio from I2S Mic (now configured 16-bit), sends via WebSocket
 */
void sendAudioChunk() {
    size_t bytes_read = 0;
    // Read 16-bit samples directly from I2S Port 0
    // The buffer i2s_read_buffer16 is already int16_t type.
    esp_err_t result = i2s_read(I2S_PORT, i2s_read_buffer16, sizeof(i2s_read_buffer16), &bytes_read, pdMS_TO_TICKS(100));

    if (result == ESP_OK && bytes_read > 0) {
        // We read 16-bit samples, so bytes_read is the number of bytes to send.
        // We need to put them into the byte buffer for sending.
        // The INMP441 might still place data in the upper bits even when reading 16-bit,
        // or the driver might handle it. Let's assume for now read gives usable 16-bit.
        // If mic audio is bad, we might need to revert port config to 32-bit and adjust read/conversion.
        memcpy(i2s_send_buffer_bytes, i2s_read_buffer16, bytes_read);

        // Send the 16-bit binary audio chunk via WebSocket
        if (!webSocket.sendBIN(i2s_send_buffer_bytes, bytes_read)) {
            Serial.println("WebSocket sendBIN failed!");
            isSendingAudio = false;
        }

    } else if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
         Serial.printf("[I2S IN %d] Read Error: %d (%s)\n", I2S_PORT, result, esp_err_to_name(result));
    }
}

/**
 * @brief Processes and plays audio chunk via I2S DAC (now on I2S_PORT)
 */
void playAudioChunk(uint8_t *payload, size_t length) {
    if (length == 0) return;
    if (length % 2 != 0) {
        Serial.printf("[I2S OUT %d] Received odd length binary data (%d bytes), skipping.\n", I2S_PORT, length);
        return;
    }

    // --- Apply Volume Control --- (Keep your existing logic)
    int16_t* samples = (int16_t*)payload;
    size_t num_samples = length / sizeof(int16_t);
    const float vol = FIXED_VOLUME;
    if (abs(vol - 1.0f) > 0.01f) {
        for (size_t i = 0; i < num_samples; i++) {
            float scaled_sample = (float)samples[i] * vol;
            if (scaled_sample > INT16_MAX) samples[i] = INT16_MAX;
            else if (scaled_sample < INT16_MIN) samples[i] = INT16_MIN;
            else samples[i] = (int16_t)scaled_sample;
        }
    }
    // --- End Volume Control ---

    // --- Write samples to I2S DAC (using I2S_PORT now) ---
    size_t bytes_written = 0;
    TickType_t max_wait = pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS);
    // Write to the single I2S_PORT
    esp_err_t result = i2s_write(I2S_PORT, payload, length, &bytes_written, max_wait);

    if (result == ESP_OK) {
        if (bytes_written < length) {
            Serial.printf("[I2S OUT %d] Wrote only %d/%d bytes (Timeout likely)\n", I2S_PORT, bytes_written, length);
        } else {
           Serial.print("p"); // Indicate chunk played
        }
    } else if (result == ESP_ERR_TIMEOUT) {
        Serial.printf("[I2S OUT %d] Write Timeout (Buffer Full?). Dropped %d bytes.\n", I2S_PORT, length);
         i2s_zero_dma_buffer(I2S_PORT); // Clear buffer
    } else {
        Serial.printf("!!! [I2S OUT %d] Write Error: %s (%d)\n", I2S_PORT, esp_err_to_name(result), result);
    }
}

// --- Helper to print memory stats --- (Keep your existing function)
void printHeapStats(const char* location) {
  Serial.printf("[%s] Heap: %d | Min Heap: %d | PSRAM: %d | Min PSRAM: %d\n",
                location ? location : "Mem",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                ESP.getFreePsram(), ESP.getMinFreePsram());
}