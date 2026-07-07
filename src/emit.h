#ifndef VOLT_EMIT_H
#define VOLT_EMIT_H

#include "ast.h"

/* Emit one checked module as C. Fills h_out (header) and c_out (source).
   is_entry adds the int main() stub. checks enables overflow-checked signed
   integer arithmetic (on for debug, off for --release). */
void emit_module(Module *m, bool is_entry, bool checks, SBuf *h_out, SBuf *c_out);

#endif
