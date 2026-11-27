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

### Device diagnostics (stock S22 Ultra, Vulkan report, 2026-11-27)

What the phone's own report said, reported by the owner (values from the Vulkan API only; no
serial, account or path):

| Item | Value |
| --- | --- |
| Loader (instance) version | 1.4.0 |
| Physical device | Adreno 730, **API 1.1.128** |
| Conformance version | 1.2.7.2 |
| `VK_KHR_buffer_device_address` / `VK_KHR_timeline_semaphore` | offered |
| `bufferDeviceAddress` / `timelineSemaphore` (through the extensions' structs) | yes / yes |
| Logical device with both KHR extensions enabled | created |
| `vkGetDeviceProcAddr`, core names (`vkGetBufferDeviceAddress`, `vkWaitSemaphores`, `vkSignalSemaphore`, `vkGetSemaphoreCounterValue`) | NULL |
| `vkGetDeviceProcAddr`, the `...KHR` names | valid |
| Core `vkGetBufferDeviceAddress` through the instance | **SIGSEGV** (reproduced) |

So on this phone the cause is settled: a 1.4 loader in front of a device that reports 1.1 and offers
the 1.2 features only as extensions. ggml requires 1.2 from the instance alone, reads features
through `VkPhysicalDeviceVulkan12Features` (not defined for a 1.1 device: left unfilled), never
enables the KHR extensions, and calls the core names. Box #109's "1.3" on another Adreno 730 is
probably that phone's loader version; it is not this phone's device version.

### Second report from the phone (report with KHR-only calls and SPIR-V pipelines, 2026-11-27)

Same device: Adreno 730, API 1.1.128, loader 1.4.0, driver id 8 (Qualcomm proprietary), 90 device
extensions offered.

| Item | Value |
| --- | --- |
| Features (through the extensions' structs) | bufferDeviceAddress, timelineSemaphore, storageBuffer16BitAccess, storageBuffer8BitAccess, shaderInt8, shaderFloat16: yes. **shaderInt64: no** |
| Offered | `KHR_spirv_1_4`, `KHR_shader_float_controls`, `KHR_vulkan_memory_model`, `EXT_scalar_block_layout`, `KHR_shader_subgroup_extended_types`, `KHR_uniform_buffer_standard_layout`, `EXT_host_query_reset`, `EXT_subgroup_size_control`, `KHR_driver_properties` |
| Not offered | `EXT_external_memory_host`, `EXT_memory_budget`, `KHR_shader_integer_dot_product`, `KHR_cooperative_matrix`, `EXT_pipeline_robustness`, `KHR_maintenance4` |
| Logical device with the KHR extensions enabled | created |
| `vkGetBufferDeviceAddressKHR` / timeline commands through the device | resolved, called: non-zero address; signal, read and wait succeed |
| Compute pipelines from SPIR-V **1.3 and 1.5** modules (plain, and with 8/16-bit storage, Int8, Float16, subgroup arithmetic) | **all four created** |

What follows for the backend:
- The SPIR-V 1.5 shaders ggml ships are taken by this driver below 1.2, at least for the capabilities
  most of them declare. A SPIR-V 1.3 shader set is not needed, and is not built.
- With no `shaderInt64`, the four `_bda` shader variants are not used. The backend gates them on
  int64 and BDA together, which is the path tested with `S22_NO_INT64`.
- No host-pointer import (`EXT_external_memory_host`): uploads go through staging. No memory budget
  extension: ggml estimates the budget itself.
- No integer dot or cooperative matrix: the scalar shader paths, which are the only ones the Android
  build carries anyway.

Still open: the backend itself loading a model on this device, every real shader's pipeline (not
only the report's four), and what the planner measures for it.

### Plans on the phone with the 1.1 + KHR path (2026-11-27)

The Vulkan device now registers and its probe runs; no SIGSEGV. Two models, as reported by the owner:

| | Dense model (about 5 GB) | Qwen3.6-35B-A3B Q4_K_M (MoE) |
| --- | --- | --- |
| Vulkan0 | prompt-width probe 2.5x the cores; refused: "has not been shown to compute the host's results on this model" | **SIGABRT (signal 6) in the memory-rate step** (`bandwidth`); unusable |
| OpenCL | refused on memory headroom | prompt 1.2x the cores, results agree: **armed for the prefill** |
| Decode | cores (the model is dense: nothing streams) | cores; expert cache 3.9 GiB of a 7.2 GiB budget; direct I/O on, 4 read lanes; predicted 5.7 tok/s |

**What the two runs measure differently** (`bandwidth_probe.cpp`, `layer_probe.cpp`):

| | Dense | MoE |
| --- | --- | --- |
| One-token probe | **F32** 16384x4096, **256 MiB** in one tensor (a model with no expert type measures in F32) | the experts' type (Q4_K), 16384x4096, about 36 MiB |
| Prompt-width probe | synthetic F32 4096x4096 at 512 tokens (64 MiB) | **the model's own layer** (`MUL_MAT_ID` on its Q4_K/Q6_K experts, about 64 MiB held, plus its dense matrices) at decode and prompt width |
| Identity (same result as the cores) | the one-token probe only: the real-layer probe declines a model without experts | the real layer at decode width, and at prompt width |

So the shaders differ (an F32 matvec against quantized `MUL_MAT_VEC`/`MUL_MAT_ID` paths), and so do the
sizes (one 256 MiB binding against several bindings of a few to tens of MiB).

- **Dense, "not shown":** reproduced on the host (lavapipe behind the shim with
  `S22_MAX_SBR=134217728`). A device whose `maxStorageBufferRange` is below the 256 MiB F32 probe
  declines it (`supports_op` false), so its identity stays Unknown, its rate 0, and the plan refuses
  it on "not shown". Nothing measured it wrong; nothing measured it at all. The phone's actual range
  is not known yet: the report now prints it.
  Changed: such a device is now asked for identity on the wide instrument's smaller matrix (4096
  rows) at one token, against the cores on the same bytes. Its rate stays unmeasured, because a
  matrix that size can sit in a cache. The bound is unchanged; this is a comparison that now
  happens, not a check skipped.
- **MoE, SIGABRT:** not reproduced on the host. The same Q4_K_M expert mix (`llama-quantize` of a
  1024-wide, 64-expert synthetic model) runs both probes there. That holds under the S22 shim with
  coopmat hidden, with the storage range at 8 MiB, and with the allocation limit at 16 MiB. The real
  layer agrees with the exact result (1.7e-13 at one token, 1.9e-5 at prompt width). With the range
  at 8 MiB the expert ops are declined, not aborted. The real layer holds about 64 MiB of experts,
  so a size limit is not the likely cause. A `GGML_ABORT` or an uncaught `vk::` exception (a
  pipeline the Adreno compiler refuses, or a lost device) in one of the quantized shaders is. Both
  print a line before the abort, which the probe kept and dropped.
  Changed: the probe's reason now carries that line, cut to the source file's name and the message
  (`probe_last_words`), e.g. `it said: "ggml-vulkan.cpp:8655: Requested preallocation size is too large"`.
  The next run on the phone names the failing step.

### The abort named, and the fix tried (fork commit `d486a5759`)

The next run on the phone (same Qwen3.6-35B-A3B Q4_K_M) carried the backend's last words in Vulkan0's
reason:

> its probe ended the process that ran it (signal 6, SIGABRT), while the device's memory rate was
> measured; it said: "libc++abi: terminating due to uncaught exception of type vk::UnknownError:
> vk::Device::createComputePipeline: ErrorUnknown"

and its Vulkan report gave the limits: `maxStorageBufferRange=134217728` (128 MiB),
`maxMemoryAllocationSize=1073741824`, `maxComputeSharedMemorySize=32768`, workgroup at most 1024
invocations (`1024x1024x64`), subgroup size 64, no clustered subgroup operations, heaps of 11204 and
256 MiB.

- **Dense "not shown": confirmed.** 128 MiB is below the dense probe's 256 MiB F32 matrix. The
  previous commit's identity check on the smaller matrix is the fix for it.
- **MoE abort: the driver's shader compiler refuses a pipeline.** The memory-rate step first builds
  the q4_K mat-vec (the model's experts are Q4_K). llama.cpp #28635 reports exactly this on an Adreno
  830 for `mul_mat_vec_q4_k_*` and `mul_mat_vec_id_q4_k`, with no fix upstream. A commenter narrowed
  it: the pipeline fails when its workgroup (specialization constant 0) is 32 or more, passes at 31
  and below, and every other quant passes (q4_0, q5_K, q6_K, q8_0, ...). On the Adreno, ggml already
  turns subgroup arithmetic off, so the q4_K mat-vec is built with the shared-memory reduction at
  workgroup 64 or 256, both in the failing range.
- **Fix tried:** on `VK_DRIVER_ID_QUALCOMM_PROPRIETARY`, with that reduction, the three q4_K mat-vec
  pipelines (f32, f16, `MUL_MAT_ID`) are created at workgroup 16. The shader splits its workgroup
  into groups of 16, so 16 is its smallest valid size. Nothing else changes: other drivers, other
  quants, the reduction. On lavapipe behind the shim with `S22_QCOM=1` (Qualcomm's driver id, so
  this path is taken), `test-backend-ops` MUL_MAT q4_K 117/117 and MUL_MAT_ID q4_K 114/114.
  **Whether the Adreno compiler accepts it, and how fast a 16-wide q4_K mat-vec runs there, is the
  phone's to show.** If another pipeline is refused, the reason will now name it: the probe's last
  words lead with ggml-vulkan's "compute pipeline creation failed for <name>" line.

### Vulkan 1.1 + KHR path (fork commit `8644cb849`)

A device below 1.2 is driven through its extensions (`vk_pre12_features` in the fork):

1. **Features and properties**: through each extension's own struct (16-bit storage (core 1.1),
   `KHR_shader_float16_int8`, `KHR_8bit_storage`, `KHR_buffer_device_address`,
   `KHR_timeline_semaphore`, `KHR_vulkan_memory_model`, `EXT_scalar_block_layout`,
   `KHR_shader_subgroup_extended_types`, `KHR_uniform_buffer_standard_layout`, `EXT_host_query_reset`;
   subgroup, float-controls and driver properties). Their values are copied into the fields the rest
   of the backend reads, so every later decision is the same code.
2. **Enabling**: each of those extensions the device offers is enabled by name, with its struct in
   `vkCreateDevice`, plus `KHR_shader_float_controls` and `KHR_spirv_1_4`. Enabling the extension is
   what makes its KHR commands valid to call.
3. **Dispatch**: each 1.2 command comes from `vkGetDeviceProcAddr`, by its core name, else by its
   KHR/EXT alias only if that extension was enabled; the global dispatcher's entries are replaced by
   these. Because the dispatcher is global, a pre-1.2 device is kept only when it is the sole Vulkan
   device (a phone); a device with no timeline semaphores at all is skipped.
4. **1.2+ devices** take the unchanged path. On every device the CPU stays the fallback, and the
   planner still uses the GPU only where it measures faster with the same results.

Earlier commits on the branch: `f7adfde` (BDA and host-import guards), `1aa23b22b` (the pre-1.2 skip,
replaced by this path; its fallback-path check stays), `ba5f8943f` (timeline commands from the device).

**Host tests**: lavapipe behind `tests/vulkan_s22_shim.c`, which reproduces the phone's report:
apiVersion 1.1.128, the Vulkan1x structs ignored, core names NULL on the device, pc-0 stubs on the
instance.

| Test | Result |
| --- | --- |
| Vulkan report under the shim | completes; no core call below 1.2; KHR buffer address and timeline calls succeed; SPIR-V 1.3 and 1.5 pipelines created |
| Tiny MoE (`llama-simple`, all layers on the device) under the shim | runs on the GPU; output byte-identical to the unchanged 1.2+ path |
| `test-backend-ops -o MUL_MAT_ID` under the shim, with `shaderInt64`, integer dot, coopmat, BDA and host import hidden | 1194/1194 |
| `MUL_MAT` f16 x f32 with n>8 | fails the same way under the shim, on the 1.2+ path and with no shim at all (lavapipe's cooperative-matrix path); passes with coopmat hidden. The Android build compiles no coopmat shaders |

The full `test-backend-ops` result under the shim with coopmat hidden is in the PR.

**SPIR-V audit** (`scripts/spirv-audit.py` over the modules the Android build produces: each one's
header and its `OpCapability`/`OpExtension`):

- All 1190 modules of the Android build are **SPIR-V 1.5** (`glslc --target-env=vulkan1.2`). By the
  spec, a 1.1 device takes SPIR-V 1.3, or 1.4 with `VK_KHR_spirv_1_4`. Whether the Adreno driver takes
  1.5 below 1.2 was the open question; the phone's second report answers it: all four of its test
  pipelines (SPIR-V 1.3 and 1.5) were created.
- Capabilities (number of modules): StorageBuffer16BitAccess 984, StorageBuffer8BitAccess 711,
  Int8 564, Int16 398, Float16 344, GroupNonUniform 223, GroupNonUniformArithmetic 201, mixed float
  dot (VALVE) 170, Shuffle 18, Vote 7, Int64 4, PhysicalStorageBufferAddresses 4, Clustered 3,
  Ballot 2, ShaderClock 1, VulkanMemoryModel 1. Each one is gated by the feature it needs, read in
  step 1. Int64 and buffer addresses appear only in the four `_bda` variants, used only with both
  features. On Adreno, ggml already turns subgroup arithmetic off (a known driver issue).
- The NDK's glslc builds no cooperative-matrix or integer-dot shaders, so on the phone those paths
  are the scalar ones already.
- Rebuilt for SPIR-V 1.3 (`--target-env=vulkan1.1`), 161 of 162 sources compile. `im2col`
  (convolutions, not used by these LLMs) needs 1.4 for a specialization-constant conversion. So a 1.3
  shader set is a viable fallback if the phone refuses 1.5. It would double the shader binaries
  (about 35 MB more in the APK). The phone accepted 1.5, so it is not built.

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

1. Plan screen, "Devices": Vulkan0's status and reason. A probe that aborts now ends its reason with
   what the backend said (`it said: "..."`); that line is the next fix's input.
2. Plan screen, "Copy Vulkan report": the whole text, now with `limits` (the storage range among
   them), `limits2` and `memory-heaps`.
3. For a first-load failure: the error text shown, and whether the engine process was killed
   (`adb logcat | grep -i "lowmemorykiller\|lmkd"`) or ended with a signal.

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

`f7adfde`, `1aa23b22b`, `ba5f8943f`, `8644cb849` and `d486a5759` on `Eun-si123/llama.cpp`
`s22/vk-device-api-gate`, on top of the two upstream hooks (`docs/seam.md`). Unverified on an Adreno
730. No release or speed claim follows from these changes alone.
