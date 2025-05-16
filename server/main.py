# main_server.py
import threading
import time # Make sure time is imported
import traceback

import asyncio
import websockets
import os
import datetime
import traceback
import wave
import subprocess # For launching pipeline script
import sys
import time # For pacing TTS stream
from dotenv import load_dotenv



# --- TTS Specific Imports & Setup ---
try:
    from cartesia import Cartesia # Assuming Cartesia is used
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
    # Decide if you want to exit or just disable TTS
    # sys.exit(1)
    CARTESIA_CLIENT = None
else:
    try:
        CARTESIA_CLIENT = Cartesia(api_key=CARTESIA_API_KEY) # SYNC client for run_in_executor
    except Exception as e:
        print(f"!!! FATAL ERROR: Failed to initialize Cartesia client: {e}")
        CARTESIA_CLIENT = None
        # sys.exit(1)

# Cartesia Config (Make these configurable if needed)
# TTS_VOICE_ID = "da05e96d-ca10-4220-9042-d8acef654fa9"
TTS_VOICE_ID = os.getenv("TTS_VOICE_ID")
# TTS_VOICE_ID = "2b3bb17d-26b9-421f-b8ca-1dd92332279f"
TTS_MODEL_ID = "sonic-english"
TTS_SOURCE_RATE = 24000
TTS_SOURCE_ENCODING = "pcm_f32le"
# -----------------------------------

# --- WebSocket Server Configuration ---
HOST = '0.0.0.0'
PORT = 8765

# --- Expected ESP32 Audio Format (Both Input & TTS Output) ---
ESP32_RATE = 16000
ESP32_WIDTH = 2
ESP32_CHANNELS = 1
ESP32_BYTES_PER_SECOND = ESP32_RATE * ESP32_WIDTH * ESP32_CHANNELS
# --- Pacing for TTS stream ---
TTS_SLEEP_MULTIPLIER = 0.5# Adjust for smoother playback

# --- Pipeline Configuration ---
PIPELINE_SCRIPT_PATH = "server/pipeline_script.py" # Path to your pipeline script

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

# --- Keep track of connected clients and their tasks ---
# Store tuples: (websocket, active_monitor_task)
# Use a dictionary mapping websocket object to its monitor task
client_tasks = {}

# --- Directory to save received audio ---
AUDIO_SAVE_DIR = "received_audio_wav"
os.makedirs(AUDIO_SAVE_DIR, exist_ok=True)
print(f"Saving received audio files to: ./{AUDIO_SAVE_DIR}/")

# --- TTS Conversion Function (from your streaming script) ---
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
        # traceback.print_exc() # Can be noisy
        return b''

# --- TTS Streaming Function (Adapted) ---
_GENERATOR_SENTINEL = object() # Sentinel object

