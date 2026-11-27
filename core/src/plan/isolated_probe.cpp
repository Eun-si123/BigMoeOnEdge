// Probing a device in another process, so that a driver which ends the process ends that one.
// See bmoe/probe.h for the contract; this file is the text format and the two sides of the pipe.

#include "bmoe/probe.h"

#include "../io/platform_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace bmoe {

const char * const device_facts_marker = "BMOE_DEVICE_FACTS";
const char * const probe_stage_marker = "BMOE_PROBE_STAGE";

namespace {

char tri_char(Tri t) {
    return t == Tri::Yes ? 'Y' : t == Tri::No ? 'N' : 'U';
}

bool tri_from(const std::string & v, Tri & out) {
    if (v == "Y")
        out = Tri::Yes;
    else if (v == "N")
        out = Tri::No;
    else if (v == "U")
        out = Tri::Unknown;
    else
        return false;
    return true;
}

// How many fields a complete line carries. A line with fewer is a child that died while printing
// or a different build's idea of the format, and neither may be half-applied.
constexpr int k_fields = 20;

// The steps of probe_one_device, as the child prints them and the parent describes them.
struct Stage {
    const char * id;
    const char * says;
};
constexpr Stage k_stages[] = {
    {"devices", "while the backends listed their devices"},
    {"model", "while the model file was read"},
    {"support", "while the device was asked which operations it runs (its first buffers)"},
    {"bandwidth", "while the device's memory rate was measured"},
    {"costs", "while the device's transfer and prompt costs were measured"},
};

void enter_stage(const char * id) {
    // stderr is unbuffered, but the parent reads one pipe for both streams: say it now.
    std::fprintf(stderr, "%s %s\n", probe_stage_marker, id);
    std::fflush(stderr);
}

} // namespace

std::string last_probe_stage(const std::string & out) {
    const std::string m = std::string(probe_stage_marker) + " ";
    const char * says = "";
    for (size_t at = out.find(m); at != std::string::npos; at = out.find(m, at + 1)) {
        const size_t b = at + m.size();
        const size_t e = out.find_first_of("\r\n", b);
        const std::string id = out.substr(b, e == std::string::npos ? std::string::npos : e - b);
        for (const Stage & s : k_stages)
            if (id == s.id) says = s.says;
    }
    return says;
}

namespace {

bool is_source_line(const std::string & l) {
    // "<path>/ggml-vulkan.cpp:5455: message": a source file, a line number, then the message.
    const size_t colon = l.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    const std::string file = l.substr(0, colon);
    if (file.find(' ') != std::string::npos) return false;
    static const char * const exts[] = {".c", ".cpp", ".cc", ".h", ".hpp", ".m", ".mm", ".cu"};
    bool source = false;
    for (const char * e : exts) {
        const size_t n = std::strlen(e);
        source = source || (file.size() > n && file.compare(file.size() - n, n, e) == 0);
    }
    size_t i = colon + 1;
    const size_t digits = i;
    while (i < l.size() && l[i] >= '0' && l[i] <= '9')
        ++i;
    return source && i > digits && i < l.size() && l[i] == ':';
}

std::string sanitized(const std::string & l) {
    std::string out;
    std::istringstream words(l);
    std::string w;
    while (words >> w) {
        const size_t slash = w.find_last_of("/\\");
        if (slash != std::string::npos) w = w.substr(slash + 1);
        for (char & c : w)
            if ((unsigned char) c < 0x20 || (unsigned char) c > 0x7e) c = '?';
        out += (out.empty() ? "" : " ") + w;
    }
    constexpr size_t k_max = 360;
    if (out.size() > k_max) out = out.substr(0, k_max) + "...";
    return out;
}

} // namespace

std::string probe_last_words(const std::string & out) {
    std::vector<std::string> lines;
    std::istringstream in(out);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    // ggml-vulkan names the pipeline, module or layout it could not create on the line before its
    // exception ends the process: kept, so the reason says which shader the driver refused.
    std::string named;
    for (const std::string & s : lines)
        if (s.rfind("ggml_vulkan: ", 0) == 0 && s.find(" creation failed for ") != std::string::npos) named = s;
    std::string said;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string & s = lines[i];
        if (s.find("libc++abi: terminating") != std::string::npos || s.find("terminate called") != std::string::npos ||
            s.find("GGML_ASSERT(") != std::string::npos || is_source_line(s))
            said = s;
        else if (s.find("what():") != std::string::npos && !said.empty() && i > 0 &&
                 lines[i - 1].find("terminate called") != std::string::npos)
            said = lines[i - 1] + " " + s;
    }
    if (!named.empty()) said = said.empty() ? named : named + " | " + said;
    return said.empty() ? "" : sanitized(said);
}

