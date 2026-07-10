# Freestanding runtime example

Proof that Volt runs with **no libc** — the profile that unlocks bare-metal /
embedded / display targets (see [docs/PORTABILITY.md](../../docs/PORTABILITY.md)
roadmap item 4).

`voltc build app.vt --freestanding` compiles the runtime and modules with
`-ffreestanding -DVT_NO_LIBC -fno-builtin` and emits `libapp.a` (no `main`, no
link step). The runtime touches the platform only through six hooks:

    vt_host_alloc / vt_host_realloc / vt_host_free
    vt_host_write / vt_host_write_err
    vt_host_abort

and exports `void vt_main(void)` for the embedder's startup to call.

## Run the host stand-in

    ./examples/freestanding/build.sh                 # defaults to 01_hello.vt
    ./examples/freestanding/build.sh examples/10_strings.vt

This compiles the runtime freestanding, asserts the runtime object references
*only* the six hooks (zero libc symbols), links it against
[host_hooks.c](host_hooks.c) — a libc-backed shim standing in for real hardware
— and runs it. No cross toolchain required.

## On real hardware

Replace `host_hooks.c` with a bump/pool allocator + your UART + a breakpoint,
and call `vt_main()` from the reset handler:

    void Reset_Handler(void) { vt_main(); for (;;) {} }

then link `libapp.a + host_hooks.o + startup.o` against your linker script with
`arm-none-eabi-gcc` (or any bare-metal C toolchain), e.g.:

    voltc build app.vt --freestanding \
        --cc 'arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb' -o libapp.a

Add `--no-float` on no-FPU parts (float→string becomes a stub) and note that
`--no-fs` is implied — `readfile`/`readlines` panic with "no filesystem".
