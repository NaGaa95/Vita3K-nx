# Switch PGO

Switch Release builds use the bundled GCC 16.1.0 gameplay profiles by default.
Five Vulkan gameplay runs train CPU/JIT helpers, memory, kernel/HLE, audio,
GXM and the shared/Vulkan renderer. The launcher, installer, OpenGL backend
and shader translator use their normal optimization settings.

Profiles are build inputs only. The application contains no collection
runtime, counters or profile writes. Untrained functions retain normal
optimization through GCC's partial-training mode.

Build with the usual Switch options. Use `-DVITA3K_SWITCH_PGO=OFF` for a
different compiler or a non-Release build. `VITA3K_SWITCH_PGO_DIR` can point
to an unpacked replacement profile set with `profiles.json` and `data/`.

Archive integrity, compiler version and individual profile hashes are
checked during configuration. Same-path builds reject changed function
coverage. Relocated checkouts warn and reuse compatible profiles; GCC leaves
unmatched functions normally optimized.
