#include "Arduino.h"
#include "driver/i2s.h" // For I2S microphone input
#include "WiFi.h"
#include <WebSocketsClient.h> // Use WebSocket client library

// --- I2S Pin Definitions (INMP441 Microphone) ---
#define I2S_MIC_SCK_PIN  18             // Pin Serial Clock (SCK)
#define I2S_MIC_WS_PIN   15             // Pin Word Select (WS)
#define I2S_MIC_SD_PIN   13             // Pin Serial Data (SD)
#define I2S_PORT_MIC     I2S_NUM_0      // I2S port number (0) for microphone

// --- I2S Configuration ---
#define SAMPLE_BUFFER_SIZE 1024      // Size for reading 32-bit samples
#define I2S_SAMPLE_RATE    (16000)   // Frecuencia de muestreo (16 kHz)
#define I2S_BITS_PER_SAMPLE (16)       // We will send 16-bit data
#define I2S_NUM_CHANNELS   (1)       // Mono
#define RECORD_DURATION_MS (10000)   // Duration to record in milliseconds (10 seconds)

// --- WiFi Credentials ---
const char* ssid = "your_ssid"; // Your Wi-Fi network name
const char* password = "your_password";   // Your Wi-Fi network password

// --- WebSocket Server Details ---
const char* websocket_server_host = "your_server_host"; // <<<--- REPLACE WITH YOUR PYTHON SERVER'S IP ADDRESS
const uint16_t websocket_server_port = your_port;         // <<<--- Port your Python WebSocket server will listen on
const char* websocket_path = "/";                   // <<<--- Path for the WebSocket connection

// --- Buffers ---
int32_t i2s_read_buffer32[SAMPLE_BUFFER_SIZE]; // Buffer for 32-bit I2S reads from Mic
uint8_t i2s_send_buffer16[SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Buffer for 16-bit data to SEND

// --- Global Objects & State ---
WebSocketsClient webSocket;
bool wsConnected = false;
bool isSendingAudio = false;     // Are we currently sending audio?
unsigned long recordingStartTime = 0; // When did the current recording start?
bool recordingSequenceDone = false; // Flag to ensure sequence runs only once per connection

// --- Function Prototypes ---
void i2s_mic_config_setup();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendAudioChunk(); // Renamed function

/**
 * @brief Configure I2S driver for INMP441 microphone (INPUT)
 */
void i2s_mic_config_setup() {
    // You might not need to uninstall every time if config doesn't change
    // i2s_driver_uninstall(I2S_PORT_MIC);
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_MIC_SCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SD_PIN
    };

    // Install driver only if not already installed (optional check)
    if (i2s_driver_install(I2S_PORT_MIC, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("E: i2s_driver_install error"); while(1);
    }
    if (i2s_set_pin(I2S_PORT_MIC, &pin_config) != ESP_OK) {
        Serial.println("E: i2s_set_pin error"); while(1);
    }
    Serial.println("I2S Mic driver configured successfully.");
}


// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WSc] Disconnected!\n");
            wsConnected = false;
            isSendingAudio = false; // Stop sending if disconnected
            recordingSequenceDone = false; // Allow sequence to run again on reconnect
            break;
        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to url: %s\n", (char*)payload);
            wsConnected = true;
            recordingSequenceDone = false; // Reset sequence flag on new connection
            Serial.println("Waiting a moment before starting recording sequence...");
            break;
        // Handle other events like TEXT, BIN (for receiving audio later), ERROR...
        default:
            break;
    }
}

// --- Main Setup ---
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 WebSocket Auto Audio Sender");

    // --- Connect to Wi-Fi ---
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi...");
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++connect_tries > 20) { Serial.println("\nWiFi connection failed! Halting."); while(1); }
    }
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());

    // --- Setup I2S Input (Microphone) ---
    i2s_mic_config_setup();
    // --- TODO: Add setup for I2S Output (Amplifier) here later ---

    // --- Configure WebSocket Client ---
    Serial.printf("Configuring WebSocket. Connecting to %s:%d%s\n", websocket_server_host, websocket_server_port, websocket_path);
    webSocket.begin(websocket_server_host, websocket_server_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    Serial.println("WebSocket client configured. Waiting for connection...");

    Serial.println("Setup complete.");
}

