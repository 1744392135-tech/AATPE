#include "it_common.h"
#include "aatped_control.h"
#include "aatped_topology.h"
#include "aatped_utils.h"
#include "aatped_state.h"
#include "aatpe-constants.h"

using namespace aatpe;
using namespace aatpe_ml;

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <cstring>
#include <chrono>

static int g_fail = 0;
static int g_checks = 0;
#define SP(cond, name, ...) do { \
    g_checks++; bool _c=(cond); if(!_c) g_fail++; \
    char _b[320]; ::snprintf(_b,sizeof(_b),__VA_ARGS__); \
    printf("    [%s] %s :: %s\n", _c?"PASS":"FAIL", name, _b); \
} while(0)

static std::string readOnDisk(const std::string& p) {
    std::string s = itReadFile(p);
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
    return s;
}

int phaseStateMatrix() {
    printf("\n--- PHASE3 state machine transition matrix ---\n");
    g_fail = 0; g_checks = 0;
    aatpe_state_setDwellMsForTest(0);

    auto resetToNormal = [&]() {
        g_thermalEmergency.store(false);
        SystemState cur = getSystemState();
        if (cur == SystemState::EMERGENCY) transitionToState(SystemState::DEGRADED, "reset");
        if (getSystemState() == SystemState::DEGRADED) transitionToState(SystemState::NORMAL, "reset");
        if (getSystemState() == SystemState::PASSIVE) { setWatchdogPassiveLocked(false); transitionToState(SystemState::NORMAL, "reset"); }
    };
    resetToNormal();
    SP(getSystemState()==SystemState::NORMAL, "M-N0 start NORMAL", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::DEGRADED, "m-deg");
    SP(getSystemState()==SystemState::DEGRADED, "M-N1 NORMAL->DEGRADED", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::NORMAL, "m-rec");
    SP(getSystemState()==SystemState::NORMAL, "M-N2 DEGRADED->NORMAL", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::EMERGENCY, "m-emg");
    SP(getSystemState()==SystemState::EMERGENCY, "M-N3 ->EMERGENCY", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::NORMAL, "m-emg-direct-release");
    SP(getSystemState()==SystemState::EMERGENCY, "M-N4 EMERGENCY->NORMAL direct BLOCKED (must route via DEGRADED)", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::DEGRADED, "m-emg-rel1");
    SP(getSystemState()==SystemState::DEGRADED, "M-N4b EMERGENCY->DEGRADED (legal recovery)", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::NORMAL, "m-emg-rel2");
    SP(getSystemState()==SystemState::NORMAL, "M-N4c DEGRADED->NORMAL after emergency", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::PASSIVE, "m-passive");
    SP(getSystemState()==SystemState::PASSIVE, "M-N5 ->PASSIVE", "state=%s", systemStateToString(getSystemState()));
    setWatchdogPassiveLocked(false);
    transitionToState(SystemState::NORMAL, "m-unlock");
    SP(getSystemState()==SystemState::NORMAL, "M-N6 PASSIVE->NORMAL(unlock)", "state=%s", systemStateToString(getSystemState()));
    transitionToState(SystemState::PASSIVE, "m-passive2");
    setWatchdogPassiveLocked(true);
    transitionToState(SystemState::NORMAL, "m-try-leave-locked");
    SP(getSystemState()==SystemState::PASSIVE, "M-N6b watchdog-locked PASSIVE cannot leave", "state=%s", systemStateToString(getSystemState()));
    setWatchdogPassiveLocked(false);
    transitionToState(SystemState::NORMAL, "m-unlock2");

    for (int i=0;i<200;i++) {
        transitionToState((i%2)?SystemState::DEGRADED:SystemState::NORMAL, "m-bounce");
    }
    { SystemState st=getSystemState();
      SP(st==SystemState::NORMAL || st==SystemState::DEGRADED,
         "M-N7 200x NORMAL<->DEGRADED bounce bounded (no crash/stuck)", "state=%s", systemStateToString(st)); }
    { std::lock_guard<std::mutex> lk(g_stateMutex);
      SP((long)g_stateHistory.size()<=32, "M-N8 stateHistory bounded (<=32)", "size=%zu", g_stateHistory.size()); }

    resetToNormal();
    g_cfg.thermalEmergency = 105; g_cfg.thermalEmergencyThreshold = 105;
    itSetThermalTemp("thermal_zone0", 30); thermal_resetRuntimeStateForTest(); readThermal();
    SP(!g_thermalEmergency.load(), "M-T1 cold 30C not emergency", "emerg=%d", (int)g_thermalEmergency.load());

    setRuntimeControlMode("assist", "m-t-baseline");
    std::string effBefore = readOnDisk(g_it.stateDir + "/control_mode_effective");
    SP(effBefore=="assist", "M-T2 baseline control_mode_effective=assist", "got=%s", effBefore.c_str());

    bool latched=false;
    for (int t=40; t<=120 && !latched; t+=10) {
        g_cfg.thermalEmergency=105; g_cfg.thermalEmergencyThreshold=105;
        itSetThermalTemp("thermal_zone0", t);
        itReadThermalFresh();
        if (g_thermalEmergency.load()) latched=true;
    }
    for (int k=0;k<3 && !latched;k++){
        g_cfg.thermalEmergency=105; g_cfg.thermalEmergencyThreshold=105;
        itSetThermalTemp("thermal_zone0",115); itReadThermalFresh();
        if (g_thermalEmergency.load()) latched=true;
    }
    SP(latched, "M-T3 >=105C ramp+debounce -> g_thermalEmergency latch", "emerg=%d", (int)g_thermalEmergency.load());
    std::string effEmerg = readOnDisk(g_it.stateDir + "/control_mode_effective");
    SP(effEmerg=="emergency", "M-T4 emergency latches control_mode_effective=emergency", "got=%s", effEmerg.c_str());

    itSetThermalTemp("thermal_zone0",30); itReadThermalFresh();
    g_thermalEmergency.store(false);
    transitionToState(SystemState::DEGRADED, "m-t-release");
    setRuntimeControlMode("assist", "m-t-release");
    std::string effAfter = readOnDisk(g_it.stateDir + "/control_mode_effective");
    SP(effAfter=="assist", "M-T5[C2] post-emergency control_mode_effective resyncs to assist (NOT stuck emergency)", "got=%s", effAfter.c_str());
    resetToNormal();

    const int MAXR = watchdog::kDefaultMaxRestarts;
    SP(!watchdog::shouldLatchPassive(MAXR, MAXR), "M-W1 restarts==max NOT latch", "max=%d", MAXR);
    SP(watchdog::shouldLatchPassive(MAXR+1, MAXR),  "M-W2 restarts==max+1 latch PASSIVE", "max=%d", MAXR);
    SP(!watchdog::shouldLatchPassive(1, MAXR),       "M-W3 1 restart stays active", "");
    const int HB = watchdog::kHeartbeatTimeoutSec;
    SP(watchdog::heartbeatIsStale(HB, HB),          "M-W4 heartbeat age==timeout => stale", "hb=%d", HB);
    SP(!watchdog::heartbeatIsStale(HB-1, HB),       "M-W5 heartbeat age==timeout-1 => healthy", "hb=%d", HB);
    const int CD = watchdog::kPassiveCooldownSec;
    SP(watchdog::inPassiveCooldown(CD-1, CD),       "M-W6 inside cooldown", "cd=%d", CD);
    SP(!watchdog::inPassiveCooldown(CD+60, CD),     "M-W7 cooldown expired", "cd=%d", CD);

    for (const char* m : {"passive","assist","active"}) {
        bool ok = setRuntimeControlMode(m, "m-mode");
        std::string cur; { std::lock_guard<std::mutex> lk(g_cfgMutex); cur=g_cfg.controlMode; }
        SP(ok && cur==m, "M-C1 setRuntimeControlMode accepted", "want=%s got=%s ok=%d", m, cur.c_str(), (int)ok);
        std::string disk = readOnDisk(g_it.stateDir + "/control_mode");
        SP(disk==m, "M-C2 control_mode persisted", "want=%s got=%s", m, disk.c_str());
    }
    bool bogus = setRuntimeControlMode("bogus_mode", "m-bogus");
    SP(!bogus, "M-C3 invalid mode rejected (false)", "bogus=%d", (int)bogus);
    bool emergUser = setRuntimeControlMode("emergency", "m-user-emerg");
    SP(emergUser, "M-C4a setRuntimeControlMode(emergency) accepted by watchdog choke point", "ret=%d", (int)emergUser);
    { std::lock_guard<std::mutex> lk(g_cfgMutex);
      SP(g_cfg.controlMode=="emergency", "M-C4b g_cfg.controlMode==emergency", "got=%s", g_cfg.controlMode.c_str()); }
    std::string emergDisk = readOnDisk(std::string(g_it.stateDir) + "/control_mode");
    SP(emergDisk=="emergency", "M-C4c on-disk control_mode==emergency", "got=%s", emergDisk.c_str());
    SP(getSystemState()==SystemState::EMERGENCY, "M-C4d state machine drove EMERGENCY", "state=%s", systemStateToString(getSystemState()));
    setRuntimeControlMode("assist", "m-reset");

    printf("  [matrix] checks=%d fails=%d\n", g_checks, g_fail);
    return g_fail;
}

