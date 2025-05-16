import asyncio
import websockets
import os
import datetime
import traceback
import wave

# --- WebSocket Server Configuration (for ESP32) ---
HOST = '0.0.0.0'
PORT = 8765

# --- Expected ESP32 Audio Format (Used for WAV header) ---
ESP32_RATE = 16000
ESP32_WIDTH = 2
ESP32_CHANNELS = 1

print(f"--- Configuration ---")
print(f"WebSocket Server: ws://{HOST}:{PORT}")
print(f"Expected Audio Format from ESP32: {ESP32_RATE} Hz, {ESP32_WIDTH*8}-bit Signed PCM, {ESP32_CHANNELS}-channel (Mono)")
print(f"Saving audio as .wav files.")
print(f"---")

# --- Keep track of connected clients ---
connected_clients = set()

# --- Directory to save received audio ---
AUDIO_SAVE_DIR = "received_audio_wav"
os.makedirs(AUDIO_SAVE_DIR, exist_ok=True)
print(f"Saving received audio files to: ./{AUDIO_SAVE_DIR}/")

# --- Connection Handler ---
async def connection_handler(websocket, path):
    """Handles WebSocket connections FROM ESP32 devices."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"WS> Client connected: {client_id} (Path: {path})")
    connected_clients.add(websocket)

    is_recording = False
    audio_file: wave.Wave_write = None
    file_path = None
    bytes_received_this_session = 0

    try:
        async for message in websocket:
            if isinstance(message, str):
                print(f"WS [{client_id}] >>> Received Text: {message}")
                if message == "START_RECORDING" and not is_recording:
                    print(f"WS [{client_id}] --- Started Recording ---")
                    is_recording = True
                    bytes_received_this_session = 0
                    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
                    safe_client_id = client_id.replace(":", "_").replace(".","_")
                    filename = f"esp32_{safe_client_id}_{timestamp}.wav"
                    file_path = os.path.join(AUDIO_SAVE_DIR, filename)
                    try:
                        audio_file = wave.open(file_path, 'wb')
                        audio_file.setnchannels(ESP32_CHANNELS)
                        audio_file.setsampwidth(ESP32_WIDTH)
                        audio_file.setframerate(ESP32_RATE)
                        print(f"WS [{client_id}] Saving audio to: {file_path}")
                    except IOError as e:
                        print(f"!!! ERROR [{client_id}] Cannot open WAV file {file_path}: {e}")
                        is_recording = False
                        if audio_file: audio_file.close()
                        audio_file = None
                    except wave.Error as e:
                         print(f"!!! ERROR [{client_id}] WAV configuration error for {file_path}: {e}")
                         is_recording = False
                         if audio_file: audio_file.close()
                         audio_file = None


                elif (message == "STOP_RECORDING" or message == "STOP_RECORDING_ERROR") and is_recording:
                    log_prefix = "--- Stopped Recording ---" if message == "STOP_RECORDING" else "!!! Received STOP_RECORDING_ERROR from client !!!"
                    print(f"WS [{client_id}] {log_prefix}")
                    is_recording = False
                    if audio_file:
                        try:
                            audio_file.close()
                            print(f"WS [{client_id}] Closed WAV file: {file_path}. Total audio bytes received: {bytes_received_this_session}")
                        except wave.Error as e:
                            print(f"!!! ERROR [{client_id}] Error closing WAV file {file_path}: {e}")
                        audio_file = None
                        file_path = None

            elif isinstance(message, bytes):
                if is_recording and audio_file:
                    try:
                        audio_file.writeframesraw(message)
                        bytes_received_this_session += len(message)
                    except wave.Error as e:
                        print(f"!!! ERROR [{client_id}] Cannot write WAV frames to {file_path}: {e}")
                        is_recording = False
                        if audio_file: audio_file.close()
                        audio_file = None
                    except Exception as e:
                         print(f"!!! ERROR [{client_id}] Cannot write to file {file_path}: {e}")
                         is_recording = False
                         if audio_file: audio_file.close()
                         audio_file = None

                elif is_recording and not audio_file:
                     print(f"WARN [{client_id}] Received binary data while recording flag is set, but no file is open!")

    except websockets.exceptions.ConnectionClosedError as close_err:
        print(f"WS> Client {client_id} disconnected abruptly: {close_err}")
    except websockets.exceptions.ConnectionClosedOK:
        print(f"WS> Client {client_id} disconnected normally.")
    except Exception as e:
        print(f"WS> Error handling client {client_id}: {type(e).__name__} - {e}")
        traceback.print_exc()
    finally:
        print(f"WS> Cleaning up connection for client {client_id}")
        if websocket in connected_clients:
            connected_clients.remove(websocket)
        if audio_file:
            print(f"WS [{client_id}] Closing WAV file due to disconnection: {file_path}")
            try:
                audio_file.close()
                print(f"WS [{client_id}] WAV File closed. Total audio bytes received in session: {bytes_received_this_session}")
            except wave.Error as e:
                 print(f"!!! ERROR [{client_id}] Error closing WAV file {file_path} during cleanup: {e}")


# --- Start Server ---
async def start_server():
    """Starts the WebSocket server to listen for ESP32 clients."""
    print(f"Starting WebSocket server on ws://{HOST}:{PORT}")
    print("Ready to receive audio data from ESP32 clients and save as WAV.")
    server_settings = {
        "ping_interval": 20,
        "ping_timeout": 15,
        "close_timeout": 10,
        "max_size": 1024 * 1024
    }
    try:
        async with websockets.serve(connection_handler, HOST, PORT, **server_settings):
            print(f"WebSocket server listening. Press Ctrl+C to stop.")
            await asyncio.Future()
    except OSError as os_err:
        if "address already in use" in str(os_err).lower():
            print(f"!!! FATAL ERROR: Port {PORT} is already in use on {HOST}.")
        else:
            print(f"!!! FATAL ERROR: Could not start server: {os_err}")
    except Exception as start_err:
        print(f"!!! FATAL ERROR: Failed to start WebSocket server: {start_err}")
        traceback.print_exc()

# --- Main execution block ---
if __name__ == "__main__":
    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\nCtrl+C received. Shutting down server...")
    finally:
        print("Server shutdown sequence complete.")