# --- TTS Streaming Function (REVISED) ---
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
        # Optionally send an error message back?
        # await websocket.send("ERROR: No text to speak.")
        return

    # --- FOR DEBUGGING: Hardcode text ---
    # text_to_speak = "This is a simple test sentence."
    # print(f"TTS> [{client_id}] DEBUG: Using hardcoded text: '{text_to_speak}'")
    # --- END DEBUG ---

    print(f"TTS> [{client_id}] Starting TTS stream (Text: '{text_to_speak[:60]}...')")
    loop = asyncio.get_running_loop()
    cartesia_ws = None # Store Cartesia WS connection obtained from sync client
    tts_generator = None # Store the sync generator

    try:
        # --- Define functions for run_in_executor ---
        def connect_and_send_cartesia_request_sync():
            # This runs in an executor thread
            thread_id = threading.get_ident()
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Connecting to Cartesia WS...")
            ws = CARTESIA_CLIENT.tts.websocket() # Use the global sync client
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Connected. Sending TTS request...")
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Voice={TTS_VOICE_ID}, Model={TTS_MODEL_ID}, Rate={TTS_SOURCE_RATE}")
            print(f"TTS EXEC> [{client_id}][T:{thread_id}] Text='{text_to_speak[:100]}...'") # Log text being sent
            cartesia_output_format = {
                "container": "raw", "encoding": TTS_SOURCE_ENCODING, "sample_rate": TTS_SOURCE_RATE
            }
            try:
                # THIS is the call that returns the synchronous generator
                gen = ws.send(
                    model_id=TTS_MODEL_ID,
                    transcript=text_to_speak,
                    voice={"id": TTS_VOICE_ID},
                    stream=True,
                    output_format=cartesia_output_format,
                )
                print(f"TTS EXEC> [{client_id}][T:{thread_id}] Cartesia request sent. Got generator: {type(gen)}")
                return ws, gen # Return the ws object AND the sync generator
            except Exception as req_err:
                 print(f"!!! TTS EXEC> [{client_id}][T:{thread_id}] ERROR during Cartesia ws.send(): {req_err}")
                 traceback.print_exc() # Log full traceback from executor thread
                 if ws: # Try to close WS if request fails
                     try: ws.close()
                     except Exception: pass
                 raise # Re-raise the exception

        def get_next_item_from_generator_sync(gen):
            # This runs in an executor thread
            thread_id = threading.get_ident()
            # print(f"TTS EXEC> [{client_id}][T:{thread_id}] Calling next() on generator...")
            try:
                item = next(gen) # Get the next item from the sync generator
                item_type = type(item)
                item_info = f"Type: {item_type}"
                if isinstance(item, dict) and 'audio' in item:
                    audio_data = item.get('audio')
                    item_info += f", Audio size: {len(audio_data) if audio_data else 0}"
                elif isinstance(item, bytes):
                     item_info += f", Bytes size: {len(item)}"
                # Limit logging frequency if too verbose
                # print(f"TTS EXEC> [{client_id}][T:{thread_id}] Got item from generator. {item_info}")
                return item
            except StopIteration:
                print(f"TTS EXEC> [{client_id}][T:{thread_id}] Generator StopIteration.")
                return _GENERATOR_SENTINEL
            except Exception as e:
                 print(f"!!! TTS EXEC> [{client_id}][T:{thread_id}] Error calling next() on generator: {e}")
                 traceback.print_exc() # Log full traceback from executor thread
                 raise # Re-raise

        # --- Setup Cartesia WebSocket Connection (in executor) ---
        print(f"TTS> [{client_id}] Awaiting Cartesia connection/request in executor...")
        try:
            # Run the sync connection function in the executor
            cartesia_ws, tts_generator = await loop.run_in_executor(
                None, connect_and_send_cartesia_request_sync
            )
            # Check if the generator was actually created
            if tts_generator is None:
                print(f"!!! TTS> [{client_id}] Failed to get generator from Cartesia (connect function might have raised).")
                # cartesia_ws might be None or already closed in the exec function
                return # Exit if setup failed

            print(f"TTS> [{client_id}] Cartesia WS connection seems ready (Generator type: {type(tts_generator)}). Starting stream processing loop.")

        except Exception as setup_err:
            # This catches errors raised from connect_and_send_cartesia_request_sync
            print(f"!!! TTS> [{client_id}] Error during Cartesia setup executor task: {setup_err}")
            # No need to close cartesia_ws here, as it likely failed before assignment or was closed in the exec function
            return # Exit if setup failed

        # --- Stream Audio Chunks ---
        total_bytes_sent = 0
        start_time = time.monotonic()

        while True:
            output_item = None
            try:
                # Get next chunk from the SYNCHRONOUS generator using the executor
                # print(f"TTS> [{client_id}] Awaiting next item from generator via executor...")
                output_item = await loop.run_in_executor(
                    None, get_next_item_from_generator_sync, tts_generator
                )

                if output_item is _GENERATOR_SENTINEL:
                    print(f"TTS> [{client_id}] Generator finished (sentinel received).")
                    break # Normal end of stream

                # print(f"TTS> [{client_id}] Received item type {type(output_item)} from executor.")

            except Exception as gen_exec_err:
                 # This catches errors raised from get_next_item_from_generator_sync
                 print(f"\n!!! TTS> [{client_id}] ERROR receiving/processing data via Cartesia generator executor: {gen_exec_err}")
                 # Don't print traceback here if it was already printed inside the executor function
                 break # Stop streaming on generator error

            # --- Process the received item ---
            source_buffer = None
            # Handle potential dictionary or direct bytes response (check Cartesia docs/behavior)
            if isinstance(output_item, dict) and 'audio' in output_item:
                source_buffer = output_item.get('audio')
            elif isinstance(output_item, bytes): # Should check actual type if not dict
                 print(f"WARN: TTS> [{client_id}] Received raw bytes, expected dict.")
                 source_buffer = output_item
            # Handle WebSocketTtsOutput objects
            elif hasattr(output_item, 'audio') and output_item.audio is not None:
                 source_buffer = output_item.audio
            # Add checks for other types if the API might send them (e.g., metadata, errors as dicts)
            else:
                 print(f"WARN: TTS> [{client_id}] Received unexpected item type from Cartesia generator: {type(output_item)}. Content: {str(output_item)[:100]}")

            if source_buffer:
                # --- Convert Audio ---
                esp32_buffer = convert_audio_chunk(source_buffer, TTS_SOURCE_RATE, ESP32_RATE)

                if not esp32_buffer:
                    # print(f"TTS> [{client_id}] Skip: Conversion resulted in empty buffer (Input size: {len(source_buffer)})")
                    continue # Skip empty buffers after conversion

                # --- Pacing and Sending ---
                buffer_len = len(esp32_buffer)
                chunk_duration_s = buffer_len / ESP32_BYTES_PER_SECOND
                target_send_time = time.monotonic() + chunk_duration_s * TTS_SLEEP_MULTIPLIER

                try:
                    # print(f"TTS> [{client_id}] Sending {buffer_len} bytes to WebSocket...")
                    send_start = time.monotonic()
                    await websocket.send(esp32_buffer) # Send to the specific client WebSocket
                    send_duration = time.monotonic() - send_start
                    total_bytes_sent += buffer_len

                    # Pacing sleep calculation
                    sleep_duration = target_send_time - time.monotonic()
                    if sleep_duration > 0.001: # Avoid tiny sleeps
                         # print(f"TTS> [{client_id}] Sleeping for {sleep_duration:.3f}s")
                         await asyncio.sleep(sleep_duration)

                except websockets.exceptions.ConnectionClosed:
                    print(f"\nTTS> [{client_id}] WebSocket closed while sending. Stopping TTS stream.")
                    break # Exit while loop
                except Exception as send_err:
                    print(f"\n!!! TTS> [{client_id}] Error sending TTS data to client WebSocket: {send_err}")
                    traceback.print_exc()
                    break # Exit while loop
            # else:
                # Already logged unexpected item type above
                # pass

        # --- Loop finished ---
        end_time = time.monotonic()
        duration = end_time - start_time
        print(f"TTS> Finished TTS stream processing loop for {client_id}. Sent {total_bytes_sent} bytes in {duration:.2f}s.") # This log should now have bytes > 0 if successful

    except Exception as e:
        # Catch any unexpected errors in the main async function body
        print(f"!!! TTS> [{client_id}] UNHANDLED ERROR in TTS streaming main try/except block: {type(e).__name__} - {e}")
        traceback.print_exc()
    finally:
        # --- Cleanup ---
        # Ensure Cartesia WebSocket (controlled by the sync client) is closed
        if cartesia_ws:
            print(f"TTS> [{client_id}] Cleaning up: Closing Cartesia WebSocket connection via executor...")
            try:
                 # Close synchronously within executor, as cartesia_ws belongs to the sync client
                 await loop.run_in_executor(None, cartesia_ws.close)
                 print(f"TTS> [{client_id}] Cartesia WebSocket closed.")
            except Exception as close_err:
                 print(f"!!! TTS> [{client_id}] Error closing Cartesia WebSocket during cleanup: {close_err}")
        tts_generator = None # Clear generator reference

