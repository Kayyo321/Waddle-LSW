"""Fail-closed admission checks for the private Win32 OpenCL runtime.

This module inspects PE32+ bytes and GNU ld map text. It does not execute the
DLL. Callers must bind the returned file hashes and exact toolchain/runtime
archive identities into their build receipt.
"""

from __future__ import annotations

import bisect
import hashlib
import os
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


class audit_error_t(ValueError):
    """Raised when a PE image or link map cannot be proven admissible."""


LimitKeys = frozenset(("max_objects", "max_bytes", "max_align"))
ReplacedMembers = (
    "libgcc_eh.a(emutls.o)",
    "libgcc_eh.a(gthr-win32-thread.o)",
)
ForbiddenMapFragments = (
    "libwinpthread.a(",
    "libpthread.a(",
    "libgcc_eh.a(emutls.o)",
    "libgcc_eh.a(gthr-win32-thread.o)",
)
AbiSymbols = (
    "__emutls_get_address",
    "__emutls_register_common",
    "__gthr_win32_create",
    "__gthr_win32_join",
    "__gthr_win32_self",
)
ForbiddenThreadAtexit = (
    "tls_atexit.o",
    "__mingw_cxa_atexit",
    "__cxa_atexit",
    "__dso_handle",
    "__cxa_thread_atexit",
    "__cxa_thread_atexit_impl",
    "__at_thread_exit",
    "notify_all_at_thread_exit",
)
SensitiveApis = ("TlsAlloc", "TlsFree", "FlsAlloc", "FlsFree", "DuplicateHandle", "GetProcAddress")
Executable = 0x20000000
Writable = 0x80000000


@dataclass(frozen=True)
class section_t:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int

    @property
    def virtual_end(self) -> int:
        return self.virtual_address + max(self.virtual_size, self.raw_size)

    @property
    def raw_end(self) -> int:
        return self.raw_offset + self.raw_size


@dataclass(frozen=True)
class import_symbol_t:
    dll: str
    name: str
    iat_rva: int


