# Advisory O5 destroy-preservation proposals

These documents are reviewed research, not an accepted observer interface or
implemented preservation path. Only IMPL_DESC8.22.3 tracked snapshot is frozen
and implemented. Later cleanup_api.md corrects the older proposals that
permanently retain observer-local allocations or call stopped drains callback-
free. Root still must consolidate one normative contract/header before source
changes. No progress is awarded by these proposals.

The selected direction reserves pending/tracked/candidate keys plus exact Free
slot headroom, keeps delivered Destroy evidence outside the ordinary hint FIFO,
protects installation/unhook callbacks and treats unhooked intervals as
discontinuities. The coordinator must block publication/capture behind pending
evidence and freshly validate cached Deferred children. Stop/bind invalidation
retains Retiring native resources; it proves no release.

Quiesced handoff may copy retained evidence into fixed caller-owned quarantine
storage and free only observer-local memory, with no restore or epoch authority.
Its final output/alias/lifecycle rules and tests remain to be frozen. Native
bundle/backing ownership remains external. Original /tmp paths in the research
are historical; these copies are durable.
