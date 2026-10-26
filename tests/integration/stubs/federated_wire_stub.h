// federated_wire_stub.h — TEST-ONLY header declaring the dormant federated
// wire-format free functions that were REMOVED from the production header
// (aatpe_federated.h) in v4.3.4 F-group governance (F-003).
//
// These functions have NO production implementation; only
// tests/integration/stubs/stub_property.cpp provides a minimal host stub for
// the integration S9 scenario. Production code MUST NOT include this header or
// call these functions — the v2 export/import path is dormant (ROUND-014
// Plan B) and exercised solely by integration tests through their own stub.
#ifndef AATPE_TEST_FEDERATED_WIRE_STUB_H
#define AATPE_TEST_FEDERATED_WIRE_STUB_H
#include <string>
#include <vector>

namespace aatpe_federated {

// Minimal 3-arg serializer. Implemented in tests/integration/stubs/stub_property.cpp.
// The legacy v1/v2 blob parsers (importModel/importModelEx/exportModelEx) are not
// implemented anywhere in the production or stub tree and are intentionally NOT
// declared here — integration tests do not reference them.
std::string exportModel(const std::vector<float>& weights, float alpha, int sampleCount);

} // namespace aatpe_federated
#endif
