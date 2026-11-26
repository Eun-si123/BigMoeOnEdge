# Stock Android / Snapdragon 8 Gen 1 compatibility plan

Goal: run large local MoE GGUFs on stock Android (12 GB RAM, Adreno 730, Hexagon v69, UFS 3.1) without root or a remote inference host. Preserve the upstream engine's lossless streaming, isolation, and portable planner. Optimize sustained decode, not just prefill. No claimed support until tested on the actual device.

See [external research and implementation references](research-references.md) for source-specific reuse candidates and validation requirements.

## Evidence and baseline

- User observation: Vulkan probe SIGSEGV (11), HTP probe SIGABRT (6), OpenCL prompt rate approximately 1.0x CPU, CPU selected. These are symptoms, not symbolized root causes.
- Large 35B Q5_K_M model reportedly generated at 2.8 tokens/s after a failed load; launcher reload suggests memory pressure, but no LMKD record or exact model/backend diagnostic has been captured.
- [Box #109](https://github.com/jegly/Box/issues/109) attributes a matching Adreno 730 crash to an unresolved Vulkan buffer-device-address entry point, established by disassembly. [Box #121](https://github.com/jegly/Box/issues/121) records remaining failures after a fix. Treat this as a hypothesis for this device.
- [llama.cpp #28593](https://github.com/ggml-org/llama.cpp/pull/28593) describes device dispatch and extension fixes on a different GPU, not proof on Adreno. It was not merged; do not globally replace the dispatcher or claim support for pre-1.2 devices.
- [PocketPal #1049](https://github.com/a-ghorbani/pocketpal-ai/pull/1049) handles OpenCL integer-dot capability. [#1110](https://github.com/a-ghorbani/pocketpal-ai/issues/1110) identifies an unrelated unsupported opt-in large-buffer compiler flag. The current BigMoeOnEdge frontend does not set that opt-in.
- [ExecuTorch Qualcomm backend](https://docs.pytorch.org/executorch/stable/backends-qualcomm.html) and [snapdragon-npu-llm](https://github.com/avisre/snapdragon-npu-llm) are research references for v69, not drop-in GGUF or MoE compatibility.

## Ordered work and acceptance gates

| Priority | Work | Acceptance |
| --- | --- | --- |
| P0 | Capture device/driver/API, model fingerprint, probe and load stages, sanitized child crash logs and memory figures; make diagnostics exportable | On stock device, distinguish native SIGSEGV, SIGABRT, driver error and LMKD; no prompt or secret in export by default |
| P0 | Vulkan: resolve optional commands per logical device, disable unavailable paths; investigate required timeline commands and core/KHR dispatch separately | No null calls in mocked capability cases; Android arm64 build, isolated probe, small GGUF prefill and decode correctness; then Adreno 730 35B streaming test. CPU fallback remains available |
| P0 | Memory: audit planner budget, load peak, common weights, context, cache brake and child restart | Repeated cold and warm loads of 35B Q5_K_M, context sweep, background pressure; no uncontrolled OOM or silent downgrade; report cache and memory high-water |
| P1 | OpenCL: check integer-dot fallback and large-buffer opt-in; measure actual kernels | Unsupported features do not abort; compare prompt and decode separately with CPU at identical model/context |
| P1 | Hexagon v69: check SDK/firmware availability, ABI and supported ops; isolated probe, then a small model with a supported backend | Real v69 device test, numerical equivalence within backend tolerance, graceful refusal of unsupported operations; only then consider GGUF/MoE integration |
| P1 | Heterogeneous CPU/GPU/NPU planning | Compare each backend and combinations, include transfer/synchronization overhead; only choose faster verified paths |
| P2 | UFS 3.1 reads, cache/prefetch policy, sustained thermal behavior | Same model and output, fixed context, repeated cold/warm measurements; report median and sustained generation, read volume, memory and temperature |
| P2 | Optional app-local Turnip | No root/system driver replacement; verify loader, license, compatibility and fallback; only if system Vulkan cannot be made reliable |
| P3 | Debug APK, regression gates, portable release | Build arm64 CPU/Vulkan/OpenCL; run existing host gates, real-device tests and independent regression checks before release |

First implementation: a separate llama.cpp fork commit resolves BDA and host-pointer-import commands per logical device and disables those optional paths if absent. It deliberately does not change the minimum Vulkan version or the global dispatcher. It is unverified on hardware. The parent repository pins that commit; upstream expert-ready and Hexagon hooks are preserved. No release or speed claim follows from this change alone.

Baseline comparisons: use the same model, quantization, prompt, context, token budget and sampling seed; record backend selection, prefill/decode rates separately, peak memory, cache hit rate and I/O wait. Run small-model smoke tests before large streamed models; compare streamed vs resident where memory permits. Keep experimental changes separate and retain a CPU-only escape path.
