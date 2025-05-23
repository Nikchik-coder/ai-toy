import asyncio
import websockets
import os
import wave
import time

# Configuration
HOST = '0.0.0.0'  
PORT = 8765
AUDIO_FILE_PATH = r'C:\Users\Doctor Who\Desktop\robottoy\audio_uploads\converted_audio.wav'

# Audio Parameters (MUST match the audio file AND ESP32 config)
EXPECTED_RATE = 16000
EXPECTED_WIDTH = 2  # 16-bit = 2 bytes
EXPECTED_CHANNELS = 1  # Mono

# Streaming Parameters
CHUNK_SIZE = 256 # Smaller chunk size that improved quality

# Pacing Calculation
try:
    BYTES_PER_SECOND = EXPECTED_RATE * EXPECTED_WIDTH * EXPECTED_CHANNELS
    if BYTES_PER_SECOND == 0:
        raise ValueError("Audio parameters result in zero bytes per second.")
    CHUNK_DURATION_S = CHUNK_SIZE / BYTES_PER_SECOND
except ZeroDivisionError:
     print("!!! ERROR: Audio parameters (Rate, Width, Channels) cannot be zero.")
     exit()
except ValueError as e:
     print(f"!!! ERROR: Invalid audio parameters: {e}")
     exit()

SLEEP_MULTIPLIER = 1.
SLEEP_DURATION = CHUNK_DURATION_S * SLEEP_MULTIPLIER

connected_clients = set()

async def send_audio_to_client(websocket, path):
    """Reads the audio file and streams it to a connected client."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"Attempting to send audio to {client_id}")

    try:
        if not os.path.exists(AUDIO_FILE_PATH):
             raise FileNotFoundError

        with wave.open(AUDIO_FILE_PATH, 'rb') as wf:
            # Verify Audio Format
            rate = wf.getframerate()
            width = wf.getsampwidth()
            channels = wf.getnchannels()
            n_frames = wf.getnframes()
            duration = n_frames / float(rate) if rate > 0 else 0
            print(f"Audio file properties: Rate={rate}, Width={width}, Channels={channels}, Frames={n_frames}, Duration={duration:.2f}s")

            if rate != EXPECTED_RATE or width != EXPECTED_WIDTH or channels != EXPECTED_CHANNELS:
                print(f"!!! ERROR: Audio file format mismatch! Expected {EXPECTED_RATE}Hz, {EXPECTED_WIDTH*8}-bit, {EXPECTED_CHANNELS}ch.")
                print(f"!!!        File is {rate}Hz, {width*8}-bit, {channels}ch.")
                await websocket.send("ERROR: Server audio format mismatch.")
                await websocket.close(code=1003, reason="Audio format mismatch")
                return

            print(f"Streaming '{os.path.basename(AUDIO_FILE_PATH)}' ({duration:.2f}s) to {client_id}...")
            print(f"Chunk size: {CHUNK_SIZE} bytes, Target sleep: {SLEEP_DURATION*1000:.2f} ms per chunk (Multiplier: {SLEEP_MULTIPLIER:.2f})")

            bytes_per_frame = width * channels
            if bytes_per_frame == 0:
                print("!!! ERROR: Bytes per frame is zero, invalid audio parameters.")
                await websocket.close(code=1011, reason="Invalid audio parameters")
                return
            frames_to_read = CHUNK_SIZE // bytes_per_frame
            if frames_to_read == 0:
                 print("!!! WARNING: CHUNK_SIZE is smaller than one frame, increasing frames_to_read to 1")
                 frames_to_read = 1

            total_bytes_sent = 0
            start_time = time.monotonic()
            expected_elapsed_time = 0.0

            while True:
                chunk_start_time = time.monotonic()
                data = wf.readframes(frames_to_read)
                if not data:
                    print(f"\nFinished streaming to {client_id}. Sent {total_bytes_sent} bytes.")
                    break

                try:
                    await websocket.send(data)
                    total_bytes_sent += len(data)

                    # Pacing Delay Logic
                    chunk_process_time = time.monotonic() - chunk_start_time
                    remaining_sleep = SLEEP_DURATION - chunk_process_time

                    if remaining_sleep > 0:
                        await asyncio.sleep(remaining_sleep)
                        expected_elapsed_time += SLEEP_DURATION
                    else:
                        expected_elapsed_time += SLEEP_DURATION
                        if total_bytes_sent % (CHUNK_SIZE * 100) == 0:
                             print(f"WARN: Chunk processing ({chunk_process_time*1000:.2f}ms) took longer than ideal interval ({SLEEP_DURATION*1000:.2f}ms). Skipping sleep.")

                except websockets.exceptions.ConnectionClosedOK:
                    print(f"\nClient {client_id} closed connection normally during streaming. Sent {total_bytes_sent} bytes.")
                    break
                except websockets.exceptions.ConnectionClosedError as close_err:
                    print(f"\nClient {client_id} connection closed abruptly during streaming: {close_err}. Sent {total_bytes_sent} bytes.")
                    break
                except Exception as send_err:
                    print(f"\nError sending data to {client_id}: {send_err}")
                    await websocket.close(code=1011, reason="Server send error")
                    break

            end_time = time.monotonic()
            streaming_duration = end_time - start_time
            print(f"Actual Streaming Duration: {streaming_duration:.2f}s")
            print(f"Target Audio Duration:     {duration:.2f}s")
            print(f"Expected Duration (sleeps):{expected_elapsed_time:.2f}s")


    except FileNotFoundError:
        print(f"!!! ERROR: Audio file not found at: {AUDIO_FILE_PATH}")
        try:
            await websocket.send("ERROR: Server could not find audio file.")
            await websocket.close(code=1011, reason="Server audio file missing")
        except: pass
    except wave.Error as wave_err:
         print(f"!!! ERROR: Could not read WAV file: {wave_err}")
         try:
            await websocket.send("ERROR: Server could not read WAV file.")
            await websocket.close(code=1011, reason="Server audio read error")
         except: pass
    except Exception as e:
        print(f"An error occurred sending audio to {client_id}: {type(e).__name__} - {e}")
        try:
            await websocket.send(f"ERROR: Server error during streaming: {type(e).__name__}")
            await websocket.close(code=1011, reason="Server streaming error")
        except: pass

async def connection_handler(websocket, path):
    """Handles new WebSocket connections."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"WS> Client connected: {client_id} (Path: {path})")
    connected_clients.add(websocket)

    send_task = None
    try:
        send_task = asyncio.create_task(send_audio_to_client(websocket, path))

        async for message in websocket:
            if isinstance(message, str):
                print(f"WS [{client_id}] >>> Received text: {message} (Ignoring)")
            else:
                 print(f"WS [{client_id}] >>> Received binary: {len(message)} bytes (Ignoring)")

    except websockets.exceptions.ConnectionClosedError as close_err:
        print(f"WS> Client {client_id} disconnected abruptly: {close_err}")
    except websockets.exceptions.ConnectionClosedOK:
        print(f"WS> Client {client_id} disconnected normally.")
    except Exception as e:
        print(f"WS> Error handling client {client_id}: {type(e).__name__} - {e}")
    finally:
        print(f"WS> Cleaning up connection for {client_id}")
        if websocket in connected_clients:
             connected_clients.remove(websocket)
        if send_task and not send_task.done():
             print(f"WS> Cancelling audio send task for {client_id}")
             send_task.cancel()
             try:
                 await send_task
             except asyncio.CancelledError:
                 print(f"WS> Audio task for {client_id} cancelled successfully.")
             except Exception as task_e:
                 print(f"WS> Error waiting for task cancellation for {client_id}: {task_e}")

