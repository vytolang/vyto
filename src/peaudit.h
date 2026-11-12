#ifndef VYTO_PEAUDIT_H
#define VYTO_PEAUDIT_H

/* Will these Windows binaries load on Windows 7? See peaudit.c.
 *
 * `exports` is the allowlist (lib/vyto/win/win7_exports.txt). Prints one
 * PASS/FAIL line per binary, with each problem indented above its FAIL, and
 * returns 0 when every binary is clean, 1 otherwise, 2 when the allowlist
 * cannot be read. */
int pe_audit(const char *exports, int nfiles, const char **files);

#endif
