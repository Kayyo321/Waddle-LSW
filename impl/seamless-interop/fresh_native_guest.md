# Fresh Windows native guest preparation

## Scope and guardrails

This test-only preparation tool creates a new Windows Server 2025 Evaluation
installation under `build/fresh_native_guests/`. It never opens, clones, backs,
or writes the preserved Windows 11 disk, its OVMF variables, TPM state, answer
media, seed media, or credentials. It does not download media, boot QEMU, or
claim native VSOCK/service acceptance. A failed preparation retains its private
run directory and failure manifest for recovery.

The selected installer is the repository-recorded Windows Server 2025
Evaluation ISO. `impl/initial-cli-passthrough/IMPL_DESC.md` §10.8 records its
SHA-256 as
`7b052573ba7894c9924e3e87ba732ccd354d18cb75a883efa9b900ea125bfd51` and
records the Fedora VirtIO 0.1.302 ISO hash as
`303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d`. Both
files must already exist at the configured paths and match these digests;
preparation refuses a mismatch. The current `build/waddle-guest-exec.exe` is
also required and its digest is recorded in the private manifest. No third
party download or unpinned media fallback is permitted.

## Generated installation

The supervisor creates a fresh 128 GiB virtual QCOW2 disk with no backing file,
copies the matching OVMF 4 MiB secure-boot code/variables into the private run
directory, and creates an empty private TPM 2.0 state directory. A new random
password is created in-process for the local `waddle_native` test account,
which belongs only to the local `Users` group. A second independent password
is assigned to the built-in Administrator account so Windows Setup does not
need an empty or interactive administrator password. The values must differ.
Each is stored only in its own mode-0600 recovery artifact
(`test-account.secret` and `administrator.secret`), the answer XML, and the
answer ISO. Neither is printed, placed in the manifest, passed in process
arguments, or copied into tool logs. The private run directory and staging
directory are mode 0700.

The generated answer file uses Windows Setup's documented `Autounattend.xml`
search path, provisions the explicit GPT partitions on the new blank disk,
creates the local account, and enables three automatic logons for initial GUI
acceptance. Microsoft documents a `LogonCount` off-by-one issue; the generator
uses the documented one-less value and does not claim guest behavior until
observed. WIM metadata identifies index 2 with `/IMAGE/NAME` value
`Windows Server 2025 SERVERSTANDARD` and display name
`Windows Server 2025 Standard Evaluation (Desktop Experience)`. The answer
file sets a separate built-in Administrator password and selects that exact
`/IMAGE/NAME` value. It creates a GPT layout on disk 0 only: 200 MiB FAT32 EFI,
16 MiB MSR, and the
remaining space as NTFS Windows partition 3. It sets `WillWipeDisk=true` only
for this newly generated blank QCOW2, then explicitly installs to disk 0,
partition 3. The answer ISO includes a specialize-pass script. Under SYSTEM, that
script locates the attached VirtIO media, installs the `viosock` driver from
`viosock\2k25\amd64`, copies the current guest-exec build to `C:\waddle`, and
sets an HKLM Run entry for `waddle-guest-exec.exe --vsock-port 5242`. It adds no
network device, shared folder, USB device, clipboard integration, WinRM, or host
export. The test user's standard account token is preserved.

This startup mechanism and driver binding remain hypotheses until the guest is
booted and the driver/service/listener are observed. The run manifest states
that no guest has been booted and that no acceptance credit is granted.

## Preparation and launch boundary

Preparation validates pinned media hashes, regular-file/non-symlink identity,
the current guest executable, the source-built pinned QEMU stamp, required host
tools, and matching OVMF files before creating a run. `qemu-img create` makes
only the private blank disk. `xorriso` builds only the private answer ISO.
Neither operation launches a VM. The manifest stores input hashes, output
paths/modes, the QEMU build identity, and a QEMU argv template. The VSOCK fd
field remains a placeholder until the caller supplies a validated descriptor
received through the existing scoped handoff; no privileged device operation
is performed here.