async def start_server():
    """Starts the WebSocket server."""
    if not os.path.exists(AUDIO_FILE_PATH):
        print(f"!!! FATAL ERROR: Audio file not found: {AUDIO_FILE_PATH}")
        print("!!! Please ensure the path is correct and the file exists.")
        return

    print(f"Starting WebSocket audio streaming server on ws://{HOST}:{PORT}")
    print(f"Will stream file: {AUDIO_FILE_PATH}")
    print(f"Expected audio format: {EXPECTED_RATE}Hz, {EXPECTED_WIDTH*8}-bit, {EXPECTED_CHANNELS} channel(s)")
    print(f"Streaming with Chunk Size: {CHUNK_SIZE} bytes, Sleep Multiplier: {SLEEP_MULTIPLIER:.2f}")

    server_settings = {
        "ping_interval": 20,
        "ping_timeout": 15,
        "close_timeout": 10
    }

    try:
        async with websockets.serve(connection_handler, HOST, PORT, **server_settings):
            print(f"WebSocket server is running. Listening on ws://{HOST}:{PORT}. Press Ctrl+C to stop.")
            await asyncio.Future()
    except OSError as os_err:
         if "address already in use" in str(os_err).lower():
              print(f"!!! FATAL ERROR: Port {PORT} is already in use on {HOST}.")
              print("!!! Please stop the other process or change the PORT in this script.")
         else:
              print(f"!!! FATAL ERROR: Could not start server due to OS error: {os_err}")
    except Exception as start_err:
         print(f"!!! FATAL ERROR: Failed to start WebSocket server: {start_err}")


if __name__ == "__main__":
    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\nCtrl+C received. Shutting down server...")
    finally:
        print("Server shutdown sequence complete.")