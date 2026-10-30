#ifndef AATPE_TEST_FEDERATED_WIRE_STUB_H
#define AATPE_TEST_FEDERATED_WIRE_STUB_H
#include <string>
#include <vector>

namespace aatpe_federated {

std::string exportModel(const std::vector<float>& weights, float alpha, int sampleCount);

}
#endif
