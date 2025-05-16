#include "AudioWebSocketHandler.h"

// Define the static instance pointer
AudioWebSocketHandler* AudioWebSocketHandler::instance = nullptr;

// --- Constructor ---
AudioWebSocketHandler::AudioWebSocketHandler() :
    wifiConnected(false),
    wsConnected(false),
    isRecording(false),
    lastButtonState(HIGH),
    lastDebounceTime(0),
    prevRawButtonReading(HIGH),
    stableButtonState(HIGH),
    actionTakenOnLastRelease(false)
{
    instance = this; // Store the instance pointer for the static callback
}

// --- Main Setup ---
void AudioWebSocketHandler::begin() {
    Serial.begin(115200);
    while (!Serial); // Optional: wait for serial monitor
    Serial.println("\n--- ESP32 Audio WebSocket Handler ---");
    printHeapStats("Start of begin()");

    setupButton();
    setupWiFi();        // Connect to WiFi first
    setupI2SInput();    // Setup microphone input
    setupI2SOutput();   // Setup DAC output
    setupWebSocket();   // Setup WebSocket connection

    printHeapStats("End of begin()");
    Serial.println("Setup complete. Waiting for WebSocket connection...");
    Serial.println("Press button to start/stop recording.");
}

// --- Main Loop ---
void AudioWebSocketHandler::loop() {
    webSocket.loop(); // Essential for WebSocket processing
    handleButton();   // Check button state

    if (wsConnected && isRecording) {
        sendAudioChunk(); // Send mic data if recording
    }

    // A small delay can prevent watchdog timeouts if loops are very tight,
    // but keep it minimal to ensure responsiveness.
    // delay(1); // Usually not needed if tasks are non-blocking
}

// --- Setup Methods ---

void AudioWebSocketHandler::setupWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++connect_tries > 30) { // ~15 seconds timeout
             Serial.println("\nWiFi connection FAILED! Halting.");
             // Consider alternative behavior like retrying later instead of halting
             while(1) { delay(1000); }
        }
    }
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
    printHeapStats("WiFi Connected");
}

void AudioWebSocketHandler::setupI2SInput() {
    Serial.println("Configuring I2S Input (Microphone)...");
    const i2s_config_t i2s_mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // Read 32 bits for INMP441 compatibility
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Or I2S_CHANNEL_FMT_ONLY_RIGHT depending on wiring
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 128, // Keep reasonably small for lower latency
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t mic_pin_config = {
        .bck_io_num = I2S_MIC_SCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SD_PIN
    };

    esp_err_t err;
    err = i2s_driver_install(I2S_PORT_MIC, &i2s_mic_config, 0, NULL);
    if (err != ESP_OK) { Serial.printf("E: I2S Mic driver_install error: %s\n", esp_err_to_name(err)); while(1); }
    err = i2s_set_pin(I2S_PORT_MIC, &mic_pin_config);
    if (err != ESP_OK) { Serial.printf("E: I2S Mic set_pin error: %s\n", esp_err_to_name(err)); while(1); }

    Serial.println("I2S Input (Mic) driver configured successfully.");
    printHeapStats("I2S Input Configured");
}

void AudioWebSocketHandler::setupI2SOutput() {
    Serial.println("Configuring I2S Output (DAC/Speaker)...");
    const i2s_config_t i2s_dac_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // Output 16-bit audio
        .channel_format = (AUDIO_CHANNELS == 1) ? I2S_CHANNEL_FMT_ONLY_RIGHT : I2S_CHANNEL_FMT_RIGHT_LEFT, // Mono -> Right channel usually
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, // More buffers can help with smoother playback
        .dma_buf_len = 256, // Larger buffer for output
        .use_apll = false,
        .tx_desc_auto_clear = true, // Auto clear descriptors is typical for TX
        .fixed_mclk = 0
    };
    const i2s_pin_config_t dac_pin_config = {
        .bck_io_num = I2S_DAC_BCLK_PIN,
        .ws_io_num = I2S_DAC_LRC_PIN,
        .data_out_num = I2S_DAC_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE // Not using input on this port
    };

    esp_err_t err;
    // Uninstall driver first if it might have been used before
    // i2s_driver_uninstall(I2S_PORT_DAC);
    err = i2s_driver_install(I2S_PORT_DAC, &i2s_dac_config, 0, NULL);
    if (err != ESP_OK) { Serial.printf("E: I2S DAC driver_install error: %s\n", esp_err_to_name(err)); while(1); }
    err = i2s_set_pin(I2S_PORT_DAC, &dac_pin_config);
    if (err != ESP_OK) { Serial.printf("E: I2S DAC set_pin error: %s\n", esp_err_to_name(err)); while(1); }
    err = i2s_zero_dma_buffer(I2S_PORT_DAC); // Clear DAC buffer initially
    if (err != ESP_OK) { Serial.printf("W: Failed to zero I2S DAC buffer: %s\n", esp_err_to_name(err)); }

    Serial.println("I2S Output (DAC) driver configured successfully.");
    Serial.printf("Playback volume set to: %.2f\n", PLAYBACK_VOLUME);
    printHeapStats("I2S Output Configured");
}

