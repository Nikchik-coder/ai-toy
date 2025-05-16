#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h> // Using ArduinoWebsockets library
#include "driver/i2s.h"
#include <limits.h> // Required for INT16_MAX, INT16_MIN

// --- Pin Definitions ---
// Make sure these match your hardware connections
#define I2S_DOUT      GPIO_NUM_25  // Data Out (DIN) to your DAC/Amplifier
#define I2S_BCLK      GPIO_NUM_27  // Bit Clock (BCLK/SCK)
#define I2S_LRC       GPIO_NUM_26  // Left/Right Clock (LRCK/WS)

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan";         // <<<--- REPLACE
const char* password = "40290678"; // <<<--- REPLACE

// --- WebSocket Server Details ---
const char* websockets_server_host = "192.168.0.101"; // <<<--- REPLACE WITH YOUR PC's IP ADDRESS!
const uint16_t websockets_server_port = 8765;
const char* websockets_server_path = "/"; // Should match server

// --- I2S Configuration (MUST match the RAW PCM data format from the server) ---
#define SAMPLE_RATE         (16000) // Sample rate in Hz (e.g., 16kHz)
#define BITS_PER_SAMPLE     (16)    // Bits per sample (e.g., 16-bit)
#define NUM_CHANNELS        (1)     // 1 for mono, 2 for stereo

// --- I2S Configuration ---
#define I2S_PORT            (I2S_NUM_0) // Use I2S Port 0
#define I2S_BUFFER_COUNT    4          // Number of DMA buffers
#define I2S_BUFFER_LENGTH   256        // Size of each DMA buffer in bytes
#define I2S_WRITE_TIMEOUT_MS 20         // Max time (ms) to wait for I2S buffer space before dropping data

// --- WebSocket Client Object ---
WebSocketsClient webSocket;
bool isWebSocketConnected = false; // Track connection state

// +++ Volume Control +++
// <<<--- SET YOUR DESIRED VOLUME HERE (e.g., 0.7 for 70%) --->>>
const float FIXED_VOLUME = 1.5f; // Set desired volume level (0.0 to 1.0 or higher, careful with >1.0)
// +++++++++++++++++++++++

// --- Function Prototypes ---
void connectWiFi();
void configureI2S();
void connectWebSocket();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void printHeapStats(const char* location);
// void handleSerialCommands(); // Removed

void setup() {
    Serial.begin(115200);
    while (!Serial); // Wait for Serial Monitor to open (optional)
    Serial.println("\n\nStarting WebSocket Audio Receiver...");
    Serial.printf("Fixed volume set to: %.2f\n", FIXED_VOLUME); // Report the fixed volume
    printHeapStats("Start of setup");

    connectWiFi();
    printHeapStats("After WiFi connect");

    Serial.println("Configuring I2S...");
    configureI2S();
    Serial.println("I2S Configured.");
    printHeapStats("After I2S config");

    connectWebSocket(); // Initial connection attempt
    printHeapStats("End of setup");
}

void loop() {
    webSocket.loop(); // MUST call frequently to process WebSocket events
    // handleSerialCommands(); // Removed call

    // Optional: Simple reconnect logic in the loop
    static unsigned long lastReconnectAttempt = 0;
    if (!isWebSocketConnected && WiFi.status() == WL_CONNECTED && millis() - lastReconnectAttempt > 5000) {
        Serial.println("Attempting WebSocket reconnect...");
        connectWebSocket(); // Try to connect again
        lastReconnectAttempt = millis();
    }

     // delay(1); // Usually not needed
}

// --- WiFi Connection Function ---
void connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", ssid);
    WiFi.disconnect(true); // Disconnect previous session
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) { // Timeout after ~15s
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection FAILED. Please check credentials and network.");
        // while(1) { delay(1000); }
    }
}


