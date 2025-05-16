"""
Test script for SQL utilities
"""

import os
from sql_utils import initialize_db, create_conversation, save_message, get_conversation_history

def main():
    print("Testing SQL utilities...")
    
    # Initialize database
    conn, cursor, memory = initialize_db()
    print("Database initialized successfully")
    
    # Create a test conversation
    user_id = "test_user"
    session_id = "test_session"
    conversation_id = create_conversation(cursor, user_id, session_id)
    print(f"Created conversation with ID: {conversation_id}")
    
    # Save some test messages
    messages = [
        ("user", "Hello, how are you?"),
        ("assistant", "I'm doing well, thank you! How can I help you today?"),
        ("user", "Tell me a story"),
        ("assistant", "Once upon a time in a far away land...")
    ]
    
    for role, content in messages:
        message_id = save_message(cursor, conversation_id, role, content)
        print(f"Saved message with ID: {message_id}")
    
    # Retrieve conversation history
    history = get_conversation_history(cursor, conversation_id)
    print("\nConversation History:")
    for i, msg in enumerate(history):
        print(f"{i+1}. {msg['role']}: {msg['content']}")
    
    print("\nAll tests completed successfully!")

if __name__ == "__main__":
    main() 