#include "Arduino.h"
#include "driver/i2s.h"        // For I2S microphone input AND output
#include "WiFi.h"
#include <WebSocketsClient.h>  // Use WebSocket client library
#include <limits.h>           // Required for INT16_MAX, INT16_MIN

// --- Button Pin Definition ---
#define BUTTON_PIN 32         // GPIO pin connected to the button (other leg to GND)

// --- I2S Pin Definitions ---
// --- Microphone (INPUT - I2S_NUM_0) ---
#define I2S_MIC_SCK_PIN   18     // Pin Serial Clock (BCK)
#define I2S_MIC_WS_PIN    15     // Pin Word Select (LRCK)
#define I2S_MIC_SD_PIN    13     // Pin Serial Data (DIN)
#define I2S_PORT_MIC      I2S_NUM_0 // Use I2S Port 0 for Microphone

// --- DAC/Amplifier (OUTPUT - I2S_NUM_1) ---
// Choose ONE set of pins and delete the other block completely.
// OPTION A: Use these pins (if available on your board)
#define I2S_DAC_DOUT_PIN  25     // Data Out (DIN on DAC)
#define I2S_DAC_BCLK_PIN  27     // Bit Clock (BCLK/SCK on DAC)
#define I2S_DAC_LRCK_PIN  26     // Left/Right Clock (LRCK/WS on DAC)
#define I2S_PORT_DAC      I2S_NUM_1

// --- I2S Configuration (Shared & Specific) ---
#define MIC_SAMPLE_BUFFER_SIZE 1024 // Size for reading 32-bit samples from Mic
#define I2S_SAMPLE_RATE    (16000) // Sampling frequency (16 kHz) - MUST MATCH SERVER EXPECTATIONS
#define I2S_BITS_PER_SAMPLE (16)   // We send/receive 16-bit data
#define I2S_NUM_CHANNELS   (1)     // Mono

// --- I2S DAC Output Buffering ---
#define I2S_DAC_BUFFER_COUNT    8  // Number of DMA buffers for output
#define I2S_DAC_BUFFER_LENGTH   512 // Size of each DMA buffer in bytes for output
#define I2S_WRITE_TIMEOUT_MS 100   // Max time (ms) to wait for I2S DAC buffer space

// --- WiFi Credentials ---
const char* ssid = "your_ssid";         // <<<--- REPLACE
const char* password = "your_password"; // <<<--- REPLACE

// --- WebSocket Server Details ---
const char* websockets_server_host = "your_server_host"; // <<<--- REPLACE WITH YOUR PC's IP ADDRESS!
const uint16_t websocket_server_port = your_port;                  // <<<--- Port your Python WebSocket server listens on
const char* websocket_path = "/";                              // <<<--- Path for the WebSocket connection

