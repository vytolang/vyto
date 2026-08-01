#ifndef VYTO_EMIT_H
#define VYTO_EMIT_H

#include "ast.h"

/* Which entry stub the entry module gets. The three output shapes each have a
   different caller: a hosted OS runs main(), a bare-metal embedder calls
   vt_main() from its own startup, and a shared library is entered by whatever
   loaded it -- on Android that is the JNI boot thread in
   lib/vyto/mobile/android/native/src/jni_boot.c, which calls vyto_app_main(). */
typedef enum {
    ENTRY_EXE,          /* int main(int, char **) */
    ENTRY_FREESTANDING, /* void vt_main(void) */
    ENTRY_SHARED,       /* void vyto_app_main(void) */
} EntryMode;

/* Emit one checked module as C. Fills h_out (header) and c_out (source).
   is_entry adds the program entry stub, shaped by mode. checks enables
   overflow-checked signed integer arithmetic (on for debug, off for --release). */
void emit_module(Module *m, bool is_entry, bool checks, EntryMode mode, SBuf *h_out,
                 SBuf *c_out);

#endif