class pe_image_t:
    """Validated PE32+ image with checked RVA/file-offset conversion."""

    def __init__(self, path: str | os.PathLike[str], data: bytes | None = None):
        self.path = Path(path)
        try:
            self.data = data if data is not None else self.path.read_bytes()
        except OSError as exc:
            raise audit_error_t(f"cannot read PE image {self.path}: {exc}") from exc
        self.sha256 = hashlib.sha256(self.data).hexdigest()
        self.sections: list[section_t] = []
        self.import_name_ranges: list[tuple[int, int]] = []
        self._parse_headers()

    def _need(self, offset: int, size: int, label: str) -> None:
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            raise audit_error_t(f"truncated PE while reading {label}")

    def _unpack(self, fmt: str, offset: int, label: str):
        size = struct.calcsize(fmt)
        self._need(offset, size, label)
        return struct.unpack_from(fmt, self.data, offset)

    def _parse_headers(self) -> None:
        if len(self.data) < 0x40 or self.data[:2] != b"MZ":
            raise audit_error_t("input is not a DOS/PE image")
        (pe_offset,) = self._unpack("<I", 0x3C, "DOS e_lfanew")
        self._need(pe_offset, 24, "PE and COFF headers")
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise audit_error_t("invalid PE signature")
        machine, section_count, _, _, _, optional_size, _ = self._unpack("<HHIIIHH", pe_offset + 4, "COFF header")
        if machine != 0x8664:
            raise audit_error_t(f"expected AMD64 PE, found machine 0x{machine:04x}")
        if section_count == 0 or section_count > 96:
            raise audit_error_t(f"invalid PE section count: {section_count}")
        optional = pe_offset + 24
        self._need(optional, optional_size, "optional header")
        magic, = self._unpack("<H", optional, "optional header magic")
        if magic != 0x20B or optional_size < 112:
            raise audit_error_t("expected a complete PE32+ optional header")
        self.image_base, = self._unpack("<Q", optional + 24, "image base")
        self.section_alignment, self.file_alignment = self._unpack("<II", optional + 32, "section alignment")
        self.size_of_image, self.size_of_headers = self._unpack("<II", optional + 56, "image sizes")
        self.number_of_directories, = self._unpack("<I", optional + 108, "data directory count")
        directory_bytes = min(self.number_of_directories, 16) * 8
        if 112 + directory_bytes > optional_size:
            raise audit_error_t("data directory table exceeds optional header")
        self.directories = [self._unpack("<II", optional + 112 + i * 8, f"data directory {i}")
                            for i in range(min(self.number_of_directories, 16))]
        section_table = optional + optional_size
        self._need(section_table, section_count * 40, "section table")
        ranges: list[tuple[int, int, str]] = []
        raw_ranges: list[tuple[int, int, str]] = []
        for i in range(section_count):
            entry = section_table + i * 40
            raw_name = self.data[entry:entry + 8].split(b"\0", 1)[0]
            try:
                name = raw_name.decode("ascii")
            except UnicodeDecodeError as exc:
                raise audit_error_t("non-ASCII section name") from exc
            virtual_size, virtual_address, raw_size, raw_offset = self._unpack("<IIII", entry + 8, "section entry")
            characteristics, = self._unpack("<I", entry + 36, "section characteristics")
            if raw_size:
                self._need(raw_offset, raw_size, f"{name} raw data")
                if raw_offset < self.size_of_headers:
                    raise audit_error_t(f"section raw data overlaps headers: {name}")
                raw_ranges.append((raw_offset, raw_offset + raw_size, name))
            if virtual_address + max(virtual_size, raw_size) > 0x100000000:
                raise audit_error_t(f"section RVA overflow: {name}")
            if virtual_address + max(virtual_size, raw_size) > self.size_of_image:
                raise audit_error_t(f"section exceeds SizeOfImage: {name}")
            section = section_t(name, virtual_size, virtual_address, raw_size, raw_offset, characteristics)
            self.sections.append(section)
            if virtual_size or raw_size:
                ranges.append((virtual_address, section.virtual_end, name))
        ranges.sort()
        for (_, previous_end, previous_name), (next_start, _, next_name) in zip(ranges, ranges[1:]):
            if next_start < previous_end:
                raise audit_error_t(f"overlapping virtual sections: {previous_name} and {next_name}")
        raw_ranges.sort()
        for (_, previous_end, previous_name), (next_start, _, next_name) in zip(raw_ranges, raw_ranges[1:]):
            if next_start < previous_end:
                raise audit_error_t(f"overlapping raw sections: {previous_name} and {next_name}")
        if self.size_of_headers > len(self.data):
            raise audit_error_t("SizeOfHeaders extends beyond file")
        if section_table + section_count * 40 > self.size_of_headers:
            raise audit_error_t("SizeOfHeaders does not contain the complete section table")

    def directory(self, index: int, name: str) -> tuple[int, int]:
        if index >= len(self.directories):
            raise audit_error_t(f"PE has no {name} directory entry")
        rva, size = self.directories[index]
        if not rva or not size:
            raise audit_error_t(f"PE {name} directory is missing")
        self.rva_offset(rva, size, name)
        return rva, size

    def rva_section(self, rva: int, size: int = 1, *, raw: bool = True) -> section_t:
        if rva < 0 or size < 0 or rva + size > 0x100000000:
            raise audit_error_t("RVA range overflows PE address space")
        matches = []
        for section in self.sections:
            limit = section.raw_size if raw else max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva and rva + size <= section.virtual_address + limit:
                matches.append(section)
        if len(matches) != 1:
            raise audit_error_t(f"RVA range 0x{rva:x}+0x{size:x} is not uniquely {'file-backed' if raw else 'mapped'}")
        return matches[0]

    def rva_offset(self, rva: int, size: int = 1, label: str = "RVA") -> int:
        section = self.rva_section(rva, size, raw=True)
        offset = section.raw_offset + rva - section.virtual_address
        self._need(offset, size, label)
        return offset

    def va_offset(self, va: int, size: int, label: str) -> int:
        if va < self.image_base:
            raise audit_error_t(f"{label} VA is below ImageBase")
        return self.rva_offset(va - self.image_base, size, label)

    def c_string(self, offset: int, limit: int, label: str) -> str:
        if limit <= 0:
            raise audit_error_t(f"empty bound while reading {label}")
        self._need(offset, limit, label)
        end = self.data.find(b"\0", offset, offset + limit)
        if end < 0:
            raise audit_error_t(f"unterminated {label}")
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise audit_error_t(f"non-ASCII {label}") from exc

    def tls_layout(self, template_symbol: int | None = None) -> dict[str, int | str]:
        tls_rva, tls_size = self.directory(9, "TLS")
        if tls_size < 40:
            raise audit_error_t("PE TLS directory is shorter than IMAGE_TLS_DIRECTORY64")
        off = self.rva_offset(tls_rva, 40, "TLS directory")
        start, end, index_va, callbacks_va, zero_fill, characteristics = self._unpack("<QQQQII", off, "TLS directory")
        if start == 0 or end <= start:
            raise audit_error_t("TLS raw template has invalid start/end")
        template_size = end - start
        start_off = self.va_offset(start, template_size, "TLS raw template")
        end_off = self.va_offset(end - 1, 1, "TLS raw template end")
        start_section = self.rva_section(start - self.image_base, template_size, raw=True)
        if self.rva_section(end - 1 - self.image_base, 1, raw=True) != start_section or end_off != start_off + template_size - 1:
            raise audit_error_t("TLS template is not one contiguous file-backed section range")
        index_rva = index_va - self.image_base
        index_section = self.rva_section(index_rva, 4, raw=False)
        if not index_section.characteristics & Writable or index_rva % 4:
            raise audit_error_t("TLS AddressOfIndex is outside writable data")
        index_off = None
        if index_rva + 4 <= index_section.virtual_address + index_section.raw_size:
            index_off = index_section.raw_offset + index_rva - index_section.virtual_address
        if template_symbol is not None:
            if not (start <= template_symbol < end):
                raise audit_error_t("runtime_tls_template is outside the PE TLS raw template")
            if (template_symbol - start) % 16:
                raise audit_error_t("runtime_tls_template is not 16-byte aligned in the TLS block")
        callback_count = 0
        if callbacks_va:
            terminated = False
            for index in range(128):
                callback_va = self._unpack("<Q", self.va_offset(callbacks_va + index * 8, 8, "TLS callback table") + 0,
                                           "TLS callback pointer")[0]
                if callback_va == 0:
                    terminated = True
                    break
                callback_section = self.rva_section(callback_va - self.image_base, 1, raw=True)
                if not callback_section.characteristics & Executable:
                    raise audit_error_t("TLS callback does not point into executable code")
                callback_count += 1
            if not terminated:
                raise audit_error_t("TLS callback table has no terminator within 128 entries")
        return {
            "start_va": start,
            "end_va": end,
            "address_of_index_va": index_va,
            "address_of_callbacks_va": callbacks_va,
            "template_bytes": template_size,
            "template_file_offset": start_off,
            "template_section": start_section.name,
            "index_file_offset": index_off,
            "zero_fill_bytes": zero_fill,
            "characteristics": characteristics,
            "callback_count": callback_count,
        }

    def imports(self) -> list[import_symbol_t]:
        import_rva, import_size = self.directory(1, "import")
        end_rva = import_rva + import_size
        cursor = import_rva
        result: list[import_symbol_t] = []
        descriptor_seen = False
        while cursor + 20 <= end_rva:
            off = self.rva_offset(cursor, 20, "import descriptor")
            lookup, timestamp, forwarder, name_rva, iat_rva = self._unpack("<IIIII", off, "import descriptor")
            if not any((lookup, timestamp, forwarder, name_rva, iat_rva)):
                descriptor_seen = True
                break
            dll_off = self.rva_offset(name_rva, 1, "import DLL name")
            dll_section = self.rva_section(name_rva, 1, raw=True)
            dll_name = self.c_string(dll_off, dll_section.raw_end - dll_off, "import DLL name")
            lookup_rva = lookup or iat_rva
            index = 0
            while index < 1_000_000:
                thunk_rva = lookup_rva + index * 8
                iat_entry_rva = iat_rva + index * 8
                entry_off = self.rva_offset(thunk_rva, 8, "import thunk")
                value, = self._unpack("<Q", entry_off, "import thunk")
                self.rva_offset(iat_entry_rva, 8, "IAT slot")
                if value == 0:
                    break
                if value & (1 << 63):
                    symbol = f"ordinal_{value & 0xffff}"
                else:
                    name_offset = self.rva_offset(value & 0x7fffffff, 3, "import by name")
                    name_section = self.rva_section(value & 0x7fffffff, 3, raw=True)
                    symbol = self.c_string(name_offset + 2, name_section.raw_end - name_offset - 2, "import name")
                    self.import_name_ranges.append((name_offset + 2, len(symbol) + 1))
                result.append(import_symbol_t(dll_name, symbol, iat_entry_rva))
                index += 1
            else:
                raise audit_error_t("unterminated import thunk table")
            if index >= 1_000_000:
                raise audit_error_t("unreasonable import thunk count")
            cursor += 20
        if not descriptor_seen:
            raise audit_error_t("import descriptors have no terminating null entry")
        if not result:
            raise audit_error_t("PE has no named imports; cannot audit runtime ownership")
        return result

    def exports(self, expected: Sequence[str]) -> list[str]:
        """Require the exact named export set supplied from the reviewed DEF."""
        if not expected or any(not isinstance(name, str) or not name for name in expected):
            raise audit_error_t("expected export list must be nonempty named symbols")
        export_rva, export_size = self.directory(0, "export")
        if export_size < 40:
            raise audit_error_t("PE export directory is shorter than IMAGE_EXPORT_DIRECTORY")
        off = self.rva_offset(export_rva, 40, "export directory")
        (_characteristics, _timestamp, _major, _minor, _name_rva, _base,
         function_count, name_count, functions_rva, names_rva, ordinals_rva) = self._unpack(
            "<IIHHIIIIIII", off, "export directory")
        if not function_count or not name_count or function_count > 100_000 or name_count > function_count:
            raise audit_error_t("invalid PE export function/name counts")
        functions_off = self.rva_offset(functions_rva, function_count * 4, "export function table")
        names_off = self.rva_offset(names_rva, name_count * 4, "export name table")
        ordinals_off = self.rva_offset(ordinals_rva, name_count * 2, "export ordinal table")
        names: list[str] = []
        for index in range(name_count):
            name_rva, = self._unpack("<I", names_off + index * 4, "export name RVA")
            ordinal, = self._unpack("<H", ordinals_off + index * 2, "export ordinal")
            if ordinal >= function_count:
                raise audit_error_t("invalid PE export ordinal")
            function_rva, = self._unpack("<I", functions_off + ordinal * 4, "export function RVA")
            if function_rva == 0:
                raise audit_error_t("PE export ordinal resolves to a null function")
            if export_rva <= function_rva < export_rva + export_size:
                raise audit_error_t("forwarded exports are outside the supported policy")
            function_section = self.rva_section(function_rva, 1, raw=True)
            if not function_section.characteristics & Executable:
                raise audit_error_t("named DLL export does not resolve to executable code")
            name_section = self.rva_section(name_rva, 1, raw=True)
            name_off = self.rva_offset(name_rva, 1, "export name")
            name = self.c_string(name_off, name_section.raw_end - name_off, "export name")
            names.append(name)
        if len(names) != len(set(names)):
            raise audit_error_t("duplicate named PE exports")
        if set(names) != set(expected) or len(names) != len(expected):
            missing = sorted(set(expected) - set(names))
            extra = sorted(set(names) - set(expected))
            raise audit_error_t(f"PE export set differs from reviewed DEF; missing={missing}, extra={extra}")
        return sorted(names)


