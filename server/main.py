# main_server.py
import threading
import time
import traceback

import asyncio
import websockets
import os
import datetime
import traceback
import wave
import subprocess
import sys
import time
from dotenv import load_dotenv

try:
    from cartesia import Cartesia
    import numpy as np
    from scipy.signal import resample
except ImportError as e:
    print(f"!!! ERROR: Missing TTS dependencies (cartesia, numpy, scipy): {e}")
    print("!!! Please install them: pip install cartesia-api numpy scipy")
    sys.exit(1)

load_dotenv()
CARTESIA_API_KEY = os.environ.get("CARTESIA_API_KEY")
if not CARTESIA_API_KEY:
    print("!!! FATAL ERROR: CARTESIA_API_KEY not found in environment variables or .env file.")
    CARTESIA_CLIENT = None
else:
    try:
        CARTESIA_CLIENT = Cartesia(api_key=CARTESIA_API_KEY)
    except Exception as e:
        print(f"!!! FATAL ERROR: Failed to initialize Cartesia client: {e}")
        CARTESIA_CLIENT = None

TTS_VOICE_ID = os.getenv("TTS_VOICE_ID")
TTS_MODEL_ID = "sonic-english"
TTS_SOURCE_RATE = 24000
TTS_SOURCE_ENCODING = "pcm_f32le"

# --- WebSocket Server Configuration ---
HOST = '0.0.0.0'
PORT = 8765

# --- Expected ESP32 Audio Format (Both Input & TTS Output) ---
ESP32_RATE = 16000
ESP32_WIDTH = 2
ESP32_CHANNELS = 1
ESP32_BYTES_PER_SECOND = ESP32_RATE * ESP32_WIDTH * ESP32_CHANNELS
TTS_SLEEP_MULTIPLIER = 0.5

# --- Pipeline Configuration ---
PIPELINE_SCRIPT_PATH = "server/pipeline_script.py"

print(f"--- Configuration ---")
print(f"WebSocket Server: ws://{HOST}:{PORT}")
print(f"Expected ESP32 Audio Format: {ESP32_RATE} Hz, {ESP32_WIDTH*8}-bit PCM, {ESP32_CHANNELS}-ch")
print(f"Saving received audio as .wav files.")
print(f"Triggering pipeline script: {PIPELINE_SCRIPT_PATH}")
if CARTESIA_CLIENT:
    print(f"TTS Enabled (Cartesia Voice: {TTS_VOICE_ID})")
else:
    print("!!! TTS Disabled (Cartesia client init failed or key missing)")
print(f"---")

client_tasks = {}

AUDIO_SAVE_DIR = "received_audio_wav"
os.makedirs(AUDIO_SAVE_DIR, exist_ok=True)
print(f"Saving received audio files to: ./{AUDIO_SAVE_DIR}/")

def convert_audio_chunk(buffer_f32le, source_rate, target_rate):
    """Converts a float32 little-endian chunk to int16 little-endian and resamples."""
    try:
        float32_array = np.frombuffer(buffer_f32le, dtype=np.float32)
        num_samples_in = len(float32_array)
        if num_samples_in == 0: return b''

        if source_rate != target_rate:
            num_samples_out = int(np.round(num_samples_in * target_rate / source_rate))
            if num_samples_out <= 0: return b''
            resampled_array = resample(float32_array, num_samples_out)
        else:
            resampled_array = float32_array

        int16_array = np.clip(resampled_array * 32767, -32768, 32767).astype(np.int16)
        return int16_array.tobytes()
    except Exception as conv_err:
        print(f"!!! ERROR during audio conversion: {conv_err}")
        return b''

_GENERATOR_SENTINEL = object()

