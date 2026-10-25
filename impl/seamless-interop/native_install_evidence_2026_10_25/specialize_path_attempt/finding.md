# Failed specialize bootstrap, no native acceptance

The isolated new Server 2025 guest reached disk installation and its first
restart, then Windows Setup rejected the Deployment RunSynchronousCommand Path.
The filtered Panther error identifies that exact setting as an invalid value;
no password, answer file, registry hive, or credential content was inspected.
The generated source command was 341 characters. Microsoft documents a maximum
of259 characters for this setting:
https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-deployment-runsynchronous-runsynchronouscommand-path

The replacement is239 characters, keeps discovery restricted to CD drives,
executes the first private bootstrap found, propagates its result and returns31
if absent. The source asserts the259 bound; the existing16 preparation tests
now also enforce this actual XML setting. No installed-session credit.

The saved supervisor/probe receipts confirm both exact children reaped and the
probe joined. ACPI did not complete while the setup dialog was displayed; forced
QEMU termination is recorded. All private assets are retained in the receipt
run_root. A separate mode0600 sparse diagnostic copy of the Windows partition
was made only after QEMU exited; ntfscat inspected Panther/setuperr.log only.
That private copy is not staged. Future attempts must use new run assets.