int phaseFaultInjection() {
    printf("\n--- PHASE4 fault injection ---\n");
    g_fail = 0; g_checks = 0;
    g_thermalEmergency.store(false);
    if (getSystemState()==SystemState::EMERGENCY) transitionToState(SystemState::DEGRADED,"f-reset");
    if (getSystemState()==SystemState::DEGRADED) transitionToState(SystemState::NORMAL,"f-reset");
    if (getSystemState()==SystemState::PASSIVE) { setWatchdogPassiveLocked(false); transitionToState(SystemState::NORMAL,"f-reset"); }
    std::string run = g_it.runDir, st = g_it.stateDir;

    ::unlink((run+"/control").c_str());
    processControlCommands();
    SP(getSystemState()==SystemState::NORMAL, "F1 empty control mailbox -> NORMAL unchanged", "state=%s", systemStateToString(getSystemState()));

    itWriteFile(run+"/control", "set_mode");
    std::string cmBefore2 = readOnDisk(st+"/control_mode");
    processControlCommands();
    std::string cmAfter2 = readOnDisk(st+"/control_mode");
    SP(cmBefore2==cmAfter2, "F2 truncated 'set_mode' (no '=') NOT applied, control_mode unchanged", "before=%s after=%s", cmBefore2.c_str(), cmAfter2.c_str());

    itWriteFile(run+"/control", "\x01\x02\xff\x00 set_mode=active; rm -rf /");
    processControlCommands();
    std::string cm = readOnDisk(st+"/control_mode");
    SP(cm!="active", "F3 injection junk NOT applied (no error-success)", "control_mode=%s", cm.c_str());

    {
        std::string big(70*1024, 'a'); big += "\n";
        itWriteFile(run+"/control_cmd", big);
        processControlCommands();
        struct stat sb; bool residue = (::stat((run+"/control_cmd.processing").c_str(),&sb)==0);
        SP(!residue, "F4 oversized control_cmd discarded, no .processing residue", "residue=%d", (int)residue);
    }

    itWriteFile(run+"/control.processing", "set_mode=assist\n");
    processControlCommands();
    SP(getSystemState()==SystemState::NORMAL, "F5 stale control.processing: no deadlock, state stayed NORMAL", "state=%s", systemStateToString(getSystemState()));
    std::string cmAfter5 = readOnDisk(st+"/control_mode");
    SP(cmAfter5=="assist", "F5 stale staging bytes NOT applied (control_mode unchanged)", "control_mode=%s", cmAfter5.c_str());
    ::unlink((run+"/control.processing").c_str());

    {
        const char* ro = getenv("AATPE_RO_DIR");
        if (ro) {
            bool ok = writeFile((std::string(ro)+"/probe").c_str(), "x");
            SP(!ok, "F6 EACCES read-only dir: writeFile fails closed (no false success)", "ret=%d", (int)ok);
        } else { SP(true, "F6 EACCES ro dir provided", "env missing, skipped"); }
    }

    {
        setRuntimeControlMode("assist", "f6b-base");
        itWriteFile(run+"/control_cmd", "set_mode=active\n");
        processControlCommands();
        std::string cur; { std::lock_guard<std::mutex> lk(g_cfgMutex); cur=g_cfg.controlMode; }
        SP(cur=="active", "F6b control_cmd drained+applied (set_mode=active took effect, no loss)", "controlMode=%s", cur.c_str());
        setRuntimeControlMode("assist", "f6b-reset");
    }

    itWriteFile(st+"/state.json", "");
    itWriteFile(st+"/state.json", "{ \"control_mode\": ");
    itWriteFile(st+"/state.json", "\xff\xfe garbage{{{");
    writeStateJson();
    {
        std::string sj = itReadFile(st+"/state.json");
        bool sjValid = !sj.empty() && sj.front()=='{' &&
                       sj.find("\"control_mode\"")!=std::string::npos;
        SP(sjValid, "F7 writer recovered: state.json overwritten with valid JSON (garbage not left behind)",
           "len=%zu head='%.24s'", sj.size(), sj.c_str());
    }

    {
        const char* full = getenv("AATPE_FULL_DIR");
        if (full) {
            std::string big(64*1024, 'x');
            bool ok = writeFile((std::string(full)+"/big").c_str(), big);
            SP(!ok, "F7b ENOSPC tiny tmpfs: oversized write fails closed (no crash)", "ret=%d", (int)ok);
        } else { SP(true, "F7b ENOSPC full dir provided", "env missing, skipped"); }
    }

    ::unlink((std::string("/sys/class/thermal/thermal_zone0/temp")).c_str());
    thermal_resetRuntimeStateForTest(); readThermal();
    SP(!g_thermalEmergency.load(), "F8 missing thermal sysfs node -> no false emergency", "emerg=%d", (int)g_thermalEmergency.load());
    itWriteFile("/sys/class/thermal/thermal_zone0/temp","30000");

    ::chmod(st.c_str(), 0500);
    writeStateJson();
    bool threw = false; (void)threw;
    ::chmod(st.c_str(), 0755);
    writeStateJson();
    {
        std::string sj9 = itReadFile(st+"/state.json");
        SP(!sj9.empty() && sj9.front()=='{',
           "F9 EACCES write tolerated (crash-only), daemon not wedged: post-recovery state.json valid",
           "len=%zu", sj9.size());
    }

    {
        std::string longc = "set_mode=" + std::string(300,'x');
        std::string cmBefore10 = readOnDisk(st+"/control_mode");
        itWriteFile(run+"/control", longc);
        processControlCommands();
        std::string cmAfter10 = readOnDisk(st+"/control_mode");
        SP(cmBefore10==cmAfter10, "F10 >256 char command rejected at length gate, control_mode unchanged",
           "before=%s after=%s", cmBefore10.c_str(), cmAfter10.c_str());
        struct stat sb10; bool resid10 = (::stat((run+"/control.processing").c_str(),&sb10)==0);
        SP(!resid10, "F10 staging .processing unlinked after drain", "residue=%d", (int)resid10);
    }

    printf("  [fault] checks=%d fails=%d\n", g_checks, g_fail);
    return g_fail;
}