async def stream_tts_response(websocket, client_id: str, text_to_speak: str):
    """Generates TTS using Cartesia and streams it to the specified websocket."""
    if not CARTESIA_CLIENT:
        print(f"TTS> [{client_id}] Cannot stream: Cartesia client not initialized.")
        return
    if not websocket or websocket.closed:
        print(f"TTS> [{client_id}] Cannot stream: WebSocket is closed.")
        return
    if not text_to_speak:
        print(f"TTS> [{client_id}] Cannot stream: Input text is empty.")
        return

    print(f"TTS> [{client_id}] Starting TTS stream (Text: '{text_to_speak[:60]}...')")
    loop = asyncio.get_running_loop()
    cartesia_ws = None
    tts_generator = None

    try:
        def connect_and_send_cartesia_request_sync():
            thread_id = threading.get_ident()
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Connecting to Cartesia WS...")
            ws = CARTESIA_CLIENT.tts.websocket()
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Connected. Sending TTS request...")
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Voice={TTS_VOICE_ID}, Model={TTS_MODEL_ID}, Rate={TTS_SOURCE_RATE}")
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Text='{text_to_speak[:100]}...'")
            cartesia_output_format = {
                "container": "raw", "encoding": TTS_SOURCE_ENCODING, "sample_rate": TTS_SOURCE_RATE
            }
            try:
                gen = ws.send(
                    model_id=TTS_MODEL_ID,
                    transcript=text_to_speak,
                    voice={"id": TTS_VOICE_ID},
                    stream=True,
                    output_format=cartesia_output_format,
                )
                print(f"TTS EXEC> [{client_id}][T:{thread_id}] Cartesia request sent. Got generator: {type(gen)}")
                return ws, gen
            except Exception as req_err:
                 print(f"!!! TTS EXEC> [{client_id}][T:{thread_id}] ERROR during Cartesia ws.send(): {req_err}")
                 traceback.print_exc()
                 if ws:
                     try: ws.close()
                     except Exception: pass
                 raise

        def get_next_item_from_generator_sync(gen):
            thread_id = threading.get_ident()
            try:
                item = next(gen)
                item_type = type(item)
                item_info = f"Type: {item_type}"
                if isinstance(item, dict) and 'audio' in item:
                    audio_data = item.get('audio')
                    item_info += f", Audio size: {len(audio_data) if audio_data else 0}"
                elif isinstance(item, bytes):
                     item_info += f", Bytes size: {len(item)}"
                return item
            except StopIteration:
                print(f"TTS EXEC> [{client_id}][T:{thread_id}] Generator StopIteration.")
                return _GENERATOR_SENTINEL
            except Exception as e:
                 print(f"!!! TTS EXEC> [{client_id}][T:{thread_id}] Error calling next() on generator: {e}")
                 traceback.print_exc()
                 raise

        print(f"TTS> [{client_id}] Awaiting Cartesia connection/request in executor...")
        try:
            cartesia_ws, tts_generator = await loop.run_in_executor(
                None, connect_and_send_cartesia_request_sync
            )
            if tts_generator is None:
                print(f"!!! TTS> [{client_id}] Failed to get generator from Cartesia (connect function might have raised).")
                return

            print(f"TTS> [{client_id}] Cartesia WS connection seems ready (Generator type: {type(tts_generator)}). Starting stream processing loop.")

        except Exception as setup_err:
            print(f"!!! TTS> [{client_id}] Error during Cartesia setup executor task: {setup_err}")
            return

        total_bytes_sent = 0
        start_time = time.monotonic()

        while True:
            output_item = None
            try:
                output_item = await loop.run_in_executor(
                    None, get_next_item_from_generator_sync, tts_generator
                )

                if output_item is _GENERATOR_SENTINEL:
                    print(f"TTS> [{client_id}] Generator finished (sentinel received).")
                    break

            except Exception as gen_exec_err:
                 print(f"\n!!! TTS> [{client_id}] ERROR receiving/processing data via Cartesia generator executor: {gen_exec_err}")
                 break

            source_buffer = None
            if isinstance(output_item, dict) and 'audio' in output_item:
                source_buffer = output_item.get('audio')
            elif isinstance(output_item, bytes):
                 print(f"WARN: TTS> [{client_id}] Received raw bytes, expected dict.")
                 source_buffer = output_item
            elif hasattr(output_item, 'audio') and output_item.audio is not None:
                 source_buffer = output_item.audio
            else:
                 print(f"WARN: TTS> [{client_id}] Received unexpected item type from Cartesia generator: {type(output_item)}. Content: {str(output_item)[:100]}")

            if source_buffer:
                esp32_buffer = convert_audio_chunk(source_buffer, TTS_SOURCE_RATE, ESP32_RATE)

                if not esp32_buffer:
                    continue

                buffer_len = len(esp32_buffer)
                chunk_duration_s = buffer_len / ESP32_BYTES_PER_SECOND
                target_send_time = time.monotonic() + chunk_duration_s * TTS_SLEEP_MULTIPLIER

                try:
                    send_start = time.monotonic()
                    await websocket.send(esp32_buffer)
                    send_duration = time.monotonic() - send_start
                    total_bytes_sent += buffer_len

                    sleep_duration = target_send_time - time.monotonic()
                    if sleep_duration > 0.001:
                         await asyncio.sleep(sleep_duration)

                except websockets.exceptions.ConnectionClosed:
                    print(f"\nTTS> [{client_id}] WebSocket closed while sending. Stopping TTS stream.")
                    break
                except Exception as send_err:
                    print(f"\n!!! TTS> [{client_id}] Error sending TTS data to client WebSocket: {send_err}")
                    traceback.print_exc()
                    break

        end_time = time.monotonic()
        duration = end_time - start_time
        print(f"TTS> Finished TTS stream processing loop for {client_id}. Sent {total_bytes_sent} bytes in {duration:.2f}s.")

    except Exception as e:
        print(f"!!! TTS> [{client_id}] UNHANDLED ERROR in TTS streaming main try/except block: {type(e).__name__} - {e}")
        traceback.print_exc()
    finally:
        if cartesia_ws:
            print(f"TTS> [{client_id}] Cleaning up: Closing Cartesia WebSocket connection via executor...")
            try:
                 await loop.run_in_executor(None, cartesia_ws.close)
                 print(f"TTS> [{client_id}] Cartesia WebSocket closed.")
            except Exception as close_err:
                 print(f"!!! TTS> [{client_id}] Error closing Cartesia WebSocket during cleanup: {close_err}")
        tts_generator = None