// --- Buffers ---
int32_t i2s_read_buffer32[MIC_SAMPLE_BUFFER_SIZE]; // Buffer for 32-bit I2S reads from Mic
uint8_t i2s_send_buffer16[MIC_SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Buffer for 16-bit data to SEND

// --- Global Objects & State ---
WebSocketsClient webSocket;
bool wsConnected = false;
bool isSendingAudio = false;     // Are we currently recording/sending audio?

// --- Button Debounce Variables ---
// Using the robust debounce logic from the sender code
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

// --- Volume Control ---
const float FIXED_VOLUME = 1.5f; // Set desired playback volume (0.0 to 1.0, >1.0 amplifies but risks clipping)

// --- Function Prototypes ---
void connectWiFi();
void i2s_mic_setup();
void i2s_dac_setup();
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void sendAudioChunk();
void playAudioChunk(uint8_t *payload, size_t length);
void handleButton();
void printHeapStats(const char* location);

// --- Main Setup ---
// --- Main Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial); // Optional: wait for serial monitor
    Serial.println("\n\nESP32 Integrated Audio Record/Stream/Play");
    Serial.printf("Fixed Playback Volume: %.2f\n", FIXED_VOLUME);
    printHeapStats("Start of setup");

    // --- Setup Button Pin ---
    pinMode(BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up resistor
    Serial.printf("Button configured on GPIO %d (INPUT_PULLUP)\n", BUTTON_PIN);

    // --- Connect to Wi-Fi ---
    connectWiFi();
    printHeapStats("After WiFi connect");

    // --- Setup I2S Output (DAC on I2S_NUM_1) ---  <<<--- MOVED UP
    // --- Setup I2S Input (Microphone on I2S_NUM_0 - MASTER) ---
    Serial.println("Configuring I2S Mic Input (I2S_NUM_0 - MASTER)...");
    i2s_mic_setup();
    printHeapStats("After I2S Mic setup");

    // --- Setup I2S Output (DAC on I2S_NUM_1 - SLAVE) ---
    Serial.println("Configuring I2S DAC Output (I2S_NUM_1 - SLAVE)...");
    i2s_dac_setup(); // Now configured as SLAVE
    printHeapStats("After I2S DAC setup");

    // --- Configure WebSocket Client ---
    Serial.printf("Configuring WebSocket. Connecting to ws://%s:%d%s\n", websockets_server_host, websocket_server_port, websocket_path);
    webSocket.begin(websockets_server_host, websocket_server_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000); // Try to reconnect every 5 seconds
    Serial.println("WebSocket client configured. Waiting for connection...");
    printHeapStats("End of setup");

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

    // Keep loop reasonably responsive - small delay is fine if needed
    // If audio playback is choppy, removing or reducing this might help
    // If button is unresponsive, reducing this might help
    delay(5);
}

// --- WiFi Connection ---
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
 * @brief Configure I2S driver for INMP441 microphone (INPUT on I2S_NUM_0)
 */
void i2s_mic_setup() {
    // Uninstall driver first to ensure clean state

    const i2s_config_t i2s_mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // Read 32 bits, INMP441 uses upper 24/16 bits
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Or I2S_CHANNEL_FMT_ONLY_RIGHT depending on wiring
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,  // More buffers for input robustness
        .dma_buf_len = 256, // Smaller buffers for lower latency
        .use_apll = false, // <-- Change to true
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t mic_pin_config = {
        .bck_io_num = I2S_MIC_SCK_PIN,
        .ws_io_num = I2S_MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE, // Input only
        .data_in_num = I2S_MIC_SD_PIN
    };

    esp_err_t err;
    err = i2s_driver_install(I2S_PORT_MIC, &i2s_mic_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to install I2S Mic driver (Port %d): %s\n", I2S_PORT_MIC, esp_err_to_name(err));
        while (true);
    }
    err = i2s_set_pin(I2S_PORT_MIC, &mic_pin_config);
    err = i2s_start(I2S_PORT_MIC); // Add this

    if (err != ESP_OK) {
        Serial.printf("!!! Failed to set I2S Mic pins (Port %d): %s\n", I2S_PORT_MIC, esp_err_to_name(err));
        while (true);
    }
    // Optional: Zero DMA buffer if needed after install
    // i2s_zero_dma_buffer(I2S_PORT_MIC);

    Serial.printf("I2S Mic Input (Port %d) configured successfully.\n", I2S_PORT_MIC);
}

/**
 * @brief Configure I2S driver for DAC/Amplifier (OUTPUT on I2S_NUM_1)
 */
/**
 * @brief Configure I2S driver for DAC/Amplifier (OUTPUT on I2S_NUM_1 - SLAVE)
 */
void i2s_dac_setup() {
    // Uninstall driver first to ensure clean state
    i2s_driver_uninstall(I2S_PORT_DAC); // Good practice

    i2s_config_t i2s_dac_config = {
        .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_TX), // Set as SLAVE Transmitter
        .sample_rate = I2S_SAMPLE_RATE, // Will sync to master clock
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Or ONLY_LEFT depending on your DAC/server needs
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DAC_BUFFER_COUNT,
        .dma_buf_len = I2S_DAC_BUFFER_LENGTH,
        .use_apll = false, // Slave doesn't generate its own clock, so APLL is irrelevant
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    // --- THIS IS THE FIX ---
    // Explicitly tell the slave peripheral which pins to LISTEN ON for clocks
    i2s_pin_config_t dac_pin_config = {
        .bck_io_num = I2S_DAC_BCLK_PIN,   // Assign GPIO 27 as the Bit Clock INPUT pin
        .ws_io_num = I2S_DAC_LRCK_PIN,    // Assign GPIO 26 as the Word Select INPUT pin
        .data_out_num = I2S_DAC_DOUT_PIN, // Assign GPIO 25 as the Data OUTPUT pin
        .data_in_num = I2S_PIN_NO_CHANGE  // No data input for this peripheral
    };
    // --- END FIX ---

    esp_err_t err;
    err = i2s_driver_install(I2S_PORT_DAC, &i2s_dac_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to install I2S DAC driver (Port %d): %s\n", I2S_PORT_DAC, esp_err_to_name(err));
        while (true);
    }

    // Use the CORRECTED dac_pin_config struct
    err = i2s_set_pin(I2S_PORT_DAC, &dac_pin_config);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to set I2S DAC pins (Port %d): %s\n", I2S_PORT_DAC, esp_err_to_name(err));
        while (true);
    }

    // Start the I2S peripheral AFTER setting pins
    err = i2s_start(I2S_PORT_DAC);
    if (err != ESP_OK) {
         Serial.printf("!!! Failed to start I2S DAC Port (%d): %s\n", I2S_PORT_DAC, esp_err_to_name(err));
         while(true);
    }

    // Optional: Zero buffer after starting
    err = i2s_zero_dma_buffer(I2S_PORT_DAC);
     if (err != ESP_OK) {
        Serial.printf("!!! Failed to zero I2S DAC DMA buffer (Port %d): %s\n", I2S_PORT_DAC, esp_err_to_name(err));
    }

    Serial.printf("I2S DAC Output (Port %d) configured successfully AS SLAVE (listening on BCLK=%d, LRCK=%d).\n",
                  I2S_PORT_DAC, I2S_DAC_BCLK_PIN, I2S_DAC_LRCK_PIN);
}
// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WSc] Disconnected!\n");
            wsConnected = false;
            isSendingAudio = false; // Stop sending if disconnected
            // Clear DAC buffer on disconnect to stop any lingering sound
            i2s_zero_dma_buffer(I2S_PORT_DAC);
            Serial.println("I2S DAC buffer cleared on disconnect.");
            break;

        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to server: %s\n", (char*)payload);
            wsConnected = true;
            Serial.println("WebSocket connected. Ready for button press.");
            // Maybe zero DAC buffer here too? Optional.
            // i2s_zero_dma_buffer(I2S_PORT_DAC);
            break;

        case WStype_TEXT:
             Serial.printf("[WSc] Received text: %s\n", payload);
             // Add logic here if server sends specific text commands/responses
             if (strstr((const char*)payload, "ERROR:") != NULL) {
                Serial.printf("!!! Server reported error: %s\n", payload);
             }
             break;

        case WStype_BIN:
            Serial.printf("[WSc] BIN received %d bytes\n", length); // Add this line    
            // Received audio data from server to play
            // Serial.printf("[WSc] Received binary data: %d bytes\n", length); // Can be very verbose
            if (length > 0 && wsConnected) {
                playAudioChunk(payload, length);
            }
            break;

        case WStype_ERROR:
            Serial.printf("[WSc] Error: %s\n", payload);
            // Optionally try to clear DAC buffer on error
            // i2s_zero_dma_buffer(I2S_PORT_DAC);
            break;

        // Other cases like PING, PONG, FRAGMENTs are handled by the library
        case WStype_PING:
            // Serial.println("[WSc] Received PING");
            break;
        case WStype_PONG:
            // Serial.println("[WSc] Received PONG");
            break;
        default:
            Serial.printf("[WSc] Received unknown event type: %d\n", type);
            break;
    }
}

