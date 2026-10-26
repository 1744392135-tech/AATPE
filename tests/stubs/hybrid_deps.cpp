// hybrid_deps.cpp - host link deps for the 5 aatpe_ml_hybrid_*.cpp TUs.
// They form a single HybridLearner class; the only external they need beyond
// libc is the LOG_DEBUG sampling counter (normally in aatped_logging.cpp).
#include <atomic>
std::atomic<unsigned int> g_debugSampleCounter{0};

// F-002 (v4.3.4): aatpe_ml_hybrid_main.cpp now reads aatpe_ml::g_mlTrainingPausedByThermal
// (mirrors the main ML thermal pause gate). Production aatpe_ml.cpp defines it
// strongly. Host hybrid-only test targets do NOT link aatpe_ml.o, so provide a WEAK
// default here (never paused). Targets that DO link aatpe_ml.o (placement, config,
// control, ...) get the strong definition, which overrides this weak one.
namespace aatpe_ml {
__attribute__((weak)) std::atomic<bool> g_mlTrainingPausedByThermal{false};
}
