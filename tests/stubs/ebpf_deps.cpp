// ebpf_deps.cpp - host link deps for aatpe_ebpf.cpp.
// (g_debugSampleCounter is supplied by hybrid_deps to avoid a multiple-definition
// clash with the main recipe which links both.)
// fix-v96 #8: getModuleDir() is provided by log_stub (linked into this recipe).
#include "aatped.h"
#include "aatpe_subsystem.h"
#include <fstream>

// ROUND-004: host stub for the config reload hook registry. Production defines
// this in aatped_config.cpp; the host test never actually registers, it drives
// EbpfScheduler::onConfigReload() directly.
namespace aatpe { namespace config {
void registerReloadHook(const std::string&, std::function<void(const Config&)>) {}
}}

// File I/O stub (integrity.cpp needs writeFile)
__attribute__((weak)) const char* STATE_DIR = "/tmp/aatpe_ebpf_test";
__attribute__((weak)) bool writeFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}
// P3-2: aatpe_ebpf.cpp now calls writeFileAtomic() for the capability + hotplug
// state files. The ebpf / ebpf_classify recipes do NOT link aatped_utils.o, so
// provide a weak best-effort stub (plain truncating write; atomicity is not
// exercised on host). Recipes that DO link the real aatped_utils.o override this.
__attribute__((weak)) bool writeFileAtomic(const char* path, const std::string& content) {
    return writeFile(path, content);
}

// D2/D3 (v4.2): aatpe_ebpf.cpp's bpftool loader + hotplug guard now call the shared
// argv exec (aatpeExecArgv, 4-param post-D2) and mkdirIfNotExist. The `ebpf` /
// `ebpf_classify` recipes link aatpe_ebpf.o WITHOUT aatped_utils.o, so provide
// neutral WEAK stubs here. weak so that recipes which DO link the real aatped_utils.o
// (d3hotplug) or main_deps (main recipe) override them with the strong definition
// instead of a multiple-definition error.
#include <sys/stat.h>
__attribute__((weak))
std::string aatpeExecArgv(const std::vector<std::string>& /*argv*/, const char* /*tag*/,
                          int* /*outExitStatus*/, bool /*bypassCooldown*/) { return std::string(); }
__attribute__((weak))
void mkdirIfNotExist(const char* /*path*/, mode_t /*mode*/) {}