// --- I2S Configuration Function ---
void configureI2S() {
    // Uninstall first to ensure clean state
    // i2s_driver_uninstall(I2S_PORT);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        // .channel_format = (NUM_CHANNELS == 1) ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ALL_RIGHT, // <<<--- OLD WAY ---<<<
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // <<<--- NEW WAY: Tell driver the buffer has MONO data (use Right or Left channel) ---<<<
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_BUFFER_COUNT,    // Keep buffer count reasonable (e.g., 8 or 16)
        .dma_buf_len = I2S_BUFFER_LENGTH,      // Keep buffer length reasonable (e.g., 512 or 1024)
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err;
    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to install I2S driver: %s\n", esp_err_to_name(err));
        while (true);
    }
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to set I2S pins: %s\n", esp_err_to_name(err));
        while (true);
    }
    err = i2s_zero_dma_buffer(I2S_PORT);
     if (err != ESP_OK) {
        Serial.printf("!!! Failed to zero I2S DMA buffer: %s\n", esp_err_to_name(err));
    }
}

// --- WebSocket Connection Function ---
void connectWebSocket() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Cannot connect WebSocket, WiFi not connected.");
        return;
    }
    if (webSocket.isConnected()) {
        Serial.println("WebSocket already connected.");
        isWebSocketConnected = true;
        return;
    }

    Serial.printf("Connecting to WebSocket server: ws://%s:%d%s\n", websockets_server_host, websockets_server_port, websockets_server_path);
    webSocket.begin(websockets_server_host, websockets_server_port, websockets_server_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}


// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            isWebSocketConnected = false;
            Serial.printf("[WSc] Disconnected!\n");
            i2s_zero_dma_buffer(I2S_PORT);
            Serial.println("I2S buffer cleared on disconnect.");
            break;

        case WStype_CONNECTED:
            isWebSocketConnected = true;
            Serial.printf("[WSc] Connected to url: %s\n", payload);
            break;

        case WStype_TEXT:
            Serial.printf("[WSc] Received text: %s\n", payload);
            if (strstr((const char*)payload, "ERROR:") != NULL) {
                Serial.printf("!!! Server reported error: %s\n", payload);
            }
            break;

        case WStype_BIN:
            if (length > 0 && isWebSocketConnected) {

                if (length % 2 != 0) {
                    Serial.printf("[WSc] Received binary data with odd length (%d bytes), skipping.\n", length);
                    break;
                }

                // --- Apply Fixed Volume Control ---
                int16_t* samples = (int16_t*)payload;
                size_t num_samples = length / 2;
                const float vol = FIXED_VOLUME; // Use the fixed volume constant

                for (size_t i = 0; i < num_samples; i++) {
                    float scaled_sample = (float)samples[i] * vol;

                    // Clamp the value
                    if (scaled_sample > INT16_MAX) {
                        samples[i] = INT16_MAX;
                    } else if (scaled_sample < INT16_MIN) {
                        samples[i] = INT16_MIN;
                    } else {
                        samples[i] = (int16_t)scaled_sample;
                    }
                }
                // --- End Volume Control ---


                // --- Write MODIFIED samples to I2S ---
                size_t bytes_written = 0;
                TickType_t max_wait = pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS);
                esp_err_t result = i2s_write(I2S_PORT, payload, length, &bytes_written, max_wait);

                if (result == ESP_ERR_TIMEOUT) {
                    Serial.printf("I2S Write Timeout (Buffer Full). Dropped %d bytes.\n", length);
                    // i2s_zero_dma_buffer(I2S_PORT); // Optional
                } else if (result != ESP_OK) {
                    Serial.printf("!!! I2S Write Error: %s (%d)\n", esp_err_to_name(result), result);
                }
            }
            break;

        case WStype_ERROR:
            Serial.printf("[WSc] Error: %s\n", payload);
            break;

        case WStype_PING: break;
        case WStype_PONG: break;
        case WStype_FRAGMENT_TEXT_START: break;
        case WStype_FRAGMENT_BIN_START: break;
        case WStype_FRAGMENT: break;
        case WStype_FRAGMENT_FIN: break;

        default:
             Serial.printf("[WSc] Unknown Event Type: %d\n", type);
             break;
    }
}

// --- Handle Serial Commands (REMOVED) ---
// void handleSerialCommands() { ... }


// --- Helper to print memory stats ---
void printHeapStats(const char* location) {
  Serial.printf("[%s] Heap: %d | Min Heap: %d | PSRAM: %d | Min PSRAM: %d\n",
                location ? location : "Mem",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                ESP.getFreePsram(), ESP.getMinFreePsram());
}