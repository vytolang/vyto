/* vyto/media/audio — the one place miniaudio is configured.

   Both translation units include this before miniaudio.h: audio_ma.c, which
   instantiates the implementation, and audio_shim.c, which uses it. The
   defines must agree between them, or the two halves disagree about the size
   of ma_context and ma_device; keeping them here is how that holds.

   Everything above the device layer is compiled out. This package moves
   samples between a program and a device (see its README): decoding, encoding,
   waveform generation, the resource manager, the node graph and the engine are
   all somebody else's job, and leaving them in roughly doubles the code.

   Backends are a whitelist so a new upstream backend does not appear in
   everyone's binary unannounced. NULL is enabled but never chosen on its own:
   audio_shim.c leaves it out of the default list and only picks it when
   VYTO_AUDIO=null asks, which is how the tests run without a sound card. */
#ifndef VT_AUDIO_CONFIG_H
#define VT_AUDIO_CONFIG_H

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_DSOUND
#define MA_ENABLE_WINMM
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_COREAUDIO
#define MA_ENABLE_AAUDIO
#define MA_ENABLE_OPENSL
#define MA_ENABLE_NULL

#endif
