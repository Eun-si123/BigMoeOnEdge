# External research and implementation references

Maintained reference register for downstream stock Android compatibility. See [compatibility plan](s22-compatibility.md) for priorities and acceptance gates. This is a research backlog, not a list of integrated features. Evidence reviewed 2026-11-27; recheck source revisions and licenses before implementation.

## P0 — Vulkan and OpenCL compatibility

| Source | Evidence / limitations | Reuse candidate and validation |
| --- | --- | --- |
| [Box #109](https://github.com/jegly/Box/issues/109), [#121](https://github.com/jegly/Box/issues/121) | Developer disassembly attributes an Adreno 730 crash to a NULL buffer-address command; subsequent reports show incomplete coverage. | Compare symbolized S22 crash with this signature; resolve per-device commands; test missing-function paths and real GPU allocations. Initial downstream implementation: [f7adfde45](https://github.com/Eun-si123/llama.cpp/commit/f7adfde45ad19369e9dc5c485aa08302ac594532), not hardware-validated. |
| [llama.cpp PR #28593](https://github.com/ggml-org/llama.cpp/pull/28593) | Closed unmerged; reports success on S21 FE Mali, not S22 Adreno. | Investigate promoted Core/KHR command aliases, enabled extensions, timeline semaphore functions and per-device dispatch. Preserve minimum API policy and multi-GPU correctness; do not blindly transplant global dispatcher initialization. |
| [PocketPal PR #1049](https://github.com/a-ghorbani/pocketpal-ai/pull/1049) | Merged OpenCL integer-dot compatibility fix. | Compare pinned ggml kernel build AND dispatch paths against diff; exercise devices without integer-dot support and preserve optimized paths on supported devices. |
| [PocketPal #1110](https://github.com/a-ghorbani/pocketpal-ai/issues/1110) | Redmi K60 report: unsetting large-buffer opt-in fixed compiler failure; not proof of full stability. | Keep opt-in unset unless verified; test compile fallback without unsupported options and isolated failure recovery. |
| [Mesa Freedreno / Turnip](https://docs.mesa3d.org/drivers/freedreno.html), [source](https://gitlab.freedesktop.org/mesa/mesa) | Alternative Vulkan implementation, not a proven fix for this app. | Research app-local loading and redistribution terms; never replace stock system driver. Compare correctness and sustained performance before enabling. |

## P1 — CPU / NPU / heterogeneous execution

| Source | Evidence / limitations | Reuse candidate and validation |
| --- | --- | --- |
| [Snapdragon NPU LLM](https://github.com/avisre/snapdragon-npu-llm) | Author reports 31.3 decode tok/s for Qwen3-0.6B via ExecuTorch QNN on OnePlus 10 Pro (v69). Not S22 or large MoE validation. | Reproduce a small supported model on stock v69; inspect QNN ABI, converted model, firmware compatibility and unsupported operators. Do not assume arbitrary GGUF works. |
| [ExecuTorch](https://github.com/pytorch/executorch), [Qualcomm backend](https://docs.pytorch.org/executorch/stable/backends-qualcomm.html) | Separate inference runtime and model export path. | Inspect v69 target mapping, quantization constraints and CPU fallback; consider integration only after an isolated prototype. |
| [Snapdragon kernels](https://github.com/TriDefender/llama.cpp-snapdragon_kernels) | README lists libggml-htp-v69.so in nightly builds. ABI, provenance, license and real device performance need verification. | Inspect corresponding source and build reproducibly; no unverified binary transplant. Compare host and DSP API versions and actual op correctness. |
| [XNNPACK](https://github.com/google/XNNPACK), [KleidiAI](https://github.com/ARM-software/kleidiai) | ARM CPU optimization references; not measured in this fork. | Check existing ggml integration first, benchmark relevant quantized operators and thread counts; avoid parallel runtimes without evidence of benefit. |
| [MNN](https://github.com/alibaba/MNN) | Mobile inference framework with multiple GPU backends, separate model ecosystem. | Compare operator placement, memory management and CPU/GPU transfer costs; do not assume format compatibility. |

## P0/P2 — memory and storage streaming

| Source | Evidence / limitations | Reuse candidate and validation |
| --- | --- | --- |
| [Android LMKD](https://source.android.com/docs/core/perf/lmkd) | Platform memory pressure mechanism; not proof of this device's crash cause. | Distinguish process death from native faults, monitor load peak and cache headroom; test cold/warm starts and background pressure without privileged settings. |
| [Quick.AI](https://github.com/nntrainer/Quick.AI), [NNTrainer](https://github.com/nntrainer/nntrainer) | README reports ~1.3 GB RAM for Qwen3 30B FSU; NNTrainer advertises up to 120B with cached-slim expert loading. Measurement definitions and model formats may differ. | Compare expert streaming, hot-cache policies, reusable buffers and memory peaks against current engine using identical weights/quality where possible. |
| [PowerInfer-2 paper](https://arxiv.org/abs/2406.06282), [PowerInfer project](https://github.com/SJTU-IPADS/PowerInfer) | Project reports 11.68 tok/s for TurboSparse-Mixtral-47B on a phone; specialized sparse model/runtime, not directly comparable to ordinary GGUF. | Study neuron/expert locality, I/O scheduling, asynchronous transfer and heterogeneous scheduling; preserve lossless inference by default. |

## Research-to-implementation protocol

1. Read the relevant linked primary issue, patch, source, or paper before implementing. Pin the revision and record the original author/source, applicability, license, and unresolved assumptions in the change description.
2. Inspect the existing implementation first. Prefer a small compatible backport or reusing existing interfaces over a second runtime or duplicate feature. Maintain upstream hooks and keep backend changes on the separate dependency fork.
3. Treat publications, developer benchmarks, and binary releases as evidence to reproduce, not as validated results on S22. Do not import binaries with unverified provenance or code with unclear redistribution rights. Preserve required attribution and notices.
4. Run correctness and failure-path tests before speed tests; compare same model, quantization, context, prompt, output length, thermal conditions, and backend. Report prefill and decode separately, peak memory, I/O and sustained performance. Maintain CPU fallback and stock Android/no-root compatibility.
5. Update this register when a source is adopted, rejected, superseded or reproduced; record the commit/test evidence and remaining limitations. Add newly found research only when it can materially improve the product; do not endlessly expand the dependency list.
