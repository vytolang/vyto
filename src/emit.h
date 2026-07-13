#ifndef VYTO_EMIT_H
#define VYTO_EMIT_H

#include "ast.h"

/* Emit one checked module as C. Fills h_out (header) and c_out (source).
   is_entry adds the program entry stub. checks enables overflow-checked signed
   integer arithmetic (on for debug, off for --release). freestanding emits an
   exported `void vt_main(void)` for a bare-metal embedder to call from its own
   startup, instead of a hosted `int main(int, char**)`. */
void emit_module(Module *m, bool is_entry, bool checks, bool freestanding, SBuf *h_out,
                 SBuf *c_out);

#endif
