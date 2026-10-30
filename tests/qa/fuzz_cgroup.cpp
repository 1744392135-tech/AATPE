#include <cstdint>
#include <cstddef>
#include <string>
#include <cstring>

namespace aatpe_spillover { namespace aatpe { namespace mech {
}}}

static std::string parseCgroupLine(const std::string& line) {
    size_t p1 = line.find(':');
    if (p1 == std::string::npos) return "";
    size_t p2 = line.find(':', p1 + 1);
    if (p2 == std::string::npos) return "";
    std::string subsys = line.substr(p1 + 1, p2 - p1 - 1);
    if (subsys != "cpuset") return "";
    std::string path = line.substr(p2 + 1);
    if (!path.empty() && path[0] == '/') path = path.substr(1);
    return path;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    std::string line(reinterpret_cast<const char*>(data), size);

    try {
        std::string result = parseCgroupLine(line);
        (void)result.length();
    } catch (...) {
    }
    return 0;
}
