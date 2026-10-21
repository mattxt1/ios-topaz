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
Status 2026-10-21: everything builds for iOS arm64, including
`build-ios/src/gallium/drivers/freedreno/river_gallium_test` (12.7 MB, linked
after the u_sync_provider fix). Signed and run on the phone: RiverGPU opened
and Mesa created an FD506 screen, then PID 1 SIGSEGV'd before context creation
returned. See RIVER.md r49g. The Mesa source tree currently has temporary
`RIVER DIAG` context markers; the diagnostic binary and RAM image are built.
