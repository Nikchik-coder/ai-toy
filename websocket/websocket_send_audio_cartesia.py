import asyncio
import websockets
import os
import time
from dotenv import load_dotenv
from cartesia import Cartesia
import traceback
import concurrent.futures
import numpy as np
from scipy.signal import resample

load_dotenv()
CARTESIA_API_KEY = os.environ.get("CARTESIA_API_KEY")
if not CARTESIA_API_KEY:
    print("!!! FATAL ERROR: CARTESIA_API_KEY not found in environment variables or .env file.")
    exit()

try:
    client = Cartesia(api_key=CARTESIA_API_KEY)
except Exception as e:
    print(f"!!! FATAL ERROR: Failed to initialize Cartesia client: {e}")
    exit()

VOICE_ID = "a38e4e85-e815-43ab-acf1-907c4688dd6c"
TRANSCRIPT = "Thinking: Given my goals to be a Web3 influencer and provide informative content to the community, I should share the crafted message about the latest Web3 trends in 2025. This aligns with my previous actions of researching and crafting informative content. The 'post_to_telegram' tool is suitable for this purpose, despite its high energy cost, as it directly contributes to my goals"
MODEL_ID = "sonic-english"

HOST = '0.0.0.0'
PORT = 8765

ESP32_RATE = 16000
ESP32_WIDTH = 2
ESP32_CHANNELS = 1
ESP32_BYTES_PER_SECOND = ESP32_RATE * ESP32_WIDTH * ESP32_CHANNELS

try:
    if ESP32_BYTES_PER_SECOND == 0: raise ValueError("ESP32 BPS is zero.")
except ValueError as e: print(f"!!! ERROR: {e}"); exit()
SLEEP_MULTIPLIER = 1.0

SOURCE_RATE = 24000
SOURCE_ENCODING = "pcm_f32le"
SOURCE_WIDTH = 4
SOURCE_CHANNELS = 1

cartesia_output_format = {
    "container": "raw",
    "encoding": SOURCE_ENCODING,
    "sample_rate": SOURCE_RATE
}
print(f"Requesting {SOURCE_RATE}Hz {SOURCE_ENCODING} from Cartesia.")
print(f"Will convert to {ESP32_RATE}Hz pcm_s16le for ESP32.")

connected_clients = set()
_GENERATOR_SENTINEL = object()

def convert_audio_chunk(buffer_f32le, source_rate, target_rate):
    """Converts a float32 little-endian chunk to int16 little-endian and resamples."""
    try:
        float32_array = np.frombuffer(buffer_f32le, dtype=np.float32)

        num_samples_in = len(float32_array)
        if num_samples_in == 0:
            return b''

        if source_rate != target_rate:
            num_samples_out = int(np.round(num_samples_in * target_rate / source_rate))
            if num_samples_out <= 0:
                return b''
            resampled_array = resample(float32_array, num_samples_out)
        else:
            resampled_array = float32_array

        int16_array = np.clip(resampled_array * 32767, -32768, 32767).astype(np.int16)

        output_buffer = int16_array.tobytes()
        return output_buffer

    except Exception as conv_err:
        print(f"!!! ERROR during audio conversion: {conv_err}")
        traceback.print_exc()
        return b''


