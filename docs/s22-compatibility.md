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

### Vulkan device API gate (second fork commit)

Root-cause hypothesis, backed by sources but not yet confirmed on the target phone: the backend
requires Vulkan 1.2 only from the *instance* (the loader). A device behind it can report 1.1; the
backend then still calls 1.2-core device commands (buffer device address, timeline semaphores),
which can resolve to NULL. That matches the crash signature in Box #109/#121 (Adreno 730,
`pc 0`, in `ggml_vk_create_buffer`), and llama.cpp #28593 describes the same failure class on a
Mali GPU that reports 1.1. Box #121 also explains why a NULL guard on the buffer-address command
alone did not hold: the device's other 1.2 commands are just as missing.

The fork commit `1aa23b22b` (branch `s22/vk-device-api-gate`) checks the device's `apiVersion` on
all three paths that pick a device: the default list, the "first non-CPU device" fallback (which
previously skipped every check), and `GGML_VK_VISIBLE_DEVICES`. A device below 1.2 is skipped with
`ggml_vulkan: skipping <name>: device reports Vulkan X.Y.Z, the backend needs 1.2`, and the CPU is
used. This does **not** make the GPU usable on such a driver: it turns a crash into a clear
refusal. Support below 1.2 (the #28593 approach) was declined upstream and is not attempted here.

Tested on the host only: lavapipe through an `LD_PRELOAD` shim that reports the device as a 1.1
integrated GPU. A 1.1 device is skipped on all three paths; a 1.2+ device is still used, and the
tiny MoE gives the same output as on the CPU. **Not tested on an Adreno 730.** If the S22 driver
reports 1.2 or higher, this gate does not apply and the crash has another cause (see the
diagnostics below).

### Probe diagnostics

The isolated device probe now prints the step it enters (`BMOE_PROBE_STAGE devices|model|support|
bandwidth|costs`). When the child dies, the Plan screen's reason names the signal and the step, for
example `its probe ended the process that ran it (signal 11, SIGSEGV), while the device was asked
which operations it runs (its first buffers)`. Only names from a fixed list reach the reason; the
child's other output (driver logs, paths) does not.

### What to record on the phone

1. Plan screen, "Device backends": each device's status and reason (now with signal and step).
2. Whether `Vulkan0` (Adreno 730) still appears. If it is gone, the driver reports below 1.2;
   `adb logcat | grep ggml_vulkan` shows the reported version.
3. For the 35B Q5_K_M first-load failure: the error text shown, and whether the engine process was
   killed (`adb logcat | grep -i "lowmemorykiller\|lmkd"`) or ended with a signal. Not investigated
   yet: no log of it exists.

### Hexagon v69 (not changed)

The release APK carries Hexagon skels for v73-v81 only. PocketPal #960 records the same SIGABRT on an
SM8450: fastrpc fails to load the v73 skel on the v69 DSP, and the abort follows. Here the probe runs
in its own process, so the abort costs only time and the device is marked unusable. A v69 skel would
need the Hexagon SDK and upstream backend support for v69; it is not attempted.

First implementation: a separate llama.cpp fork commit resolves BDA and host-pointer-import commands per logical device and disables those optional paths if absent. It deliberately does not change the minimum Vulkan version or the global dispatcher. It is unverified on hardware. The parent repository pins that commit; upstream expert-ready and Hexagon hooks are preserved. No release or speed claim follows from this change alone.

Baseline comparisons: use the same model, quantization, prompt, context, token budget and sampling seed; record backend selection, prefill/decode rates separately, peak memory, cache hit rate and I/O wait. Run small-model smoke tests before large streamed models; compare streamed vs resident where memory permits. Keep experimental changes separate and retain a CPU-only escape path.
