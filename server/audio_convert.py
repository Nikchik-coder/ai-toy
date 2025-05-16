from pydub import AudioSegment
from pydub.exceptions import CouldntDecodeError
import os
import wave # For verification

# --- Configuration ---
# <<<--- SET PATH TO YOUR ORIGINAL AUDIO FILE ---<<<
INPUT_AUDIO_FILE = r'C:\Users\Doctor Who\Desktop\robottoy\audio_uploads\test.wav' # Or .wav, .ogg, .m4a, etc.

# <<<--- SET PATH FOR THE CONVERTED OUTPUT FILE ---<<<
OUTPUT_AUDIO_FILE = r'C:\Users\Doctor Who\Desktop\robottoy\audio_uploads\converted_audio.wav'

TARGET_RATE = 16000          # Target sample rate (Hz)
TARGET_CHANNELS = 1            # Target channels (1 for mono)
TARGET_SAMPLE_WIDTH_BYTES = 2  # Target sample width in bytes (16-bit = 2)
OUTPUT_FORMAT = "wav"          # Target format
# --- --- --- --- ---

def convert_audio(input_path, output_path, target_rate, target_channels, target_width_bytes, output_format):
    """Converts an audio file to the specified format using pydub."""
    if not os.path.exists(input_path):
        print(f"!!! ERROR: Input file not found: {input_path}")
        return False

    print(f"Attempting to load audio file: {input_path}")
    try:
        # Load the audio file. Pydub uses ffmpeg/libav behind the scenes
        # for non-wav files or complex operations.
        audio = AudioSegment.from_file(input_path)
        print(f"  Original - Rate: {audio.frame_rate} Hz, Channels: {audio.channels}, Width: {audio.sample_width} bytes ({audio.sample_width*8}-bit)")

        # --- Apply Conversions ---
        print(f"Converting to Rate: {target_rate} Hz, Channels: {target_channels}, Width: {target_width_bytes*8}-bit...")

        # 1. Set Sample Rate
        if audio.frame_rate != target_rate:
            print(f"  - Setting frame rate to {target_rate} Hz...")
            audio = audio.set_frame_rate(target_rate)

        # 2. Set Channels (Convert to Mono if needed)
        if audio.channels != target_channels:
            print(f"  - Setting channels to {target_channels} (mono)...")
            audio = audio.set_channels(target_channels)

        # 3. Set Sample Width (Bit Depth)
        # Pydub's AudioSegment object has a sample_width property.
        # Exporting to WAV usually defaults to 16-bit if the source wasn't already lower.
        # We can try setting it explicitly for clarity, though export might override/handle it.
        # Note: This might not always be necessary as export() often handles it for WAV.
        if audio.sample_width != target_width_bytes:
             print(f"  - Setting sample width to {target_width_bytes} bytes ({target_width_bytes*8}-bit)...")
             # Be careful: directly setting sample width can sometimes cause issues
             # if not handled correctly during export. Often, relying on the format
             # default during export is safer for standard formats like 16-bit WAV.
             # Let's rely on the export format parameter first.
             # audio = audio.set_sample_width(target_width_bytes) # Try uncommenting if export isn't 16-bit

        # --- Export the audio ---
        print(f"Exporting to '{output_format}' format: {output_path}")
        # For WAV, pydub typically defaults to 16-bit PCM.
        # The 'parameters' argument is more often used for codec-specific options
        # in compressed formats (like bitrate for mp3).
        audio.export(output_path, format=output_format)

        print("Conversion successful!")

        # --- Optional: Verify output file properties using wave module ---
        print("Verifying output file...")
        try:
            with wave.open(output_path, 'rb') as wf:
                out_rate = wf.getframerate()
                out_width = wf.getsampwidth()
                out_channels = wf.getnchannels()
                print(f"  Verified Output - Rate: {out_rate} Hz, Channels: {out_channels}, Width: {out_width} bytes ({out_width*8}-bit)")
                if out_rate != target_rate or out_channels != target_channels or out_width != target_width_bytes:
                     print("!!! WARNING: Output file verification failed for some parameters!")
                     print(f"!!! Expected: Rate={target_rate}, Channels={target_channels}, Width={target_width_bytes}")
                     return False # Consider it a failure if verification fails
                else:
                     print("  Output file format verified successfully.")
                     return True
        except wave.Error as e:
             print(f"  Error verifying output WAV file: {e}")
             return False # Treat verification error as failure
        except Exception as e:
             print(f"  An unexpected error occurred during output verification: {e}")
             return False

    except CouldntDecodeError:
        print(f"!!! ERROR: Could not decode input file: {input_path}")
        print("!!!        Make sure FFmpeg or Libav is installed and accessible in your system PATH.")
        return False
    except FileNotFoundError: # Should be caught earlier, but good practice
         print(f"!!! ERROR: Input file disappeared or invalid path during processing: {input_path}")
         return False
    except Exception as e:
        print(f"!!! ERROR: An unexpected error occurred during conversion: {e}")
        return False

# --- Run the conversion ---
if __name__ == "__main__":
    if convert_audio(INPUT_AUDIO_FILE, OUTPUT_AUDIO_FILE,
                     TARGET_RATE, TARGET_CHANNELS, TARGET_SAMPLE_WIDTH_BYTES, OUTPUT_FORMAT):
        print(f"\nSuccessfully converted '{os.path.basename(INPUT_AUDIO_FILE)}' to '{os.path.basename(OUTPUT_AUDIO_FILE)}'")
        print(f"You can now update your server script's AUDIO_FILE_PATH to:\n{OUTPUT_AUDIO_FILE}")
    else:
        print(f"\nConversion FAILED for '{os.path.basename(INPUT_AUDIO_FILE)}'")