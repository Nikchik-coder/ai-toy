import os
import requests
import asyncio
from dotenv import load_dotenv
import logging
#Libraries for different LLMs
from langchain_together import ChatTogether
from langchain_google_genai import ChatGoogleGenerativeAI
from langchain_mistralai.chat_models import ChatMistralAI
from openai import OpenAI
import traceback
import assemblyai as aai
import json
from pathlib import Path
import time

load_dotenv()

PROJECT_ROOT = os.getenv("PROJECT_ROOT", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


# Configure logging
logs_dir = os.path.join(PROJECT_ROOT, "logs")
if not os.path.exists(logs_dir):
    os.makedirs(logs_dir)

# Added Logging Configuration
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(os.path.join(logs_dir, 'utils.log')),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__) # Use a logger instance



def setup_llm():
    """Initialize the LLM based on environment configuration"""
    logger.info("Setting up main LLM...")
    
    MODEL_PROVIDER = os.getenv("MODEL_PROVIDER").lower()
    MODEL_NAME = os.getenv("MODEL_NAME")
    TOGETHER_API_KEY = os.getenv("TOGETHER_API_KEY")
    GOOGLE_API_KEY = os.getenv("GOOGLE_API_KEY")
    MISTRAL_API_KEY = os.getenv("MISTRAL_API_KEY")
    
    logger.info(f"Using model provider: {MODEL_PROVIDER}, model: {MODEL_NAME}")
    
    if MODEL_PROVIDER == "together":
        if not TOGETHER_API_KEY:
            logger.error("TOGETHER_API_KEY environment variable is required for Together AI")
            raise ValueError("TOGETHER_API_KEY environment variable is required for Together AI")
        try:
            logger.info("Initializing Together AI model")
            return ChatTogether(
                together_api_key=TOGETHER_API_KEY,
                model=MODEL_NAME
            )
        except Exception as e:
            logger.error(f"Failed to initialize Together AI model: {e}")
            raise
    elif MODEL_PROVIDER == "google":
        if not GOOGLE_API_KEY:
            logger.error("GOOGLE_API_KEY environment variable is required for Google AI")
            raise ValueError("GOOGLE_API_KEY environment variable is required for Google AI")
        try:
            logger.info("Initializing Google AI model")
            return ChatGoogleGenerativeAI(
                google_api_key=GOOGLE_API_KEY,
                model=MODEL_NAME
            )
        except Exception as e:
            logger.error(f"Failed to initialize Google AI model: {e}")
            raise
    elif MODEL_PROVIDER == "mistral":
        if not MISTRAL_API_KEY:
            logger.error("MISTRAL_API_KEY environment variable is required for Mistral AI")
            raise ValueError("MISTRAL_API_KEY environment variable is required for Mistral AI")
        try:
            logger.info("Initializing Mistral AI model")
            return ChatMistralAI(
                api_key=MISTRAL_API_KEY,
                model=MODEL_NAME
            )
        except Exception as e:
            logger.error(f"Failed to initialize Mistral AI model: {e}")
            raise
    else:
        logger.error(f"Unsupported model provider: {MODEL_PROVIDER}")
        raise ValueError(f"Unsupported model provider: {MODEL_PROVIDER}")
    

def transcribe_audio_whisper(file_path: str) -> str | None:
    """
    Transcribe audio file using OpenAI Whisper via DeepInfra.
    
    Args:
        file_path: Path to the audio file
        
    Returns:
        Transcribed text or None if failed
    """
    logger.info(f"Running Whisper STT on {file_path}...")

    try:
        client = OpenAI(
            api_key=os.getenv("DEEP_INFRA_KEY"),
            base_url="https://api.deepinfra.com/v1/openai",
        )
    except Exception as e:
        logger.error(f"Failed to initialize OpenAI client: {e}")
        raise
    
    try:
        with open(file_path, "rb") as audio_file:
            transcript = client.audio.transcriptions.create(
                model="openai/whisper-large-v3",
                file=audio_file,
                language="ru"  # Specify Russian language
            )

        if transcript and transcript.text:
            logger.info("Whisper transcription successful!")
            logger.debug(f"Transcription text: {transcript.text}")
            return transcript.text
        else:
            logger.error("Whisper returned empty transcript")
            return None
            
    except Exception as e:
        logger.error(f"Whisper transcription failed: {e}")
        logger.debug(traceback.format_exc())
        return None
    