void AudioWebSocketHandler::setupWebSocket() {
    Serial.printf("Configuring WebSocket. Connecting to ws://%s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
    webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
    // Use the static wrapper function as the callback
    webSocket.onEvent(AudioWebSocketHandler::webSocketEventCallback);
    webSocket.setReconnectInterval(5000); // Try to reconnect every 5 seconds
    // webSocket.enableHeartbeat(15000, 3000, 2); // Optional: Keep connection alive
    Serial.println("WebSocket client configured.");
}

void AudioWebSocketHandler::setupButton() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("Button configured on GPIO %d (INPUT_PULLUP)\n", BUTTON_PIN);
    lastButtonState = digitalRead(BUTTON_PIN); // Initialize state
    prevRawButtonReading = lastButtonState;
    stableButtonState = lastButtonState;
}


// --- Runtime Methods ---

void AudioWebSocketHandler::handleButton() {
    int rawReading = digitalRead(BUTTON_PIN);

    // Debounce Logic (Same as original button script)
    if (rawReading != prevRawButtonReading) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > BUTTON_DEBOUNCE_MS) {
        if (rawReading != stableButtonState) {
            stableButtonState = rawReading;
            actionTakenOnLastRelease = false; // Reset flag when state changes stably
            // Serial.printf("Button Stable State Changed To: %s\n", stableButtonState == HIGH ? "HIGH (Released)" : "LOW (Pressed)");
        }
    }

    // Action Logic: Trigger ONLY once when stable state transitions to HIGH (released)
    if (stableButtonState == HIGH && !actionTakenOnLastRelease) {
        actionTakenOnLastRelease = true; // Act on this release

        Serial.println("Button Released (Debounced - Action Trigger)");

        if (wsConnected) {
            isRecording = !isRecording; // Toggle recording state

            if (isRecording) {
                // --- Start Recording ---
                Serial.println(">>> Sending START_RECORDING <<<");
                if (!webSocket.sendTXT("START_RECORDING")) Serial.println("Send START failed!");
                // Optionally discard initial buffer silence/noise
                 size_t bytes_discarded = 0;
                 i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_discarded, pdMS_TO_TICKS(50));
                Serial.println("Recording started...");
            } else {
                // --- Stop Recording ---
                Serial.println("\n>>> Sending STOP_RECORDING <<<");
                if (!webSocket.sendTXT("STOP_RECORDING")) Serial.println("Send STOP failed!");
                Serial.println("Recording stopped.");
            }
        } else {
            Serial.println("Button released, but WebSocket not connected.");
            isRecording = false; // Ensure recording is off if not connected
        }
    }
    prevRawButtonReading = rawReading; // Update previous reading for next loop
}


void AudioWebSocketHandler::sendAudioChunk() {
    size_t bytes_read = 0;
    // Read 32-bit samples from I2S Mic
    esp_err_t result = i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_read, pdMS_TO_TICKS(MIC_READ_TIMEOUT_MS));

    if (result == ESP_OK && bytes_read > 0) {
        int samples_read = bytes_read / sizeof(int32_t);
        // Ensure we don't exceed buffer limits (shouldn't happen with correct setup)
        if (samples_read > MIC_READ_BUFFER_SIZE) samples_read = MIC_READ_BUFFER_SIZE;

        int bytes_to_send = samples_read * sizeof(int16_t);

        // Convert 32-bit data (INMP441 uses top 16 bits) to 16-bit PCM Little Endian
        for (int i = 0; i < samples_read; i++) {
            // Shift right by 16 bits to get the significant high bits
            // You might need to adjust the shift (e.g., 14 or 15) depending on the actual
            // bit depth and justification of your microphone if it's not exactly 16 MSB.
            // For INMP441, right shift by 16 is common practice.
            int16_t sample16 = (int16_t)(i2s_read_buffer32[i] >> 16);

            // Place into buffer as little-endian bytes
            i2s_send_buffer16[2 * i]     = (uint8_t)(sample16 & 0xFF);
            i2s_send_buffer16[2 * i + 1] = (uint8_t)((sample16 >> 8) & 0xFF);
        }

        // Send the 16-bit binary audio chunk via WebSocket
        if (!webSocket.sendBIN(i2s_send_buffer16, bytes_to_send)) {
            Serial.println("W: WebSocket sendBIN failed!");
            // Consider stopping recording on send failure
            // isRecording = false;
            // webSocket.sendTXT("STOP_RECORDING_ERROR"); // This might also fail
        } else {
           // Serial.print("."); // Verbose: Indicate chunk sent
        }

    } else if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
         Serial.printf("E: I2S Mic Read Error: %d (%s)\n", result, esp_err_to_name(result));
         // Consider stopping on persistent read errors
         // isRecording = false;
         // if(wsConnected) webSocket.sendTXT("STOP_RECORDING_ERROR");
    }
    // Note: ESP_ERR_TIMEOUT might happen if loop is delayed or mic isn't producing data fast enough.
}