/**
 * @brief Check button state with debouncing and toggle recording
 */
void handleButton() {
    // Uses the robust debounce logic from the original sender code
    int rawReading = digitalRead(BUTTON_PIN);
    static int prevRawReading = HIGH;
    static int stableButtonState = HIGH;
    static unsigned long lastDebounceTime = 0;
    static bool actionTakenOnLastRelease = false;

    if (rawReading != prevRawReading) {
        lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > debounceDelay) {
        if (rawReading != stableButtonState) {
            stableButtonState = rawReading;
            actionTakenOnLastRelease = false; // Reset flag when state changes stably
            // Serial.printf("Button Stable State Changed To: %s\n", stableButtonState == HIGH ? "HIGH (Released)" : "LOW (Pressed)");
        }
    }

    // Action Logic: Trigger ONLY once when stable state becomes HIGH (on release)
    if (stableButtonState == HIGH && !actionTakenOnLastRelease) {
        actionTakenOnLastRelease = true; // Mark action taken for this release

        Serial.println("Button Released (Debounced - Action Trigger)");

        if (wsConnected) {
            if (!isSendingAudio) {
                // --- Start Recording ---
                isSendingAudio = true; // SET STATE FIRST
                Serial.println(">>> Sending START_RECORDING <<<");
                if (!webSocket.sendTXT("START_RECORDING")) {
                    Serial.println("Send START failed!");
                    isSendingAudio = false; // Revert state on failure
                } else {
                    // Discard initial buffer potentially containing noise/clicks
                    size_t bytes_discarded = 0;
                    i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_discarded, pdMS_TO_TICKS(50));
                    Serial.println("Recording started...");
                }
            } else {
                // --- Stop Recording ---
                isSendingAudio = false; // SET STATE FIRST
                Serial.println("\n>>> Sending STOP_RECORDING <<<");
                if (!webSocket.sendTXT("STOP_RECORDING")) {
                    Serial.println("Send STOP failed!");
                    // State is already false, maybe retry later? For now, just log.
                }
                Serial.println("Recording stopped.");
            }
        } else {
            Serial.println("Button released, but WebSocket not connected.");
            isSendingAudio = false; // Ensure sending is off if disconnected
        }
    }

    prevRawReading = rawReading;
}


