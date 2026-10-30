#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_utils_state";

std::vector<int> g_allCpus{0, 1, 2, 3, 4, 5, 6, 7};

std::vector<int> getOnlineCpus() {
    return g_allCpus;
}
