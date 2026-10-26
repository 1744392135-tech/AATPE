// AATPE CLI - Adaptive Android Task Placement Engine
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/wait.h>
#include "aatpe-constants.h"
#include "aatpe_pid_identity.h"

// aatpeExec() popen stub is NOT dead code — aatpe_ml.cpp
// (GfxInfoFrameTimeProvider / SurfaceFlingerLatencyProvider) calls it at lines 876/1032.
// The aatpe-cli link target does not include aatped.cpp (which has the real fork+poll
// implementation), so this popen stub is required for CLI-side ML frame-time providers.
// Real implementation with 3s timeout protection lives in aatped.cpp for the daemon.
// aatpeExec() — P2-#16: popen() with a 3s hard timeout. The old popen() could block
// forever if the spawned command hung (e.g. a hung `getprop` or service call).
// We fork/exec a shell running the command, poll the read end with a wall-clock
// deadline, and SIGKILL the child if it exceeds the budget.
// G9-015: cmd is the fixed prefix the daemon's ML frame-time providers use
// ("getprop ...", "dumpsys gfxinfo <pkg>"). The only variable part is <pkg>, which
// the caller (aatpe_ml.cpp) validates against a [A-Za-z0-9._] whitelist BEFORE
// concatenation, so there is no shell-injection surface today. If that call is ever
// changed, prefer direct exec of argv[] (no shell) to keep it that way.
std::string aatpeExec(const std::string& cmd, const char* debugTag) {
    (void)debugTag;
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";
    pid_t pid = vfork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execl("/system/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    std::string result;
    char buf[256];
    // G9-009: drive the 3s budget off a wall-clock deadline, not only off poll
    // timeouts. The previous `elapsedMs` only advanced when poll() timed out, so a
    // child that trickled >=1 byte every <200ms never hit the budget and could block
    // the CLI indefinitely.
    auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    while (true) {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        long long leftMs = 3000 - (nowMs - startMs);
        if (leftMs <= 0) break;
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, (int)(leftMs > 200 ? 200 : leftMs));
        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) break;
            result.append(buf, n);
        } else if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // pr==0 = poll timeout; loop re-evaluates the wall-clock deadline.
    }
    kill(pid, SIGKILL);
    int status;
    // waitpid may return ECHILD if the child already exited on its own.
    if (waitpid(pid, &status, 0) < 0 && errno != ECHILD) {
        // unexpected; nothing to do
    }
    close(pipefd[0]);
    return result;
}

// aatpe_ml.cpp references logMsg() via LOG_WARN() macros. The CLI link target
// does not include aatped_logging.cpp, so provide a no-op stub.
extern "C" void logMsg(int /*level*/, const char* /*fmt*/, ...) {}

// G9-003: explicit-stop request flag shared with the watchdog. `daemon stop`
// writes it; `daemon start`/`restart` and service.sh clear it. While it exists
// the watchdog must NOT auto-restart the daemon (otherwise a 3s poll revives a
// deliberate stop). Root-only 0600 in the state dir.
static std::string cliStopRequestPath() {
    return std::string(aatpe::paths::kStateDir) + "/stop_request";
}
static void cliSetStopRequest() {
    std::ofstream f(cliStopRequestPath(), std::ios::trunc);
    if (f) f << "1\n";
}
static void cliClearStopRequest() {
    ::unlink(cliStopRequestPath().c_str());
}

// signal handling — SIGPIPE ignored to avoid crash on broken
// pipe, and a global interrupt flag set by SIGINT/SIGTERM for long loops.
static std::atomic<bool> g_cliInterrupted{false};
static void cliSignalHandler(int sig) {
    (void)sig;
    g_cliInterrupted.store(true, std::memory_order_relaxed);
}

int g_cpuCount = 8;
static void initCpuCount() {
    FILE* fp = fopen("/sys/devices/system/cpu/online", "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            int maxCpu = 0;
            for (char* p = buf; *p; p++) {
                if (*p >= '0' && *p <= '9') {
                    int n = aatpe::parseIntSafe(p);
                    if (n > maxCpu) maxCpu = n;
                    while (*p >= '0' && *p <= '9') p++;
                }
            }
            g_cpuCount = maxCpu + 1;
        }
        fclose(fp);
    }
}
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "aatpe-constants.h"
#include "aatpe-version.h"
#include "aatpe_ml.h"
#include "aatped_apklabel.h"
using namespace aatpe;
using namespace aatpe_ml;

static float cliSoftmaxFuseScores(float modelScore, float heuristicScore, float temperature) {
    if (temperature <= 0.01f) temperature = 0.01f;
    float m = modelScore / temperature, h = heuristicScore / temperature;
    float maxVal = (m > h) ? m : h; m -= maxVal; h -= maxVal;
    float em = expf(m), eh = expf(h), sum = em + eh;
    if (sum < 1e-6f) return heuristicScore;
    return (modelScore * em + heuristicScore * eh) / sum;
}

static const char* cliModulePath() {
    static std::string p;
    if (p.empty()) {
        // mirror getModuleDir() candidate order (Magisk / KernelSU /
        // APatch / Magisk pending update). The CLI is a standalone binary that
        // does not link aatped_utils.cpp, so it keeps its own copy of the SAME
        // canonical list instead of a Magisk-only hardcode.
        static const char* kDirs[] = {
            "/data/adb/modules/aatpe",
            "/data/adb/ksu/modules/aatpe",
            "/data/adb/ap/modules/aatpe",
            "/data/adb/modules_update/aatpe",
        };
        for (const char* d : kDirs) { if (access(d, F_OK)==0) { p=d; break; } }
        if (p.empty()) p = "/data/adb/modules/aatpe";
    }
    return p.c_str();
}
static std::string cliMlModelPath() { return std::string(cliModulePath()) + "/model.dat"; }

const char* RED="\033[0;31m",*GREEN="\033[0;32m",*YELLOW="\033[0;33m",*BLUE="\033[0;34m",*CYAN="\033[0;36m",*NC="\033[0m";
bool g_useColor=true;
const char* color(const char* c){return g_useColor?c:"";}

std::string readFile(const char* path){std::ifstream f(path);if(!f)return"";std::stringstream ss;ss<<f.rdbuf();std::string s=ss.str();size_t st=s.find_first_not_of(" \t\r\n");if(st==std::string::npos)return"";size_t en=s.find_last_not_of(" \t\r\n");return s.substr(st,en-st+1);}
int readInt(const char* path,int def){std::string s=readFile(path);if(s.empty())return def;try{return std::stoi(s);}catch(...){return def;}}
std::string readState(const char* key,const char* def){std::string path=std::string(aatpe::paths::kStateDir)+"/"+key;std::string s=readFile(path.c_str());return s.empty()?def:s;}
void trim(std::string& s){size_t st=s.find_first_not_of(" \t\r\n");if(st==std::string::npos){s.clear();return;}size_t en=s.find_last_not_of(" \t\r\n");s=s.substr(st,en-st+1);}
static int safeStoi(const std::string& s,int def=0){try{return std::stoi(s);}catch(...){return def;}}
// P3 build-fix: safeStoull is currently unreferenced (kept for future CLI u64
// parsing). [[maybe_unused]] silences -Wunused-function under -Werror without
// deleting the helper.
[[maybe_unused]] static unsigned long long safeStoull(const std::string& s,unsigned long long def=0){try{return std::stoull(s);}catch(...){return def;}}
static long long safeStoll(const std::string& s,long long def=0){try{return std::stoll(s);}catch(...){return def;}}
// RFC 8259 string escaper. Escapes " \ newline CR tab AND every other byte <0x20
// as \u00XX. The old hand-rolled loops in `ui-snapshot` only handled " \ \n \r
// and left the remaining control bytes (0x01-0x08, 0x0B, 0x0C, 0x0E-0x1F) raw
// inside a JSON string — V8 JSON.parse() rejects raw control chars, so the whole
// `aatpe-cli ui-snapshot` payload was illegal JSON and the WebUI silently fell
// back to N per-file exec cats.
static std::string jsonEscapeStr(const std::string& v){
    static const char* H="0123456789abcdef";
    std::string o; o.reserve(v.size()+8);
    for(unsigned char c:v){
        if(c=='"'){o+="\\\"";}
        else if(c=='\\'){o+="\\\\";}
        else if(c=='\n'){o+="\\n";}
        else if(c=='\r'){o+="\\r";}
        else if(c=='\t'){o+="\\t";}
        else if(c<0x20){o+="\\u00";o+=H[(c>>4)&0xF];o+=H[c&0xF];}
        else{o+=(char)c;}
    }
    return o;
}

// P1-2 (hardened): PID-reuse-safe identity check. Delegates to the unified
// aatpe::pid_identity::verifyAatpedPid() which enforces triple identity:
//   1. kill(pid, 0) confirms the process exists.
//   2. /proc/<pid>/exe realpath EXACTLY equals "/system/bin/aatped"
//      (NOT a substring/find("aatped") match — rejects aatped_fake etc.).
//   3. /proc/<pid>/stat starttime matches the starttime the daemon published
//      to /dev/aatpe/state/daemon_starttime at its own startup.
// Without this, kill(dpid,0)==0 on an unrelated process that reused the PID would
// make the CLI believe the daemon is alive (and potentially signal the wrong
// process with SIGTERM/SIGUSR1).
static bool isAatpedPid(int pid) {
    return aatpe::pid_identity::verifyAatpedPid((pid_t)pid);
}

