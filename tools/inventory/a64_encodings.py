#!/usr/bin/env python3
"""Inventory Arm A64 encoding records and actual Mach-O text words against the process decoder."""
import argparse
import collections
import hashlib
import json
import pathlib
import platform
import re
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
A64 = "Instruction.Instruction"
ALIAS = "Instruction.InstructionAlias"
FILL = (0x01234567, 0x9abcdef0)
WORD = re.compile(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.+)")


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def predicates(node):
    """Preserve Boolean structure and field constraints beside each fixed-bit mask."""
    t = node["_type"]
    if t == "AST.Bool":
        return str(node["value"]).lower()
    if t in ("AST.Identifier", "Values.Value"):
        return node["value"]
    if t == "AST.Function" and node["name"] == "IsFeatureImplemented":
        return predicates(node["arguments"][0])
    if t == "AST.BinaryOp":
        return f'({predicates(node["left"])} {node["op"]} {predicates(node["right"])})'
    if t == "AST.UnaryOp":
        return f'{node["op"]}({predicates(node["expr"])})'
    if t == "AST.Set":
        return "{" + ", ".join(map(predicates, node["values"])) + "}"
    raise ValueError(f"Unsupported Arm condition {t}")


def encodings(node, ancestors=(), mask=0, value=0, conditions=()):
    if node["_type"] == ALIAS:
        return
    for field in node["encoding"]["values"]:
        bits = field["value"]["value"].strip("'")
        origin = field["range"]["start"]
        if len(bits) != field["range"]["width"] or re.fullmatch("[01x]+", bits) is None:
            raise ValueError(f"Unsupported Arm bit pattern {bits!r}")
        m = int("".join("0" if c == "x" else "1" for c in bits), 2) << origin
        v = int(bits.replace("x", "0"), 2) << origin
        if (value ^ v) & mask & m:
            raise ValueError(f'Conflicting parent mask in {node["name"]}')
        mask |= m
        value |= v
    cond = predicates(node["condition"])
    if cond != "true" and cond not in conditions:
        conditions += (cond,)
    if node["_type"] == A64:
        yield {
            "record": node["name"], "group": ancestors[1], "parents": ancestors,
            "conditions": conditions, "mask": f"{mask:08x}", "fixed": f"{value:08x}",
            "samples": list(dict.fromkeys(f"{value | (f & ~mask):08x}" for f in FILL)),
        }
    for child in node.get("children", []):
        yield from encodings(child, ancestors + (node["name"],), mask, value, conditions)


