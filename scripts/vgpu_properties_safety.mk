# Private standalone Properties2 safety gate. Root owns aggregate/CI integration.
# Exact native/wire/render closure and three fresh oracles; generated headers are
# read-only inputs. The runner retains a unique UTC/PID attempt, including failures.
.PHONY: vgpu-properties-owned-sanitizers
vgpu-properties-owned-sanitizers:
	python3 -B scripts/icd_properties_sanitizers.py build/properties_owned_safety
