#!/usr/bin/env python3
"""Mutation testing for the C++ sources: each universalmutator mutant is built and judged by
tests/check.sh, in parallel worker copies of the repo.

A mutant that does not compile is invalid, not killed. Mutants on comment and #include lines
are skipped. Survivors are listed with their diff line so they can be classified.

Usage: tests/mutate.py <workdir> <source>... [--jobs N]
  workdir: scratch directory (worker copies, mutants and verdicts.jsonl go there); rerunning with
           the same workdir skips mutants already killed or invalid, and judges survivors again
  source:  repo-relative files to mutate, e.g. plugin/qqmlcoverageservice.cpp
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import queue
import resource
import shutil
import signal
import subprocess
import sys
import threading

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # -I leaves it out
from proctree import kill_tree  # noqa: E402

repo = pathlib.Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("workdir")
parser.add_argument("sources", nargs="+")
parser.add_argument("--jobs", type=int, default=8)
args = parser.parse_args()
work = pathlib.Path(args.workdir).resolve()


def child_limits():
    # Mutants crash the program on purpose. With a piped core_pattern (systemd-coredump) the
    # kernel ignores a core limit of 0 but treats 1 as "do not dump".
    resource.setrlimit(resource.RLIMIT_CORE, (1, 1))
    # A mutant that allocates without bound must fail its own allocation (killed), not push the
    # machine into swap until the OOM killer ends the whole run.
    resource.setrlimit(resource.RLIMIT_DATA, (3 << 30, 3 << 30))


def run(cmd, cwd, timeout=None):
    """Exit code, or None on timeout. On timeout the command and everything it started are
    killed: a mutant that hangs leaves no programs behind holding memory."""
    child = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             preexec_fn=child_limits, start_new_session=True)
    try:
        return child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        kill_tree(child.pid)
        child.wait()
        return None


def changed_line(original, mutant):
    for number, (a, b) in enumerate(zip(original, mutant), start=1):
        if a != b:
            return number, a.strip(), b.strip()
    return None


def worker_copy(index):
    copy = work / f"worker{index}"
    shutil.rmtree(copy, ignore_errors=True)
    copy.mkdir(parents=True)
    for part in ("CMakeLists.txt", "plugin", "client", "tests"):
        source = repo / part
        (shutil.copytree if source.is_dir() else shutil.copy)(source, copy / part)
    assert run(["cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                "-DQT_NO_PRIVATE_MODULE_WARNING=ON"], copy) == 0
    assert run(["cmake", "--build", "build"], copy) == 0
    assert run(["tests/check.sh", "build"], copy, timeout=120) == 0, "baseline must pass"
    return copy


mutants = []
for source in args.sources:
    original = (repo / source).read_text().splitlines()
    mutant_dir = work / "mutants" / source.replace("/", "_")
    shutil.rmtree(mutant_dir, ignore_errors=True)
    mutant_dir.mkdir(parents=True)
    subprocess.run(["mutate", str(repo / source), "--mutantDir", str(mutant_dir), "--noCheck"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    for path in sorted(mutant_dir.iterdir()):
        change = changed_line(original, path.read_text().splitlines())
        if change is None or change[1].startswith(("//", "#include", "*")):
            continue
        mutants.append((source, path, change))

copies = queue.Queue()
with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    for copy in pool.map(worker_copy, range(args.jobs)):
        copies.put(copy)


def pristine_check(_):
    copy = copies.get()
    try:
        return run(["tests/check.sh", "build"], copy, timeout=120) == 0
    finally:
        copies.put(copy)


# A check that fails under load would count mutants as killed that are not: a false green.
with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    flaky = list(pool.map(pristine_check, range(2 * args.jobs))).count(False)
if flaky:
    sys.exit(f"check.sh failed {flaky} of {2 * args.jobs} unmutated runs under load; fix that first")
print(f"baseline: {2 * args.jobs} unmutated runs under load all passed", flush=True)


def judge(mutant):
    source, path, change = mutant
    copy = copies.get()
    target = copy / source
    pristine = (repo / source).read_text()
    try:
        target.write_text(path.read_text())
        if run(["cmake", "--build", "build"], copy, timeout=300) != 0:
            return "invalid", mutant
        # A timeout counts as killed: the check did not pass.
        return ("survived" if run(["tests/check.sh", "build"], copy, timeout=120) == 0
                else "killed"), mutant
    finally:
        target.write_text(pristine)
        copies.put(copy)


def key(mutant):
    # By content: mutant file names are indexes and mean something else once the source changes.
    return f"{mutant[0]}:{hashlib.sha1(mutant[1].read_bytes()).hexdigest()}"


journal_path = work / "verdicts.jsonl"
verdicts = {}
if journal_path.exists():
    for line in journal_path.read_text().splitlines():
        entry = json.loads(line)
        # Tests only ever get stronger between runs, so a survivor is judged again.
        if entry["verdict"] != "survived":
            verdicts[entry["mutant"]] = entry["verdict"]
pending = [m for m in mutants if key(m) not in verdicts]
print(f"{len(mutants) - len(pending)} mutants already judged, {len(pending)} to go", flush=True)

lock = threading.Lock()
with open(journal_path, "a") as journal, \
        concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    for done, (verdict, mutant) in enumerate(pool.map(judge, pending), start=1):
        with lock:
            verdicts[key(mutant)] = verdict
            journal.write(json.dumps({"mutant": key(mutant), "verdict": verdict}) + "\n")
            journal.flush()
        if done % 50 == 0:
            print(f"{done}/{len(pending)}", flush=True)

counts = {"killed": 0, "survived": 0, "invalid": 0}
survivors = []
for mutant in mutants:
    counts[verdicts[key(mutant)]] += 1
    if verdicts[key(mutant)] == "survived":
        survivors.append(mutant)

print(f"total {len(mutants)} {counts}")
valid = counts["killed"] + counts["survived"]
if valid:
    print(f"score {100 * counts['killed'] / valid:.1f}% of {valid} compiling mutants")
for source, path, (line, before, after) in survivors:
    print(f"SURVIVED {source}:{line}\n  - {before}\n  + {after}")
