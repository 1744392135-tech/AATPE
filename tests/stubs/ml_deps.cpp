// ml_deps.cpp — minimal host stub for aatpe_ml.cpp's only external dependency.
//
// aatpe_ml.cpp declares `std::string aatpeExec(const std::string&, const char*)`
// at line 18 (defined in aatped.cpp on device). GfxInfoFrameTimeProvider and
// SurfaceFlingerLatencyProvider call it to run dumpsys/surfaceflinger commands.
// The ML unit tests never exercise those providers, but the symbols must still
// link. Provide a neutral empty-string implementation.
#include <string>
#include <vector>
std::string aatpeExec(const std::string& /*cmd*/, const char* /*tag*/) {
    return std::string();
}
// #8/#9: cold_start_rules now calls the shared argv exec. Neutral empty stub.
std::string aatpeExecArgv(const std::vector<std::string>& /*argv*/, const char* /*tag*/, int*, bool) {
    return std::string();
}
