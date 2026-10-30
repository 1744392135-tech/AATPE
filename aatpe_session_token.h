#ifndef AATPE_SESSION_TOKEN_H
#define AATPE_SESSION_TOKEN_H

#include <string>

std::string aatpeSessionIssue(void);

bool aatpeSessionVerify(const std::string& token);

bool aatpeSessionAnyActive(void);

void aatpeSessionPurgeAll(void);

#ifdef AATPE_HOST_TEST
void aatpeSessionSetClockFailForTest(bool on);
#endif

#endif
