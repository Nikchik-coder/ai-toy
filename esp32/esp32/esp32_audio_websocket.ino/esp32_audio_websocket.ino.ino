#include "AudioWebSocketHandler.h" // Include the class header

// Create an instance of the handler
AudioWebSocketHandler audioHandler;

void setup() {
  // Initialize the handler (connects WiFi, sets up I2S, WebSocket, etc.)
  audioHandler.begin();
}

void loop() {
  // Run the handler's loop function (manages WebSocket, button, audio sending)
  audioHandler.loop();
}