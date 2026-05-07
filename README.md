# MacBook Bridge/T2 Linux Driver
A driver for MacBook models 2018 and newer, implementing the VHCI (required for mouse/keyboard/etc.) and audio functionality.

The project is divided into 3 main components:
- BCE (Buffer Copy Engine) - this is what the files in the root directory are for. This estabilishes a basic communication channel with the T2. VHCI and Audio both require this component.
- VHCI - this is a virtual USB host controller; keyboard, mouse and other system components are provided by this component (other drivers use this host controller to provide more functionality, however USB drivers are not in this project's scope).
- Audio - a driver for the T2 audio interface, currently only audio output is supported.

Please note that the `master` branch does not currently support system suspend and resume.

If you want to support me, you can do so by donating to me on PayPal: https://paypal.me/mcmrarm

## Fork notes

This fork adds a fourth component on top of upstream:

- Video (AVE) - a V4L2 mem2mem encoder exposing the T2's hardware HEVC encoder. NV12/NV12M input, HEVC bitstream output. Registers as `apple-ave` (card "Apple T2 HEVC Encoder") with `V4L2_CAP_VIDEO_M2M_MPLANE`. Lives in `video/`, built into the same `apple_bce` module as the rest of the driver.

The encoder speaks an XPC protocol the kernel does not implement. The driver forwards encode sessions to a userspace daemon over a Unix socket — default `/run/aveserverd.sock`, overridable via the `sock_path` parameter on the `apple_bce` module.

The daemon is [t2aved](https://github.com/klizas/t2aved). It must be built and running before anything opens the encoder's `/dev/videoN` node, otherwise session setup fails. Without t2aved the V4L2 device is present but non-functional.