async def monitor_pipeline_and_stream_tts(process: subprocess.Popen, websocket, client_id: str, input_wav_path: str):
    """Waits for pipeline subprocess, gets result, triggers TTS stream."""
    print(f"MONITOR> Monitoring pipeline process (PID: {process.pid}) for {client_id}...")
    llm_response = None
    stdout_data = None
    stderr_data = None

    try:
        while process.poll() is None:
            await asyncio.sleep(0.5)

        return_code = process.returncode
        print(f"MONITOR> Pipeline process {process.pid} finished for {client_id} with code {return_code}.")

        stdout_data, stderr_data = process.communicate()
        if stderr_data:
             print(f"MONITOR> Pipeline process {process.pid} stderr for {client_id}:\n{stderr_data.decode('utf-8', errors='replace')}")

        if return_code == 0 and stdout_data:
            stdout_text = stdout_data.decode('utf-8', errors='replace')
            print(f"MONITOR> Pipeline process {process.pid} stdout for {client_id}:\n{stdout_text[:200]}...")
            prefix = "FINAL_LLM_RESPONSE:"
            for line in stdout_text.splitlines():
                if line.startswith(prefix):
                    llm_response = line[len(prefix):].strip()
                    print(f"MONITOR> Found LLM response for {client_id}: '{llm_response[:60]}...'")
                    break
            if not llm_response:
                 print(f"WARN> MONITOR> Pipeline process {process.pid} finished successfully but '{prefix}' not found in stdout for {client_id}.")

        elif return_code != 0:
             print(f"!!! MONITOR> Pipeline process {process.pid} failed for {client_id} (Code: {return_code}).")

    except Exception as e:
        print(f"!!! MONITOR> Error monitoring pipeline process {process.pid} for {client_id}: {e}")
        traceback.print_exc()
        if process.poll() is None: process.terminate()

    if llm_response:
        print(f"MONITOR> LLM response: {llm_response}")
        if websocket and not websocket.closed:
            print(f"MONITOR> Triggering TTS stream back to {client_id}.")
            asyncio.create_task(stream_tts_response(websocket, client_id, llm_response))
        else:
            print(f"MONITOR> Client {client_id} disconnected before TTS could be triggered.")
    else:
        print(f"MONITOR> No valid LLM response found for {client_id}. Skipping TTS.")

    try:
       print(f"MONITOR> Deleting input file: {input_wav_path}")
       os.remove(input_wav_path)
    except OSError as e:
       print(f"WARN> MONITOR> Failed to delete {input_wav_path}: {e}")

