// test_probe.cpp - Direct unit tests for aatpe-probe.cpp helpers.
#include <cstdio>
#include <cstdlib>
#include <string>
#define main probe_main_unused
#include "aatpe-probe.cpp"
#undef main
extern int g_pass, g_fail; // reuse probe.cpp counters
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)
int main(){
    printf("=== Probe: color ===\n");
    g_useColor = false;
    CHECK(std::string(color("x"))=="", "color disabled -> empty");
    g_useColor = true;
    CHECK(std::string(color("x"))=="x", "color enabled -> passthrough");

    printf("=== Probe: readFile/readInt ===\n");
    FILE* fp=fopen("/tmp/probe_test.txt","w"); fputs("  777  \n",fp); fclose(fp);
    std::string s=readFile("/tmp/probe_test.txt");
    CHECK(s=="777", "readFile trims");
    CHECK(readInt("/tmp/probe_test.txt",0)==777, "readInt 777");
    CHECK(readInt("/tmp/nope", 42)==42, "readInt missing -> default");

    printf("=== Probe: probeItem classifies result strings ===\n");
    int p0=g_pass, w0=g_warn, f0=g_fail;
    probeItem("TEST","item","OK","","","");
    CHECK(g_pass==p0+1, "probeItem 'OK' increments g_pass");
    probeItem("TEST","item","WARN","r","a","d");
    CHECK(g_warn==w0+1, "probeItem 'WARN' increments g_warn");
    probeItem("TEST","item","FAIL","r","a","d");
    CHECK(g_fail==f0+1, "probeItem 'FAIL' increments g_fail");
    // Restore the counter so this deliberate classification probe does not
    // pollute the process exit code (aatpe-probe.cpp returns g_fail>0 -> 1).
    g_fail = f0;

    printf("=== Probe: G9-005 exit code matches verdict bands ===\n");
    int savedFail = g_fail;
    g_fail = 0;  CHECK(probeExitCode()==0, "g_fail=0 -> exit 0 (fully compatible)");
    g_fail = 3;  CHECK(probeExitCode()==0, "g_fail=3 -> exit 0 (basic compatible)");
    g_fail = 4;  CHECK(probeExitCode()==1, "g_fail=4 -> exit 1 (poor verdict; was 0 before fix)");
    g_fail = 5;  CHECK(probeExitCode()==1, "g_fail=5 -> exit 1 (poor verdict; was 0 before fix)");
    g_fail = savedFail;

    printf("\n========================================\nProbe results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail==0?0:1;
}