// P2-9+18: thread_db is now a single atomic JSON file (thread_db.json) instead
// of a directory of per-tid key=value files. Parse it with a minimal extractor
// (the daemon controls the exact format, so we don't need a full JSON parser).
struct ThreadRecord{int pid,tid,priority;bool protect;unsigned long long nrSwitches,runTime;std::string pname,tname,role,subtype;};

// Pull the string value for "key":"..." out of a JSON object substring.
static std::string jsonStrField(const std::string& obj, const std::string& key){
    std::string pat="\""+key+"\":\"";
    size_t p=obj.find(pat); if(p==std::string::npos) return "";
    size_t s=p+pat.size(); size_t e=s;
    while(e<obj.size()){
        if(obj[e]=='\\'){e+=2;continue;}
        if(obj[e]=='"') break;
        ++e;
    }
    return obj.substr(s,e-s);
}
static long long jsonIntField(const std::string& obj, const std::string& key, long long def=0){
    std::string pat="\""+key+"\":";
    size_t p=obj.find(pat); if(p==std::string::npos) return def;
    size_t s=p+pat.size(); size_t e=s;
    while(e<obj.size() && (isdigit((unsigned char)obj[e])||obj[e]=='-')) ++e;
    if(e==s) return def;
    try{return std::stoll(obj.substr(s,e-s));}catch(...){return def;}
}

static std::vector<ThreadRecord> loadThreadDb(){
    std::vector<ThreadRecord> records;
    std::string path=std::string(aatpe::paths::kRunDir)+"/thread_db/thread_db.json";
    std::ifstream f(path); if(!f) return records;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Walk objects inside "threads":[ ... ].
    size_t arr=content.find("\"threads\":[");
    if(arr==std::string::npos) return records;
    size_t pos=arr+11; // strlen("\"threads\":")
    while(pos<content.size()){
        size_t o=content.find('{',pos); if(o==std::string::npos) break;
        // R8-013 (LED-120): find the MATCHING closing brace by depth, not the first
        // '}'. A thread object whose pname/tname string value contains a '}' (or any
        // nested object) previously truncated the record early and dropped fields.
        // Braces inside string literals are ignored.
        size_t c = std::string::npos;
        {
            int depth = 0;
            bool inStr = false;
            bool esc = false;
            for (size_t i = o; i < content.size(); ++i) {
                char ch = content[i];
                if (inStr) {
                    if (esc) { esc = false; }
                    else if (ch == '\\') { esc = true; }
                    else if (ch == '"') { inStr = false; }
                    continue;
                }
                if (ch == '"') { inStr = true; continue; }
                if (ch == '{') ++depth;
                else if (ch == '}') { --depth; if (depth == 0) { c = i; break; } }
            }
        }
        if(c==std::string::npos) break;
        std::string obj=content.substr(o,c-o+1);
        ThreadRecord r;
        r.pid=(int)jsonIntField(obj,"pid");
        r.tid=(int)jsonIntField(obj,"tid");
        r.pname=jsonStrField(obj,"pname");
        r.tname=jsonStrField(obj,"tname");
        r.role=std::to_string(jsonIntField(obj,"role"));
        r.subtype=jsonStrField(obj,"subtype");
        r.priority=(int)jsonIntField(obj,"priority");
        r.protect=(jsonIntField(obj,"protect",0)==1);
        r.nrSwitches=(unsigned long long)jsonIntField(obj,"nr_switches");
        r.runTime=(unsigned long long)jsonIntField(obj,"run_time");
        if(r.pid>0) records.push_back(r);
        pos=c+1;
    }
    return records;
}
static std::string roleToDisplayName(const std::string& roleStr){int role=safeStoi(roleStr,-1);if(role>=0&&role<role_names::kRoleCount)return role_names::getName(role);if(!roleStr.empty()&&roleStr[0]=='S')return roleStr;return role_names::kUnknown;}

void showHeader(){printf("%s",color(CYAN));printf("+=================================================================+\n");printf("|     AATPE - Adaptive Android Task Placement Engine              |\n");printf("|     CLI %s                                                       |\n",AATPE_VERSION);printf("+=================================================================+\n");printf("%s",color(NC));}

void cmdStatus(){
    showHeader();
    printf("\n=== SYSTEM STATUS ===\n");
    printf("Version:     %s\n",readFile((std::string(aatpe::paths::kRunDir)+"/version").c_str()).c_str());
    printf("Mode:        %s\n",readState("control_mode","assist").c_str());
    printf("Log Level:   %s\n",readState("log_level","2").c_str());
    printf("Profile:     %s\n",readState("current_profile","balanced").c_str());
    {std::string apkStr=readState("helper_apk_installed","0");printf("Helper APK:  %s\n",(apkStr=="1")?"Installed":"Not found");}
    {std::string apStr=readState("auto_profile","1"),modeStr=readState("auto_profile_mode","module_controlled");if(apStr=="1")printf("Auto Profile:Enabled (module)\n");else printf("Auto Profile:%s\n",(modeStr=="apk_controlled")?"Disabled (APK controlled)":"Disabled");}
    int dpid=readInt(aatpe::paths::kPidFile,0);bool running=(dpid>0)&&(kill(dpid,0)==0)&&isAatpedPid(dpid);
    printf("Daemon:      %s%s%s\n",color(running?GREEN:RED),running?"RUNNING":"STOPPED",color(NC));
    printf("Uptime:      %ss\n",readState("daemon_uptime","0").c_str());
    printf("\n=== THERMAL ===\n");
    {std::string cpuMax=readState("thermal_cpu_max","N/A");printf("CPU Max:     %s%s\n",cpuMax.c_str(),(cpuMax=="N/A")?"":"C");}
    {std::string skinStr=readState("thermal_skin","N/A");if(skinStr=="-1"||skinStr.empty())skinStr="N/A";printf("Skin:        %s%s\n",skinStr.c_str(),(skinStr=="N/A")?"":"C");}
    {std::string batStr=readState("thermal_battery","N/A");printf("Battery:     %s%s\n",batStr.c_str(),(batStr=="N/A")?"":"C");}
    {std::string hrStr=readState("thermal_headroom","N/A");printf("Headroom:    %s%s (%s%%)\n",hrStr.c_str(),(hrStr=="N/A")?"":"C",readState("thermal_headroom_pct","0").c_str());}
    printf("Throttle:    %s\n",readState("thermal_throttle","0").c_str());
    printf("Emergency:   %s\n",readState("thermal_emergency","0").c_str());
    printf("Skin Press:  %s\n",readState("thermal_skin_pressure","0").c_str());
    printf("Bat Press:   %s\n",readState("thermal_battery_pressure","0").c_str());
    printf("\n=== FRAME (V3) ===\n");
    // v4.1.2 hotfix: when SF latency is not actually measured (timestats disabled /
    // no game layer) the daemon still writes a theoretical-period fallback to
    // sf_latency_avg but flags sf_latency_valid=0. Do not present that as a measured
    // latency (the webui monitor applies the same gate).
    {std::string sfv=readState("sf_latency_valid","0");bool sfOk=(sfv=="1");
     printf("SF Latency:  %s\n",sfOk?readState("sf_latency_avg","N/A").c_str():"-- (未实测)");
     printf("SF Max:      %s\n",sfOk?readState("sf_latency_max","N/A").c_str():"-- (未实测)");}
    printf("Layers:      %s\n",readState("layer_count","N/A").c_str());
    printf("VSync:       %sHz\n",readState("vsync_rate","60").c_str());
    printf("Frame Crit:  %s\n",readState("frame_critical","0").c_str());
    printf("Jank:        %s\n",readState("jank_detected","0").c_str());
    printf("Safety Lat:  %s\n",readState("safety_latency_active","0").c_str());
    printf("Target Ft:   %sus\n",readState("target_frame_time","N/A").c_str());
    {std::string fgApp=readState("foreground_app","unknown"),fgSrc=readState("foreground_app_source","module"),srcLabel;if(fgSrc=="apk")srcLabel="APK";else if(fgSrc=="module_fallback")srcLabel="Module(fallback)";else srcLabel="Module";printf("Foreground:  %s [%s]\n",fgApp.c_str(),srcLabel.c_str());}
    {std::string apkStatus=readState("fg_apk_status","N/A"),apkAge=readState("fg_apk_age","N/A"),apkPath=readState("fg_apk_path","N/A");printf("APK Status:  %s (age=%ss)\n",apkStatus.c_str(),apkAge.c_str());if(apkPath!="N/A"&&!apkPath.empty())printf("APK Path:    %s\n",apkPath.c_str());}
    printf("\n=== CPU ===\n");
    printf("Count:       %s\n",readState("cpu_count","N/A").c_str());
    printf("LITTLE:      [%s]\n",readState("little_pool","").c_str());
    printf("MID:         [%s]\n",readState("mid_pool","").c_str());
    printf("BIG:         [%s]\n",readState("big_pool","").c_str());
    {int cpuCount=readInt((std::string(aatpe::paths::kStateDir)+"/cpu_count").c_str(),0);if(cpuCount>0){printf("\n--- Per-CPU Utilization ---\n");printf("%-6s %-8s %-12s %-8s %-6s\n","CPU","Util%","CurFreq(kHz)","Freq%","TempC");for(int c=0;c<cpuCount;c++){std::string base=std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(c);int util=readInt((base+"/util").c_str(),-1),curFreq=readInt((base+"/cur_freq").c_str(),0),freqPct=readInt((base+"/freq_pct").c_str(),0),temp=readInt((base+"/temp").c_str(),0);std::string utilStr=(util>=0)?std::to_string(util)+"%":"N/A";printf("%-6d %-8s %-12d %-8d %-6d\n",c,utilStr.c_str(),curFreq,freqPct,temp);}}}
    printf("\n=== METRICS ===\n");
    printf("Threads:     %s\n",readState("tracked_threads","0").c_str());
    printf("Decisions:   %s\n",readState("decision_count","0").c_str());
    printf("Migrations:  %s\n",readState("migration_count","0").c_str());
    printf("Load:        %s\n",readState("system_load","0").c_str());
    printf("\n=== PLACEMENT ===\n");
    printf("Success:     %s\n",readState("apply_success","0").c_str());
    printf("Failed:      %s\n",readState("apply_failed","0").c_str());
    printf("Cooldown:    %s\n",readState("apply_cooldown_skip","0").c_str());
    printf("CpusetSkip:  %s\n",readState("apply_cpuset_skip","0").c_str());
    printf("KernelSkip:  %s\n",readState("apply_kernel_skip","0").c_str());
    printf("Unmigratable:%s\n",readState("apply_unmigratable_skip","0").c_str());
    printf("UnmigCount:  %s\n",readState("unmigratable_count","0").c_str());
    printf("Interval Skip:%s\n",readState("apply_interval_skip","0").c_str());
    printf("\n=== BATTERY ===\n");
    int batLevel=readInt("/sys/class/power_supply/battery/capacity",-1);
    std::string batStatus=readFile("/sys/class/power_supply/battery/status");
    // P3-3: named string variable before .c_str()
    std::string batLevelStr=batLevel>=0?(std::to_string(batLevel)+"%"):"N/A";
    printf("Level:       %s\n",batLevelStr.c_str());
    printf("Status:      %s\n",batStatus.empty()?"N/A":batStatus.c_str());
    printf("\n=== EVENT DRIVEN ===\n");
    printf("Inotify:     %s\n",readState("event_inotify_active","0")=="1"?"active":"polling");
    printf("Jank:        %s\n",readState("event_jank_count","0").c_str());
    printf("Thermal:     %s\n",readState("event_thermal_count","0").c_str());
    printf("FreqChange:  %s\n",readState("event_freq_count","0").c_str());
    printf("Hotplug:     %s\n",readState("event_hotplug_count","0").c_str());
    printf("AppSwitch:   %s\n",readState("event_app_switch_count","0").c_str());
    printf("Total:       %s\n",readState("event_total","0").c_str());
    printf("\n=== FRAME SYNC ===\n");
    printf("Enabled:     %s\n",readState("vsync_enabled","0")=="1"?"yes":"no");
    printf("RefreshRate: %sHz\n",readState("vsync_refresh_rate","60").c_str());
    printf("FrameInt:    %sms\n",readState("vsync_frame_interval","16").c_str());
    printf("Decisions:   %s\n",readState("vsync_decisions","0").c_str());
    printf("Migrations:  %s\n",readState("vsync_migrations","0").c_str());
    printf("Overruns:    %s\n",readState("vsync_overruns","0").c_str());
    printf("\n=== PREDICTIVE ===\n");
    printf("MAE:         %s\n",readState("prediction_mae","0").c_str());
    printf("Confidence:  %s\n",readState("prediction_confidence","0.5").c_str());
    printf("Proactive:   %s\n",readState("prediction_proactive_migrations","0").c_str());
}

