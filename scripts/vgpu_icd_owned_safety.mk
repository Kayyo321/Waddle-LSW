# Genuine owned Zig ICD and embedded-import access instrumentation, separate from public ICD artifacts.
# Read-only native/pinned-oracle dependencies; the Python tool owns only the
# exclusive build/icd_closure_safety directory. No suppression or fault variants.
# UBSan applies to the strict C frontend; Zig Debug runtime checks remain enabled.
IcdOwnedSafetyObjects = build/venus_capabilities.o build/venus_command.o build/venus_objects.o build/venus_instance_wire.o build/venus_query_wire.o build/venus_values.o build/venus_values_oracle.o
IcdOwnedSafetyOracles = build/venus_features_query_oracle.o build/venus_features_reply_oracle.o build/venus_render_wire_oracle.o build/venus_descriptor_wire_oracle.o build/venus_compute_wire_oracle.o build/venus_graphics_wire_oracle.o build/venus_graphics_pipeline_wire_oracle.o build/venus_graphics_command_wire_oracle.o

IcdOwnedSafetySources = src/vgpu/venus_icd.zig src/vgpu/venus_features_native.zig src/vgpu/venus_features_wire.zig src/vgpu/venus_descriptor_wire.zig src/vgpu/venus_icd_profiles.zig src/vgpu/venus_compute_state.zig src/vgpu/venus_compute_wire.zig src/vgpu/venus_render_wire.zig src/vgpu/venus_graphics_wire.zig src/vgpu/venus_graphics_state.zig src/vgpu/venus_graphics_pipeline_wire.zig src/vgpu/venus_graphics_command_wire.zig

.PHONY: vgpu-icd-owned-sanitizers
# Synchronous exclusive artifact owner. All selected production definitions must
# be accounted for and the entire LLVM module must pass exact reverse proof.
vgpu-icd-owned-sanitizers: scripts/icd_owned_sanitizers.py $(IcdOwnedSafetySources) $(VgpuIcdHeaders) $(IcdOwnedSafetyObjects) $(IcdOwnedSafetyOracles)
	python3 scripts/icd_owned_sanitizers.py src/vgpu/venus_icd.zig build/icd_closure_safety

# Separately linked codec access instrumentation; private native/allocator suites.
.PHONY: vgpu-capabilities-owned-sanitizers
vgpu-capabilities-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_capabilities.zig tests/vgpu/capabilities.c include/waddle/venus_capabilities.h include/waddle/venus_ring.h
	python3 scripts/icd_dependency_sanitizers.py capabilities build/icd_dependency_safety/capabilities

.PHONY: vgpu-command-owned-sanitizers
vgpu-command-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_command.zig tests/vgpu/command.c include/waddle/venus_command.h include/waddle/venus_request.h include/waddle/venus_ring.h
	python3 scripts/icd_dependency_sanitizers.py command build/icd_dependency_safety/command

.PHONY: vgpu-objects-owned-sanitizers
vgpu-objects-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_objects.zig tests/vgpu/objects.c include/waddle/venus_objects.h submodules/venus_protocol/include/vulkan/vk_icd.h
	python3 scripts/icd_dependency_sanitizers.py objects build/icd_dependency_safety/objects

.PHONY: vgpu-instance_wire-owned-sanitizers
vgpu-instance_wire-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_instance_wire.zig tests/vgpu/instance_wire.c include/waddle/venus_instance_wire.h
	python3 scripts/icd_dependency_sanitizers.py instance_wire build/icd_dependency_safety/instance_wire

.PHONY: vgpu-query_wire-owned-sanitizers
vgpu-query_wire-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_query_wire.zig tests/vgpu/query_wire.c include/waddle/venus_query_wire.h
	python3 scripts/icd_dependency_sanitizers.py query_wire build/icd_dependency_safety/query_wire
