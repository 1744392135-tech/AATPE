#ifndef AATPED_PSI_H
#define AATPED_PSI_H

#include <atomic>

namespace aatpe {
namespace psi {

void init();

void tick();

bool noncriticalMigrationPaused() noexcept;
bool bigcoreDeprefer() noexcept;

#ifdef AATPE_HOST_TEST
void resetForTest();
#endif

void shutdown();

}
}

#endif