async def stream_cartesia_to_client(esp32_websocket, path):
    """Connects to Cartesia TTS WS, gets audio, converts, and forwards to ESP32 WS."""
    client_id = f"{esp32_websocket.remote_address[0]}:{esp32_websocket.remote_address[1]}"
    print(f"Attempting Cartesia WS stream for ESP32 client: {client_id}")
    loop = asyncio.get_running_loop()
    cartesia_ws = None

    try:
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
                output_format=cartesia_output_format,
            )
            print("Cartesia TTS request sent. Generator obtained.")
            return ws, tts_generator

        cartesia_ws, tts_generator = await loop.run_in_executor(
            None, connect_and_send_cartesia_request
        )
        print("Cartesia WS connection established and generator ready.")

        def get_next_item_from_generator(gen):
            try: return next(gen)
            except StopIteration: return _GENERATOR_SENTINEL
            except Exception as e: print(f"Error in get_next_item: {e}"); raise

        total_bytes_sent_to_esp = 0
        start_time = time.monotonic()
        expected_elapsed_time = 0.0
        total_conversion_time = 0.0
        total_sleep_time = 0.0

        while True:
            output = None
            try:
                output = await loop.run_in_executor(
                    None, get_next_item_from_generator, tts_generator
                )
                if output is _GENERATOR_SENTINEL:
                    print("\nCartesia TTS WebSocket stream finished.")
                    break
            except Exception as tts_err:
                print(f"\n!!! ERROR receiving data from Cartesia WS executor: {tts_err}")
                break

            source_buffer = None
            if isinstance(output, dict) and 'audio' in output and output['audio']:
                source_buffer = output['audio']
            else:
                print(f"WARN: Received non-standard output from Cartesia WS: type={type(output)}")

            if source_buffer:
                conv_start_time = time.monotonic()
                esp32_buffer = convert_audio_chunk(source_buffer, SOURCE_RATE, ESP32_RATE)
                conv_end_time = time.monotonic()
                total_conversion_time += (conv_end_time - conv_start_time)

                if not esp32_buffer:
                    print("WARN: Skipping empty buffer after conversion.")
                    continue

                buffer_len_to_send = len(esp32_buffer)

                chunk_duration_s = buffer_len_to_send / ESP32_BYTES_PER_SECOND
                target_interval_s = chunk_duration_s * SLEEP_MULTIPLIER
                send_prep_time = time.monotonic()

                try:
                    await esp32_websocket.send(esp32_buffer)
                    send_complete_time = time.monotonic()
                    total_bytes_sent_to_esp += buffer_len_to_send

                    send_time = send_complete_time - send_prep_time
                    chunk_process_time = send_complete_time - conv_start_time

                    remaining_sleep = target_interval_s - chunk_process_time

                    if remaining_sleep > 0:
                        await asyncio.sleep(remaining_sleep)
                        total_sleep_time += remaining_sleep
                        expected_elapsed_time += target_interval_s
                    else:
                        expected_elapsed_time += target_interval_s
                        if total_bytes_sent_to_esp % (1024 * 10) < buffer_len_to_send :
                             print(f"WARN: Conv/Send ({chunk_process_time*1000:.1f}ms) >= target interval ({target_interval_s*1000:.1f}ms). No sleep.")

                except websockets.exceptions.ConnectionClosedOK: print(f"\nESP32 client {client_id} closed connection normally."); return
                except websockets.exceptions.ConnectionClosedError as close_err: print(f"\nESP32 client {client_id} connection closed abruptly: {close_err}."); return
                except Exception as send_err: print(f"\nError sending data to ESP32 {client_id}: {send_err}"); return

        end_time = time.monotonic()
        streaming_duration = end_time - start_time
        print(f"\nFinished forwarding Cartesia stream for {client_id}.")
        print(f"Total bytes sent to ESP32: {total_bytes_sent_to_esp}")
        print(f"Actual Duration: {streaming_duration:.2f}s")
        print(f"Expected Duration (sleeps): {expected_elapsed_time:.2f}s")
        print(f"Total Conversion Time: {total_conversion_time:.2f}s")
        print(f"Total Sleep Time: {total_sleep_time:.2f}s")


    except Exception as e:
        print(f"!!! ERROR in stream_cartesia_to_client main try block for {client_id}: {type(e).__name__} - {e}")
        traceback.print_exc()
        try: await esp32_websocket.send(f"ERROR: Server error during TTS setup/streaming: {type(e).__name__}"); await esp32_websocket.close(code=1011, reason="Server TTS error")
        except: pass
    finally:
        if cartesia_ws:
            print(f"Closing Cartesia WebSocket connection for {client_id}...")
            try: await loop.run_in_executor(None, cartesia_ws.close); print("Cartesia WebSocket closed.")
            except Exception as close_err: print(f"Error closing Cartesia WebSocket: {close_err}")


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