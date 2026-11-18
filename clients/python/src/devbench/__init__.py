"""Python client for devbench's REST port: call a running game's tools from a script, a REPL or a test.

Importing the package never imports pytest; the opt-in pytest plugin lives in devbench.testing.
"""

from importlib.metadata import PackageNotFoundError, version

from devbench.checks import (
    RequirementError,
    require,
    require_actions,
    require_min_version,
    require_tools,
    require_version,
)
from devbench.client import Devbench, DevbenchError, ModTool, find_port
from devbench.console import console, console_number
from devbench.events import Events
from devbench.files import Backup, delete_file, list_files, read_bytes, read_text, write_bytes, write_text
from devbench.log import LogMark
from devbench.menus import close_dialogs, close_menus, message_box_text, open_menus
from devbench.process import launch, quit_game, restart, wait_gone
from devbench.vr import Hands
from devbench.waits import wait_for, wait_until

try:
    __version__ = version("devbench-client")
except PackageNotFoundError:  # a source tree on sys.path that was never installed
    __version__ = "0.0.0"

__all__ = [
    "Backup",
    "Devbench",
    "DevbenchError",
    "Events",
    "Hands",
    "LogMark",
    "ModTool",
    "RequirementError",
    "__version__",
    "close_dialogs",
    "close_menus",
    "console",
    "console_number",
    "delete_file",
    "find_port",
    "launch",
    "list_files",
    "message_box_text",
    "open_menus",
    "quit_game",
    "read_bytes",
    "read_text",
    "require",
    "require_actions",
    "require_min_version",
    "require_tools",
    "require_version",
    "restart",
    "wait_gone",
    "wait_for",
    "wait_until",
    "write_bytes",
    "write_text",
]
