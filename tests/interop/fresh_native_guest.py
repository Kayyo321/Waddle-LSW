"""Prepare a private, fresh Windows guest for later native acceptance.

Preparation validates pinned inputs, creates only new per-run artifacts and
records a QEMU argv template. It never starts QEMU or swtpm and never accesses
the existing Windows 11 image, answer media, seed, or credential store.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import html
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
import string
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ElementTree
from typing import Any, Callable, Sequence

Root = Path(__file__).resolve().parents[2]
DefaultInstallIso = Path("/home/dev/.local/share/waddle-vm-validation/windows_server.iso")
DefaultVirtioIso = Path("/home/dev/.local/share/waddle-vm-validation/virtio_win.iso")
DefaultQemuBuildDir = Root / "build" / "qemu-seamless-source"
DefaultGuestExec = Root / "build" / "waddle-guest-exec.exe"
DefaultRunParent = Root / "build" / "fresh_native_guests"
DefaultOvmfCode = Path("/usr/share/OVMF/OVMF_CODE_4M.secboot.fd")
DefaultOvmfVars = Path("/usr/share/OVMF/OVMF_VARS_4M.ms.fd")
WindowsServerIsoSha256 = "7b052573ba7894c9924e3e87ba732ccd354d18cb75a883efa9b900ea125bfd51"
VirtioIsoSha256 = "303f7ae40dad495d6ae474fdc571df58958a4dbc5c37a522d80f9a203867949d"
GuestDiskBytes = 128 * 1024 * 1024 * 1024
PreparationTimeoutSeconds = 180
MaximumPreparationTimeoutSeconds = 300
VsockMinimumCid = 3
VsockMaximumCid = 0xFFFFFFFE
UnattendNs = "urn:schemas-microsoft-com:unattend"
WcmNs = "http://schemas.microsoft.com/WMIConfig/2002/State"
XsiNs = "http://www.w3.org/2001/XMLSchema-instance"
ElementTree.register_namespace("", UnattendNs)
ElementTree.register_namespace("wcm", WcmNs)
ElementTree.register_namespace("xsi", XsiNs)


class fresh_guest_error_t(RuntimeError):
    """A preflight or preparation failure; any created private run is retained."""


@dataclasses.dataclass(frozen=True)
class fresh_guest_config_t:
    """Caller-selected paths and host settings for one fresh installation.

    ``workspace_root`` bounds output; ``install_iso`` and ``virtio_iso`` are
    read-only pinned inputs; ``qemu_build_dir`` and ``guest_exec`` identify
    validated built artifacts; ``run_parent`` receives private output;
    ``ovmf_code`` and ``ovmf_vars`` are read-only firmware inputs;
    ``guest_cid``, ``memory_mib`` and ``cpus`` select the later launch;
    ``image_name`` selects the evaluation edition; and
    ``preparation_timeout_seconds`` bounds each preparation subprocess.
    """

    workspace_root: Path = Root
    install_iso: Path = DefaultInstallIso
    virtio_iso: Path = DefaultVirtioIso
    qemu_build_dir: Path = DefaultQemuBuildDir
    guest_exec: Path = DefaultGuestExec
    run_parent: Path = DefaultRunParent
    ovmf_code: Path = DefaultOvmfCode
    ovmf_vars: Path = DefaultOvmfVars
    guest_cid: int = 52942
    memory_mib: int = 8192
    cpus: int = 4
    image_name: str = "Windows Server 2025 SERVERSTANDARD"
    preparation_timeout_seconds: int = PreparationTimeoutSeconds


def _sha256_file(path: Path) -> str:
    """Hash a regular input file without retaining its contents."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _regular_file(path: Path, label: str) -> Path:
    """Return a canonical regular-file path, refusing symlinks."""
    if path.is_symlink():
        raise fresh_guest_error_t(f"{label} must not be a symlink")
    try:
        info = path.stat()
    except OSError as error:
        raise fresh_guest_error_t(f"{label} is unavailable: {error}") from error
    if not stat.S_ISREG(info.st_mode):
        raise fresh_guest_error_t(f"{label} must be a regular file")
    return path.resolve(strict=True)


def _validate_cid(guest_cid: int) -> None:
    """Reject booleans and reserved/out-of-range guest CIDs."""
    if (isinstance(guest_cid, bool) or not isinstance(guest_cid, int) or
            not VsockMinimumCid <= guest_cid <= VsockMaximumCid):
        raise fresh_guest_error_t(
            f"guest CID must be an integer in {VsockMinimumCid}..{VsockMaximumCid}")


