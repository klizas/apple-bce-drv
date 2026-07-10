# Fork notes
This fork fixes various issues discovered when using the original [apple-bce-drv](https://github.com/t2linux/apple-bce-drv) module:
- Sleep / resume not working properly
- Built-in camera
- Audio: microphone capture, reliable period wakeups, honest hw pointer / latency reporting
- USB
- Various optimisations / bug fixes
- Module param `apple_bce.timestamp_interval_ms` (T2 timestamp heartbeat, default 10s, 0 = off)

There's also the [ave](https://github.com/klizas/apple-bce-drv/tree/ave) branch, with a driver for the T2's HEVC hardware video encoder.

## Tested on
- MacBookPro16,1 2019

Help expanding this list by submitting a PR or an issue.

## My personal setup
- MacBookPro16,1 2019
- Kernel: stable CachyOS with custom [patches](https://github.com/klizas/t2-kernel-patches) and this module: https://github.com/klizas/cachyos-kernel-builder/releases/tag/latest
- Using discrete AMD GPU exclusively
- Boot args: `intel_iommu=on iommu=pt pcie_ports=compat`

# Original README
A driver for MacBook models 2018 and newer, implementing the VHCI (required for mouse/keyboard/etc.) and audio functionality.

The project is divided into 3 main components:
- BCE (Buffer Copy Engine) - this is what the files in the root directory are for. This estabilishes a basic communication channel with the T2. VHCI and Audio both require this component.
- VHCI - this is a virtual USB host controller; keyboard, mouse and other system components are provided by this component (other drivers use this host controller to provide more functionality, however USB drivers are not in this project's scope).
- Audio - a driver for the T2 audio interface, currently only audio output is supported.

Please note that the `master` branch does not currently support system suspend and resume.

If you want to support me, you can do so by donating to me on PayPal: https://paypal.me/mcmrarm