async def connection_handler(websocket, path):
    """Handles WebSocket connections FROM ESP32 devices."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"WS> Client connected: {client_id} (Path: {path})")
    client_tasks[websocket] = None

    is_recording = False
    audio_file: wave.Wave_write = None
    file_path = None
    bytes_received_this_session = 0

    try:
        async for message in websocket:
            if isinstance(message, str):
                print(f"WS [{client_id}] >>> Received Text: {message}")
                if message == "START_RECORDING" and not is_recording:
                    if websocket in client_tasks and client_tasks[websocket]:
                        monitor_task = client_tasks[websocket]
                        if not monitor_task.done():
                            print(f"WS [{client_id}] Cancelling previous pipeline monitoring task.")
                            monitor_task.cancel()
                        client_tasks[websocket] = None

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
                    except Exception as e:
                        print(f"!!! ERROR [{client_id}] Cannot open/configure WAV file {file_path}: {e}")
                        is_recording = False
                        if audio_file: audio_file.close()
                        audio_file = None
                        file_path = None

                elif (message == "STOP_RECORDING" or message == "STOP_RECORDING_ERROR") and is_recording:
                    log_prefix = "--- Stopped Recording ---" if message == "STOP_RECORDING" else "!!! Received STOP_RECORDING_ERROR from client !!!"
                    print(f"WS [{client_id}] {log_prefix}")
                    is_recording = False
                    successfully_saved_path = None

                    if audio_file:
                        current_file_path = file_path
                        try:
                            audio_file.close()
                            print(f"WS [{client_id}] Closed WAV file: {current_file_path}. Total audio bytes received: {bytes_received_this_session}")
                            if message == "STOP_RECORDING":
                                successfully_saved_path = current_file_path
                        except wave.Error as e:
                            print(f"!!! ERROR [{client_id}] Error closing WAV file {current_file_path}: {e}")
                        finally:
                             audio_file = None
                             file_path = None

                    if successfully_saved_path:
                        if not os.path.exists(PIPELINE_SCRIPT_PATH):
                             print(f"!!! ERROR [{client_id}] Pipeline script not found at: {PIPELINE_SCRIPT_PATH}")
                        else:
                            print(f"WS [{client_id}] Launching pipeline subprocess for: {successfully_saved_path}")
                            try:
                                command = [sys.executable, PIPELINE_SCRIPT_PATH, successfully_saved_path]
                                print(f"WS [{client_id}] Running command: {' '.join(command)}")
                                pipeline_process = subprocess.Popen(
                                    command,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,
                                    text=False,
                                    env={**os.environ, 'PYTHONIOENCODING': 'utf-8'}
                                )
                                print(f"WS [{client_id}] Pipeline process started (PID: {pipeline_process.pid})")

                                monitor_task = asyncio.create_task(
                                    monitor_pipeline_and_stream_tts(pipeline_process, websocket, client_id, successfully_saved_path)
                                )
                                client_tasks[websocket] = monitor_task

                            except Exception as sub_err:
                                 print(f"!!! ERROR [{client_id}] Failed to launch subprocess: {sub_err}")
                                 traceback.print_exc()

                    elif message == "STOP_RECORDING":
                         print(f"WARN [{client_id}] Recording stopped, but no valid audio file was saved. Skipping pipeline.")

            elif isinstance(message, bytes):
                 if is_recording and audio_file:
                    try:
                        audio_file.writeframesraw(message)
                        bytes_received_this_session += len(message)
                    except Exception as e:
                        print(f"!!! ERROR [{client_id}] Cannot write WAV frames to {file_path}: {e}")
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
        if websocket in client_tasks:
            monitor_task = client_tasks.pop(websocket)
            if monitor_task and not monitor_task.done():
                print(f"WS> Cancelling pipeline monitoring task for disconnected client {client_id}.")
                monitor_task.cancel()
        if audio_file:
            print(f"WS [{client_id}] Closing WAV file due to disconnection: {file_path}")
            try: audio_file.close()
            except wave.Error as e: print(f"!!! ERROR closing WAV file {file_path}: {e}")

async def start_server():
    """Starts the WebSocket server."""
    print(f"Starting WebSocket server on ws://{HOST}:{PORT}")
    print("Ready to receive audio data from ESP32 clients and save as WAV.")
    server_settings = {
        "ping_interval": 20, "ping_timeout": 15, "close_timeout": 10, "max_size": 1024 * 1024
    }
    try:
        async with websockets.serve(connection_handler, HOST, PORT, **server_settings):
            print(f"WebSocket server listening. Press Ctrl+C to stop.")
            await asyncio.Future()
    except OSError as os_err:
        if "address already in use" in str(os_err).lower(): print(f"!!! FATAL ERROR: Port {PORT} is already in use on {HOST}.")
        else: print(f"!!! FATAL ERROR: Could not start server: {os_err}")
    except Exception as start_err: print(f"!!! FATAL ERROR: Failed to start WebSocket server: {start_err}")

if __name__ == "__main__":
    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\nCtrl+C received. Shutting down server...")
    finally:
        print("Server shutdown sequence complete.")