def _validate_resources(memory_mib: int, cpus: int, timeout_seconds: int) -> None:
    """Validate strict integer VM and preparation bounds before argv creation."""
    if (isinstance(memory_mib, bool) or not isinstance(memory_mib, int) or
            not 2048 <= memory_mib <= 32768):
        raise fresh_guest_error_t("guest memory must be an integer from 2048 to 32768 MiB")
    if isinstance(cpus, bool) or not isinstance(cpus, int) or not 1 <= cpus <= 16:
        raise fresh_guest_error_t("guest CPU count must be an integer from 1 to 16")
    if (isinstance(timeout_seconds, bool) or not isinstance(timeout_seconds, int) or
            not 1 <= timeout_seconds <= MaximumPreparationTimeoutSeconds):
        raise fresh_guest_error_t("preparation timeout must be an integer within supported bounds")


def _validate_config(config: fresh_guest_config_t, *,
                     qemu_validator: Callable[[Path, Path], dict[str, Any]] | None = None
                     ) -> dict[str, Any]:
    """Validate all read-only inputs and host tools before creating a run."""
    _validate_cid(config.guest_cid)
    _validate_resources(config.memory_mib, config.cpus, config.preparation_timeout_seconds)
    if (not isinstance(config.image_name, str) or not config.image_name or
            len(config.image_name) > 128 or any(
            ord(character) < 32 for character in config.image_name)):
        raise fresh_guest_error_t("Windows image name is invalid")
    for path in (config.install_iso, config.virtio_iso, config.ovmf_code,
                 config.ovmf_vars, config.run_parent):
        if "," in str(path) or any(ord(character) < 32 for character in str(path)):
            raise fresh_guest_error_t("QEMU keyval paths cannot contain comma or control bytes")

    install_iso = _regular_file(config.install_iso, "Windows installer ISO")
    virtio_iso = _regular_file(config.virtio_iso, "VirtIO installer ISO")
    if _sha256_file(install_iso) != WindowsServerIsoSha256:
        raise fresh_guest_error_t("Windows installer ISO does not match the pinned evaluation media")
    if _sha256_file(virtio_iso) != VirtioIsoSha256:
        raise fresh_guest_error_t("VirtIO ISO does not match the pinned 0.1.302 media")
    guest_exec = _regular_file(config.guest_exec, "guest-exec build")
    ovmf_code = _regular_file(config.ovmf_code, "OVMF secure-boot code")
    ovmf_vars = _regular_file(config.ovmf_vars, "OVMF Microsoft variables template")

    if config.qemu_build_dir.is_symlink():
        raise fresh_guest_error_t("QEMU build directory must not be a symlink")
    try:
        qemu_build_dir = config.qemu_build_dir.resolve(strict=True)
    except OSError as error:
        raise fresh_guest_error_t(f"QEMU build directory is unavailable: {error}") from error
    qemu = _regular_file(qemu_build_dir / "qemu-system-x86_64", "QEMU binary")
    qemu_stamp = _regular_file(qemu_build_dir / ".waddle-qemu-build.json", "QEMU build stamp")
    qemu_img = shutil.which("qemu-img")
    swtpm = shutil.which("swtpm")
    xorriso = shutil.which("xorriso")
    if qemu_img is None or swtpm is None or xorriso is None:
        raise fresh_guest_error_t("qemu-img, swtpm, and xorriso must be installed")
    if Path(qemu_img).is_symlink() or Path(swtpm).is_symlink() or Path(xorriso).is_symlink():
        raise fresh_guest_error_t("host preparation tools must not be symlinks")
    try:
        if qemu_validator is None:
            from native_guest_runner import _validate_qemu
            qemu_info = _validate_qemu(qemu, qemu_stamp)
        else:
            qemu_info = qemu_validator(qemu, qemu_stamp)
    except (OSError, ValueError, RuntimeError) as error:
        raise fresh_guest_error_t(f"pinned QEMU validation failed: {error}") from error

    try:
        workspace = config.workspace_root.resolve(strict=True)
    except OSError as error:
        raise fresh_guest_error_t(f"workspace root is unavailable: {error}") from error
    if config.workspace_root.is_symlink() or not workspace.is_dir():
        raise fresh_guest_error_t("workspace root must be a real directory")
    expected_parent = workspace / "build" / "fresh_native_guests"
    if config.run_parent != expected_parent and expected_parent not in config.run_parent.parents:
        raise fresh_guest_error_t("fresh guest artifacts must stay under build/fresh_native_guests")
    try:
        relative_parent = config.run_parent.relative_to(workspace)
    except ValueError as error:
        raise fresh_guest_error_t("fresh guest run parent is outside the workspace") from error
    cursor = workspace
    for part in relative_parent.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise fresh_guest_error_t("fresh guest output path contains a symlink")
    if config.run_parent.exists():
        if not config.run_parent.is_dir():
            raise fresh_guest_error_t("fresh guest run parent must be a real directory")
        resolved_parent = config.run_parent.resolve(strict=True)
        if resolved_parent != expected_parent and expected_parent not in resolved_parent.parents:
            raise fresh_guest_error_t("fresh guest run parent resolves outside build/fresh_native_guests")
        if stat.S_IMODE(config.run_parent.stat().st_mode) != 0o700:
            raise fresh_guest_error_t("fresh guest run parent must have mode 0700")

    return {
        "install_iso": {"path": str(install_iso), "sha256": WindowsServerIsoSha256,
                        "size_bytes": install_iso.stat().st_size},
        "virtio_iso": {"path": str(virtio_iso), "sha256": VirtioIsoSha256,
                       "size_bytes": virtio_iso.stat().st_size},
        "guest_exec": {"path": str(guest_exec), "sha256": _sha256_file(guest_exec),
                       "size_bytes": guest_exec.stat().st_size},
        "ovmf_code": {"path": str(ovmf_code), "sha256": _sha256_file(ovmf_code)},
        "ovmf_vars_template": {"path": str(ovmf_vars), "sha256": _sha256_file(ovmf_vars)},
        # Keep the canonical binary path separately: native_guest_runner's
        # provenance result intentionally describes identity, not location.
        "qemu_path": str(qemu),
        "qemu": qemu_info,
        "tools": {"qemu_img": str(Path(qemu_img).resolve(strict=True)),
                  "swtpm": str(Path(swtpm).resolve(strict=True)),
                  "xorriso": str(Path(xorriso).resolve(strict=True))},
    }


