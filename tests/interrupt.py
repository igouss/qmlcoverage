#!/usr/bin/env python3
"""Run a command in its own session, then SIGINT its whole process group after a delay, the way
a terminal's Ctrl+C does. Prints the command's output and exit code. A command still running
INTERRUPT_GRACE seconds (default 30) later is killed with everything it started: exit=timeout.

Usage: interrupt.py <seconds> <command> [args...]
"""
import os
import pathlib
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # -I leaves it out
from proctree import kill_tree  # noqa: E402

delay = float(sys.argv[1])
child = subprocess.Popen(sys.argv[2:], start_new_session=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(delay)
os.killpg(child.pid, signal.SIGINT)
try:
    output, _ = child.communicate(timeout=float(os.environ.get("INTERRUPT_GRACE", 30)))
    status = child.returncode
except subprocess.TimeoutExpired:
    kill_tree(child.pid)
    output, _ = child.communicate()
    status = "timeout"
sys.stdout.write(output.decode())
print(f"exit={status}")
