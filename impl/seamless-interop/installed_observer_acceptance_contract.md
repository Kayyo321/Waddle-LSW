# Installed observer lifecycle fixture

Root owns `tests/interop/window_observer_installed_windows.c`. This is a bounded
native acceptance fixture, not a session agent or capture coordinator. It runs
with a real interactive Default desktop and launches one exact child process
of itself. The child owns a visible toplevel and an owned dialog; the parent
owns the real native observer and graph. No API query, hook, or graph is doubled.
The child has a 30-second lifetime cap and a named stop event. Named objects
include the parent PID and reject preexisting names. Process handles are not
inherited; the parent joins that exact child before releasing its handles.

The parent verifies foreign-process identity and real graph admission, parent
identity/generation, native title update, hide quarantine, blocked stale action,
explicit retirement completion, and readmission of the same still-valid HWND
with a larger generation. Every callback-capable owner-thread operation follows
the observer reservation boundary; native event delivery is dispatched with a
bounded message budget. Refresh/pump/event draining are bounded by a five-second
phase deadline. No synthetic hint or fabricated HWND is inserted into the graph.

This fixture never creates capture objects, mappings, frame pins, host buffers,
wire channels or compositor surfaces. Its retirement assertion is therefore
limited to the factual empty local capture/pin sets in this fixture. It does not
establish any production detach/epoch-reuse proof. Cleanup stops the observer,
drains copied events, completes these empty-resource retirements, synchronizes
its tracked snapshot, then destroys observer and graph. A retained error is a
fixture failure; no success result hides failed cleanup. The child is stopped,
joined, or explicitly terminated and joined on failure. Prior DPI context is
restored. Native execution is required; cross-compilation alone earns no credit.
