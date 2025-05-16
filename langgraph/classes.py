from typing import Annotated, List
from pydantic import BaseModel, Field
from langchain_core.messages import HumanMessage
from langgraph.graph.message import add_messages


class State(BaseModel):
    messages: Annotated[list, add_messages]
    tool: str = ""
    tool_input: str = ""
    tool_output: str = ""
    need_tool: bool = False
    safety_issue: bool = False

