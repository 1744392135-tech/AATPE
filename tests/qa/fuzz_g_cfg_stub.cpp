// Minimal fuzz stub: the global daemon Config singleton. Constructed (so
// LearningModel default ctor from aatpe_ml prod is linked) but never used by the
// command-dispatcher parser under fuzz.
#include "aatped.h"
Config g_cfg;