The emitted argv uses KVM, the new disk, private firmware/TPM state, readonly
installer and VirtIO media, private QMP/log endpoints, a VGA display, and
`vhost-vsock-pci` with the selected CID. It uses QEMU's `none` display backend
and a VGA device so the guest remains available through QMP screendump without
requiring GTK or a host display. It has no NIC, guest networking, clipboard,
USB, or host export arguments. A later launcher must pass only its
duplicated validated `/dev/vhost-vsock` descriptor with `pass_fds`, supervise
exact QEMU/swtpm children, request QMP ACPI shutdown, bound cleanup, and retain
the run directory if either child cannot be reaped. This implementation stops
at artifact preparation and argv generation; root owns any later launch.
The planned later install observer must impose a 30-minute overall install
deadline, use QMP `system_powerdown`, then terminate/reap only the exact owned
QEMU child if needed; forced shutdown and unreaped children must be distinct
manifest outcomes. That process supervisor is intentionally not part of this
prepare-only slice.

## Failure and recovery

Preflight failures happen before run-directory creation. Once a run directory
exists, failures write a non-secret failure record and retain generated files.
No cleanup deletes a failed run. A successful preparation receipt contains no
answer XML, answer ISO hash, account password, or plaintext command argument.
The private `test-account.secret` exists solely so an operator can recover the
new disposable guest after a reviewed launch; it is never an existing-device
credential. The whole run tree should be retained until QEMU and swtpm are
confirmed stopped; only then may its owner archive or remove that isolated run.

## Verification

The unit suite uses temporary directories, mock `qemu-img`/`xorriso` commands,
and a mocked QEMU provenance validator. It checks pinned-media refusal, symlink
and path refusal, new-disk arguments, answer-file account/driver/startup
settings, secret redaction and file modes, no network/export devices, failure
manifest retention, bounded preparation arguments, explicit GPT/EFI/MSR/NTFS
partition ordering and install target, post-copy guest-executable hash
verification, and tool-failure handling. Tool stdout
and stderr are discarded rather than captured because they could echo input
file contents; outer diagnostics report only an exception class and suppress
all exception details because tools can echo answer-file contents. Tests never start QEMU or
swtpm. Native installation, VSOCK listener reachability, and GUI acceptance
remain separate manual gates.

Microsoft documentation used for answer-file behavior:

- [Automate Windows Setup](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/automate-windows-setup?view=windows-11)
- [LocalAccount unattend setting](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-useraccounts-localaccounts-localaccount)
- [AdministratorPassword unattend setting](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-useraccounts-administratorpassword)
- [AdministratorPassword unattend setting](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-useraccounts-administratorpassword)
- [AutoLogon unattend setting](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-autologon)
- [AutoLogon LogonCount and known issue](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/unattend/microsoft-windows-shell-setup-autologon-logoncount)

## Night checkpoint: actual preparation refusal

The prepare-only prototype passes15 unit tests, but the first real preparation
stopped with a retained `KeyError` before any VM launch. Its private assets are
under `build/fresh_native_guests/fresh-0582e3b441114ef5980dbacb27af3a7c/`.
Do not print/copy the secret files, answer XML or answer ISO from that directory.
The manifest is secret-free. The exact integration defect is that
`prepare_fresh_guest` expects `validated["qemu"]["path"]`, while the real
`native_guest_runner._validate_qemu` result contains `accelerators`, `sha256`,
`source_commit`, `stamp`, and `version`; the test validator incorrectly supplies
an extra `path`. Resume by storing the already validated QEMU binary path as an
explicit field in `_validate_config`, consume that field, and add a regression
using the real provenance result shape. Do not weaken QEMU provenance checks.
The blank disk, private vars/TPM directory and answer media remain recoverable;
no QEMU or swtpm process was started. Installation and guest startup are untested.
