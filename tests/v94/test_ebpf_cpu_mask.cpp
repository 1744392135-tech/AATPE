#include <cstdio>
#include <string>
#include <vector>

namespace aatpe_ebpf {
std::vector<int> parseCpuOnlineMask(const std::string& text);
}

using namespace aatpe_ebpf;

static int g_pass = 0, g_fail = 0;

static void checkVec(const std::vector<int>& actual,
                     const std::vector<int>& expected,
                     const char* msg) {
    if (actual == expected) {
        g_pass++;
        printf("  [PASS] %s\n", msg);
    } else {
        g_fail++;
        printf("  [FAIL] %s (got [", msg);
        for (size_t i = 0; i < actual.size(); i++)
            printf("%d%s", actual[i], i+1<actual.size()?", ":"");
        printf("], expected [");
        for (size_t i = 0; i < expected.size(); i++)
            printf("%d%s", expected[i], i+1<expected.size()?", ":"");
        printf("])\n");
    }
}

int main() {
    printf("=== #24 parseCpuOnlineMask unit tests ===\n");

    checkVec(parseCpuOnlineMask("0-7"),     {0,1,2,3,4,5,6,7}, "0-7 -> 8 contiguous");
    checkVec(parseCpuOnlineMask("0-1,4-5"), {0,1,4,5},          "0-1,4-5 -> sparse");
    checkVec(parseCpuOnlineMask("2"),       {2},                "2 -> single id");
    checkVec(parseCpuOnlineMask(""),        {},                 "empty -> empty");
    checkVec(parseCpuOnlineMask("0-3\n"),  {0,1,2,3},          "0-3 newline -> range");
    checkVec(parseCpuOnlineMask("0-1, 7"), {0,1,7},            "0-1, 7 -> space after comma");
    checkVec(parseCpuOnlineMask("5-3"),      {3,4,5},            "5-3 -> reversed normalized");
    checkVec(parseCpuOnlineMask("0,3,7"),   {0,3,7},            "0,3,7 -> three singles");

    printf("\n=== #24 aggregation simulation (sparse topology) ===\n");
    uint64_t percpu[8] = {10, 20, 0, 0, 40, 50, 0, 0};
    std::vector<int> online = parseCpuOnlineMask("0-1,4-5");
    std::vector<uint64_t> aggregated;
    for (int cpuId : online) {
        if (cpuId >= 0 && cpuId < 8) aggregated.push_back(percpu[cpuId]);
    }
    bool ok = (aggregated.size() == 4 &&
               aggregated[0] == 10 && aggregated[1] == 20 &&
               aggregated[2] == 40 && aggregated[3] == 50);
    if (ok) { g_pass++; printf("  [PASS] aggregation picks online slots 0,1,4,5\n"); }
    else {
        g_fail++;
        printf("  [FAIL] aggregation wrong:");
        for (auto v : aggregated) printf(" %llu", (unsigned long long)v);
        printf("\n");
    }

    printf("\n========================================\n"
           "cpu mask parser: %d passed, %d failed\n========================================\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
