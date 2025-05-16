#include "Arduino.h"
#include "driver/i2s.h"        // For I2S microphone input
#include "WiFi.h"
#include <WebSocketsClient.h>  // Use WebSocket client library

// --- Button Pin Definition ---
#define BUTTON_PIN 27         // GPIO pin connected to the button (other leg to GND)

// --- I2S Pin Definitions (INMP441 Microphone) ---
#define I2S_MIC_SCK_PIN  18     // Pin Serial Clock (SCK)
#define I2S_MIC_WS_PIN   15     // Pin Word Select (WS)
#define I2S_MIC_SD_PIN   13     // Pin Serial Data (SD)
#define I2S_PORT_MIC     I2S_NUM_0 // I2S port number (0) for microphone

// --- I2S Configuration ---
#define SAMPLE_BUFFER_SIZE 1024 // Size for reading 32-bit samples
#define I2S_SAMPLE_RATE    (16000) // Sampling frequency (16 kHz)
#define I2S_BITS_PER_SAMPLE (16) // We will send 16-bit data
#define I2S_NUM_CHANNELS   (1)  // Mono

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan"; // Your Wi-Fi network name
const char* password = "40290678";    // Your Wi-Fi network password

// --- WebSocket Server Details ---
const char* websocket_server_host = "192.168.0.101"; // <<<--- REPLACE WITH YOUR PYTHON SERVER'S IP ADDRESS
const uint16_t websocket_server_port = 8765;         // <<<--- Port your Python WebSocket server will listen on
const char* websocket_path = "/";                   // <<<--- Path for the WebSocket connection

// --- Buffers ---
int32_t i2s_read_buffer32[SAMPLE_BUFFER_SIZE]; // Buffer for 32-bit I2S reads from Mic
uint8_t i2s_send_buffer16[SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Buffer for 16-bit data to SEND

// --- Global Objects & State ---
WebSocketsClient webSocket;
bool wsConnected = false;
bool isSendingAudio = false;     // Are we currently sending audio?

// --- Button Debounce Variables ---
int lastButtonState = HIGH;      // Previous stable state of the button
unsigned long lastDebounceTime = 0; // Last time the output pin was toggled
unsigned long debounceDelay = 50;   // Debounce time in milliseconds

// --- Function Prototypes ---
void i2s_mic_config_setup();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendAudioChunk();
void handleButton(); // Function to check button state

/**
 * @brief Configure I2S driver for INMP441 microphone (INPUT)
 */
void i2s_mic_config_setup() {
    // (I2S setup remains the same)
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
            // Reset button state on disconnect? Optional, maybe not needed.
            // lastButtonState = HIGH;
            break;
        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to url: %s\n", (char*)payload);
            wsConnected = true;
            Serial.println("WebSocket connected. Ready for button press.");
            break;
        // Handle other events like TEXT, BIN (for receiving audio later), ERROR...
        case WStype_TEXT:
             Serial.printf("[WSc] Received text: %s\n", payload);
             // Could add logic here if server sends commands back
             break;
        case WStype_ERROR:
        case WStype_BIN: // Ignore binary data received from server for now
        default:
            break;
    }
}

// --- Main Setup ---
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 WebSocket Button Audio Sender");

    // --- Setup Button Pin ---
    pinMode(BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up resistor
    Serial.printf("Button configured on GPIO %d (INPUT_PULLUP)\n", BUTTON_PIN);

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

    // --- Configure WebSocket Client ---
    Serial.printf("Configuring WebSocket. Connecting to %s:%d%s\n", websocket_server_host, websocket_server_port, websocket_path);
    webSocket.begin(websocket_server_host, websocket_server_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000); // Try to reconnect every 5 seconds
    Serial.println("WebSocket client configured. Waiting for connection...");

    Serial.println("Setup complete. Press button to start/stop recording.");
}

// --- Main Loop ---
void loop() {
    // MUST call webSocket.loop() frequently!
    webSocket.loop();

    // Check the button state
    handleButton();

    // Send audio data if currently recording and connected
    if (wsConnected && isSendingAudio) {
        sendAudioChunk();
    }

    // Keep loop reasonably responsive - small delay is fine
    delay(5); // Reduced delay to make button more responsive
}

/**
 * @brief Check button state with debouncing and toggle recording
 */
