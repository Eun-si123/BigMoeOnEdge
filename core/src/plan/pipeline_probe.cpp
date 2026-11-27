// The compute pipelines a backend's driver will not create, asked one at a time. See bmoe/probe.h.
//
// A driver's shader compiler can refuse one kernel among hundreds (the S22 Ultra's Adreno 730 ended
// its probe with createComputePipeline: ErrorUnknown on a q4_K mat-vec). The backend builds its
// pipelines lazily, so a probe finds the refused ones one crash at a time. This file asks for a family
// of them at once, through the entry point this engine's llama.cpp fork exports for it
// (ggml_backend_vk_check_pipelines); a backend without it is simply not asked.

#include "bmoe/probe.h"

#include "ggml-backend.h"

namespace bmoe {

namespace {

using check_cb = void (*)(const char * name, const char * status, void * user);
using check_fn = int (*)(size_t dev, const char * prefixes, check_cb cb, void * user);

} // namespace

bool check_backend_pipelines(const std::string & prefixes, const PipelineReport & report) {
    bool asked = false;
    for (size_t r = 0; r < ggml_backend_reg_count(); ++r) {
        ggml_backend_reg_t reg = ggml_backend_reg_get(r);
        const auto check = (check_fn) ggml_backend_reg_get_proc_address(reg, "ggml_backend_vk_check_pipelines");
        if (!check) continue;
        for (size_t d = 0; d < ggml_backend_reg_dev_count(reg); ++d) {
            const std::string device = ggml_backend_dev_name(ggml_backend_reg_dev_get(reg, d));
            struct Ctx {
                const PipelineReport * report;
                const std::string * device;
            } ctx{&report, &device};
            const int refused = check(
                d, prefixes.c_str(),
                [](const char * name, const char * status, void * user) {
                    const Ctx * c = static_cast<const Ctx *>(user);
                    (*c->report)(*c->device, name, status);
                },
                &ctx);
            asked = asked || refused >= 0;
        }
    }
    return asked;
}

} // namespace bmoe
