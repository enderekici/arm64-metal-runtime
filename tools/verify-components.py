#!/usr/bin/env python3
"""Run focused native-vs-Metal process ABI fixtures from component-manifest.json."""

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import re
import resource
import signal
import subprocess
import sys
import time
import uuid


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def host_info():
    values = {"machine": platform.machine(), "platform": platform.platform()}
    for key, command in {
        "os_product_version": ["sw_vers", "-productVersion"],
        "os_build": ["sw_vers", "-buildVersion"],
        "hardware_model": ["sysctl", "-n", "hw.model"],
        "cpu_brand": ["sysctl", "-n", "machdep.cpu.brand_string"],
    }.items():
        try:
            values[key] = subprocess.check_output(command, text=True, timeout=5).strip()
        except (OSError, subprocess.SubprocessError):
            values[key] = "unavailable"
    return values


def normalized_status(returncode):
    return returncode if returncode >= 0 else 128 - returncode


def run_bounded(command, environment, timeout_seconds, stdout_path, stderr_path, cwd,
                start_marker=None, lock_wait_seconds=None):
    started = time.monotonic()
    status = "NOT_STARTED"
    raw_returncode = None
    try:
        with open(stdout_path, "wb") as stdout, open(stderr_path, "wb") as stderr:
            process = subprocess.Popen(
                command,
                cwd=cwd,
                env=environment,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
            )
            execution_started = started if start_marker is None else None
            while True:
                raw_returncode = process.poll()
                if raw_returncode is not None:
                    status = normalized_status(raw_returncode)
                    break
                now = time.monotonic()
                if start_marker is not None and pathlib.Path(start_marker).exists():
                    if execution_started is None:
                        execution_started = now
                if execution_started is not None:
                    timed_out = now - execution_started >= timeout_seconds
                else:
                    timed_out = now - started >= (
                        lock_wait_seconds if lock_wait_seconds is not None else timeout_seconds
                    )
                if timed_out:
                    status = "GPU_TIMEOUT" if execution_started is not None and start_marker else (
                        "GPU_LOCK_WAIT_TIMEOUT" if start_marker else "TIMEOUT"
                    )
                    raw_returncode = None
                    break
                time.sleep(0.02)
            if raw_returncode is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait()
    except OSError as error:
        pathlib.Path(stderr_path).write_text(f"{error}\n")
        status = "LAUNCH_ERROR"
    pathlib.Path(stdout_path).with_suffix(".status").write_text(f"{status}\n")
    pathlib.Path(stdout_path).with_suffix(".returncode").write_text(
        f"{raw_returncode if raw_returncode is not None else status}\n"
    )
    return {
        "status": status,
        "returncode": raw_returncode,
        "duration_seconds": round(time.monotonic() - started, 3),
    }


def substitute(values, replacements):
    return [str(value).format_map(replacements) for value in values]


def same_files(first, second):
    return pathlib.Path(first).read_bytes() == pathlib.Path(second).read_bytes()


