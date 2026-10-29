# Staged renderer performance candidate

`eagl-clear-trace-queries.patch` targets the local Mesa working source at
`src/gallium/drivers/freedreno/sweetgl/eagl.c`. It avoids three diagnostic-only
GL state queries in every clear unless a trace window is active. Rendering,
texture barriers and GPU synchronization are unchanged.

The candidate passed an iOS16 arm64 object compile using the local Mesa
compile_commands.json flags. An ASan/UBSan mock executing the actual sgl_Clear
body verified that glClear still runs in every case and the state queries are
absent outside the diagnostic window. It has NOT been linked/deployed or
benchmarked. No FPS improvement is claimed.

The local Mesa source differs from the older exported0001 patch (including
rectangle-uniform fallback and profile changes); this candidate is intentionally
not part of an automatic patch series. Check source/applicability before use.
A broader negative-uniform cache was investigated but withheld because context
ownership and invalidation need further review.

Baseline eagl.c SHA256: `c2660743b033431782152bcfd82491166ce1b0e8f3f45f39c70de0913a342012`.

Current device GLES library was not modified during this investigation.
