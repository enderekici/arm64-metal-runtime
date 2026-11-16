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


def run_bounded(command, environment, timeout_seconds, stdout_path, stderr_path, cwd):
    started = time.monotonic()
    status = "NOT_STARTED"
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
            try:
                status = normalized_status(process.wait(timeout=timeout_seconds))
            except subprocess.TimeoutExpired:
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
                status = "TIMEOUT"
    except OSError as error:
        pathlib.Path(stderr_path).write_text(f"{error}\n")
        status = "LAUNCH_ERROR"
    pathlib.Path(str(stdout_path) + ".status").write_text(f"{status}\n")
    return {"status": status, "duration_seconds": round(time.monotonic() - started, 3)}


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
    base_environment = os.environ.copy()

    compile_result = run_bounded(
        compile_command, base_environment, case.get("compile_timeout_seconds", 120),
        case_dir / "compile.stdout", case_dir / "compile.stderr", repo,
    )
    result = {
        "id": case["id"],
        "domain": case["domain"],
        "fixture": case["fixture"],
        "fixture_sha256": sha256(source),
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
        "evidence": "not_observed",
        "outcome": "failed",
    }
    if compile_result["status"] != 0:
        result["failure"] = "fixture compile failed"
        (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
        return result

    native_args = substitute(case.get("native_args", []), replacements)
    guest_args = substitute(case.get("guest_args", []), replacements)
    native_command = [str(executable), *native_args]
    guest_command = [str(gpulock), "env", *[
        f"{key}={value}" for key, value in case["required_launch_flags"].items()
    ], str(runtime), "launch", str(executable), *guest_args]
    result["native_command"] = native_command
    result["guest_command"] = guest_command

    native_result = run_bounded(
        native_command, base_environment, case["timeout_seconds"],
        case_dir / "native.stdout", case_dir / "native.stderr", repo,
    )
    result["native"] = native_result
    if native_result["status"] != case["expected_status"]:
        result["failure"] = "native positive control did not match manifest status"
        result["evidence"] = "native_control_failed; guest_not_run"
        (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
        return result

    guest_environment = base_environment.copy()
    guest_environment["GPULOCK_SLOTS"] = str(root["gpu_slots"])
    guest_result = run_bounded(
        guest_command, guest_environment, case["timeout_seconds"] + root["gpu_lock_wait_seconds"],
        case_dir / "guest.stdout", case_dir / "guest.stderr", repo,
    )
    result["guest"] = guest_result
    stdout_equal = same_files(case_dir / "native.stdout", case_dir / "guest.stdout")
    stderr_equal = same_files(case_dir / "native.stderr", case_dir / "guest.stderr")
    status_equal = native_result["status"] == guest_result["status"]
    result["comparison"] = {
        "stdout_equal": stdout_equal,
        "stderr_equal": stderr_equal,
        "status_equal": status_equal,
    }
    if not (stdout_equal and stderr_equal and status_equal and guest_result["status"] == case["expected_status"]):
        result["failure"] = "native/guest result mismatch or unexpected guest status"
        result["evidence"] = "native_positive_control_passed;_guest_mismatch"
    else:
        result["outcome"] = "passed"
        result["evidence"] = case["evidence_level"]
    (case_dir / "case.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=pathlib.Path)
    parser.add_argument("--results", default=pathlib.Path(".build/component-conformance"), type=pathlib.Path)
    parser.add_argument("--manifest", default=pathlib.Path("component-manifest.json"), type=pathlib.Path)
    parser.add_argument("--case", action="append", dest="cases", help="case id; repeat to select several")
    parser.add_argument("--jobs", type=int, default=2, help="bounded concurrent GPU test lanes (1-4)")
    parser.add_argument("--gpu-slots", type=int, default=4, help="shared gpulock slot count")
    parser.add_argument("--gpulock", type=pathlib.Path,
                        default=pathlib.Path(os.environ.get("ARM64_METAL_GPULOCK", "/Users/ender/code/evamcep/arm64-metal-work/bin/gpulock")))
    parser.add_argument("--gpu-lock-wait", type=int, default=1800)
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

    runtime = args.runtime.resolve()
    gpulock = args.gpulock.resolve()
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        parser.error(f"runtime is not executable: {runtime}")
    if not gpulock.is_file() or not os.access(gpulock, os.X_OK):
        parser.error(f"shared GPU lock wrapper is unavailable: {gpulock}")

    wanted = set(args.cases or [case["id"] for case in available])
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
        "gpulock": str(gpulock),
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
                result = {"id": case["id"], "domain": case["domain"], "outcome": "failed", "failure": repr(error), "evidence": "not_observed"}
                (run_dir / case["id"]).mkdir(exist_ok=True)
                (run_dir / case["id"] / "case.json").write_text(json.dumps(result, indent=2) + "\n")
            results.append(result)
            print(f"{result['id']}: {result['outcome']} ({result.get('evidence', 'not_observed')})", flush=True)

    runtime_hash_after = sha256(runtime)
    counts = {
        "selected": len(selected),
        "passed": sum(result["outcome"] == "passed" for result in results),
        "failed": sum(result["outcome"] != "passed" for result in results),
        "passed_positive": sum(result["outcome"] == "passed" and not result["polarity"].startswith("negative") for result in results),
        "passed_expected_negative": sum(result["outcome"] == "passed" and result["polarity"].startswith("negative") for result in results),
        "not_run": sum(result.get("evidence") == "not_observed" for result in results),
    }
    summary = {
        "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "counts": counts,
        "runtime_sha256_before": runtime_hash,
        "runtime_sha256_after": runtime_hash_after,
        "runtime_immutable_during_run": runtime_hash == runtime_hash_after,
        "case_outcomes": [{"id": result["id"], "outcome": result["outcome"], "evidence": result.get("evidence")} for result in results],
        "supported_claims": [result["id"] for result in results if result["outcome"] == "passed"],
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("counts=" + json.dumps(counts, sort_keys=True))
    print(f"runtime_immutable={summary['runtime_immutable_during_run']}")
    print(f"summary={run_dir / 'summary.json'}")
    return 0 if counts["failed"] == 0 and summary["runtime_immutable_during_run"] else 1


if __name__ == "__main__":
    sys.exit(main())
