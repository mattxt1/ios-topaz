# Mesa freedreno on Moto G7 river

**Current on 21 October 2026:** Mesa 26.2.5 runs Gallium and OpenGL ES 3.1
on Adreno 506 through the RiverGPU IOKit service. The GPU clear, offscreen
GLES draw, and dynamically loaded GLES library draw passed on the phone.
CoreAnimation is still using its existing CPU renderer. Current integration
plan and phone state are in `../../HANDOFF.md`; dated diagnostic evidence is
in `../RIVER.md`.

## Source layout

`../../mesa/` is a separate shallow Mesa 26.2.5 tree. Its river changes are
exported in `patches/0001-freedreno-river-iokit-backend.patch` tracked by
`river-xnu/`. The current Mesa tree already contains these changes; apply
that patch only to a clean matching Mesa checkout. From the current Mesa
working tree, `git apply --reverse --check` against the exported patch
passed at handoff.

The patch includes:

- `freedreno-kmds=river`: `src/freedreno/drm/river/` talks to RiverGPU IOKit
  selectors GET_PARAM, BO_NEW, BO_FREE, SUBMIT, WAIT, CPU_PREP and LOG.
  BOs map with `IOConnectMapMemory64(type = handle)`.
- `include/river-libdrm/xf86drm.h`: small libdrm stand-in. `drmGetVersion()`
  returns `river` so freedreno chooses the native river backend.
- `river_gallium_test.c`: direct Gallium 64×64 clear, fence and readback.
- `river_gl_runtime.c/.h`: a surfaceless GLES frontend exposing context
  create/bind/destroy in `/usr/local/lib/librivergl.dylib`. Mesa's
  `libglapi_bridge` is linked whole so GL entry points are exported.
- `river_gles_test.c`: small executable linked to that dylib; compiles
  shaders, draws an orange triangle on blue into a 64×64 FBO and reads
  pixels back.
- Meson changes for the river backend and iOS `BUILDING_MESA` definition.

## Build on the Mac

From the handoff folder's `mesa/` directory, with Xcode iPhoneOS SDK and
Homebrew Bison installed:

```sh
python3 -m venv ../mesa-venv
../mesa-venv/bin/pip install meson ninja mako pyyaml packaging
brew install bison
env PATH="$(brew --prefix bison)/bin:$PWD/../mesa-venv/bin:$PATH" \
  ../mesa-venv/bin/meson setup build-ios \
  --cross-file ../river-xnu/mesa/ios-arm64.cross --wrap-mode=nodownload \
  -Dgallium-drivers=freedreno -Dfreedreno-kmds=river \
  -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
  -Dgbm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
  -Dllvm=disabled -Dshader-cache=disabled -Dzstd=disabled \
  -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
  -Dbuildtype=release
env PATH="$(brew --prefix bison)/bin:$PWD/../mesa-venv/bin:$PATH" \
  ../mesa-venv/bin/ninja -j4 -C build-ios \
  src/gallium/drivers/freedreno/river_gallium_test \
  src/gallium/drivers/freedreno/river_gles_test
```

The latter target also builds `librivergl.dylib`. Before packing a phone
image, ad-hoc sign both the dylib and test executable with `codesign -f -s -`.
Pack the dylib as `/usr/local/lib/librivergl.dylib` and the test as
`/usr/local/bin/river_gles_test` using `../river-xnu/tools/river-files.py`,
then locally repack the existing r49 kernel/ADT with `mkpayload.py` and
`shim-river/build.py`. Preserve the private RTC boot-arg value from the
previous local r49 payload; never copy it into Git. Test only with RAM-only
`fastboot boot`. The phone's ordinary GUI image is separate from these
PID 1 test images.

## Hardware results and fixes

| Boot | Observation |
| --- | --- |
| `r49-gallium-live-11.txt` | Gallium GPU clear: fence signalled in 34.42 ms, 4096/4096 RGBA8 pixels correct. |
| `r49-gallium-live-13.txt` | GLES 3.1 / FD506, FBO complete, shader link succeeded, near `ff4000ff`, far `0000ffff`, GL error 0, `PASS`. |
| `r49-gallium-live-14.txt` | Signed shared library loaded dynamically; the same GLES draw and readback passed. |

These logs are under private `../../logs-private/`, not Git. The signed
shared-library RAM image is ignored at
`../shim-river/out/river-r49-gles-dylib-boot.img` (SHA256
`db4dbf379ce6412a95c3206a3935fc46253efd217f3a77dc67fe3a6de3bc4674`).

The direct Gallium test initially crashed in `glsl_array_type()`: it had
bypassed Mesa's normal frontend and had not initialized the GLSL type
singleton. Explicit init/decref fixed it. The first GLES test crashed
inside `st_api_create_context()` because its frontend `get_param` callback
was null; supplying the callback fixed it. The dylib initially omitted GL
entry points because static linking dropped unreferenced objects; linking
`libglapi_bridge` whole exported them. These fixes were verified by the
passing phone tests, not inferred from build success alone.

## Next integration step

The public Pixel 1 port's `tools/sweetgl` is an interface reference for
CoreAnimation's EAGL/GLI renderer and IOSurface imports:
https://github.com/joranv1/ios-sailfish/tree/main/tools/sweetgl . River's
native KMD removes the need for its DRM emulation. Write an independent
adapter, first verify a controlled library load in backboardd, then route
EAGL context and GLI dispatch calls to Mesa and import or copy IOSurfaces.
A passing offscreen draw does not establish GPU-rendered UI.
