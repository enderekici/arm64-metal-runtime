#!/usr/bin/env python3
"""Run safe, straight-line native/Metal A64 specimens with independently known exit bytes."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import resource
import signal
import subprocess

# Setup, measured instruction, result extraction, expected low byte of x0.
PROBES = {
    "adds": (["mov x1, #41"], "adds x0, x1, #1", [], 42),
    "udf": (["mov x0, #0"], "udf #0", [], -signal.SIGILL),
    "crc32b": (["mov w1, #1", "mov w0, #0"], "crc32b w0, w0, w1", [], 150),
    "sdot": (["movi v0.4s, #0", "movi v1.16b, #2", "movi v2.16b, #3"],
             "sdot v0.4s, v1.16b, v2.16b", ["umov w0, v0.s[0]"], 24),
    "smmla": (["movi v0.4s, #0", "movi v1.16b, #2", "movi v2.16b, #3"],
              "smmla v0.4s, v1.16b, v2.16b", ["umov w0, v0.s[0]"], 48),
    "bfdot": (["movi v0.4s, #0", "mov w9, #0x3f80", "dup v1.8h, w9"],
              "bfdot v0.4s, v1.8h, v1.8h", ["fcvtzs w0, s0"], 2),
    "sqrdmlah": (["movi v0.4s, #0", "mov w9, #0x40000000", "dup v1.4s, w9"],
                 "sqrdmlah v0.4s, v1.4s, v1.4s", ["umov w0, v0.s[0]", "lsr w0, w0, #28"], 2),
    "fjcvtzs": (["mov w9, #42", "scvtf d1, w9", "mov w0, #99"], "fjcvtzs w0, d1", [], 42),
}


def run(cmd, env):
    return subprocess.run(cmd, capture_output=True, env=env)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(args, directory, name, env):
    before, operation, after, expected = PROBES[name]
    directory.mkdir()
    src, exe = directory / "test.s", directory / "test"
    lines = [".text", ".globl _main", ".p2align 2", "_main:", *before,
             ".globl _specimen", "_specimen:", operation, *after, "ret"]
    src.write_text("\n".join(lines) + "\n")
    row = {"instruction": operation, "expected": expected}
    compile_cmd = ["xcrun", "clang", "-arch", "arm64", "-mcpu=native", "-nostdlib", "-Wl,-e,_main"]
    build = run([*compile_cmd, src, "-lSystem", "-o", exe], env)
    if build.returncode:
        row["compile_error"] = build.stderr.decode()
        return row
    disassembly = run(["xcrun", "llvm-objdump", "-d", exe], env)
    disassembly.check_returncode()
    text = disassembly.stdout.decode()
    main = int(re.search(r"([a-f0-9]+) <_main>:", text)[1], 16)
    specimen = re.search(r"([a-f0-9]+) <_specimen>:\s*\n\s*[a-f0-9]+:\s*([a-f0-9]{8})", text)
    offset, word = int(specimen[1], 16) - main, specimen[2]
    row.update(word=word, executable_sha256=sha(exe))
    native = run(["/usr/bin/timeout", "10", exe], env)
    (directory / "native.stdout").write_bytes(native.stdout)
    (directory / "native.stderr").write_bytes(native.stderr)
    row["native_status"] = native.returncode
    row["native_matches"] = native.returncode == expected and not native.stdout and not native.stderr
    if not row["native_matches"]:
        return row
    # Replacing only the specimen with NOP must change the native observation.
    mutant_source, mutant = directory / "noop.s", directory / "noop"
    mutant_source.write_text(src.read_text().replace("_specimen:\n" + operation + "\n",
                                                    "_specimen:\nnop\n"))
    run([*compile_cmd, mutant_source, "-lSystem", "-o", mutant], env).check_returncode()
    noop = run(["/usr/bin/timeout", "10", mutant], env)
    row["native_noop_status"] = noop.returncode
    if noop.returncode == expected or noop.returncode < 0 or noop.returncode == 124:
        row["oracle_error"] = "NOP mutation did not produce a distinct normal exit"
        return row
    for mode, enabled in [("translated", "0"), ("aot", "1")]:
        cmd = ["/usr/bin/timeout", "30", args.runtime, "launch", exe]
        if args.gpulock:
            cmd = ["/usr/bin/timeout", "300", args.gpulock, *cmd]
        result = run(cmd, {**env, "GPULOCK_SLOTS": str(args.slots), "ARM64_METAL_INTERPRETER": enabled,
                           "ARM64_METAL_INTERPRETER_LIBRARY": str(args.library),
                           "ARM64_METAL_TRACE": "1", "ARM64_METAL_TRACE_INTERPRETER_PACKETS": "1"})
        (directory / f"{mode}.stderr").write_bytes(result.stderr)
        (directory / f"{mode}.stdout").write_bytes(result.stdout)
        trace = result.stderr.decode(errors="replace")
        diagnostics = [line for line in trace.splitlines() if not line.startswith("arm64-metal trace ")]
        matched = result.returncode == expected and not result.stdout and not diagnostics
        if matched:
            status = "matched"
        elif result.returncode == 124:
            status = "timed-out"
        elif diagnostics:
            status = "runtime-diagnostic"
        else:
            status = "result-mismatch"
        if matched and expected == -signal.SIGILL:
            status = "matched-sigill"
        if result.returncode == 1 and any(
            re.fullmatch(rf"arm64-metal: Unsupported ARM64 instruction 0x{word} at __text\+0x[0-9a-f]+", d)
            for d in diagnostics
        ):
            status = "rejected-at-specimen"
        entry = re.search(r"event=image-load-done pc=(0x[0-9a-f]+)", trace)
        if mode == "aot" and matched and expected >= 0 and entry:
            pc = hex(int(entry[1], 16) + offset)
            # These probes have exactly one straight-line path from entry through the specimen.
            dispatches = re.findall(r"event=dispatch-start pc=(0x[0-9a-f]+) ", trace)
            typed = re.search(rf"event=interpreter-dispatch pc={entry[1]} detail=(\d+)", trace)
            if (dispatches == [entry[1]] and typed and int(typed[1]) > offset // 4 and
                    re.search(rf"event=interpreter-packet pc={pc} ", trace)):
                status = "matched-with-typed-packet"
        row[mode] = {"status": result.returncode, "outcome": status}
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "library", "output"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--gpulock", type=pathlib.Path)
    parser.add_argument("--slots", type=int, choices=range(1, 5), default=2)
    args = parser.parse_args()
    args.runtime, args.library = args.runtime.resolve(), args.library.resolve()
    args.output.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = {k: v for k, v in os.environ.items() if not k.startswith("ARM64_METAL_")}
    hashes = [sha(args.runtime), sha(args.library)]
    rows = {name: probe(args, args.output / name, name, env) for name in PROBES}
    immutable = hashes == [sha(args.runtime), sha(args.library)]
    report = {"runtime_sha256": hashes[0], "metallib_sha256": hashes[1], "immutable": immutable,
              "audit_checkout": subprocess.check_output(["git", "-C", pathlib.Path(__file__).resolve().parents[2],
                                                 "rev-parse", "HEAD"], text=True).strip(),
              "probes": rows}
    (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    positive, invalid = rows["adds"], rows["udf"]
    controls = (positive.get("translated", {}).get("outcome") == "matched" and
                positive.get("aot", {}).get("outcome") == "matched-with-typed-packet" and
                all(invalid.get(mode, {}).get("outcome") == "matched-sigill"
                    for mode in ("translated", "aot")))
    return 0 if immutable and controls and not any("oracle_error" in r for r in rows.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