def execute_case(case, root, runtime, gpulock, results_dir, repo):
    case_dir = results_dir / case["id"]
    case_dir.mkdir(parents=True)
    source = (repo / case["fixture"]).resolve()
    executable = case_dir / source.stem
    replacements = {"repo": str(repo), "source": str(source), "executable": str(executable), "case_dir": str(case_dir)}
    compile_command = substitute(case["compile"], replacements)
    input_artifacts = []
    for input_path in case.get("input_files", []):
        resolved_input = pathlib.Path(input_path.format_map(replacements)).resolve()
        input_artifacts.append({
            "path": str(resolved_input),
            "size_bytes": resolved_input.stat().st_size,
            "sha256": sha256(resolved_input),
        })
    base_environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith("ARM64_METAL_") or key == "ARM64_METAL_KERNEL_CACHE"
    }

    compile_result = run_bounded(
        compile_command, base_environment, case.get("compile_timeout_seconds", 120),
        case_dir / "compile.stdout", case_dir / "compile.stderr", repo,
    )
    result = {
        "id": case["id"],
        "domain": case["domain"],
        "fixture": case["fixture"],
        "fixture_sha256": sha256(source),
        "input_artifacts": input_artifacts,
        "compiled_executable": str(executable),
        "compiled_executable_sha256": sha256(executable) if executable.exists() else None,
        "compile_command": compile_command,
        "compile": compile_result,
        "native_command": None,
        "guest_command": None,
        "expected_status": case["expected_status"],
        "polarity": case["polarity"],
        "negative_control": case["negative_control"],
        "evidence_level": case["evidence_level"],
        "required_launch_flags": case["required_launch_flags"],
        "effective_guest_environment": {
            **({"ARM64_METAL_KERNEL_CACHE": base_environment["ARM64_METAL_KERNEL_CACHE"]}
               if "ARM64_METAL_KERNEL_CACHE" in base_environment else {}),
            **case["required_launch_flags"],
        },
        "evidence": "not_observed",
        "not_run": True,
        "outcome": "failed",
    }
    if compile_result["status"] != 0:
        result["failure"] = "fixture compile failed"
        (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
        return result

    native_args = substitute(case.get("native_args", []), replacements)
    guest_args = substitute(case.get("guest_args", []), replacements)
    native_command = [str(executable), *native_args]
    result["native_command"] = native_command

    native_result = run_bounded(
        native_command, base_environment, case["timeout_seconds"],
        case_dir / "native.stdout", case_dir / "native.stderr", repo,
    )
    result["native"] = native_result
    expected_returncode = case.get("expected_returncode")
    native_stdout_lines = (case_dir / "native.stdout").read_bytes().count(b"\n")
    result["native_stdout_lines"] = native_stdout_lines
    if (native_result["status"] != case["expected_status"]
            or (expected_returncode is not None and native_result["returncode"] != expected_returncode)
            or ("expected_stdout_lines" in case
                and native_stdout_lines != case["expected_stdout_lines"])):
        result["failure"] = "native positive control did not match manifest status"
        result["evidence"] = "native_control_failed; guest_not_run"
        result["guest"] = {"status": "NOT_RUN_NATIVE_CONTROL_FAILED"}
        result["not_run"] = True
        (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
        return result

    guest_environment = base_environment.copy()
    guest_environment["GPULOCK_SLOTS"] = str(root["gpu_slots"])
    start_marker = case_dir / "guest-slot-acquired"
    guest_invocation = ["env", *[
        f"{key}={value}" for key, value in case["required_launch_flags"].items()
    ], str(runtime), "launch", str(executable), *guest_args]
    if gpulock:
        guest_command = [
            str(gpulock), "sh", "-c",
            'printf acquired > "$1"; shift; exec "$@"',
            "component-run", str(start_marker), *guest_invocation,
        ]
    else:
        guest_command = guest_invocation
        start_marker = None
    result["guest_command"] = guest_command
    result["not_run"] = False
    guest_result = run_bounded(
        guest_command, guest_environment, case["timeout_seconds"],
        case_dir / "guest.stdout", case_dir / "guest.stderr", repo,
        start_marker if gpulock else None,
        root["gpu_lock_wait_seconds"] if gpulock else None,
    )
    if guest_result["status"] in ("GPU_LOCK_WAIT_TIMEOUT", "GPU_TIMEOUT", "TIMEOUT"):
        execution_timeout = guest_result["status"] in ("GPU_TIMEOUT", "TIMEOUT")
        result["not_run"] = not execution_timeout
        result["failure"] = "guest run exceeded its execution bound" if execution_timeout else "shared GPU lock wait exceeded its bound"
        result["evidence"] = "native_control_passed;guest_timeout" if execution_timeout else "native_control_passed;guest_not_started"
        result["guest_outcome"] = "guest_timeout" if execution_timeout else "gpu_lock_wait_timeout"
        pathlib.Path(case_dir / "guest.status").write_text(f"{guest_result['status']}\n")
        result["guest"] = guest_result
        (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
    result["guest"] = guest_result
    stdout_equal = same_files(case_dir / "native.stdout", case_dir / "guest.stdout")
    stderr_equal = same_files(case_dir / "native.stderr", case_dir / "guest.stderr")
    stderr_policy_satisfied = stderr_equal
    if "guest_stderr_regex" in case:
        native_stderr = (case_dir / "native.stderr").read_text()
        guest_stderr = (case_dir / "guest.stderr").read_text()
        stderr_policy_satisfied = (
            native_stderr == case.get("native_stderr_exact", "")
            and re.fullmatch(case["guest_stderr_regex"], guest_stderr) is not None
        )
    status_equal = native_result["status"] == guest_result["status"]
    returncode_equal = native_result["returncode"] == guest_result["returncode"]
    guest_stdout_lines = (case_dir / "guest.stdout").read_bytes().count(b"\n")
    stdout_line_count_match = (
        "expected_stdout_lines" not in case
        or (native_stdout_lines == case["expected_stdout_lines"]
            and guest_stdout_lines == case["expected_stdout_lines"])
    )
    result["guest_stdout_lines"] = guest_stdout_lines
    result["comparison"] = {
        "stdout_equal": stdout_equal,
        "stderr_equal": stderr_equal,
        "stderr_policy_satisfied": stderr_policy_satisfied,
        "status_equal": status_equal,
        "returncode_equal": returncode_equal,
        "stdout_line_count_match": stdout_line_count_match,
        "stderr_policy": "exact" if stderr_equal else "manifest guest diagnostic accepted",
    }
    if not (stdout_equal and stdout_line_count_match and stderr_policy_satisfied and status_equal and returncode_equal
            and guest_result["status"] == case["expected_status"]
            and (expected_returncode is None or guest_result["returncode"] == expected_returncode)):
        result["failure"] = "native/guest result mismatch or unexpected guest status"
        result["evidence"] = "native_control_passed;guest_mismatch"
        result["guest_outcome"] = "guest_timeout" if guest_result["status"] == "GPU_TIMEOUT" else "abi_mismatch"
    else:
        if stderr_equal:
            result["outcome"] = "passed"
            result["evidence"] = case["evidence_level"]
        else:
            result["outcome"] = "passed_with_diagnostic"
            result["evidence"] = "stdout, normalized status, and raw returncode match; launcher diagnostic accepted by manifest"
    (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=pathlib.Path)
    parser.add_argument("--results", default=pathlib.Path(".build/component-conformance"), type=pathlib.Path)
    parser.add_argument("--manifest", default=pathlib.Path("component-manifest.json"), type=pathlib.Path)
    parser.add_argument("--case", action="append", dest="cases", help="case id; repeat to select several")
    parser.add_argument("--jobs", type=int, default=2, help="bounded concurrent GPU test lanes (1-4)")
    parser.add_argument("--gpu-slots", type=int, default=4, help="shared gpulock slot count")
    parser.add_argument("--gpulock", type=pathlib.Path,
        default=pathlib.Path(os.environ["ARM64_METAL_GPULOCK"]) if os.environ.get("ARM64_METAL_GPULOCK") else None)
    parser.add_argument("--gpu-lock-wait", type=int, default=300)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    manifest_path = (repo / args.manifest).resolve() if not args.manifest.is_absolute() else args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text())
    available = manifest["cases"]
    if args.list:
        for case in available:
            print(f"{case['id']}\t{case['domain']}\t{case['polarity']}\t{case['timeout_seconds']}s")
        return 0
    if args.runtime is None:
        parser.error("--runtime is required unless --list is used")
    if args.jobs < 1 or args.jobs > 4 or args.gpu_slots < 1 or args.gpu_slots > 4 or args.jobs > args.gpu_slots:
        parser.error("jobs and gpu-slots must be 1..4, with jobs <= gpu-slots")
    if args.gpu_lock_wait < 1:
        parser.error("gpu-lock-wait must be at least one second")

    runtime = args.runtime.resolve()
    gpulock = args.gpulock.resolve() if args.gpulock else None
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        parser.error(f"runtime is not executable: {runtime}")
    if gpulock and (not gpulock.is_file() or not os.access(gpulock, os.X_OK)):
        parser.error(f"shared GPU lock wrapper is unavailable: {gpulock}")

    wanted = set(args.cases or [case["id"] for case in available
                                if case.get("default_enabled", True)])
    unknown = wanted - {case["id"] for case in available}
    if unknown:
        parser.error(f"unknown cases: {', '.join(sorted(unknown))}")
    selected = [case for case in available if case["id"] in wanted]

    args.results.mkdir(parents=True, exist_ok=True)
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    run_dir = args.results.resolve() / f"run-{run_id}"
    run_dir.mkdir()
    copied_manifest = run_dir / "component-manifest.json"
    copied_manifest.write_bytes(manifest_path.read_bytes())
    copied_manifest.chmod(0o444)
    runtime_hash = sha256(runtime)
    metadata = {
        "run_id": run_id,
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repo": str(repo),
        "source_revision": subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "runtime": str(runtime),
        "runtime_sha256_before": runtime_hash,
        "manifest_sha256": sha256(copied_manifest),
        "host": host_info(),
        "jobs": args.jobs,
        "gpu_slots": args.gpu_slots,
        "core_dump_limit": list(resource.getrlimit(resource.RLIMIT_CORE)),
        "gpulock": str(gpulock) if gpulock else None,
        "selected_case_ids": [case["id"] for case in selected],
    }
    run_file = run_dir / "run.json"
    run_file.write_text(json.dumps(metadata, indent=2) + "\n")
    run_file.chmod(0o444)

    (run_dir / "results_root.txt").write_text(str(run_dir) + "\n")
    print(f"component run={run_dir}")
    print(f"runtime_sha256={runtime_hash} host={metadata['host']['os_product_version']}/{metadata['host']['os_build']} {metadata['host']['hardware_model']}")

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        future_cases = {
            pool.submit(execute_case, case, {"gpu_slots": args.gpu_slots, "gpu_lock_wait_seconds": args.gpu_lock_wait}, runtime, gpulock, run_dir, repo): case
            for case in selected
        }
        for future in concurrent.futures.as_completed(future_cases):
            case = future_cases[future]
            try:
                result = future.result()
            except Exception as error:
                result = {
                    "id": case["id"],
                    "domain": case["domain"],
                    "polarity": case["polarity"],
                    "outcome": "failed",
                    "failure": repr(error),
                    "evidence": "not_observed",
                    "not_run": True,
                }
                (run_dir / case["id"]).mkdir(exist_ok=True)
                (run_dir / case["id"] / "case.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            print(f"{result['id']}: {result['outcome']} ({result.get('evidence', 'not_observed')})", flush=True)

    runtime_hash_after = sha256(runtime)
    counts = {
        "selected": len(selected),
        "passed_exact": sum(result["outcome"] == "passed" for result in results),
        "passed_with_diagnostic": sum(result["outcome"] == "passed_with_diagnostic" for result in results),
        "failed": sum(result["outcome"] == "failed" for result in results),
        "passed_positive": sum(result["outcome"] == "passed" and not result["polarity"].startswith("negative") for result in results),
        "passed_expected_negative": sum(result["outcome"] == "passed" and result["polarity"].startswith("negative") for result in results),
        "not_run": sum(bool(result.get("not_run")) for result in results),
    }
    summary = {
        "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "counts": counts,
        "runtime_sha256_before": runtime_hash,
        "runtime_sha256_after": runtime_hash_after,
        "runtime_immutable_during_run": runtime_hash == runtime_hash_after,
        "case_outcomes": [{"id": result["id"], "outcome": result["outcome"], "evidence": result.get("evidence")} for result in results],
        "supported_claims": [result["id"] for result in results if result["outcome"] == "passed"],
        "diagnosed_matches": [result["id"] for result in results if result["outcome"] == "passed_with_diagnostic"],
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("counts=" + json.dumps(counts, sort_keys=True))
    print(f"runtime_immutable={summary['runtime_immutable_during_run']}")
    print(f"summary={run_dir / 'summary.json'}")
    return 0 if counts["failed"] == 0 and summary["runtime_immutable_during_run"] else 1


if __name__ == "__main__":
    sys.exit(main())
