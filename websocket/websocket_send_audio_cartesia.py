import asyncio
import websockets
import os
import time
from dotenv import load_dotenv
# Use the SYNC client to access the .websocket() method based on Cartesia's example
from cartesia import Cartesia
import traceback
import concurrent.futures # For running sync code in async
import numpy as np               # For numerical processing
from scipy.signal import resample  # For audio resampling

# --- Load Environment Variables ---
load_dotenv()
CARTESIA_API_KEY = os.environ.get("CARTESIA_API_KEY")
if not CARTESIA_API_KEY:
    print("!!! FATAL ERROR: CARTESIA_API_KEY not found in environment variables or .env file.")
    exit()

# --- Cartesia Configuration ---
# Initialize the SYNC client
try:
    client = Cartesia(api_key=CARTESIA_API_KEY)
except Exception as e:
    print(f"!!! FATAL ERROR: Failed to initialize Cartesia client: {e}")
    exit()

# <<<--- CHOOSE YOUR VOICE ID ---<<<
VOICE_ID = "a38e4e85-e815-43ab-acf1-907c4688dd6c" # The one that worked in the sync test
# <<<--- SET THE TEXT TO SPEAK ---<<<
# TRANSCRIPT = "Hello ESP32! This audio is being streamed directly from Cartesia's Text-to-Speech WebSocket endpoint, with resampling for potentially higher quality."
TRANSCRIPT = "Thinking: Given my goals to be a Web3 influencer and provide informative content to the community, I should share the crafted message about the latest Web3 trends in 2025. This aligns with my previous actions of researching and crafting informative content. The 'post_to_telegram' tool is suitable for this purpose, despite its high energy cost, as it directly contributes to my goals"
# Choose the appropriate model for your voice
MODEL_ID = "sonic-english" # Or sonic-2, etc.

# --- WebSocket Server Configuration (for ESP32) ---
HOST = '0.0.0.0'  # Listen on all available interfaces
PORT = 8765

# --- ESP32 Audio Format Configuration (TARGET) ---
# THIS MUST MATCH YOUR ESP32 I2S CONFIGURATION
ESP32_RATE = 16000
ESP32_WIDTH = 2  # 16-bit = 2 bytes
ESP32_CHANNELS = 1  # Mono
ESP32_BYTES_PER_SECOND = ESP32_RATE * ESP32_WIDTH * ESP32_CHANNELS

# --- Pacing Calculation ---
# Based on TARGET ESP32 format
try:
    if ESP32_BYTES_PER_SECOND == 0: raise ValueError("ESP32 BPS is zero.")
except ValueError as e: print(f"!!! ERROR: {e}"); exit()
SLEEP_MULTIPLIER = 1.0 # Tune this value (start around 1.0-1.1)

# --- Cartesia Output Format Request (SOURCE - Higher Quality) ---
# Request a potentially higher quality format from Cartesia
# Common high-quality defaults: 24000 Hz or 44100 Hz, float32
SOURCE_RATE = 24000 # Try 24000 or 44100 - Check Cartesia Docs for supported rates
SOURCE_ENCODING = "pcm_f32le" # float32 little-endian often native/high quality
SOURCE_WIDTH = 4 # Bytes for float32
SOURCE_CHANNELS = 1 # Assuming Cartesia provides mono for this encoding

cartesia_output_format = {
    "container": "raw",
    "encoding": SOURCE_ENCODING,
    "sample_rate": SOURCE_RATE
}
print(f"Requesting {SOURCE_RATE}Hz {SOURCE_ENCODING} from Cartesia.")
print(f"Will convert to {ESP32_RATE}Hz pcm_s16le for ESP32.")

# --- Keep track of connected ESP32 clients ---
connected_clients = set()
_GENERATOR_SENTINEL = object()

# --- Conversion Function ---
def convert_audio_chunk(buffer_f32le, source_rate, target_rate):
    """Converts a float32 little-endian chunk to int16 little-endian and resamples."""
    try:
        # 1. Convert bytes to numpy float32 array
        # Assumes little-endian float32 from Cartesia based on 'pcm_f32le'
        float32_array = np.frombuffer(buffer_f32le, dtype=np.float32)

        # Ensure we actually got some samples
        num_samples_in = len(float32_array)
        if num_samples_in == 0:
            return b'' # Return empty bytes if input buffer was empty

        # 2. Resample if rates differ
        if source_rate != target_rate:
            # Calculate precise number of output samples
            num_samples_out = int(np.round(num_samples_in * target_rate / source_rate))
            if num_samples_out <= 0: # Handle case where output length rounds to 0
                return b''
            # Use scipy.signal.resample for resampling
            resampled_array = resample(float32_array, num_samples_out)
        else:
            resampled_array = float32_array # No resampling needed

        # 3. Convert float32 [-1.0, 1.0] to int16 [-32768, 32767]
        # Multiply by max int16 value and ensure clipping
        int16_array = np.clip(resampled_array * 32767, -32768, 32767).astype(np.int16)

        # 4. Convert numpy int16 array back to bytes (system's default endianness, usually LE)
        output_buffer = int16_array.tobytes()
        return output_buffer

    except Exception as conv_err:
        print(f"!!! ERROR during audio conversion: {conv_err}")
        traceback.print_exc()
        return b'' # Return empty bytes on conversion error


