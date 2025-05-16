import os
import sqlite3
from typing import Tuple, Optional, List, Dict, Any
from langgraph.checkpoint.sqlite import SqliteSaver
import logging

def get_db_path() -> str:
    """Get the database path from environment or default"""
    current_dir = os.path.dirname(os.path.abspath(__file__))
    db_filename = "toy.db"
    db_path = os.getenv("DB_PATH", os.path.join(current_dir, db_filename))
    return db_path

def connect_db() -> Tuple[sqlite3.Connection, sqlite3.Cursor]:
    """Connect to the SQLite database and return connection and cursor"""
    db_path = get_db_path()
    try:
        conn = sqlite3.connect(db_path, check_same_thread=False)
        cursor = conn.cursor()
        return conn, cursor
    except sqlite3.Error as e:
        logging.error(f"SQLite error: {e}")
        raise

def create_db(db_path: str = None) -> Tuple[sqlite3.Connection, sqlite3.Cursor]:
    """Create the database tables if they don't exist"""
    if db_path is None:
        db_path = get_db_path()
    
    conn = sqlite3.connect(db_path, check_same_thread=False)
    cursor = conn.cursor()
    
    # Create the checkpoints table for LangGraph
    cursor.execute('''
    CREATE TABLE IF NOT EXISTS checkpoints (
        thread_id TEXT NOT NULL,
        checkpoint_ns TEXT NOT NULL,
        checkpoint_id TEXT NOT NULL,
        parent_checkpoint_id TEXT,
        type TEXT NOT NULL,
        checkpoint TEXT NOT NULL,
        metadata TEXT NOT NULL
    )
    ''')
    
    # Create additional tables for conversation history
    cursor.execute('''
    CREATE TABLE IF NOT EXISTS conversations (
        id INTEGER PRIMARY KEY,
        user_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
    )
    ''')
    
    cursor.execute('''
    CREATE TABLE IF NOT EXISTS messages (
        id INTEGER PRIMARY KEY,
        conversation_id INTEGER,
        role TEXT NOT NULL,
        content TEXT NOT NULL,
        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (conversation_id) REFERENCES conversations (id)
    )
    ''')
    
    conn.commit()
    return conn, cursor

def create_memory_saver(conn: sqlite3.Connection) -> SqliteSaver:
    """Create a SqliteSaver for graph memory"""
    return SqliteSaver(conn)

def initialize_db() -> Tuple[sqlite3.Connection, sqlite3.Cursor, SqliteSaver]:
    """Initialize the database and return connection, cursor and memory saver"""
    db_path = get_db_path()
    db_exists = os.path.exists(db_path)
    
    # Debug output
    print(f"Database path: {db_path}")
    print(f"Current directory: {os.path.dirname(db_path)}")
    print(f"Database directory exists: {os.path.exists(os.path.dirname(db_path))}")
    print(f"Database directory is writable: {os.access(os.path.dirname(db_path), os.W_OK)}")
    
    # Always create or ensure tables exist
    conn, cursor = create_db(db_path)
    print("Database tables created/verified")
    
    # Create SqliteSaver for langgraph
    memory = create_memory_saver(conn)
    
    return conn, cursor, memory

# Helper functions for chat history
def save_message(cursor: sqlite3.Cursor, conversation_id: int, role: str, content: str) -> int:
    """Save a message to the database"""
    cursor.execute(
        "INSERT INTO messages (conversation_id, role, content) VALUES (?, ?, ?)",
        (conversation_id, role, content)
    )
    cursor.connection.commit()
    return cursor.lastrowid

def get_conversation_history(cursor: sqlite3.Cursor, conversation_id: int) -> List[Dict[str, Any]]:
    """Get the conversation history for a conversation ID"""
    cursor.execute(
        "SELECT role, content FROM messages WHERE conversation_id = ? ORDER BY timestamp",
        (conversation_id,)
    )
    results = cursor.fetchall()
    return [{"role": row[0], "content": row[1]} for row in results]

def create_conversation(cursor: sqlite3.Cursor, user_id: str, session_id: str) -> int:
    """Create a new conversation and return its ID"""
    cursor.execute(
        "INSERT INTO conversations (user_id, session_id) VALUES (?, ?)",
        (user_id, session_id)
    )
    cursor.connection.commit()
    return cursor.lastrowid 