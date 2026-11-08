#!/usr/bin/env python3
import re
import sys

if len(sys.argv) != 9:
    raise SystemExit(
        "Usage: check-jit-trace.py STDOUT STDERR COMPILER CLASS METHOD MARKER STREAM TRACE"
    )

stdout_path, stderr_path, compiler, class_name, method_name, marker, stream, trace_required = sys.argv[1:]
stdout = open(stdout_path, encoding="utf-8", errors="replace").read()
stderr = open(stderr_path, encoding="utf-8", errors="replace").read()
marker_output = stdout if stream == "stdout" else stderr
assert marker in marker_output, "Java workload result marker missing"

compiled_header = re.search(
    rf"Compiled method \({re.escape(compiler)}\).*"
    rf"{re.escape(class_name)}::{re.escape(method_name)}"
    rf"(?: @ \d+)? \(\d+ bytes\)",
    stdout,
)
assert compiled_header, (
    f"{class_name}::{method_name} did not compile at {compiler}"
)
code_range = re.search(
    r"main code\s+\[0x([0-9a-f]+),0x([0-9a-f]+)\]",
    stdout[compiled_header.end():],
)
assert code_range, "compiled method code range missing"
start, end = (int(value, 16) for value in code_range.groups())
assert end > start

if trace_required == "1":
    events = re.findall(
        r"arm64-metal jit-trace event=dispatch pc=0x([0-9a-f]+) "
        r"owner=dynamic-vm-executable range=0x([0-9a-f]+)-0x([0-9a-f]+) "
        r"backing=0x([0-9a-f]+) generation=(\d+) map-jit=(true|false) "
        r"backend=(gpu-translated|gpu-interpreter)",
        stderr,
    )
    dispatched = any(
        start <= int(pc, 16) < end
        and int(owner_begin, 16) <= int(pc, 16) < int(owner_end, 16)
        for pc, owner_begin, owner_end, *_ in events
    )
    assert dispatched, (
        f"runtime did not dispatch {class_name}::{method_name} from its "
        "owned executable VM mapping"
    )