async def stream_cartesia_to_client(esp32_websocket, path):
    """Connects to Cartesia TTS WS, gets audio, converts, and forwards to ESP32 WS."""
    client_id = f"{esp32_websocket.remote_address[0]}:{esp32_websocket.remote_address[1]}"
    print(f"Attempting Cartesia WS stream for ESP32 client: {client_id}")
    loop = asyncio.get_running_loop()
    cartesia_ws = None

    try:
        # --- Setup Cartesia WebSocket Connection ---
        def connect_and_send_cartesia_request():
            print("Connecting to Cartesia TTS WebSocket...")
            ws = client.tts.websocket()
            print("Connected to Cartesia TTS WS.")
            print(f"Sending TTS request (Requesting {SOURCE_RATE}Hz {SOURCE_ENCODING})...")
            tts_generator = ws.send(
                model_id=MODEL_ID,
                transcript=TRANSCRIPT,
                voice_id=VOICE_ID,
                stream=True,
                output_format=cartesia_output_format, # Request SOURCE format
            )
            print("Cartesia TTS request sent. Generator obtained.")
            return ws, tts_generator

        cartesia_ws, tts_generator = await loop.run_in_executor(
            None, connect_and_send_cartesia_request
        )
        print("Cartesia WS connection established and generator ready.")

        # --- Define Helper ---
        def get_next_item_from_generator(gen):
            try: return next(gen)
            except StopIteration: return _GENERATOR_SENTINEL
            except Exception as e: print(f"Error in get_next_item: {e}"); raise

        # --- Stream Audio Chunks ---
        total_bytes_sent_to_esp = 0
        start_time = time.monotonic()
        expected_elapsed_time = 0.0
        total_conversion_time = 0.0
        total_sleep_time = 0.0

        while True:
            output = None
            try:
                # Get the next chunk (dictionary) from Cartesia
                output = await loop.run_in_executor(
                    None, get_next_item_from_generator, tts_generator
                )
                if output is _GENERATOR_SENTINEL:
                    print("\nCartesia TTS WebSocket stream finished.")
                    break
            except Exception as tts_err:
                print(f"\n!!! ERROR receiving data from Cartesia WS executor: {tts_err}")
                break

            # --- Process and Forward Chunk to ESP32 ---
            source_buffer = None # Buffer in SOURCE format (e.g., f32le @ 24kHz)
            if isinstance(output, dict) and 'audio' in output and output['audio']:
                source_buffer = output['audio']
            else:
                print(f"WARN: Received non-standard output from Cartesia WS: type={type(output)}")

            if source_buffer:
                # --- !!! CONVERT CHUNK !!! ---
                conv_start_time = time.monotonic()
                esp32_buffer = convert_audio_chunk(source_buffer, SOURCE_RATE, ESP32_RATE)
                conv_end_time = time.monotonic()
                total_conversion_time += (conv_end_time - conv_start_time)
                # --- !!! CONVERSION DONE !!! ---

                if not esp32_buffer: # Skip if conversion failed or resulted in empty buffer
                    print("WARN: Skipping empty buffer after conversion.")
                    continue

                # Use the length of the CONVERTED buffer for pacing
                buffer_len_to_send = len(esp32_buffer)

                # Calculate pacing based on the CONVERTED chunk size and TARGET format
                chunk_duration_s = buffer_len_to_send / ESP32_BYTES_PER_SECOND
                target_interval_s = chunk_duration_s * SLEEP_MULTIPLIER
                send_prep_time = time.monotonic() # Time before trying to send

                try:
                    # Send the CONVERTED (pcm_s16le @ 16kHz) chunk to ESP32
                    await esp32_websocket.send(esp32_buffer)
                    send_complete_time = time.monotonic()
                    total_bytes_sent_to_esp += buffer_len_to_send

                    # --- Pacing Delay ---
                    # Time spent just sending the data
                    send_time = send_complete_time - send_prep_time
                    # Total processing time (conversion + send prep + send) for this chunk cycle
                    chunk_process_time = send_complete_time - conv_start_time # From conversion start to send complete

                    remaining_sleep = target_interval_s - chunk_process_time

                    if remaining_sleep > 0:
                        await asyncio.sleep(remaining_sleep)
                        total_sleep_time += remaining_sleep
                        expected_elapsed_time += target_interval_s
                    else:
                        # No sleep needed, but account for the time this chunk represents
                        expected_elapsed_time += target_interval_s
                        if total_bytes_sent_to_esp % (1024 * 10) < buffer_len_to_send :
                             print(f"WARN: Conv/Send ({chunk_process_time*1000:.1f}ms) >= target interval ({target_interval_s*1000:.1f}ms). No sleep.")

                # Handle ESP32 connection errors
                except websockets.exceptions.ConnectionClosedOK: print(f"\nESP32 client {client_id} closed connection normally."); return
                except websockets.exceptions.ConnectionClosedError as close_err: print(f"\nESP32 client {client_id} connection closed abruptly: {close_err}."); return
                except Exception as send_err: print(f"\nError sending data to ESP32 {client_id}: {send_err}"); return

        # --- Streaming Loop Finished Naturally ---
        end_time = time.monotonic()
        streaming_duration = end_time - start_time
        print(f"\nFinished forwarding Cartesia stream for {client_id}.")
        print(f"Total bytes sent to ESP32: {total_bytes_sent_to_esp}")
        print(f"Actual Duration: {streaming_duration:.2f}s")
        print(f"Expected Duration (sleeps): {expected_elapsed_time:.2f}s")
        print(f"Total Conversion Time: {total_conversion_time:.2f}s")
        print(f"Total Sleep Time: {total_sleep_time:.2f}s")


    except Exception as e:
        # Catch errors during setup or the main loop execution
        print(f"!!! ERROR in stream_cartesia_to_client main try block for {client_id}: {type(e).__name__} - {e}")
        traceback.print_exc()
        try: await esp32_websocket.send(f"ERROR: Server error during TTS setup/streaming: {type(e).__name__}"); await esp32_websocket.close(code=1011, reason="Server TTS error")
        except: pass
    finally:
        # --- Ensure Cartesia WebSocket is ALWAYS closed ---
        if cartesia_ws:
            print(f"Closing Cartesia WebSocket connection for {client_id}...")
            try: await loop.run_in_executor(None, cartesia_ws.close); print("Cartesia WebSocket closed.")
            except Exception as close_err: print(f"Error closing Cartesia WebSocket: {close_err}")


