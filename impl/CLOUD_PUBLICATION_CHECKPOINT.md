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

