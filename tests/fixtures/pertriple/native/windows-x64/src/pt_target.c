/* Only windows-x64 compiles this one. The marker string lets the test find
   it in the exe; a Linux build must not contain it. */
const char *pt_target(void) { return "windows-x64 src PT_WIN_MARKER"; }
