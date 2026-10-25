# Manifest lane (TODO #33a, #33 integration commands). The rooted engine's unit,
# checkpoint, syscall, and allocation tests live in src/interop/manifest_store.zig,
# which interop-manifest-store-test already runs. This fragment adds the public API
# suite, the integration producer and CLI unit suites, and the end-to-end run of
# build/waddle against a temporary HOME and XDG layout.
InteropLaneTests += interop-manifest-commands-test interop-integration-unit-test interop-integration-e2e-test

.PHONY: interop-manifest-commands-test interop-integration-unit-test interop-integration-e2e-test
interop-manifest-commands-test:
	$(ZIG) test -lc --dep manifest_store -Mroot=tests/interop/manifest_store_commands.zig -Mmanifest_store=src/interop/manifest_store.zig

interop-integration-unit-test:
	$(ZIG) test -lc --dep manifest_store -Mintegration_producers=src/interop/integration_producers.zig -Mmanifest_store=src/interop/manifest_store.zig
	$(ZIG) test -lc --dep mimeapps -Mroot=src/cli/integration_mime.zig -Mmimeapps=src/interop/mimeapps.zig
	$(ZIG) test -lc --dep manifest_store --dep integration_producers --dep mimeapps -Mroot=src/cli/integration_commands.zig -Mmanifest_store=src/interop/manifest_store.zig --dep manifest_store -Mintegration_producers=src/interop/integration_producers.zig -Mmimeapps=src/interop/mimeapps.zig

interop-integration-e2e-test: build/waddle
	python3 -I tests/interop/integration_commands_e2e.py
