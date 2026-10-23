#!/usr/bin/env python3
"""Adversarial, synthetic-PE tests for clvk_runtime_audit.py."""

from __future__ import annotations

import importlib.util
import copy
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path


Root = Path(__file__).resolve().parents[2]
ModulePath = Root / "scripts" / "clvk_runtime_audit.py"
Spec = importlib.util.spec_from_file_location("clvk_runtime_audit", ModulePath)
Audit = importlib.util.module_from_spec(Spec)
sys.modules[Spec.name] = Audit
Spec.loader.exec_module(Audit)


Base = 0x140000000
CompiledLimits = {"max_objects": 32, "max_bytes": 192, "max_align": 16}
ExpectedExports = ("clBuildProgram",)


def make_pe(*, emutls=((0x2000, 8, 8),), tls_start=0x4020, tls_end=0x40F0,
            index_rva=0x2080, teb_offset=0x58, apis=("TlsAlloc", "TlsFree"),
            bad_import_terminator=False, add_dynamic_name=False):
    """Construct a small PE32+ image with real headers, imports, TLS, and code."""
    # Fixed file mappings: headers 0x000-0x3ff; .text/.data/.rdata/.tls start
    # at 0x400/0x600/0xa00/0xe00 and RVAs 0x1000/0x2000/0x3000/0x4000.
    data = bytearray(0x1000)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    pe = 0x80
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, pe + 4, 0x8664, 4, 0, 0, 0, 0xF0, 0x2022)
    opt = pe + 24
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<Q", data, opt + 24, Base)
    struct.pack_into("<II", data, opt + 32, 0x1000, 0x200)
    struct.pack_into("<II", data, opt + 56, 0x5000, 0x400)
    struct.pack_into("<I", data, opt + 108, 16)
    # Import directory and TLS directory.
    struct.pack_into("<II", data, opt + 112, 0x3300, 0x80)
    struct.pack_into("<II", data, opt + 112 + 8, 0x3000, 40)
    struct.pack_into("<II", data, opt + 112 + 9 * 8, 0x3100, 40)
    section_table = opt + 0xF0
    sections = [
        (b".text", 0x200, 0x1000, 0x200, 0x400, 0x60000020),
        (b".data", 0x400, 0x2000, 0x400, 0x600, 0xC0000040),
        (b".rdata", 0x400, 0x3000, 0x400, 0xA00, 0x40000040),
        (b".tls", 0x200, 0x4000, 0x200, 0xE00, 0xC0000040),
    ]
    for i, (name, vsize, va, raw_size, raw, flags) in enumerate(sections):
        e = section_table + i * 40
        data[e:e + len(name)] = name
        struct.pack_into("<IIII", data, e + 8, vsize, va, raw_size, raw)
        struct.pack_into("<I", data, e + 36, flags)
    # Emulated-TLS controls in writable data.
    symbols = [f"{Base + 0x1000:016x} T __emutls_get_address",
               f"{Base + 0x1040:016x} T __gthr_win32_key_create",
               f"{Base + 0x1080:016x} T __gthr_win32_key_delete",
               f"{Base + 0x1100:016x} T __gthr_win32_create",
               f"{Base + 0x1140:016x} T __gthr_win32_join",
               f"{Base + 0x1180:016x} T __gthr_win32_self",
               f"{Base + tls_start:016x} d runtime_tls_template",
               f"{Base + index_rva:016x} B _tls_index"]
    for i, (rva, size, align) in enumerate(emutls):
        off = 0x600 + (rva - 0x2000)
        struct.pack_into("<QQQQ", data, off, size, align, 0, 0)
        symbols.append(f"{Base + rva:016x} d __emutls_v.fixture_{i}")
    struct.pack_into("<IIII", data, 0xC80, 1,
                     CompiledLimits["max_bytes"], CompiledLimits["max_objects"],
                     CompiledLimits["max_align"])
    symbols.append(f"{Base + 0x3280:016x} r waddle_clvk_runtime_tls_limits")
    struct.pack_into("<IIHHIIIIIII", data, 0xD00, 0, 0, 0, 0, 0x3370, 1,
                     1, 1, 0x3340, 0x3350, 0x3360)
    struct.pack_into("<I", data, 0xD40, 0x1000)
    struct.pack_into("<IH", data, 0xD50, 0x3390, 0)
    data[0xD70:0xD70 + len(b"OpenCL.dll\0")] = b"OpenCL.dll\0"
    data[0xD90:0xD90 + len(b"clBuildProgram\0")] = b"clBuildProgram\0"
    # The required TEB vector access is inside __emutls_get_address.
    teb = bytes((0x65, 0x48, 0x8B, 0x04, 0x25)) + struct.pack("<I", teb_offset)
    data[0x400:0x400 + len(teb)] = teb
    symbols[0] = f"{Base + 0x1000:016x} T __emutls_get_address"
    # Null-terminated import descriptors; IAT is at RVA 0x3060.
    struct.pack_into("<IIIII", data, 0xA00, 0x3040, 0, 0, 0x3030, 0x3060)
    term = (1 if bad_import_terminator else 0, 0, 0, 0, 0)
    struct.pack_into("<IIIII", data, 0xA14, *term)
    data[0xA30:0xA30 + len(b"KERNEL32.dll\0")] = b"KERNEL32.dll\0"
    names = []
    name_cursor = 0xA80
    for api in apis:
        rva = 0x3000 + name_cursor - 0xA00
        names.append((api, rva))
        struct.pack_into("<H", data, name_cursor, 0)
        encoded = api.encode("ascii") + b"\0"
        data[name_cursor + 2:name_cursor + 2 + len(encoded)] = encoded
        name_cursor += 0x20
    for i, (_, rva) in enumerate(names):
        struct.pack_into("<Q", data, 0xA40 + i * 8, rva)
        struct.pack_into("<Q", data, 0xA60 + i * 8, rva)
    struct.pack_into("<Q", data, 0xA40 + len(names) * 8, 0)
    struct.pack_into("<Q", data, 0xA60 + len(names) * 8, 0)
    # TLS directory is in .rdata. The index is in writable .data.
    start_va, end_va = Base + tls_start, Base + tls_end
    struct.pack_into("<QQQQII", data, 0xB00, start_va, end_va, Base + index_rva, 0, 0, 0)
    # IAT calls attributed to exact caller names; additional APIs get unique
    # sites so the fixture can exercise FLS/dynamic lookup policy mutations.
    for index, (api, iat_rva) in enumerate((name, 0x3060 + 8 * i) for i, name in enumerate(apis)):
        if api not in ("TlsAlloc", "TlsFree", "FlsAlloc", "DuplicateHandle", "GetProcAddress"):
            continue
        if api == "TlsAlloc":
            pos, caller = 0x40, "__gthr_win32_key_create"
        elif api == "TlsFree":
            pos, caller = 0x80, "__gthr_win32_key_delete"
        else:
            pos, caller = 0xA0 + index * 8, f"foreign_{api.lower()}_caller"
        rva = 0x1000 + pos
        instruction_va = Base + rva
        target_va = Base + iat_rva
        data[0x400 + pos:0x400 + pos + 6] = b"\xff\x15" + struct.pack("<i", target_va - (instruction_va + 6))
        symbols.append(f"{Base + rva:016x} T {caller}")
    if add_dynamic_name:
        data[0xDB0:0xDB0 + len(b"TlsAlloc\0")] = b"TlsAlloc\0"
    return bytes(data), symbols


