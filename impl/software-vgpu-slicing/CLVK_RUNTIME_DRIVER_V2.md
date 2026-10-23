# CLVK Win32 runtime build driver v2

This contract defines `scripts/build_clvk_windows.py` and its unit coverage in
`tests/vgpu/clvk_build.py`. The driver prepares a static OpenCL DLL with the
GCC 14.2.0 MinGW **win32** thread model, links Waddle's runtime objects ahead
of static archives, and admits the exact link map and PE image before it copies
or publishes a DLL. A successful cross-build is static build evidence only; it
does not establish Windows loader, TLS, heap, handle, GPU, or workload behavior.

## Selectors and reviewed inputs

Every build names the C compiler, C++ compiler, `dlltool`, `objdump`, `nm`,
`windres`, `ar`, and `ranlib` with explicit executable paths. CMake receives
those same paths from its generated toolchain file. The driver does not select
a compiler through the ambient MinGW `PATH`. CMake and Ninja are resolved once
from `PATH`, then their resolved paths, SHA-256 values, and version strings are
recorded and included in the cache key.

Before any LLVM build, the driver captures the selected compiler identity and
requires an externally reviewed toolchain JSON whose complete record matches:

```json
{
  "target": "x86_64-w64-mingw32",
  "cc": {
    "resolved_path": "...",
    "sha256": "...",
    "version": "...",
    "dumpfullversion": "14-win32",
    "dumpmachine": "x86_64-w64-mingw32",
    "thread_model": "win32"
  },
  "cxx": { "same fields as cc" },
  "runtime_archives": {
    "libgcc.a": { "resolved_path": "...", "sha256": "..." },
    "libgcc_eh.a": { "resolved_path": "...", "sha256": "..." },
    "libstdc++.a": { "resolved_path": "...", "sha256": "..." },
    "libmingw32.a": { "resolved_path": "...", "sha256": "..." },
    "libmsvcrt.a": { "resolved_path": "...", "sha256": "..." },
    "dllcrt2.o": { "resolved_path": "...", "sha256": "..." },
    "crtbegin.o": { "resolved_path": "...", "sha256": "..." },
    "crtend.o": { "resolved_path": "...", "sha256": "..." }
  },
  "critical_headers": { "resolved header path": "SHA-256" }
}
```

The header closure is captured by asking the selected C++ compiler to preprocess
`<thread>`, `<mutex>`, and `<exception>` and hashing every resolved dependency.
GCC 14.2.0-19+27+b1 reports its own version as `14-win32`; the driver preserves
that exact string and binds it to Debian package provenance instead of rewriting
it as `14.2.0`. The identity also hashes `cc1`, `cc1plus`, `collect2`, `as`, and
`ld`. `--toolchain-provenance FILE` is required for both capture and build. Its
schema is:

```json
{
  "schema": 1,
  "package_receipt_path": ".../package_receipt.json",
  "extracted_root": ".../extracted",
  "packages": [
    {"name": "...", "version": "...", "deb_path": "...deb", "sha256": "..."}
  ]
}
```

The driver verifies each `.deb` hash and Debian package/version metadata against
the package receipt, then checks compiler, GCC backend, runtime archive, and each
critical-header byte against an exact member of one declared package. Host
binutils outside the private extraction remain separately recorded by resolved
path, file hash, and version. The toolchain record is host-specific.
`--capture-toolchain FILE` writes the identity and package-member proof atomically
and exits without checking pins, compiling sources, or building LLVM. The capture
is evidence for review; it is not self-approval. A normal build requires
`--reviewed-toolchain FILE` and a separate `--runtime-policy FILE`.
The isolated probe also requires `--probe-runtime-policy FILE`. Its caller
lists and TLS pairs are independently reviewed because its source callers differ
from OpenCL's production callers.

The runtime policy JSON has exactly these fields:

```json
{
  "allowed_api_callers": {
    "TlsAlloc": ["reviewed exact symbol names"],
    "FlsAlloc": [],
    "DuplicateHandle": []
  },
  "tls_pairs": { "TlsAlloc caller": "matching TlsFree caller" },
  "dynamic_lookup_callers": []
}
```

