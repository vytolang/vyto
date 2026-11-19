/* vyto/media/audio — the miniaudio implementation, instantiated once.

   Its own translation unit, apart from audio_shim.c, so the two compile in
   parallel (VYTO_JOBS) and the shim stays small enough to read. native/src is
   compiled flat and non-recursively, so src/miniaudio/ is never a TU of its
   own; this file is the only place the implementation exists.

   Freestanding builds (-DVT_NO_LIBC) get an empty TU: miniaudio needs pthreads,
   dlopen and libm, and audio_shim.c's stub arm answers every entry point
   without it. */
#ifndef VT_NO_LIBC

#include "audio_config.h"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

#else
/* ISO C forbids an empty translation unit. */
typedef int vt_audio_ma_unused;
#endif
