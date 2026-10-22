# Mesa freedreno for river (iOS 16 userland, Adreno 506 via RiverGPU)

Base: Mesa **mesa-26.2.5** (shallow clone in `../mesa`, not in this repo).
Patch: `patches/0001-freedreno-river-iokit-backend.patch` (apply with
`git apply` in the Mesa tree). It adds:
- `freedreno-kmds=river`: new backend `src/freedreno/drm/river/` (device,
  bo, pipe, softpin submit) over the RiverGPU IOKit user client
  (selectors: 0 GET_PARAM, 1 BO_NEW, 2 BO_FREE, 3 SUBMIT, 4 WAIT,
  5 CPU_PREP, 6 LOG; map = IOConnectMapMemory64(type = handle)).
- `include/river-libdrm/xf86drm.h`: libdrm stand-in; drmGetVersion() says
  "river" so fd_device_new() dispatches to river_device_new().
- meson: `river_libdrm_stub`, build src/freedreno/drm for river, msm files
  only with `msm`, skip src/util/u_sync_provider.c with the stub.
- `src/gallium/drivers/freedreno/river_gallium_test.c` + meson target
  `river_gallium_test`: fd_screen_create -> clear_render_target 64x64
  RGBA8 -> fence -> texture_map readback, logs via LOG selector, PID 1.

Build (Mac):
```
python3 -m venv ../mesa-venv && ../mesa-venv/bin/pip install meson ninja mako pyyaml packaging
brew install bison            # Apple bison 2.3 is too old for ir3_parser.y
export PATH=$(brew --prefix bison)/bin:$PWD/../mesa-venv/bin:$PATH
cd ../mesa && meson setup build-ios --cross-file ../river-xnu/mesa/ios-arm64.cross \
  -Dgallium-drivers=freedreno -Dfreedreno-kmds=river -Dvulkan-drivers= -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=true -Dgles1=disabled \
  -Dgles2=enabled -Dllvm=disabled -Dshader-cache=disabled -Dzstd=disabled \
  -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuildtype=release
ninja -C build-ios src/gallium/drivers/freedreno/river_gallium_test
```
Status 2026-10-21: `river_gallium_test` ran on the Moto G7. It created the
FD506 screen and context, compiled the blitter shaders, submitted a real GPU
clear, waited for its fence, and read back 4096/4096 correct RGBA8 pixels
(private USB log `../logs-private/r49-gallium-live-11.txt`). The first run
crashed in `glsl_array_type` because this direct Gallium test had not called
`glsl_type_singleton_init_or_ref()`; the test now initializes and releases
Mesa's GLSL type cache. Temporary `RIVER DIAG` source markers were removed.
The signed successful RAM image is ignored at
`shim-river/out/river-r49-gallium-glslinit-boot.img`.

Next: build a GLES frontend for CoreAnimation. The Pixel 1 iOS port at
https://github.com/joranv1/ios-sailfish/tree/main/tools/sweetgl provides a
working reference for Mesa's state tracker, EAGL/GLI dispatch, IOSurface
imports, and CoreAnimation shader compatibility. Its `mesa.patch` only
changes BO mmap and IOSurface pitch handling; it does not address our test's
GLSL type cache initialization. River's KMD is already implemented in
freedreno and can be used directly instead of its MSM DRM emulation.

Initial GLES smoke test: `src/gallium/drivers/freedreno/river_gles_test.c`
creates a Mesa ES 3 context through `st_api_create_context`, draws an
offscreen triangle, and reads back pixels. The iOS Meson path now defines
`BUILDING_MESA` like macOS, so Mesa's GL shader handles have the expected
integer type. Build with Homebrew Bison ahead of `/usr/bin`:
```
PATH="$(brew --prefix bison)/bin:$PWD/../mesa-venv/bin:$PATH" \
  ../mesa-venv/bin/ninja -j4 -C build-ios \
  src/gallium/drivers/freedreno/river_gles_test
```
The 19 MB arm64 executable builds and signs. First RAM boot created an FD506
screen, then PID 1 crashed during state tracker context creation (private
`r49-gallium-live-12.txt`). Mesa's current `st_api_create_context` calls
`fscreen->get_param`; the test now provides that callback. The second
RAM boot passed: Mesa reported OpenGL ES 3.1 / FD506, linked both shaders,
rendered to an FBO, and read back the expected triangle and clear colors
with GL error 0 (private `r49-gallium-live-13.txt`). The signed diagnostic
RAM image with context setup markers is at
`shim-river/out/river-r49-gles-diag1-boot.img` (SHA256
`9b19b4585068f92986a45112c6419c66f75f48320a54070067a79923d787ac0a`).
The clean source builds; temporary markers are only in that image.

Next: wrap the working GLES state tracker in a dylib, provide CoreAnimation's
EAGL and GLI calls, and import IOSurfaces as GPU resources. The Pixel port's
`tools/sweetgl/eagl.c` describes the EAGL entry points and GL dispatch
layout; its boot args inject the dylib into backboardd with
`DYLD_INSERT_LIBRARIES`. River currently has no equivalent environment boot
arg, so a controlled injection mechanism is needed before UI integration.
