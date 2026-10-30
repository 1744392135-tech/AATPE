#include <cstdint>
#include <cstddef>
#include <string>

std::string jsonEscape(const std::string& s);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string s(reinterpret_cast<const char*>(data), size);
    std::string out = jsonEscape(s);
    if (out.size() < s.size()) __builtin_trap();
    for (char c : out) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) __builtin_trap();
    }
    return 0;
}