/**
 * @brief Reads one chunk of audio from I2S Mic, converts, and sends via WebSocket
 */
void sendAudioChunk() {
    size_t bytes_read = 0;
    // Read 32-bit samples from I2S Mic (Port 0)
    esp_err_t result = i2s_read(I2S_PORT_MIC, i2s_read_buffer32, sizeof(i2s_read_buffer32), &bytes_read, pdMS_TO_TICKS(100)); // Blocking call with timeout

    if (result == ESP_OK && bytes_read > 0) {
        int samples_read = bytes_read / sizeof(int32_t);
        // Clamp samples read just in case, though should match buffer size
        if (samples_read > MIC_SAMPLE_BUFFER_SIZE) samples_read = MIC_SAMPLE_BUFFER_SIZE;
        int bytes_to_send = samples_read * sizeof(int16_t);

        // Convert 32-bit data (using top 16 bits) to 16-bit PCM Little Endian
        for (int i = 0; i < samples_read; i++) {
            // Shift right by 16 bits to get the significant high bits (typical for INMP441)
            // Adjust the shift (e.g., >> 8) if your mic provides data differently (e.g., 24-bit in lower bits)
            int16_t sample16 = (int16_t)(i2s_read_buffer32[i] >> 16);

            // Place into the send buffer as little-endian bytes
            i2s_send_buffer16[2 * i]     = (uint8_t)(sample16 & 0xFF);        // Low byte
            i2s_send_buffer16[2 * i + 1] = (uint8_t)((sample16 >> 8) & 0xFF); // High byte
        }

        // Send the 16-bit binary audio chunk via WebSocket
        if (!webSocket.sendBIN(i2s_send_buffer16, bytes_to_send)) {
            Serial.println("WebSocket sendBIN failed!");
            // Stop sending on failure to avoid flooding logs?
            isSendingAudio = false;
            // Consider sending STOP_RECORDING_ERROR if possible, but sendBIN already failed...
            // webSocket.sendTXT("STOP_RECORDING_ERROR"); // This might also fail
        } else {
           // Serial.print("."); // Indicate chunk sent - uncomment if needed, but can be very verbose
        }

    } else if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
         // Log actual I2S read errors, ignore timeouts if they are expected sometimes
         Serial.printf("[I2S IN %d] Read Error: %d (%s)\n", I2S_PORT_MIC, result, esp_err_to_name(result));
         // Consider stopping recording on persistent read errors
         // isSendingAudio = false;
         // if(wsConnected) webSocket.sendTXT("STOP_RECORDING_ERROR");
    }
    // Note: ESP_ERR_TIMEOUT means no data was available within the timeout. This can be normal if the mic isn't producing data fast enough or if the loop is blocked elsewhere.
}

