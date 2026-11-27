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

### Vulkan: what is known, and the fork commits

The Adreno 730 crash (Box #109/#121: `pc 0`, in `ggml_vk_create_buffer`, on driver 0615.95) means a
command resolved to NULL, or to a stub that jumps to an empty slot. Which command, and why, is **not
confirmed** on this phone. Box #109 reports "Vulkan API 1.3 (0x00403000)" without saying whether that
is the loader's version or the device's; if it is the device's, a "device below 1.2" explanation does
not apply to that phone. The diagnostics below exist to settle it.

Fork branch `s22/vk-device-api-gate` on `Eun-si123/llama.cpp`, on top of `f7adfde`:

- `1aa23b22b`: a device whose own `apiVersion` is below 1.2 is skipped, with a log line, on every path
  that picks a device (the default list, the "first non-CPU device" fallback that skipped every check,
  and `GGML_VK_VISIBLE_DEVICES`). Applies only if the driver reports 1.1.
- `ba5f8943f`: **a mechanism reproduced here that defeats NULL checks.** ggml's global dispatcher takes
  `vkWaitSemaphores`, `vkSignalSemaphore` and `vkGetSemaphoreCounterValue` from
  `vkGetInstanceProcAddr`. Where the core name answers NULL, Vulkan-Hpp falls back to the KHR alias,
  which the loader does answer; the device never enabled `VK_KHR_timeline_semaphore`, so that
  trampoline jumps to an empty slot. The result is SIGSEGV at pc 0 in
  `ggml_backend_vk_device_event_synchronize`, which a check on the dispatcher cannot see, and
  `f7adfde`'s guards do not cover. It is the same failure class as Box #121 (a guard added, the same
  crash), which does not prove it is the same command. With one Vulkan device the commit takes these
  three commands from `vkGetDeviceProcAddr`: **the GPU keeps running.** If the device has none of them,
  it stops with the command named instead of crashing. With several devices the global dispatcher is
  left alone, because one device's pointer is not valid for another.
- The same commit removes `1aa23b22b`'s `vkGetBufferDeviceAddressKHR` fallback. The report below shows a
  conformant driver (Mesa) answering NULL for the KHR name until the extension is enabled, and ggml
  never enables it. So the fallback can only ever succeed on a non-conformant driver, by handing out a
  command that is not valid to call.

Mock tests on the host: lavapipe shown as an integrated GPU, through an `LD_PRELOAD` interposer
(`SHIM_NULL`, `SHIM_NULL_INSTANCE_ONLY`, `SHIM_API11`) used during development; the shim is not
committed.

| Case | Before (`f7adfde`) | After (`ba5f8943f`) |
| --- | --- | --- |
| no NULL | runs on the GPU | runs on the GPU, same output as the CPU |
| `vkGetBufferDeviceAddress` NULL (instance and device) | BDA off, runs | same |
| `vkGetMemoryHostPointerPropertiesEXT` NULL | staging, runs | same |
| `vkWaitSemaphores` NULL on the instance only | **SIGSEGV pc 0** | log line, **runs on the GPU** |
| a timeline command NULL on instance and device | **SIGSEGV pc 0** (vkWaitSemaphores) | abort naming the command |
| device reports 1.1 | used | skipped, CPU |

`test-backend-ops -o MUL_MAT_ID` on lavapipe: 1194/1194 with no NULL. The run with all three timeline
commands NULL on the instance is noted in the PR. **Nothing here has run on an Adreno 730.**

Not covered: `resetQueryPool` (1.2 core) is reached only with the perf logger on (`GGML_VK_PERF_LOGGER`).

### Vulkan report (`/api/diagnostics/vulkan`, "Copy Vulkan report")

The question the fixes above cannot answer without the phone is which command the S22 driver leaves
NULL, and on which path. The report asks the driver directly, in a child process, without ggml. It
lists the loader and device versions; features; extensions; every command above, core and KHR, through
the instance and through the device, on a device created as ggml creates it and again with the KHR
extensions enabled; and then the buffer-address and timeline calls themselves. See
[server-api.md](server-api.md). On the phone: Plan screen, "Copy Vulkan report", paste the text into
the issue.

### Probe diagnostics

The isolated device probe now prints the step it enters (`BMOE_PROBE_STAGE devices|model|support|
bandwidth|costs`). When the child dies, the Plan screen's reason names the signal and the step, for
example `its probe ended the process that ran it (signal 11, SIGSEGV), while the device was asked
which operations it runs (its first buffers)`. Only names from a fixed list reach the reason; the
child's other output (driver logs, paths) does not.

### What to record on the phone

1. Plan screen, "Copy Vulkan report": the whole text. It settles the device version and the NULL commands.
2. Plan screen, "Device backends": each device's status and reason (now with signal and step). Whether
   `Vulkan0` (Adreno 730) is used, refused on results or speed, or unusable, and why.
3. For the 35B Q5_K_M first-load failure: the error text shown, and whether the engine process was
   killed (`adb logcat | grep -i "lowmemorykiller\|lmkd"`) or ended with a signal. Not investigated
   yet: no log of it exists.

### What a working GPU can be expected to give

Getting the Adreno past its crash makes it *eligible*; the planner still decides whether to use it, on
the same measurements as on every machine (`docs/backends.md`). On the phones measured so far, a GPU
that shares the cores' memory did not decode faster: decode reads every active weight once per token,
and the GPU reads the same LPDDR at a similar rate. Prefill is where it could help. So "the GPU runs"
and "the GPU is used" are separate results, and the plan reports both. The planner is not loosened to
choose a device that measured slower or did not reproduce the CPU's answer.

### Hexagon v69 (research; no code)

The release APK carries Hexagon skels for v73-v81 only. PocketPal #960 records the same SIGABRT on an
SM8450: fastrpc fails to load the v73 skel on the v69 DSP, and the abort follows. Here the probe runs
in its own process, so the abort costs only time, and the device is marked unusable. llama.cpp's
Snapdragon backend documents v73 and newer only (`docs/backend/snapdragon/README.md`, read 2026-11-27).
For the v69 paths, see [research-references.md](research-references.md). None of them is a GGUF/ggml
backend, and none is a drop-in for this engine's streamed MoE. Each needs Qualcomm's QNN/QAIRT SDK,
which is account-walled and not redistributable, so a build here cannot include it.

### Fork commits pinned

`f7adfde`, `1aa23b22b` and `ba5f8943f` on `Eun-si123/llama.cpp` `s22/vk-device-api-gate`, on top of
the two upstream hooks (`docs/seam.md`). With one Vulkan device the global dispatcher's three timeline
commands are replaced by the device's own; with several it is left alone. Unverified on hardware. No
release or speed claim follows from these changes alone.