def write_file(path: Path, data: bytes) -> Path:
    path.write_bytes(data)
    return path


def valid_policy():
    return {"allowed_api_callers": {"TlsAlloc": ["__gthr_win32_key_create"],
                                    "TlsFree": ["__gthr_win32_key_delete"]},
            "tls_pairs": {"__gthr_win32_key_create": "__gthr_win32_key_delete"}}


def map_text(*, output="OpenCL.dll", abi_owner="clvk_runtime_tls.o", include_eh=True, bad_crt=False):
    lines = [f"OUTPUT({output} pei-x86-64)",
             f" .text 0x1000 0x100 /tmp/{abi_owner}"]
    for symbol in Audit.AbiSymbols:
        lines.append(f" 0x140001000 {symbol}")
    if include_eh:
        lines += [" .text 0x1020 0x20 /tmp/clvk_runtime_eh_pool.o",
                  " 0x140001020 waddle_clvk_eh_pool_register",
                  " 0x140001028 waddle_clvk_eh_pool_release",
                  " 0x140001030 waddle_clvk_eh_pool_initializer",
                  " .rdata 0x1130 0x10 /tmp/clvk_runtime_tls.o",
                  " 0x140001130 waddle_clvk_runtime_tls_limits",
                  " .CRT$XCA 0x1000 0x8 cinitexe.o",
                  " .CRT$XCAWDL 0x1008 0x8 clvk_runtime_eh_pool.o",
                  " .CRT$XCZ 0x1010 0x8 cinitexe.o",
                  " .CRT$XIA 0x1020 0x8 cinitexe.o",
                  " .CRT$XIAA 0x1028 0x8 dllcrt2.o",
                  " .CRT$XIZ 0x1030 0x8 cinitexe.o",
                  " .text 0x1100 0x20 /toolchain/dllcrt2.o",
                  " 0x140001100 atexit",
                  " .text 0x1120 0x20 /toolchain/libstdc++.a(eh_alloc.o)",
                  " 0x140001120 __gnu_cxx::__freeres()",
                  " .text 0x1140 0x60 external/llvm/lib/libLLVMSupport.a(ManagedStatic.cpp.obj)",
                  " 0x140001140 llvm::llvm_shutdown()"]
    if bad_crt:
        lines.append(" .text 0x1200 0x20 /toolchain/libgcc_eh.a(tls_atexit.o)")
    return "\n".join(lines) + "\n"