def _qname(local_name: str) -> str:
    return f"{{{UnattendNs}}}{local_name}"


def _component(parent: ElementTree.Element, name: str) -> ElementTree.Element:
    component = ElementTree.SubElement(parent, _qname("component"), {
        "name": name,
        "processorArchitecture": "amd64",
        "publicKeyToken": "31bf3856ad364e35",
        "language": "neutral",
        "versionScope": "nonSxS",
    })
    return component


def _add_text(parent: ElementTree.Element, name: str, value: str) -> ElementTree.Element:
    element = ElementTree.SubElement(parent, _qname(name))
    element.text = value
    return element


def _answer_xml(user_password: str, administrator_password: str, image_name: str) -> bytes:
    """Create unattended XML; caller owns both secret-bearing inputs and output bytes."""
    root = ElementTree.Element(_qname("unattend"), {
        f"{{{WcmNs}}}Ignorable": "wcm",
        f"{{{XsiNs}}}schemaLocation": f"{UnattendNs} unattend.xsd",
    })
    windows_pe = ElementTree.SubElement(root, _qname("settings"), {"pass": "windowsPE"})
    intl = _component(windows_pe, "Microsoft-Windows-International-Core-WinPE")
    setup_ui = ElementTree.SubElement(intl, _qname("SetupUILanguage"))
    _add_text(setup_ui, "UILanguage", "en-US")
    _add_text(intl, "InputLocale", "en-US")
    _add_text(intl, "SystemLocale", "en-US")
    _add_text(intl, "UILanguage", "en-US")
    _add_text(intl, "UserLocale", "en-US")

    setup = _component(windows_pe, "Microsoft-Windows-Setup")
    disk_configuration = ElementTree.SubElement(setup, _qname("DiskConfiguration"))
    disk = ElementTree.SubElement(disk_configuration, _qname("Disk"),
                                  {f"{{{WcmNs}}}action": "add"})
    _add_text(disk, "DiskID", "0")
    _add_text(disk, "WillWipeDisk", "true")
    create_partitions = ElementTree.SubElement(disk, _qname("CreatePartitions"))
    efi = ElementTree.SubElement(create_partitions, _qname("CreatePartition"),
                                 {f"{{{WcmNs}}}action": "add"})
    _add_text(efi, "Order", "1")
    _add_text(efi, "Type", "EFI")
    _add_text(efi, "Size", "200")
    msr = ElementTree.SubElement(create_partitions, _qname("CreatePartition"),
                                 {f"{{{WcmNs}}}action": "add"})
    _add_text(msr, "Order", "2")
    _add_text(msr, "Type", "MSR")
    _add_text(msr, "Size", "16")
    primary = ElementTree.SubElement(create_partitions, _qname("CreatePartition"),
                                     {f"{{{WcmNs}}}action": "add"})
    _add_text(primary, "Order", "3")
    _add_text(primary, "Type", "Primary")
    _add_text(primary, "Extend", "true")
    modify_partitions = ElementTree.SubElement(disk, _qname("ModifyPartitions"))
    modify_efi = ElementTree.SubElement(modify_partitions, _qname("ModifyPartition"),
                                        {f"{{{WcmNs}}}action": "add"})
    _add_text(modify_efi, "Order", "1")
    _add_text(modify_efi, "PartitionID", "1")
    _add_text(modify_efi, "Format", "FAT32")
    _add_text(modify_efi, "Label", "System")
    modify_primary = ElementTree.SubElement(modify_partitions, _qname("ModifyPartition"),
                                            {f"{{{WcmNs}}}action": "add"})
    _add_text(modify_primary, "Order", "2")
    _add_text(modify_primary, "PartitionID", "3")
    _add_text(modify_primary, "Format", "NTFS")
    _add_text(modify_primary, "Label", "Windows")
    _add_text(modify_primary, "Letter", "C")
    userdata = ElementTree.SubElement(setup, _qname("UserData"))
    _add_text(userdata, "AcceptEula", "true")
    image_install = ElementTree.SubElement(setup, _qname("ImageInstall"))
    os_image = ElementTree.SubElement(image_install, _qname("OSImage"))
    install_from = ElementTree.SubElement(os_image, _qname("InstallFrom"))
    metadata = ElementTree.SubElement(install_from, _qname("MetaData"), {f"{{{WcmNs}}}action": "add"})
    _add_text(metadata, "Key", "/IMAGE/NAME")
    _add_text(metadata, "Value", image_name)
    install_to = ElementTree.SubElement(os_image, _qname("InstallTo"))
    _add_text(install_to, "DiskID", "0")
    _add_text(install_to, "PartitionID", "3")

    specialize = ElementTree.SubElement(root, _qname("settings"), {"pass": "specialize"})
    deploy = _component(specialize, "Microsoft-Windows-Deployment")
    commands = ElementTree.SubElement(deploy, _qname("RunSynchronous"))
    command = ElementTree.SubElement(commands, _qname("RunSynchronousCommand"),
                                     {f"{{{WcmNs}}}action": "add"})
    _add_text(command, "Order", "1")
    _add_text(command, "Description", "Install VirtIO VSOCK and stage guest-exec")
    # RunSynchronousCommand/Path permits at most 259 characters. Keep CD
    # discovery here; the full bootstrap stays on the private answer medium.
    command_line = (
        "foreach($d in Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=5'){"
        "$p=$d.DeviceID+'\\waddle-guest-setup.ps1';"
        "if(Test-Path $p){& $p;exit $LASTEXITCODE}};exit 31"
    )
    setup_path = ("powershell.exe -NoProfile -NonInteractive "
                  "-ExecutionPolicy Bypass -Command \"" + command_line + "\"")
    if len(setup_path) > 259:
        raise fresh_guest_error_t("specialize bootstrap exceeds the Windows Setup Path limit")
    _add_text(command, "Path", setup_path)
    _add_text(command, "WillReboot", "Never")
    shell_specialize = _component(specialize, "Microsoft-Windows-Shell-Setup")
    _add_text(shell_specialize, "ComputerName", "WADDLE-FRESH")

    oobe = ElementTree.SubElement(root, _qname("settings"), {"pass": "oobeSystem"})
    shell = _component(oobe, "Microsoft-Windows-Shell-Setup")
    oobe_settings = ElementTree.SubElement(shell, _qname("OOBE"))
    _add_text(oobe_settings, "HideEULAPage", "true")
    _add_text(oobe_settings, "HideLocalAccountScreen", "true")
    _add_text(oobe_settings, "HideOnlineAccountScreens", "true")
    _add_text(oobe_settings, "ProtectYourPC", "3")
    user_accounts = ElementTree.SubElement(shell, _qname("UserAccounts"))
    admin_password = ElementTree.SubElement(user_accounts, _qname("AdministratorPassword"))
    admin_value = ElementTree.SubElement(admin_password, _qname("Value"))
    admin_value.text = administrator_password
    _add_text(admin_password, "PlainText", "true")
    local_accounts = ElementTree.SubElement(user_accounts, _qname("LocalAccounts"))
    local_account = ElementTree.SubElement(local_accounts, _qname("LocalAccount"),
                                           {f"{{{WcmNs}}}action": "add"})
    account_password = ElementTree.SubElement(local_account, _qname("Password"))
    _add_text(account_password, "Value", user_password)
    _add_text(account_password, "PlainText", "true")
    _add_text(local_account, "Description", "Disposable native acceptance test account")
    _add_text(local_account, "DisplayName", "Waddle Native Test")
    _add_text(local_account, "Group", "Users")
    _add_text(local_account, "Name", "waddle_native")
    autologon = ElementTree.SubElement(shell, _qname("AutoLogon"))
    autologon_password = ElementTree.SubElement(autologon, _qname("Password"))
    _add_text(autologon_password, "Value", user_password)
    _add_text(autologon_password, "PlainText", "true")
    _add_text(autologon, "Enabled", "true")
    # Microsoft documents a Windows Setup off-by-one; value 2 yields 3 logons.
    _add_text(autologon, "LogonCount", "2")
    _add_text(autologon, "Username", "waddle_native")

    return ElementTree.tostring(root, encoding="utf-8", xml_declaration=True)


