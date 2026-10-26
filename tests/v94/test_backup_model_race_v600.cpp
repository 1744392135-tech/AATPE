// test_backup_model_race_v600.cpp — v6.0.0 RTC-01.
//
// Regression test for the tmp-name collision in backupModelFile. The production
// body (aatped_main.cpp:187) used "<model.dat>.bak.tmp.<getpid()>". The control
// thread (model_save) and the periodic-save thread both call backupModelFile on
// the SAME model.dat AFTER round3 moved backup out of g_mlMutex; same pid => same
// tmp path, so one O_TRUNC truncated the other's in-flight stream and rename()
// published a torn .bak. The fix uses mkstemp() (unique leaf per call).
//
// NOTE: aatped_main.cpp is the giant daemon TU and cannot be cleanly linked into
// a host recipe (it pulls the whole main loop / g_clusters / property stubs).
// This test therefore carries the production backupModelFile body VERBATIM (with
// the logging calls stubbed) as a mirror, exactly as cpu_metrics_race mirrors the
// g_cpuMetrics lock contract. Under ThreadSanitizer it must show zero races and
// the final .bak must be byte-identical to the source (no torn mix) with no
// .tmp.* residue.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { \
    if (c) { g_pass++; printf("  [PASS] %s\n", m); } \
    else   { g_fail++; printf("  [FAIL] %s\n", m); } \
} while(0)

// ---- mirrored production helper (logging stubbed out) --------------------
// Verbatim copy of aatped_main.cpp backupModelFile AFTER the RTC-01 mkstemp fix.
static void backupModelFile(const char* path) {
    if (!path || !path[0]) return;
    std::string bak = std::string(path) + ".bak";
    std::string tmpBak = bak + ".tmp.XXXXXX";   // RTC-01: mkstemp unique per call
    int in = open(path, O_RDONLY);
    if (in < 0) return;
    int out = mkstemp(&tmpBak[0]);
    if (out < 0) { close(in); return; }
    {
        char buf[4096];
        ssize_t n;
        bool ok = true;
        while (true) {
            n = read(in, buf, sizeof(buf));
            if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (n == 0) break;
            ssize_t totalWritten = 0;
            while (totalWritten < n) {
                ssize_t w = write(out, buf + totalWritten, (size_t)(n - totalWritten));
                if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
                totalWritten += w;
            }
            if (!ok) break;
        }
        if (ok && fsync(out) != 0) {}
        close(out);
        if (ok) {
            if (rename(tmpBak.c_str(), bak.c_str()) != 0) unlink(tmpBak.c_str());
        } else {
            unlink(tmpBak.c_str());
        }
    }
    close(in);
}

// Read a whole file into a std::string (empty on error).
static std::string slurp(const std::string& p) {
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) return "";
    std::string s; char b[4096]; ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0) s.append(b, n);
    close(fd);
    return s;
}

int main() {
    printf("=== v6.0.0 RTC-01: concurrent backupModelFile (mkstemp, TSan-clean) ===\n");

    const std::string dir = "/tmp/backup_race_v600";
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    const std::string model = dir + "/model.dat";

    // Write a distinctive source model (~40KB so a torn interleaving is likely
    // to be detectable). Content: a deterministic repeating pattern.
    std::string src;
    for (int i = 0; i < 8000; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "REC %08d TheQuickBrownFoxJumpsOverTheLazyDog\n", i);
        src += line;
    }
    int wfd = open(model.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(wfd, src.data(), src.size()); close(wfd);

    // Two threads = control + periodic, hammering backup of the SAME model.dat.
    std::atomic<bool> stop{false};
    auto worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            backupModelFile(model.c_str());
        }
    };
    std::thread t1(worker), t2(worker);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    stop.store(true);
    t1.join(); t2.join();

    // Final .bak must be byte-identical to the source (no torn mix).
    std::string bak = slurp(model + ".bak");
    CHECK(bak.size() == src.size(), "final .bak size equals source (no truncation)");
    CHECK(bak == src, "final .bak content byte-identical (no torn/interleaved mix)");

    // No mkstemp tmp residue: list the dir; any *.tmp.* file is a leak.
    bool residue = false;
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strstr(e->d_name, ".tmp.")) { residue = true; }
        }
        closedir(d);
    }
    CHECK(!residue, "no .tmp.* residue left after concurrent backups");

    printf("\n=== backup_model_race_v600: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
