# Native DXVK heap qualification

Status: capture support implemented; real heap and DXVK qualification pending.

The ordinary Windows fixture keeps its existing arguments and GPU assertions.
`WADDLE_DXVK_HEAP_AUDIT` optionally selects a fresh absolute private directory.
`WADDLE_DXVK_HEAP_CYCLES` selects 1–8 complete application lifetimes. The fixture
publishes `baseline.ready` before acquiring project or DXVK modules, then
`unloaded_N.ready` after each successful GPU workload and complete module release.
Each record identifies the exact process PID, creation time, and native handle
count. Audit-only module-base records allow allocation addresses to be mapped to
the exact preserved DLL/PDB/COFF inputs after unloading.

The supervisor owns every controller, worker, config, UMDH output, and continuation
file. Before admitting the next lifetime it verifies natural host retirement,
starts a fresh authenticated receiver, and writes its config through the existing
private SID transfer. Marker handles close before capture. An exact regular-file
`continue=stage` acknowledgement releases each gate; malformed input or a 60-second
missing acknowledgement fails the fixture. Per-controller, transport, and GPU
completion deadlines remain unchanged.

Microsoft SDK Debugging Tools are copied into an isolated runtime directory and
verified by SHA-256 and Microsoft Authenticode signatures. UST is enabled only for
a fresh unique executable name whose preexisting per-image registry state is
recorded as absent. Cleanup disables UST and restores that exact absence; no
global flags or reboot are needed. `OANOCACHE=1` is confined to the audited process.

Acceptance requires all real device, exact rendering/compute, and presented pixel
assertions; natural owned-process retirement; project DLL absence; exact native
owner counts across lifetimes after independently attributed OS initialization;
and UMDH allocation-stack differences across baseline and complete unloads.
Every surviving project or DXVK allocation must be explained and released.
Independently proven Windows process caches are recorded by their exact stacks;
raw heap totals or blanket exclusions cannot establish zero leaks.