const char * signal_name(int sig) {
    switch (sig) {
    case 4:
        return "SIGILL";
    case 6:
        return "SIGABRT";
    case 7:
        return "SIGBUS";
    case 8:
        return "SIGFPE";
    case 9:
        return "SIGKILL";
    case 11:
        return "SIGSEGV";
    default:
        return "";
    }
}

std::string device_facts_to_text(const ComputeDevice & d) {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "v=3 shared=%c hostbuf=%c hostptr=%c async=%c rebind=%d runs=%c repack=%c bw=%.6g ident=%c "
                  "hpok=%c split=%.9g splits=%u wide=%.6g wideok=%c locked=%c up=%.6g uphb=%.6g expert=%.6g dense=%.6g "
                  "hop=%.9g",
                  tri_char(d.shares_host_memory), tri_char(d.host_buffer), tri_char(d.host_ptr_buffers),
                  tri_char(d.async_copies), d.rebindable ? 1 : 0, tri_char(d.runs_expert_op), tri_char(d.needs_repack),
                  d.memory_bandwidth_gibs, tri_char(d.identity_ok), tri_char(d.host_ptr_verified), d.split_seconds,
                  (unsigned) d.graph_splits, d.wide_matmul_gibs, tri_char(d.wide_identity_ok),
                  tri_char(d.buffers_locked), d.upload_gibs, d.upload_host_buffer_gibs, d.expert_gibs, d.dense_gibs,
                  d.host_hop_seconds);
    return b;
}

bool device_facts_from_text(const std::string & text, ComputeDevice & d) {
    ComputeDevice n = d; // applied whole or not at all
    std::istringstream in(text);
    std::string kv;
    int seen = 0;
    bool versioned = false;
    while (in >> kv) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        bool ok = true;
        if (k == "v") {
            versioned = v == "3"; // the same binary on both ends: a mismatch is noise
            continue;
        } else if (k == "shared")
            ok = tri_from(v, n.shares_host_memory);
        else if (k == "hostbuf")
            ok = tri_from(v, n.host_buffer);
        else if (k == "hostptr")
            ok = tri_from(v, n.host_ptr_buffers);
        else if (k == "async")
            ok = tri_from(v, n.async_copies);
        else if (k == "rebind")
            n.rebindable = v == "1";
        else if (k == "runs")
            ok = tri_from(v, n.runs_expert_op);
        else if (k == "repack")
            ok = tri_from(v, n.needs_repack);
        else if (k == "bw")
            n.memory_bandwidth_gibs = std::strtod(v.c_str(), nullptr);
        else if (k == "ident")
            ok = tri_from(v, n.identity_ok);
        else if (k == "hpok")
            ok = tri_from(v, n.host_ptr_verified);
        else if (k == "split")
            n.split_seconds = std::strtod(v.c_str(), nullptr);
        else if (k == "splits")
            n.graph_splits = (uint32_t) std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "wide")
            n.wide_matmul_gibs = std::strtod(v.c_str(), nullptr);
        else if (k == "wideok")
            ok = tri_from(v, n.wide_identity_ok);
        else if (k == "locked")
            ok = tri_from(v, n.buffers_locked);
        else if (k == "up")
            n.upload_gibs = std::strtod(v.c_str(), nullptr);
        else if (k == "uphb")
            n.upload_host_buffer_gibs = std::strtod(v.c_str(), nullptr);
        else if (k == "hop")
            n.host_hop_seconds = std::strtod(v.c_str(), nullptr);
        else if (k == "expert")
            n.expert_gibs = std::strtod(v.c_str(), nullptr);
        else if (k == "dense")
            n.dense_gibs = std::strtod(v.c_str(), nullptr);
        else
            return false;
        if (!ok) return false;
        ++seen;
    }
    if (!versioned || seen != k_fields) return false;
    d = n;
    return true;
}

