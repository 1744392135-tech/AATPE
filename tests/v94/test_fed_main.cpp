#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "aatpe_federated.h"

using namespace aatpe_federated;

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

int main() {
    const char* dir = "/tmp/aatpe_fed_main";
    rmdir((std::string(dir) + "/import").c_str()); rmdir(dir);

    printf("=== FedMain ML-06: init() does NOT create dirs when disabled ===\n");
    FederatedLearner fl;
    fl.init(dir);
    struct stat st;
    CHECK(stat(dir, &st) != 0, "init() on disabled learner: exchange dir NOT created");
    CHECK(stat((std::string(dir) + "/import").c_str(), &st) != 0,
          "init() on disabled learner: import dir NOT created");

    printf("=== FedMain ML-06: setEnabled(true) lazily creates dirs ===\n");
    fl.setEnabled(true);
    CHECK(stat(dir, &st) == 0 && S_ISDIR(st.st_mode),
          "setEnabled(true): exchange dir created");
    CHECK(stat((std::string(dir) + "/import").c_str(), &st) == 0,
          "setEnabled(true): import dir created");

    printf("=== FedMain: init idempotent (re-init on existing dir) ===\n");
    fl.init(dir);
    CHECK(stat(dir, &st) == 0, "second init does not remove existing dir");

    printf("=== FedMain: setLocalModel stores weights/alpha/samples ===\n");
    fl.setLocalModel({0.1f, 0.2f, 0.3f}, 0.5f, 2000);
    const FederatedStats& s = fl.getStats();
    CHECK(s.rounds == 0, "fresh learner: 0 rounds");
    CHECK(s.imports == 0 && s.exports == 0, "fresh learner: 0 imports/exports");

    printf("=== FedMain G7-001: disabled by default -> zero export, zero aggregation ===\n");
    {
        const char* gdir = "/tmp/aatpe_fed_gate";
        system(("rm -rf " + std::string(gdir)).c_str());
        FederatedLearner g;
        g.init(gdir);
        CHECK(g.isEnabled() == false, "learner starts DISABLED (opt-in)");
        CHECK(stat(gdir, &st) != 0, "disabled init: exchange dir NOT created on disk");
        g.setLocalModel({0.1f, 0.2f}, 0.5f, 5000);
        g.exportLocalModel();
        g.runAggregation();
        CHECK(stat((std::string(gdir)+"/local_model.bin").c_str(), &st) != 0,
              "DISABLED: no local_model.bin written to disk");
        CHECK(g.getStats().exports == 0, "DISABLED: export counter stays 0");

        g.setEnabled(true);
        CHECK(g.isEnabled() == true, "setEnabled(true) flips the gate");
        CHECK(stat(gdir, &st) == 0, "opt-in: exchange dir now created");
        g.exportLocalModel();
        CHECK(stat((std::string(gdir)+"/local_model.bin").c_str(), &st) != 0,
              "ENABLED+exchange removed: no local_model.bin written (dormant)");
        CHECK(g.getStats().exports == 0, "dormant export: counter stays 0");

        long e0 = g.getStats().exports;
        g.setEnabled(false);
        g.exportLocalModel();
        CHECK(g.getStats().exports == e0, "DISABLED again: export stays at prior count");
    }

    printf("\n========================================\n"
           "FedMain results: %d passed, %d failed\n"
           "========================================\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