// --- Main Loop ---
void loop() {
    // MUST call webSocket.loop() frequently!
    webSocket.loop();

    // --- Automatic Recording Sequence Logic ---
    if (wsConnected && !isSendingAudio && !recordingSequenceDone) {
        // Check if enough time has passed after connection to start
        // Let's wait 2 seconds after connection before starting
        static unsigned long connectTime = millis(); // Note: This gets reset if loop runs before connection
        if (!wsConnected) connectTime = millis(); // Ensure connectTime is updated on connect

        if (millis() - connectTime > 2000) {
            // --- Start Recording Sequence ---
            isSendingAudio = true;
            recordingStartTime = millis(); // Record the start time
            Serial.println(">>> Sending START_RECORDING <<<");
            webSocket.sendTXT("START_RECORDING");
            // Discard initial buffer
            size_t bytes_discarded = 0;
            i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_discarded, pdMS_TO_TICKS(50));
            Serial.println("Recording sequence started...");
        }
    }
    else if (wsConnected && isSendingAudio) {
        // --- Currently Recording/Sending ---
        unsigned long currentTime = millis();
        if (currentTime - recordingStartTime < RECORD_DURATION_MS) {
            // Duration not yet reached, send another chunk
            sendAudioChunk();
        } else {
            // --- Duration reached - Stop Recording Sequence ---
            isSendingAudio = false;
            recordingSequenceDone = true; // Mark sequence as done for this connection
            Serial.println("\n>>> Sending STOP_RECORDING <<<");
            webSocket.sendTXT("STOP_RECORDING");
            Serial.println("Recording sequence finished.");
        }
    }

    // Keep loop delay small
    delay(5);
}


/**
 * @brief Reads one chunk of audio from I2S Mic, converts, and sends via WebSocket
 */
void sendAudioChunk() {
    size_t bytes_read = 0;
    // Read 32-bit samples from I2S Mic
    esp_err_t result = i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_read, pdMS_TO_TICKS(100));

    if (result == ESP_OK && bytes_read > 0) {
        int samples_read = bytes_read / sizeof(int32_t);
        // Ensure we don't exceed the send buffer size if bytes_read was partial
        if (samples_read > SAMPLE_BUFFER_SIZE) samples_read = SAMPLE_BUFFER_SIZE;
        int bytes_to_send = samples_read * sizeof(int16_t);

        // Convert 32-bit data to 16-bit data
        for (int i = 0; i < samples_read; i++) {
            int16_t sample16 = (int16_t)(i2s_read_buffer32[i] >> 16);
            i2s_send_buffer16[2 * i]     = (uint8_t)(sample16 & 0xFF);
            i2s_send_buffer16[2 * i + 1] = (uint8_t)((sample16 >> 8) & 0xFF);
        }

        // Send the 16-bit binary audio chunk via WebSocket
        if (!webSocket.sendBIN(i2s_send_buffer16, bytes_to_send)) {
            Serial.println("WebSocket sendBIN failed!");
            // Consider stopping on send failure
            isSendingAudio = false;
            recordingSequenceDone = true; // Prevent trying again immediately
            webSocket.sendTXT("STOP_RECORDING_ERROR");
        } else {
           // Serial.print("."); // Indicate chunk sent - can be verbose
        }

    } else if (result != ESP_ERR_TIMEOUT) {
         Serial.printf("[I2S IN] Read Error: %d\n", result);
         // Consider stopping on read error
         isSendingAudio = false;
         recordingSequenceDone = true;
         webSocket.sendTXT("STOP_RECORDING_ERROR");
    }
}