#ifndef AUDIO_WEBSOCKET_HANDLER_H
#define AUDIO_WEBSOCKET_HANDLER_H

#include "Arduino.h"
#include "driver/i2s.h"
#include "WiFi.h"
#include <WebSocketsClient.h>
#include <limits.h> // For INT16_MAX/MIN

// --- Configuration Constants ---

// WiFi
#define WIFI_SSID "988664 info-lan" // <<<--- REPLACE
#define WIFI_PASSWORD "40290678"    // <<<--- REPLACE

// WebSocket
#define WS_HOST "192.168.0.101" // <<<--- REPLACE WITH YOUR PYTHON SERVER'S IP
#define WS_PORT 8765
#define WS_PATH "/"

// Button Pin (Moved from 27 to avoid conflict with DAC BCLK)
#define BUTTON_PIN 32 // <<<--- CHOOSE AN AVAILABLE GPIO

// I2S Input (Microphone - INMP441 - I2S Port 0)
#define I2S_MIC_SCK_PIN  18     // Pin Serial Clock (SCK/BCK)
#define I2S_MIC_WS_PIN   15     // Pin Word Select (WS/LRC)
#define I2S_MIC_SD_PIN   13     // Pin Serial Data (SD/DIN)
#define I2S_PORT_MIC     I2S_NUM_0

// I2S Output (DAC/Amplifier - I2S Port 1)
#define I2S_DAC_BCLK_PIN 27     // Pin Bit Clock (BCK)
#define I2S_DAC_LRC_PIN  26     // Pin Left/Right Clock (LRC/WS)
#define I2S_DAC_DOUT_PIN 25     // Pin Data Out (DOUT/DIN)
#define I2S_PORT_DAC     I2S_NUM_1 // Use the second I2S peripheral

// Audio Parameters (MUST MATCH SERVER/PROCESSING)
#define AUDIO_SAMPLE_RATE    (16000) // 16 kHz
#define AUDIO_BITS_PER_SAMPLE (16)    // 16-bit
#define AUDIO_CHANNELS       (1)     // Mono

// Buffering & Timing
#define MIC_READ_BUFFER_SIZE   1024 // Samples (int32_t) for mic read
#define MIC_SEND_BUFFER_SIZE   (MIC_READ_BUFFER_SIZE * sizeof(int16_t)) // Bytes for 16-bit send
#define DAC_WRITE_TIMEOUT_MS   20   // Max ms to wait for I2S DAC buffer space
#define MIC_READ_TIMEOUT_MS    100  // Max ms to wait for I2S Mic data
#define BUTTON_DEBOUNCE_MS     50

// Playback Volume (0.0 to 1.0 or higher, use >1.0 carefully)
#define PLAYBACK_VOLUME 1.0f

class AudioWebSocketHandler {
public:
    AudioWebSocketHandler();
    void begin(); // Main setup
    void loop();  // Main loop processing

private:
    // --- Objects ---
    WebSocketsClient webSocket;

    // --- State Variables ---
    bool wifiConnected;
    bool wsConnected;
    bool isRecording;
    int lastButtonState;
    unsigned long lastDebounceTime;
    int prevRawButtonReading;
    int stableButtonState;
    bool actionTakenOnLastRelease;

    // --- Buffers ---
    int32_t i2s_read_buffer32[MIC_READ_BUFFER_SIZE];
    uint8_t i2s_send_buffer16[MIC_SEND_BUFFER_SIZE];
    // No specific receive buffer needed if we write directly to I2S DAC

    // --- Setup Methods ---
    void setupWiFi();
    void setupI2SInput();
    void setupI2SOutput();
    void setupWebSocket();
    void setupButton();

    // --- Runtime Methods ---
    void handleButton();
    void sendAudioChunk();
    void playAudioChunk(uint8_t* payload, size_t length);

    // --- WebSocket Event Handler ---
    // Needs to be static or handled via lambda/wrapper to be passed to library
    // We use a static wrapper method
    static void webSocketEventCallback(WStype_t type, uint8_t * payload, size_t length);
    void webSocketEvent(WStype_t type, uint8_t * payload, size_t length); // The actual instance handler

    // --- Helper ---
    void printHeapStats(const char* location = nullptr);

    // Static instance pointer for the callback wrapper
    static AudioWebSocketHandler* instance;
};

#endif // AUDIO_WEBSOCKET_HANDLER_H