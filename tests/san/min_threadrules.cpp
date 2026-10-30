#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "aatped.h"
#include "aatped_utils.h"

std::vector<ThreadRule> g_rules;
const char* CONF_DIR  = SAN_CONF_DIR;
const char* STATE_DIR = "/tmp/san_min_state";
const char* RUN_DIR   = "/tmp/san_min_run";

void logMsg(int , const char* fmt, ...) {
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