def _guest_setup_script() -> bytes:
    """Return SYSTEM specialize script without credentials or external sharing."""
    script = r'''$ErrorActionPreference = 'Stop'
$log = Join-Path $env:windir 'Temp\waddle-fresh-setup.log'
try {
    $answerRoot = $null
    foreach ($volume in Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=5') {
        $candidate = $volume.DeviceID
        if (Test-Path -LiteralPath (Join-Path $candidate 'waddle-guest-exec.exe')) {
            $answerRoot = $candidate
            break
        }
    }
    if (-not $answerRoot) { throw 'private answer media was not found' }

    $driverRoot = $null
    foreach ($volume in Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=5') {
        $candidate = Join-Path $volume.DeviceID 'viosock\2k25\amd64'
        if (Test-Path -LiteralPath $candidate) { $driverRoot = $candidate; break }
    }
    if (-not $driverRoot) { throw 'pinned VirtIO VSOCK driver directory was not found' }
    $driverFiles = @(Get-ChildItem -LiteralPath $driverRoot -Filter '*.inf' -File)
    if ($driverFiles.Count -eq 0) { throw 'VirtIO VSOCK package contains no INF files' }
    foreach ($driver in $driverFiles) {
        & pnputil.exe /add-driver $driver.FullName /install
        if ($LASTEXITCODE -ne 0) { throw "pnputil failed with exit code $LASTEXITCODE" }
    }

    $installRoot = 'C:\waddle'
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $answerRoot 'waddle-guest-exec.exe') `
        -Destination (Join-Path $installRoot 'waddle-guest-exec.exe') -Force
    $runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name 'WaddleGuestExec' -Type String `
        -Value '"C:\waddle\waddle-guest-exec.exe" --vsock-port 5242'
    'driver_installed_and_guest_exec_staged' | Set-Content -LiteralPath $log -Encoding ASCII
    exit 0
} catch {
    $_.Exception.Message | Set-Content -LiteralPath $log -Encoding ASCII
    exit 31
}
'''
    return script.encode("utf-8")


