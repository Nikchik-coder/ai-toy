#include <Arduino.h>
#include "driver/i2s.h"
#include <math.h> // Required for M_PI and sin()

// --- Pin Definitions ---
// *** MAKE SURE these match your hardware connections ***
#define I2S_DOUT      GPIO_NUM_25  // Data Out (DIN) to your DAC/Amplifier
#define I2S_BCLK      GPIO_NUM_27  // Bit Clock (BCLK/SCK)
#define I2S_LRC       GPIO_NUM_26  // Left/Right Clock (LRCK/WS)

// --- I2S Configuration ---
#define SAMPLE_RATE         (16000) // Sample rate in Hz (e.g., 16kHz)
#define BITS_PER_SAMPLE     (16)    // Bits per sample (e.g., 16-bit)
#define NUM_CHANNELS        (1)     // 1 for mono
#define I2S_PORT            (I2S_NUM_0) // Use I2S Port 0

// --- Sine Wave Generation ---
#define SINE_FREQUENCY      (440.0f) // Frequency of the sine wave in Hz (e.g., 440 Hz = A4 note)
#define SINE_AMPLITUDE      (15000)  // Amplitude (loudness). Max for 16-bit is 32767. Keep below max to avoid clipping.
#define BUFFER_SAMPLES      (256)    // Number of samples per buffer write
#define BUFFER_BYTES        (BUFFER_SAMPLES * (BITS_PER_SAMPLE / 8)) // Bytes per buffer write

// --- Global Variables ---
int16_t i2s_write_buffer[BUFFER_SAMPLES]; // Buffer to hold samples before writing
float phase_accumulator = 0.0f;           // Tracks the current position in the sine wave cycle
float phase_increment = 0.0f;             // How much to advance the phase per sample

// --- Function Prototypes ---
void configureI2S();
void generateAndPlaySine();

void setup() {
    Serial.begin(115200);
    while (!Serial); // Wait for Serial Monitor to open (optional)
    Serial.println("\n\nStarting I2S Sine Wave Test...");

    Serial.println("Configuring I2S...");
    configureI2S();
    Serial.println("I2S Configured.");

    // Calculate phase increment based on sample rate and frequency
    phase_increment = (2.0f * M_PI * SINE_FREQUENCY) / SAMPLE_RATE;
    Serial.printf("Sample Rate: %d Hz\n", SAMPLE_RATE);
    Serial.printf("Sine Frequency: %.2f Hz\n", SINE_FREQUENCY);
    Serial.printf("Sine Amplitude: %d\n", SINE_AMPLITUDE);
    Serial.printf("Phase Increment: %f radians/sample\n", phase_increment);
    Serial.printf("Buffer Size: %d samples (%d bytes)\n", BUFFER_SAMPLES, BUFFER_BYTES);

    Serial.println("Starting sine wave generation...");
}

void loop() {
    generateAndPlaySine();
    // No delay needed here usually, as i2s_write will block if the buffer is full.
    // If you experience watchdog timeouts, you might need a tiny delay(1),
    // but it's generally better if i2s_write handles the flow control.
}


// --- I2S Configuration Function (Adapted from your original code) ---
void configureI2S() {
    // Uninstall driver first if it was previously installed (good practice)
    // i2s_driver_uninstall(I2S_PORT); // Uncomment if needed

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), // Transmit mode
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // Use the IDF enum
        // For mono audio on stereo DACs (like MAX98357A, PCM5102), duplicate mono sample to L/R
        // If your DAC *only* uses Left or Right, change this.
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // Assumes mono played on stereo DAC
        .communication_format = I2S_COMM_FORMAT_STAND_I2S, // Standard I2S protocol
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,      // Interrupt level
        .dma_buf_count = 8,                            // Number of DMA buffers (can adjust, 8 is often enough for generation)
        .dma_buf_len = BUFFER_SAMPLES,                 // Length of each buffer *in samples* (Max 1024 samples for ESP32)
        .use_apll = false,                             // Use internal APLL clock? Usually false for standard rates
        .tx_desc_auto_clear = true,                    // Auto clear TX descriptors
        .fixed_mclk = 0                                // Set to 0 for auto MCLK, or specify MCLK rate
    };

    // Configure I2S pins
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE // Not using input
    };

    // Install and start I2S driver
    esp_err_t err;
    err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to install I2S driver: %s\n", esp_err_to_name(err));
        while (true); // Halt
    }
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("!!! Failed to set I2S pins: %s\n", esp_err_to_name(err));
        while (true); // Halt
    }
    // Optional: Zero the DMA buffer initially
    err = i2s_zero_dma_buffer(I2S_PORT);
     if (err != ESP_OK) {
        Serial.printf("!!! Failed to zero I2S DMA buffer: %s\n", esp_err_to_name(err));
    }
}

// --- Generate Sine Wave and Write to I2S ---
void generateAndPlaySine() {
    // 1. Fill the buffer with sine wave samples
    for (int i = 0; i < BUFFER_SAMPLES; i++) {
        // Calculate sample value
        float sample_float = SINE_AMPLITUDE * sin(phase_accumulator);

        // Convert to 16-bit integer and store in buffer
        i2s_write_buffer[i] = (int16_t)sample_float;

        // Increment phase
        phase_accumulator += phase_increment;

        // Wrap phase around 2*PI to prevent it from growing indefinitely (optional but good practice)
        if (phase_accumulator >= (2.0f * M_PI)) {
            phase_accumulator -= (2.0f * M_PI);
        }
    }

    // 2. Write the buffer to I2S
    size_t bytes_written = 0;
    // Use portMAX_DELAY to wait indefinitely until space is available.
    // If this causes issues (like watchdog resets), you might need a timeout,
    // but for simple generation, waiting is usually fine.
    esp_err_t result = i2s_write(I2S_PORT, i2s_write_buffer, BUFFER_BYTES, &bytes_written, portMAX_DELAY);

    // 3. Check for errors
    if (result != ESP_OK) {
        Serial.printf("!!! I2S Write Error: %s\n", esp_err_to_name(result));
    }
    if (bytes_written < BUFFER_BYTES) {
        Serial.printf("!!! I2S Write Underrun: only wrote %d/%d bytes\n", bytes_written, BUFFER_BYTES);
        // This indicates the loop isn't filling the buffer fast enough,
        // or the I2S clocking is wrong. Should not happen with simple sine generation.
    }
}