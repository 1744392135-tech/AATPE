// integrity_deps.cpp - host link deps for integrity.cpp.
// integrity.cpp is self-contained for file I/O (readFileToString) but references
// STATE_DIR/RUN_DIR and the writeFile() overloads + LOG_* (logMsg, from log_stub).
#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_integrity";
const char* RUN_DIR    = "/dev/aatpe_v94_integrity";

#include <fstream>
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int)                { return true; }
bool writeFile(const char*, long long)          { return true; }
bool writeFile(const char*, float)               { return true; }
