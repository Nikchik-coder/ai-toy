#include "Arduino.h"
#include "driver/i2s.h"
#include "WiFi.h"
#include <FS.h>       // Added for Filesystem
#include <SPIFFS.h>   // Added for SPIFFS

// Definición de pines para el micrófono INMP441
#define I2S_SCK_PIN  18
#define I2S_WS_PIN   15
#define I2S_SD_PIN   13

// Configuración del driver I2S
#define I2S_PORT           I2S_NUM_0
#define SAMPLE_BUFFER_SIZE 1024
#define I2S_SAMPLE_RATE    (16000)
#define RECORD_TIME        (10)      // seconds of grabación

// Credenciales de la red WiFi
const char* ssid = "988664 info-lan"; // Reemplaza con el nombre de tu red WiFi
const char* password = "40290678";   // Reemplaza con tu contraseña WiFi

// Servidor HTTP (IP y puerto)
const char* serverName = "192.168.0.101"; // <<<--- ONLY the IP address
const int serverPort = 8888;

// Constantes para la cabecera del archivo WAV y Filename
const int HEADERSIZE = 44;
const char* FILENAME = "/recording.wav"; // File path on SPIFFS

// Cliente WiFi
WiFiClient client;

// Prototipos de funciones
void micTask_RecordThenSend(void* parameter); // Renamed task function
void setWavHeader(uint8_t* header, int wavDataSize);
void i2s_config_setup();

// Buffers
int32_t i2s_read_buffer[SAMPLE_BUFFER_SIZE];
uint8_t i2s_write_buff16[SAMPLE_BUFFER_SIZE * sizeof(int16_t)]; // Buffer for 16-bit data going to SPIFFS
uint8_t transmitBuffer[1024]; // Separate buffer for sending file data

// Estructura de parámetros
struct MicTaskParameters {
    int duracion;
    int frecuencia;
    int bufferSize;
} micParams;


/**
 * @brief Configuración I2S
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
        // Increased buffer count as discussed - keep this or adjust if needed
        .dma_buf_count = 16,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN, .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD_PIN
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
 * @brief Setup inicial
 */
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 Audio Recorder (SPIFFS) to Flask Server - Diag Print Added"); // Updated title

    // --- Initialize SPIFFS ---
    if (!SPIFFS.begin(true)) { // Format SPIFFS if first time mount fails
        Serial.println("SPIFFS Mount Failed! Halting.");
        while (1);
    }
    Serial.println("SPIFFS mounted successfully.");

    // Configure I2S
    i2s_config_setup();

    // --- Connect to WiFi ---
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi "); Serial.print(ssid);
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        connect_tries++;
        if (connect_tries > 20) {
            Serial.println("\nFailed to connect to WiFi. Halting."); while(1);
        }
    }
    Serial.println("\nConnected to WiFi");
    Serial.print("IP address: "); Serial.println(WiFi.localIP());

    // Populate parameters
    micParams.duracion = RECORD_TIME;
    micParams.frecuencia = I2S_SAMPLE_RATE;
    micParams.bufferSize = SAMPLE_BUFFER_SIZE;
    Serial.println("Task Configuration:");
    Serial.println("Duration: " + String(micParams.duracion) + "s");
    Serial.println("Frequency: " + String(micParams.frecuencia) + " Hz");
    Serial.println("Buffer Size: " + String(micParams.bufferSize) + " samples");

   // Create task
   xTaskCreatePinnedToCore(
                    micTask_RecordThenSend, // Use the modified task function
                    "micRecSend",
                    10000,            // Keep stack size generous due to file I/O + network
                    &micParams,
                    1, NULL, 1);
}

void loop() {
    delay(1000); // Keep loop simple
}

/**
 * @brief Tarea para grabar audio a SPIFFS y luego enviarlo por HTTP (Revisada + Diag Print)
 */
