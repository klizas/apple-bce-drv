# Fork notes
This branch is [aur](https://github.com/klizas/apple-bce-drv/tree/aur) — all fixes to the original [apple-bce-drv](https://github.com/t2linux/apple-bce-drv) module (sleep/resume, camera, audio, USB, various optimisations; see its README) — plus a driver for the T2's hardware HEVC video encoder.

## Video encoder (AVE)
The T2 inherits the Apple Video Encoder (AVE) block from the A10 it is derived from; macOS uses it through VideoToolbox for HEVC transcoding. This branch exposes it as a standard V4L2 stateful mem2mem encoder — `/dev/videoN`, card `Apple T2 HEVC Encoder`, `V4L2_CAP_VIDEO_M2M_MPLANE` — driven over BCE queues like audio and VHCI. Code lives in `video/`, built into the same `apple-bce` module.

Capabilities:
- Input: NV12 / NV12M, 128×128 to 4096×2304 in steps of 2, default 1920×1080.
- Output: HEVC Annex B. VPS/SPS/PPS are prepended to IRAP frames; `V4L2_BUF_FLAG_KEYFRAME` is reported for firmware-scheduled keyframes too, not just forced ones.
- Profile Main / Main 10, level up to 6.2 (default 5.1).
- Rate control: VBR (default), CBR, or constant quality 1–100. Bitrate 100 kbit/s – 100 Mbit/s (default 4 Mbit/s), changeable on a live session; the new rate applies before the next frame.
- GOP size 0–600 (0 = firmware default), HEVC min/max QP 0–51, `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`.
- Frame rate via `VIDIOC_S_PARM`, default 30/1.
- Colour primaries / transfer / matrix are forwarded to the firmware from the V4L2 colorspace of the input format.
- Encode sessions are torn down on suspend; the firmware session does not survive S3.

### Runtime requirement: t2aved
The encoder speaks an XPC protocol the kernel does not implement. The driver forwards encode sessions to a userspace daemon over a Unix socket — default `/run/aveserverd.sock`, overridable via the `sock_path` parameter on the `apple_bce` module.

**[t2aved](https://github.com/klizas/t2aved) must be installed and running before anything opens the encoder's `/dev/videoN` node.** Without it the V4L2 device is present but non-functional (session setup fails). See the [t2aved README](https://github.com/klizas/t2aved#readme) for install and diagnostics.

## Tested on
- MacBookPro16,1 2019

# Original README
A driver for MacBook models 2018 and newer, implementing the VHCI (required for mouse/keyboard/etc.) and audio functionality.

The project is divided into 3 main components:
- BCE (Buffer Copy Engine) - this is what the files in the root directory are for. This estabilishes a basic communication channel with the T2. VHCI and Audio both require this component.
- VHCI - this is a virtual USB host controller; keyboard, mouse and other system components are provided by this component (other drivers use this host controller to provide more functionality, however USB drivers are not in this project's scope).
- Audio - a driver for the T2 audio interface, currently only audio output is supported.

Please note that the `master` branch does not currently support system suspend and resume.

If you want to support me, you can do so by donating to me on PayPal: https://paypal.me/mcmrarm
