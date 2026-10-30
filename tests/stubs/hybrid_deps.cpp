#include <atomic>
std::atomic<unsigned int> g_debugSampleCounter{0};

namespace aatpe_ml {
__attribute__((weak)) std::atomic<bool> g_mlTrainingPausedByThermal{false};
}