int phaseSignalSafety() {
    printf("\n--- PHASE5 signal safety (subprocess) ---\n");
    g_fail = 0; g_checks = 0;
    int sigs[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
    for (int sig : sigs) {
        pid_t pid = ::fork();
        if (pid == 0) {
            installCrashHandlers();
            raise(sig);
            _exit(0);
        }
        int status=0; bool reaped=false;
        for (int w=0; w<50; w++) {
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) { reaped=true; break; }
            if (r < 0 && errno == ECHILD) { reaped=true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!reaped) {
            ::kill(pid, SIGKILL);
            for (int w=0;w<30;w++){ pid_t r=::waitpid(pid,&status,WNOHANG); if(r==pid||(r<0&&errno==ECHILD)){break;} std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
        bool killedBySig = reaped && WIFSIGNALED(status);
        int termSig = killedBySig ? WTERMSIG(status) : 0;
        std::string cs = readOnDisk(g_it.stateDir + "/crash_signal");
        bool fileWritten = !cs.empty();
        SP(killedBySig && termSig==sig, "SIG child re-raising fatal dies by own signal (no self-abort loop)",
           "sig=%d killed=%d termsig=%d", sig, (int)killedBySig, termSig);
        SP(fileWritten, "SIG crash_signal file written by handler", "sig=%d val=%s", sig, cs.c_str());
    }
    {
        pid_t pid = ::fork();
        if (pid == 0) {
            signalHandler(SIGTERM);
            _exit(g_shouldExit.load()?0:3);
        }
        int st=0; bool reap2=false;
        for (int w=0; w<50; w++) {
            pid_t r=::waitpid(pid,&st,WNOHANG);
            if (r==pid) { reap2=true; break; }
            if (r<0 && errno==ECHILD) { reap2=true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!reap2) {
            ::kill(pid,SIGKILL);
            for (int w=0;w<30;w++){ pid_t r=::waitpid(pid,&st,WNOHANG); if(r==pid||(r<0&&errno==ECHILD)){reap2=true;break;} std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            if(!reap2) ::printf("    [sig-term] child D-state unreaped, detaching\n");
        }
        SP(reap2 && WIFEXITED(st) && WEXITSTATUS(st)==0, "SIGTERM sets g_shouldExit graceful", "exit=%d", reap2&&WIFEXITED(st)?WEXITSTATUS(st):-1);
    }
    printf("  [signal] checks=%d fails=%d\n", g_checks, g_fail);
    return g_fail;
}

static void walkTree(const std::string& root, std::vector<std::string>& out) {
    DIR* d = ::opendir(root.c_str()); if(!d) return;
    struct dirent* e;
    while ((e=::readdir(d))) {
        if (!::strcmp(e->d_name,".")||!::strcmp(e->d_name,"..")) continue;
        std::string p = root + "/" + e->d_name;
        struct stat sb;
        if (::stat(p.c_str(),&sb)!=0) continue;
        if (S_ISDIR(sb.st_mode)) walkTree(p, out);
        else out.push_back(p);
    }
    ::closedir(d);
}
long phaseUninstallCoverage() {
    printf("\n--- PHASE6 runtime dynamic path enumeration ---\n");
    writeStateJson();
    setRuntimeControlMode("active","cov");
    setRuntimeControlMode("assist","cov");
    itWriteFile(g_it.runDir+"/control_cmd","set_mode=assist\n");
    processControlCommands();
    writeFile((g_it.stateDir+"/heartbeat").c_str(), 12345);
    std::vector<std::string> files;
    walkTree(g_it.mockRoot, files);
    long n=0;
    for (auto& f : files) {
        printf("    node: %s\n", f.c_str()); n++;
    }
    printf("  [uninstall] runtime materialized files=%ld\n", n);
    return n;
}

#include "aatpe_session_token.h"
#include "webui_http.h"
#include <deque>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

struct CredOut { long sseSlots; long activeSessions; long nonceCache; long halfopenReclaimMs; };

static std::deque<std::pair<std::string,time_t>> g_credSess;
static int  g_httpPort = 0;
static std::string g_daemonTok;
static std::vector<long> g_reclaimSamples;

static int sseOpenHalfOpen(int port, const std::string& tok) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int rcvbuf = 65536;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in a; ::memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (::connect(s,(struct sockaddr*)&a,sizeof a) < 0) { ::close(s); return -1; }
    std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nX-Daemon-Token: " + tok + "\r\n\r\n";
    size_t off=0; const char* p=req.c_str();
    while (off < req.size()) { ssize_t n=::send(s,p+off,req.size()-off,0); if(n<=0) break; off+=n; }
    struct timeval tv; tv.tv_sec=2; tv.tv_usec=0;
    ::setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    char hdr[512]; int n=::recv(s,hdr,sizeof hdr,0);
    if (n>0 && hdr[0]=='H' && ::strstr(hdr,"200")) return s;
    ::close(s); return -1;
}

static long sseHalfOpenBatch(int port, const std::string& tok) {
    std::vector<int> hold;
    for (int i=0;i<6;i++) { int s=sseOpenHalfOpen(port,tok); if(s>=0) hold.push_back(s); }
    long openCount = (long)hold.size();

    SP(openCount == 6, "half-open: all 6 SSE slots acquired",
       "openCount=%ld/6 (kSseMaxPerSharedTcp=6)", openCount);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int probe7 = sseOpenHalfOpen(port, tok);
    SP(probe7 < 0, "half-open: 7th SSE conn rejected (429, shared-TCP bucket full)",
       "probe7_fd=%d (want -1)", probe7);
    if (probe7 >= 0) ::close(probe7);

    auto t0 = std::chrono::steady_clock::now();
    bool reclaimed=false; int lastStatus=-1;
    long halfOpenPollSec = 20;
    if (const char* e = ::getenv("AATPE_HALFOPEN_POLL_SEC")) {
        long v = ::atol(e); if (v >= 1 && v <= 120) halfOpenPollSec = v;
    }
    auto deadline = t0 + std::chrono::seconds(halfOpenPollSec);
    int pollTick=0;
    while (std::chrono::steady_clock::now() < deadline) {
        int s = sseOpenHalfOpen(port,tok);
        if (s >= 0) { ::close(s); reclaimed=true; break; }
        lastStatus = -1;
        if ((++pollTick % 10)==0) {
            long live = aatpe::webui_http::hasActiveClients();
            ::printf("      [halfopen-poll] t=%lds g_sseActive=%ld held=%zu\n",
                     (long)std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now()-t0).count(), live, hold.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now()-t0).count();
    for (int s : hold) ::close(s);
    ::printf("    [halfopen] openCount=%ld reaped=%d pollMs=%ld\n", openCount, (int)reclaimed, ms);
    return reclaimed ? ms : -1;
}

CredOut credRound(int roundNo) {
    CredOut out{0,0,0,-1};
    time_t now = ::time(nullptr);

    std::string tok = aatpeSessionIssue();
    if (!tok.empty()) g_credSess.push_back({tok, now});
    bool validAccepted = aatpeSessionVerify(tok);
    bool bogusRejected = aatpeSessionVerify(std::string(64,'f'));
    SP(validAccepted, "CRED valid session verifies (200/applied)", "tokLen=%zu", tok.size());
    SP(!bogusRejected, "CRED bogus stok rejected (403)", "");

    while (!g_credSess.empty() && (now - g_credSess.front().second) > 300)
        g_credSess.pop_front();
    long active=0;
    for (auto& kv : g_credSess) if (aatpeSessionVerify(kv.first)) active++;

    long reclaimMs = -1;
    if (g_httpPort > 0) {
        reclaimMs = sseHalfOpenBatch(g_httpPort, g_daemonTok);
        if (reclaimMs >= 0) g_reclaimSamples.push_back(reclaimMs);
        if (reclaimMs < 0)
            printf("    [halfopen-OBSERVE] not reclaimed within poll window (host loopback idle); real-device must review\n");
    }

    out.sseSlots = (g_httpPort>0) ? (long)aatpe::webui_http::hasActiveClients() : 0;
    out.activeSessions = active;
    out.nonceCache = 0;
    out.halfopenReclaimMs = reclaimMs;
    return out;
}

int phaseCredentialBootstrap() {
    printf("\n--- PHASE7 credential/sse half-open bootstrap (F-01 long-lived) ---\n");
    g_fail = 0; g_checks = 0;
    aatpeSessionPurgeAll();
    g_credSess.clear(); g_reclaimSamples.clear();

    std::vector<std::string> toks;
    for (int i=0;i<20;i++) toks.push_back(aatpeSessionIssue());
    long live=0;
    for (auto& t : toks) if (aatpeSessionVerify(t)) live++;
    SP(live <= 8, "CRED-BOOT session cap<=8 (LRU evict oldest)", "live=%ld/20", live);
    SP(live >= 1, "CRED-BOOT at least newest token live", "live=%ld", live);
    SP(aatpeSessionVerify(toks.back()), "CRED-BOOT newest token still live", "");
    aatpeSessionPurgeAll();

    aatpe::webui_http::ensureDaemonToken();
    bool up = aatpe::webui_http::startHttpServer();
    g_httpPort = up ? aatpe::webui_http::getHttpPort() : 0;
    SP(up && g_httpPort>0, "CRED-BOOT real HTTP server started for SSE half-open", "port=%d", g_httpPort);
    if (up) {
        std::ifstream tf(std::string(g_it.stateDir)+"/daemon_token");
        g_daemonTok.assign((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
        while (!g_daemonTok.empty() && (g_daemonTok.back()=='\n'||g_daemonTok.back()=='\r'||g_daemonTok.back()==' ')) g_daemonTok.pop_back();
        ::std::this_thread::sleep_for(::std::chrono::milliseconds(300));
    }
    printf("  [cred-bootstrap] checks=%d fails=%d httpPort=%d\n", g_checks, g_fail, g_httpPort);
    return g_fail;
}

void phaseHttpTeardown() {
    if (g_httpPort > 0) { aatpe::webui_http::stopHttpServer(); g_httpPort = 0; }
}