// P2-9+18: buildTidToNameMap now derived from the JSON thread_db (no directory walk).
static std::map<int,std::string> buildTidToNameMap(){
    std::map<int,std::string> m;
    for (const auto& r : loadThreadDb()) {
        std::string tname = r.tname.empty() ? ("tid-"+std::to_string(r.tid)) : r.tname;
        if (tname.length() > 20) tname.resize(20);
        m[r.tid] = tname;
    }
    return m;
}
void cmdThreads(){auto records=loadThreadDb();printf("=== THREAD DATABASE ===\n");int count=0;for(const auto& r:records){if(count>=cli_display::kThreadsTruncate){printf("... (truncated, total: %s)\n",readState("tracked_threads","0").c_str());break;}printf("[%3d] %5d/%5d %-20s:%-20s role=%-14s sub=%-12s prio=%2d protect=%s\n",count,r.pid,r.tid,r.pname.substr(0,20).c_str(),r.tname.substr(0,20).c_str(),roleToDisplayName(r.role).c_str(),r.subtype.c_str(),r.priority,r.protect?"Y":"N");count++;}if(count==0)printf("No thread data. Daemon may not be running.\n");}
void cmdTop(){auto records=loadThreadDb();printf("=== TOP THREADS BY ACTIVITY ===\n");printf("%-6s %-6s %-20s %-20s %-14s %-10s %-10s\n","PID","TID","Process","Thread","Role","Switches","AvgRun(us)");printf("-------------------------------------------------------------------------------\n");std::sort(records.begin(),records.end(),[](const ThreadRecord&a,const ThreadRecord&b){return a.nrSwitches>b.nrSwitches;});int count=0;for(const auto& r:records){if(count>=cli_display::kTopTruncate)break;unsigned long long ns=r.nrSwitches;if(ns==0)ns=1;unsigned long long avgRun=r.runTime/ns/1000;printf("%-6d %-6d %-20s %-20s %-14s %-10llu %-10llu\n",r.pid,r.tid,r.pname.substr(0,20).c_str(),r.tname.substr(0,20).c_str(),roleToDisplayName(r.role).c_str(),r.nrSwitches,avgRun);count++;}}

struct DecisionRecord{time_t ts;int pid,tid,targetCpu;std::string role,reason;};
std::vector<DecisionRecord> loadDecisions(){
    std::vector<DecisionRecord> records;DIR* dir=opendir(aatpe::paths::kDecisionDir);if(!dir)return records;struct dirent* entry;
    while((entry=readdir(dir))!=nullptr){if(entry->d_name[0]=='.')continue;std::string path=std::string(aatpe::paths::kDecisionDir)+"/"+entry->d_name;std::ifstream f(path);if(!f)continue;DecisionRecord r;r.ts=0;r.pid=0;r.tid=0;r.targetCpu=0;std::string line;
        while(std::getline(f,line)){trim(line);size_t eq=line.find('=');if(eq==std::string::npos)continue;std::string key=line.substr(0,eq),val=line.substr(eq+1);trim(key);trim(val);
            if(key=="pid")r.pid=safeStoi(val);else if(key=="tid")r.tid=safeStoi(val);else if(key=="target_cpu")r.targetCpu=safeStoi(val);else if(key=="role")r.role=val;else if(key=="reason")r.reason=val;else if(key=="timestamp")r.ts=(time_t)safeStoll(val);}
        if(r.pid>0)records.push_back(r);}
    closedir(dir);std::sort(records.begin(),records.end(),[](const DecisionRecord&a,const DecisionRecord&b){return a.ts>b.ts;});return records;
}
void cmdDecisions(int limit){auto records=loadDecisions();auto tidNameMap=buildTidToNameMap();printf("=== RECENT DECISIONS (last %d) ===\n",limit);if(records.empty()){printf("No decisions recorded.\n");return;}int count=0;for(const auto& r:records){if(count>=limit)break;char tsBuf[32];struct tm tm_buf;localtime_r(&r.ts,&tm_buf);strftime(tsBuf,sizeof(tsBuf),"%H:%M:%S",&tm_buf);std::string tname;auto it=tidNameMap.find(r.tid);if(it!=tidNameMap.end())tname=it->second;else tname="tid-"+std::to_string(r.tid);std::string roleName=roleToDisplayName(r.role);printf("[%s] %d/%d (%s, %s) -> CPU%-2d %s\n",tsBuf,r.pid,r.tid,tname.c_str(),roleName.c_str(),r.targetCpu,r.reason.c_str());count++;}}

