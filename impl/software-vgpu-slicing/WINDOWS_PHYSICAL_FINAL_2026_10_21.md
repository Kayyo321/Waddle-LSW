# Final Windows physical GPU matrix

Root alone ran the signed-in preserved Windows11 guest against the RTX5080
Linux host. `build/wddm_dxvk_checkpoint/final_vulkan_f1d49c85/results.json`
contains12cases: normal and ASan/LSan/UBSan host builds, each executing triangle,
compute and compute_push, each with a positive and postflush failure case.
The six positives complete48 exact GPU lifetimes. The six postflush cases
return expected native1 while proving local mapped-memory poison/flush cleanup.
All12 controllers/workers return0, authenticated sessions are distinct, owned
host graphs retire without supervisor signals, and guest inputs match before
and after. This is actual host/guest physical execution, not a mock/cross-build.

The source8038ad9 snapshot is
`build/wddm_dxvk_checkpoint/dxvk_source_69bec518`; root binds216 relevant
production/include/TCP fixture/audit/loader files in
`build/qualification_feature_20261021/windows_physical_binding.json`.
`run_windows_physical_revision1.py` and `windows_physical_twelve.py` preserve the
exact invocation and the checked inverse of its only workload-list change.
The presentation/WSI lifecycle workload is qualified separately by the strict
[M2 ownership runs](../wsi-window-lifetime/M2_OWNED_NATIVE_2026_10_21.md).
No GDI/USER assertion was removed from that gate.

Independent offline recomputation is preserved in
`build/qualification_feature_20261021/final_scope_reviews/windows_physical/raw_receipt_review.json`,
SHA256 b6a675e7cb7abcb39ad890872a65edcb96d29165f90834ee4e2c9d8e62a64add.
It checks all12 raw folders,48lifetimes,216sources,12sessions and49 closed
pidfd owners (the last case includes one additional witnessed controller child).
The portable37-module/Linux physical qualification remains separately bound
in `QUALIFICATION_CONTINUATION_2026_10_21.md`.

This matrix completes its stated functional physical scope. It does not
establish DXVK/OpenCL native heap lifetime acceptance or full OpenCL reload
stress. Those remaining feature gates, exact final CI and independent review
prevent final feature closure. Parent task credit remains unchanged here.
