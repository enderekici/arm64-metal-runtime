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
        r"event=(compile-start|dispatch-start) pc=0x([0-9a-f]+)", stderr
    )
    compiled = any(
        event == "compile-start" and start <= int(pc, 16) < end
        for event, pc in events
    )
    dispatched = any(
        event == "dispatch-start" and start <= int(pc, 16) < end
        for event, pc in events
    )
    assert compiled, f"runtime did not translate {class_name}::{method_name} code"
    assert dispatched, f"runtime did not dispatch {class_name}::{method_name} code"