void cmdThermal(){
    printf("=== THERMAL ZONES ===\n");printf("%-35s %6s %s\n","Name","Temp","Status");printf("---------------------------------------------------------------\n");
    DIR* dir=opendir("/sys/class/thermal");if(dir){struct dirent* entry;while((entry=readdir(dir))!=nullptr){if(strncmp(entry->d_name,"thermal_zone",12)!=0)continue;std::string typePath=std::string("/sys/class/thermal/")+entry->d_name+"/type",tempPath=std::string("/sys/class/thermal/")+entry->d_name+"/temp";std::string typeName=readFile(typePath.c_str());int temp=readInt(tempPath.c_str(),-1);if(typeName.empty()||temp<0)continue;if(typeName.length()>35)typeName.resize(35);if(temp>cli_display::kTempMilliScale)temp/=cli_display::kTempMilliScale;const char* status="OK";if(temp>=cli_display::kThermalCrit)status="CRIT";else if(temp>=cli_display::kThermalHot)status="HOT";else if(temp>=cli_display::kThermalWarn)status="WARN";printf("%-35s %5dC %s%s%s\n",typeName.c_str(),temp,color(temp>=cli_display::kThermalHot?RED:(temp>=cli_display::kThermalWarn?YELLOW:GREEN)),status,color(NC));}closedir(dir);}
    printf("\n=== CPU FREQUENCIES ===\n");dir=opendir("/sys/devices/system/cpu");if(dir){struct dirent* entry;while((entry=readdir(dir))!=nullptr){if(strncmp(entry->d_name,"cpu",3)!=0)continue;char* end;errno=0;long cpuIdL=strtol(entry->d_name+3,&end,10);if(*end!='\0'||errno==ERANGE||cpuIdL<0||cpuIdL>1023)continue;int cpuId=(int)cpuIdL;std::string base=std::string("/sys/devices/system/cpu/")+entry->d_name+"/cpufreq";int cur=readInt((base+"/scaling_cur_freq").c_str(),0),maxf=readInt((base+"/scaling_max_freq").c_str(),0);std::string gov=readFile((base+"/scaling_governor").c_str());if(maxf>0){int pct=cur*100/maxf;printf("CPU%-2d: %7d / %7d kHz (%3d%%) %s\n",cpuId,cur,maxf,pct,gov.c_str());}}closedir(dir);}
}

struct CpuMetricLite{int capacity=topology::kDefaultCapacity,util=0,temp=0;};
static std::map<int,CpuMetricLite> g_cliCpuMetrics;
static std::vector<int> g_cliLittlePool,g_cliMidPool,g_cliBigPool;
static std::string g_cliProfile="balanced";
// P1-6/P1-7: extracted static parsePoolStr for reuse by cmdPredict/cmdTopology
static std::vector<int> parsePoolStr(const std::string& s){std::vector<int> v;std::stringstream ss(s);std::string token;while(std::getline(ss,token,',')){trim(token);if(!token.empty()){try{v.push_back(std::stoi(token));}catch(const std::exception&){/* malformed token: skip */}}}return v;}
static void cliLoadCpuMetrics(){int cpuCount=readInt((std::string(aatpe::paths::kStateDir)+"/cpu_count").c_str(),0);for(int c=0;c<cpuCount;c++){CpuMetricLite m;m.capacity=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(c)+"/capacity").c_str(),topology::kDefaultCapacity);m.util=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(c)+"/util").c_str(),0);m.temp=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(c)+"/temp").c_str(),0);g_cliCpuMetrics[c]=m;}g_cliLittlePool=parsePoolStr(readState("little_pool",""));g_cliMidPool=parsePoolStr(readState("mid_pool",""));g_cliBigPool=parsePoolStr(readState("big_pool",""));g_cliProfile=readState("current_profile","balanced");}
static int cliCapacityScore(int cpuId,int role){auto it=g_cliCpuMetrics.find(cpuId);if(it==g_cliCpuMetrics.end())return 0;int cap=it->second.capacity;if(role==0)return 0;if(role==5)return capacity::kBackgroundInverseRef-cap;return cap;}
static int cliLatencyScore(int role){switch(role){case 1:return latency::kScoreUi;case 2:return latency::kScoreRender;case 3:return latency::kScoreGame;case 4:return latency::kScoreThroughput;default:return 0;}}
static int cliThermalScore(int cpuId){auto it=g_cliCpuMetrics.find(cpuId);if(it==g_cliCpuMetrics.end())return 0;int temp=it->second.temp;int penalty=(temp>thermal::kHighTempThreshold)?(temp-thermal::kHighTempThreshold)*thermal::kPenaltyPerDegree:0;int boost=(temp<thermal::kLowTempThreshold)?(thermal::kLowTempThreshold-temp)*thermal::kBoostPerDegree:0;return boost-penalty;}
static int cliPowerScore(int role){if(g_cliProfile=="battery"){if(role==5)return power::kBatteryBgScore;if(role==4)return power::kBatteryThroughputScore;return power::kBatteryOtherScore;}else if(g_cliProfile=="gaming"){if(role==3||role==2||role==1)return power::kgamingInteractiveScore;if(role==5)return power::kgamingBgScore;return power::kgamingOtherScore;}else if(g_cliProfile=="thermal"){if(role==5)return power::kThermalBgScore;return power::kThermalOtherScore;}return 0;}
static int cliPoolScore(int cpuId,int role){bool inLittle=std::find(g_cliLittlePool.begin(),g_cliLittlePool.end(),cpuId)!=g_cliLittlePool.end(),inMid=std::find(g_cliMidPool.begin(),g_cliMidPool.end(),cpuId)!=g_cliMidPool.end(),inBig=std::find(g_cliBigPool.begin(),g_cliBigPool.end(),cpuId)!=g_cliBigPool.end();switch(role){case 1:if(inBig)return pool_score::kUiBig;if(inMid)return pool_score::kUiMid;if(inLittle)return pool_score::kUiLittle;break;case 2:if(inBig)return pool_score::kRenderBig;if(inMid)return pool_score::kRenderMid;if(inLittle)return pool_score::kRenderLittle;break;case 3:if(inBig)return pool_score::kGameBig;if(inMid)return pool_score::kGameMid;if(inLittle)return pool_score::kGameLittle;break;case 4:if(inBig)return pool_score::kThroughputBig;if(inMid)return pool_score::kThroughputMid;if(inLittle)return pool_score::kThroughputLittle;break;case 5:if(inLittle)return pool_score::kBackgroundLittle;if(inMid)return pool_score::kBackgroundMid;if(inBig)return pool_score::kBackgroundBig;break;}return 0;}
static int cliContentionPenalty(int cpuId){auto it=g_cliCpuMetrics.find(cpuId);if(it==g_cliCpuMetrics.end())return 0;int util=it->second.util;if(util>contention::kHighUtilThreshold)return contention::kHighUtilPenalty;if(util>contention::kMidUtilThreshold)return contention::kMidUtilPenalty;if(util>contention::kLowUtilThreshold)return contention::kLowUtilPenalty;return contention::kIdleBoost;}
static int cliComputeScore(int cpuId,const ThreadRecord& r){int role=safeStoi(r.role,5);if(role==0)return 0;return cliCapacityScore(cpuId,role)+cliLatencyScore(role)+cliThermalScore(cpuId)+cliPowerScore(role)+cliPoolScore(cpuId,role)+cliContentionPenalty(cpuId);}

void cmdExplain(int targetTid){auto records=loadThreadDb();bool found=false;for(const auto& r:records){if(r.tid!=targetTid)continue;cliLoadCpuMetrics();int role=safeStoi(r.role,5);printf("=== EXPLAIN: %s:%s ===\n",r.pname.c_str(),r.tname.c_str());printf("PID=%d TID=%d Role=%s Priority=%d\n",r.pid,r.tid,roleToDisplayName(r.role).c_str(),r.priority);printf("Profile: %s\n\n",g_cliProfile.c_str());printf("CPU Score Breakdown:\n");printf("%-6s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n","CPU","Cap","Lat","Therm","Power","Pool","Contend","TOTAL");printf("----------------------------------------------------------------\n");int cpuCount=readInt((std::string(aatpe::paths::kStateDir)+"/cpu_count").c_str(),0),bestCpu=-1,bestScore=-999999;for(int c=0;c<cpuCount;c++){int cap=cliCapacityScore(c,role),lat=cliLatencyScore(role),therm=cliThermalScore(c),pow=cliPowerScore(role),pool=cliPoolScore(c,role),cont=cliContentionPenalty(c),total=cap+lat+therm+pow+pool+cont;printf("%-6d %-8d %-8d %-8d %-8d %-8d %-8d %-8d\n",c,cap,lat,therm,pow,pool,cont,total);if(total>bestScore){bestScore=total;bestCpu=c;}}printf("\nRecommended CPU: %d (score=%d)\n",bestCpu,bestScore);found=true;break;}if(!found)printf("Thread %d not found in database.\n",targetTid);}

