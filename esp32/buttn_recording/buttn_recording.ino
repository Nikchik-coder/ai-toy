#include "Arduino.h"
#include "driver/i2s.h"
#include "WiFi.h"
#include <HTTPClient.h> // Use HTTPClient for easier POST requests
#include <WiFiClient.h> // Still needed for HTTPClient

// --- Button Definition ---
#define BUTTON_PIN 22 // GPIO pin for the button

// --- I2S Pin Definitions (INMP441) ---
#define I2S_SCK_PIN  18
#define I2S_WS_PIN   15
#define I2S_SD_PIN   13

// --- I2S Configuration ---
#define I2S_PORT           I2S_NUM_0
#define SAMPLE_BUFFER_SIZE 1024      // Size for reading 32-bit samples
#define I2S_SAMPLE_RATE    (16000)
#define BITS_PER_SAMPLE    (16)       // We will send 16-bit data
#define NUM_CHANNELS       (1)

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan";
const char* password = "40290678";

// --- Server Details ---
const char* serverName = "192.168.0.101"; // <<<--- REPLACE WITH YOUR FLASK SERVER IP
const int serverPort = 8888;              // <<<--- Port Flask will listen on
String serverPath = "/uploadAudio";       // Endpoint on the server

// --- Buffers ---
int32_t i2s_read_buffer32[SAMPLE_BUFFER_SIZE]; // Buffer for 32-bit I2S reads
uint8_t i2s_send_buffer16[SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Buffer for 16-bit data to send

// --- Global Objects & State ---
WiFiClient client; // Reusable client instance for HTTPClient
HTTPClient http;
bool isRecording = false; // State variable to track recording status
bool buttonWasPressed = false; // To detect button release edge

// --- Function Prototypes ---
void i2s_config_setup();
void recordAndSendChunk();

/**
 * @brief Configure I2S driver for INMP441 microphone
 */
void i2s_config_setup() {
    // Uninstall first to ensure clean config
    i2s_driver_uninstall(I2S_PORT);
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // Read 32-bit for INMP441
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Or ONLY_RIGHT depending on mic wiring
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, // Adjusted buffer count
        .dma_buf_len = 128, // Adjusted buffer length (in samples)
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };

    if (ESP_OK != i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL)) {
        Serial.println("i2s_driver_install error"); while(1);
    }
    if (ESP_OK != i2s_set_pin(I2S_PORT, &pin_config)) {
        Serial.println("i2s_set_pin error"); while(1);
    }
    Serial.println("I2S driver configured successfully.");
}

/**
 * @brief Main setup function
 */
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 Button Controlled Audio Recorder");

    // --- Configure Button Pin ---
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println("Button pin configured (GPIO " + String(BUTTON_PIN) + ")");
    // --- --- --- --- --- --- ---

    // --- Connect to WiFi ---
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi "); Serial.print(ssid);
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++connect_tries > 20) { Serial.println("\nWiFi connection failed!"); while(1); }
    }
    Serial.println("\nConnected to WiFi");
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    // --- --- --- --- --- --- ---

    // --- Configure I2S Input ---
    i2s_config_setup();
    // --- --- --- --- --- --- ---

    Serial.println("Setup complete. Press button to start recording.");
}

/**
 * @brief Main program loop
 */
