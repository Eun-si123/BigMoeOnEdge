// The Vulkan report's contract, on whatever this machine has: no loader, a software driver, a GPU.
// What it must hold everywhere: it says it started, every line is "key: value", it ends either with
// "done" or with the line that says why it could not ask, and it never prints a filesystem path.

#include "vulkan_report.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char * what, const std::string & detail = "") {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failures;
}
} // namespace

int main() {
    std::vector<std::string> lines;
    const bool asked = bmoe::frontend::vulkan_report([&](const std::string & l) { lines.push_back(l); });
    check(!lines.empty() && (lines.front() == "vulkan.report: 1" || lines.front().rfind("vulkan: ", 0) == 0),
          "the report says it started, or why this build cannot ask", lines.empty() ? "" : lines.front());
    bool shaped = true, no_path = true;
    std::string bad;
    for (const std::string & l : lines) {
        if (l.find(": ") == std::string::npos) {
            shaped = false;
            bad = l;
        }
        // The loader's own name is printed, never a directory it came from or one of this machine's.
        if (l.find("/home/") != std::string::npos || l.find("/Users/") != std::string::npos ||
            l.find("/data/") != std::string::npos || l.find(":\\") != std::string::npos) {
            no_path = false;
            bad = l;
        }
    }
    check(shaped, "every line is key: value", bad);
    check(no_path, "no line carries a filesystem path", bad);
    if (asked)
        check(lines.back() == "vulkan.report: done", "a report that asked ends with done", lines.back());
    else
        check(lines.back().find("none") != std::string::npos || lines.back().find("no") != std::string::npos ||
                  lines.back().rfind("vulkan: ", 0) == 0,
              "a report that could not ask ends on why", lines.back());
    std::printf("%s (%zu lines, %s)\n", failures ? "FAILED" : "ok", lines.size(),
                asked ? "asked the driver" : "did not");
    return failures ? 1 : 0;
}
