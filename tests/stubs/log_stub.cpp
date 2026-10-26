#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
std::atomic<int> g_logLevel{3};
void logMsg(int level, const char* fmt, ...){(void)level;(void)fmt;}
void logFlush(){} void logInit(){} void logReinit(){} void logShutdown(){}
// H8: webui_json.o's atomicWriteJson now logs rename failures via aatpeStrError.
// Real impl lives in aatped_logging.cpp; hermetic recipes that link webui_json.o
// but NOT aatped_logging.o (webui_json recipe) need this weak fallback.
#include <cstring>
__attribute__((weak)) const char* aatpeStrError(int err, char* buf, size_t n) {
    const char* s = strerror(err);
    if (buf && n) { snprintf(buf, n, "%s", s ? s : "unknown"); return buf; }
    return s ? s : "unknown";
}
// fix-v96 #8: host stub for the canonical module-dir resolver. The federated,
// ebpf, hmac and main recipes link only log_stub and need this default. It is
// WEAK so the `utils` recipe (which links the real aatped_utils.o strong
// definition) silently overrides it instead of failing to link with two strong
// definitions. Tests override PSK/config paths via env, so an empty module dir is
// harmless.
__attribute__((weak)) std::string getModuleDir() { return std::string(); }

// perf-fix: weak fallbacks for symbols that production TUs
// (aatpe_subsystem.o, aatpe_command_queue.o) reference but whose recipes do not
// link the corresponding stub. Strong definitions in main_deps / webui_deps /
// v94_daemon_deps / placement_deps silently override these when present.
#include <sys/stat.h>
__attribute__((weak)) bool writeFileAtomic(const char* /*path*/, const std::string& /*content*/) { return false; }
__attribute__((weak)) bool isValidControlChars(const std::string& /*line*/) { return true; }

// v94 stub-integration: uevent_monitor.o bumps g_ueventEventCount (real home is
// aatped_uevent.cpp:9). The uevent/event_monitor/main hermetic recipes link
// uevent_monitor.o but NOT aatped_uevent.o, so provide a weak definition here.
// The uevent_fallback recipe DOES link aatped_uevent.o (strong def), which
// silently overrides this weak one — no multiple-definition error.
__attribute__((weak)) std::atomic<int> g_ueventEventCount{0};

// ROUND-007: weak stub for crash-loop self-heal user-mode restore.
// Production impl lives in aatped_control.cpp; hermetic subsystem recipes
// (v342_round5 etc.) link only log_stub so need this weak fallback.
__attribute__((weak)) void restoreUserControlMode() {}

// v4.2 A3: aatpe_caps.o's autocal path references aatpe_ml::computeDeviceTopologyHash().
// Recipes that link aatpe_caps.o but NOT aatpe_ml.o (e.g. mech_uclamp) need a link
// fallback; recipes that DO link aatpe_ml.o (placement/v343_placement/main/thermal)
// pull the strong production definition, which silently overrides this weak one.
namespace aatpe_ml {
__attribute__((weak)) std::string computeDeviceTopologyHash() { return std::string(); }
}
