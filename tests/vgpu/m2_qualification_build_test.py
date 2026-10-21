#!/usr/bin/env python3
"""Read actual PE exports and COFF definitions; never execute Windows code.

The ordinary DLL must match its checked-in definition exactly. The qualified DLL
adds exactly two read-only queries. Fault controls and accidental observer exports
fail closed. Tools/read failures propagate; a build-only PASS is not execution.
"""
import argparse
from pathlib import Path
import re
import subprocess


def exports(path):
    """Return unique actual PE export names; reject absent/malformed tables."""
    text = subprocess.check_output(["objdump", "-p", str(path)], text=True)
    marker = "[Ordinal/Name Pointer] Table"
    if text.count(marker) != 1:
        raise ValueError("missing or ambiguous export name table: " + str(path))
    suffix = text.split(marker, 1)[1]
    heading, body = suffix.split("\n", 1)
    if heading.strip() and not re.fullmatch(r" -- Ordinal Base [0-9]+", heading):
        raise ValueError("unexpected export table heading")
    body = body.split("\n\n", 1)[0]
    names = []
    for line in body.splitlines():
        if line.split() == ["Ordinal", "Hint", "Name"]:
            continue
        match = re.fullmatch(r"\s*\[\s*[0-9]+\] (?:\+base\[\s*[0-9]+\]  [0-9a-fA-F]+ )?([A-Za-z_][A-Za-z0-9_]*)", line)
        if not match:
            raise ValueError("malformed export name: " + repr(line))
        names.append(match[1])
    if not names or len(names) != len(set(names)):
        raise ValueError("empty or duplicate export names: " + str(path))
    return set(names)


def definitions(path):
    """Return externally defined COFF names; tool failure is fatal."""
    text = subprocess.check_output(["nm", "-g", "--defined-only", str(path)], text=True)
    return {line.split()[-1] for line in text.splitlines() if line.split()}


def require(condition, message):
    """Enforce even under Python optimization."""
    if not condition:
        raise ValueError(message)


def main():
    """Inspect the default/qualified build artifacts from the repository cwd."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ordinary", type=Path, default=Path("build/waddle_vulkan_experimental.dll"))
    parser.add_argument("--qualified", type=Path, default=Path("build/m2_qualification/waddle_vulkan_experimental.dll"))
    args = parser.parse_args()
    definition = Path("src/vgpu/venus_icd.def").read_text().split("EXPORTS\n", 1)[1]
    expected = {line.strip() for line in definition.splitlines() if line.strip()}
    require(len(expected) == 11, "unexpected ordinary definition; review export contract")
    queries = {"venus_icd_m2_snapshot", "venus_m2_dc_snapshot"}
    require(exports(args.ordinary) == expected, "ordinary DLL export set changed")
    require(exports(args.qualified) == expected | queries, "qualified DLL must add exactly both read-only queries")
    ordinary = definitions(Path("build/venus_icd_windows.obj"))
    qualified = definitions(Path("build/m2_qualification/venus_icd_windows.obj"))
    require("venus_icd_m2_snapshot" not in ordinary, "ordinary ICD contains probe")
    require(qualified == ordinary | {"venus_icd_m2_snapshot"}, "qualified ICD definitions must add only its read-only query")
    observer = definitions(Path("build/wsi-lifecycle/m2_ownership_probe_windows.obj"))
    require(observer == {"venus_m2_dc_acquire", "venus_m2_dc_release", "venus_m2_dc_snapshot"}, "observer contains unexpected definitions")
    all_symbols = qualified | observer
    for name in ("venus_win32_present_windows.obj", "m2_owner_ledger_windows.obj"):
        all_symbols |= definitions(Path("build/m2_qualification") / name)
    require(not any(name.startswith("venus_m2_dc_test_") for name in all_symbols), "qualified candidate contains fault controls")
    print("M2_QUALIFICATION_BUILD PASS default_exports=11 qualified_exports=13 readonly_queries=2 fault_controls=0 execution=NOT_RUN")


if __name__ == "__main__":
    main()
