#include "Arduino.h"
#include "driver/i2s.h"
// #include "WiFi.h" // WiFi library no longer needed for this test

// Definición de pines para el micrófono INMP441
#define I2S_SCK_PIN  18             // Pin Serial Clock (SCK)
#define I2S_WS_PIN   15             // Pin Word Select (WS)
#define I2S_SD_PIN   13             // Pin Serial Data (SD)

// Configuración del driver I2S para el micrófono INMP441
#define I2S_PORT           I2S_NUM_0 // I2S port number (0)
#define SAMPLE_BUFFER_SIZE 1024      // Tamaño del buffer de muestras (1024)
#define I2S_SAMPLE_RATE    (16000)   // Frecuencia de muestreo (16 kHz)
// #define RECORD_TIME        (10)      // Recording time limit not strictly enforced in this test

// --- Network variables removed ---
// const char* ssid = "YOUR_SSID";
// const char* password = "YOUR_PASSWORD";
// const char* serverName = "YOUR_SERVER_IP";
// const int serverPort = 8888;
// WiFiClient client;

// Constantes para la cabecera del archivo WAV - Not used, but harmless to keep definition
const int HEADERSIZE = 44;

// Prototipos de funciones
void micTask_RecordOnly(void* parameter); // Renamed task function
// void setWavHeader(uint8_t* header, int wavSize); // Header calculation not needed
void i2s_config_setup();

// Buffer de muestras de audio (32 bits) y buffer para 16-bit data
int32_t i2s_read_buffer[SAMPLE_BUFFER_SIZE];
uint8_t i2s_read_buff16[SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Still useful if checking conversion

// Estructura de parámetros para la tarea del micrófono - Simplified
struct MicTaskParameters {
    // int duracion; // Not used for continuous test
    int frecuencia;
    int bufferSize;
} micParams;


/**
 * @brief Función de configuración del driver I2S para el micrófono INMP441
 */
void i2s_config_setup() {
    // Attempt uninstall first to ensure clean state, ignore error if not installed
    i2s_driver_uninstall(I2S_PORT);
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, // Try increasing this (e.g., 12, 16) if glitches persist even here
        .dma_buf_len = 1024,
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
        Serial.println("i2s_driver_install error");
        while(1); // Halt on error
    }

    if (ESP_OK != i2s_set_pin(I2S_PORT, &pin_config)) {
        Serial.println("i2s_set_pin error");
        while(1); // Halt on error
    }
    Serial.println("I2S driver configured successfully.");
}

/**
 * @brief Función de configuración de la placa ESP32
 * Se ejecuta una sola vez al inicio del programa
 */
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 I2S Audio Recording Test (No Network)");

    // --- WiFi connection removed ---
    // WiFi.begin(ssid, password); ...

    // Configure I2S
    i2s_config_setup();

    // Populate parameters for the task
    // micParams.duracion = RECORD_TIME; // Not needed
    micParams.frecuencia = I2S_SAMPLE_RATE;
    micParams.bufferSize = SAMPLE_BUFFER_SIZE;
    Serial.println("Microphone Task Configuration:");
    Serial.println("Frequency: " + String(micParams.frecuencia) + " Hz");
    Serial.println("Buffer Size: " + String(micParams.bufferSize) + " samples");

   // Create task to ONLY record audio continuously
   xTaskCreatePinnedToCore(
                    micTask_RecordOnly, // Use the modified task function
                    "micRecTask",       // Name of the task
                    4096,               // Stack size can likely be reduced now
                    &micParams,         // Task input parameter
                    1,                  // Priority
                    NULL,               // Task handle
                    1);                 // Core 1
}

/**
 * @brief Función principal del programa (bucle infinito)
 */
void loop() {
    // Keep loop empty
    delay(1000);
}

/**
 * @brief Tarea para leer muestras de audio del micrófono I2S CONTINUOUSLY
 *          This version DOES NOT send data over network.
 * @param parameter Puntero a los parámetros de la tarea (MicTaskParameters)
 */
void micTask_RecordOnly(void* parameter) {
    MicTaskParameters * params = (MicTaskParameters *) parameter;
    int bufferSize = params->bufferSize;
    size_t bytes_read = 0;

    Serial.println("RecordOnly Task started: Continuously reading I2S data...");

    // --- Network connection & HTTP sending removed ---

    // --- Discard initial buffers (still good practice) ---
    int initial_discard_buffers = 8;
    Serial.printf("Discarding first %d buffers...\n", initial_discard_buffers);
    for (int i = 0; i < initial_discard_buffers; i++) {
        i2s_read(I2S_PORT, i2s_read_buffer, bufferSize * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    }
    Serial.println("Initial buffers discarded. Starting continuous read.");

    long loopCounter = 0;
    unsigned long lastPrintTime = 0;

    // --- Continuous Recording Loop ---
    while (true) { // Loop indefinitely
        // Read 32-bit samples from I2S
        esp_err_t result = i2s_read(I2S_PORT, i2s_read_buffer, bufferSize * sizeof(int32_t), &bytes_read, portMAX_DELAY); // Wait as needed

        if (result != ESP_OK || bytes_read == 0) {
            Serial.printf("I2S read error! Result: %d, Bytes Read: %d\n", result, bytes_read);
            // Optional: Add a small delay before retrying?
            vTaskDelay(pdMS_TO_TICKS(10));
            continue; // Try again
        }

        // Optional: Minimal data processing (like the conversion) can stay if you want to test that too
        // If you only want to test i2s_read itself, you can comment this out.
        int samples_read = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples_read; i++) {
            i2s_read_buff16[2 * i]     = (uint8_t)(i2s_read_buffer[i] >> 16 & 0xFF);
            i2s_read_buff16[2 * i + 1] = (uint8_t)(i2s_read_buffer[i] >> 24 & 0xFF);
        }

        // --- Data Sending Removed ---
        // client.write(...);

        // --- Provide Periodic Feedback (Avoid flooding Serial) ---
        loopCounter++;
        unsigned long currentTime = millis();
        if (currentTime - lastPrintTime >= 2000) { // Print every 2 seconds
            // Example: Print the first 16-bit sample value (reconstructed)
            int16_t firstSample = (int16_t)((i2s_read_buff16[1] << 8) | i2s_read_buff16[0]);
            Serial.printf("Loop %ld: Read %d bytes. First sample: %d\n", loopCounter, bytes_read, firstSample);
            lastPrintTime = currentTime;
        }

        // Minimal delay to allow other system tasks (like idle task) to run briefly
        // Should not be necessary if i2s_read blocks appropriately, but safe to keep.
        vTaskDelay(1);

    } // End of while(true)

    // --- Code below this point is unreachable in while(true) loop ---
    // --- Server response waiting / client.stop() removed ---

    Serial.println("RecordOnly Task finished (should not happen).");
    // Task will loop forever, need to reset ESP32 to stop.
    // vTaskDelete(NULL); // No longer reachable
}

// --- setWavHeader function removed as it's not called ---