#include <ESP8266Audio.h> // Requires the ESP8266Audio library by Earle F. Philhower, III (Install via Library Manager)
#include <Arduino.h>
#include <WiFi.h>
 


// --- Pin Definitions (Using YOUR specified pins) ---
#define I2S_DOUT      25  // Data Out pin (Connect this ESP32 pin to MAX98357A DIN)
#define I2S_BCLK      27  // Bit Clock pin (Connect this ESP32 pin to MAX98357A BCLK)
#define I2S_LRC       26  // Left/Right Clock pin (Word Select) (Connect this ESP32 pin to MAX98357A LRC)

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan"; // Your Wi-Fi network name
const char* password = "40290678";   // Your Wi-Fi network password

// --- Audio Object ---
Audio audio;

// --- Radio Stream URL ---
// Using the one from your original example. You can change this to another MP3 stream.
const char* stream_url = "http://vis.media-ice.musicradio.com/CapitalMP3";

void setup() {
  Serial.begin(115200); // Start serial communication for debugging
  Serial.println("\n\nStarting Audio Test...");

  // --- Connect to WiFi ---
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.disconnect(true); // Disconnect from any previous network
  WiFi.mode(WIFI_STA);   // Set WiFi mode to Station
  WiFi.begin(ssid, password);

  // Wait for connection
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 20) { // Timeout after ~10 seconds
       Serial.println("\nFailed to connect to WiFi!");
       // Optional: loop forever or try to reboot
       while(true) { delay(1000); }
    }
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // --- Configure Audio ---
  Serial.println("Setting up I2S pins (DOUT=25, BCLK=27, LRC=26)...");
  // NOTE: The order in setPinout is typically BCLK, LRC, DOUT
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

  Serial.println("Setting volume (starting low)...");
  // Set volume (range 0-21 is common for this library, 100 might be invalid or max gain)
  // Start with a moderate volume like 15. Adjust as needed.
  audio.setVolume(15); // Start lower than 100! Adjust later if needed.

  Serial.print("Connecting to audio host: ");
  Serial.println(stream_url);
  if (!audio.connecttohost(stream_url)) {
      Serial.println("Failed to connect to audio host!");
      // Optional: loop forever or try to reboot
      while(true) { delay(1000); }
  } else {
      Serial.println("Connected to audio host. Playback should start.");
  }
}

void loop()
{
  audio.loop(); // This is REQUIRED - it handles buffering, decoding, and sending data
  // Add a small delay if needed, but audio.loop() usually manages timing.
  // delay(5);
}

// Optional: Add callbacks for audio events (useful for debugging)
void audio_info(const char *info){
    Serial.print("Audio Info: "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    Serial.print("ID3 Data: "); Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    Serial.print("End of MP3: "); Serial.println(info);
}
// Add more callbacks as needed based on the library documentation