# --- connection_handler ---
# (Remains the same as the previous correct version)
async def connection_handler(websocket, path):
    """Handles new WebSocket connections FROM ESP32 devices."""
    client_id = f"{websocket.remote_address[0]}:{websocket.remote_address[1]}"
    print(f"WS> ESP32 Client connected: {client_id} (Path: {path})")
    connected_clients.add(websocket)
    send_task = None
    try:
        send_task = asyncio.create_task(stream_cartesia_to_client(websocket, path))
        async for message in websocket:
            if isinstance(message, str): print(f"WS [{client_id}] >>> Received text from ESP32: {message} (Ignoring)")
            else: print(f"WS [{client_id}] >>> Received binary from ESP32: {len(message)} bytes (Ignoring)")
    except websockets.exceptions.ConnectionClosedError as close_err: print(f"WS> ESP32 Client {client_id} disconnected abruptly: {close_err}")
    except websockets.exceptions.ConnectionClosedOK: print(f"WS> ESP32 Client {client_id} disconnected normally.")
    except Exception as e: print(f"WS> Error handling ESP32 client {client_id}: {type(e).__name__} - {e}")
    finally:
        print(f"WS> Cleaning up connection for ESP32 client {client_id}")
        if websocket in connected_clients: connected_clients.remove(websocket)
        if send_task and not send_task.done():
            print(f"WS> Cancelling Cartesia TTS task for {client_id}")
            send_task.cancel()
            try: await send_task
            except asyncio.CancelledError: print(f"WS> Cartesia TTS task for {client_id} cancelled successfully.")
            except Exception as task_e: print(f"WS> Error awaiting task cancellation for {client_id}: {task_e}")

# --- start_server ---
# (Remains the same as the previous correct version)
async def start_server():
    """Starts the WebSocket server to listen for ESP32 clients."""
    print(f"Starting WebSocket server on ws://{HOST}:{PORT} for ESP32 clients")
    print(f"Ready to stream Cartesia TTS upon connection.")
    print(f"Requesting {SOURCE_RATE}Hz {SOURCE_ENCODING} from Cartesia.")
    print(f"Converting to {ESP32_RATE}Hz pcm_s16le for ESP32.")
    print(f"Using Cartesia Voice ID: {VOICE_ID}")
    print(f"Pacing Sleep Multiplier: {SLEEP_MULTIPLIER:.2f}")
    server_settings = {"ping_interval": 20, "ping_timeout": 15, "close_timeout": 10}
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