# --- Subprocess Monitoring and TTS Trigger Task ---
async def monitor_pipeline_and_stream_tts(process: subprocess.Popen, websocket, client_id: str, input_wav_path: str):
    """Waits for pipeline subprocess, gets result, triggers TTS stream."""
    print(f"MONITOR> Monitoring pipeline process (PID: {process.pid}) for {client_id}...")
    llm_response = None
    stdout_data = None
    stderr_data = None

    try:
        # Asynchronously wait for process and capture output
        # This requires launching with asyncio.create_subprocess_exec, let's stick to polling Popen for now
        # stdout_data, stderr_data = await process.communicate() # This waits and blocks if used with Popen

        # --- Polling approach ---
        while process.poll() is None:
            await asyncio.sleep(0.5) # Check every half second

        return_code = process.returncode
        print(f"MONITOR> Pipeline process {process.pid} finished for {client_id} with code {return_code}.")

        # Try to read stdout/stderr if they were piped (Requires change in Popen call)
        # For simplicity now, assume script prints to main stdout/stderr

        # --- Alternative: Read result from a temporary file ---
        # If pipeline script wrote response to a known temp file:
        # response_file = input_wav_path + ".response"
        # if os.path.exists(response_file):
        #    with open(response_file, "r") as f: llm_response = f.read().strip()
        #    os.remove(response_file) # Clean up

        # --- Best approach with Popen: Capture stdout ---
        # We need to modify Popen call to capture stdout. Then read it here.
        # This part assumes the Popen call was modified as shown below.
        stdout_data, stderr_data = process.communicate() # Get output after process finished
        if stderr_data:
             print(f"MONITOR> Pipeline process {process.pid} stderr for {client_id}:\n{stderr_data.decode('utf-8', errors='replace')}")

        if return_code == 0 and stdout_data:
            # Properly decode stdout with explicit UTF-8 encoding
            stdout_text = stdout_data.decode('utf-8', errors='replace')
            print(f"MONITOR> Pipeline process {process.pid} stdout for {client_id}:\n{stdout_text[:200]}...") # Print beginning
            # Find the specific line with the response
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
        # Ensure process is terminated if error occurs during monitoring
        if process.poll() is None: process.terminate()

    # --- Trigger TTS ---
    if llm_response:
        print(f"MONITOR> LLM response: {llm_response}")
        if websocket and not websocket.closed:
            print(f"MONITOR> Triggering TTS stream back to {client_id}.")
            # Run TTS streaming as a new task so monitoring task can finish
            asyncio.create_task(stream_tts_response(websocket, client_id, llm_response))
        else:
            print(f"MONITOR> Client {client_id} disconnected before TTS could be triggered.")
    else:
        print(f"MONITOR> No valid LLM response found for {client_id}. Skipping TTS.")

    # # --- Cleanup input file? (Optional) ---
    try:
       print(f"MONITOR> Deleting input file: {input_wav_path}")
       os.remove(input_wav_path)
    except OSError as e:
       print(f"WARN> MONITOR> Failed to delete {input_wav_path}: {e}")
    # --------------------------------------

