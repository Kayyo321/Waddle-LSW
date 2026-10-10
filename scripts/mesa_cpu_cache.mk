# Mesa CPU affinity storage ownership repair. All constants refer to private
# ignored build outputs; the pinned vendor checkout is never modified.
MesaCpuCommit = 742a20f48c59e8649533c84c4d49dd95b403f5da
MesaCpuSource = build/vendor/mesa_cpu_cache_dual_source
MesaCpuBuild = build/vendor/mesa_cpu_cache_dual_owned
MesaCpuLibrary = $(MesaCpuBuild)/src/amd/vulkan/libvulkan_radeon.so
MesaCpuLavapipeLibrary = $(MesaCpuBuild)/src/gallium/targets/lavapipe/libvulkan_lvp.so
MesaCpuLibraries = $(MesaCpuLibrary) $(MesaCpuLavapipeLibrary)
MesaCpuManifest = $(MesaCpuBuild)/radeon_icd.json
MesaCpuLavapipeManifest = $(MesaCpuBuild)/lvp_icd.json
MesaCpuLlvmConfig = /usr/bin/llvm-config-19
MesaCpuOptions = --wrap-mode=nofallback -Dbuildtype=release -Dvulkan-drivers=amd,swrast -Dgallium-drivers=llvmpipe -Dplatforms=[] -Dllvm=enabled -Dshared-llvm=enabled -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs=[] -Dtools=[] -Dbuild-tests=false

.PHONY: mesa-cpu-cache mesa-cpu-cache-pin
# Fail closed on changed tracked sources or a dependency pin mismatch. Git owns
# transient descriptors; no application allocations or retained native handles.
mesa-cpu-cache-pin:
	test "$$(git -C submodules/mesa rev-parse HEAD)" = "$(MesaCpuCommit)"
	git -C submodules/mesa diff --quiet
	git -C submodules/mesa diff --cached --quiet

$(MesaCpuSource)/.patched: scripts/patches/mesa_l3_affinity.patch scripts/patches/mesa_lavapipe_visibility.patch submodules/mesa/src/util/u_cpu_detect.c | build/vendor mesa-cpu-cache-pin
	rm -rf $(MesaCpuSource)
	mkdir -p $(MesaCpuSource)
	git -C submodules/mesa archive --output="$(CURDIR)/$(MesaCpuSource)/.source_archive.tar" $(MesaCpuCommit)
	tar -xf $(MesaCpuSource)/.source_archive.tar -C $(MesaCpuSource)
	rm $(MesaCpuSource)/.source_archive.tar
	patch -d $(MesaCpuSource) -p1 --forward < scripts/patches/mesa_l3_affinity.patch
	patch -d $(MesaCpuSource) -p1 --forward < scripts/patches/mesa_lavapipe_visibility.patch
	touch $@

$(MesaCpuBuild)/build.ninja: $(MesaCpuSource)/.patched scripts/mesa_cpu_cache.mk
	$(MesaCpuLlvmConfig) --version
	sha256sum scripts/patches/mesa_l3_affinity.patch scripts/patches/mesa_lavapipe_visibility.patch
	LLVM_CONFIG=$(MesaCpuLlvmConfig) MESA_GIT_SHA1_OVERRIDE=$(MesaCpuCommit) meson setup $(MesaCpuBuild) $(MesaCpuSource) $(MesaCpuOptions) $$(test ! -f $@ || printf '%s' --reconfigure)

# Grouped targets give one Ninja writer even under parallel make. Both drivers
# share the same source, compiler, options, CPU allocator and export policy.
$(MesaCpuLibraries) &: $(MesaCpuBuild)/build.ninja $(MesaCpuSource)/.patched
	MESA_GIT_SHA1_OVERRIDE=$(MesaCpuCommit) ninja -C $(MesaCpuBuild) -j4 src/amd/vulkan/libvulkan_radeon.so src/gallium/targets/lavapipe/libvulkan_lvp.so

# Owned manifest contains the absolute private build library, so loading cannot
# silently resolve the system driver. Python owns temporary JSON strings only.
$(MesaCpuManifest): $(MesaCpuLibrary) scripts/mesa_cpu_cache.mk
	python3 -c 'import json,pathlib,sys; pathlib.Path(sys.argv[2]).write_text(json.dumps({"file_format_version":"1.0.0","ICD":{"library_path":str(pathlib.Path(sys.argv[1]).resolve()),"api_version":"1.3.0"}},indent=2)+"\n")' "$(MesaCpuLibrary)" "$@"

$(MesaCpuLavapipeManifest): $(MesaCpuLavapipeLibrary) scripts/mesa_cpu_cache.mk
	python3 -c 'import json,pathlib,sys; pathlib.Path(sys.argv[2]).write_text(json.dumps({"file_format_version":"1.0.0","ICD":{"library_path":str(pathlib.Path(sys.argv[1]).resolve()),"api_version":"1.3.0"}},indent=2)+"\n")' "$(MesaCpuLavapipeLibrary)" "$@"

mesa-cpu-cache: mesa-cpu-cache-pin $(MesaCpuManifest) $(MesaCpuLavapipeManifest)

build/vgpu_mesa_cpu_cache_test: tests/vgpu/mesa_cpu_cache.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -ldl -o $@

.PHONY: mesa-cpu-cache-test mesa-cpu-cache-sanitizers
mesa-cpu-cache-test: mesa-cpu-cache build/vgpu_mesa_cpu_cache_test
	VK_DRIVER_FILES="$(CURDIR)/$(MesaCpuManifest)" ./build/vgpu_mesa_cpu_cache_test "$(CURDIR)/$(MesaCpuLibrary)"
	VK_DRIVER_FILES="$(CURDIR)/$(MesaCpuLavapipeManifest)" ./build/vgpu_mesa_cpu_cache_test "$(CURDIR)/$(MesaCpuLavapipeLibrary)" --require-device

# Real driver allocations are intercepted by the instrumented C fixture. The
# vendor C/C++ library keeps ordinary native build flags and remains unloadable.
mesa-cpu-cache-sanitizers: mesa-cpu-cache
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,leak,undefined -fno-omit-frame-pointer tests/vgpu/mesa_cpu_cache.c -ldl -o build/vgpu_mesa_cpu_cache_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 VK_DRIVER_FILES="$(CURDIR)/$(MesaCpuManifest)" ./build/vgpu_mesa_cpu_cache_sanitized "$(CURDIR)/$(MesaCpuLibrary)"
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 VK_DRIVER_FILES="$(CURDIR)/$(MesaCpuLavapipeManifest)" ./build/vgpu_mesa_cpu_cache_sanitized "$(CURDIR)/$(MesaCpuLavapipeLibrary)" --require-device