void probe_devices_isolated(HardwareProfile & hw, const DeviceProbeRunner & run) {
    if (!run) return;
    for (ComputeDevice & d : hw.devices) {
        // The host is this process: not something a second process could try on its behalf.
        if (d.is_cpu) continue;
        std::string facts, why;
        d.probed_out_of_process = true;
        if (run(d.name, facts, why) && device_facts_from_text(facts, d)) {
            d.usable = Tri::Yes;
        } else {
            d.usable = Tri::No;
            d.unusable_reason = why.empty() ? "its probe returned nothing that could be read" : why;
        }
    }
}

bool probe_one_device(const char * model_path, const std::string & device, std::string & facts) {
    enter_stage("devices");
    HardwareProfile hw = probe_hardware(nullptr);
    bool found = false;
    std::vector<ComputeDevice> keep;
    for (ComputeDevice & d : hw.devices) {
        if (d.is_cpu) {
            keep.push_back(d); // the reference every device is judged against
        } else if (d.name == device) {
            keep.push_back(d);
            found = true;
        }
    }
    if (!found) return false;
    hw.devices = std::move(keep);
    enter_stage("model");
    const ModelProfile mp = probe_model(model_path, probe_engine_layers(model_path));
    enter_stage("support");
    probe_device_support(hw, mp);
    enter_stage("bandwidth");
    probe_bandwidth(hw, mp);
    enter_stage("costs");
    probe_device_costs(hw, mp);
    for (const ComputeDevice & d : hw.devices)
        if (!d.is_cpu) facts = device_facts_to_text(d);
    return true;
}

DeviceProbeRunner self_process_runner(std::function<std::vector<std::string>(const std::string & device)> argv_for,
                                      double timeout_seconds) {
    if (!pio::can_run_self()) return nullptr;
    return [argv_for, timeout_seconds](const std::string & device, std::string & facts, std::string & why) {
        std::string out;
        const pio::ChildResult r = pio::run_self(argv_for(device), timeout_seconds, &out);
        const std::string stage = last_probe_stage(out);
        const std::string words = probe_last_words(out);
        const std::string where =
            (stage.empty() ? "" : ", " + stage) + (words.empty() ? "" : "; it said: \"" + words + "\"");
        if (r.outcome == pio::ChildOutcome::TimedOut) {
            char b[96];
            std::snprintf(b, sizeof(b), "its probe did not finish in %.0f s and was stopped", timeout_seconds);
            why = b + where;
            return false;
        }
        if (r.outcome == pio::ChildOutcome::Signalled) {
            const char * name = signal_name(r.code);
            why = "its probe ended the process that ran it (signal " + std::to_string(r.code) +
                  (*name ? std::string(", ") + name : std::string()) + ")" + where;
            return false;
        }
        if (r.outcome != pio::ChildOutcome::Exited || r.code != 0) {
            why = r.outcome == pio::ChildOutcome::Exited
                      ? "its probe failed (exit " + std::to_string(r.code) + ")" + where
                      : "a second process could not be started to probe it";
            return false;
        }
        // The line after the marker; a backend prints what it likes on the same stream.
        const size_t at = out.rfind(device_facts_marker);
        if (at == std::string::npos) {
            why = "its probe finished without reporting";
            return false;
        }
        size_t b0 = at + std::strlen(device_facts_marker);
        size_t e0 = out.find('\n', b0);
        facts = out.substr(b0, e0 == std::string::npos ? std::string::npos : e0 - b0);
        return true;
    };
}

bool run_self_child(const std::vector<std::string> & args,
                    double timeout_seconds,
                    std::string & out,
                    std::string & why) {
    why.clear();
    if (!pio::can_run_self()) {
        why = "this platform cannot start a second process, so it was not run";
        return false;
    }
    const pio::ChildResult r = pio::run_self(args, timeout_seconds, &out);
    if (r.outcome == pio::ChildOutcome::TimedOut) {
        char b[96];
        std::snprintf(b, sizeof(b), "it did not finish in %.0f s and was stopped", timeout_seconds);
        why = b;
    } else if (r.outcome == pio::ChildOutcome::Signalled) {
        const char * name = signal_name(r.code);
        why = "it ended with signal " + std::to_string(r.code) + (*name ? std::string(", ") + name : std::string());
    } else if (r.outcome != pio::ChildOutcome::Exited) {
        why = "a second process could not be started";
        return false;
    } else if (r.code != 0) {
        why = "it exited with " + std::to_string(r.code);
    }
    return true;
}

} // namespace bmoe
