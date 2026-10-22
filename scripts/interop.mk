# Seamless-interop targets (impl/seamless-interop). Host dependencies come from
# base system packages; Wayland protocol XML is pinned by TODO #7.

.PHONY: interop-test
# Hardware-free gates for the seamless-interop feature.
interop-test:
	bash tests/interop/verify_m0.sh