Caller names must be reviewed against source and the audited compiler output.
The driver never derives an allowlist from observed PE callers. The schema does
not permit caller-supplied forbidden-symbol or CRT-order overrides; the audit
helper's mandatory policies remain in force. The complete JSON bytes and parsed
policy are hashed and stored in the receipt/cache key.
The probe policy uses the same exact schema, but is a separate reviewed file.
Both policy files' complete bytes are hashed and checked again before publish.

Example capture and build commands (replace `PRIVATE` with the reviewed
extraction's executable paths and provide the reviewed files):

```sh
python3 scripts/build_clvk_windows.py \
  --cc PRIVATE/usr/bin/x86_64-w64-mingw32-gcc-win32 \
  --cxx PRIVATE/usr/bin/x86_64-w64-mingw32-g++-win32 \
  --dlltool /usr/bin/x86_64-w64-mingw32-dlltool \
  --objdump /usr/bin/x86_64-w64-mingw32-objdump \
  --nm /usr/bin/x86_64-w64-mingw32-nm \
  --windres /usr/bin/x86_64-w64-mingw32-windres \
  --ar PRIVATE/usr/bin/x86_64-w64-mingw32-gcc-ar-win32 \
  --ranlib PRIVATE/usr/bin/x86_64-w64-mingw32-gcc-ranlib-win32 \
  --toolchain-provenance REVIEWED-PACKAGES.json \
  --probe-runtime-policy PROBE-POLICY.json \
  --capture-toolchain build/qualification_feature_20261021/driver_worker/toolchain.json
```

The capture command is repeated with the same selectors plus
`--reviewed-toolchain REVIEWED.json --runtime-policy POLICY.json
--probe-runtime-policy PROBE-POLICY.json` to request a build. A missing reviewed
file is a preflight error. There is no option to skip
tool identity, probe admission, map admission, PE admission, or source hashing.

## Preflight and build order

The normal invocation performs these gates before the expensive native LLVM and
libclc build:

1. Resolve and hash every selected tool. Query compiler version, target, thread
   model, runtime archive paths/hashes, and the C++ standard-header closure.
2. Require exact reviewed identity, package-member provenance, and policy JSON.
   Require `x86_64-w64-mingw32` and the `win32` model for both compilers, while
   preserving each compiler's exact reported version string.
3. Hash both Waddle runtime C files, the probe C/C++ sources, linker-section
   script and its negative-link control, audit helper, OpenCL export definition,
   shutdown ABI header, driver, and private patches.
   Validate all required paths and compare source exports before a long build.
4. Verify pinned source trees and compile both isolated probe harnesses plus the
   C++ Win32-model probe DLL with `-mthreads`, `-Wpedantic`, static GCC/libstdc++,
   and the selected runtime objects. Retain runtime metadata with
   `--undefined=waddle_clvk_runtime_tls_limits`; apply the reviewed linker-section
   script and undefined-reference wrappers for unsupported thread-exit and
   gthread-equality APIs. Run `verify_runtime_link` against the probe's exact
   map/DLL `OUTPUT` and PE/TLS/API admission using the separate probe policy. A
   rejected probe records its hashes and exact reason in
   `probe_preflight/probe_preflight.json` and stops before LLVM compilation.
   Neither probe executable is run by the driver.
5. Compile `clvk_runtime_tls.c` and `clvk_runtime_eh_pool.c` as C11 objects with
   warnings-as-errors, `-mthreads`, and the recorded TLS limits. Their object
   paths lead the shared-link flags, before archive libraries. The same
   metadata-retention flag, linker-section script, and three undefined-reference
   wrappers apply to production. The exact generated link map is retained beside
   the CMake output.
6. Build OpenCL from a private patched source copy. The original submodules are
   read-only inputs. The linked output must be exactly one `libOpenCL.dll` and
   its fresh map must exist. Before copying, run `verify_runtime_link` with the
   exact map/DLL paths, then `audit_runtime_tls` with reviewed limits, exports,
   policy, and explicit `nm` path. These checks enforce ABI ownership, CRT
   ordering, PE/TLS structure, API caller policy, and forbidden thread-exit
   symbol rejection.
7. Only after both audits pass, inspect the PE export/import table, copy the DLL,
   compare its SHA-256 with the admitted bytes, recheck source/tool/policy hashes,
   rerun both admission checks, and atomically replace the receipt. A failure
   leaves the previous receipt unchanged.

The receipt binds source/helper/patch hashes, reviewed and captured toolchain
identities, selected support-tool hashes, runtime-policy bytes and values, TLS
limits, pins, link-map hash/output/work directory, original linked DLL hash,
published-copy hash, audit results, imports, and exports. It records
`executed=false` and `native_acceptance=false`.
With the default output root, the compatibility receipt remains
`build/clvk_client.json`; an explicitly selected custom output root gets a local
`clvk_client.json` there. The native LLVM cache key includes only the LLVM pin,
LLVM switches, host C/C++ compiler identities, and host CMake/Ninja identities.
It is independent of the selected MinGW compiler and Windows support tools.

## Cache behavior and non-claims

A matching cache key is not acceptance. Reuse rechecks the original linked DLL,
link-map, and published-copy hashes, verifies that the copy equals the linked
image, recalculates source/helper/toolchain/policy hashes, and runs both static
admission helpers again. A stale or rejected cache is not returned as success;
the driver proceeds to a fresh isolated build. The previous receipt is replaced
only after the new image passes all gates.

The driver does not run a native executable, load the DLL, contact a VM, use a
GPU, install an SDK, alter source submodules, or change upstream pins. Natural
2048-cycle completion, Windows loader behavior, actual TLS/FLS/heap/handle
retirement, and GPU workload behavior remain separate native qualification
gates. A static rejection is preserved as a failure report; it cannot be
overridden by environment variables or a permissive cache.

## Driver tests

Run:

```sh
python3 -B tests/vgpu/clvk_build.py
python3 -O -B tests/vgpu/clvk_build.py
```

Tests cover explicit compiler selectors and toolchain-file generation, capture
without building, package provenance rejection, separate probe policy, required
linker fences, host-only LLVM cache keying, canonical receipt location, probe
map/PE admission, cache re-admission, stale map rejection, and receipt
preservation when final admission fails. They do not execute a Windows binary or
claim candidate DLL acceptance.

### Independent root preflight evidence (2026-10-21)

Root ran all34 driver tests normally and optimized outside ptrace; both pass,
including actual ASan/LSan extraction tests. Worker ptrace-related sanitizer
failures remain in their original logs. A real preflight exposed a Python3.13
importlib/dataclass registration defect missed by stub tests. The corrected
loader registers the actual helper in sys.modules before execution and restores
the prior entry on failure; regression tests load the real dataclass module and
verify reviewed preflight inputs. A root receipt-serialization error occurred
after preflight (Path is not JSON serializable), before probe compilation;
its original output is retained and corrected with explicit path serialization.
The subsequent actual probe C/C++ compilation, strict map/PE/TLS/API admission,
three-cycle executable and2048-cycle executable builds all pass. Evidence is in
`build/qualification_feature_20261021/root_tls/driver_root_review_v2/` (full
test logs/receipt and `probe_v2/probe_preflight/probe_preflight.json`). This is
static evidence only: no candidate OpenCL native qualification or acceptance.

### Build entry point and CI integration

`make vgpu-opencl-build ClvkBuildArgs='COMPLETE REVIEWED ARGUMENTS'` forwards
all explicit selectors to the driver. An empty argument set is a Make error;
there is no ambient compiler/policy fallback. The target names all three approved
patches, both Waddle C objects, the audit helper and linker supplement as inputs.
The OpenCL CI job runs optimized build-driver tests, both modes of adversarial
runtime-image tests, real-source TLS/EH plain and sanitizer/coverage gates, and
the actual native-probe cross-build matrix. This job does not build or execute
the final OpenCL runtime and earns no native/physical acceptance credit.
Root reran this exact new Make gate group successfully outside ptrace, verified
empty-selector rejection and explicit forwarding, and parsed workflow YAML.
Receipt: `build/qualification_feature_20261021/root_tls/ci_gate_wiring/receipt.json`.
Remote CI for the current candidate remains outstanding.