void AudioWebSocketHandler::playAudioChunk(uint8_t* payload, size_t length) {
    if (!payload || length == 0) {
        //Serial.println("W: Received empty audio chunk.");
        return;
    }
    // Ensure length is even for 16-bit samples
    if (length % 2 != 0) {
        Serial.printf("W: Received audio chunk with odd length (%d bytes). Truncating.\n", length);
        length--; // Ignore the last byte
        if (length == 0) return;
    }

    // --- Optional: Apply Volume Control ---
    int16_t* samples = (int16_t*)payload;
    size_t num_samples = length / sizeof(int16_t);
    const float vol = PLAYBACK_VOLUME; // Use the defined volume

    if (vol != 1.0f) { // Apply volume only if it's not 1.0
        for (size_t i = 0; i < num_samples; i++) {
            // Apply volume as float calculation
            float scaled_sample = (float)samples[i] * vol;

            // Clamp the value to prevent overflow/wrap-around
            if (scaled_sample > INT16_MAX) {
                samples[i] = INT16_MAX;
            } else if (scaled_sample < INT16_MIN) {
                samples[i] = INT16_MIN;
            } else {
                samples[i] = (int16_t)scaled_sample;
            }
        }
    }
    // --- End Volume Control ---


    // --- Write samples to I2S DAC ---
    size_t bytes_written = 0;
    TickType_t max_wait = pdMS_TO_TICKS(DAC_WRITE_TIMEOUT_MS);
    esp_err_t result = i2s_write(I2S_PORT_DAC, payload, length, &bytes_written, max_wait);

    if (result == ESP_ERR_TIMEOUT) {
        Serial.printf("W: I2S DAC Write Timeout (Buffer Full). Dropped %d bytes.\n", length - bytes_written);
        // Optionally clear buffer if it seems stuck:
        // i2s_zero_dma_buffer(I2S_PORT_DAC);
    } else if (result != ESP_OK) {
        Serial.printf("E: I2S DAC Write Error: %s (%d)\n", esp_err_to_name(result), result);
    } else if (bytes_written < length) {
         Serial.printf("W: I2S DAC Write partial (%d/%d bytes written).\n", bytes_written, length);
         // This might indicate the buffer was full for the remaining part
    }
    // else { Serial.print("p"); } // Verbose: Indicate chunk played
}


// --- Static WebSocket Event Callback Wrapper ---
void AudioWebSocketHandler::webSocketEventCallback(WStype_t type, uint8_t * payload, size_t length) {
    // Call the instance method using the stored pointer
    if (instance) {
        instance->webSocketEvent(type, payload, length);
    }
}

// --- Instance WebSocket Event Handler ---
void AudioWebSocketHandler::webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.println("[WSc] Disconnected!");
            wsConnected = false;
            isRecording = false; // Stop recording if disconnected
            i2s_zero_dma_buffer(I2S_PORT_DAC); // Clear DAC buffer on disconnect
            Serial.println("I2S DAC buffer cleared.");
            break;

        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to url: %s\n", payload);
            wsConnected = true;
            // Optionally send a confirmation message to server upon connection
            // webSocket.sendTXT("ESP32_CONNECTED");
            break;

        case WStype_TEXT:
            Serial.printf("[WSc] Received text: %s\n", payload);
            // Add any specific text command handling here if needed
            if (strstr((const char*)payload, "ERROR:") != NULL) {
                Serial.printf("!!! Server reported error: %s\n", payload);
            }
            break;

        case WStype_BIN:
            // Serial.printf("[WSc] Received binary data: %d bytes\n", length);
            // Assume binary data is audio for playback
            playAudioChunk(payload, length);
            break;

        case WStype_ERROR:
            Serial.printf("[WSc] Error: %s\n", payload);
            break;

        case WStype_PING:
            // Serial.println("[WSc] Received ping");
            break;
        case WStype_PONG:
            // Serial.println("[WSc] Received pong");
            break;

        // Handle fragments if your server sends large messages in chunks
        // case WStype_FRAGMENT_TEXT_START:
        // case WStype_FRAGMENT_BIN_START:
        // case WStype_FRAGMENT:
        // case WStype_FRAGMENT_FIN:
        //     break;

        default:
            Serial.printf("[WSc] Unknown Event Type: %d\n", type);
            break;
    }
}

// --- Helper ---
void AudioWebSocketHandler::printHeapStats(const char* location) {
  #ifdef ESP32 // Only compile this for ESP32 platform
    Serial.printf("[%s] Heap: %u | Min Heap: %u | PSRAM: %u | Min PSRAM: %u\n",
                  location ? location : "Mem",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  ESP.getFreePsram(), ESP.getMinFreePsram());
  #else
      // Optional: Print message for other platforms if needed
      // Serial.printf("[%s] Heap stats not available for this platform.\n", location ? location : "Mem");
  #endif
}