void handleButton() {
    // Use the robust debounce logic pattern from before
    int rawReading = digitalRead(BUTTON_PIN);
    static int prevRawReading = HIGH;    // Store previous raw reading
    static int stableButtonState = HIGH; // Store last confirmed stable state
    static unsigned long lastDebounceTime = 0; // Last time input changed
    static bool actionTakenOnLastRelease = false; // Flag to prevent double actions

    // If the switch changed, due to noise or pressing: reset the debounce timer
    if (rawReading != prevRawReading) {
        lastDebounceTime = millis();
    }

    // Check if enough time has passed since the last raw change
    if ((millis() - lastDebounceTime) > debounceDelay) {
        // If the current reading is different from the last stable state, update stable state
        if (rawReading != stableButtonState) {
            stableButtonState = rawReading; // Update to the new stable state
            actionTakenOnLastRelease = false; // Reset flag when state changes stably
            Serial.printf("Button Stable State Changed To: %s\n", stableButtonState == HIGH ? "HIGH (Released)" : "LOW (Pressed)");
        }
    }

    // *** Action Logic: Trigger ONLY once when stable state becomes HIGH ***
    // Check if the stable state is HIGH (released) AND we haven't already acted on this specific release
    if (stableButtonState == HIGH && !actionTakenOnLastRelease) {
        actionTakenOnLastRelease = true; // Mark that we are acting on this release NOW

        Serial.println("Button Released (Debounced - Action Trigger)");

        if (wsConnected) {
            // Toggle the recording state cleanly
            if (!isSendingAudio) {
                // --- Start Recording ---
                isSendingAudio = true; // SET STATE FIRST
                Serial.println(">>> Sending START_RECORDING <<<");
                if (!webSocket.sendTXT("START_RECORDING")) Serial.println("Send START failed!");
                // Discard the initial buffer
                size_t bytes_discarded = 0;
                i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_discarded, pdMS_TO_TICKS(50));
                Serial.println("Recording started...");
            } else {
                // --- Stop Recording ---
                isSendingAudio = false; // SET STATE FIRST
                Serial.println("\n>>> Sending STOP_RECORDING <<<");
                if (!webSocket.sendTXT("STOP_RECORDING")) Serial.println("Send STOP failed!");
                Serial.println("Recording stopped.");
            }
        } else {
            Serial.println("Button released, but WebSocket not connected.");
        }
    }

    // Update the previous raw reading for the next loop iteration
    prevRawReading = rawReading;
}


/**
 * @brief Reads one chunk of audio from I2S Mic, converts, and sends via WebSocket
 */
void sendAudioChunk() {
    size_t bytes_read = 0;
    // Read 32-bit samples from I2S Mic
    esp_err_t result = i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_read, pdMS_TO_TICKS(100)); // Potential blocking point

    if (result == ESP_OK && bytes_read > 0) {
        int samples_read = bytes_read / sizeof(int32_t);
        if (samples_read > SAMPLE_BUFFER_SIZE) samples_read = SAMPLE_BUFFER_SIZE; // Should not happen with correct buffer sizes
        int bytes_to_send = samples_read * sizeof(int16_t);

        // Convert 32-bit data (only top 16 bits used by INMP441) to 16-bit data
        for (int i = 0; i < samples_read; i++) {
            // Shift right by 16 to get the significant high bits for INMP441
            int16_t sample16 = (int16_t)(i2s_read_buffer32[i] >> 16);
            // Place into buffer as little-endian
            i2s_send_buffer16[2 * i]     = (uint8_t)(sample16 & 0xFF);
            i2s_send_buffer16[2 * i + 1] = (uint8_t)((sample16 >> 8) & 0xFF);
        }

        // Send the 16-bit binary audio chunk via WebSocket
        if (!webSocket.sendBIN(i2s_send_buffer16, bytes_to_send)) {
            Serial.println("WebSocket sendBIN failed!");
            // Stop sending on failure
            isSendingAudio = false;
            // Maybe send an error message if possible, but sendBIN already failed...
            // webSocket.sendTXT("STOP_RECORDING_ERROR"); // This might also fail
        } else {
           // Serial.print("."); // Indicate chunk sent - can be verbose
        }

    } else if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
         // Log actual errors, ignore timeouts if they are expected sometimes
         Serial.printf("[I2S IN] Read Error: %d (%s)\n", result, esp_err_to_name(result));
         // Consider stopping on persistent read errors
         // isSendingAudio = false;
         // if(wsConnected) webSocket.sendTXT("STOP_RECORDING_ERROR");
    }
    // Note: ESP_ERR_TIMEOUT might happen if loop is delayed elsewhere or buffer isn't filling fast enough.
}