def _load_symbols(dll: Path, nm: str = "x86_64-w64-mingw32-nm", lines: Iterable[str] | None = None) -> list[tuple[int, str, str]]:
    if lines is None:
        executable = shutil.which(nm)
        if not executable:
            raise audit_error_t(f"required symbol tool not found: {nm}")
        try:
            completed = subprocess.run([executable, "-n", "--defined-only", str(dll)],
                                       check=True, text=True, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, timeout=60)
        except (OSError, subprocess.SubprocessError) as exc:
            raise audit_error_t(f"cannot inspect PE symbols with {nm}: {exc}") from exc
        lines = completed.stdout.splitlines()
    symbols: list[tuple[int, str, str]] = []
    for line in lines:
        fields = line.split()
        if len(fields) != 3:
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        kind, name = fields[1], fields[2]
        if kind not in "tTwWdDbBrR":
            continue
        symbols.append((address, kind, name))
    if not symbols:
        raise audit_error_t("symbol table is empty or stripped; cannot prove TLS/runtime ownership")
    return sorted(symbols)


def _emutls_name(name: str) -> bool:
    name = name.removeprefix(".data$")
    return name.startswith("__emutls_v.")


def _verify_teb_tls_accessor(image: pe_image_t, symbols: Sequence[tuple[int, str, str]], symbol: str = "__emutls_get_address") -> None:
    """Check actual inline accessor or one exact direct call to its C helper.

    Borrowed PE/symbol inputs; no mutation. Accept any x64 destination register
    for GS absolute mov, not arbitrary GS occurrences elsewhere in the image.
    Unsupported code forms/missing call edges/wrong offsets raise audit_error_t.
    """
    starts = sorted((address, name) for address, kind, name in symbols if kind in "tT")

    def body(name):
        matches = [address for address, actual in starts if actual == name]
        if len(matches) != 1:
            raise audit_error_t(f"expected exactly one executable {name} symbol")
        address = matches[0]
        section = image.rva_section(address - image.image_base, 1, raw=True)
        if not section.characteristics & Executable:
            raise audit_error_t(f"{name} does not reside in an executable section")
        following = [value for value, _ in starts if value > address]
        upper = min(min(following) if following else address + 4096,
                    image.image_base + section.virtual_address + section.raw_size)
        offset = image.va_offset(address, upper - address, name)
        return address, image.data[offset:offset + upper - address]

    address, code = body(symbol)
    scopes = [code]
    helper = "runtime_tls_current_record"
    helper_addresses = [value for value, name in starts if name == helper]
    if helper_addresses:
        if len(helper_addresses) != 1:
            raise audit_error_t("duplicate runtime TLS helper")
        target = helper_addresses[0]
        called = any(code[pos] in (0xE8, 0xE9) and
                     address + pos + 5 + struct.unpack_from("<i", code, pos + 1)[0] == target
                     for pos in range(max(0, len(code) - 4)))
        if called:
            scopes.append(body(helper)[1])
    offsets = []
    for scope in scopes:
        for pos in range(max(0, len(scope) - 8)):
            if (scope[pos] == 0x65 and scope[pos + 1] in (0x48, 0x4C) and
                scope[pos + 2] == 0x8B and scope[pos + 3] & 0xC7 == 0x04 and
                scope[pos + 4] == 0x25):
                offsets.append(struct.unpack_from("<I", scope, pos + 5)[0])
    if any(offset != 0x58 for offset in offsets):
        wrong = next(offset for offset in offsets if offset != 0x58)
        raise audit_error_t(f"runtime TLS accessor uses TEB offset 0x{wrong:x}; expected x64 TLS vector offset 0x58")
    if not offsets:
        raise audit_error_t(f"{symbol} does not contain the required GS:0x58 TLS-vector access or direct helper call")


