from pydub import AudioSegment
from pydub.exceptions import CouldntDecodeError
import os
import wave

# Configuration
INPUT_AUDIO_FILE = r'C:\Users\Doctor Who\Desktop\robottoy\audio_uploads\test.wav'
OUTPUT_AUDIO_FILE = r'C:\Users\Doctor Who\Desktop\robottoy\audio_uploads\converted_audio.wav'
TARGET_RATE = 16000
TARGET_CHANNELS = 1
TARGET_SAMPLE_WIDTH_BYTES = 2
OUTPUT_FORMAT = "wav"

def convert_audio(input_path, output_path, target_rate, target_channels, target_width_bytes, output_format):
    """Converts an audio file to the specified format using pydub."""
    if not os.path.exists(input_path):
        print(f"!!! ERROR: Input file not found: {input_path}")
        return False

    print(f"Attempting to load audio file: {input_path}")
    try:
        audio = AudioSegment.from_file(input_path)
        print(f"  Original - Rate: {audio.frame_rate} Hz, Channels: {audio.channels}, Width: {audio.sample_width} bytes ({audio.sample_width*8}-bit)")

        print(f"Converting to Rate: {target_rate} Hz, Channels: {target_channels}, Width: {target_width_bytes*8}-bit...")

        if audio.frame_rate != target_rate:
            print(f"  - Setting frame rate to {target_rate} Hz...")
            audio = audio.set_frame_rate(target_rate)

        if audio.channels != target_channels:
            print(f"  - Setting channels to {target_channels} (mono)...")
            audio = audio.set_channels(target_channels)

        if audio.sample_width != target_width_bytes:
             print(f"  - Setting sample width to {target_width_bytes} bytes ({target_width_bytes*8}-bit)...")

        print(f"Exporting to '{output_format}' format: {output_path}")
        audio.export(output_path, format=output_format)

        print("Conversion successful!")

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
                     return False
                else:
                     print("  Output file format verified successfully.")
                     return True
        except wave.Error as e:
             print(f"  Error verifying output WAV file: {e}")
             return False
        except Exception as e:
             print(f"  An unexpected error occurred during output verification: {e}")
             return False

    except CouldntDecodeError:
        print(f"!!! ERROR: Could not decode input file: {input_path}")
        print("!!!        Make sure FFmpeg or Libav is installed and accessible in your system PATH.")
        return False
    except FileNotFoundError:
         print(f"!!! ERROR: Input file disappeared or invalid path during processing: {input_path}")
         return False
    except Exception as e:
        print(f"!!! ERROR: An unexpected error occurred during conversion: {e}")
        return False

if __name__ == "__main__":
    if convert_audio(INPUT_AUDIO_FILE, OUTPUT_AUDIO_FILE,
                     TARGET_RATE, TARGET_CHANNELS, TARGET_SAMPLE_WIDTH_BYTES, OUTPUT_FORMAT):
        print(f"\nSuccessfully converted '{os.path.basename(INPUT_AUDIO_FILE)}' to '{os.path.basename(OUTPUT_AUDIO_FILE)}'")
        print(f"You can now update your server script's AUDIO_FILE_PATH to:\n{OUTPUT_AUDIO_FILE}")
    else:
        print(f"\nConversion FAILED for '{os.path.basename(INPUT_AUDIO_FILE)}'")