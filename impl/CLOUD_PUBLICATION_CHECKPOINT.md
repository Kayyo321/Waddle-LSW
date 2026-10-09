# Cloud publication source map

Published baseline: `875ddc81b1316a9f48461fc431818b975e79eab3` on `feature/software-vgpu-slicing`.

The source checkpoints in the implementation receipts were created locally. Publication preserved their eleven atomic changes and messages, but fresh commit metadata produced different commit IDs. Each published tree was checked against the original Git tree SHA, so these pairs have identical file contents and modes. Use the published IDs for a fresh checkout or GitHub link; the original IDs remain provenance labels in earlier receipts.

| Original source checkpoint | Published commit | Identical tree |
| --- | --- | --- |
| `0df71a8ab05ee23143856de435d9f1574674d445` | [`c4a4d39ed155870a6b1c5ff64de58e5bd4f6e2eb`](https://github.com/Kayyo321/Waddle-LSW/commit/c4a4d39ed155870a6b1c5ff64de58e5bd4f6e2eb) | `50ba51521140b74118ff4575d63a63345ed8fa95` |
| `1585dac8a4b628febb2980daf3cb405e939c7943` | [`887c5f725665a83a5d9739d8b8651ee4726d4532`](https://github.com/Kayyo321/Waddle-LSW/commit/887c5f725665a83a5d9739d8b8651ee4726d4532) | `4c48cfd1c61ceab4df72844ef28cc5612fce3194` |
| `789460882284c793ecd09cab159ca04a78f7544c` | [`388b7b35064fcd529cf5c011562ed01ac65ff80a`](https://github.com/Kayyo321/Waddle-LSW/commit/388b7b35064fcd529cf5c011562ed01ac65ff80a) | `2c86ab7804fd6e91134de588c37c504ea56c1a1e` |
| `55d24fbb76cf316cd3bcb46e0f81c62aa8c52511` | [`2d4cb16b0f3e2792b2e0216e04b366c2b24c11f5`](https://github.com/Kayyo321/Waddle-LSW/commit/2d4cb16b0f3e2792b2e0216e04b366c2b24c11f5) | `1e7d1947c14565e27d0a0ef99e157f66d5b48007` |
| `7ac4306315bb620f149d4add110d842248c3fe97` | [`c854a79617b6414db1b1a9f651d1422d46bbbca7`](https://github.com/Kayyo321/Waddle-LSW/commit/c854a79617b6414db1b1a9f651d1422d46bbbca7) | `78db1657c56941bec693f7816b5ebb8554d18479` |
| `5f3a88444724a8f91475a626a6494cb33223b229` | [`272f030e1092fb1630ad5a7df920eb19af1cdb86`](https://github.com/Kayyo321/Waddle-LSW/commit/272f030e1092fb1630ad5a7df920eb19af1cdb86) | `0d73b251910d77f37f6194fd16dd6be06423d834` |
| `5be8152610619c49578f89581370d65ba0146181` | [`51ee17bca208db3241fefaf65fddcbc8577616dd`](https://github.com/Kayyo321/Waddle-LSW/commit/51ee17bca208db3241fefaf65fddcbc8577616dd) | `af8df65719998e1a203d7f4207be3af341b5d06d` |
| `57b07f629b9836b4260be1480ce365b6b3116da2` | [`6a4e65ad92608b973cdecb7f28f9120076f3f03e`](https://github.com/Kayyo321/Waddle-LSW/commit/6a4e65ad92608b973cdecb7f28f9120076f3f03e) | `f20e052a03ca6a84db95ab3df1584e5845a04f23` |
| `d1bb0ea972f56b5441e4a791ec2635849988b69c` | [`f130306e448a7ae6ce127a2b5cd47947c47b9cb9`](https://github.com/Kayyo321/Waddle-LSW/commit/f130306e448a7ae6ce127a2b5cd47947c47b9cb9) | `68c73771c08e8f553cbc3cdc2f65c1d75acd0477` |
| `908f83b919695eb79ed10eb31480be35fc662c8f` | [`78ed9d1e4629c4684b5eb8b6f3dae1a7f3b363f6`](https://github.com/Kayyo321/Waddle-LSW/commit/78ed9d1e4629c4684b5eb8b6f3dae1a7f3b363f6) | `8f5516dc04fbabe8b9732ab468c28d2fbee8e6db` |
| `60b9eecae5fc050a315899bb53ece7e5bafad63b` | [`875ddc81b1316a9f48461fc431818b975e79eab3`](https://github.com/Kayyo321/Waddle-LSW/commit/875ddc81b1316a9f48461fc431818b975e79eab3) | `40abc503c8b86b47c563a8530228ee9e8db38d8e` |

The branch update was a verified non-force fast-forward from `6eea43c743b589ab59be5b594721e44f7c3ce478`. README and dependency pins were unchanged. The final published tree is `40abc503c8b86b47c563a8530228ee9e8db38d8e`.

## Verification status at this backup

- Both implementation milestones, their regression tests, CI updates, and the [hardware testing handoff](HARDWARE_TEST_HANDOFF.md) are backed up in the published baseline.
- Local AV, WSI and 386-case Debug/ReleaseSafe ICD unit runs and Windows cross-builds passed as described in their receipts. Cross-builds are not Windows execution.
- The additional whole-ICD coverage gate finished below its unchanged 90% branch requirement: 4353/4837 branches (89.99%) and 4693/4764 lines (98.51%). This is a failed gate, not a rounded pass. A focused test correction is being validated separately; this backup does not claim that correction has passed.
- Required LeakSanitizer checks remain blocked by the cloud executor's ptrace restriction. Native Windows/Wayland/GPU, delayed HWND reuse, DXVK heap and integrated application acceptance remain open.
- CI started for the exact published baseline; pending CI is not acceptance. Record later results against their exact resulting commit using the [run template](HARDWARE_TEST_RESULT_TEMPLATE.md).


## Follow-on coverage correction (2026-10-09 UTC)

The six reviewed regression additions subsequently passed the unchanged whole-ICD coverage gate: **4369/4837 production branches (90.32%)** and **4694/4764 production lines (98.53%)**, exit 0. The branch denominator is unchanged from the failed baseline above; the new tests exercise 16 additional production edges. No production code, threshold, coverage harness or exclusions were changed.

The final source also passed all 392 ICD unit cases in each of Debug and ReleaseSafe, both 128-cycle native dispatch fixtures, and all 392 instrumented cases. Exact source evidence and commands are recorded in the [WSI tracker](wsi-window-lifetime/TRACKER.md). These results supersede the failed local coverage status, not the hardware or LeakSanitizer gates.

CI for the initial published baseline encountered Docker Hub failures before Linux checkout, including unauthenticated image-pull rate limiting. One Linux CLI retry failed for the same external dependency. Native Windows jobs were still running when this note was written. This is neither a source failure nor a CI pass; inspect CI for the latest branch SHA before acceptance.

## Follow-on published source map

The coverage correction and its documentation were verified at remote
`1de3fddf13067238ca4e428c223338ca79e37287`, identical to local `e827fde2cb3b0edfe47e858804dee2671103a372`
with tree `62d9d5d71d14ea6d9f8a37ddcf5ba2e0019bfc88`.

| Original source checkpoint | Published commit |
| --- | --- |
| `cce4711e1d1d3d767821c4277fdb3d1d0c76a779` | `b137f3ee008a54635d6fad382b7af37a546eb76c` |
| `c5ea2c72aeddfd2e4a7d5897b8542f3a1254f914` | `2d97603e3275d0c329ae81f3fe56413b641bd3d4` |
| `893ed455c0d3608a99fc24a66a3c7b0d65f842a5` | `96291c5a7ea9e26a3028c1a5b0433f4bd40e0650` |
| `e827fde2cb3b0edfe47e858804dee2671103a372` | `1de3fddf13067238ca4e428c223338ca79e37287` |

Each intermediate tree was also checked against its local counterpart before the
non-force branch update. Native and hardware limits above remain open.

## Windows test portability and review-index publication

Verified backup `e9b0663a961dfeaadc43dfa3ab7ce90900aed856` has the same tree
`ccf6ee0cc4c889a3d0381f35db7e89afb69c0a78` as local `f13634a728ba9ba7fb727f1558c423673d5ea43a`.
Every intermediate tree was also checked before the non-force update.

| Original source checkpoint | Published commit |
| --- | --- |
| `99e1701cb40e16af93285eb4512e0d79a4194fcb` | `4e07ac8edae6187da8fc8aeaa497df29e4a4f9c5` |
| `80762d227371aa1ce2b140c647d536a6a53469da` | `622e79e78891a639b6475a2e02d30b0e0d129583` |
| `b36d7d26f3026d0a45018a432c0b8a9442448196` | `b7c591274e6b896bbc0dffc1ce56056911c85a7d` |
| `cb0844a238570b3aa136a9c9ce7e72be52aaaf85` | `37970ae95d630c447c2fb108b1cef97535b9a059` |
| `f13634a728ba9ba7fb727f1558c423673d5ea43a` | `e9b0663a961dfeaadc43dfa3ab7ce90900aed856` |

The test-only Windows environment correction passed 393 Debug and ReleaseSafe
units, both Windows full-unit cross-links and unchanged whole-ICD coverage
(90.32% branches, 98.53% lines). Native execution of that correction is tracked
separately. See the [current review index](REVIEW_CHECKLIST.md),
[environment receipt](windows-icd-test-environment/TRACKER.md) and
[scoped AV leak-enabled CI receipt](desktop-input/EVIDENCE.md).

## Generation-safe AV lifecycle and native-oracle checkpoints

The following atomic checkpoints have matching local/published Git trees. The
source links are included in this handoff's containing publication, preserving
all production, test and documentation changes without force-pushing.

| Original source checkpoint | Published commit |
| --- | --- |
| `7294714487f914007411cac5c30e7baac2730806` | `3cf2faa0e7f9eccd674ae574c718e2b9c889dec2` |
| `db6a489510641eb5a1dfd3a7350732e8e1a32f42` | `dfe7969bca9a301ca426472f7752cba09f6d35a7` |
| `413c2c46e69d6b24d84691793e240524b011f7ef` | `cf1ca1e5c2108368742c3b5648eb5e1231662327` |
| `4e37ef9922f7c385914a81b4a6f565abacabcc3f` | `c11ce21af9df63ab4479ae18539ce73f19000aa1` |
| `84bcb08272a4512731d3bf34ac1cf017c9a07b2d` | `580349cda0ccebf26552ae6f8498baf48fb6fb55` |
| `1535600d5c5b3c72960efff6b2d3b441d8968a7d` | `b317c0577cafb20aa16463544491449cd1c61715` |
| `56b48be4d6fc1052a32b20eda5f9fbbfeb62928a` | `1337fe5a3a02695b047209b24f8474b60acfeb9c` |
| `dec66026e79d64ad36049ab0d125fb4491142d1b` | `efc09c87a4508185ad98dff610ea34529079f6cc` |
| `fd6092f7c016ebe732a3a46d51f60f844919b2e1` | `f8163d926b6e232660f4e6fe5dd44da64b68d430` |
| `89c1da2abbd36c0da980f0e74135c1049f8bc46d` | `f6e5695dd4c6c365f18baeb18ffa30003c7424fc` |
| `df5675b8ffec14ed30cbb479c8e420fe9d42f7d9` | `7c26f20d27ed1ecaeca427cef09ca2b9ba348861` |

The lifecycle implementation preserves the 328-byte envelope, adds explicit
CreateV2 admission, rejects stale identity effects and requires coordinated
host/guest upgrades. Its compatibility, identity and callback fixtures pass;
three Windows executables cross-link. The later platform-fixture correction
records synthetic compositor input without claiming Windows injection.

The native extension oracle repair changes tests only: Windows' exact guest
swapchain projection and Linux's empty legacy projection are now asserted.
393 Debug/ReleaseSafe and instrumented ICD tests pass; whole-ICD coverage stays
90.32% branches and 98.53% lines. Native Windows execution of this repair and the
new lifecycle fixtures remains pending on the resulting published commit.

At earlier source `c11ce21af9df63ab4479ae18539ce73f19000aa1`, Linux CI verified
leak-enabled device-wire/native, WSI and ICD sanitizer stages. This supplements
rather than rewrites the failed local ptrace receipts. Overall vGPU CI still
failed: Windows had the now-corrected extension oracle; Linux push later missed
a presented-worker link input, and identical-source PR coverage unexpectedly
exceeded its site recorder. Those latter failures are being investigated, not
hidden by threshold/exclusion changes. See the [current checklist](REVIEW_CHECKLIST.md).

## Presented-worker link follow-on

The lifecycle/oracle handoff was verified on branch commit
`ec8bf07230e36c861b3efeb524b5030f6e2dd9a7`, identical to local
`485f9662107cc3b28c2a509ae340971a67a27cca`, tree
`5773a9f6d843d9bc7bb6122070f843c5ff23abc9`.

The next bounded repair adds the existing production native sink object to the
shared presented-worker link inputs. It does not change production code, mocks,
assertions or acceptance criteria. Four real normal/sanitized worker binaries
cross-link with unique resolved native symbols; full local execution is blocked
by missing renderer build prerequisites and awaits CI.

| Original source checkpoint | Published commit |
| --- | --- |
| `18e9d57ed79775779ad4d7416b77e516adbf26fd` | `37df9499f197c51443e1aca892139cf6a41a59c6` |
| `449c9f72f9a0953c3d61e3f45a1aca05e7b03d4f` | `d6be1c1cf9717d08ca2ee53ca1efa25ecddeeb20` |

Both trees were matched exactly. See [the receipt](presented-worker-link/TRACKER.md)
for the actual attempted commands, missing prerequisites and unchanged LSan limits.


## Native CI receipt at the worker-link backup

Verified branch commit `f04e3583ad2af32f28145933c2df563ee4cac295` matches local
`9762151431a3bc7b368d3fccb66a197afac52263`, tree
`a97736e864273089ebc8b97bfb1a9bb0427af022`.
Both AV workflows and the native guest passed at this source. Linux AV exercised
actual leak-enabled tests, including identity coverage 25/25 lines and 58/58
branches, and input coverage 78/78 lines and 104/104 branches. Native Windows AV
also exercised the new lifecycle/quiescence fixtures. [AV run evidence](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38002124467/job/114063342283).

Native Windows vGPU cleared the prior environment and extension-oracle failures,
then reported 391/393 unit tests passing. Its two failing WSI fixtures omit a
required synthetic device capability; their repair is tracked separately. Linux
vGPU and worker runtime qualification remain pending as of this receipt. This
native CI evidence does not qualify physical input, Wayland/GPU integration or
Adobe workloads. Consult the [current index](REVIEW_CHECKLIST.md) for later status.


## Native WSI fixture and all-sites recorder repairs

Verified remote `51583f927a0ba82adecaa6d48d2814ffda97bc26` matches local
`a181bf2124b27d61989b0770c4298a4dadd7a83f`, tree
`3c24de96dca614feaf3a7f690383b0a73bdd3f45`.

| Original source checkpoint | Published commit |
| --- | --- |
| `a085361d65460f2cee318ea0f1ebcc299e5f014e` | `337021afc1628e80b5032c8ca29ed6e0cf987a44` |
| `ffd5beba8b827422399b27b0e10d6d2274394fc8` | `0c65763ee5a06bdf3606a4ad82a2c6ef46fb2ea9` |
| `110117947714e4d5d1b5d97a678a7bb977e47907` | `6f9494ce135cd88b8086a4e6edb97d627752e297` |
| `e268984ec71b10e2cc848802001d5724299fd103` | `8d2dd4f1b24ca322e53efbb26c2bbedf992e250c` |
| `a181bf2124b27d61989b0770c4298a4dadd7a83f` | `51583f927a0ba82adecaa6d48d2814ffda97bc26` |

The WSI fixture repair passes 394 Debug and ReleaseSafe units, three C ABI
fixtures and both complete Windows cross-links. No production or original test
body changed. Native Windows rerun remains pending.

The recorder repair retains every native raw site, source key and threshold
within the unchanged 512KiB storage budget. Seven focused tests, exact original
76,512-site Zen3 replay and final native aggregate passed independent review.
Local leak detection remains ptrace-blocked; strict sanitizer CI is mandatory.
See [the fixture receipt](native-wsi-test-capabilities/TRACKER.md) and
[recorder receipt](coverage-recorder-integrity/TRACKER.md).

The prior f04e358 Linux CI also proved the presented-worker link repair, then
failed the real worker runtime with mode 0. This remains an open acceptance
blocker, not an inferred pass from successful linking.
