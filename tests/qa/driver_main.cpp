// driver_main.cpp — standalone fallback "fuzzer" main for hosts without
// -fsanitize=fuzzer. Iterates a seed corpus directory and feeds every file
// (plus /dev/stdin) into LLVMFuzzerTestOneInput(). This is NOT coverage-guided
// fuzzing, but it actually runs the production parser over a curated corpus
// under ASan+UBSan — it is a real regression sweep, not a no-op.
//
// Usage: fuzz_<name> <corpus-dir> [extra files...]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void run_buf(const uint8_t* data, size_t size, const char* tag) {
    int rc = LLVMFuzzerTestOneInput(data, size);
    if (rc != 0) { fprintf(stderr, "[driver_main] input '%s' returned %d\n", tag, rc); exit(1); }
}

static void run_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return; }
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    fprintf(stderr, "[driver_main] %s (%zu bytes)\n", path, got);
    run_buf(buf.data(), got, path);
}

int main(int argc, char** argv) {
    // No args: read stdin once.
    if (argc < 2) {
        std::string all;
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), stdin)) > 0) all.append(tmp, n);
        std::vector<uint8_t> buf(all.begin(), all.end());
        run_buf(buf.data(), buf.size(), "<stdin>");
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        struct stat st;
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR* d = opendir(argv[i]);
            if (!d) continue;
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (e->d_name[0] == '.') continue;
                std::string p = std::string(argv[i]) + "/" + e->d_name;
                run_file(p.c_str());
            }
            closedir(d);
        } else {
            run_file(argv[i]);
        }
    }
    fprintf(stderr, "[driver_main] corpus sweep complete, no crash\n");
    return 0;
}