def _write_private(path: Path, data: bytes) -> None:
    """Create one private file without following or overwriting an existing path."""
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb", closefd=False) as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    finally:
        os.close(descriptor)
    os.chmod(path, 0o600)


def _new_test_password() -> str:
    """Generate a strong password with mixed character classes for Windows policy."""
    alphabet = string.ascii_letters + string.digits + "!#$%&()*+,-./:;=?@[]^_{|}~"
    required = [secrets.choice(string.ascii_uppercase), secrets.choice(string.ascii_lowercase),
                secrets.choice(string.digits), secrets.choice("!#$%&()*+,-./:;=?@[]^_{|}~")]
    required.extend(secrets.choice(alphabet) for _ in range(28))
    secrets.SystemRandom().shuffle(required)
    return "".join(required)


def _run_command(argv: Sequence[str], *, timeout: int, log_path: Path,
                 run: Callable[..., Any]) -> None:
    """Run a bounded tool with output discarded and write a nonsecret status log."""
    try:
        completed = run(list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, timeout=timeout, check=False,
                        close_fds=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        _write_private(log_path, (f"preparation command failed: {type(error).__name__}\n").encode())
        raise fresh_guest_error_t(f"preparation command failed: {type(error).__name__}") from error
    _write_private(log_path, f"preparation command exit status: {completed.returncode}\n".encode())
    if completed.returncode != 0:
        raise fresh_guest_error_t(f"preparation command exited with status {completed.returncode}")


def _secret_variants(secret: str) -> tuple[str, ...]:
    """Return raw and XML-escaped spellings that must never enter a manifest."""
    return (secret, html.escape(secret, quote=False), html.escape(secret, quote=True))


def _qemu_argv(run_root: Path, qemu: Path, *, guest_cid: int, vhost_fd: int | str,
               ovmf_code: Path, install_iso: Path, virtio_iso: Path,
               memory_mib: int, cpus: int) -> list[str]:
    """Build the exact isolated install argv, without launching the VM."""
    _validate_cid(guest_cid)
    if isinstance(vhost_fd, bool) or not isinstance(vhost_fd, (int, str)):
        raise fresh_guest_error_t("VSOCK descriptor must be an integer or manifest placeholder")
    if isinstance(vhost_fd, int) and vhost_fd < 0:
        raise fresh_guest_error_t("VSOCK descriptor must be nonnegative")
    _validate_resources(memory_mib, cpus, PreparationTimeoutSeconds)
    if isinstance(vhost_fd, str) and vhost_fd != "CALLER_VALIDATED_VHOST_VSOCK_FD":
        raise fresh_guest_error_t("VSOCK descriptor placeholder is invalid")
    for path in (run_root, ovmf_code, install_iso, virtio_iso):
        path_text = str(path)
        if "," in path_text or any(ord(character) < 32 for character in path_text):
            raise fresh_guest_error_t("QEMU keyval paths cannot contain comma or control bytes")
    return [
        str(qemu), "-name", "waddle-fresh-native-test", "-machine", "q35,accel=kvm",
        "-cpu", "host", "-smp", str(cpus), "-m", str(memory_mib), "-nodefaults", "-no-user-config",
        "-display", "none", "-monitor", "none", "-serial", "none", "-nic", "none",
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={run_root / 'vars.fd'}",
        "-device", "ich9-ahci,id=sata",
        "-drive", f"if=none,id=windows,format=qcow2,file={run_root / 'windows.qcow2'}",
        "-device", "ide-hd,drive=windows,bus=sata.0",
        "-drive", f"if=none,id=installer,media=cdrom,readonly=on,format=raw,file={install_iso}",
        "-device", "ide-cd,drive=installer,bus=sata.1",
        "-drive", f"if=none,id=virtio,media=cdrom,readonly=on,format=raw,file={virtio_iso}",
        "-device", "ide-cd,drive=virtio,bus=sata.2",
        "-drive", f"if=none,id=answer,media=cdrom,readonly=on,format=raw,file={run_root / 'answer.iso'}",
        "-device", "ide-cd,drive=answer,bus=sata.3", "-boot", "once=d,order=c,menu=on",
        "-device", "VGA", "-chardev", f"socket,id=chrtpm,path={run_root / 'tpm.sock'}",
        "-tpmdev", "emulator,id=tpm0,chardev=chrtpm", "-device", "tpm-tis,tpmdev=tpm0",
        "-qmp", f"unix:{run_root / 'qmp.sock'},server=on,wait=off",
        "-D", str(run_root / "qemu.log"), "-device",
        f"vhost-vsock-pci,guest-cid={guest_cid},vhostfd={vhost_fd}",
    ]


def _swtpm_argv(run_root: Path, swtpm: str) -> list[str]:
    """Build the exact private TPM command for a later supervised launcher."""
    return [swtpm, "socket", "--tpm2", "--tpmstate", f"dir={run_root / 'tpm'}",
            "--ctrl", f"type=unixio,path={run_root / 'tpm.sock'}"]


def prepare_fresh_guest(config: fresh_guest_config_t = fresh_guest_config_t(), *,
                        run: Callable[..., Any] = subprocess.run,
                        password_factory: Callable[[], str] | None = None,
                        administrator_password_factory: Callable[[], str] | None = None,
                        qemu_validator: Callable[[Path, Path], dict[str, Any]] | None = None,
                        clock: Callable[[], float] = time.time) -> dict[str, Any]:
    """Create a new private blank guest and unattended answer media.

    This function does not start QEMU, swtpm, or any device operation. It
    returns a non-secret manifest and retains any partial run for recovery.
    """
    validated = _validate_config(config, qemu_validator=qemu_validator)

    run_parent = config.run_parent
    try:
        run_parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    except OSError as error:
        raise fresh_guest_error_t(f"cannot create private fresh guest parent: {error}") from error
    if run_parent.is_symlink() or stat.S_IMODE(run_parent.stat().st_mode) != 0o700:
        raise fresh_guest_error_t("fresh guest run parent must be a nonsymlink mode-0700 directory")
    run_root = run_parent / f"fresh-{uuid.uuid4().hex}"
    try:
        run_root.mkdir(mode=0o700)
    except OSError as error:
        raise fresh_guest_error_t(f"cannot create private fresh guest run: {error}") from error
    os.chmod(run_root, 0o700)
    manifest: dict[str, Any] = {
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(clock())),
        "run_root": str(run_root),
        "guest_cid": config.guest_cid,
        "selected_image_name": config.image_name,
        "guest_user": "waddle_native",
        "guest_user_group": "Users",
        "guest_password_in_manifest": False,
        "guest_password_in_logs": False,
        "guest_password_in_argv": False,
        "administrator_password_in_manifest": False,
        "administrator_password_in_logs": False,
        "administrator_password_in_argv": False,
        "installer": validated["install_iso"],
        "virtio": validated["virtio_iso"],
        "guest_exec": validated["guest_exec"],
        "ovmf_code": validated["ovmf_code"],
        "ovmf_vars_template": validated["ovmf_vars_template"],
        "qemu": validated["qemu"],
        "host_tools": validated["tools"],
        "virtual_disk": {"path": str(run_root / "windows.qcow2"),
                          "virtual_size_bytes": GuestDiskBytes, "format": "qcow2",
                          "backing_file": None},
        "answer_xml_sha256_recorded": False,
        "answer_iso_sha256_recorded": False,
        "guest_booted": False,
        "guest_vsock_listener_verified": False,
        "native_acceptance_credit": False,
        "qemu_argv_template": None,
        "swtpm_argv": None,
        "failure": None,
    }
    user_password = ""
    administrator_password = ""
    try:
        user_password = (password_factory or _new_test_password)()
        administrator_password = (administrator_password_factory or _new_test_password)()
        for password in (user_password, administrator_password):
            if not isinstance(password, str) or len(password) < 20 or any(
                    ch in password for ch in "\r\n\x00"):
                raise fresh_guest_error_t("generated account password does not meet local format bounds")
        if user_password == administrator_password:
            raise fresh_guest_error_t("generated account passwords must be distinct")
        # 0600 files are created exclusively under the new private 0700 run root.
        _write_private(run_root / "test-account.secret", (user_password + "\n").encode("utf-8"))
        _write_private(run_root / "administrator.secret",
                       (administrator_password + "\n").encode("utf-8"))
        answer_xml = _answer_xml(user_password, administrator_password, config.image_name)
        _write_private(run_root / "Autounattend.xml", answer_xml)
        shutil.copyfile(validated["ovmf_vars_template"]["path"], run_root / "vars.fd")
        os.chmod(run_root / "vars.fd", 0o600)
        (run_root / "tpm").mkdir(mode=0o700)
        os.chmod(run_root / "tpm", 0o700)

        stage = run_root / "answer-stage"
        stage.mkdir(mode=0o700)
        _write_private(stage / "Autounattend.xml", answer_xml)
        _write_private(stage / "waddle-guest-setup.ps1", _guest_setup_script())
        staged_guest_exec = stage / "waddle-guest-exec.exe"
        shutil.copyfile(validated["guest_exec"]["path"], staged_guest_exec)
        if _sha256_file(staged_guest_exec) != validated["guest_exec"]["sha256"]:
            raise fresh_guest_error_t("staged guest-exec does not match the validated build hash")
        os.chmod(staged_guest_exec, 0o600)
        qemu_img = validated["tools"]["qemu_img"]
        _run_command([qemu_img, "create", "-f", "qcow2", str(run_root / "windows.qcow2"),
                      str(GuestDiskBytes)], timeout=config.preparation_timeout_seconds,
                     log_path=run_root / "qemu-img.log", run=run)
        disk_path = run_root / "windows.qcow2"
        if not disk_path.is_file() or disk_path.is_symlink():
            raise fresh_guest_error_t("qemu-img did not create the new private disk")
        os.chmod(disk_path, 0o600)
        answer_iso = run_root / "answer.iso"
        xorriso = validated["tools"]["xorriso"]
        # Windows ignores Rock Ridge names. Joliet preserves the hyphens in
        # bootstrap/binary paths instead of ISO9660 underscore substitution.
        _run_command([xorriso, "-as", "mkisofs", "-iso-level", "3", "-J", "-V", "WADDLEANS",
                      "-o", str(answer_iso), str(stage)],
                     timeout=config.preparation_timeout_seconds,
                     log_path=run_root / "xorriso.log", run=run)
        if not answer_iso.is_file() or answer_iso.is_symlink():
            raise fresh_guest_error_t("xorriso did not create the private answer ISO")
        os.chmod(answer_iso, 0o600)
        shutil.rmtree(stage)

        qemu_path = Path(validated["qemu_path"])
        qemu_args = _qemu_argv(run_root, qemu_path, guest_cid=config.guest_cid,
                               vhost_fd="CALLER_VALIDATED_VHOST_VSOCK_FD",
                               ovmf_code=Path(validated["ovmf_code"]["path"]),
                               install_iso=Path(validated["install_iso"]["path"]),
                               virtio_iso=Path(validated["virtio_iso"]["path"]),
                               memory_mib=config.memory_mib, cpus=config.cpus)
        manifest["qemu_argv_template"] = qemu_args
        manifest["swtpm_argv"] = _swtpm_argv(run_root, validated["tools"]["swtpm"])
        manifest["private_files"] = {
            "disk_mode": oct(stat.S_IMODE((run_root / "windows.qcow2").stat().st_mode)),
            "answer_iso_mode": oct(stat.S_IMODE(answer_iso.stat().st_mode)),
            "answer_xml_mode": oct(stat.S_IMODE((run_root / "Autounattend.xml").stat().st_mode)),
            "test_secret_mode": oct(stat.S_IMODE((run_root / "test-account.secret").stat().st_mode)),
            "administrator_secret_mode": oct(
                stat.S_IMODE((run_root / "administrator.secret").stat().st_mode)),
            "tpm_directory_mode": oct(stat.S_IMODE((run_root / "tpm").stat().st_mode)),
        }
        _write_manifest(run_root, manifest, (user_password, administrator_password))
        return manifest
    except BaseException as error:
        manifest["failure"] = {"type": type(error).__name__,
                               "message": "details suppressed", "retained": True}
        try:
            _write_manifest(run_root, manifest, (user_password, administrator_password))
        except OSError:
            pass
        raise fresh_guest_error_t(
            f"fresh guest preparation failed: {type(error).__name__}; details suppressed; "
            f"recoverable run retained at {run_root}") from None