void cmdProfile(const char* profile){if(!profile||strlen(profile)==0){printf("Available profiles:\n");DIR* dir=opendir((std::string(aatpe::paths::kConfDir)+"/profiles").c_str());if(dir){struct dirent* entry;while((entry=readdir(dir))!=nullptr){std::string name=entry->d_name;if(name.length()>5&&name.substr(name.length()-5)==".conf")printf("  %s\n",name.substr(0,name.length()-5).c_str());}closedir(dir);}printf("\nUsage: aatpe-cli profile <name>\n");return;}std::string profileStr(profile?profile:"");for(char c:profileStr){if(!isalnum((unsigned char)c)&&c!='_'&&c!='-'){printf("Invalid profile name: %s (only alphanumeric, underscore, hyphen allowed)\n",profile);return;}}std::string pfile=std::string(aatpe::paths::kConfDir)+"/profiles/"+profileStr+".conf";struct stat st;if(stat(pfile.c_str(),&st)==0){std::string ctrlPath=std::string(aatpe::paths::kRunDir)+"/control";std::ofstream cf(ctrlPath,std::ios::app);if(cf){cf<<"set_profile="<<profile<<"\n";cf.close();}int dpid=readInt(aatpe::paths::kPidFile,0);if(dpid>0){if(isAatpedPid(dpid)){kill(dpid,SIGUSR1);}else{printf("PID %d is not aatped (possible PID reuse), aborting\n",dpid);}}printf("Profile: %s\n",profile);printf("Note: Daemon will apply on next cycle.\n");}else printf("Profile not found: %s\n",profile);}

void cmdBenchmark(){printf("=== AATPE BENCHMARK ===\n");auto records=loadThreadDb();int threadCount=(int)records.size(),cpuCount=readInt((std::string(aatpe::paths::kStateDir)+"/cpu_count").c_str(),0);printf("Tracked threads: %d\nCPU count: %d\n\n",threadCount,cpuCount);if(threadCount==0){printf("No thread data available. Start daemon first.\n");return;}if(cpuCount==0){printf("CPU count unknown. Run 'aatped --init-topology' first.\n");return;}cliLoadCpuMetrics();printf("Benchmark: computing real scores for all threads on all CPUs...\n");struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);long long totalOps=0,totalScore=0;for(int i=0;i<threadCount&&i<cli_display::kBenchmarkMaxThreads;i++){for(int c=0;c<cpuCount;c++){int s=cliComputeScore(c,records[i]);totalScore+=s;totalOps++;}}clock_gettime(CLOCK_MONOTONIC,&end);long long durationMs=(end.tv_sec-start.tv_sec)*1000+(end.tv_nsec-start.tv_nsec)/1000000;printf("Scored %lld CPU-thread pairs in %lldms (total score=%lld)\n",totalOps,durationMs,totalScore);if(durationMs>0)printf("Rate: %lld ops/sec\n",totalOps*1000/durationMs);}

void cmdLog(int lines){FILE* f=fopen(aatpe::paths::kLogFile,"r");if(!f){printf("Log file not found: %s\n",aatpe::paths::kLogFile);return;}fseek(f,0,SEEK_END);long long fileSize=ftell(f),pos=fileSize;int chunkSize=cli_display::kLogChunkSize;std::vector<std::string> allLines;std::string leftover;// stop chunked read early on SIGINT/SIGTERM
while(pos>0&&(int)allLines.size()<lines+10&&!g_cliInterrupted.load(std::memory_order_relaxed)){long long readSize=(pos<chunkSize)?pos:chunkSize;pos-=readSize;fseek(f,pos,SEEK_SET);char* buf=new char[readSize+1];size_t got=fread(buf,1,readSize,f);buf[got]='\0';std::string chunk(buf,got);delete[] buf;chunk+=leftover;size_t lastNewline=chunk.rfind('\n');if(lastNewline!=std::string::npos){leftover=chunk.substr(0,lastNewline);std::string rest=chunk.substr(lastNewline+1);std::stringstream ss(rest);std::string line;std::vector<std::string> chunkLines;while(std::getline(ss,line))chunkLines.push_back(line);for(auto it=chunkLines.rbegin();it!=chunkLines.rend();++it)allLines.push_back(*it);}else leftover=chunk;}if(!leftover.empty())allLines.push_back(leftover);fclose(f);if(g_cliInterrupted.load())printf("\n(interrupted)\n");printf("=== LAST %d LOG LINES ===\n",lines);int count=0;for(auto it=allLines.rbegin();it!=allLines.rend()&&count<lines;++it,++count)printf("%s\n",it->c_str());}

