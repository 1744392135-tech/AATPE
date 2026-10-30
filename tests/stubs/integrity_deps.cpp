#include "aatped.h"

const char* STATE_DIR = "/tmp/aatpe_v94_integrity";
const char* RUN_DIR    = "/dev/aatpe_v94_integrity";

#include <fstream>
bool writeFile(const char*, const std::string&) { return true; }
bool writeFile(const char*, int)                { return true; }
bool writeFile(const char*, long long)          { return true; }
bool writeFile(const char*, float)               { return true; }
