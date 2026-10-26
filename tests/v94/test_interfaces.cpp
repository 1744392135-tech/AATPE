// test_interfaces.cpp - Direct unit tests for aatpe_interfaces.cpp
// Covers: Mock* contract objects, initInterfaces/cleanupInterfaces globals,
//         PollingEventSource, OnlineLearningModel<->LearningModel wrapper,
//         DefaultCpuMonitor defaults on missing sysfs.
#include <cstdio>
#include <string>
#include <vector>
#include "aatpe_interfaces.h"
#include "aatped.h"
using namespace aatpe_ml;
static int g_pass=0,g_fail=0;
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)
int main(){
    printf("=== Interfaces: Mock contract objects ===\n");
    // RC6 (C4): do NOT bind a reference to null (was
    // *reinterpret_cast<const ThreadInfo*>(0)) - UBSan flags "reference binding to
    // null". Use a valid, unrelated ThreadInfo; MockScorer ignores its argument.
    ThreadInfo dummyTi{};
    MockScorer ms; CHECK(ms.computeScore(0, dummyTi)==50, "MockScorer returns 50");
    CHECK(ms.name()==std::string("mock_scorer"), "MockScorer name");
    ThreadInfo ti{};
    MockPlacementDecider md;
    Config cfg{};
    Decision d = md.decidePlacement(ti, cfg);
    CHECK(d.targetCpu==0, "MockPlacementDecider targets cpu0");
    CHECK(d.reason==std::string("mock"), "MockPlacementDecider reason=mock");
    MockEventSource me; CHECK(me.init()==true && me.poll(10)==false, "MockEventSource init true / poll false");
    CHECK(me.name()==std::string("mock_event"), "MockEventSource name");
    MockMlModel mm; CHECK(mm.predict(0,{1,2,3})==0.0f, "MockMlModel predict 0");
    CHECK(mm.getPoolCount()==3, "MockMlModel poolCount 3");
    mm.update(0,{1,2,3},0.5f); mm.setWeights(0,{1.0f,2.0f}); // no-op, must not crash
    CHECK(mm.getWeights(0).empty(), "MockMlModel getWeights empty");
    MockCpuMonitor mc; CHECK(mc.getCpuCount()==8 && mc.getTemperature()==40, "MockCpuMonitor defaults");
    CHECK(mc.getCpuUtilization(0)==50.0f && mc.getCpuFrequency(0)==2000000, "MockCpuMonitor util/freq defaults");

    printf("=== Interfaces: init/cleanup wiring ===\n");
    initInterfaces();
    CHECK(g_scorer!=nullptr, "g_scorer wired");
    CHECK(g_placementDecider!=nullptr, "g_placementDecider wired");
    CHECK(g_cpuMonitor!=nullptr, "g_cpuMonitor wired");
    CHECK(g_eventSource!=nullptr, "g_eventSource wired");
    CHECK(g_mlModelInterface!=nullptr, "g_mlModelInterface wired");
    CHECK(std::string(g_scorer->name())=="default_scorer", "default scorer installed");
    CHECK(std::string(g_mlModelInterface->name())=="online_learning", "online learning installed");
    CHECK(g_eventSource->poll(5)==false, "PollingEventSource.poll always false (fallback)");
    cleanupInterfaces();
    CHECK(g_scorer==nullptr && g_placementDecider==nullptr && g_cpuMonitor==nullptr
          && g_eventSource==nullptr && g_mlModelInterface==nullptr, "cleanupInterfaces nulls all globals");

    printf("=== Interfaces: OnlineLearningModel wrapper over real LearningModel ===\n");
    OnlineLearningModel ol;
    CHECK(ol.getPoolCount()==g_mlModel.getPoolCount(), "wrapper getPoolCount delegates");
    std::vector<float> f = {1000.0f, 10.0f, 0.5f, 0.0f, 0.0f, 1.0f, 40.0f, 0.5f, 0.0f, 0.0f, 0,0,0,0};
    float p0 = ol.predict(0, f);
    bool fin = (p0>=0.0f);
    CHECK(fin, "wrapper predict(0) returns finite >=0");
    ol.update(0, f, 0.8f);
    std::vector<float> w = ol.getWeights(0);
    CHECK((int)w.size()==ML_WEIGHT_COUNT, "getWeights returns ML_WEIGHT_COUNT size");
    std::vector<float> wset(w.size(), 0.1f);
    ol.setWeights(0, wset);
    std::vector<float> w2 = ol.getWeights(0);
    bool allSet = true;
    for (float x : w2) if (x < 0.099f || x > 0.101f) allSet = false;
    CHECK(allSet, "setWeights round-trip via per-pool model");

    printf("=== Interfaces: DefaultCpuMonitor host fallbacks ===\n");
    DefaultCpuMonitor dcm;
    CHECK(dcm.getCpuCount()==g_cpuCount, "DefaultCpuMonitor::getCpuCount = g_cpuCount");
    CHECK(dcm.getTemperature()==40, "DefaultCpuMonitor temp fallback 40 (no sysfs on host)");
    CHECK(dcm.getCpuFrequency(9)==2000000, "DefaultCpuMonitor freq fallback on unreadable sysfs");

    printf("\n========================================\nInterfaces results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail==0?0:1;
}