def decoder(words, directory):
    executable = directory / "a64-decode"
    cpp = REPO / "cpp"
    sources = ["process_decoder.cpp", "guest_strings.cpp", "guest_vector_crypto.cpp",
               "translate_core.cpp"]
    subprocess.run(["xcrun", "clang++", "-std=c++17", "-O2", "-I", cpp,
                    REPO / "tools/inventory/a64-decode.cpp", *(cpp / s for s in sources),
                    "-o", executable], check=True)
    words = sorted(words)
    result = subprocess.run([executable], input="\n".join(words) + "\n", text=True,
                            check=True, capture_output=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(words):
        raise ValueError("Incomplete decoder output")
    return dict(zip(words, lines))


def text_words(binary, directory):
    archs = run("xcrun", "lipo", "-archs", binary).split()
    arch = next((a for a in ("arm64", "arm64v8", *archs)
                 if a in archs and (a in ("arm64", "arm64v8") or a.startswith("arm64e"))), None)
    if not arch:
        raise ValueError(f"{binary} has no A64 slice")
    selected = binary
    if len(archs) > 1:
        selected = directory / "slice"
        run("xcrun", "lipo", binary, "-thin", arch, "-output", selected)
    lines = run("xcrun", "llvm-objdump", "-d", "--section=__text", selected).splitlines()
    words = collections.Counter()
    assembly = {}
    for line in lines:
        match = WORD.match(line)
        if match:
            word, asm = match.groups()
            words[word] += 1
            assembly[word] = asm.strip()
    if not words:
        raise ValueError(f"No __text disassembly in {binary}")
    return {"path": str(binary), "sha256": sha(binary), "slice": arch,
            "words": {w: [count, assembly[w]] for w, count in sorted(words.items())}}


def fixture_words(directory):
    fixtures = []
    for source in sorted((REPO / "fixtures/interpreter").iterdir()):
        if source.suffix not in (".s", ".S"):
            continue
        options = [[]]
        if source.name == "scalar-fp-compare-cases.S":
            options = [[f"-DFP_CASE={i}"] for i in range(1, 6)]
        for flags in options:
            obj = directory / "fixture.o"
            compile_result = subprocess.run(
                ["xcrun", "clang", "-arch", "arm64", "-mcpu=native", *flags, "-c", source, "-o", obj],
                text=True, capture_output=True)
            row = {"source": str(source.relative_to(REPO)), "source_sha256": sha(source),
                   "compile_flags": flags}
            if compile_result.returncode:
                row["compile_error"] = compile_result.stderr.strip()
            else:
                obj_row = text_words(obj, directory)
                del obj_row["path"]
                row.update(obj_row)
            fixtures.append(row)
    return fixtures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=pathlib.Path, required=True, help="Arm Instructions.json")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--fixture-corpus", action="store_true",
                        help="also assemble all fixtures/interpreter .s/.S files")
    parser.add_argument("binaries", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    spec = json.loads(args.spec.read_bytes())
    root = next(n for n in spec["instructions"] if n["name"] == "A64")
    rows = list(encodings(root))
    aliases = sum(n["_type"] == ALIAS for n in walk(root))
    sample_words = {w for row in rows for w in row["samples"]}
    with tempfile.TemporaryDirectory() as temp:
        directory = pathlib.Path(temp)
        inputs = [text_words(b.resolve(), directory) for b in args.binaries]
        fixtures = fixture_words(directory) if args.fixture_corpus else []
        words = sample_words.union(*(item.get("words", {}) for item in inputs + fixtures))
        classified = decoder(words, directory)
    for row in rows:
        row["samples"] = {w: classified[w] for w in row["samples"]}
    for item in inputs + fixtures:
        if "words" not in item:
            continue
        counts = collections.Counter()
        for w, data in item["words"].items():
            data.append(classified[w])
            counts[classified[w].split()[0]] += data[0]
        item["static_occurrences"] = dict(counts)
    sysctls = {}
    for line in run("sysctl", "-a").splitlines():
        key, sep, value = line.partition(": ")
        if sep and (key.startswith("hw.optional.arm.FEAT_") or
                    key in ("hw.optional.floatingpoint", "hw.optional.neon", "hw.model")):
            sysctls[key] = value
    known_features = {k.rsplit(".", 1)[1]: int(v) for k, v in sysctls.items()
                      if k.startswith("hw.optional.arm.FEAT_")}
    for arm, apple in (("FEAT_AdvSIMD", "hw.optional.neon"), ("FEAT_FP", "hw.optional.floatingpoint")):
        if apple in sysctls:
            known_features[arm] = int(sysctls[apple])
    for row in rows:
        referenced = set(re.findall(r"\bFEAT_\w+", " ".join(row["conditions"])))
        row["feature_reports"] = {name: known_features.get(name) for name in sorted(referenced)}
    summary = {
        "a64_instruction_records": len(rows), "a64_alias_records": aliases,
        "groups": dict(collections.Counter(r["group"] for r in rows)),
        "fixture_objects": len(fixtures),
        "fixture_compile_failures": sum("compile_error" in f for f in fixtures),
        "binary_inputs": len(inputs),
        "distinct_synthetic_words": len(sample_words),
        "synthetic_word_outcomes": dict(collections.Counter(
            classified[w].split()[0] for w in sample_words)),
    }
    report = {
        "arm_release": spec["_meta"]["version"], "instructions_sha256": sha(args.spec),
        "source": run("git", "-C", REPO, "rev-parse", "HEAD"),
        "tracked_changes": run("git", "-C", REPO, "status", "--porcelain", "-uno"),
        "host": platform.platform(), "clang": run("xcrun", "clang", "--version").splitlines()[0],
        "sysctls": sysctls, "fill_words": [f"{f:08x}" for f in FILL], "summary": summary,
        "interpretation": [
            "Rows enumerate Arm A64 instruction records, including privileged and optional forms.",
            "Feature reports are macOS sysctl values (null = unpublished). A reported feature does not establish EL0 access or evaluated encoding conditions.",
            "Samples combine fixed bits with deterministic fills. Field predicates, reserved operand values, exception level and behavior are unmeasured.",
            "typed-ir means the process decoder supplied a typed operation. Block planning and actual Metal dispatch are unmeasured.",
            "translated-ir means a lowering exists without a typed operation. Special traps and host service boundaries can also emit IR.",
            "Binary entries are static text occurrences, including embedded data or unused paths. They do not imply execution. Decoder rejections include BRK/UDF words handled as traps by the outer process runner.",
            "Each binary word maps to [occurrences, disassembly, decoder result]; PC for static decoding is 0x100000000, without imports or stubs.",
        ],
        "encodings": rows, "binaries": inputs, "fixture_objects": fixtures,
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, sort_keys=True))


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


if __name__ == "__main__":
    main()
