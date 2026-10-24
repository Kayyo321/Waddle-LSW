# Graph and observer identifier audit

Read-only naming audit of the imported graph, ABI, observer, and test/native fixture declarations. Three type-binding issues remain; the C test/native fixtures contain no declaration violations.

| File and scope | Current token | Proposed token | Count |
|---|---|---|---:|
| `src/interop/window_graph_abi.zig:1311` and references at 1317, 1330, 1338–1340 | `BadKind` | `bad_kind_t` | 6 |
| `src/interop/window_graph_abi.zig:1419` and references at 1428–1429 | `Job` | `job_t` | 3 |
| `src/interop/window_graph_abi.zig` generic type bindings | `T` | `value_t` | 17 |

The `T` binding occurs in six independent generic scopes. Rename only within each scope:

- `span_of`: 3 token occurrences at lines 122, 124, 125.
- `array_span_of`: 3 token occurrences at lines 129, 131, 132.
- `aligned_const`: 3 token occurrences at lines 171, 171, 171.
- `aligned_mut`: 3 token occurrences at lines 175, 175, 175.
- `sentinel`: 3 token occurrences at lines 753, 753, 754.
- `bad_pointer`: 2 token occurrences at lines 1317, 1317.

The test/native C fixture declaration audit found no violations in:

- `tests/interop/window_graph.c`
- `tests/interop/window_observer_api_double.h`
- `tests/interop/window_observer_native_failures_windows.c`
- `tests/interop/window_observer_native_windows.c`
- `tests/interop/window_observer_windows.c`

PascalCase enum members, macros, constants, and constant tables conform. Windows SDK identifiers and imported C API/member names were excluded. The machine-readable report includes occurrence line/column data and counts by lexical scope at [`seamless_graph_naming_audit.json`](/tmp/seamless_graph_naming_audit.json). No repository files were changed; no build or test was run.
