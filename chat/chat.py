import os
from dotenv import load_dotenv
import sys


sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from utils.utils import print_stream, load_json, setup_llm
from database.sql_utils import initialize_db
from langgraph.agent import Agent



load_dotenv()

# Initialize database connection and memory
conn, cursor, memory = initialize_db()

# Get personality path - use absolute path to ensure it works
current_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.dirname(current_dir)
personality_path = os.path.join(project_root, "data", "toy.json")
print(f"Loading personality from: {personality_path}") 


llm = setup_llm()

# tools = [history_search, story_teller, input_validator]

def main():
    agent = Agent(llm, memory, personality_path)
    while True:

        print("Users input")
        user_input = input("User: ")
        if user_input.lower() in ["quit", "exit", "q", "bye", "adios"]:
            print("Goodbye!")
            break
        result = agent.stream_graph_updates(user_input)
        if result and result.get("messages") and len(result["messages"]) > 1:
            assistant_response = result["messages"][-1].content
            print(f"Assistant: {assistant_response}")
        else:
            print("Assistant: No response generated.")

if __name__ == "__main__":
    main()


