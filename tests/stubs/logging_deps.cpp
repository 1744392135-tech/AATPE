#include "aatped.h"
#include <cerrno>
#include <sys/stat.h>

const char* LOG_FILE = "/tmp/aatpe_v94_state/aatpe_daemon.log";
const char* RUN_DIR  = "/tmp/aatpe_v94_state";

void mkdirIfNotExist(const char* path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
    }
}
