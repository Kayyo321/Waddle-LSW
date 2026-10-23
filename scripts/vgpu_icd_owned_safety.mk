# Genuine owned Zig ICD and embedded-import access instrumentation, separate from public ICD artifacts.
# Read-only native/pinned-oracle dependencies; the Python tool owns only the
# exclusive new37 run directory under build/icd_closure_safety_37. No suppression or fault variants.
# UBSan applies to the strict C frontend; Zig Debug runtime checks remain enabled.
IcdOwnedSafetyObjects = build/venus_capabilities.o build/venus_command.o build/venus_objects.o build/venus_instance_wire.o build/venus_query_wire.o build/venus_values.o build/venus_values_oracle.o
IcdOwnedSafetyOracles = build/venus_features_query_oracle.o build/venus_features_reply_oracle.o build/venus_render_wire_oracle.o build/venus_descriptor_wire_oracle.o build/venus_compute_wire_oracle.o build/venus_graphics_wire_oracle.o build/venus_graphics_pipeline_wire_oracle.o build/venus_graphics_command_wire_oracle.o build/venus_device_wire_oracle.o

IcdOwnedSafetySources = src/vgpu/venus_compute_state.zig src/vgpu/venus_compute_wire.zig src/vgpu/venus_descriptor_template_native.zig src/vgpu/venus_descriptor_wire.zig src/vgpu/venus_device_native.zig src/vgpu/venus_device_wire.zig src/vgpu/venus_dynamic_rendering_wire.zig src/vgpu/venus_extensions_wire.zig src/vgpu/venus_extra_objects_wire.zig src/vgpu/venus_features_native.zig src/vgpu/venus_features_wire.zig src/vgpu/venus_graphics_command_wire.zig src/vgpu/venus_graphics_dynamic_wire.zig src/vgpu/venus_graphics_general_wire.zig src/vgpu/venus_graphics_state.zig src/vgpu/venus_graphics_wire.zig src/vgpu/venus_icd.zig src/vgpu/venus_icd_profiles.zig src/vgpu/venus_image_transfer_native.zig src/vgpu/venus_image_transfer_wire.zig src/vgpu/venus_image_view_native.zig src/vgpu/venus_modern_sync_wire.zig src/vgpu/venus_pipeline_wire_helpers.zig src/vgpu/venus_properties_native.zig src/vgpu/venus_properties_wire.zig src/vgpu/venus_render_wire.zig src/vgpu/venus_requirements2_wire.zig src/vgpu/venus_sampler_descriptor_wire.zig src/vgpu/venus_shader_wire.zig src/vgpu/venus_transfer2_native.zig src/vgpu/venus_wsi.zig

.PHONY: vgpu-icd-owned-sanitizers
# Synchronous exclusive artifact owner. All selected production definitions must
# be accounted for and the entire LLVM module must pass exact reverse proof.
vgpu-icd-owned-sanitizers: vgpu-owned-sanitizer-inventory-test scripts/icd_owned_sanitizers.py $(IcdOwnedSafetySources) $(VgpuIcdHeaders) $(IcdOwnedSafetyObjects) $(IcdOwnedSafetyOracles)
	python3 scripts/icd_owned_sanitizers.py src/vgpu/venus_icd.zig build/icd_closure_safety_37

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

.PHONY: vgpu-values-owned-sanitizers
vgpu-values-owned-sanitizers: scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py src/vgpu/venus_values.zig tests/vgpu/values.c include/waddle/venus_values.h
	python3 scripts/icd_dependency_sanitizers.py values build/icd_dependency_safety/values

# Each phony gate owns a distinct private output directory; generated headers
# are required by read-only discovery and are never make prerequisites here.
.PHONY: vgpu-icd-dependency-sanitizers
vgpu-icd-dependency-sanitizers: vgpu-capabilities-owned-sanitizers vgpu-command-owned-sanitizers vgpu-objects-owned-sanitizers vgpu-instance_wire-owned-sanitizers vgpu-query_wire-owned-sanitizers vgpu-values-owned-sanitizers

# Full private owned seam: nested codec rebuilds, fresh C oracles and final-binary
# access proof. Header discovery is read-only; no generated/shared prerequisite.
.PHONY: vgpu-icd-complete-seam-sanitizers
vgpu-icd-complete-seam-sanitizers: vgpu-owned-sanitizer-inventory-test scripts/icd_seam_sanitizers.py scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py
	python3 scripts/icd_seam_sanitizers.py build/icd_complete_seam_safety_37 --expected-icd-units 415 --latest-record build/icd_complete_seam_safety_37/latest.json

# Serial isolated GPU matrix; every invocation retains a unique artifact receipt.
# Frozen production runtime and all eight driver manifests remain read-only.
.PHONY: vgpu-icd-physical-owned-sanitizers
vgpu-icd-physical-owned-sanitizers: vgpu-icd-complete-seam-sanitizers scripts/icd_physical_sanitizers.py scripts/icd_seam_sanitizers.py scripts/icd_dependency_sanitizers.py scripts/icd_owned_sanitizers.py
	python3 scripts/icd_physical_sanitizers.py build/icd_physical_owned_safety_37 --seam-record build/icd_complete_seam_safety_37/latest.json --expected-device "NVIDIA GeForce RTX 5080"

# Current ICD profile exposes mapping only for actual host-coherent visible types.
# The legacy noncoherent physical target above remains a separate strict contract.
.PHONY: vgpu-icd-physical-coherent-owned-sanitizers
vgpu-icd-physical-coherent-owned-sanitizers: vgpu-icd-complete-seam-sanitizers scripts/icd_physical_sanitizers.py
	python3 scripts/icd_physical_sanitizers.py build/icd_physical_coherent_owned_safety_37 --seam-record build/icd_complete_seam_safety_37/latest.json --memory-profile coherent --expected-device "NVIDIA GeForce RTX 5080"
