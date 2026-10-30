#include <sys/system_properties.h>
#include <string.h>
#include <vector>
#include <string>
extern "C" int __system_property_get(const char* name, char* value) {
    if (!strcmp(name, "ro.build.version.sdk")) { strcpy(value, "30"); return 2; }
    if (!strcmp(name, "ro.build.version.release")) { strcpy(value, "12"); return 2; }
    value[0] = '\0'; return 0;
}

namespace aatpe_federated {
std::string exportModel(const std::vector<float>& w, float alpha, int samples) {
    std::string out;
    out.reserve(w.size() * 4 + 32);
    out += "FEDv1 ";
    out += std::to_string(w.size());
    out += " ";
    out += std::to_string(alpha);
    out += " ";
    out += std::to_string(samples);
    out += "\n";
    for (float v : w) {
        out += std::to_string(v);
        out += ",";
    }
    return out;
}
}
