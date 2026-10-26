// Minimal, uniform-instrumentation host harness for W1-AU-004 re-verification.
// Single responsibility: call the REAL product loadThreadRules() on the REAL
// rules/thread_roles.conf, with ALL TUs compiled under the SAME ASan+UBSan flags.
// Provides the external globals the daemon would otherwise define.
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "aatped.h"
#include "aatped_utils.h"

// ---- external globals normally defined in aatped_main.cpp / logging TU ----
std::vector<ThreadRule> g_rules;
const char* CONF_DIR  = SAN_CONF_DIR;   // -DSAN_CONF_DIR=...
const char* STATE_DIR = "/tmp/san_min_state";
const char* RUN_DIR   = "/tmp/san_min_run";

void logMsg(int /*level*/, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

int main() {
    fprintf(stderr, "[min] calling loadThreadRules() with CONF_DIR=%s\n", SAN_CONF_DIR);
    loadThreadRules();
    fprintf(stderr, "[min] OK, g_rules.size()=%zu\n", g_rules.size());
    return 0;
}
