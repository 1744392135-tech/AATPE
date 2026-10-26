// logging_deps.cpp - host link deps for aatped_logging.cpp. logging.cpp defines
// logMsg/logInit/logReinit/... itself; it only needs LOG_FILE, RUN_DIR and
// mkdirIfNotExist (which live in aatped_utils.cpp on device).
#include "aatped.h"
#include <cerrno>
#include <sys/stat.h>

const char* LOG_FILE = "/tmp/aatpe_v94_state/aatpe_daemon.log";
const char* RUN_DIR  = "/tmp/aatpe_v94_state";

void mkdirIfNotExist(const char* path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        // host stub: ignore (no LOG_WARN available without logMsg recursion)
    }
}
