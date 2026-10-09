# Exact device sanitizer source inventory

## Scope and root cause

The standard Linux software-vGPU workflow runs `make vgpu-device-wire-test vgpu-device-wire-sanitizers vgpu-device-wire-coverage`, followed by the corresponding native-device aggregate. The device codec imports render serialization. Render serialization now imports `venus_pipeline_wire_helpers.zig` and `venus_image_view_native.zig`, but neither helper declared the fixture delimiter required by the strict owned-source inventory. The first aggregate stopped before instrumentation with `missing unique fixture boundary`. Its exact two-module allowlist and the native aggregate's exact three-module allowlist were also stale.

This repair includes the complete actual four-module wire closure and five-module native closure. It changes only source comments, exact sanitizer manifests, regression-test wiring, and this documentation. It does not alter production executable code, compiler options, runtime environment, source exclusions, coverage thresholds, sanitizer success criteria, or immutable-input checks. README and ICD source are outside scope.

## Architecture and source boundaries

`venus_device_native.zig` imports `venus_device_wire.zig`, which imports `venus_render_wire.zig`; render imports the two shared helpers. Each source inventory owns a synchronous snapshot of production declaration names, original line numbers, test lines, and SHA-256. The sanitizer adds access instrumentation only to definitions matched to those exact source locations, requires every inventoried runtime declaration to be emitted, preserves the complete original LLVM module through a reversible transformation, and proves owned ASan accesses in both the object and the final executable.

The pipeline helper contains five production functions and no local fixture or test declarations. Its single `// Test-only fixtures.` delimiter therefore belongs after the complete last production function, `encode_specialization`. All five names remain required: `address`, `elements`, `collect_chain`, `encode_chain`, and `encode_specialization`. Their instantiated generic definitions must still be emitted. Existing imported render tests execute compute-chain and specialization behavior.

The image-view helper has six production declarations: `format_class`, `compatible`, `aspects`, `snapshot`, `view_usage`, and `validate`. Its delimiter belongs immediately before `image_info`, followed by `view_info` and six tests. Those two private construction functions are used only in its tests. No production declaration moves across the delimiter. The existing render file's interleaved `image_fixture` exception remains exactly unchanged.

The wire manifest names device wire, render wire, image-view native, and pipeline helpers. The native manifest adds native device. Both the initial recursive source inventory and final executable module set are checked against these exact manifest names. Module counts remain exact four/five checks, not lower bounds; new imports cause a failure until their production scope is explicitly reviewed.

## Data, ownership, and lifetime

There is no protocol, wire-layout, alignment, ABI, allocation, or production ownership change. Manifest tuples and regression sets are immutable test configuration. Reports retain the existing declaration-to-source mapping and multiple instantiated symbols per generic declaration. All five pipeline declarations and all six image-view declarations must be present; neither helper receives a lazy-native exception or an excluded production declaration.

The Python regression borrows repository sources read-only. Its only writes are synthetic parser input files in a `TemporaryDirectory`; the context manager removes those files even when an assertion fails. Missing/duplicated-boundary cases are parser-input checks, never compiled fault-injected production variants. No process credentials, GPU state, or application data is touched.

## Execution sequence

1. Run the source-inventory regression before either device sanitizer recipe.
2. Require exact recursive wire/native source sets and 32/45 imported tests, respectively.
3. Require exactly the eleven known helper production declarations before their delimiters and only the two actual image-view fixtures after them; preserve the render fixture exception.
4. Check that synthetic missing/duplicate fixture delimiters are rejected and a unique delimiter inventories only its production declaration.
5. Run the unchanged sanitizer pipeline: snapshot all source/dependency inputs, rebuild both C oracles with ASan/LSan/UBSan, emit Zig Debug LLVM, instrument every owned runtime definition, verify reversible guards/module preservation, compile/link, and prove per-module object/final hooks.
6. Execute with the unchanged strict leak-detection environment, verify complete expected tests and zero checker findings, then write the final report only after every gate succeeds.

## Concurrency and synchronization

Sanitizer runs keep their preexisting unique timestamp/PID artifact directories and synchronous subprocess ownership. No shared generated-header rewrite or report overwrite is added. The regression uses private temporary fixtures and may run independently. A shared phony make prerequisite lets either device sanitizer gate enforce the regression and deduplicates it within a single make invocation. Git commits are serialized with the workspace's agreed index lock and include only explicitly owned paths.

## Failure modes and acceptance

A missing or duplicate delimiter, closure drift, hidden helper declaration, extra fixture function, test-count drift, absent emitted definition, missing real ASan access, source mutation, timeout, runtime checker diagnostic, or nonzero runner exit must fail. This repair does not catch, suppress, or reclassify these failures as success.

The cloud executor can run all instrumented tests but its ptrace environment prevents LeakSanitizer's final thread scan. When the runner prints all tests passed followed by `LeakSanitizer has encountered a fatal error` and its explicit ptrace warning, the sanitizer target still fails and writes no completion report. Such evidence demonstrates source inventory, emission, hooks, and unit execution only. Native CI must establish the unchanged full zero-leak gate. Separate coverage runs are required locally because make stops before coverage on that LSan failure.

## Deferred broader owned-ICD migration

The separate owned-ICD/full-seam tools are not dependencies of the standard workflow's `vgpu-icd-sanitizers` target. They retain a fourteen-module exact ICD manifest, a sixteen-module recursion ceiling, twenty-module complete-seam assumptions, and older oracle/source lists. The current recursive ICD closure is thirty-two modules. General-graphics also contains scoped methods with duplicate short names, requiring a proper identity model rather than an exclusion workaround.

The eighteen additional ICD modules beyond the old manifest are: `venus_descriptor_template_native.zig`, `venus_dynamic_rendering_wire.zig`, `venus_extensions_wire.zig`, `venus_extra_objects_wire.zig`, `venus_graphics_dynamic_wire.zig`, `venus_graphics_general_wire.zig`, `venus_image_transfer_native.zig`, `venus_image_transfer_wire.zig`, `venus_image_view_native.zig`, `venus_modern_sync_wire.zig`, `venus_pipeline_wire_helpers.zig`, `venus_properties_native.zig`, `venus_properties_wire.zig`, `venus_requirements2_wire.zig`, `venus_sampler_descriptor_wire.zig`, `venus_shader_wire.zig`, `venus_transfer2_native.zig`, and `venus_wsi.zig`.

That full migration remains a distinct explicit blocker. This bounded repair neither raises its recursion ceiling, updates its manifests, invents fixture/lazy exceptions, nor claims that broader ownership gate passed. Physical GPU/VM, Windows execution, Vulkan support, and application acceptance are also outside this repair.