def emutls_inventory(image: pe_image_t, symbols: Sequence[tuple[int, str, str]], limits: Mapping[str, int]) -> dict:
    controls = sorted({address: name.removeprefix(".data$")
                       for address, _, name in symbols if _emutls_name(name)}.items())
    if not controls:
        raise audit_error_t("no __emutls_v.* control objects found; inventory would be vacuous")
    count, worst_case, maximum_align = len(controls), 0, 0
    records = []
    for address, name in controls:
        if address < image.image_base:
            raise audit_error_t(f"emutls control {name} is below ImageBase")
        offset = image.rva_offset(address - image.image_base, 16, f"emutls control {name}")
        size, align = struct.unpack_from("<QQ", image.data, offset)
        if align == 0:
            align = 1
        if align & (align - 1):
            raise audit_error_t(f"emutls control {name} has non-power-of-two alignment {align}")
        padding = max(align, 1) - 1
        worst_case += size + padding
        maximum_align = max(maximum_align, align)
        records.append({"name": name, "address": address, "size": size, "align": align, "worst_case_padding": padding})
    if count > limits["max_objects"]:
        raise audit_error_t(f"emulated TLS object count {count} exceeds {limits['max_objects']}")
    if worst_case > limits["max_bytes"]:
        raise audit_error_t(f"emulated TLS worst-case bytes {worst_case} exceeds {limits['max_bytes']}")
    if maximum_align > limits["max_align"]:
        raise audit_error_t(f"emulated TLS alignment {maximum_align} exceeds {limits['max_align']}")
    return {"emutls_objects": count, "emutls_worst_case_bytes": worst_case,
            "emutls_max_align": maximum_align, "objects": records}


def _compiled_tls_limits(image: pe_image_t, symbols: Sequence[tuple[int, str, str]],
                         expected: Mapping[str, int]) -> dict:
    """Read retained four-word metadata and bind inventory limits to this PE."""
    matches = [address for address, _, name in symbols
               if name.lstrip("_") == "waddle_clvk_runtime_tls_limits"]
    if len(matches) != 1:
        raise audit_error_t(f"expected exactly one retained TLS limits symbol, found {len(matches)}")
    address = matches[0]
    offset = image.va_offset(address, 16, "compiled TLS limits metadata")
    section = image.rva_section(address - image.image_base, 16, raw=True)
    if section.characteristics & (Executable | Writable) or section.name != ".rdata":
        raise audit_error_t("compiled TLS limits metadata is not retained in read-only .rdata")
    schema, arena_bytes, slot_capacity, max_align = struct.unpack_from("<IIII", image.data, offset)
    if schema != 1:
        raise audit_error_t(f"unsupported compiled TLS limits metadata schema {schema}")
    compiled = {"max_objects": slot_capacity, "max_bytes": arena_bytes, "max_align": max_align}
    if compiled != dict(expected):
        raise audit_error_t(f"PE TLS limits metadata {compiled} does not match build receipt {dict(expected)}")
    return {"schema": schema, **compiled, "symbol_va": address,
            "file_offset": offset, "section": section.name}


def _sensitive_callers(image: pe_image_t, imports: Sequence[import_symbol_t], symbols: Sequence[tuple[int, str, str]]) -> dict[str, list[str]]:
    sensitive_slots: dict[str, set[int]] = {name: set() for name in SensitiveApis}
    for item in imports:
        if item.name in sensitive_slots:
            sensitive_slots[item.name].add(image.image_base + item.iat_rva)
    if not any(sensitive_slots.values()):
        return {name: [] for name in SensitiveApis}
    functions = sorted((address, name) for address, kind, name in symbols if kind in "tT")
    if not functions:
        raise audit_error_t("no executable symbols to attribute sensitive API references")
    starts = [address for address, _ in functions]
    callers = {name: set() for name in SensitiveApis}
    thunk_targets: dict[int, set[str]] = {}
    thunk_addresses: dict[str, set[int]] = {name: set() for name in SensitiveApis}
    # Scan every executable byte, intentionally conservative. A candidate byte
    # pattern that cannot be attributed is a hard error rather than a pass.
    for section in image.sections:
        if not section.characteristics & Executable:
            continue
        if section.raw_size == 0:
            raise audit_error_t(f"executable section {section.name} has no file-backed bytes")
        code = image.data[section.raw_offset:section.raw_end]
        for pos in range(len(code)):
            absolute_va = image.image_base + section.virtual_address + pos
            owner_index = bisect.bisect_right(starts, absolute_va) - 1
            owner = functions[owner_index][1] if owner_index >= 0 else "<unattributed>"
            if owner == "<unattributed>":
                raise audit_error_t("executable bytes precede the first function symbol")
            if pos + 6 <= len(code) and code[pos] == 0xFF and code[pos + 1] in (0x15, 0x25):
                displacement, = struct.unpack_from("<i", code, pos + 2)
                target = absolute_va + 6 + displacement
                # Direct thunk shape: jmp qword ptr [rip+IAT].
                if code[pos:pos + 2] == b"\xff\x25":
                    for api, slots in sensitive_slots.items():
                        if target in slots:
                            thunk_targets.setdefault(absolute_va, set()).add(api)
                            thunk_addresses[api].add(absolute_va)
                else:
                    for api, slots in sensitive_slots.items():
                        if target in slots:
                            callers[api].add(owner)
            elif pos + 7 <= len(code) and code[pos] in (0x48, 0x4C) and code[pos + 1] in (0x8B, 0xFF) and code[pos + 2] & 0xC7 == 0x05:
                displacement, = struct.unpack_from("<i", code, pos + 3)
                target = absolute_va + 7 + displacement
                for api, slots in sensitive_slots.items():
                    if target in slots:
                        callers[api].add(owner)
            elif pos + 7 <= len(code) and code[pos] in (0x48, 0x4C) and code[pos + 1] == 0x8D and code[pos + 2] & 0xC7 == 0x05:
                displacement, = struct.unpack_from("<i", code, pos + 3)
                target = absolute_va + 7 + displacement
                for api, slots in sensitive_slots.items():
                    if target in slots:
                        raise audit_error_t(f"unsupported LEA-based sensitive API pointer reference to {api} in {owner}")
        for api, slots in sensitive_slots.items():
            for slot in slots:
                needle = struct.pack("<Q", slot)
                if needle in code:
                    raise audit_error_t(f"absolute sensitive API IAT pointer reference to {api} in executable section {section.name}")
    # Find direct rel32 calls/jumps to imported API thunks.
    for section in image.sections:
        if not section.characteristics & Executable:
            continue
        code = image.data[section.raw_offset:section.raw_end]
        for pos in range(max(0, len(code) - 4)):
            if code[pos] not in (0xE8, 0xE9) or pos + 5 > len(code):
                continue
            src_va = image.image_base + section.virtual_address + pos
            displacement, = struct.unpack_from("<i", code, pos + 1)
            target = src_va + 5 + displacement
            if target in thunk_targets:
                owner_index = bisect.bisect_right(starts, src_va) - 1
                if owner_index < 0:
                    raise audit_error_t("unattributed direct call to sensitive API thunk")
                owner = functions[owner_index][1]
                for api in thunk_targets[target]:
                    callers[api].add(owner)
    # A function pointer to an import thunk is not a direct call instruction.
    # Search every byte in non-executable sections so unaligned refptrs are
    # covered too; these data aliases have no callsite attribution and fail.
    pointer_references = []
    thunk_to_api = {address: api for api, addresses in thunk_addresses.items() for address in addresses}
    iat_to_api = {slot: api for api, slots in sensitive_slots.items() for slot in slots}
    for section in image.sections:
        if section.characteristics & Executable:
            continue
        raw = image.data[section.raw_offset:section.raw_end]
        for value, api, target_kind in (
            *((address, api, "thunk") for address, api in thunk_to_api.items()),
            *((address, api, "IAT") for address, api in iat_to_api.items()),
        ):
            needle = struct.pack("<Q", value)
            search_from = 0
            while True:
                pos = raw.find(needle, search_from)
                if pos < 0:
                    break
                pointer_references.append((section.name, section.virtual_address + pos, api, target_kind))
                search_from = pos + 1
    if pointer_references:
        raise audit_error_t(f"sensitive API thunk address escapes into data pointers: {pointer_references[:8]}")
    # Pointer/reference-to-thunk forms are rejected unless caller policy can
    # prove the exact function. Search raw words and report as unbound metadata.
    # The check remains conservative: it prevents empty call-site results from
    # being treated as proof when an imported sensitive API is present.
    for api, slots in sensitive_slots.items():
        if slots and not callers[api]:
            raise audit_error_t(f"imported {api} has no attributable executable reference (vacuous call-site scan)")
    return {name: sorted(values) for name, values in callers.items()}


