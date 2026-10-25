#!/usr/bin/env python3
"""Gate genuine Windows-generated MinGW gcov data against one exact source."""
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys

Root = Path(__file__).resolve().parents[2]
Scopes = {"icon_windows": ("icon_fixture", Root / "src/interop/icon_windows.c")}
if len(sys.argv) != 3 or sys.argv[1] not in Scopes:
    raise SystemExit("usage: native_component_coverage.py icon_windows PROFILE_DIRECTORY")
name = sys.argv[1]
directory = Path(sys.argv[2]).resolve(strict=True)
object_name, source = Scopes[name]
notes = directory / (object_name + ".gcno")
data = directory / (object_name + ".gcda")
if not notes.is_file() or not data.is_file():
    raise SystemExit("missing genuine Windows gcda or matching compiler gcno")
inputs = json.loads((directory / "inputs.json").read_text())
for relative, expected in inputs["sha256"].items():
    candidate = Root / relative
    if candidate.is_file() and hashlib.sha256(candidate.read_bytes()).hexdigest() != expected:
        raise SystemExit("profile input hash mismatch: " + relative)
command = ["x86_64-w64-mingw32-gcov", "--json-format", "-b", "-c", str(notes)]
result = subprocess.run(command, cwd=directory, text=True, capture_output=True)
(directory / (name + "-gcov.log")).write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.returncode)
report_path = directory / (object_name + ".gcov.json.gz")
report = json.loads(gzip.decompress(report_path.read_bytes()))
files = [item for item in report["files"] if Path(item["file"]).resolve() == source.resolve()]
if len(files) != 1:
    raise SystemExit("expected exactly one complete native production source scope")
lines = files[0]["lines"]
arcs = [(line["line_number"], arc) for line in lines for arc in line.get("branches", [])]
summary = {"scope": str(source.relative_to(Root)), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
           "gcno_sha256": hashlib.sha256(notes.read_bytes()).hexdigest(),
           "gcda_sha256": hashlib.sha256(data.read_bytes()).hexdigest(), "command": command}
failed = False
for kind, entries in (("lines", [(line["line_number"], line) for line in lines]), ("taken_arcs", arcs)):
    if not entries:
        raise SystemExit("empty native source coverage scope")
    covered = sum(entry["count"] > 0 for _, entry in entries)
    percent = 100 * covered / len(entries)
    summary[kind] = {"covered": covered, "total": len(entries), "percent": percent,
                     "missed": [line for line, entry in entries if not entry["count"]]}
    print(f"{name} native {kind}: {covered}/{len(entries)} ({percent:.2f}%)", flush=True)
    failed |= percent < 90
(directory / (name + "-summary.json")).write_text(json.dumps(summary, indent=2) + "\n")
if failed:
    raise SystemExit("native production coverage remains below90%; acceptance stays open")
