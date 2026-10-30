
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <cassert>

static std::mutex g_spillEntryMtx;

static int spilloverTickOld() {
    std::lock_guard<std::mutex> lk(g_spillEntryMtx);
    return 0;
}

static int spilloverTickFixed() {
    std::unique_lock<std::mutex> lk(g_spillEntryMtx, std::try_to_lock);
    if (!lk.owns_lock()) {
        return -1;
    }
    return 0;
}

int main() {
    {
        std::atomic<bool> returned{false};
        std::thread t([&]{ (void)spilloverTickOld(); returned = true; });
        t.join();
        if (!returned) { std::cerr << "spillover old-path thread did not return\n"; std::abort(); }
        std::cout << "[model] old-path unconditional lock: blocking-by-design (device symptom)\n";
    }

    {
        g_spillEntryMtx.lock();
        int rc = spilloverTickFixed();
        g_spillEntryMtx.unlock();
        assert(rc == -1);
        std::cout << "[model] fixed-path try_lock while held: rc=" << rc << " (skipped, no block) PASS\n";
    }

    {
        int rc = spilloverTickFixed();
        assert(rc == 0);
        std::cout << "[model] fixed-path uncontended: rc=" << rc << " (proceeds) PASS\n";
    }

    {
        std::atomic<bool> workerHeld{false}, go{false};
        std::thread w([&]{
            g_spillEntryMtx.lock();
            workerHeld = true;
            while (!go) std::this_thread::yield();
            g_spillEntryMtx.unlock();
        });
        while (!workerHeld) std::this_thread::yield();
        int rc1 = spilloverTickFixed();
        assert(rc1 == -1);
        go = true;
        w.join();
        int rc2 = spilloverTickFixed();
        assert(rc2 == 0);
        std::cout << "[model] bounded skip under contention then proceed: PASS\n";
    }

    std::cout << "ALL PASS\n";
    return 0;
}
