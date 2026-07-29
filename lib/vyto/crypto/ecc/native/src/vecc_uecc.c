/* Wrapper TU for micro-ecc's uECC.c.
 *
 * vytoc compiles the .c files in native/src flat and does not descend into
 * subdirectories, so native/src/microecc/uECC.c is never compiled alone — this file is
 * what the build sees, and it exists to set uecc_config.h's macros before
 * upstream's headers can install their own defaults. Same arrangement as the
 * vregex_*.c wrappers in lib/vyto/regex/native/src.
 *
 * Nothing under native/src/microecc/ is edited; it is byte-identical to the
 * v1.1 release and native/refresh-microecc.sh --verify proves it.
 */
#include "uecc_config.h"
#include "microecc/uECC.c"
