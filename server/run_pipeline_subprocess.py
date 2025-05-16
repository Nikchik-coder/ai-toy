#!/usr/bin/env python
# run_pipeline_subprocess.py
"""
Example script that demonstrates how to run pipeline_script.py as a subprocess.
This mimics how it would be called from a main application.
"""

import os
import sys
import logging
import subprocess
from pathlib import Path

# Set console code page to UTF-8 on Windows
if sys.platform == 'win32':
    os.system('chcp 65001 > nul')

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger('run_pipeline')

def run_pipeline_as_subprocess(audio_file_path):
    """
    Run the pipeline_script.py as a subprocess with the given audio file.
    
    Args:
        audio_file_path: Path to the audio file to process
        
    Returns:
        The output from the subprocess and the return code
    """
    # Convert to absolute path if needed
    audio_file_path = os.path.abspath(audio_file_path)
    
    # Check if file exists
    if not os.path.exists(audio_file_path):
        logger.error(f"Audio file not found at {audio_file_path}")
        return None, 1
    
    # Get the path to pipeline_script.py
    pipeline_script = os.path.join(os.path.dirname(__file__), "pipeline_script.py")
    
    logger.info(f"Running pipeline script as subprocess with audio file: {audio_file_path}")
    
    # Run the pipeline script as a subprocess
    # capture_output=True captures stdout and stderr
    # Set UTF-8 encoding for the subprocess
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8"
    
    result = subprocess.run(
        [sys.executable, pipeline_script, audio_file_path],
        capture_output=True,
        text=True,
        encoding='utf-8',
        env=env
    )
    
    # Log the output at different levels based on return code
    if result.returncode == 0:
        logger.info("Pipeline subprocess completed successfully")
        logger.debug("Subprocess output:\n" + result.stdout)
    else:
        logger.warning(f"Pipeline subprocess exited with code {result.returncode}")
        logger.info("Subprocess output:\n" + result.stdout)
    
    if result.stderr:
        logger.error("Subprocess errors:\n" + result.stderr)
    
    # Extract LLM response if successful
    llm_response = None
    if result.returncode == 0:
        for line in result.stdout.splitlines():
            if line.startswith("FINAL_LLM_RESPONSE:"):
                # Extract the clean response text
                llm_response = line.replace("FINAL_LLM_RESPONSE:", "", 1).strip()
                logger.info("Successfully extracted LLM response")
                break
    
    return llm_response, result.returncode

if __name__ == "__main__":
    # Get the project root directory
    project_root = Path(__file__).parent.parent
    
    # Default to a sample file in the received_audio_wav directory
    # DEFAULT_AUDIO_FILE = project_root / "received_audio_wav" / "record.wav"

    DEFAULT_AUDIO_FILE = r"C:\Users\Doctor Who\Desktop\TedToy\tedtoy\received_audio_wav\audio.wav"
    
    # Use the provided command line argument or the default
    audio_file_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_AUDIO_FILE
    
    logger.info(f"Starting pipeline test with audio file: {audio_file_path}")
    
    # Run the pipeline as a subprocess
    response, return_code = run_pipeline_as_subprocess(str(audio_file_path))
    
    # Print the extracted LLM response if available
    if response:
        logger.info("Pipeline test completed successfully")
        print("\n--- FINAL RESPONSE ---")
        print(response)  # Print just the clean response text
    else:
        logger.error("Pipeline test failed")
    
    # Exit with the same return code as the subprocess
    sys.exit(return_code) 