# --- Connection Handler ---
async def connection_handler(websocket, path):
    """Handles WebSocket connections FROM ESP32 devices."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"WS> Client connected: {client_id} (Path: {path})")
    # Add client to global dict, initially with no task
    client_tasks[websocket] = None

    is_recording = False
    audio_file: wave.Wave_write = None
    file_path = None
    bytes_received_this_session = 0
    # Removed active_pipeline_process from here, managed via client_tasks

    try:
        async for message in websocket:
            if isinstance(message, str):
                print(f"WS [{client_id}] >>> Received Text: {message}")
                if message == "START_RECORDING" and not is_recording:
                    # --- Cancel previous MONITOR task for this client if running ---
                    if websocket in client_tasks and client_tasks[websocket]:
                        monitor_task = client_tasks[websocket]
                        if not monitor_task.done():
                            print(f"WS [{client_id}] Cancelling previous pipeline monitoring task.")
                            monitor_task.cancel()
                            # Also terminate the associated subprocess if possible? Requires storing the process object too.
                            # This gets complicated quickly. Simplest is just cancel the monitor.
                        client_tasks[websocket] = None # Clear task handle
                    # ----------------------------------------------------------------

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

                    # --- !!! TRIGGER PIPELINE SUBPROCESS & MONITOR TASK HERE !!! ---
                    if successfully_saved_path:
                        if not os.path.exists(PIPELINE_SCRIPT_PATH):
                             print(f"!!! ERROR [{client_id}] Pipeline script not found at: {PIPELINE_SCRIPT_PATH}")
                        else:
                            print(f"WS [{client_id}] Launching pipeline subprocess for: {successfully_saved_path}")
                            try:
                                command = [sys.executable, PIPELINE_SCRIPT_PATH, successfully_saved_path]
                                print(f"WS [{client_id}] Running command: {' '.join(command)}")
                                # --- Launch subprocess with stdout/stderr piped ---
                                pipeline_process = subprocess.Popen(
                                    command,
                                    stdout=subprocess.PIPE, # Capture standard output
                                    stderr=subprocess.PIPE, # Capture standard error
                                    text=False,             # Use binary mode for output
                                    env={**os.environ, 'PYTHONIOENCODING': 'utf-8'} # Ensure Python uses UTF-8 encoding
                                )
                                print(f"WS [{client_id}] Pipeline process started (PID: {pipeline_process.pid})")

                                # --- Launch the monitoring task ---
                                monitor_task = asyncio.create_task(
                                    monitor_pipeline_and_stream_tts(pipeline_process, websocket, client_id, successfully_saved_path)
                                )
                                # Store the task handle associated with this client
                                client_tasks[websocket] = monitor_task
                                # --------------------------------

                            except Exception as sub_err:
                                 print(f"!!! ERROR [{client_id}] Failed to launch subprocess: {sub_err}")
                                 traceback.print_exc()

                    elif message == "STOP_RECORDING":
                         print(f"WARN [{client_id}] Recording stopped, but no valid audio file was saved. Skipping pipeline.")
                    # ---------------------------------------------

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
        # --- Cancel monitor task for this client upon disconnect ---
        if websocket in client_tasks:
            monitor_task = client_tasks.pop(websocket) # Remove and get task
            if monitor_task and not monitor_task.done():
                print(f"WS> Cancelling pipeline monitoring task for disconnected client {client_id}.")
                monitor_task.cancel()
                # Optionally terminate the subprocess if the monitor hasn't finished?
                # This requires storing the process object with the task, adding complexity.
        # ---------------------------------------------------------
        if audio_file: # Ensure file is closed if connection drops mid-recording
            print(f"WS [{client_id}] Closing WAV file due to disconnection: {file_path}")
            try: audio_file.close()
            except wave.Error as e: print(f"!!! ERROR closing WAV file {file_path}: {e}")


# --- Start Server ---
async def start_server():
    """Starts the WebSocket server."""
    # (No changes needed here)
    print(f"Starting WebSocket server on ws://{HOST}:{PORT}")
    print("Ready to receive audio data from ESP32 clients and save as WAV.")
    server_settings = {
        "ping_interval": 20, "ping_timeout": 15, "close_timeout": 10, "max_size": 1024 * 1024
    }
    try:
        async with websockets.serve(connection_handler, HOST, PORT, **server_settings):
            print(f"WebSocket server listening. Press Ctrl+C to stop.")
            await asyncio.Future() # Run forever
    except OSError as os_err:
        if "address already in use" in str(os_err).lower(): print(f"!!! FATAL ERROR: Port {PORT} is already in use on {HOST}.")
        else: print(f"!!! FATAL ERROR: Could not start server: {os_err}")
    except Exception as start_err: print(f"!!! FATAL ERROR: Failed to start WebSocket server: {start_err}")


# --- Main execution block ---
if __name__ == "__main__":
    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\nCtrl+C received. Shutting down server...")
    finally:
        print("Server shutdown sequence complete.")
        # Optional: Clean up any remaining tasks or processes if needed
        # This can be complex to do reliably on shutdown.
