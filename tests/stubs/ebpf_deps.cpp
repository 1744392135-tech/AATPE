#include "aatped.h"
#include "aatpe_subsystem.h"
#include <fstream>

namespace aatpe { namespace config {
void registerReloadHook(const std::string&, std::function<void(const Config&)>) {}
}}

__attribute__((weak)) const char* STATE_DIR = "/tmp/aatpe_ebpf_test";
__attribute__((weak)) bool writeFile(const char* path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << content;
    return true;
}
__attribute__((weak)) bool writeFileAtomic(const char* path, const std::string& content) {
    return writeFile(path, content);
}

#include <sys/stat.h>
__attribute__((weak))
std::string aatpeExecArgv(const std::vector<std::string>& , const char* ,
                          int* , bool ) { return std::string(); }
__attribute__((weak))
void mkdirIfNotExist(const char* , mode_t ) {}