// P2-#17: wait for pid to disappear after SIGTERM, up to 5s.
static void waitForDaemonExit(int dpid) {
    if (dpid <= 0) return;
    for (int i = 0; i < 50; i++) {
        if (kill(dpid, 0) != 0) return;  // process gone
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Still alive after 5s. R8-002 (LED-109): re-verify the PID is still OUR daemon
    // before SIGKILL — in the 5s window the PID could have been recycled to an
    // unrelated process. Killing a reused PID would murder an innocent process.
    if (!isAatpedPid(dpid)) {
        fprintf(stderr, "aatpe-cli: pid=%d no longer identified as aatped (PID reuse?), NOT sending SIGKILL\n", dpid);
        return;
    }
    // Escalate to SIGKILL.
    fprintf(stderr, "aatpe-cli: pid=%d did not exit after 5s, sending SIGKILL\n", dpid);
    kill(dpid, SIGKILL);
    // R8-023 (LED-130): reap with a bounded WNOHANG loop instead of a blocking
    // waitpid(dpid,&st,0) which could hang the CLI forever if the child is not our
    // direct child (we are usually not — the daemon daemonizes).
    for (int i = 0; i < 20; i++) {
        int st;
        pid_t w = waitpid(dpid, &st, WNOHANG);
        if (w == dpid) break;                 // reaped
        if (kill(dpid, 0) != 0) break;       // gone; ECHILD if we reap later
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// G9-004: returns a process exit code (0 success, 1 operational failure,
// 2 usage error) so the full-function test can distinguish PASS/FAIL by exit code.
int cmdDaemon(const char* action) {
    if (!action) { printf("Usage: aatpe-cli daemon <start|stop|restart|status>\n"); return 2; }
    int dpid = readInt(aatpe::paths::kPidFile, 0);
    bool running = (dpid > 0) && (kill(dpid, 0) == 0) && isAatpedPid(dpid);
    if (strcmp(action, "start") == 0) {
        // G9-003: starting clears any prior explicit-stop flag so the watchdog
        // resumes watching the freshly launched daemon.
        cliClearStopRequest();
        if (running) { printf("Daemon already running (PID=%d)\n", dpid); return 0; }
        printf("Starting daemon...\n");
        pid_t child = fork();
        if (child < 0) {
            printf("Daemon fork failed: %s\n", strerror(errno));
            return 1;
        }
        if (child == 0) {
            execl("/system/bin/aatped", "aatped", "--daemon", nullptr);
            _exit(1);
        }
        // Issue-20: bounded wait loop to reap the intermediate child (aatped --daemon
        // double-forks; the first child exits after daemonizing). Previously only two
        // WNOHANG polls were done, leaving a zombie if daemonization took >1s.
        {
            int status;
            for (int i = 0; i < 50; i++) {
                pid_t w = waitpid(child, &status, WNOHANG);
                if (w == child) break;
                if (w < 0) break;  // ECHILD — already reaped
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        dpid = readInt(aatpe::paths::kPidFile, 0);
        running = (dpid > 0) && (kill(dpid, 0) == 0) && isAatpedPid(dpid);
        if (running) { printf("Daemon started (PID=%d)\n", dpid); return 0; }
        printf("Daemon start failed. Check log: %s\n", aatpe::paths::kLogFile);
        return 1;
    } else if (strcmp(action, "stop") == 0) {
        if (running) {
            if (isAatpedPid(dpid)) {
                // G9-003: drop the explicit-stop flag BEFORE signalling so that even
                // if the watchdog polls in the same window, it sees the request and
                // does NOT revive the daemon we are about to kill.
                cliSetStopRequest();
                kill(dpid, SIGTERM);
                // P2-#17: actually wait for the process to exit before returning.
                waitForDaemonExit(dpid);
                unlink(aatpe::paths::kPidFile);
                unlink((std::string(aatpe::paths::kRunDir) + "/daemon.lock").c_str());
                printf("Daemon stopped (was PID=%d). Watchdog will not restart until `daemon start`.\n", dpid);
                return 0;
            } else {
                printf("PID %d is not aatped (possible PID reuse), aborting\n", dpid);
                return 1;
            }
        }
        // Already stopped: make the stop idempotent but still arm the flag so a
        // half-started daemon (or watchdog race) does not immediately revive it.
        cliSetStopRequest();
        printf("Daemon not running\n");
        return 0;
    } else if (strcmp(action, "restart") == 0) {
        int rc1 = cmdDaemon("stop");
        int rc2 = cmdDaemon("start");
        return (rc1 == 0 && rc2 == 0) ? 0 : 1;
    } else if (strcmp(action, "status") == 0) {
        if (running) printf("Daemon %sRUNNING%s (PID=%d)\n", color(GREEN), color(NC), dpid);
        else printf("Daemon %sSTOPPED%s\n", color(RED), color(NC));
        return 0;
    } else {
        printf("Usage: aatpe-cli daemon <start|stop|restart|status>\n");
        return 2;
    }
}

void cmdModelStatus(){printf("=== ML MODEL STATUS (Per-Pool) ===\n\n");LearningModel model;model.init(3);bool loaded=model.load(cliMlModelPath().c_str());if(!loaded){printf("Model file not found: %s\nStatus: COLD START (zero weights, alpha=0)\nPools: 3 (LITTLE/MID/BIG)\n",cliMlModelPath().c_str());return;}bool mature=model.isMature();printf("ML Maturity: %s (confidence gate: %s)\n",mature?"MATURE":"IMMATURE (cold/early warm start)",mature?"ENABLED":"DISABLED (migrations allowed freely)");printf("  - global alpha: %.4f (threshold >0.1)\n",(double)model.getAlpha());printf("  - total samples: %d (threshold >100)\n",model.getTotalSamples());for(int p=0;p<model.getPoolCount();p++){PoolModel pm=model.getPoolModel(p);printf("  - pool %d samples: %d (threshold >20)\n",p,pm.sample_count);}printf("\n");std::string status=model.getStatusString();printf("%s",status.c_str());}
void cmdModelReset(){printf("Resetting ML model...\n");int ret=unlink(cliMlModelPath().c_str());if(ret==0){printf("Model file deleted: %s\nDaemon will cold start (zero weights) on next restart.\n",cliMlModelPath().c_str());}else{printf("Model file not found or cannot delete: %s (errno=%d)\nModel may already be in cold start state.\n",cliMlModelPath().c_str(),errno);}std::string ctrlPath=std::string(aatpe::paths::kRunDir)+"/control";std::ofstream cf(ctrlPath,std::ios::app);if(cf){cf<<"model_reset=1\n";cf.close();}}
// G9-010: model.dat has a single writer (the daemon). Previously the CLI ALSO
// opened and overwrote model.dat itself right after telling the daemon to save,
// so two writers could race and produce a torn/half-written model. Now the CLI
// only sends the control command; the daemon performs the atomic save.
int cmdModelSave(){printf("Saving ML model...\n");std::string ctrlPath=std::string(aatpe::paths::kRunDir)+"/control";std::ofstream cf(ctrlPath,std::ios::app);if(cf){cf<<"model_save=1\n";cf.close();printf("Save request sent to daemon.\n");return 0;}printf("Cannot write to control file: %s\n",ctrlPath.c_str());return 1;}

void cmdPredict(int targetTid){
    printf("=== ML PREDICTION (Per-Pool): TID %d ===\n\n",targetTid);
    LearningModel model;model.init(3);bool loaded=model.load(cliMlModelPath().c_str());
    if(!loaded)printf("Model not loaded (cold start). All predictions will be 0.5 (sigmoid(0)).\n");
    auto records=loadThreadDb();const ThreadRecord* found=nullptr;
    for(const auto& r:records){if(r.tid==targetTid){found=&r;break;}}
    if(!found){printf("Thread %d not found in thread_db. Daemon may not be running.\n",targetTid);return;}
    printf("Thread: %s:%s (PID=%d)\n",found->pname.c_str(),found->tname.c_str(),found->pid);
    printf("Role: %s  Priority: %d\n",roleToDisplayName(found->role).c_str(),found->priority);
    printf("Model samples: %d  Alpha: %.4f\n\n",model.getTotalSamples(),model.getAlpha());
    printf("%-10s %-12s %-12s %-12s %-20s\n","Pool","ModelScore","NormHeuristic","Fused(alpha)","CPUs");
    printf("--------------------------------------------------------------------------------\n");
    unsigned long long avgRunUs=0;if(found->nrSwitches>0)avgRunUs=found->runTime/found->nrSwitches/1000;
    bool isGame=(safeStoi(found->role)==3);
    int cpuCount=readInt((std::string(aatpe::paths::kStateDir)+"/cpu_count").c_str(),8);
    // P1-7: read pools from state, fallback only when empty
    std::vector<int> poolCpus[3];
    poolCpus[0]=parsePoolStr(readState("little_pool",""));
    poolCpus[1]=parsePoolStr(readState("mid_pool",""));
    poolCpus[2]=parsePoolStr(readState("big_pool",""));
    if(poolCpus[0].empty()&&poolCpus[1].empty()&&poolCpus[2].empty()){
        if(cpuCount>=8){for(int i=0;i<4;i++)poolCpus[0].push_back(i);for(int i=4;i<6;i++)poolCpus[1].push_back(i);for(int i=6;i<cpuCount;i++)poolCpus[2].push_back(i);}
        else if(cpuCount>=4){for(int i=0;i<cpuCount/2;i++)poolCpus[0].push_back(i);for(int i=cpuCount/2;i<cpuCount;i++)poolCpus[2].push_back(i);}
        else{for(int i=0;i<cpuCount;i++)poolCpus[2].push_back(i);}
    }
    const char* poolNames[]={"LITTLE","MID","BIG"};int bestPool=-1;float bestScore=-1.0f;
    for(int poolIdx=0;poolIdx<3;poolIdx++){
        int totalCap=0,totalTemp=0,totalUtil=0,count=0;
        for(int cpu:poolCpus[poolIdx]){totalCap+=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(cpu)+"/capacity").c_str(),380);totalTemp+=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(cpu)+"/temp").c_str(),0);totalUtil+=readInt((std::string(aatpe::paths::kStateDir)+"/cpu"+std::to_string(cpu)+"/util").c_str(),0);count++;}
        int capacity=(count>0)?totalCap/count:380,temp=(count>0)?totalTemp/count:0,util=(count>0)?totalUtil/count:0;
        ThreadFeatures features=LearningModel::extractFeatures(avgRunUs,found->nrSwitches,util,found->priority,0,isGame,temp,capacity,false,0);
        float modelScore=model.predict(poolIdx,features);
        int heuristic=capacity+(isGame?150:50)-(temp>70?(temp-70)*3:0);
        float normH=1.0f/(1.0f+std::exp(-((float)heuristic-100.0f)/150.0f));
        float alpha=model.getAlphaForPool(poolIdx);if(alpha<=0.0f)alpha=model.getAlpha();
        float fused=cliSoftmaxFuseScores(modelScore,normH*100.0f,1.0f)/100.0f;
        std::string cpuListStr;for(size_t i=0;i<poolCpus[poolIdx].size();i++){if(i>0)cpuListStr+=",";cpuListStr+=std::to_string(poolCpus[poolIdx][i]);}
        printf("%-10s %-12.4f %-12.4f %-12.4f %-20s\n",poolNames[poolIdx],modelScore,normH,fused,cpuListStr.c_str());
        if(fused>bestScore){bestScore=fused;bestPool=poolIdx;}
    }
    if(bestPool>=0)printf("\nRecommended Pool: %s (fused score=%.4f)\n",poolNames[bestPool],bestScore);
    else printf("\nNo recommendation available.\n");
}

void cmdHelp(){showHeader();printf("\nUsage: aatpe-cli <command> [args]\n\n");printf("Commands:\n");printf("  status              Show full system status\n  threads             List all tracked threads\n  top                 Show top threads by activity\n  decisions [N]       Show last N placement decisions\n  thermal             Show all thermal zones and CPU freqs\n  explain <tid>       Explain placement logic for thread\n  profile [name]      Show or switch profile\n  benchmark           Run placement benchmark\n  log [N]             Show last N log lines\n  daemon <cmd>        Control daemon (start/stop/restart/status)\n  model-status        Show ML model status (weights, samples, alpha)\n  model-reset         Reset ML model (cold start, zero weights)\n  model-save          Force save ML model to disk\n  shadow-dump [-o f]  Drain observe-only shadow samples as JSONL (0700 state dir)\n  predict <tid>       Predict ML model scores for a thread on all CPUs\n  topology            Show CPU topology (pools, cores, freqs)\n  version             Show version and build info\n  help                Show this help\n");printf("\nState directory: %s\nConfig directory: %s\n",aatpe::paths::kRunDir,aatpe::paths::kConfDir);}

#ifndef NDK_VERSION_STR
#define NDK_VERSION_STR "unknown"
#endif
static void cmdVersion(){printf("AATPE %s (versionCode %d)\n",AATPE_VERSION,AATPE_VERSION_CODE);printf("Build: NDK %s, clang %s, aarch64-linux-android29, minSdk=29\n",NDK_VERSION_STR,__clang_version__);printf("C++ standard: C++17, optimization: -O3\n");}

// P1-6 + P2-8: cmdTopology reads pools from state, capacity defaults -1
static void cmdTopology(){
    printf("=== CPU TOPOLOGY ===\n\n");
    std::vector<int> littlePool=parsePoolStr(readState("little_pool","")),midPool=parsePoolStr(readState("mid_pool","")),bigPool=parsePoolStr(readState("big_pool",""));
    auto poolOf=[&](int cpu)->const char*{if(std::find(littlePool.begin(),littlePool.end(),cpu)!=littlePool.end())return"LITTLE";if(std::find(midPool.begin(),midPool.end(),cpu)!=midPool.end())return"MID";if(std::find(bigPool.begin(),bigPool.end(),cpu)!=bigPool.end())return"BIG";return"UNKNOWN";};
    auto joinCpus=[](const std::vector<int>& v){std::string s;for(size_t i=0;i<v.size();i++){if(i)s+=",";s+=std::to_string(v[i]);}return s;};
    if(!littlePool.empty()||!midPool.empty()||!bigPool.empty()){printf("Pool layout (from module state):\n");printf("  LITTLE: CPU [%s]\n",joinCpus(littlePool).c_str());printf("  MID:    CPU [%s]\n",joinCpus(midPool).c_str());printf("  BIG:    CPU [%s]\n\n",joinCpus(bigPool).c_str());}
    else{printf("Pool layout (state not available, typical 8-core):\n");printf("  LITTLE: CPU 0-3 (efficiency cores)\n");printf("  MID:    CPU 4-6 (performance cores)\n");printf("  BIG:    CPU 7   (prime core)\n\n");}
    printf("%-6s %-10s %-12s %-12s %-10s\n","CPU","Pool","CurFreq","MaxFreq","Capacity");
    printf("------ ---------- ------------ ------------ ----------\n");
    for(int cpu=0;cpu<(int)g_cpuCount;cpu++){
        std::string base="/sys/devices/system/cpu/cpu"+std::to_string(cpu);
        int curFreq=readInt((base+"/cpufreq/scaling_cur_freq").c_str(),0)/1000;
        int maxFreq=readInt((base+"/cpufreq/scaling_max_freq").c_str(),0)/1000;
        int capacity=-1;int cpuCap=readInt((base+"/cpu_capacity").c_str(),-1);if(cpuCap>0)capacity=cpuCap;
        const char* pool=poolOf(cpu);
        if(capacity>=0)printf("%-6d %-10s %-12d %-12d %-10d\n",cpu,pool,curFreq,maxFreq,capacity);
        else printf("%-6d %-10s %-12d %-12d %-10s\n",cpu,pool,curFreq,maxFreq,"N/A");
    }
    printf("\nOnline CPUs: ");
    for(int cpu=0;cpu<(int)g_cpuCount;cpu++){
        // readInt() defaulted to 1 when the online file was
        // missing, so phantom cores (no cpu directory at all) were printed as
        // online. Gate on the cpu directory existing first. If the directory
        // exists but has no 'online' file (cpu0 cannot be hot-removed), treat
        // it as online; otherwise honor the file's value.
        std::string cpuDir="/sys/devices/system/cpu/cpu"+std::to_string(cpu);
        if(access(cpuDir.c_str(), F_OK)!=0) continue;
        std::string onlinePath=cpuDir+"/online";
        int online=1;
        if(access(onlinePath.c_str(), F_OK)==0){
            online=readInt(onlinePath.c_str(),1);
        }
        if(online)printf("%d ",cpu);
    }
    printf("\n");
}

// v4.2.5: `apps --json` — root-bridge fallback for the SukiSU WebView, which
// cannot reach http://127.0.0.1 even though root shell can. Emits the EXACT same
// JSON shape as GET /api/apps (single source: same tiered enumeration as
// frame.cpp listInstalledPackages + manual game_overrides.list).
static std::string cliJsonEsc(const std::string& s) {
    std::string o; o.reserve(s.size()+2);
    for (char c : s) {
        if (c=='"'||c=='\\') { o+='\\'; o+=c; }
        else if (c=='\n') o+="\\n";
        else if ((unsigned char)c<0x20) {}
        else o+=c;
    }
    return o;
}
static std::vector<std::string> cliParsePm(const std::string& out) {
    std::vector<std::string> pkgs; std::istringstream ss(out); std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("package:",0)==0) pkgs.push_back(line.substr(8));
    }
    return pkgs;
}
static std::vector<std::string> cliScanDataApp() {
    // /data/app/*/*/base.apk -> infer pkg from the dir two levels up.
    std::vector<std::string> pkgs;
    DIR* d0 = opendir("/data/app");
    if (!d0) return pkgs;
    struct dirent* e;
    while ((e=readdir(d0))) {
        if (e->d_name[0]=='.') continue;
        std::string p1 = std::string("/data/app/")+e->d_name;
        DIR* d1 = opendir(p1.c_str());
        if (!d1) continue;
        struct dirent* e2;
        while ((e2=readdir(d1))) {
            if (e2->d_name[0]=='.') continue;
            // entry like com.tencent.tmgp.sgame-xxxxxxxx
            std::string pkg = e2->d_name;
            size_t dash = pkg.find_last_of('-');
            if (dash != std::string::npos && dash>0) pkg.resize(dash);
            if (pkg.find('.')!=std::string::npos) pkgs.push_back(pkg);
        }
        closedir(d1);
    }
    closedir(d0);
    return pkgs;
}
static int cmdAppsJson() {
    std::vector<std::string> pkgs;
    static const char* cmds[] = {
        "/system/bin/pm list packages -3 2>/dev/null",
        "/system/bin/cmd package list packages -3 2>/dev/null",
        "/system/bin/pm list packages 2>/dev/null",
    };
    for (const char* c : cmds) {
        pkgs = cliParsePm(aatpeExec(c,"APPS_LIST"));
        if (!pkgs.empty()) break;
    }
    if (pkgs.empty()) pkgs = cliScanDataApp();

    std::string body = "{\"apps\":[";
    for (size_t i=0;i<pkgs.size();++i){ if(i)body+=",";
        std::string lab = aatpe::apklabel::labelForPackage(pkgs[i]);
        if (lab.empty()) lab = pkgs[i];
        body += "{\"pkg\":\""+cliJsonEsc(pkgs[i])+"\",\"label\":\""+cliJsonEsc(lab)+"\"}"; }
    body += "],\"manual\":[";
    // read manual override list directly (same file the daemon uses).
    std::ifstream mf("/data/adb/aatpe/game_overrides.list");
    std::string line; bool first=true;
    while (std::getline(mf,line)) {
        size_t a=line.find_first_not_of(" \t\r\n");
        size_t b=line.find_last_not_of(" \t\r\n");
        if (a==std::string::npos) continue;
        line=line.substr(a,b-a+1);
        if(!first) body+=",";
        body += "\""+cliJsonEsc(line)+"\"";
        first=false;
    }
    body += "]}";
    if (pkgs.empty()) body.insert(body.size()-1,
        ",\"note\":\"empty: pm/cmd/and /data/app scan all returned no packages (check root, SELinux)\"");
    fputs(body.c_str(), stdout); fputc('\n', stdout);
    return 0;
}

