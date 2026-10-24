"""Killing a command with everything it started. A process group is not enough: interrupt.py
starts its command in its own session, and qmlcoverage puts the program in its own group."""
import os
import pathlib
import signal


def alive(pid):
    """False for a zombie: killed, but its parent has not reaped it, so it stays listed."""
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2] != "Z"


def descendants(pid):
    """Every live descendant of pid."""
    found = []
    pending = [pid]
    while pending:
        parent = pending.pop()
        for task in pathlib.Path(f"/proc/{parent}/task").glob("*"):
            try:
                children = [int(c) for c in (task / "children").read_text().split()]
            except OSError:
                continue
            found += [c for c in children if alive(c)]
            pending += children
    return found


def kill_tree(pid):
    # Repeat until nothing is left, so a process forked during the walk is caught next time.
    while tree := descendants(pid):
        for process in tree:
            try:
                os.kill(process, signal.SIGKILL)
            except ProcessLookupError:
                pass
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
