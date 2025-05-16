"""
Speech-to-text and LLM processing pipeline script.
Takes an audio file path as input, transcribes it, and processes the text with an LLM.
"""

import os
import sys
import time
import logging
import traceback
from pathlib import Path
import locale
from typing import Optional, Dict, Any

# Add the project root to Python path to make imports work
project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, project_root)

# Configure proper encoding for Windows
if sys.platform == 'win32':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    os.system('chcp 65001 > nul')
    try:
        locale.setlocale(locale.LC_ALL, 'Russian_Russia.utf8')
    except locale.Error:
        try:
            locale.setlocale(locale.LC_ALL, '.UTF-8')
        except locale.Error:
            pass

# Third-party imports
import assemblyai as aai
from dotenv import load_dotenv
from langchain_core.messages import HumanMessage
from langchain_together import ChatTogether
from openai import OpenAI

# Local imports
from utils.utils import (
    setup_llm,
    transcribe_audio_whisper,
    run_llm_sync,
    setup_llm_services
)
from langgraph.tools import history_search, story_teller, input_validator
from database.sql_utils import initialize_db
from config.config import langgraph_config
from langgraph.agent import Agent

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger('pipeline')

load_dotenv()

def run_agent_graph(text: str) -> Optional[str]:
    """
    Process transcribed text using the Agent graph.
    
    Args:
        text: The text to process from the audio transcription
        
    Returns:
        Agent response or None if failed
    """
    logger.info(f"Running Agent graph for text: '{text[:80]}...'")
    
    try:
        llm = llm_validate = setup_llm()
        
        conn, cursor, memory = initialize_db()
        
        current_dir = os.path.dirname(os.path.abspath(__file__))
        personality_path = os.getenv("PERSONALITY_PATH", os.path.join(project_root, "chat", "toy.json"))
        
        agent = Agent(
            model=llm,
            checkpointer=memory,
            personality_path=personality_path
        )
        
        result = agent.stream_graph_updates(text)
        
        if result and isinstance(result, dict) and "messages" in result and result["messages"]:
            try:
                response_text = result["messages"][-1].content
                
                logger.info("Agent graph processing completed")
                logger.debug(f"Agent Result: {response_text}")
                
                if isinstance(response_text, bytes):
                    response_text = response_text.decode('utf-8', errors='replace')
                
                return response_text
            except (IndexError, AttributeError) as e:
                logger.error(f"Failed to extract response from agent result: {e}")
                return None
        else:
            logger.error("Agent returned invalid result structure")
            return None
            
    except Exception as e:
        logger.error(f"Agent graph processing failed: {e}")
        logger.debug(traceback.format_exc())
        return None

def process_audio_file(audio_file_path: str) -> Optional[str]:
    """
    Process an audio file through the STT and LLM pipeline.
    
    Args:
        audio_file_path: Path to the audio file to process
        
    Returns:
        The LLM response or None if the pipeline failed
    """
    logger.info(f"--- PIPELINE PROCESSING: {audio_file_path} ---")
    start_time = time.monotonic()
    
    file_basename = os.path.basename(audio_file_path)
    logger.info(f"Processing file: {file_basename}")

    transcribed_text = transcribe_audio_whisper(audio_file_path)
    
    if not transcribed_text:
        logger.error(f"Transcription failed for {file_basename}")
        return None

    llm_final_response = run_agent_graph(transcribed_text)

    if not llm_final_response:
        logger.error(f"LLM processing failed for {file_basename}")
        return None

    end_time = time.monotonic()
    logger.info(f"Pipeline completed for {file_basename} (Took {end_time - start_time:.2f}s)")
    
    return llm_final_response

def main() -> None:
    """Main pipeline execution function."""
    logger.info(f"--- PIPELINE SCRIPT ({os.getpid()}) START ---")
    
    if len(sys.argv) != 2:
        logger.error("Incorrect arguments")
        logger.info("Usage: python pipeline_script.py <path_to_wav_file>")
        sys.exit(1)

    input_wav_path = sys.argv[1]
    
    result = process_audio_file(input_wav_path)
    
    if result:
        try:
            print(f"FINAL_LLM_RESPONSE:{result}")
            sys.stdout.flush()
            sys.exit(0)
        except Exception as e:
            logger.error(f"Encoding error when outputting result: {e}")
            try:
                print(result)
            except:
                print(result.encode('utf-8').decode('utf-8', errors='replace'))
            sys.exit(0)
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()