class runtime_audit_test_t(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="clvk-runtime-audit-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def image(self, **kwargs):
        raw, symbols = make_pe(**kwargs)
        return Audit.pe_image_t("synthetic.dll", raw), symbols

    def test_valid_pe_inventory_tls_and_paired_callers(self):
        raw, symbols = make_pe()
        dll = write_file(self.root / "OpenCL.dll", raw)
        result = Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits,
                                         expected_exports=ExpectedExports,
                                         runtime_policy=valid_policy())
        self.assertEqual(result["emutls_objects"], 1)
        self.assertEqual(result["emutls_worst_case_bytes"], 15)
        self.assertEqual(result["tls"]["template_bytes"], 208)
        self.assertEqual(result["tls"]["template_section"], ".tls")
        self.assertEqual(result["compiled_tls_limits"]["max_bytes"], 192)
        self.assertEqual(result["sensitive_api_callers"]["TlsAlloc"], ["__gthr_win32_key_create"])

    def test_truncated_dos_pe_optional_and_section_bounds_fail_closed(self):
        for raw in (b"", b"MZ", b"MZ" + bytes(64), bytearray(make_pe()[0][:0x250])):
            with self.subTest(size=len(raw)):
                with self.assertRaises(Audit.audit_error_t):
                    Audit.pe_image_t("bad.dll", bytes(raw))
        raw = bytearray(make_pe()[0])
        struct.pack_into("<I", raw, 0x80 + 24 + 0xF0 + 8 + 12, 0xFFFFFFFF)
        with self.assertRaisesRegex(Audit.audit_error_t, "raw data"):
            Audit.pe_image_t("bad.dll", bytes(raw))

    def test_wrong_machine_and_missing_tls_directory_rejected(self):
        raw = bytearray(make_pe()[0])
        struct.pack_into("<H", raw, 0x84, 0x14C)
        with self.assertRaisesRegex(Audit.audit_error_t, "AMD64"):
            Audit.pe_image_t("bad.dll", bytes(raw))
        raw = bytearray(make_pe()[0])
        struct.pack_into("<II", raw, 0x98 + 112 + 9 * 8, 0, 0)
        with self.assertRaisesRegex(Audit.audit_error_t, "TLS directory is missing"):
            Audit.pe_image_t("bad.dll", bytes(raw)).tls_layout()
        raw = bytearray(make_pe()[0])
        struct.pack_into("<I", raw, 0x80 + 24 + 56, 0x2000)
        with self.assertRaisesRegex(Audit.audit_error_t, "SizeOfImage"):
            Audit.pe_image_t("bad_image_size.dll", bytes(raw))
        raw = bytearray(make_pe()[0])
        struct.pack_into("<I", raw, 0x80 + 24 + 60, 0x200)
        with self.assertRaisesRegex(Audit.audit_error_t, "complete section table"):
            Audit.pe_image_t("bad_header_size.dll", bytes(raw))

    def test_tls_bounds_template_alignment_and_address_of_index(self):
        for kwargs, message in (({"tls_start": 0x3FF0}, "file-backed"),
                                ({"tls_end": 0x5000}, "file-backed"),
                                ({"index_rva": 0x5000}, "not uniquely mapped")):
            image, _ = self.image(**kwargs)
            with self.subTest(kwargs=kwargs), self.assertRaises(Audit.audit_error_t):
                image.tls_layout(Base + 0x4020)
        image, symbols = self.image()
        with self.assertRaisesRegex(Audit.audit_error_t, "16-byte aligned"):
            image.tls_layout(Base + 0x4028)
        raw, symbols = make_pe()
        changed_index = bytearray(raw)
        struct.pack_into("<Q", changed_index, 0xB00 + 16, Base + 0x2090)
        dll = write_file(self.root / "bad_index_owner.dll", bytes(changed_index))
        with self.assertRaisesRegex(Audit.audit_error_t, "AddressOfIndex does not resolve"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_teb_register_allocation_and_actual_outlined_call_edge(self):
        for rex in (0x48, 0x4C):
            for register in range(8):
                raw, symbols = make_pe()
                mutable = bytearray(raw)
                mutable[0x400:0x409] = bytes((0x65, rex, 0x8B, 0x04 | register << 3, 0x25)) + struct.pack('<I', 0x58)
                image = Audit.pe_image_t('register.dll', bytes(mutable))
                Audit._verify_teb_tls_accessor(image, Audit._load_symbols(Path('unused'), lines=symbols))
        raw, symbols = make_pe()
        mutable = bytearray(raw)
        mutable[0x400:0x409] = b'\xe8' + struct.pack('<i', 0x11C0 - 0x1005) + bytes(4)
        mutable[0x5C0:0x5C9] = bytes.fromhex('65 4c 8b 04 25 58 00 00 00')
        symbols.append(f'{Base + 0x11C0:016x} t runtime_tls_current_record')
        parsed = Audit._load_symbols(Path('unused'), lines=symbols)
        Audit._verify_teb_tls_accessor(Audit.pe_image_t('outlined.dll', bytes(mutable)), parsed)
        mutable[0x400:0x409] = bytes(9)
        with self.assertRaisesRegex(Audit.audit_error_t, 'direct helper call'):
            Audit._verify_teb_tls_accessor(Audit.pe_image_t('unreachable_decoy.dll', bytes(mutable)), parsed)
        mutable[0x400:0x405] = b'\xe8' + struct.pack('<i', 0x11C0 - 0x1005)
        mutable[0x5C5] = 0x60
        with self.assertRaisesRegex(Audit.audit_error_t, 'TEB offset 0x60'):
            Audit._verify_teb_tls_accessor(Audit.pe_image_t('bad_outlined.dll', bytes(mutable)), parsed)
        raw, symbols = make_pe(teb_offset=0x60)
        dll = write_file(self.root / "bad_teb.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "TEB offset 0x60"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_emutls_inventory_empty_count_and_first_touch_bound(self):
        image, symbols = self.image(emutls=())
        with self.assertRaisesRegex(Audit.audit_error_t, "vacuous"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)
        image, symbols = self.image(emutls=tuple((0x2000 + i * 0x10, 2, 16) for i in range(32)))
        with self.assertRaisesRegex(Audit.audit_error_t, "worst-case bytes"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols),
                                   CompiledLimits)
        image, symbols = self.image(emutls=tuple((0x2000 + i * 0x10, 1, 1) for i in range(33)))
        with self.assertRaisesRegex(Audit.audit_error_t, "object count"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)
        image, symbols = self.image(emutls=((0x2000, 600, 1),))
        with self.assertRaisesRegex(Audit.audit_error_t, "worst-case bytes"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)
        image, symbols = self.image(emutls=((0x2000, 1, 32),))
        with self.assertRaisesRegex(Audit.audit_error_t, "alignment 32"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)

    def test_non_power_two_alignment_and_exact_bound(self):
        image, symbols = self.image(emutls=((0x2000, 1, 3),))
        with self.assertRaisesRegex(Audit.audit_error_t, "non-power-of-two"):
            Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)
        image, symbols = self.image(emutls=((0x2000, 1, 1),))
        result = Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols),
                                        {"max_objects": 1, "max_bytes": 1, "max_align": 1})
        self.assertEqual(result["emutls_worst_case_bytes"], 1)
        image, symbols = self.image(emutls=((0x2000, 8, 8), (0x2000, 8, 8)))
        result = Audit.emutls_inventory(image, Audit._load_symbols(Path("unused"), lines=symbols), CompiledLimits)
        self.assertEqual(result["emutls_objects"], 1, "multiple weak aliases at one address are one control object")

    def test_compiled_limits_metadata_is_required_and_must_match_receipt(self):
        image, symbols = self.image()
        parsed = Audit._load_symbols(Path("unused"), lines=symbols)
        limits = Audit._compiled_tls_limits(image, parsed, CompiledLimits)
        self.assertEqual(limits["max_bytes"], 192)
        with self.assertRaisesRegex(Audit.audit_error_t, "does not match build receipt"):
            Audit._compiled_tls_limits(image, parsed, {**CompiledLimits, "max_bytes": 512})
        no_limits = [item for item in parsed if "waddle_clvk_runtime_tls_limits" not in item[2]]
        with self.assertRaisesRegex(Audit.audit_error_t, "exactly one retained TLS limits symbol"):
            Audit._compiled_tls_limits(image, no_limits, CompiledLimits)

    def test_pe_exports_must_exactly_match_reviewed_def(self):
        raw, _ = make_pe()
        image = Audit.pe_image_t("exports.dll", raw)
        self.assertEqual(image.exports(ExpectedExports), list(ExpectedExports))
        with self.assertRaisesRegex(Audit.audit_error_t, "differs from reviewed DEF"):
            image.exports(("clBuildProgram", "waddle_clvk_eh_pool_release"))
        corrupted = bytearray(raw)
        struct.pack_into("<H", corrupted, 0xD60, 1)
        with self.assertRaisesRegex(Audit.audit_error_t, "invalid PE export ordinal"):
            Audit.pe_image_t("bad_export_ordinal.dll", bytes(corrupted)).exports(ExpectedExports)

    def test_metadata_must_be_read_only_and_complete_record_zeroed(self):
        raw, symbols = make_pe()
        mutable = bytearray(raw)
        # Third section's characteristics: a .rdata name alone is insufficient.
        struct.pack_into("<I", mutable, 0x98 + 0xF0 + 2 * 40 + 36, 0xC0000040)
        dll = write_file(self.root / "writable_metadata.dll", bytes(mutable))
        with self.assertRaisesRegex(Audit.audit_error_t, "read-only"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits,
                                    expected_exports=ExpectedExports, runtime_policy=valid_policy())
        raw, symbols = make_pe(tls_end=0x40E0)
        dll = write_file(self.root / "short_record.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "complete runtime TLS record"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits,
                                    expected_exports=ExpectedExports, runtime_policy=valid_policy())
        raw, symbols = make_pe()
        mutable = bytearray(raw)
        mutable[0xEEF] = 1  # Last byte of actual record, not only the first word.
        dll = write_file(self.root / "dirty_record.dll", bytes(mutable))
        with self.assertRaisesRegex(Audit.audit_error_t, "zero initialized"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits,
                                    expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_sensitive_reference_at_final_instruction_is_not_omitted(self):
        for prefix, width in ((b"\xff\x15", 6), (b"\x48\x8b\x05", 7)):
            raw, symbols = make_pe()
            mutable = bytearray(raw)
            pos = 0x200 - width
            va = Base + 0x1000 + pos
            mutable[0x400 + pos:0x600] = prefix + struct.pack("<i", Base + 0x3060 - (va + width))
            symbols.append(f"{va:016x} T forbidden_tail_caller")
            dll = write_file(self.root / "tail_reference.dll", bytes(mutable))
            with self.subTest(width=width), self.assertRaisesRegex(Audit.audit_error_t, "unapproved TlsAlloc"):
                Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits,
                                        expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_discarded_inventory_cannot_hide_an_actual_forbidden_output(self):
        good = map_text()
        ready_section = ".text$_ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv"
        prefix = ("Discarded input sections\n .text$__at_thread_exit\n 0x0 0x10 /toolchain/libstdc++.a(condition_variable.o)\n "
                  + ready_section + "\n 0x0 0x10 /toolchain/libstdc++.a(future.o)\nLinker script and memory map\n")
        dll = write_file(self.root / "OpenCL.dll", make_pe()[0])
        path = self.root / "test.map"
        path.write_text(prefix + good + " /DISCARD/\n *(.text$__at_thread_exit)\n")
        Audit.verify_runtime_link(path, dll, expected_output_name='OpenCL.dll', map_working_directory=self.root)
        path.write_text(prefix + good + " .text$__at_thread_exit 0x1000 0x10 /toolchain/libstdc++.a(condition_variable.o)\n")
        with self.assertRaisesRegex(Audit.audit_error_t, "forbidden CRT"):
            Audit.verify_runtime_link(path, dll, expected_output_name='OpenCL.dll', map_working_directory=self.root)
        path.write_text(prefix + good + " " + ready_section + " 0x2000 0x10 /toolchain/libstdc++.a(future.o)\n")
        with self.assertRaisesRegex(Audit.audit_error_t, "forbidden CRT"):
            Audit.verify_runtime_link(path, dll, expected_output_name='OpenCL.dll', map_working_directory=self.root)
        path.write_text(prefix.replace('Linker script and memory map', 'unrecognized boundary') + good)
        with self.assertRaisesRegex(Audit.audit_error_t, "unique output boundary"):
            Audit.verify_runtime_link(path, dll, expected_output_name='OpenCL.dll', map_working_directory=self.root)

    def test_gnu_long_section_continuation_preserves_exact_owner(self):
        lines = [" .text.startup 0x140001000 0x10 wrong.o",
                 " .text.unlikely._ZN9__gnu_cxx9__freeresEv",
                 "                0x140001010 0x30 /toolchain/libstdc++.a(eh_alloc.o)",
                 "                0x140001010 __gnu_cxx::__freeres()"]
        self.assertEqual(Audit._symbol_input(lines, '__gnu_cxx::__freeres()'),
                         '/toolchain/libstdc++.a(eh_alloc.o)')

    def test_import_thunk_policies_reject_unpaired_and_unapproved_apis(self):
        raw, symbols = make_pe()
        symbols = [line.replace("__gthr_win32_key_create", "foreign_tls_allocator") for line in symbols]
        dll = write_file(self.root / "bad_caller.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "unapproved TlsAlloc"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())
        raw, symbols = make_pe(apis=("TlsAlloc", "TlsFree", "FlsAlloc"))
        dll = write_file(self.root / "bad_fls.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "unapproved FlsAlloc"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())
        raw, symbols = make_pe(apis=("TlsAlloc", "TlsFree", "DuplicateHandle"))
        dll = write_file(self.root / "bad_duplicate.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "unapproved DuplicateHandle"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_direct_call_to_api_thunk_is_attributed_and_data_alias_fails(self):
        raw, symbols = make_pe()
        mutable = bytearray(raw)
        # Replace the direct TlsAlloc IAT call with a real import thunk and a
        # rel32 caller. The thunk is in .text; its body jumps through the IAT.
        mutable[0x440:0x446] = b"\x90" * 6
        struct.pack_into("<i", mutable, 0x400 + 0xC2,
                         (Base + 0x3060) - (Base + 0x1000 + 0xC0 + 6))
        mutable[0x400 + 0xC0:0x400 + 0xC0 + 6] = b"\xff\x25" + mutable[0x400 + 0xC2:0x400 + 0xC6]
        call_pos = 0xE0
        mutable[0x400 + call_pos] = 0xE8
        struct.pack_into("<i", mutable, 0x400 + call_pos + 1,
                         (Base + 0x10C0) - (Base + 0x1000 + call_pos + 5))
        symbols += [f"{Base + 0x10C0:016x} T TlsAlloc",
                    f"{Base + 0x10E0:016x} T foreign_thunk_caller"]
        image = Audit.pe_image_t("thunk.dll", bytes(mutable))
        imports = image.imports()
        sites = Audit._sensitive_callers(image, imports, Audit._load_symbols(Path("unused"), lines=symbols))
        self.assertIn("foreign_thunk_caller", sites["TlsAlloc"])
        # A .refptr-style absolute alias of the thunk is not allowed to vanish
        # from the caller scan and become an empty successful policy result.
        struct.pack_into("<Q", mutable, 0xA00 + 0x303, Base + 0x10C0)
        image = Audit.pe_image_t("pointer.dll", bytes(mutable))
        with self.assertRaisesRegex(Audit.audit_error_t, "escapes into data pointers"):
            Audit._sensitive_callers(image, image.imports(), Audit._load_symbols(Path("unused"), lines=symbols))

    def test_unsupported_sensitive_api_pointer_encodings_fail_closed(self):
        raw, symbols = make_pe()
        mutable = bytearray(raw)
        iat_va = Base + 0x3060
        instruction_va = Base + 0x1040
        mutable[0x440:0x447] = b"\x48\x8d\x0d" + struct.pack("<i", iat_va - (instruction_va + 7))
        image = Audit.pe_image_t("lea.dll", bytes(mutable))
        with self.assertRaisesRegex(Audit.audit_error_t, "LEA-based sensitive API pointer"):
            Audit._sensitive_callers(image, image.imports(), Audit._load_symbols(Path("unused"), lines=symbols))
        mutable[0x440:0x447] = b"\x90" * 7
        struct.pack_into("<Q", mutable, 0x400 + 0x1A0, iat_va)
        image = Audit.pe_image_t("absolute.dll", bytes(mutable))
        with self.assertRaisesRegex(Audit.audit_error_t, "absolute sensitive API IAT pointer"):
            Audit._sensitive_callers(image, image.imports(), Audit._load_symbols(Path("unused"), lines=symbols))

    def test_sensitive_dynamic_lookup_and_unterminated_imports_rejected(self):
        raw, symbols = make_pe(apis=("TlsAlloc", "TlsFree", "GetProcAddress"), add_dynamic_name=True)
        dll = write_file(self.root / "dynamic.dll", raw)
        dynamic_policy = {**valid_policy(), "dynamic_lookup_callers": ["foreign_getprocaddress_caller"]}
        with self.assertRaisesRegex(Audit.audit_error_t, "sensitive API name"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=dynamic_policy)
        raw, _ = make_pe(bad_import_terminator=True)
        with self.assertRaises(Audit.audit_error_t):
            Audit.pe_image_t("bad_import.dll", raw).imports()

    def test_missing_symbols_and_symbols_forbidden_thread_atexit_rejected(self):
        raw, symbols = make_pe()
        dll = write_file(self.root / "stripped.dll", raw)
        with self.assertRaisesRegex(Audit.audit_error_t, "empty or stripped"):
            Audit.audit_runtime_tls(dll, symbol_lines=[], limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())
        symbols.append(f"{Base + 0x1200:016x} T __cxa_thread_atexit")
        with self.assertRaisesRegex(Audit.audit_error_t, "forbidden thread-exit"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())
        symbols = [line for line in symbols if "__cxa_thread_atexit" not in line]
        symbols.append(f"{Base + 0x1300:016x} T _ZNSt13__future_base13_State_baseV211_Make_ready6_M_setEv")
        with self.assertRaisesRegex(Audit.audit_error_t, "forbidden thread-exit"):
            Audit.audit_runtime_tls(dll, symbol_lines=symbols, limits=CompiledLimits, expected_exports=ExpectedExports, runtime_policy=valid_policy())

    def test_map_binds_output_to_dll_and_requires_shim_eh_crt_and_symbols(self):
        raw, _ = make_pe()
        dll = write_file(self.root / "libOpenCL.dll", raw)
        map_path = self.root / "libOpenCL.dll.map"
        map_path.write_text(map_text(output="libOpenCL.dll"))
        result = Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root)
        self.assertEqual(result["map_output"], "libOpenCL.dll")
        self.assertEqual(result["abi_symbol_inputs"]["__emutls_get_address"], "/tmp/clvk_runtime_tls.o")
        map_path.write_text(map_text(output="libOpenCL.dll").replace(
            "cinitexe.o", "libmingw32.a(lib64_libmingw32_a-cinitexe.o)"))
        self.assertEqual(Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root)["map_output"],
                         "libOpenCL.dll")
        for text, message, kwargs in (("", "empty", {}),
                                      (map_text(output="other.dll"), "expected libOpenCL.dll", {}),
                                      (map_text(output="libOpenCL.dll").replace("clvk_runtime_tls.o", "foreign.o"), "expected Waddle shim", {}),
                                      (map_text(output="libOpenCL.dll", include_eh=False), "required EH-pool", {}),
                                      (map_text(output="libOpenCL.dll", bad_crt=True), "forbidden CRT", {}),
                                      (map_text(output="libOpenCL.dll").replace(
                                          " .CRT$XCAWDL 0x1008 0x8 clvk_runtime_eh_pool.o\n .CRT$XCZ 0x1010 0x8 cinitexe.o",
                                          " .CRT$XCZ 0x1010 0x8 cinitexe.o\n .CRT$XCAWDL 0x1008 0x8 clvk_runtime_eh_pool.o"),
                                       "unexpected .CRT entry", {}),
                                      (map_text(output="libOpenCL.dll").replace("__gthr_win32_join", "foreign_join"), "define __gthr_win32_join", {}),
                                      (map_text(output="libOpenCL.dll").replace("libLLVMSupport.a(ManagedStatic.cpp.obj)", "foreign.a(shutdown.o)"),
                                       "expected ManagedStatic.cpp.obj", {}),
                                      (map_text(output="libOpenCL.dll").replace(" 0x140001140 llvm::llvm_shutdown()\n", ""),
                                       "define llvm::llvm_shutdown()", {})):
            map_path.write_text(text)
            with self.subTest(message=message), self.assertRaises(Audit.audit_error_t):
                Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root, **kwargs)
        probe = map_text(output="libOpenCL.dll").replace(" 0x140001140 llvm::llvm_shutdown()\n", "")
        map_path.write_text(probe)
        self.assertEqual(Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root,
                                                   llvm_runtime=False)["map_output"], "libOpenCL.dll")
        with self.assertRaisesRegex(Audit.audit_error_t, "define llvm::llvm_shutdown"):
            Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root)
        map_path.write_text(map_text(output="libOpenCL.dll"))
        with self.assertRaisesRegex(Audit.audit_error_t, "cannot weaken mandatory"):
            Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root,
                                      runtime_policy={"crt_sections": {"XC": (), "XI": ()}})
        with self.assertRaisesRegex(Audit.audit_error_t, "EH-pool object is mandatory"):
            Audit.verify_runtime_link(map_path, dll, eh_pool_object="",
                                      map_working_directory=self.root)
        map_path.unlink()
        with self.assertRaisesRegex(Audit.audit_error_t, "cannot read link map"):
            Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root)

    def test_runtime_archive_members_require_the_exact_archive(self):
        """A correct object basename cannot authorize a foreign runtime archive."""
        dll = write_file(self.root / "libOpenCL.dll", make_pe()[0])
        map_path = self.root / "runtime.map"
        for symbol, owner, member in (
                ("llvm::llvm_shutdown()", "libLLVMSupport.a", "ManagedStatic.cpp.obj"),
                ("__gnu_cxx::__freeres()", "libstdc++.a", "eh_alloc.o")):
            expected = f"{owner}({member})"
            for replacement in (f"foreign.a({member})", member,
                                f"{owner}(foreign-{member})"):
                map_path.write_text(map_text(output="libOpenCL.dll").replace(expected, replacement))
                with self.subTest(symbol=symbol, owner=replacement), self.assertRaisesRegex(
                        Audit.audit_error_t, "expected " + re.escape(expected)):
                    Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root)
            for prefix in ("/private/runtime/", "C:\\private\\runtime\\"):
                self.assertTrue(Audit._source_matches(prefix + expected, expected))
        map_path.write_text(map_text(output="libOpenCL.dll"))
        for symbol, owner in (("llvm::llvm_shutdown()", "ManagedStatic.cpp.obj"),
                              ("__gnu_cxx::__freeres()", "eh_alloc.o")):
            with self.subTest(symbol=symbol), self.assertRaisesRegex(
                    Audit.audit_error_t, "cannot weaken mandatory"):
                Audit.verify_runtime_link(map_path, dll, map_working_directory=self.root,
                                          runtime_policy={"required_map_symbols": {symbol: owner}})

    def test_toolchain_requires_exact_reviewed_14_2_win32_metadata(self):
        compiler = {"resolved_path": "/toolchain/gcc", "sha256": "a" * 64,
                    "version": "gcc 14.2.0", "dumpfullversion": "14.2.0",
                    "dumpmachine": "x86_64-w64-mingw32", "thread_model": "win32"}
        archives = {name: {"resolved_path": "/runtime/" + name, "sha256": "b" * 64}
                    for name in ("libgcc.a", "libgcc_eh.a", "libstdc++.a", "libmingw32.a",
                                 "libmsvcrt.a", "dllcrt2.o", "crtbegin.o", "crtend.o")}
        fields = {"cc": compiler, "cxx": {**compiler, "resolved_path": "/toolchain/g++"},
                  "runtime_archives": archives, "critical_headers": {"/headers/c++config.h": "c" * 64},
                  "target": "x86_64-w64-mingw32"}
        self.assertEqual(Audit.verify_toolchain_identity(fields, fields)["identity"], fields)
        with self.assertRaisesRegex(Audit.audit_error_t, "mismatch"):
            changed = {**fields, "runtime_archives": {**archives,
                       "libgcc_eh.a": {"resolved_path": "/runtime/libgcc_eh.a", "sha256": "d" * 64}}}
            Audit.verify_toolchain_identity(changed, fields)

    def test_debian_reported_alias_requires_complete_exact_package_proof(self):
        compiler = {"resolved_path": "/private/gcc", "sha256": "a" * 64,
                    "version": "gcc (GCC) 14-win32", "dumpfullversion": "14-win32",
                    "dumpmachine": "x86_64-w64-mingw32", "thread_model": "win32"}
        archives = {name: {"resolved_path": "/private/" + name, "sha256": "b" * 64}
                    for name in ("libgcc.a", "libgcc_eh.a", "libstdc++.a", "libmingw32.a",
                                 "libmsvcrt.a", "dllcrt2.o", "crtbegin.o", "crtend.o")}
        fields = {"cc": compiler, "cxx": {**compiler, "resolved_path": "/private/g++"},
                  "runtime_archives": archives, "critical_headers": {"/private/header": "c" * 64},
                  "target": "x86_64-w64-mingw32"}
        with self.assertRaisesRegex(Audit.audit_error_t, "package provenance"):
            Audit.verify_toolchain_identity(fields, fields)
        gcc_package = "gcc-mingw-w64-x86-64-win32"
        versions = {gcc_package: "14.2.0-19+27+b1", gcc_package + "-runtime": "14.2.0-19+27+b1",
                    "g++-mingw-w64-x86-64-win32": "14.2.0-19+27+b1", "mingw-w64-common": "12.0.0-5",
                    "mingw-w64-x86-64-dev": "12.0.0-5"}
        fields['backend_programs'] = {model: {name: {"sha256": "d" * 64} for name in ('cc1', 'cc1plus', 'collect2')}
                                      for model in ('cc', 'cxx')}
        members = {model: compiler['sha256'] for model in ('cc', 'cxx')}
        members.update({model + ':' + name: 'd' * 64 for model in ('cc', 'cxx') for name in ('cc1', 'cc1plus', 'collect2')})
        members.update({'archive:' + name: item['sha256'] for name, item in archives.items()})
        members['header:/private/header'] = 'c' * 64
        fields['package_provenance'] = {'provenance': {'schema': 1, 'packages': [
            {'name': name, 'version': version, 'sha256': 'e' * 64} for name, version in versions.items()]},
            'package_receipt_sha256': 'f' * 64, 'provenance_sha256': 'f' * 64,
            'package_member_proof': {label: {'relative_path': 'usr/member', 'package': gcc_package, 'sha256': digest}
                                     for label, digest in members.items()}}
        Audit.verify_toolchain_identity(fields, fields)
        for mutation in ('version', 'missing_backend', 'member_digest', 'absolute_path', 'schema'):
            changed = copy.deepcopy(fields)
            origin = changed['package_provenance']
            if mutation == 'version': origin['provenance']['packages'][0]['version'] = '14.3.0'
            elif mutation == 'missing_backend': del origin['package_member_proof']['cc:cc1']
            elif mutation == 'member_digest': origin['package_member_proof']['cxx']['sha256'] = '0' * 64
            elif mutation == 'absolute_path': origin['package_member_proof']['cc']['relative_path'] = '/outside'
            else: origin['provenance']['schema'] = 2
            with self.subTest(mutation=mutation), self.assertRaises(Audit.audit_error_t):
                Audit.verify_toolchain_identity(changed, changed)
        with self.assertRaisesRegex(Audit.audit_error_t, "14.2.0"):
            changed = {**fields, "cc": {**compiler, "version": "gcc 13.2.0"}}
            Audit.verify_toolchain_identity(changed, fields)


if __name__ == "__main__":
    unittest.main(verbosity=2)
