# River camera, microphone and torch: offline survey

2026-10-30. This is a source-based plan, not a hardware verification. The
active image and phone hardware must be checked before applying it.

## Evidence

The Android kernel sources below are pinned to [commit 2dba45b8](https://github.com/LineageOS/android_kernel_motorola_sdm632/tree/2dba45b8e247958d5a1b15b33c6064c15af4983e).
They describe a River configuration, not verified hardware on this phone.

* The [River camera overlay](https://github.com/LineageOS/android_kernel_motorola_sdm632/blob/2dba45b8e247958d5a1b15b33c6064c15af4983e/arch/arm64/boot/dts/qcom/sdm632-camera-sensor-mot-river-overlay.dtsi) connects torch0 and flash0 to PMI632. Comments identify rear OV12A10, front S5K4H7 and second rear S5K5E9. Camera nodes use CCI, 24 MHz clocks, power rails, GPIOs, CSI PHY and CSID. These sensor labels are candidates, not ID measurements.
* The [PMI632 tree](https://github.com/LineageOS/android_kernel_motorola_sdm632/blob/2dba45b8e247958d5a1b15b33c6064c15af4983e/arch/arm64/boot/dts/qcom/pmi632.dtsi) places the LED block at peripheral d3 on SPMI slave 3, with fault, droop and thermal controls. The River overlay sets `qcom,current = <100>` for torch0, but the [LED driver](https://github.com/LineageOS/android_kernel_motorola_sdm632/blob/2dba45b8e247958d5a1b15b33c6064c15af4983e/drivers/leds/leds-qpnp-flash-v2.c) parses `qcom,current-ma` instead; the base torch value is 300 mA. Neither is a verified safe setting for this boot. Trace initialization, current conversion, enable, shutdown and fault handling before implementation.
* The [River audio overlay](https://github.com/LineageOS/android_kernel_motorola_sdm632/blob/2dba45b8e247958d5a1b15b33c6064c15af4983e/arch/arm64/boot/dts/qcom/sdm632-river-audio-overlay.dtsi) describes a Cirrus CS47L35 codec on SPI 7, reset GPIO 68, interrupt GPIO 46, a 9.6 MHz audio clock, mic-bias routes and Qualcomm Q6 audio endpoints. CS35L41 on I2C is a separate speaker amplifier. Microphone routing and DSP state remain unverified.
* Existing River patches contain SPMI read/write machinery, but the haptics channel helper has a hard-coded fallback when lookup fails. Do not reuse it for the LED without a fail-closed lookup and synchronization with other clients. Haptics is owned by the other chat.
* Historical NOTES reports VirtualAudio disabled after a mediaserverd crash with no audio device. This does not establish microphone capture or iOS audio integration. No camera or torch integration was identified in the tracked tree.

## Milestones

1. **Inventory, then torch:** after the other chat releases the phone and the user coordinates testing, collect read-only OS/IORegistry evidence and available boot logs. Avoid MMIO during that survey. Audit the full LED driver and SPMI arbitration, then design a default-off, bounded torch operation with controlled current, fault handling, timeout and explicit shutdown. Verify physical light and shutdown first, then separately prove iOS flashlight control discovers and operates it.
2. **Microphone:** identify codec, bus, rails, clocks and actual microphone routing. Determine Q6 DSP and firmware state and how captured buffers reach the CPU. Verify samples from a minimal capture path, then implement an iOS-compatible audio device and test app recording. DSP messaging, DMA, codec setup, audio HAL and permissions are independent unknowns.
3. **Camera:** identify fitted sensors and calibration; bring up CCI and sensor power/clock and read an ID with controlled shutdown. A frame additionally needs CSI PHY/CSID, ISP, DMA and buffer handling, sensor setup, format processing and an iOS camera-provider interface. Sensor identification alone is not camera integration.

Do not flash partitions. Coordinate every device test and display restart with
the user and other chat. Preserve the working UI and sleep/wake behavior.