void loop() {
    // --- Read Button State ---
    int buttonState = digitalRead(BUTTON_PIN);

    Serial.printf("DEBUG: Button Pin %d State = %d\n", BUTTON_PIN, buttonState);
    
    bool buttonPressed = (buttonState == LOW); // LOW means pressed with INPUT_PULLUP
    // --- --- --- --- --- ---

    // --- Handle Recording State ---
    if (buttonPressed && !isRecording) {
        // --- Button just pressed - Start Recording ---
        isRecording = true;
        buttonWasPressed = true; // Remember it was pressed
        Serial.println("Button PRESSED - Recording Started.");
        // Optional: Send a "start recording" signal to the server?
        // Discard initial buffer to avoid click noise (optional)
         size_t bytes_discarded = 0;
         i2s_read(I2S_PORT, i2s_read_buffer32, SAMPLE_BUFFER_SIZE * sizeof(int32_t), &bytes_discarded, pdMS_TO_TICKS(50));
        // --- --- --- --- --- --- --- --- --- --- --- ---

    } else if (!buttonPressed && isRecording && buttonWasPressed) {
        // --- Button just released - Stop Recording ---
        isRecording = false;
        buttonWasPressed = false; // Reset edge detection flag
        Serial.println("Button RELEASED - Recording Stopped.");
        // Optional: Send an "end recording" signal to the server?
        // --- --- --- --- --- --- --- --- --- --- --- ---

    } else if (isRecording) {
        // --- Button held down - Continue Recording ---
        recordAndSendChunk();
        // --- --- --- --- --- --- --- --- --- --- --- ---
    }
    // else {
    //    // Button not pressed, recording not active - do nothing
    // }

    delay(10); // Small delay to prevent busy-waiting and allow WiFi tasks
}

/**
 * @brief Reads one chunk of audio from I2S, converts to 16-bit, and sends via HTTP POST
 */
void recordAndSendChunk() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Cannot record/send: WiFi disconnected.");
        isRecording = false; // Stop recording if WiFi lost
        return;
    }

    size_t bytes_read = 0;
    // Read 32-bit samples from I2S
    esp_err_t result = i2s_read(I2S_PORT, i2s_read_buffer32, SAMPLE_BUFFER_SIZE * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(100)); // Add small timeout

    if (result != ESP_OK || bytes_read == 0) {
        if (result != ESP_ERR_TIMEOUT) { // Don't spam timeout errors
             Serial.printf("I2S read error! Result: %d, Bytes Read: %d\n", result, bytes_read);
        }
        return; // Don't send if no data or error
    }

    int samples_read = bytes_read / sizeof(int32_t);
    int bytes_to_send = samples_read * sizeof(int16_t);

    // Convert 32-bit data to 16-bit data (taking top 16 bits)
    for (int i = 0; i < samples_read; i++) {
        // Proper conversion for I2S 32-bit data (often left-justified)
        // Shift right by 16 to get the most significant 16 bits
        int16_t sample16 = (int16_t)(i2s_read_buffer32[i] >> 16);
        i2s_send_buffer16[2 * i]     = (uint8_t)(sample16 & 0xFF);      // LSB
        i2s_send_buffer16[2 * i + 1] = (uint8_t)((sample16 >> 8) & 0xFF); // MSB
    }

    // --- Send via HTTP POST using HTTPClient ---
    String serverUri = "http://" + String(serverName) + ":" + String(serverPort) + serverPath;

    // Begin request. Use the global 'client' and 'http' objects.
    // Pass the WiFiClient instance to http.begin()
    if (http.begin(client, serverUri)) {
        Serial.print("Sending chunk to " + serverUri + "...");

        // Add headers: Specify RAW PCM format
        http.addHeader("Content-Type", "application/octet-stream"); // Generic binary
        // OR more specific (server needs to understand this)
        // http.addHeader("Content-Type", "audio/L16; rate=16000; channels=1");

        // Send the POST request with the 16-bit audio data buffer
        int httpResponseCode = http.POST(i2s_send_buffer16, bytes_to_send);

        if (httpResponseCode > 0) {
            Serial.printf(" Done (Code: %d)\n", httpResponseCode);
            // String payload = http.getString(); // Get response if needed
            // Serial.println(payload);
        } else {
            Serial.printf(" Failed! Error: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        http.end(); // Free resources IMPORTANT!
    } else {
        Serial.println("HTTP connection failed!");
        // Consider setting isRecording = false here?
    }
    // --- --- --- --- --- --- --- --- --- --- ---
}