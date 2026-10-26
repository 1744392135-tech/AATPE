// fuzz_json.cpp — libFuzzer driver for the WebUI JSON string escaper.
//
// Target: jsonEscape(const std::string&) in webui_json.cpp.
// The escaper runs on every thread name / decision type / error string the
// daemon writes into the WebUI JSON files. Thread names come from /proc/<tid>/comm
// and can contain arbitrary bytes (including <0x20 control chars, stray UTF-8
// continuation bytes, and embedded quotes). The escaper must produce valid,
// balanced JSON escaping and must never OOB-read or hang on hostile input.
#include <cstdint>
#include <cstddef>
#include <string>

// Provided by webui_json.cpp (linked in via build_fuzz.sh).
std::string jsonEscape(const std::string& s);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string s(reinterpret_cast<const char*>(data), size);
    std::string out = jsonEscape(s);
    // Sanity invariants the escaper must always hold:
    //   * output length >= input length (escaping only adds chars)
    //   * output contains no raw control byte < 0x20 (all must be \uXXXX or
    //     the named escapes \n \r \t \" \\).
    if (out.size() < s.size()) __builtin_trap();
    for (char c : out) {
        // A control byte survives ONLY if it is one of the 4-char escape
        // sequences consumed; a raw <0x20 byte (other than the literal escape
        // chars we intentionally emit) is a bug.
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) __builtin_trap();
    }
    return 0;
}
