#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wait.h>
#include <vector>
#include <algorithm>
#include <string>

using clk = std::chrono::steady_clock;

static double backup_new(const char* src, const char* bak) {
    auto t0 = clk::now();
    std::string tmp = std::string(bak) + ".tmp.XXXXXX";
    int in = ::open(src, O_RDONLY);
    if (in < 0) { std::perror("open src"); return -1; }
    int out = ::mkstemp(&tmp[0]);
    if (out < 0) { std::perror("mkstemp"); ::close(in); return -1; }
    char buf[4096];
    ssize_t n; bool ok = true;
    while (true) {
        n = ::read(in, buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; ok=false; break; }
        if (n == 0) break;
        ssize_t tw=0;
        while (tw < n) {
            ssize_t w = ::write(out, buf+tw, (size_t)(n-tw));
            if (w < 0) { if (errno==EINTR) continue; ok=false; break; }
            tw += w;
        }
        if (!ok) break;
    }
    if (ok) ::fsync(out);
    ::close(out);
    if (ok) ::rename(tmp.c_str(), bak); else ::unlink(tmp.c_str());
    ::close(in);
    return std::chrono::duration<double,std::nano>(clk::now()-t0).count();
}

static double backup_old(const char* src, const char* bak) {
    auto t0 = clk::now();
    std::string tmp = std::string(bak) + ".tmp." + std::to_string((long)::getpid());
    int in = ::open(src, O_RDONLY);
    if (in < 0) { std::perror("open src"); return -1; }
    int out = ::open(tmp.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (out < 0) { std::perror("open old tmp"); ::close(in); return -1; }
    char buf[4096];
    ssize_t n; bool ok = true;
    while (true) {
        n = ::read(in, buf, sizeof(buf));
        if (n < 0) { if (errno == EINTR) continue; ok=false; break; }
        if (n == 0) break;
        ssize_t tw=0;
        while (tw < n) {
            ssize_t w = ::write(out, buf+tw, (size_t)(n-tw));
            if (w < 0) { if (errno==EINTR) continue; ok=false; break; }
            tw += w;
        }
        if (!ok) break;
    }
    if (ok) ::fsync(out);
    ::close(out);
    if (ok) ::rename(tmp.c_str(), bak); else ::unlink(tmp.c_str());
    ::close(in);
    return std::chrono::duration<double,std::nano>(clk::now()-t0).count();
}

static double pct(std::vector<double>& v, double p){ std::sort(v.begin(),v.end()); return v[(size_t)std::llround(p*(v.size()-1))]; }

int main(int argc, char** argv) {
    long modelBytes = argc > 1 ? atol(argv[1]) : 64*1024;
    int rounds = argc > 2 ? atoi(argv[2]) : 15;
    const char* dir = "/tmp/aatpe_backup_bench";
    ::mkdir(dir, 0755);
    std::string src = std::string(dir) + "/model.dat";
    std::string bak = std::string(dir) + "/model.dat.bak";

    {
        int f = ::open(src.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
        char chunk[4096]; for (int i=0;i<4096;++i) chunk[i]=(char)(i&0xff);
        long left = modelBytes;
        while (left>0){ ssize_t w=::write(f,chunk,sizeof(chunk)); if(w<=0)break; left-=w; }
        ::fsync(f); ::close(f);
    }

    std::printf("=== backupModelFile cold-path bench ===\n");
    std::printf("model size = %ld bytes (%ld x 4KB blocks), rounds=%d\n",
                modelBytes, (modelBytes+4095)/4096, rounds);

    for (int i=0;i<5;++i){ backup_old(src.c_str(),bak.c_str()); backup_new(src.c_str(),bak.c_str()); }

    std::vector<double> ot, nt;
    for (int i=0;i<rounds;++i) {
        double o = backup_old(src.c_str(), bak.c_str());
        double n = backup_new(src.c_str(), bak.c_str());
        if (o>0) ot.push_back(o);
        if (n>0) nt.push_back(n);
    }
    double oMed=pct(ot,0.50), nMed=pct(nt,0.50);
    double o95=pct(ot,0.95),  n95=pct(nt,0.95);
    double delta=(nMed-oMed)/oMed*100.0;
    std::printf("\n%-18s | %12s | %12s\n","path","P50 (ns)","P95 (ns)");
    std::printf("%-18s | %12.1f | %12.1f\n","OLD getpid name", oMed, o95);
    std::printf("%-18s | %12.1f | %12.1f\n","NEW mkstemp",    nMed, n95);
    std::printf("\ndelta P50 (NEW-OLD)/OLD = %+.2f%%\n", delta);
    std::printf("absolute delta = %.1f ns = %.2f us\n", nMed-oMed, (nMed-oMed)/1e3);
    std::printf("sample n: old=%zu new=%zu\n", ot.size(), nt.size());

    ::unlink(src.c_str()); ::unlink(bak.c_str());
    return 0;
}
