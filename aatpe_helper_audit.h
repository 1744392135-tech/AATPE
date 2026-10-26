// AATPE - Helper directory content audit (#74)
// Ensures /data/adb/modules/aatpe/helper/ only contains public state files.
// Sensitive files (token, HMAC key, model, thread internals) must never be
// placed there — they belong in /dev/aatpe (0700 root-only).
#ifndef AATPE_HELPER_AUDIT_H
#define AATPE_HELPER_AUDIT_H

// Audit the helper directory. Removes or hardens any file that leaks sensitive
// state. Returns the number of sensitive files found and removed.
int helperDirAudit();

// Visible for unit tests: classify a basename as sensitive (true) or protected
// config/asset (false).
bool isSensitiveHelperFile(const char* name);

#endif // AATPE_HELPER_AUDIT_H
