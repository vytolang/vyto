/* A DLL linked against the Universal CRT, for the import audit's UCRT rule
   (tests/run_tests.sh, win_audit_ucrt_fails). Built with -lucrt, never run. */
#include <string.h>
__declspec(dllexport) int ucrt_probe(const char *a, const char *b) { return (int)strlen(a) + strcmp(a, b); }
