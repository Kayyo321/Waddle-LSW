# Manifest lane (TODO #33a). The rooted engine's unit, checkpoint, syscall, and
# allocation tests live in src/interop/manifest_store.zig, which
# interop-manifest-store-test already runs. This fragment adds the public API
# integration suite for status, removal, and migration.
InteropLaneTests += interop-manifest-commands-test

.PHONY: interop-manifest-commands-test
interop-manifest-commands-test:
	$(ZIG) test -lc --dep manifest_store -Mroot=tests/interop/manifest_store_commands.zig -Mmanifest_store=src/interop/manifest_store.zig