void micTask_RecordThenSend(void* parameter) {
    MicTaskParameters * params = (MicTaskParameters *) parameter;
    int duracion = params->duracion;
    int frecuencia = params->frecuencia;
    int bufferSize = params->bufferSize;
    long numBuffers = (long)duracion * frecuencia / bufferSize;

    Serial.printf("Task started: Will record %d seconds to SPIFFS file %s\n", duracion, FILENAME);

    size_t bytes_read = 0;
    File audioFile; // File object
    long totalAudioBytesWritten = 0; // Counter for actual audio bytes

    // --- Phase 1: Recording to SPIFFS ---
    Serial.println("Phase 1: Recording audio to SPIFFS...");

    // Delete existing file first, if it exists, for a clean start
    if (SPIFFS.exists(FILENAME)) {
        Serial.println("Removing existing recording file...");
        if (!SPIFFS.remove(FILENAME)) {
             Serial.println("Failed to remove existing file!");
        }
    }

    audioFile = SPIFFS.open(FILENAME, FILE_WRITE);
    if (!audioFile) {
        Serial.println("Failed to open file for writing!");
        vTaskDelete(NULL); return;
    }

    uint8_t placeholderHeader[HEADERSIZE] = {0};
    size_t written = audioFile.write(placeholderHeader, HEADERSIZE);
    if (written != HEADERSIZE) {
         Serial.println("Error writing placeholder header!");
         audioFile.close(); SPIFFS.remove(FILENAME); vTaskDelete(NULL); return;
    }

    Serial.println("Discarding initial buffers...");
    for (int i = 0; i < 8; i++) {
        i2s_read(I2S_PORT, i2s_read_buffer, bufferSize * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    }
    Serial.println("Starting main recording loop...");

    for (long j = 0; j < numBuffers; j++) {
        esp_err_t result = i2s_read(I2S_PORT, i2s_read_buffer, bufferSize * sizeof(int32_t), &bytes_read, portMAX_DELAY);
        if (result != ESP_OK || bytes_read == 0) {
            Serial.printf("I2S read error during recording! Result: %d, Bytes Read: %d\n", result, bytes_read);
            break;
        }

        int samples_read = bytes_read / sizeof(int32_t);
        int bytes_to_write = samples_read * sizeof(int16_t);

        // Convert 32-bit data to 16-bit data
        for (int i = 0; i < samples_read; i++) {
            i2s_write_buff16[2 * i]     = (uint8_t)(i2s_read_buffer[i] >> 16 & 0xFF); // LSB
            i2s_write_buff16[2 * i + 1] = (uint8_t)(i2s_read_buffer[i] >> 24 & 0xFF); // MSB
        }

        // *** DIAGNOSTIC PRINT ADDED HERE ***
        // Print samples from one buffer near the beginning (e.g., buffer #10)
        // Make noise near the mic when this is expected to print!
        if (j == 10 && samples_read > 5) {
             Serial.print("Diag: Raw 32b Samples (buf 10): ");
             for(int k=0; k<5; k++) { Serial.printf("%ld ", i2s_read_buffer[k]); }
             Serial.println();

             Serial.print("Diag: Conv 16b Samples (buf 10): ");
             for(int k=0; k<5; k++) {
                 // Reconstruct the 16-bit signed value for printing
                 int16_t sample = (int16_t)((i2s_write_buff16[2*k+1] << 8) | i2s_write_buff16[2*k]);
                 Serial.printf("%d ", sample);
             }
             Serial.println();
        }
        // *** END DIAGNOSTIC PRINT ***

        // Write the converted 16-bit data to the file
        written = audioFile.write(i2s_write_buff16, bytes_to_write);
        if (written != bytes_to_write) {
            Serial.println("Error writing audio data chunk to SPIFFS!");
            break;
        }
        totalAudioBytesWritten += written;
        vTaskDelay(1); // Yield briefly
    }
    Serial.println("\nFinished recording loop.");
    Serial.printf("Total raw audio bytes written to SPIFFS (calculated): %ld\n", totalAudioBytesWritten);
    audioFile.close(); // Close the file after writing audio data
    Serial.println("Audio file closed after recording.");


    // --- Phase 1.5: Update WAV Header with Correct Size ---
    Serial.println("Phase 1.5: Updating WAV header...");

    long actualAudioDataSize = totalAudioBytesWritten;
    if (actualAudioDataSize <= 0) {
        Serial.println("Error: Recorded audio data size is zero or negative!");
        SPIFFS.remove(FILENAME); vTaskDelete(NULL); return;
    }

    // Reopen using "r+" mode for read/write without truncation
    audioFile = SPIFFS.open(FILENAME, "r+");
    if (!audioFile) {
        Serial.println("Failed to reopen file in 'r+' mode for header update!");
        SPIFFS.remove(FILENAME); vTaskDelete(NULL); return;
    }
    Serial.println("File successfully reopened in 'r+' mode.");

    uint8_t finalHeader[HEADERSIZE];
    setWavHeader(finalHeader, actualAudioDataSize); // Generate header with correct size

    // Explicitly seek to the beginning before writing header
    if (!audioFile.seek(0)) {
        Serial.println("Failed to seek to beginning of file for header update!");
        audioFile.close(); SPIFFS.remove(FILENAME); vTaskDelete(NULL); return;
    }
    Serial.println("Seek to beginning successful.");

    // Write the final header over the placeholder
    written = audioFile.write(finalHeader, HEADERSIZE);
    if (written != HEADERSIZE) {
         Serial.println("Error writing final WAV header!");
         audioFile.close(); SPIFFS.remove(FILENAME); vTaskDelete(NULL); return;
    }
    Serial.println("Header write successful.");

    audioFile.flush(); // Ensure data is committed
    audioFile.close(); // Close after writing header
    Serial.println("WAV header updated successfully and file closed.");
    delay(100); // Allow filesystem operations to settle

    // --- Phase 2: Transmission from SPIFFS ---
    Serial.println("Phase 2: Sending recorded file from SPIFFS...");

    // Get the final total file size for Content-Length
    audioFile = SPIFFS.open(FILENAME, FILE_READ); // Open fresh for reading
    if (!audioFile) {
        Serial.println("Failed to open file for reading/sending!");
        vTaskDelete(NULL); return;
    }
    size_t totalWavFileSize = audioFile.size(); // Read size AFTER reopening
    Serial.printf("File reopened for reading. Reported size: %u bytes\n", totalWavFileSize);
    if (totalWavFileSize <= HEADERSIZE) {
         Serial.printf("Error: Final WAV file size (%u bytes) is too small after reopening! Expected > %d bytes.\n", totalWavFileSize, HEADERSIZE);
         audioFile.close(); vTaskDelete(NULL); return;
    }
    Serial.printf("Total file size seems OK. Proceeding to send: %u bytes\n", totalWavFileSize);

    // Connect to server
    Serial.printf("Connecting to server: %s:%d\n", serverName, serverPort);
    if (!client.connect(serverName, serverPort)) {
        Serial.println("Connection to server failed!");
        audioFile.close(); vTaskDelete(NULL); return;
    }
    Serial.println("Connected to server.");

    // Send HTTP POST Request
    Serial.println("Sending HTTP POST request...");
    client.println("POST /uploadAudio HTTP/1.1");
    client.println("Host: " + String(serverName) + ":" + String(serverPort));
    client.println("Content-Type: audio/wav");
    client.println("Content-Length: " + String(totalWavFileSize)); // Use actual size
    client.println("Connection: close");
    client.println(); // End of headers

    // Send file content
    Serial.println("Sending file content...");
    size_t bytesSent = 0;
    while (audioFile.available()) {
        size_t bytesToRead = audioFile.read(transmitBuffer, sizeof(transmitBuffer));
        if (bytesToRead > 0) {
            size_t bytesWrittenToNet = client.write(transmitBuffer, bytesToRead);
            if (bytesWrittenToNet != bytesToRead) {
                Serial.println("Error sending file chunk to network!"); break;
            }
            bytesSent += bytesWrittenToNet;
        } else {
            break;
        }
    }
    client.flush(); // Ensure all buffered network data is sent
    Serial.printf("Finished sending file data. Total bytes sent: %u\n", bytesSent);
    audioFile.close(); // Close file after reading

    if(bytesSent != totalWavFileSize){
        Serial.printf("Warning: Bytes sent (%u) does not match file size (%u)!\n", bytesSent, totalWavFileSize);
    }

    // Wait for Server Response
    Serial.println("Waiting for server response...");
    unsigned long timeoutStart = millis();
    while (client.available() == 0) {
        if (millis() - timeoutStart > 10000) {
            Serial.println("Server response timeout!"); client.stop(); vTaskDelete(NULL); return;
        }
        delay(100);
    }
    Serial.println("Server response:");
    while (client.available()) { String line = client.readStringUntil('\r'); Serial.print(line); }
    Serial.println("\nEnd of response.");

    // Cleanup
    client.stop();
    Serial.println("Connection closed.");
    if (SPIFFS.remove(FILENAME)) {
        Serial.printf("File %s deleted successfully.\n", FILENAME);
    } else {
        Serial.printf("Failed to delete file %s.\n", FILENAME);
    }

    Serial.println("micTask_RecordThenSend finished.");
    vTaskDelete(NULL); // Delete this task
}


/**
 * @brief Configura encabezado WAV (16-bit Mono PCM)
 */
void setWavHeader(uint8_t* header, int wavDataSize) { // wavDataSize is ONLY audio samples
    int fileSize = wavDataSize + HEADERSIZE - 8;
    int sampleRate = I2S_SAMPLE_RATE;
    int bitsPerSample = 16;
    int numChannels = 1;
    int byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    int blockAlign = numChannels * (bitsPerSample / 8);

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(fileSize & 0xFF); header[5] = (uint8_t)((fileSize >> 8) & 0xFF);
    header[6] = (uint8_t)((fileSize >> 16) & 0xFF); header[7] = (uint8_t)((fileSize >> 24) & 0xFF);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;
    header[22] = (uint8_t)numChannels; header[23] = 0;
    header[24] = (uint8_t)(sampleRate & 0xFF); header[25] = (uint8_t)((sampleRate >> 8) & 0xFF);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xFF); header[27] = (uint8_t)((sampleRate >> 24) & 0xFF);
    header[28] = (uint8_t)(byteRate & 0xFF); header[29] = (uint8_t)((byteRate >> 8) & 0xFF);
    header[30] = (uint8_t)((byteRate >> 16) & 0xFF); header[31] = (uint8_t)((byteRate >> 24) & 0xFF);
    header[32] = (uint8_t)blockAlign; header[33] = 0;
    header[34] = (uint8_t)bitsPerSample; header[35] = 0;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(wavDataSize & 0xFF); header[41] = (uint8_t)((wavDataSize >> 8) & 0xFF);
    header[42] = (uint8_t)((wavDataSize >> 16) & 0xFF); header[43] = (uint8_t)((wavDataSize >> 24) & 0xFF);
}