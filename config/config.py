"""
Shared configuration settings for the chat application.
This avoids circular imports between modules.
"""
import logging


def setup_logging(level=logging.INFO):
    """Set up logging configuration"""
    logging.basicConfig(
        level=level,
        format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    return logging.getLogger(__name__)

# Initialize logger
logger = setup_logging()

# LangGraph thread configuration
langgraph_config = {"configurable": {"thread_id": "main_thread"}} 