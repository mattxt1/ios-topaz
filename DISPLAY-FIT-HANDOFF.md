# River display-fit experiments — 2026-10-23

## Current phone state

The Moto G7 was restored with a RAM-only `fastboot boot` of
`shim-river/out/river-r76-fullram-boot.img`. The user confirmed that it booted.
This is the known working image for subsequent performance work. No partition
was flashed during the display-fit tests.

## What did not work

| Probe | Result |
| --- | --- |
| r78, patch 0207, A5xx mode 7 / CP_BLIT op 3 scale packet | The 64×64 to 32×32 command completed its fence, but all destination samples stayed at the sentinel value. It did not establish a GPU scale operation. |
| r79, patch 0208, CPU nearest-neighbor fit | The full 1125×2436 canvas fit inside 1048×2270 with 16-pixel side margins. Scrolling flickered and left a black strip; static content looked good. The copy took about 85–126 ms per fitted frame. A second boot with GPU fence wait and cache refresh showed the same defect. |
| r79, `sweetfb_native=1` | SpringBoard launched, but the display remained on boot text. One GPU present timed out. The phone was restored with r76. |
| r80, patch 0209, VIG0 hardware fit | The display became solid blue. The USB receiver was unavailable, so this run has no useful live kernel log. The phone was restored with r76. |
| r81, patch 0210, VIG0 clock and mixer overlay | CI `39992378505` passed. A RAM-only boot reached SpringBoard and logged full-screen swaps plus VIG0 activation, but the user saw a blue screen with intermittent Apple logo. The USB log is `logs-private/r81-vigfit-live-2.txt`. The phone was restored with r76 and the user confirmed it booted. |

## What the results mean

The panel's ABL RGB0 pipe is marked non-scaling in the River device tree. VIG0
is the plausible hardware scaler, but neither VIG0 probe produced a usable
picture. The r81 log proves that software submitted frames and enabled the
overlay; it does not prove that the scaled pixels reached the panel. Do not
use r79–r81 as the normal boot or assume that another cache flush fixes the
blue screen. The CPU approach is too slow and visibly tears during motion.

An offline MDP register and handoff audit is required before another display
test. Useful checks are the VIG0 source buffer contents, pipe clock and fetch
state, layer mixer stage and blend registers, CTL flush routing, and whether
the bootloader's RGB0 layer remains visible underneath. UniXNU's display
code provides a useful diagnostic pattern: retain each submitted frame until
completion and read back early scanout frames to separate copy failures from
display-pipe failures. Its Adreno 740 and UEFI GOP implementation is not a
drop-in driver for River's A5xx and ABL framebuffer.

## Performance-chat handoff

Start from the confirmed r76 full-RAM image and its default display path.
The display-fit boot arguments `riverfitcpu=1`, `riverfitmdp=1`, and
`sweetfb_native=1` are diagnostic only. See `NOTES.md` for the preceding
River performance sample and the separate GPU/EAGL work.