def _check_dynamic_lookup(image: pe_image_t, imports: Sequence[import_symbol_t], api_callers: Mapping[str, Sequence[str]], policy: Mapping) -> None:
    if not any(item.name == "GetProcAddress" for item in imports):
        return
    actual_callers = set(api_callers.get("GetProcAddress", ()))
    permitted_callers = set(policy.get("dynamic_lookup_callers", ()))
    if not actual_callers or actual_callers != permitted_callers:
        raise audit_error_t(f"GetProcAddress callers are not fully allowlisted: {sorted(actual_callers)}")
    needles = (b"TlsAlloc\0", b"FlsAlloc\0", b"TlsFree\0", b"FlsFree\0", b"DuplicateHandle\0",
               "TlsAlloc\0".encode("utf-16le"), "FlsAlloc\0".encode("utf-16le"),
               "TlsFree\0".encode("utf-16le"), "FlsFree\0".encode("utf-16le"),
               "DuplicateHandle\0".encode("utf-16le"))
    found = []
    for needle in needles:
        for section in image.sections:
            if section.raw_size == 0:
                continue
            search_from = section.raw_offset
            while True:
                offset = image.data.find(needle, search_from, section.raw_end)
                if offset < 0:
                    break
                search_from = offset + 1
                if any(start <= offset < start + length for start, length in image.import_name_ranges):
                    continue
                found.append(needle.rstrip(b"\0"))
                break
            if found and found[-1] == needle.rstrip(b"\0"):
                break
    if found:
        raise audit_error_t(f"sensitive API name is embedded with GetProcAddress imported: {sorted(set(found))}")


