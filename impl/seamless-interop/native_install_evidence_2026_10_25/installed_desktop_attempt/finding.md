# Installed desktop, execution channel still open

The corrected bounded specialize Path allowed the new isolated Server2025
evaluation guest to reach its real interactive desktop. No legacy account,
credential or base image was used. The bounded host VSOCK probe did not connect
to CID52944 port5242. Desktop boot alone earns no native interop acceptance.
Root requested stop for offline bootstrap diagnostics; saved receipts show
ACPI shutdown without forced termination, both exact children reaped and probe
joined. Private disk/firmware/TPM/answer assets are retained at run_root.

Offline diagnostic copy completed after clean VM shutdown. The restricted
Panther/UnattendGC setup log confirms the exact bootstrap command returned31;
no C:\waddle directory or bootstrap status log was created. A nonsecret mock
ISO reproduced the reason: without Joliet, Windows-visible ISO9660 names replace
hyphens with underscores. The bootstrap searches for a hyphenated filename.
Adding -J preserves both exact script/binary names; filename_probe.json records
the independently generated mock result. No answer ISO or XML content was read
for this diagnosis. Preparation's existing16 tests now require the Joliet flag.
