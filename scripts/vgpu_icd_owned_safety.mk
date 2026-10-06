# Genuine owned Zig ICD and embedded-import access instrumentation, separate from public ICD artifacts.
# Read-only native/pinned-oracle dependencies; the Python tool owns only the
# exclusive build/icd_closure_safety directory. No suppression or fault variants.
# UBSan applies to the strict C frontend; Zig Debug runtime checks remain enabled.
IcdOwnedSafetyObjects = build/venus_capabilities.o build/venus_command.o build/venus_objects.o build/venus_instance_wire.o build/venus_query_wire.o build/venus_values.o build/venus_values_oracle.o
IcdOwnedSafetyOracles = build/venus_render_wire_oracle.o build/venus_descriptor_wire_oracle.o build/venus_compute_wire_oracle.o build/venus_graphics_wire_oracle.o build/venus_graphics_pipeline_wire_oracle.o build/venus_graphics_command_wire_oracle.o

IcdOwnedSafetySources = src/vgpu/venus_icd.zig src/vgpu/venus_descriptor_wire.zig src/vgpu/venus_icd_profiles.zig src/vgpu/venus_compute_state.zig src/vgpu/venus_compute_wire.zig src/vgpu/venus_render_wire.zig src/vgpu/venus_graphics_wire.zig src/vgpu/venus_graphics_state.zig src/vgpu/venus_graphics_pipeline_wire.zig src/vgpu/venus_graphics_command_wire.zig

.PHONY: vgpu-icd-owned-sanitizers
# Synchronous exclusive artifact owner. All selected production definitions must
# be accounted for and the entire LLVM module must pass exact reverse proof.
vgpu-icd-owned-sanitizers: scripts/icd_owned_sanitizers.py $(IcdOwnedSafetySources) $(VgpuIcdHeaders) $(IcdOwnedSafetyObjects) $(IcdOwnedSafetyOracles)
	python3 scripts/icd_owned_sanitizers.py src/vgpu/venus_icd.zig build/icd_closure_safety