def audit_runtime_tls(
    dll_path: str | os.PathLike[str],
    *,
    nm: str = "x86_64-w64-mingw32-nm",
    symbol_lines: Iterable[str] | None = None,
    limits: Mapping[str, int],
    expected_exports: Sequence[str],
    runtime_policy: Mapping | None = None,
) -> dict:
    """Audit PE/TLS inventory, runtime API call sites, and CRT teardown policy.

    `limits` is required and must be captured from the compiled shim constants
    in this build, not selected by the caller as a policy allowance.
    `runtime_policy` maps sensitive API names to exact allowed function names,
    plus optional `dynamic_lookup_callers`, `template_symbol`, `crt_symbols`,
    and `forbidden_symbols`. Without an explicit allowlist, any referenced
    TlsAlloc/FlsAlloc/DuplicateHandle is rejected. No dynamic lookup is accepted
    by default. `nm` is read-only and bounded to 60 seconds.
    """
    dll = Path(dll_path)
    image = pe_image_t(dll)
    policy = dict(runtime_policy or {})
    if not isinstance(limits, Mapping) or set(limits) != LimitKeys:
        raise audit_error_t(f"expected TLS limits must contain exactly {sorted(LimitKeys)}")
    actual_limits = dict(limits)
    for key in LimitKeys:
        if type(actual_limits[key]) is not int or actual_limits[key] <= 0:
            raise audit_error_t(f"invalid TLS limit {key}")
    if actual_limits["max_align"] & (actual_limits["max_align"] - 1):
        raise audit_error_t("compiled TLS max_align must be a power of two")
    if actual_limits["max_bytes"] % actual_limits["max_align"]:
        raise audit_error_t("compiled TLS arena size must be a multiple of its alignment")
    symbols = _load_symbols(dll, nm, symbol_lines)
    compiled_limits = _compiled_tls_limits(image, symbols, actual_limits)
    names = {name for _, _, name in symbols}
    template_name = policy.get("template_symbol", "runtime_tls_template")
    template_matches = [address for address, _, name in symbols if name.lstrip("_") == template_name]
    if len(template_matches) != 1:
        raise audit_error_t(f"expected exactly one {template_name} symbol, found {len(template_matches)}")
    tls = image.tls_layout(template_matches[0])
    # x64 C record: aligned byte arena, uint32 initialization mask, pointer
    # aligned to 8, then tail padding to 16. Bind the complete record, not just
    # its first byte; a truncated template would otherwise pass admission.
    record_bytes = ((actual_limits["max_bytes"] + 4 + 7) // 8 * 8 + 8 + 15) // 16 * 16
    if template_matches[0] + record_bytes > tls["end_va"]:
        raise audit_error_t("complete runtime TLS record exceeds initialized template")
    record_offset = image.va_offset(template_matches[0], record_bytes, "complete runtime TLS record")
    if any(image.data[record_offset:record_offset + record_bytes]):
        raise audit_error_t("runtime TLS record template must be entirely zero initialized")
    index_matches = [address for address, _, name in symbols if name.lstrip("_") == "tls_index"]
    if len(index_matches) != 1 or tls["address_of_index_va"] != index_matches[0]:
        raise audit_error_t("TLS AddressOfIndex does not resolve exactly to the writable _tls_index symbol")
    if tls["template_section"] != ".tls":
        raise audit_error_t("runtime TLS template is not in the PE .tls section")
    inventory = emutls_inventory(image, symbols, actual_limits)
    _verify_teb_tls_accessor(image, symbols)
    exports = image.exports(expected_exports)
    imports = image.imports()
    api_callers = _sensitive_callers(image, imports, symbols)
    allow = dict(policy.get("allowed_api_callers", {}))
    for api in ("TlsAlloc", "FlsAlloc", "DuplicateHandle"):
        actual = set(api_callers.get(api, ()))
        permitted = set(allow.get(api, ()))
        if actual != permitted:
            raise audit_error_t(f"unapproved {api} or stale caller set; observed={sorted(actual)} allowed={sorted(permitted)}")
        if api == "TlsAlloc" and not actual:
            raise audit_error_t("TlsAlloc caller scan is empty; cannot prove paired runtime-key ownership")
        if api == "TlsAlloc" and not policy.get("tls_pairs"):
            raise audit_error_t("TlsAlloc policy must name create/delete pairing evidence")
        if api == "TlsAlloc":
            pairs = policy["tls_pairs"]
            if not isinstance(pairs, Mapping) or not pairs:
                raise audit_error_t("tls_pairs must map each exact TlsAlloc caller to its TlsFree caller")
            required_pair = set(pairs) | set(pairs.values())
            if not required_pair <= names:
                raise audit_error_t(f"paired TLS symbols absent: {sorted(required_pair - names)}")
            actual_tls_free = set(api_callers.get("TlsFree", ()))
            if actual != set(pairs) or actual_tls_free != set(pairs.values()):
                raise audit_error_t("TlsAlloc/TlsFree callsite sets do not exactly match the declared key create/delete pairs")
    _check_dynamic_lookup(image, imports, api_callers, policy)
    forbidden_symbols = set(ForbiddenThreadAtexit) | set(policy.get("forbidden_symbols", ()))
    forbidden_found = sorted(name for name in names if any(token in name for token in forbidden_symbols))
    if forbidden_found:
        raise audit_error_t(f"forbidden thread-exit runtime symbols linked: {forbidden_found}")
    return {
        "dll_sha256": image.sha256,
        "dll_bytes": len(image.data),
        "tls": tls,
        "compiled_tls_limits": compiled_limits,
        "exports": exports,
        **inventory,
        "imports": [{"dll": item.dll, "name": item.name, "iat_rva": item.iat_rva} for item in imports],
        "sensitive_api_callers": api_callers,
        "tls_pairs": dict(policy.get("tls_pairs", {})),
    }


def _map_output(text: str) -> str:
    matches = re.findall(r"^OUTPUT\((.+?)\s+pei-x86-64\)$", text, re.MULTILINE)
    if len(matches) != 1:
        raise audit_error_t(f"expected exactly one PE64 OUTPUT record in map, found {len(matches)}")
    return matches[0]


def _symbol_input(map_lines: Sequence[str], symbol: str) -> str:
    symbol_lines = [i for i, line in enumerate(map_lines) if re.search(rf"\s{re.escape(symbol)}\s*$", line)]
    if len(symbol_lines) != 1:
        raise audit_error_t(f"link map must define {symbol} exactly once, found {len(symbol_lines)}")
    i = symbol_lines[0]
    for j in range(i - 1, max(-1, i - 12), -1):
        line = map_lines[j]
        if re.match(r"\s+(?:\.\S+\s+)?0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+\S+", line):
            return line.strip().split()[-1]
    raise audit_error_t(f"cannot attribute map definition of {symbol} to an input object")


def _source_matches(source: str, expected: str) -> bool:
    """Match a direct object or the same object as a GNU archive member."""
    normalized = source.strip().replace("\\", "/")
    basename = normalized.rsplit("/", 1)[-1]
    if basename == expected:
        return True
    if "(" not in basename or not basename.endswith(")"):
        return False
    archive, member = basename.rsplit("(", 1)
    member = member[:-1]
    return bool(archive.endswith(".a") and (member == expected or member.endswith("-" + expected)))


def _runtime_map_audit(map_text: str, policy: Mapping, shim_object: str, eh_pool_object: str | None) -> dict:
    lowered = map_text.lower()
    forbidden = [fragment for fragment in ForbiddenMapFragments if fragment.lower() in lowered]
    if forbidden:
        raise audit_error_t(f"forbidden runtime archive members appear in link map: {forbidden}")
    if not re.search(rf"(?:^|[/\\]){re.escape(Path(shim_object).name)}(?:\s|$)", map_text):
        raise audit_error_t(f"link map does not contain required shim object {shim_object}")
    if eh_pool_object and not re.search(rf"(?:^|[/\\]){re.escape(Path(eh_pool_object).name)}(?:\s|$)", map_text):
        raise audit_error_t(f"link map does not contain required EH-pool object {eh_pool_object}")
    lines = map_text.splitlines()
    for symbol in AbiSymbols:
        owner = _symbol_input(lines, symbol)
        if Path(owner).name != Path(shim_object).name:
            raise audit_error_t(f"{symbol} resolves from {owner}, expected Waddle shim object {shim_object}")
    forbidden_tokens = tuple(ForbiddenThreadAtexit) + tuple(policy.get("forbidden_map_tokens", ()))
    # Discarded input inventory and linker wildcard declarations are not mapped
    # output. Reject actual forbidden output rows; PE defined symbols are also
    # independently rejected. A missing/duplicate boundary fails closed.
    active_text = map_text
    boundary = "Linker script and memory map"
    if "Discarded input sections" in map_text:
        if map_text.count(boundary) != 1:
            raise audit_error_t("discarded map inventory has no unique output boundary")
        active_text = map_text.split(boundary, 1)[1]
    active_text = "\n".join(line for line in active_text.splitlines()
                            if not line.lstrip().startswith("*("))
    present = sorted(token for token in forbidden_tokens if token in active_text)
    if present:
        raise audit_error_t(f"forbidden CRT/thread-atexit entries appear in map: {present}")
    crt_policy = policy.get("crt_sections")
    crt_result = None
    if crt_policy is not None:
        crt_sections: dict[str, list[tuple[str, str]]] = {"XC": [], "XI": []}
        for line in lines:
            m = re.match(r"\s+\.CRT\$(XC\w*|XI\w*)\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+(.+)$", line)
            if m:
                kind, source = m.groups()
                category = kind[:2]
                crt_sections[category].append((kind, source.strip()))
        for category in ("XC", "XI"):
            expected = [(str(kind), str(source)) for kind, source in crt_policy.get(category, ())]
            actual = crt_sections[category]
            if len(actual) != len(expected):
                raise audit_error_t(f".CRT${category} entry count {len(actual)} does not match expected {len(expected)}")
            for (actual_kind, actual_source), (expected_kind, expected_source) in zip(actual, expected):
                if actual_kind != expected_kind or not _source_matches(actual_source, Path(expected_source).name):
                    raise audit_error_t(f"unexpected .CRT entry {actual_kind} from {actual_source}; expected {expected_kind} from {expected_source}")
        crt_result = {key: [{"section": section, "source": source} for section, source in value]
                      for key, value in crt_sections.items()}
    required_map_symbols = policy.get("required_map_symbols", {})
    for symbol, expected_source in required_map_symbols.items():
        owner = _symbol_input(lines, symbol)
        expected_name = Path(expected_source).name
        if not _source_matches(owner, expected_name):
            raise audit_error_t(f"{symbol} resolves from {owner}, expected {expected_source}")
    return {"abi_symbol_inputs": {symbol: _symbol_input(lines, symbol) for symbol in AbiSymbols},
            "crt_sections": crt_result}


def verify_runtime_link(
    link_map: str | os.PathLike[str],
    dll_path: str | os.PathLike[str],
    *,
    expected_output_name: str = "libOpenCL.dll",
    shim_object: str = "clvk_runtime_tls.o",
    eh_pool_object: str = "clvk_runtime_eh_pool.o",
    runtime_policy: Mapping | None = None,
    map_working_directory: str | os.PathLike[str] | None = None,
) -> dict:
    """Prove link-map output identity, ABI member ownership, and CRT policy.

    The link map and DLL are borrowed inputs. Caller must produce both in the
    same fresh build invocation and bind this result with their receipt; a GNU
    map cannot cryptographically prove which DLL bytes were linked.
    """
    map_path = Path(link_map)
    dll = Path(dll_path)
    try:
        map_bytes = map_path.read_bytes()
    except OSError as exc:
        raise audit_error_t(f"cannot read link map {map_path}: {exc}") from exc
    if not map_bytes:
        raise audit_error_t("link map is empty")
    if len(map_bytes) > 256 * 1024 * 1024:
        raise audit_error_t("link map exceeds 256 MiB audit limit")
    try:
        map_text = map_bytes.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise audit_error_t("link map is not valid UTF-8") from exc
    output = _map_output(map_text)
    if Path(output.replace("\\", "/")).name.lower() != expected_output_name.lower():
        raise audit_error_t(f"link map output is {output}, expected {expected_output_name}")
    workdir = Path(map_working_directory or Path.cwd()).resolve()
    output_path = (workdir / output).resolve()
    dll_path_resolved = dll.resolve()
    if output_path != dll_path_resolved:
        raise audit_error_t(f"map OUTPUT resolves to {output_path}, not audited DLL {dll_path_resolved}")
    if not eh_pool_object:
        raise audit_error_t("the EH-pool object is mandatory; callers cannot disable teardown admission")
    policy = dict(runtime_policy or {})
    mandatory_policy = {
        "crt_sections": {
            "XC": (("XCA", "cinitexe.o"), ("XCAWDL", Path(eh_pool_object).name), ("XCZ", "cinitexe.o")),
            "XI": (("XIA", "cinitexe.o"), ("XIAA", "dllcrt2.o"), ("XIZ", "cinitexe.o")),
        },
        "required_map_symbols": {
            "atexit": "dllcrt2.o",
            "__gnu_cxx::__freeres()": "eh_alloc.o",
            "waddle_clvk_eh_pool_register": Path(eh_pool_object).name,
            "waddle_clvk_eh_pool_release": Path(eh_pool_object).name,
            "waddle_clvk_eh_pool_initializer": Path(eh_pool_object).name,
            "waddle_clvk_runtime_tls_limits": Path(shim_object).name,
        },
    }
    for key, required in mandatory_policy.items():
        if key in policy and policy[key] != required:
            raise audit_error_t(f"runtime policy cannot weaken mandatory {key} checks")
        policy[key] = required
    mapped = _runtime_map_audit(map_text, policy, shim_object, eh_pool_object)
    image = pe_image_t(dll)
    return {"map_sha256": hashlib.sha256(map_bytes).hexdigest(),
            "map_bytes": len(map_bytes), "map_output": output,
            "dll_path": str(dll_path_resolved), "dll_sha256": image.sha256,
            **mapped}


def _verify_debian_version_alias(record: Mapping[str, object]) -> None:
    """Require package-member proof for Debian's literal `14-win32` label.

    in: borrowed identity collected from actual package members; expected whole
    identity must also match exactly at the caller. No I/O or mutation; raises
    audit_error_t on incomplete release/member binding. Version is never spoofed.
    """
    origin = record.get("package_provenance")
    if not isinstance(origin, Mapping) or not isinstance(origin.get("provenance"), Mapping):
        raise audit_error_t("Debian 14-win32 requires exact package provenance")
    if origin["provenance"].get("schema") != 1:
        raise audit_error_t("unsupported Debian package provenance schema")
    for key in ("package_receipt_sha256", "provenance_sha256"):
        if not isinstance(origin.get(key), str) or not re.fullmatch("[0-9a-f]{64}", origin[key]):
            raise audit_error_t("missing Debian provenance/receipt digest")
    packages = origin["provenance"].get("packages")
    expected_versions = {
        "gcc-mingw-w64-x86-64-win32": "14.2.0-19+27+b1",
        "gcc-mingw-w64-x86-64-win32-runtime": "14.2.0-19+27+b1",
        "g++-mingw-w64-x86-64-win32": "14.2.0-19+27+b1",
        "mingw-w64-common": "12.0.0-5", "mingw-w64-x86-64-dev": "12.0.0-5",
    }
    if not isinstance(packages, list) or len(packages) != len(expected_versions):
        raise audit_error_t("Debian alias requires the five reviewed package versions")
    observed = {}
    for item in packages:
        if not isinstance(item, Mapping) or not isinstance(item.get("name"), str):
            raise audit_error_t("invalid Debian package provenance entry")
        name = item["name"]
        digest = item.get("sha256")
        if name in observed or name not in expected_versions or item.get("version") != expected_versions[name]:
            raise audit_error_t("unreviewed Debian compiler/SDK package version")
        if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
            raise audit_error_t("missing Debian package digest")
        observed[name] = item["version"]
    proof = origin.get("package_member_proof")
    backends = record.get("backend_programs")
    if not isinstance(proof, Mapping) or not isinstance(backends, Mapping):
        raise audit_error_t("Debian alias requires compiler/backend/header member proof")
    members = {compiler: record[compiler]["sha256"] for compiler in ("cc", "cxx")}
    for compiler in ("cc", "cxx"):
        programs = backends.get(compiler)
        if not isinstance(programs, Mapping):
            raise audit_error_t("missing compiler backend closure")
        for name in ("cc1", "cc1plus", "collect2"):
            item = programs.get(name)
            if not isinstance(item, Mapping) or not isinstance(item.get("sha256"), str):
                raise audit_error_t("missing private compiler backend digest")
            members[compiler + ":" + name] = item["sha256"]
    members.update({"archive:" + name: item["sha256"] for name, item in record["runtime_archives"].items()})
    members.update({"header:" + path: digest for path, digest in record["critical_headers"].items()})
    for label, digest in members.items():
        if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
            raise audit_error_t("invalid Debian member digest: " + label)
        item = proof.get(label)
        if not isinstance(item, Mapping) or item.get("sha256") != digest or item.get("package") not in observed:
            raise audit_error_t("missing or mismatched Debian package member proof: " + label)
        if not isinstance(item.get("relative_path"), str) or not item["relative_path"] or Path(item["relative_path"]).is_absolute() or ".." in Path(item["relative_path"]).parts:
            raise audit_error_t("invalid Debian package member path")


def verify_toolchain_identity(actual: Mapping[str, object], expected: Mapping[str, object]) -> dict:
    """Require exact compiler, runtime archive, and C++ header identities.

    Schema: `cc` and `cxx` each contain resolved_path, sha256, version,
    dumpfullversion, dumpmachine, and thread_model; `runtime_archives` contains
    path/SHA-256 entries for libgcc.a, libgcc_eh.a, libstdc++.a, libmingw32.a,
    libmsvcrt.a, dllcrt2.o, crtbegin.o, crtend.o; `critical_headers` maps each
    resolved header path to SHA-256. The expected record is host-specific and
    must be reviewed with the produced DLL; no fixed cross-host hashes are
    embedded here.
    """
    if not expected:
        raise audit_error_t("expected reviewed toolchain identity is empty")
    required_top = {"cc", "cxx", "runtime_archives", "critical_headers", "target"}
    required_compiler = {"resolved_path", "sha256", "version", "dumpfullversion", "dumpmachine", "thread_model"}
    required_archives = {"libgcc.a", "libgcc_eh.a", "libstdc++.a", "libmingw32.a",
                         "libmsvcrt.a", "dllcrt2.o", "crtbegin.o", "crtend.o"}
    for label, record in (("actual", actual), ("expected", expected)):
        if not required_top <= set(record):
            raise audit_error_t(f"{label} toolchain identity must include {sorted(required_top)}")
        for compiler in ("cc", "cxx"):
            if not isinstance(record[compiler], Mapping) or not required_compiler <= set(record[compiler]):
                raise audit_error_t(f"{label} {compiler} identity must include {sorted(required_compiler)}")
        if not isinstance(record["runtime_archives"], Mapping) or not required_archives <= set(record["runtime_archives"]):
            raise audit_error_t(f"{label} runtime identity must include {sorted(required_archives)}")
        if not isinstance(record["critical_headers"], Mapping) or not record["critical_headers"]:
            raise audit_error_t(f"{label} critical header closure is empty")
        if not isinstance(record["target"], str):
            raise audit_error_t(f"{label} target must be a string")
        for compiler in ("cc", "cxx"):
            if any(not isinstance(record[compiler][key], str) for key in required_compiler):
                raise audit_error_t(f"{label} {compiler} identity fields must be strings")
        if any(not isinstance(path, str) or not isinstance(digest, str)
               for path, digest in record["critical_headers"].items()):
            raise audit_error_t(f"{label} critical header identity fields must be strings")
        for archive_name in required_archives:
            archive = record["runtime_archives"][archive_name]
            if not isinstance(archive, Mapping) or not {"resolved_path", "sha256"} <= set(archive):
                raise audit_error_t(f"{label} runtime archive identities must include path and SHA-256")
            if any(not isinstance(archive[key], str) for key in ("resolved_path", "sha256")):
                raise audit_error_t(f"{label} runtime archive identity fields must be strings")
    if actual["cc"]["thread_model"] != "win32" or actual["cxx"]["thread_model"] != "win32":
        raise audit_error_t("runtime shim requires both MinGW compiler thread models to be win32")
    if actual["target"] != "x86_64-w64-mingw32" or actual["cc"]["dumpmachine"] != actual["target"] or actual["cxx"]["dumpmachine"] != actual["target"]:
        raise audit_error_t("runtime shim requires target x86_64-w64-mingw32")
    canonical = all("14.2.0" in actual[compiler]["version"] and actual[compiler]["dumpfullversion"] == "14.2.0"
                    for compiler in ("cc", "cxx"))
    debian_alias = all(actual[compiler]["version"].endswith("(GCC) 14-win32") and actual[compiler]["dumpfullversion"] == "14-win32"
                       for compiler in ("cc", "cxx"))
    if not canonical and not debian_alias:
        raise audit_error_t("runtime shim admits only the reviewed GCC 14.2.0 ABI")
    if debian_alias:
        _verify_debian_version_alias(actual)
        _verify_debian_version_alias(expected)
    for record in (actual, expected):
        flat_values = [record["target"], *record["critical_headers"].keys(), *record["critical_headers"].values()]
        for compiler in ("cc", "cxx"):
            flat_values.extend(record[compiler].values())
        for archive in record["runtime_archives"].values():
            if not isinstance(archive, Mapping) or not {"resolved_path", "sha256"} <= set(archive):
                raise audit_error_t("runtime archive identities must include resolved_path and sha256")
            flat_values.extend(archive.values())
        if any(not value for value in flat_values):
            raise audit_error_t("toolchain identity contains an empty field")
        for compiler in ("cc", "cxx"):
            digest = record[compiler]["sha256"]
            if not isinstance(digest, str) or len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest.lower()):
                raise audit_error_t("compiler identities must include SHA-256 hex digests")
    for record in (actual, expected):
        if any(not isinstance(value, str) or len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value.lower())
               for value in record["critical_headers"].values()):
            raise audit_error_t("critical header closure values must be SHA-256 hex digests")
        for archive in record["runtime_archives"].values():
            digest = archive["sha256"]
            if not isinstance(digest, str) or len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest.lower()):
                raise audit_error_t("runtime archive values must be SHA-256 hex digests")
    if actual != expected:
        raise audit_error_t("toolchain/runtime identity mismatch")
    return {"identity": dict(actual), "reviewed_identity": dict(expected)}