int main(int argc,char* argv[]){
    // install signal handlers early.
    // SIGPIPE ignored so writing to a closed pipe (e.g. `aatpe-cli log | head`)
    // does not kill the CLI; SIGINT/SIGTERM set g_cliInterrupted for long loops.
    signal(SIGPIPE, SIG_IGN);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = cliSignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    initCpuCount();
    if(!isatty(STDOUT_FILENO))g_useColor=false;
    std::string cmd=(argc>1)?argv[1]:"status";

    // root check. Most commands read/write /dev/aatpe which
    // requires root; without it the CLI previously silently failed.
    // version/help are safe to run as a normal user.
    bool needsRoot = !(cmd=="version"||cmd=="--version"||cmd=="-v"||
                       cmd=="help"||cmd=="--help"||cmd=="-h");
    if(needsRoot && getuid()!=0){
        fprintf(stderr,"Error: aatpe-cli '%s' requires root privileges\n", cmd.c_str());
        fprintf(stderr,"Hint: run with su/root. (version/help commands work without root.)\n");
        return 1;
    }

    if(cmd=="status"){cmdStatus();return 0;}
    if(cmd=="apps"){return cmdAppsJson();}  // v4.2.5: --json apps for root bridge
    if(cmd=="threads"){cmdThreads();return 0;}
    if(cmd=="top"){cmdTop();return 0;}
    if(cmd=="decisions"){int limit=20;if(argc>2){try{limit=std::stoi(argv[2]);}catch(...){limit=20;}}if(limit<1)limit=1;if(limit>10000)limit=10000;cmdDecisions(limit);return 0;}
    if(cmd=="thermal"){cmdThermal();return 0;}
    if(cmd=="explain"){if(argc>2){int tid=0;try{tid=std::stoi(argv[2]);}catch(...){printf("Invalid tid: %s\n",argv[2]);return 1;}cmdExplain(tid);return 0;}printf("Usage: aatpe-cli explain <tid>\n");return 2;}
    if(cmd=="profile"){cmdProfile(argc>2?argv[2]:nullptr);return 0;}
    if(cmd=="benchmark"){cmdBenchmark();return 0;}
    if(cmd=="log"){int lines=50;if(argc>2){try{lines=std::stoi(argv[2]);}catch(...){lines=50;}}if(lines<1)lines=1;if(lines>10000)lines=10000;cmdLog(lines);return 0;}
    if(cmd=="daemon"){return cmdDaemon(argc>2?argv[2]:nullptr);}
    if(cmd=="model-status"){cmdModelStatus();return 0;}
    if(cmd=="model-reset"){cmdModelReset();return 0;}
    if(cmd=="model-save"){return cmdModelSave();}
    // v6.0.0: shadow-ring JSONL export. Writes `shadow_dump=1` to the root-0600
    // control pipe (same auth channel as model_save=1 / set_profile=), waits for
    // the daemon to drain+atomic-write STATE_DIR/diagnostic/shadow_samples.jsonl,
    // then prints it. With -o <path> it copies the JSONL to that path instead.
    // Default output NEVER touches /sdcard; the daemon-side file is the 0700
    // private state/diagnostic dir.
    if(cmd=="shadow-dump"){
        std::string outPath;
        for(int i=2;i<argc;i++){
            std::string a=argv[i];
            if(a=="-o"&&i+1<argc){outPath=argv[++i];continue;}
            printf("Usage: aatpe-cli shadow-dump [-o <file>]\n");return 2;
        }
        std::string srcPath=std::string(aatpe::paths::kStateDir)+"/diagnostic/shadow_samples.jsonl";
        // Record the mtime (or "absent") of any PRE-EXISTING dump file BEFORE we
        // send the command. Without this, a slow daemon leaves an older dump on
        // disk and the first poll would print STALE samples that belong to a
        // previous drain — and an immediately-written empty file (ring empty now)
        // could race with a real drain landing moments later. We only accept a
        // file whose mtime is NEWER than this baseline (i.e. written by OUR
        // shadow_dump command). We send the command EXACTLY once; no retry.
        struct stat beforeSt;
        bool beforeExists = (::stat(srcPath.c_str(), &beforeSt) == 0);
        double baselineSec = beforeExists ? (double)beforeSt.st_mtime
                                          : -1.0;
        {
            std::string ctrlPath=std::string(aatpe::paths::kRunDir)+"/control";
            std::ofstream cf(ctrlPath,std::ios::app);
            if(!cf){printf("rejected: cannot open control: %s\n",aatpe::paths::kRunDir);return 1;}
            cf<<"shadow_dump=1\n"; cf.close();
            int dpid=readInt(aatpe::paths::kPidFile,0);
            if(dpid>0&&isAatpedPid(dpid)) kill(dpid,SIGUSR1);
        }
        // Bounded poll: accept only a file with mtime strictly newer than the
        // baseline. 40 x 50ms = 2.0s ceiling. An empty-but-fresh file means the
        // daemon drained an empty ring this round (legitimately) — we accept it
        // as "no samples" rather than falling through to stale data.
        std::string content;
        bool gotFresh = false;
        for(int i=0;i<40;i++){
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            struct stat st;
            if(::stat(srcPath.c_str(), &st) != 0) continue;
            double mt = (double)st.st_mtime;
            // File is "ours" if it did not exist before, or its mtime advanced.
            if(!beforeExists || mt > baselineSec + 0.0) {
                content = readFile(srcPath.c_str());
                gotFresh = true;
                break;
            }
        }
        if(!gotFresh){
            printf("timeout: daemon did not write a fresh shadow dump within 2s (no data)\n");
            return 0;
        }
        if(content.empty()){
            printf("(drained 0 shadow samples — ring empty or shadow sampling off)\n");
            return 0;
        }
        if(outPath.empty()){
            fwrite(content.data(),1,content.size(),stdout);
        }else{
            std::ofstream of(outPath,std::ios::trunc|std::ios::binary);
            if(!of){printf("cannot open output: %s\n",outPath.c_str());return 1;}
            of<<content; of.close();
            printf("wrote %zu bytes -> %s\n",content.size(),outPath.c_str());
        }
        return 0;
    }
    // v4.2.8: one-shot aggregated UI snapshot as JSON. Reads only STATE_DIR
    // files (no fresh dumpsys) so a root-bridge tick costs exactly one exec.
    if(cmd=="ui-snapshot"){
        DIR* d=opendir(aatpe::paths::kStateDir);
        if(!d){printf("{}\n");return 1;}
        printf("{");
        bool first=true;
        struct dirent* e;
        while((e=readdir(d))!=nullptr){
            std::string name=e->d_name;
            if(name=="."||name=="..") continue;
            if(name.size()>4 && name.compare(name.size()-4,4,".log")==0) continue;
            // v4.2.8 security: STATE_DIR holds credential material (e.g.
            // daemon_token). The UI snapshot is handed to the WebView renderer,
            // so never serialize tokens/secrets/key material. State filenames
            // are lowercase; substring-deny is sufficient here.
            if(strstr(name.c_str(),"token")||strstr(name.c_str(),"secret")||
               strstr(name.c_str(),"credential")||strstr(name.c_str(),"passwd")||
               strstr(name.c_str(),"private")||strstr(name.c_str(),"salt")||
               strstr(name.c_str(),"hmac_key")) continue;
            // frame_apps.json is inlined below as a nested object; do not also
            // emit an escaped-string copy under its raw filename.
            if(name=="frame_apps.json") continue;
            std::string path=std::string(aatpe::paths::kStateDir)+"/"+name;
            struct stat st;
            if(stat(path.c_str(),&st)!=0||!S_ISREG(st.st_mode)) continue;
            if(st.st_size>65536) continue;
            std::string val=readFile(path.c_str());
            std::string out=jsonEscapeStr(val);
            if(!first) printf(",");
            first=false;
            printf("\"%s\":\"%s\"",name.c_str(),out.c_str());
        }
        closedir(d);
        // Also inline frame_apps.json as a nested object (already JSON on disk).
        std::string faPath=std::string(aatpe::paths::kStateDir)+"/frame_apps.json";
        std::string fa=readFile(faPath.c_str());
        if(!fa.empty()){ if(!first) printf(","); printf("\"frame_apps_raw\":%s",fa.c_str()); first=false; }
        // v4.2.8: inline WEBUI_DIR percpu.json/system.json (small, exempt from 64KB).
        auto inlineJson=[&](const char* key,const std::string& p){
            std::string v=readFile(p.c_str());
            if(v.empty()) return;
            if(!first) printf(",");
            // G-002 fix: must set first=FALSE (matching the emit pattern used for
            // STATE_DIR files and frame_apps_raw above). The old code set first=TRUE,
            // so when BOTH percpu.json and system.json were non-empty the second
            // inlineJson() saw first==true and skipped the separator comma, yielding
            //   "percpu_raw":{...}"system_raw":{...}
            // which is illegal JSON and made the whole `ui-snapshot` fail JSON.parse
            // in the root bridge (then silently fall back to per-file cat).
            first=false;
            printf("\"%s\":%s",key,v.c_str());
        };
        inlineJson("percpu_raw", "/dev/aatpe/webui/percpu.json");
        inlineJson("system_raw", "/dev/aatpe/webui/system.json");
        // threads array (reuse loadThreadDb, capped 100).
        // v4.2.8 P2: JSON-escape thread comm (can contain '"'/'\\') so one odd
        // name cannot corrupt the whole snapshot JSON.
        auto jsonEsc=[](const std::string& v){ return jsonEscapeStr(v); };
        auto recs=loadThreadDb();
        if(!first) printf(",");
        first=false;
        printf("\"threads\":[");
        for(size_t i=0;i<recs.size()&&i<100;i++){
            if(i) printf(",");
            const auto& r=recs[i];
            printf("{\"tid\":%d,\"name\":\"%s\",\"role\":\"%s\",\"protect\":%s}",
                   r.tid, jsonEsc(r.tname).c_str(), jsonEsc(roleToDisplayName(r.role)).c_str(),
                   r.protect?"1":"0");
        }
        printf("]");
        printf("}\n");
        return 0;
    }
    // v4.2.7: generic write-command dispatcher used by the WebUI root-bridge
    // fallback. Writes one control line to RUN_DIR/control (append), wakes the
    // daemon with SIGUSR1. Strictly validates against the same charset the
    // daemon reader allows (aatped_control.cpp isValidControlChars) so a bad
    // argument can never be turned into shell metacharacters.
    if(cmd=="send"){
        if(argc<3){printf("Usage: aatpe-cli send <cmd=value>\n");return 2;}
        std::string c=argv[2];
        if(c.empty()||c.size()>256){printf("rejected: bad length\n");return 2;}
        for(char ch:c){
            bool ok=(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||
                    ch=='_'||ch=='.'||ch=='-'||ch=='='||ch=='&';
            if(!ok){printf("rejected: illegal char '%c'\n",ch);return 2;}
        }
        int eq=0; for(char ch:c) if(ch=='=') eq++;
        if(eq>3){printf("rejected: too many '='\n");return 2;}
        std::string ctrlPath=std::string(aatpe::paths::kRunDir)+"/control";
        std::ofstream cf(ctrlPath,std::ios::app);
        if(!cf){printf("rejected: cannot open control: %s\n",aatpe::paths::kRunDir);return 1;}
        cf<<c<<"\n"; cf.close();
        int dpid=readInt(aatpe::paths::kPidFile,0);
        if(dpid>0&&isAatpedPid(dpid)) kill(dpid,SIGUSR1);
        printf("sent: %s\n",c.c_str());
        return 0;
    }
    if(cmd=="predict"){if(argc>2){int tid=0;try{tid=std::stoi(argv[2]);}catch(...){printf("Invalid tid: %s\n",argv[2]);return 1;}cmdPredict(tid);return 0;}printf("Usage: aatpe-cli predict <tid>\n");return 2;}
    if(cmd=="topology"){cmdTopology();return 0;}
    if(cmd=="version"||cmd=="--version"||cmd=="-v"){cmdVersion();return 0;}
    if(cmd=="help"||cmd=="--help"||cmd=="-h"){cmdHelp();return 0;}
    printf("Unknown command: %s\n",cmd.c_str());
    printf("Run 'aatpe-cli help' for usage.\n");
    return 1;
}
