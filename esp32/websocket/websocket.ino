#include "Arduino.h"
#include "WiFi.h"
#include <WebSocketsClient.h> // Use WebSocket client library

// --- WiFi Credentials ---
const char* ssid = "988664 info-lan"; // Your Wi-Fi network name
const char* password = "40290678";   // Your Wi-Fi network password

// --- WebSocket Server Details ---
const char* websocket_server_host = "192.168.0.101"; // <<<--- REPLACE WITH YOUR PYTHON SERVER'S IP ADDRESS
const uint16_t websocket_server_port = 8765;         // <<<--- Port your Python WebSocket server will listen on
const char* websocket_path = "/"; 
unsigned long lastTextSendTime = 0;
const long textInterval = 5000;                  // <<<--- Path for the WebSocket connection (can be simple "/")

// --- Global Objects ---
WebSocketsClient webSocket;
bool wsConnected = false;

// --- WebSocket Event Handler ---
// This function gets called when WebSocket events occur
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WSc] Disconnected!\n");
            wsConnected = false;
            break;
        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected to url: %s\n", (char*)payload);
            wsConnected = true;
            // Optional: Send a PING or an introductory message
            // webSocket.sendTXT("ESP32 Client Connected");
            break;
        case WStype_TEXT:
            Serial.printf("[WSc] Received text: %s\n", (char*)payload);
            // We are not expecting text from server in this basic example
            break;
        case WStype_BIN:
            Serial.printf("[WSc] Received binary data of length %u\n", length);
            // We are not expecting binary data yet
            break;
        case WStype_ERROR:
             Serial.printf("[WSc] Error: %s\n", (char*)payload);
             break;
        case WStype_PING:
            Serial.println("[WSc] Received ping");
            // Pong is handled automatically by library
            break;
        case WStype_PONG:
            Serial.println("[WSc] Received pong");
            break;
        // Handle other cases if needed
        default:
            break;
    }
}

// --- Main Setup ---
void setup() {
    Serial.begin(115200);
    Serial.println("\nESP32 WebSocket Client");

    // --- Connect to Wi-Fi ---
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi...");
    int connect_tries = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++connect_tries > 20) {
            Serial.println("\nWiFi connection failed! Halting.");
            while(1); // Stop if cannot connect
        }
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    // --- --- --- --- --- ---

    // --- Configure WebSocket Client ---
    Serial.printf("Configuring WebSocket. Connecting to %s:%d%s\n",
                  websocket_server_host, websocket_server_port, websocket_path);
    // Server address, port and URL path
    webSocket.begin(websocket_server_host, websocket_server_port, websocket_path);

    // Register event handler function
    webSocket.onEvent(webSocketEvent);

    // Set reconnect interval if connection is lost (in milliseconds)
    webSocket.setReconnectInterval(5000); // try every 5 seconds

    // Optional: Enable Heartbeat (PING/PONG) to keep connection alive
    // webSocket.enableHeartbeat(15000, 3000, 2); // Send PING every 15s, timeout 3s, 2 retries

    Serial.println("WebSocket client configured. Waiting for connection...");
    // --- --- --- --- --- ---

    // I2S setup is removed for this connection-only example
}

// --- Main Loop ---
void loop() {
    // MUST call webSocket.loop() frequently!
    // This handles receiving messages, PING/PONG, reconnection etc.
    webSocket.loop();
    unsigned long currentTime = millis(); // <<<--- ADD THIS LINE

    // Send a text message periodically if connected
    if (wsConnected && (currentTime - lastTextSendTime >= textInterval)) {
        lastTextSendTime = currentTime;
        String message = "Hello from ESP32! Time: " + String(currentTime);
        Serial.printf("Sending TEXT: %s\n", message.c_str());
        if (!webSocket.sendTXT(message)) {
             Serial.println("WebSocket sendTXT failed!");
        }
    }
    delay(50);
}