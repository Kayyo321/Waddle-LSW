"""Lease-owner wrapper revision 2; prepared for explicit user approval.

Require the exact Codex continuation owner and the exact Claude prior owner.
Delegate to the unchanged frozen validator after mapping only its owner field;
idle-state, identity-set, PID and start-tick checks remain mandatory. This module
never writes the lease, starts processes or changes the frozen TargetOwner.
Approval applies to using the wrapper, not merely importing/testing it offline.
"""
from __future__ import annotations
import hashlib, importlib.util, sys
from pathlib import Path

ContinuationOwner = 'Codex software-vgpu-slicing continuation (sole VM/GPU runner)'
PreviousOwner = 'Claude Opus 5.5 Fable-handoff continuation (sole VM/GPU runner)'
ExecutorPath = Path('/tmp/waddle_opencl_reload_native_plan_revision6/opencl_reload_execute.py')


def require(ok: bool, message: str) -> None:
    if not ok:
        raise RuntimeError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wrap_validator(executor):
    """Replace executor.validate_vm_lease with the owner-transfer wrapper (idempotent)."""
    original = executor.validate_vm_lease
    if getattr(original, '__lease_owner_wrapper__', False):
        return executor
    frozen_owner = executor.admission.TargetOwner

    def validate_vm_lease(lease, proc_root=Path('/proc'), expected_identity=None):
        require(isinstance(lease, dict) and lease.get('owner') == ContinuationOwner,
                'VM lease is not held by the exact continuation owner')
        require(lease.get('previous_owner') == PreviousOwner,
                'VM lease transfer does not record the exact Claude previous owner')
        mapped = dict(lease, owner=frozen_owner)
        require(original(mapped, proc_root=proc_root, expected_identity=expected_identity) is mapped,
                'original lease validator did not accept the mapped lease')
        return lease

    validate_vm_lease.__lease_owner_wrapper__ = True
    executor.validate_vm_lease = validate_vm_lease
    return executor


def install_on_runner(runner, executor_path: Path = ExecutorPath):
    """Wrap runner.load so every load of the frozen executor gets the owner wrapper."""
    original_load = runner.load
    resolved = Path(executor_path).resolve()

    def load(name, path):
        module = original_load(name, path)
        if Path(path).resolve() == resolved:
            wrap_validator(module)
        return module

    runner.load = load
    return runner


def load_runner(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, 'cannot import frozen runner: ' + str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module