def transcribe_audio_assemblyai(file_path: str) -> str | None:
    """
    Transcribe audio file using AssemblyAI.
    
    Args:
        file_path: Path to the audio file
        
    Returns:
        Transcribed text or None if failed
    """
    file_path = Path(file_path)


    logger.info(f"[{file_path.name}] Running STT...")

    try:
        aai.settings.api_key = os.getenv("ASSEMBLYAI_API_KEY")
    except Exception as e:
        logger.error(f"Failed to initialize AssemblyAI client: {e}")
        raise
    
    if not file_path.exists():
        logger.error(f"Input WAV file not found: {file_path}")
        return None 
    
    config = aai.TranscriptionConfig(
        speech_model=aai.SpeechModel.best,
        language_code="ru"
    )
    
    transcriber = aai.Transcriber(config=config)

    try:
        logger.info(f"Transcribing file: {file_path}")
        transcript = transcriber.transcribe(str(file_path))

        if transcript.status == aai.TranscriptStatus.error:
            logger.error(f"Transcription failed: {transcript.error}")
            return None
            
        logger.info("Transcription successful!")
        logger.debug(f"Transcription text: {transcript.text}")
        return transcript.text

    except Exception as e:
        logger.error(f"An error occurred during transcription: {e}")
        return None



def run_llm_sync(text: str) -> str | None:
    """
    Process transcribed text with LLM.
    
    Args:
        text: The text to process
        
    Returns:
        LLM response or None if failed
    """
    logger.info(f"Running LLM for text: '{text[:80]}...'")
    
    prompt = f"""
    Ты ИИ помощник, который отвечает на вопросы пользователя.
    Тебе дан текст пользователя полученный после транскрибации аудио.
    Отвечай на вопросы пользователя, используя данный текст.
    Отвечай на русском языке.
    Text: {text}
    """
    
    try:
        llm = setup_llm()
        llm_response = llm.invoke(prompt)
        
        # Extract just the content string from the response
        if hasattr(llm_response, 'content'):
            response_text = llm_response.content
        else:
            response_text = str(llm_response.text)
        
        logger.info(f"LLM processing completed")
        logger.debug(f"LLM Result: {response_text}")
        
        # Make sure the response is properly encoded as UTF-8 text
        if isinstance(response_text, bytes):
            response_text = response_text.decode('utf-8', errors='replace')
        
        # Removing the problematic encode/decode that can cause issues
        return response_text
    except Exception as e:
        logger.error(f"LLM processing failed: {e}")
        logger.debug(traceback.format_exc())
        return None

def setup_llm_services() -> tuple[ChatTogether, ChatTogether]:
    """Set up LLM services with proper error handling."""
    try:
        TOGETHER_API_KEY = os.getenv("TOGETHER_API_KEY")

        if not TOGETHER_API_KEY:
            raise ValueError("TOGETHER_API_KEY not found in environment variables")
            
        llm = ChatTogether(
            together_api_key=TOGETHER_API_KEY,
            model="meta-llama/Llama-3.3-70B-Instruct-Turbo"
        )

        llm_validate = ChatTogether(
            together_api_key=TOGETHER_API_KEY,
            model="meta-llama/Llama-3.3-70B-Instruct-Turbo"
        )
        
        return llm, llm_validate
    except Exception as e:
        logger.error(f"Failed to set up LLM services: {e}")
        raise


def load_json(file_path):
    """Loads personality from a JSON file."""
    try:
       logger.debug("Loading JSON from %s", file_path)
       with open(file_path, 'r') as f:
          data = json.load(f)
          logger.debug("Successfully loaded JSON data")
          return data
    except Exception as e:
       logger.error("Error loading JSON: %s", e)
       return None
    

def print_stream(text, prefix=""):
    """
    Stream text word by word with a slight delay to simulate typing.
    This function still uses print as it's specifically for user output display.
    """
    words = text.split()
    count = 0
    logger.info(prefix, end="")
    for word in words:
        if count == 20:
          logger.info('\n' + prefix, end='', flush=True)  # Print a newline without extra space
          count = 0
        print(word, end=' ', flush=True)
        count += 1
        time.sleep(0.2)

    print()