/**
 * @brief Processes and plays a received audio chunk via I2S DAC (Port 1)
 */
void playAudioChunk(uint8_t *payload, size_t length) {
    if (length == 0) return;

    // Ensure data length is even for 16-bit samples
    if (length % 2 != 0) {
        Serial.printf("[I2S OUT %d] Received odd length binary data (%d bytes), skipping.\n", I2S_PORT_DAC, length);
        return;
    }

    // --- Apply Volume Control (in-place modification of payload) ---
    int16_t* samples = (int16_t*)payload; // Cast buffer to 16-bit samples
    size_t num_samples = length / sizeof(int16_t);
    const float vol = FIXED_VOLUME; // Use the global fixed volume

    if (abs(vol - 1.0f) > 0.01f) { // Apply volume only if it's not effectively 1.0
        for (size_t i = 0; i < num_samples; i++) {
            // Scale the sample
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

    // --- Write MODIFIED samples to I2S DAC (Port 1) ---
    size_t bytes_written = 0;
    TickType_t max_wait = pdMS_TO_TICKS(I2S_WRITE_TIMEOUT_MS); // Max time to wait for buffer space

    esp_err_t result = i2s_write(I2S_PORT_DAC, payload, length, &bytes_written, max_wait);

    if (result == ESP_OK) {
        if (bytes_written < length) {
            Serial.printf("[I2S OUT %d] Wrote only %d/%d bytes (Timeout likely)\n", I2S_PORT_DAC, bytes_written, length);
            // Buffer was likely full, data was dropped. Consider clearing buffer if this persists.
            // i2s_zero_dma_buffer(I2S_PORT_DAC);
        } else {
           // Successfully wrote all bytes
           Serial.print("p"); // Indicate chunk played - can be verbose
        }
    } else if (result == ESP_ERR_TIMEOUT) {
        // This is expected if the buffer is full and we can't write within the timeout
        Serial.printf("[I2S OUT %d] Write Timeout (Buffer Full?). Dropped %d bytes.\n", I2S_PORT_DAC, length);
        // Consider clearing the buffer if timeouts happen frequently, implies data rate mismatch or processing delays
         i2s_zero_dma_buffer(I2S_PORT_DAC); // Clear buffer to potentially recover
    } else {
        // Log other unexpected I2S write errors
        Serial.printf("!!! [I2S OUT %d] Write Error: %s (%d)\n", I2S_PORT_DAC, esp_err_to_name(result), result);
    }
}


// --- Helper to print memory stats ---
void printHeapStats(const char* location) {
  Serial.printf("[%s] Heap: %d | Min Heap: %d | PSRAM: %d | Min PSRAM: %d\n",
                location ? location : "Mem",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                ESP.getFreePsram(), ESP.getMinFreePsram());
}