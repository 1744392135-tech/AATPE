#include "it_common.h"

extern void scenario1_daemon_loop();
extern void scenario2_ebpf_events();
extern void scenario3_ml_loop();
extern void scenario4_thermal();
extern void scenario5_config_reload();
extern void scenario6_webui_http();
extern void scenario7_watchdog();
extern void scenario8_classification();
extern void scenario9_federated();
extern void scenario10_recovery();
extern void scenario11_reload_hook();
extern void scenario12_topology_clusters();
extern void scenario13_control_modes();
extern void scenario14_prime_override();
extern void scenario15_sched_mech();
extern void scenario16_topology_resilience();
extern void scenario17_state_contract();
extern void scenario18_thread_utf8();
extern void scenario19_cluster_pressure();
extern void scenario20_rt_prune_starttime();
extern void scenario21_shutdown_restore();
extern void scenario22_emergency_gpu();
extern void scenario23_timestats_degrade();
extern void scenario24_schema_cases();
extern void scenario25_thread_storm();
extern void scenario26_fg_switch_storm();
extern void scenario27_fg_bg_fg_repromote();
extern void scenario28_spillover_demote_starttime_guard();

static const ScenarioDef kScenarios[] = {
    {"S1_DaemonMainLoop",    scenario1_daemon_loop},
    {"S2_EbpfEvents",        scenario2_ebpf_events},
    {"S3_MLLearningLoop",    scenario3_ml_loop},
    {"S4_ThermalStateMachine", scenario4_thermal},
    {"S5_ConfigHotReload",   scenario5_config_reload},
    {"S6_WebUIHttp",         scenario6_webui_http},
    {"S7_Watchdog",          scenario7_watchdog},
    {"S8_ThreadClassification", scenario8_classification},
    {"S9_Federated",         scenario9_federated},
    {"S10_ExceptionRecovery", scenario10_recovery},
    {"S11_ReloadHookChain",  scenario11_reload_hook},
    {"S12_TopologyClusters",  scenario12_topology_clusters},
    {"S13_ControlModes",     scenario13_control_modes},
    {"S14_PrimePoolOverride", scenario14_prime_override},
    {"S15_SchedMechanism",    scenario15_sched_mech},
    {"S16_TopologyResilience", scenario16_topology_resilience},
    {"S17_StateContract",    scenario17_state_contract},
    {"S18_ThreadUtf8",        scenario18_thread_utf8},
    {"S19_ClusterPressure",   scenario19_cluster_pressure},
    {"S20_RtPruneStarttime",   scenario20_rt_prune_starttime},
    {"S21_ShutdownRestore",    scenario21_shutdown_restore},
    {"S22_EmergencyGpu",       scenario22_emergency_gpu},
    {"S23_TimestatsDegrade",   scenario23_timestats_degrade},
    {"S24_SchemaCases",        scenario24_schema_cases},
    {"S25_ThreadStorm",        scenario25_thread_storm},
    {"S26_FgSwitchStorm",      scenario26_fg_switch_storm},
    {"S27_FgBgFgRepromote",    scenario27_fg_bg_fg_repromote},
    {"S28_SpilloverDemoteStarttimeGuard", scenario28_spillover_demote_starttime_guard},
};

int main() {
    printf("===============================================================\n");
    printf(" AATPE v94 End-to-End Integration Test Harness\n");
    printf(" Linking REAL production objects; mock /sys,/proc via namespace\n");
    printf("===============================================================\n");

    g_it.mockRoot = getenv("AATPE_IT_ROOT") ? getenv("AATPE_IT_ROOT") : "/tmp/aatpe_it_root";

    aatpe_state_setDwellMsForTest(0);

    int ran = 0;
    const char* only = getenv("AATPE_ONLY");
    for (const auto& s : kScenarios) {
        if (only && *only) {
            std::string o(only);
            if (o.find(s.name) == std::string::npos) continue;
        }
        printf("\n>>> Running %s\n", s.name);
        itResetToFreshState();
        itTeardownScenario();
        size_t before = g_it.points.size();
        try {
            s.fn();
        } catch (const std::exception& ex) {
            printf("      [FATAL] scenario threw: %s\n", ex.what());
            g_it.points.push_back({s.name, "scenario_completed", false, ex.what()});
        } catch (...) {
            printf("      [FATAL] scenario threw unknown\n");
            g_it.points.push_back({s.name, "scenario_completed", false, "unknown"});
        }
        size_t after = g_it.points.size();
        int sp = 0, sf = 0;
        for (size_t i = before; i < after; i++) {
            if (g_it.points[i].pass) sp++; else sf++;
        }
        printf("<<< %s : %d passed, %d failed\n", s.name, sp, sf);
        ran++;
    }

    printf("\n\n================ INTEGRATION COVERAGE TABLE ================\n");
    printf("%-26s %6s %6s %6s %8s\n", "Scenario", "Total", "Pass", "Fail", "Cov%");
    int grandTotal = 0, grandPass = 0;
    std::map<std::string, std::array<int,3>> by;
    for (auto& p : g_it.points) {
        by[p.scenario][0]++;
        if (p.pass) by[p.scenario][1]++; else by[p.scenario][2]++;
    }
    for (auto& kv : by) {
        int tot = kv.second[0], pp = kv.second[1], ff = kv.second[2];
        double cov = tot ? 100.0 * pp / tot : 0.0;
        printf("%-26s %6d %6d %6d %7.1f%%\n", kv.first.c_str(), tot, pp, ff, cov);
        grandTotal += tot; grandPass += pp;
    }
    double grandCov = grandTotal ? 100.0 * grandPass / grandTotal : 0.0;
    printf("-----------------------------------------------------------\n");
    printf("%-26s %6d %6d %6d %7.1f%%\n", "TOTAL", grandTotal, grandPass,
           grandTotal - grandPass, grandCov);
    printf("===========================================================\n");

    FILE* rep = fopen((g_it.mockRoot + "/it_result.txt").c_str(), "w");
    if (rep) {
        fprintf(rep, "TOTAL_POINTS=%d\nPASSED=%d\nFAILED=%d\nCOV=%.2f\n",
                grandTotal, grandPass, grandTotal-grandPass, grandCov);
        for (auto& p : g_it.points) {
            fprintf(rep, "PT|%s|%s|%d|%s\n", p.scenario.c_str(), p.name.c_str(),
                    (int)p.pass, p.detail.c_str());
        }
        fclose(rep);
    }
    FILE* tl = fopen((g_it.mockRoot + "/it_timeline.txt").c_str(), "w");
    if (tl) {
        for (auto& e : g_it.timeline) fprintf(tl, "%lld %s\n", e.first, e.second.c_str());
        fclose(tl);
    }

    printf("\nResult: %s\n", (grandPass == grandTotal) ? "ALL INTEGRATION SCENARIOS PASSED"
                                 : "SOME POINTS FAILED");
    return (grandPass == grandTotal) ? 0 : 1;
}
