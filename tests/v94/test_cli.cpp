// test_cli.cpp - Direct unit tests for aatpe-cli.cpp helpers.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#define main cli_main_unused
#include "aatpe-cli.cpp"
#undef main
static int g_pass=0,g_fail=0;
#define CHECK(c,m) do{ if(c){g_pass++;printf("  [PASS] %s\n",m);} else {g_fail++;printf("  [FAIL] %s\n",m);} }while(0)
int main(){
    printf("=== CLI: cliSoftmaxFuseScores ===\n");
    float f = cliSoftmaxFuseScores(0.8f, 0.2f, 1.0f);
    CHECK(f > 0.2f && f < 0.8f, "softmax fuse is a convex blend (0.2 < f < 0.8)");
    float f2 = cliSoftmaxFuseScores(0.9f, 0.9f, 1.0f);
    CHECK(std::fabs(f2 - 0.9f) < 0.01f, "equal scores -> that score");
    float f3 = cliSoftmaxFuseScores(0.8f, 0.2f, 0.001f); // tiny T -> clamped
    CHECK(std::isfinite(f3), "tiny temperature clamped, finite output");

    printf("=== CLI: safe numeric parsers ===\n");
    CHECK(safeStoi("42")==42, "safeStoi 42");
    CHECK(safeStoi("abc", -1)==-1, "safeStoi bad -> default");
    CHECK(safeStoull("1234567890")==1234567890ULL, "safeStoull");
    CHECK(safeStoull("x", 7)==7, "safeStoull bad -> default");
    CHECK(safeStoll("-5")==-5LL, "safeStoll negative");

    printf("=== CLI: trim ===\n");
    { std::string s="  a b  "; trim(s); CHECK(s=="a b", "trim cli"); }

    printf("=== CLI: readFile/readInt from temp file ===\n");
    FILE* fp = fopen("/tmp/cli_test.txt","w"); fputs("  1234  \n", fp); fclose(fp);
    std::string s = readFile("/tmp/cli_test.txt");
    CHECK(s=="1234", "readFile trims whitespace/newline");
    CHECK(readInt("/tmp/cli_test.txt", 0)==1234, "readInt parses 1234");
    CHECK(readInt("/tmp/does_not_exist", 99)==99, "readInt missing -> default");

    printf("=== CLI: color / role display ===\n");
    g_useColor = false;
    CHECK(std::string(color(RED))=="", "color disabled -> empty");
    g_useColor = true;
    CHECK(std::string(color(RED))==RED, "color enabled -> passthrough");
    CHECK(roleToDisplayName("0")==std::string(role_names::kNames[0]), "role 0 maps to name");
    CHECK(roleToDisplayName("S99")=="S99", "S-prefixed role passthrough");
    CHECK(roleToDisplayName("")==std::string(role_names::kUnknown), "empty role -> unknown");

    printf("=== CLI: cliMlModelPath ===\n");
    std::string mp = cliMlModelPath();
    CHECK(mp.find("model.dat")!=std::string::npos, "cliMlModelPath ends with model.dat");

    // ---- G9-004: subcommand exit codes (五级判定) ---------------------------
    printf("=== CLI: G9 exit-code contract ===\n");
    // daemon status on host (no /dev/aatpe) -> treated as stopped, not an error.
    CHECK(cmdDaemon("status") == 0, "daemon status -> 0");
    // bad usage -> 2
    CHECK(cmdDaemon(nullptr) == 2, "daemon (no action) -> 2 usage error");
    CHECK(cmdDaemon("bogus") == 2, "daemon bogus -> 2 usage error");
    // v4.2.9: this target now runs under a mount namespace with a writable tmpfs
    // /dev/aatpe run dir (needed for ui-snapshot's kStateDir). With a writable run
    // dir, model-save successfully appends to control and returns 0 (the historical
    // "no control file -> 1" branch is only reachable when the run dir is absent,
    // which the namespace no longer reproduces). Assert the success path end-to-end:
    CHECK(cmdModelSave() == 0, "model-save with writable run dir -> 0");
    CHECK(readFile((std::string(aatpe::paths::kRunDir)+"/control").c_str()).find("model_save=1")!=std::string::npos,
          "model-save request appended to control file");

    // ---- v4.2.9: ui-snapshot credential isolation + JSON-escape injection ----
    // These run `ui-snapshot` end-to-end via the renamed main. The recipe runs the
    // whole binary under `unshare --map-root-user --mount` with a tmpfs bound over
    // /dev, so getuid()==0 (root check passes) and /dev/aatpe/state is a writable
    // scratch dir we seed here. kStateDir is a hardcoded constexpr (no env seam);
    // the mount-namespace bind is the established test seam (see run_a2_boost).
    printf("\n=== CLI: ui-snapshot credential isolation (v4.2.9) ===\n");
    {
        auto wstate=[](const std::string& name,const std::string& content){
            std::ofstream f(std::string(aatpe::paths::kStateDir)+"/"+name,std::ios::binary);
            f<<content; f.close();
        };
        // credential material that MUST never reach the WebView.
        wstate("daemon_token",   "SECRET-DAEMON-TOKEN-12345");
        wstate("session_token",  "SECRET-SESSION-TOKEN-67890");
        wstate("hmac_key",       "SECRET-HMAC-KEY-ABCDEF");
        // plain state files that MUST appear.
        wstate("control_mode",   "active");
        wstate("current_profile","balanced");
        wstate("frame_apps.json","{\"current\":\"com.x\",\"apps\":[]}");

        // capture stdout of a fresh `ui-snapshot` invocation.
        fflush(stdout);
        char snapPath[]="/tmp/aatpe_cli_snap_XXXXXX";
        int sfd=mkstemp(snapPath);
        int saved=dup(STDOUT_FILENO);
        dup2(sfd,STDOUT_FILENO);
        char* argv[]={(char*)"aatpe-cli",(char*)"ui-snapshot"};
        int rc=cli_main_unused(2,argv);
        fflush(stdout);
        dup2(saved,STDOUT_FILENO); close(saved); close(sfd);

        std::string out=readFile(snapPath);
        printf("  [INFO] ui-snapshot rc=%d bytes=%zu\n",rc,out.size());
        CHECK(rc==0,"ui-snapshot exits 0");
        // credential file names must be redacted...
        CHECK(out.find("daemon_token")==std::string::npos,"daemon_token filename redacted");
        CHECK(out.find("session_token")==std::string::npos,"session_token filename redacted");
        CHECK(out.find("hmac_key")==std::string::npos,"hmac_key filename redacted");
        // ...and their secret contents must not leak.
        CHECK(out.find("SECRET-DAEMON-TOKEN-12345")==std::string::npos,"daemon_token value redacted");
        CHECK(out.find("SECRET-SESSION-TOKEN-67890")==std::string::npos,"session_token value redacted");
        CHECK(out.find("SECRET-HMAC-KEY-ABCDEF")==std::string::npos,"hmac_key value redacted");
        // plain state files DO appear (both key and value).
        CHECK(out.find("\"control_mode\"")!=std::string::npos,"control_mode key present");
        CHECK(out.find("active")!=std::string::npos,"control_mode value present");
        // frame_apps.json inlined as frame_apps_raw, not as a raw escaped filename.
        CHECK(out.find("frame_apps_raw")!=std::string::npos,"frame_apps.json inlined as frame_apps_raw");
        unlink(snapPath);
    }

    printf("\n=== CLI: ui-snapshot JSON-escape injection in threads[] (v4.2.9) ===\n");
    {
        // Seed a thread_db.json whose tname, after loadThreadDb() extracts it,
        // literally contains a backslash, a double-quote, and a REAL newline.
        // On disk:  "tname":"na\"me<LF>next"  (backslash-quote = escaped quote so the
        // scanner keeps it; <LF> is a raw byte). jsonStrField() returns the raw
        // substring without unescaping, so r.tname = n a \ " m e <LF> n e x t.
        ::mkdir((std::string(aatpe::paths::kRunDir)+"/thread_db").c_str(),0755);
        {
            std::ofstream db(std::string(aatpe::paths::kRunDir)+"/thread_db/thread_db.json",std::ios::binary);
            db << "{\"threads\":["
               << "{\"pid\":1001,\"tid\":2001,\"pname\":\"game\","
               << "\"tname\":\"na\\\"me\nnext\","
               << "\"role\":3,\"subtype\":\"s\",\"priority\":10,\"protect\":1,"
               << "\"nr_switches\":5,\"run_time\":10}"
               << "]}";
            db.close();
        }

        fflush(stdout);
        char snapPath[]="/tmp/aatpe_cli_snap2_XXXXXX";
        int sfd=mkstemp(snapPath);
        int saved=dup(STDOUT_FILENO);
        dup2(sfd,STDOUT_FILENO);
        char* argv[]={(char*)"aatpe-cli",(char*)"ui-snapshot"};
        int rc=cli_main_unused(2,argv);
        fflush(stdout);
        dup2(saved,STDOUT_FILENO); close(saved); close(sfd);

        std::string out=readFile(snapPath); // trims the single trailing \n
        printf("  [INFO] snapshot-with-injection bytes=%zu\n",out.size());
        CHECK(rc==0,"ui-snapshot (injected tname) exits 0");
        // the injected thread record must be present.
        CHECK(out.find("\"tid\":2001")!=std::string::npos,"injected thread record present");
        // (a) the embedded REAL newline must have been escaped: the whole JSON body
        //     is a single physical line (no raw '\n' survives).
        CHECK(out.find('\n')==std::string::npos,"no raw newline inside JSON (embedded LF escaped)");
        // (b) structural balance, treating backslash-escaped quotes as non-structural.
        {
            int curly=0,sq=0; bool inStr=false,esc=false,balanced=true;
            for(char c:out){
                if(inStr){
                    if(esc) esc=false;
                    else if(c=='\\') esc=true;
                    else if(c=='"') inStr=false;
                    continue;
                }
                if(c=='"') inStr=true;
                else if(c=='{') curly++;
                else if(c=='}') curly--;
                else if(c=='[') sq++;
                else if(c==']') sq--;
            }
            if(inStr||curly!=0||sq!=0) balanced=false;
            CHECK(balanced,"threads[] injection leaves JSON structurally balanced");
        }
        // (c) the dangerous quote must appear escaped (as \") not as a raw breaking quote.
        //     raw output between name quotes is: na \\ \" me \n next  -> contains "\\\""
        CHECK(out.find("\\\"")!=std::string::npos,"double-quote escaped as \\\" in output");
        unlink(snapPath);
    }

    printf("\n========================================\nCLI results: %d passed, %d failed\n========================================\n", g_pass, g_fail);
    return g_fail==0?0:1;
}
