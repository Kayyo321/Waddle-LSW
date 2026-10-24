# Codex review of the Claude window-graph return

The human released clean isolated branch `feat/seamless-window-graph` at
`3714cf2`. Codex reviewed and corrected that return in the same isolated branch
through `73bc8330af5423989d62aad3824a771e169e356f`. The original return and
its commit history remain available; `reviewed_branch_history.txt` preserves
all branch subjects/bodies. The parent imports an explicit owned-file snapshot,
without replacing its newer global tracker, design or build files.

Accepted corrections include create-output alias preservation, retaining the
singleton reservation until hook cleanup succeeds, retryable failed unhook
ownership, standalone native-query snapshot cleanup, target-correlated native
hook assertions, and pruning cached deferred descendants across accepted
parent destruction, exclusion or identity change. Same-incarnation
reclassification retains the existing parent-first flush behavior.

The final source passes the complete isolated Linux gates, including both Zig
optimization modes, C consumers, leak-enabled sanitizers, unchanged90%
production coverage gates and exact21 exported symbols. Actual Windows
CI40126247004 passes all145 graph tests,159 ABI tests, C27, API doubles65 and
the own-process real Win32 adapter smoke. Exact source identities, logs and CI
limits are in `review_qualification_a32922b.json` and
`ci_qualification_73bc833.json`. Main-checkout qualification is recorded
separately before parent credit.

O5 remains open: a full ordinary hint queue discards delivered destroy evidence;
a later refresh with an identical HWND identity tuple cannot prove continuity.
Hook-install/restart callback gaps are part of the pending preservation design.
Portable observer/native adapter credit is withheld until their applicable
requirements pass. Installed guest, foreign-process/UIAccess/UAC/secure-desktop,
capture, slot/wire integration and compositor release remain separate work.
No CPU unit or own-window smoke proves native resource drain or epoch reuse.
