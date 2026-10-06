# Mesa CPU affinity storage ownership repair. All constants refer to private
# ignored build outputs; the pinned vendor checkout is never modified.
MesaCpuCommit = 742a20f48c59e8649533c84c4d49dd95b403f5da
MesaCpuSource = build/vendor/mesa_cpu_cache_source
MesaCpuBuild = build/vendor/mesa_cpu_cache
MesaCpuLibrary = $(MesaCpuBuild)/src/amd/vulkan/libvulkan_radeon.so
MesaCpuManifest = $(MesaCpuBuild)/radeon_icd.json
MesaCpuOptions = --wrap-mode=nofallback -Dbuildtype=release -Dvulkan-drivers=amd -Dgallium-drivers=[] -Dplatforms=[] -Dllvm=disabled -Damd-use-llvm=false -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dvideo-codecs=[] -Dtools=[] -Dbuild-tests=false

.PHONY: mesa-cpu-cache mesa-cpu-cache-pin
# Fail closed on changed tracked sources or a dependency pin mismatch. Git owns
# transient descriptors; no application allocations or retained native handles.
mesa-cpu-cache-pin:
	test "$$(git -C submodules/mesa rev-parse HEAD)" = "$(MesaCpuCommit)"
	git -C submodules/mesa diff --quiet
	git -C submodules/mesa diff --cached --quiet

$(MesaCpuSource)/.patched: scripts/patches/mesa_l3_affinity.patch submodules/mesa/src/util/u_cpu_detect.c | build/vendor mesa-cpu-cache-pin
	rm -rf $(MesaCpuSource)
	mkdir -p $(MesaCpuSource)
	git -C submodules/mesa archive --output="$(CURDIR)/$(MesaCpuSource)/.source_archive.tar" $(MesaCpuCommit)
	tar -xf $(MesaCpuSource)/.source_archive.tar -C $(MesaCpuSource)
	rm $(MesaCpuSource)/.source_archive.tar
	patch -d $(MesaCpuSource) -p1 --forward < scripts/patches/mesa_l3_affinity.patch
	touch $@

$(MesaCpuBuild)/build.ninja: $(MesaCpuSource)/.patched scripts/mesa_cpu_cache.mk
	MESA_GIT_SHA1_OVERRIDE=$(MesaCpuCommit) meson setup $(MesaCpuBuild) $(MesaCpuSource) $(MesaCpuOptions) $$(test ! -f $@ || printf '%s' --reconfigure)

$(MesaCpuLibrary): $(MesaCpuBuild)/build.ninja $(MesaCpuSource)/.patched
	MESA_GIT_SHA1_OVERRIDE=$(MesaCpuCommit) ninja -C $(MesaCpuBuild) -j4 src/amd/vulkan/libvulkan_radeon.so

# Owned manifest contains the absolute private build library, so loading cannot
# silently resolve the system driver. Python owns temporary JSON strings only.
$(MesaCpuManifest): $(MesaCpuLibrary) scripts/mesa_cpu_cache.mk
	python3 -c 'import json,pathlib,sys; pathlib.Path(sys.argv[2]).write_text(json.dumps({"file_format_version":"1.0.0","ICD":{"library_path":str(pathlib.Path(sys.argv[1]).resolve()),"api_version":"1.3.0"}},indent=2)+"\n")' "$(MesaCpuLibrary)" "$@"

mesa-cpu-cache: mesa-cpu-cache-pin $(MesaCpuManifest)
