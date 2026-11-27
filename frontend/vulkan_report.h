// What this machine's Vulkan stack says about itself, as text a person can copy into a report.
//
// The Vulkan backend is ggml's, and it reaches the driver in ways that can end the process with no
// error to catch: a phone's driver advertises a feature, and the command behind it resolves to NULL.
// The isolated device probe contains that crash but cannot say which command it was. This report
// asks the same questions directly, one step at a time, without loading any ggml backend:
//
//   - the loader's version, and each physical device's own API and driver version;
//   - the features and device extensions the backend relies on;
//   - every 1.2-core command the backend calls, and its KHR alias, resolved both through
//     vkGetInstanceProcAddr (how ggml's dispatcher resolves them) and vkGetDeviceProcAddr, on a device
//     created the way ggml creates it, and again with the KHR extensions enabled where offered;
//   - the calls themselves (a buffer's device address, a timeline semaphore), made only through a
//     pointer that resolved.
//
// Each line is written and flushed as it is known, so a driver that ends the process still leaves
// every line before the step that did it. Only values the Vulkan API returns are printed: no path,
// no model, nothing about the person.
#pragma once

#include <functional>
#include <string>

namespace bmoe::frontend {

// Runs the report, handing each line to `emit` as soon as it is known. Returns false where this
// build or this machine has no Vulkan loader (the report then says why, in one line).
bool vulkan_report(const std::function<void(const std::string & line)> & emit);

// The prefix a child process puts before each report line, so a parent can keep exactly the report's
// lines out of whatever else shares the stream.
extern const char * const vulkan_report_marker;

} // namespace bmoe::frontend