def _write_manifest(run_root: Path, manifest: dict[str, Any],
                    secrets_to_exclude: Sequence[str]) -> None:
    """Write a secret-free private JSON receipt atomically."""
    text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    if any(variant in text for secret in secrets_to_exclude if secret
           for variant in _secret_variants(secret)):
        raise fresh_guest_error_t("manifest redaction invariant failed")
    temporary = run_root / ".run-manifest.tmp"
    if temporary.exists() or temporary.is_symlink():
        temporary.unlink()
    _write_private(temporary, text.encode("utf-8"))
    os.replace(temporary, run_root / "run-manifest.json")
    os.chmod(run_root / "run-manifest.json", 0o600)


def main(argv: Sequence[str] | None = None) -> int:
    """Prepare private install artifacts and print only their non-secret location."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prepare", choices=("prepare",))
    parser.add_argument("--guest-cid", type=int, default=52942)
    parser.add_argument("--install-iso", type=Path, default=DefaultInstallIso)
    parser.add_argument("--virtio-iso", type=Path, default=DefaultVirtioIso)
    args = parser.parse_args(argv)
    try:
        config = dataclasses.replace(fresh_guest_config_t(), guest_cid=args.guest_cid,
                                     install_iso=args.install_iso, virtio_iso=args.virtio_iso)
        manifest = prepare_fresh_guest(config)
        print(json.dumps({"run_root": manifest["run_root"], "manifest":
                          str(Path(manifest["run_root"]) / "run-manifest.json"),
                          "guest_booted": False, "native_acceptance_credit": False}, indent=2))
        return 0
    except fresh_guest_error_t:
        # The exception may originate in a tool or filesystem diagnostic.
        print("fresh guest preparation refused; details suppressed", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
