#include "encoder.h"
#include "../apple_bce.h"

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <media/v4l2-ctrls.h>

/*
 * Stamp session token at +0x08 in the command buffer.
 * The T2 returns this token in the CodecID response[+0x18], and all subsequent
 * commands must carry it at offset +0x08. (Discovered via disassembly of
 * AppleAVEEncoder.bundle — previously thought to be hardcoded 0xFEEDBEEF.)
 */
static inline void ave_stamp_session_token(struct ave_session *session)
{
	*(u64 *)(session->cmd_buf + 0x08) = session->session_token;
}

static int ave_send_copy_property(struct ave_session *session, const char *name)
{
	pr_debug("apple-ave: [setup] CopyProperty (0x08): \"%s\"\n", name);
	ave_build_cmd_copy_property(session->cmd_buf, name);
	ave_stamp_session_token(session);
	return ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
}

static int ave_send_set_property_bool(struct ave_session *session, const char *name, bool value)
{
	pr_debug("apple-ave: [setup] SetProperty (0x09): \"%s\" = %s\n",
		 name, value ? "true" : "false");
	ave_build_cmd_set_property_bool(session->cmd_buf, name, value);
	ave_stamp_session_token(session);
	return ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
}

static int ave_send_set_property_s32(struct ave_session *session, const char *name, s32 value)
{
	pr_debug("apple-ave: [setup] SetProperty (0x09): \"%s\" = %d (0x%x)\n",
		 name, value, value);
	ave_build_cmd_set_property_s32(session->cmd_buf, name, value);
	ave_stamp_session_token(session);
	return ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
}

static int ave_send_set_property_float32(struct ave_session *session, const char *name,
					 u32 ieee754_bits)
{
	pr_debug("apple-ave: [setup] SetProperty (0x09): \"%s\" = float32(0x%08x)\n",
		 name, ieee754_bits);
	ave_build_cmd_set_property_float32(session->cmd_buf, name, ieee754_bits);
	ave_stamp_session_token(session);
	return ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
}

static int ave_send_set_property_string(struct ave_session *session, const char *name,
					const char *value)
{
	pr_debug("apple-ave: [setup] SetProperty (0x09): \"%s\" = \"%s\"\n", name, value);
	ave_build_cmd_set_property_string(session->cmd_buf, name, value);
	ave_stamp_session_token(session);
	return ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
}

/*
 * IEEE 754 float32 lookup table for Quality percentages 0-100.
 * Maps integer percentage to float32 bit pattern: table[n] = float32(n/100.0).
 * Avoids kernel FPU — all values precomputed at compile time.
 */
static const u32 ave_quality_to_float32[101] = {
	0x00000000, /* 0 = 0.00 */
	0x3C23D70A, /* 1 = 0.01 */
	0x3CA3D70A, /* 2 = 0.02 */
	0x3CF5C28F, /* 3 = 0.03 */
	0x3D23D70A, /* 4 = 0.04 */
	0x3D4CCCCD, /* 5 = 0.05 */
	0x3D75C28F, /* 6 = 0.06 */
	0x3D8F5C29, /* 7 = 0.07 */
	0x3DA3D70A, /* 8 = 0.08 */
	0x3DB851EC, /* 9 = 0.09 */
	0x3DCCCCCD, /* 10 = 0.10 */
	0x3DE147AE, /* 11 = 0.11 */
	0x3DF5C28F, /* 12 = 0.12 */
	0x3E051EB8, /* 13 = 0.13 */
	0x3E0F5C29, /* 14 = 0.14 */
	0x3E19999A, /* 15 = 0.15 */
	0x3E23D70A, /* 16 = 0.16 */
	0x3E2E147B, /* 17 = 0.17 */
	0x3E3851EC, /* 18 = 0.18 */
	0x3E428F5C, /* 19 = 0.19 */
	0x3E4CCCCD, /* 20 = 0.20 */
	0x3E570A3D, /* 21 = 0.21 */
	0x3E6147AE, /* 22 = 0.22 */
	0x3E6B851F, /* 23 = 0.23 */
	0x3E75C28F, /* 24 = 0.24 */
	0x3E800000, /* 25 = 0.25 */
	0x3E851EB8, /* 26 = 0.26 */
	0x3E8A3D71, /* 27 = 0.27 */
	0x3E8F5C29, /* 28 = 0.28 */
	0x3E947AE1, /* 29 = 0.29 */
	0x3E99999A, /* 30 = 0.30 */
	0x3E9EB852, /* 31 = 0.31 */
	0x3EA3D70A, /* 32 = 0.32 */
	0x3EA8F5C3, /* 33 = 0.33 */
	0x3EAE147B, /* 34 = 0.34 */
	0x3EB33333, /* 35 = 0.35 */
	0x3EB851EC, /* 36 = 0.36 */
	0x3EBD70A4, /* 37 = 0.37 */
	0x3EC28F5C, /* 38 = 0.38 */
	0x3EC7AE14, /* 39 = 0.39 */
	0x3ECCCCCD, /* 40 = 0.40 */
	0x3ED1EB85, /* 41 = 0.41 */
	0x3ED70A3D, /* 42 = 0.42 */
	0x3EDC28F6, /* 43 = 0.43 */
	0x3EE147AE, /* 44 = 0.44 */
	0x3EE66666, /* 45 = 0.45 */
	0x3EEB851F, /* 46 = 0.46 */
	0x3EF0A3D7, /* 47 = 0.47 */
	0x3EF5C28F, /* 48 = 0.48 */
	0x3EFAE148, /* 49 = 0.49 */
	0x3F000000, /* 50 = 0.50 */
	0x3F028F5C, /* 51 = 0.51 */
	0x3F051EB8, /* 52 = 0.52 */
	0x3F07AE14, /* 53 = 0.53 */
	0x3F0A3D71, /* 54 = 0.54 */
	0x3F0CCCCD, /* 55 = 0.55 */
	0x3F0F5C29, /* 56 = 0.56 */
	0x3F11EB85, /* 57 = 0.57 */
	0x3F147AE1, /* 58 = 0.58 */
	0x3F170A3D, /* 59 = 0.59 */
	0x3F19999A, /* 60 = 0.60 */
	0x3F1C28F6, /* 61 = 0.61 */
	0x3F1EB852, /* 62 = 0.62 */
	0x3F2147AE, /* 63 = 0.63 */
	0x3F23D70A, /* 64 = 0.64 */
	0x3F266666, /* 65 = 0.65 */
	0x3F28F5C3, /* 66 = 0.66 */
	0x3F2B851F, /* 67 = 0.67 */
	0x3F2E147B, /* 68 = 0.68 */
	0x3F30A3D7, /* 69 = 0.69 */
	0x3F333333, /* 70 = 0.70 */
	0x3F35C28F, /* 71 = 0.71 */
	0x3F3851EC, /* 72 = 0.72 */
	0x3F3AE148, /* 73 = 0.73 */
	0x3F3D70A4, /* 74 = 0.74 */
	0x3F400000, /* 75 = 0.75 */
	0x3F428F5C, /* 76 = 0.76 */
	0x3F451EB8, /* 77 = 0.77 */
	0x3F47AE14, /* 78 = 0.78 */
	0x3F4A3D71, /* 79 = 0.79 */
	0x3F4CCCCD, /* 80 = 0.80 */
	0x3F4F5C29, /* 81 = 0.81 */
	0x3F51EB85, /* 82 = 0.82 */
	0x3F547AE1, /* 83 = 0.83 */
	0x3F570A3D, /* 84 = 0.84 */
	0x3F59999A, /* 85 = 0.85 */
	0x3F5C28F6, /* 86 = 0.86 */
	0x3F5EB852, /* 87 = 0.87 */
	0x3F6147AE, /* 88 = 0.88 */
	0x3F63D70A, /* 89 = 0.89 */
	0x3F666666, /* 90 = 0.90 */
	0x3F68F5C3, /* 91 = 0.91 */
	0x3F6B851F, /* 92 = 0.92 */
	0x3F6E147B, /* 93 = 0.93 */
	0x3F70A3D7, /* 94 = 0.94 */
	0x3F733333, /* 95 = 0.95 */
	0x3F75C28F, /* 96 = 0.96 */
	0x3F7851EC, /* 97 = 0.97 */
	0x3F7AE148, /* 98 = 0.98 */
	0x3F7D70A4, /* 99 = 0.99 */
	0x3F800000, /* 100 = 1.00 */
};

/*
 * Build a T2 ProfileLevel string from V4L2 HEVC profile and level enums.
 * Format: "HEVC_{profile}_{level}" or "HEVC_{profile}_AutoLevel"
 *
 * The T2 encoder plugin accepts strings like:
 *   "HEVC_Main_AutoLevel", "HEVC_Main_5.1", "HEVC_Main10_4.0"
 */
static void ave_build_profile_level_string(char *buf, size_t size, s32 profile, s32 level)
{
	const char *prof_str;
	const char *lvl_str;

	switch (profile) {
	case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10:
		prof_str = "Main10";
		break;
	case V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE:
		prof_str = "MainStill";
		break;
	default:
		prof_str = "Main";
		break;
	}

	switch (level) {
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_1:   lvl_str = "1.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_2:   lvl_str = "2.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1: lvl_str = "2.1"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_3:   lvl_str = "3.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1: lvl_str = "3.1"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4:   lvl_str = "4.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1: lvl_str = "4.1"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_5:   lvl_str = "5.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1: lvl_str = "5.1"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2: lvl_str = "5.2"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_6:   lvl_str = "6.0"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1: lvl_str = "6.1"; break;
	case V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2: lvl_str = "6.2"; break;
	default:                              lvl_str = NULL;   break;
	}

	if (lvl_str)
		snprintf(buf, size, "HEVC_%s_%s", prof_str, lvl_str);
	else
		snprintf(buf, size, "HEVC_%s_AutoLevel", prof_str);
}

int ave_session_setup(struct ave_session *session, struct apple_bce_device *bce,
		      u32 width, u32 height, const struct ave_enc_params *params)
{
	int status;
	char profile_buf[64];

	pr_debug("apple-ave: === SESSION SETUP START ===\n");
	pr_debug("apple-ave: params: %ux%u @ %u bps, %u/%u fps\n",
		 width, height, params->bitrate, params->fps_num, params->fps_den);

	memset(session, 0, sizeof(*session));
	session->bce = bce;
	session->width = width;
	session->height = height;
	session->bitrate = params->bitrate;
	session->fps_num = params->fps_num;
	session->fps_den = params->fps_den;
	session->state = AVE_STATE_IDLE;

	session->cmd_buf = kmalloc(AVE_CMD_BUF_SIZE, GFP_KERNEL);
	if (!session->cmd_buf)
		return -ENOMEM;

	/* Step 1: Create BCE queues */
	pr_debug("apple-ave: [setup] Step 1: Creating BCE queues\n");
	status = ave_queues_create(bce, &session->queues);
	if (status) {
		pr_err("apple-ave: [setup] queue creation failed (%d)\n", status);
		goto fail_cmd;
	}

	/* Step 2: Q1 recv buffers are submitted per-command in ave_cmd_send_sync().
	 * Q2/Q3 recv buffers are pre-submitted before frame encoding, not here.
	 * macOS does NOT submit any Q2/Q3 buffers during the setup phase. */

	/* Step 3: Cmd 0x00 — Codec ID "hvc1" */
	pr_debug("apple-ave: [setup] Step 3: CodecID (hvc1)\n");
	ave_build_cmd_codec_id(session->cmd_buf);
	status = ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [setup] CodecID FAILED (%d)\n", status);
		goto fail_queues;
	}
	pr_debug("apple-ave: [setup] Step 3: CodecID OK\n");

	/* Extract session token from T2's CodecID response at offset +0x18.
	 * Disassembly of AppleAVEEncoder.bundle shows: storage[0x08] = response[0x18].
	 * This token (observed as 0xFEEDBEEF) must be placed at +0x08 in all
	 * subsequent commands. */
	session->session_token = *(u64 *)(session->queues.cmd_resp_buf + 0x18);
	pr_debug("apple-ave: [setup] session token = 0x%llx\n", session->session_token);

	/* Step 4: Cmd 0x01 — Session config */
	pr_debug("apple-ave: [setup] Step 4: SessionConfig (%ux%u)\n", width, height);
	ave_build_cmd_session_config(session->cmd_buf, width, height);
	ave_stamp_session_token(session);
	status = ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [setup] SessionConfig FAILED (%d)\n", status);
		goto fail_queues;
	}
	pr_debug("apple-ave: [setup] Step 4: SessionConfig OK\n");

	/* Step 5: Cmd 0x08 — CopyProperty "UsingHardwareAcceleratedVideoEncoder" */
	pr_debug("apple-ave: [setup] Step 5: CopyProperty UsingHardwareAcceleratedVideoEncoder\n");
	status = ave_send_copy_property(session, "UsingHardwareAcceleratedVideoEncoder");
	if (status) {
		pr_err("apple-ave: [setup] UsingHWAccel #1 FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 6: Cmd 0x08 — CopyProperty "UsingHardwareAcceleratedVideoEncoder" */
	pr_debug("apple-ave: [setup] Step 6: CopyProperty UsingHardwareAcceleratedVideoEncoder\n");
	status = ave_send_copy_property(session, "UsingHardwareAcceleratedVideoEncoder");
	if (status) {
		pr_err("apple-ave: [setup] UsingHWAccel #2 FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 7: SetProperty "RealTime" = true */
	status = ave_send_set_property_bool(session, "RealTime", true);
	if (status) {
		pr_err("apple-ave: [setup] RealTime FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 8: SetProperty "AllowFrameReordering" = false
	 * B-frames require frame reordering, but our synchronous pipeline
	 * (submit 1 frame, wait for output) cannot handle buffered/reordered
	 * output. Keep disabled unconditionally. */
	status = ave_send_set_property_bool(session, "AllowFrameReordering", false);
	if (status) {
		pr_err("apple-ave: [setup] AllowFrameReordering FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 8b: SetProperty "ExpectedFrameRate" (integer fps only) */
	if (params->fps_den == 1 && params->fps_num > 0) {
		status = ave_send_set_property_s32(session, "ExpectedFrameRate",
						   params->fps_num);
		if (status)
			pr_warn("apple-ave: [setup] ExpectedFrameRate rejected (%d)\n", status);
	}

	/* Step 8c: SetProperty "ProfileLevel" (string) */
	ave_build_profile_level_string(profile_buf, sizeof(profile_buf),
				       params->profile, params->level);
	status = ave_send_set_property_string(session, "ProfileLevel", profile_buf);
	if (status)
		pr_warn("apple-ave: [setup] ProfileLevel \"%s\" rejected (%d), using default\n",
			profile_buf, status);

	/* Step 9: SetProperty "AverageBitRate" */
	status = ave_send_set_property_s32(session, "AverageBitRate", params->bitrate);
	if (status) {
		pr_err("apple-ave: [setup] AverageBitRate FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 9b: SetProperty "ConstantBitRate" (CBR mode only) */
	if (params->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) {
		status = ave_send_set_property_bool(session, "ConstantBitRate", true);
		if (status)
			pr_warn("apple-ave: [setup] ConstantBitRate rejected (%d)\n", status);
	}

	/* Step 9c: SetProperty "Quality" (CQ mode only, float32) */
	if (params->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) {
		s32 q = clamp(params->quality, 1, 100);

		status = ave_send_set_property_float32(session, "Quality",
						       ave_quality_to_float32[q]);
		if (status)
			pr_warn("apple-ave: [setup] Quality rejected (%d)\n", status);
	}

	/* Step 9d: SetProperty "MaxKeyFrameInterval" (GOP size) */
	if (params->gop_size > 0) {
		status = ave_send_set_property_s32(session, "MaxKeyFrameInterval",
						   params->gop_size);
		if (status)
			pr_warn("apple-ave: [setup] MaxKeyFrameInterval rejected (%d)\n", status);
	}

	/* Step 9e: SetProperty "MinAllowedFrameQP" */
	if (params->min_qp > 0) {
		status = ave_send_set_property_s32(session, "MinAllowedFrameQP",
						   params->min_qp);
		if (status)
			pr_warn("apple-ave: [setup] MinAllowedFrameQP rejected (%d)\n", status);
	}

	/* Step 9f: SetProperty "MaxAllowedFrameQP" */
	if (params->max_qp > 0) {
		status = ave_send_set_property_s32(session, "MaxAllowedFrameQP",
						   params->max_qp);
		if (status)
			pr_warn("apple-ave: [setup] MaxAllowedFrameQP rejected (%d)\n", status);
	}

	/* Step 10: CopyProperty "MVHEVCVideoLayerIDs" */
	status = ave_send_copy_property(session, "MVHEVCVideoLayerIDs");
	if (status) {
		pr_err("apple-ave: [setup] MVHEVCVideoLayerIDs FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 11: PrepareToEncodeFrames */
	ave_build_cmd_prepare(session->cmd_buf);
	ave_stamp_session_token(session);
	status = ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [setup] Prepare FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 11b: SetProperty "BPictures" = 0
	 * Our synchronous pipeline cannot handle B-frame reordering.
	 * T2's processPrepareToEncodeFrames may internally set BPictures=1
	 * for resolutions >1080p, so always override to 0 after Prepare. */
	status = ave_send_set_property_s32(session, "BPictures", 0);
	if (status) {
		pr_err("apple-ave: [setup] BPictures FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 12: SetProperty "ColorPrimaries" */
	status = ave_send_set_property_s32(session, "ColorPrimaries",
					   params->color_primaries);
	if (status) {
		pr_err("apple-ave: [setup] ColorPrimaries FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 13: SetProperty "YCbCrMatrix" */
	status = ave_send_set_property_s32(session, "YCbCrMatrix",
					   params->ycbcr_matrix);
	if (status) {
		pr_err("apple-ave: [setup] YCbCrMatrix FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 13b: SetProperty "TransferFunction" */
	status = ave_send_set_property_s32(session, "TransferFunction",
					   params->transfer_func);
	if (status)
		pr_warn("apple-ave: [setup] TransferFunction rejected (%d)\n", status);

	session->state = AVE_STATE_CONFIGURED;
	session->frame_counter = 0;
	pr_debug("apple-ave: === SESSION SETUP COMPLETE (%ux%u @ %u bps) ===\n",
		 width, height, params->bitrate);
	return 0;

fail_queues:
	pr_debug("apple-ave: [setup] cleaning up after failure...\n");
	ave_queues_destroy(&session->queues);
fail_cmd:
	kfree(session->cmd_buf);
	session->cmd_buf = NULL;
	session->state = AVE_STATE_ERROR;
	pr_err("apple-ave: === SESSION SETUP FAILED ===\n");
	return status;
}

void ave_session_teardown(struct ave_session *session)
{
	if (!session->cmd_buf)
		return;

	pr_debug("apple-ave: === SESSION TEARDOWN START ===\n");

	if (session->state == AVE_STATE_CONFIGURED ||
	    session->state == AVE_STATE_ENCODING ||
	    session->state == AVE_STATE_ERROR) {
		/* Re-enable auto-resubmit so teardown commands work normally */
		session->queues.q3_auto_resubmit = true;

		/* Cmd 0x03 — CompleteFrames: drain in-flight frames */
		pr_debug("apple-ave: [teardown] sending CompleteFrames (0x03)\n");
		ave_build_cmd_complete_frames(session->cmd_buf);
		ave_stamp_session_token(session);
		ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);

		/* Cmd 0x06 — EndSession: tell firmware to release session */
		pr_debug("apple-ave: [teardown] sending EndSession (0x06)\n");
		ave_build_cmd_end_session(session->cmd_buf);
		ave_stamp_session_token(session);
		ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
	}

	ave_queues_destroy(&session->queues);
	kfree(session->cmd_buf);
	session->cmd_buf = NULL;
	kfree(session->hvcc_data);
	session->hvcc_data = NULL;
	kfree(session->annex_b_header);
	session->annex_b_header = NULL;
	session->state = AVE_STATE_IDLE;
	pr_debug("apple-ave: === SESSION TEARDOWN COMPLETE ===\n");
}

/*
 * Extract hvcC from Frame 1's first Q3 callback and build an Annex B header
 * containing VPS + SPS + PPS (prepended to IDR frames).
 *
 * The hvcC structure starts at callback buffer offset +0x68 and follows
 * ISO/IEC 14496-15: 23-byte fixed header, then arrays of parameter sets.
 */
static int ave_extract_hvcc(struct ave_session *session, const void *q3_data)
{
	const u8 *hvc = (const u8 *)q3_data + AVE_HVCC_OFFSET;
	const u8 *hvc_end = hvc + AVE_CMD_BUF_SIZE - AVE_HVCC_OFFSET;
	u8 num_arrays, nal_type;
	u16 num_nalus, nal_len;
	const u8 *p;
	u8 *out;
	size_t out_pos = 0;
	int i, j;
	static const u8 start_code[4] = {0x00, 0x00, 0x00, 0x01};

	if (hvc[0] != 1) {
		pr_warn("apple-ave: [hvcc] unexpected version %d, expected 1\n", hvc[0]);
		return -EINVAL;
	}

	num_arrays = hvc[AVE_HVCC_HEADER_SIZE - 1];
	pr_debug("apple-ave: [hvcc] version=%d profile=%d level=%d numArrays=%d\n",
		 hvc[0], hvc[1] & 0x1F, hvc[12], num_arrays);

	/* First pass: calculate total size needed */
	p = hvc + AVE_HVCC_HEADER_SIZE;
	for (i = 0; i < num_arrays && i < 8; i++) {
		if (p + 3 > hvc_end)
			break;
		num_nalus = ((u16)p[1] << 8) | p[2];
		p += 3;
		for (j = 0; j < num_nalus && j < 16; j++) {
			if (p + 2 > hvc_end)
				goto size_done;
			nal_len = ((u16)p[0] << 8) | p[1];
			p += 2;
			if (p + nal_len > hvc_end)
				goto size_done;
			out_pos += 4 + nal_len; /* start code + NAL data */
			p += nal_len;
		}
	}
size_done:
	if (out_pos == 0) {
		pr_warn("apple-ave: [hvcc] no parameter sets found\n");
		return -EINVAL;
	}

	/* Allocate and build Annex B header */
	session->annex_b_header = kmalloc(out_pos, GFP_KERNEL);
	if (!session->annex_b_header)
		return -ENOMEM;
	session->annex_b_header_size = out_pos;

	out = session->annex_b_header;
	out_pos = 0;
	p = hvc + AVE_HVCC_HEADER_SIZE;
	for (i = 0; i < num_arrays && i < 8; i++) {
		if (p + 3 > hvc_end)
			break;
		nal_type = p[0] & 0x3F;
		num_nalus = ((u16)p[1] << 8) | p[2];
		p += 3;
		for (j = 0; j < num_nalus && j < 16; j++) {
			if (p + 2 > hvc_end)
				goto build_done;
			nal_len = ((u16)p[0] << 8) | p[1];
			p += 2;
			if (p + nal_len > hvc_end)
				goto build_done;

			pr_debug("apple-ave: [hvcc] array[%d]: type=%d (%s) len=%d\n",
				 i, nal_type,
				 nal_type == 32 ? "VPS" :
				 nal_type == 33 ? "SPS" :
				 nal_type == 34 ? "PPS" : "?",
				 nal_len);

			memcpy(out + out_pos, start_code, 4);
			out_pos += 4;
			memcpy(out + out_pos, p, nal_len);
			out_pos += nal_len;
			p += nal_len;
		}
	}
build_done:
	session->annex_b_header_size = out_pos;

	/* Also save a copy of the raw hvcC for potential future use */
	session->hvcc_data = kmalloc(AVE_CMD_BUF_SIZE, GFP_KERNEL);
	if (session->hvcc_data) {
		memcpy(session->hvcc_data, q3_data, AVE_CMD_BUF_SIZE);
		session->hvcc_size = AVE_CMD_BUF_SIZE;
	}

	pr_debug("apple-ave: [hvcc] Annex B header built: %zu bytes (VPS+SPS+PPS)\n",
		 session->annex_b_header_size);
	return 0;
}

/*
 * Resubmit consumed Q3 buffers to the ring so T2 has buffers for the next frame.
 */
static void ave_resubmit_q3_bufs(struct ave_queues *queues, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (ave_submit_q3_buf(queues)) {
			pr_err("apple-ave: [encode] failed to resubmit Q3 buf %zu\n", i);
			break;
		}
	}
}

/*
 * Frame 1 encoding pipeline (verified on macOS at ~50ms):
 *
 * 1. Submit: Q0(EncodeFrame) → Q0(Y) → Q0(UV) → Q1(recv)
 * 2. Wait Q3 #1: hvcC config (~40ms) — extract VPS/SPS/PPS
 * 3. Echo Q3 #1 on Q2 (hvcC acknowledgment)
 * 4. Wait Q3 #2: metadata (~1ms)
 * 5. Wait Q3 #3: encoded NAL data (~0.2ms) — result scalar = size
 * 6. Echo Q3 #2 on Q2 (metadata acknowledgment)
 * 7. Wait Q1: frame complete (near-instant after Q3/Q2 done)
 * 8. Resubmit consumed Q3 buffers
 *
 * CRITICAL: Service Q3 events and echo on Q2 BEFORE waiting for Q1.
 * The T2 waits for Q2 callback echoes before signalling Q1.
 */
static int ave_encode_first_frame(struct ave_session *session,
			      void *y_data, size_t y_size,
			      void *uv_data, size_t uv_size,
			      void *out_buf, size_t out_buf_size,
			      size_t *encoded_size)
{
	struct ave_queues *q = &session->queues;
	struct device *dev = &q->bce->pci->dev;
	struct bce_dma_buffer y_dma, uv_dma;
	bool y_mapped = false, uv_mapped = false;
	void *q3_data;
	size_t raw_size, out_pos = 0;
	int status;

	/* Disable Q3 auto-resubmit — we need to read the completed data */
	q->q3_auto_resubmit = false;
	ave_q3_reset(q);

	/* Submit EncodeFrame command on Q0 */
	status = ave_submit_q0_cmd(q, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [frame1] Q0 EncodeFrame submit failed (%d)\n", status);
		goto fail;
	}

	/* Submit Y and UV plane data on Q0 (async — no Q0 ack wait) */
	status = ave_submit_frame_data_async(q, y_data, y_size, &y_dma);
	if (status) {
		pr_err("apple-ave: [frame1] Y plane submit failed (%d)\n", status);
		goto fail;
	}
	y_mapped = true;

	status = ave_submit_frame_data_async(q, uv_data, uv_size, &uv_dma);
	if (status) {
		pr_err("apple-ave: [frame1] UV plane submit failed (%d)\n", status);
		goto fail;
	}
	uv_mapped = true;

	/* Submit Q1 receive buffer (arms cmd_completion) */
	status = ave_submit_q1_recv(q);
	if (status) {
		pr_err("apple-ave: [frame1] Q1 recv submit failed (%d)\n", status);
		goto fail;
	}

	/* === Service Q3 FIRST (3 events for Frame 1) === */

	/* Q3 #1: hvcC config (~40ms after submit) */
	pr_debug("apple-ave: [frame1] waiting for Q3 #1 (hvcC)...\n");
	status = ave_wait_q3(q, 10000);
	if (status) {
		pr_err("apple-ave: [frame1] Q3 #1 (hvcC) TIMEOUT\n");
		goto fail;
	}
	pr_debug("apple-ave: [frame1] Q3 #1 (hvcC) received, result=0x%zx\n",
		ave_q3_completed_size(q, 0));

	/* Extract hvcC and build Annex B header (VPS+SPS+PPS) */
	q3_data = ave_q3_completed_data(q, 0);
	status = ave_extract_hvcc(session, q3_data);
	if (status)
		pr_warn("apple-ave: [frame1] hvcC extraction failed (%d), continuing without header\n",
			status);

	/* Echo hvcC callback on Q2 — T2 waits for this before proceeding */
	status = ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("apple-ave: [frame1] Q2 echo (hvcC) failed (%d)\n", status);

	/* Q3 #2: metadata callback (~1ms) */
	pr_debug("apple-ave: [frame1] waiting for Q3 #2 (metadata)...\n");
	status = ave_wait_q3(q, 10000);
	if (status) {
		pr_err("apple-ave: [frame1] Q3 #2 (metadata) TIMEOUT\n");
		goto fail;
	}
	pr_debug("apple-ave: [frame1] Q3 #2 (metadata) received\n");

	/* Q3 #3: encoded NAL data (~0.2ms) — result scalar = encoded size */
	pr_debug("apple-ave: [frame1] waiting for Q3 #3 (output)...\n");
	status = ave_wait_q3(q, 10000);
	if (status) {
		pr_err("apple-ave: [frame1] Q3 #3 (output) TIMEOUT\n");
		goto fail;
	}

	raw_size = ave_q3_completed_size(q, 2);
	pr_debug("apple-ave: [frame1] Q3 #3 (output) received, encoded_size=%zu\n", raw_size);

	if (raw_size > AVE_MAX_ENCODED_SIZE) {
		pr_err("apple-ave: [frame1] encoded size %zu exceeds buffer size %d\n",
		       raw_size, AVE_MAX_ENCODED_SIZE);
		status = -ENOSPC;
		goto fail;
	}

	/* Echo metadata callback on Q2 — echoes Q3 event #1 (metadata),
	 * NOT the output data. Matches reference: cb3_buf still holds metadata
	 * because the output went into a separate buffer (Q3 ring slot). */
	q3_data = ave_q3_completed_data(q, 1);
	status = ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("apple-ave: [frame1] Q2 echo (metadata) failed (%d)\n", status);

	/* === Wait Q1 last (near-instant, T2 already done) === */
	pr_debug("apple-ave: [frame1] waiting for Q1...\n");
	status = ave_wait_q1(q, 10000);
	if (status) {
		pr_err("apple-ave: [frame1] Q1 TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("apple-ave: [frame1] Q1 done\n");

	/* Unmap DMA buffers now that T2 is done */
	bce_unmap_dma_buffer(dev, &y_dma);
	y_mapped = false;
	bce_unmap_dma_buffer(dev, &uv_dma);
	uv_mapped = false;

	/* === Build output: Annex B header + converted NAL data === */
	if (raw_size == 0) {
		pr_warn("apple-ave: [frame1] T2 returned 0 bytes\n");
		*encoded_size = 0;
		goto resubmit;
	}

	/* Prepend Annex B header (VPS+SPS+PPS) for IDR frame */
	if (session->annex_b_header && session->annex_b_header_size > 0) {
		if (session->annex_b_header_size > out_buf_size) {
			status = -ENOSPC;
			goto fail;
		}
		memcpy(out_buf, session->annex_b_header, session->annex_b_header_size);
		out_pos = session->annex_b_header_size;
	}

	/* Convert length-prefixed NALs to Annex B and append */
	q3_data = ave_q3_completed_data(q, 2);
	status = ave_convert_to_annex_b(q3_data, raw_size,
					(u8 *)out_buf + out_pos,
					out_buf_size - out_pos, encoded_size);
	if (status) {
		pr_err("apple-ave: [frame1] Annex B conversion failed (%d)\n", status);
		goto fail;
	}
	*encoded_size += out_pos;

resubmit:
	/* Resubmit consumed Q3 buffers (3 for Frame 1) */
	ave_resubmit_q3_bufs(q, q->q3_event_count);
	return 0;

fail:
	if (y_mapped)
		bce_unmap_dma_buffer(dev, &y_dma);
	if (uv_mapped)
		bce_unmap_dma_buffer(dev, &uv_dma);
	session->state = AVE_STATE_ERROR;
	return status ? status : -EIO;
}

/*
 * Frame 2+ encoding pipeline (verified on macOS at ~18ms):
 *
 * 1. Submit: Q0(EncodeFrame) → Q0(Y) → Q0(UV) → Q1(recv)
 * 2. Wait Q1: EncodeFrame ACCEPT (~3ms — just queues the frame)
 * 3. Submit: Q0(CompleteFrames) → Q1(recv)
 * 4. Wait Q3 #1: metadata (~8ms)
 * 5. Wait Q3 #2: encoded NAL data (~0.1ms) — result scalar = size
 * 6. Echo Q3 #1 on Q2 (metadata acknowledgment)
 * 7. Wait Q1: CompleteFrames done (near-instant after Q3/Q2)
 * 8. Resubmit consumed Q3 buffers
 */
static int ave_encode_next_frame(struct ave_session *session,
			      void *y_data, size_t y_size,
			      void *uv_data, size_t uv_size,
			      void *out_buf, size_t out_buf_size,
			      size_t *encoded_size, bool is_keyframe)
{
	struct ave_queues *q = &session->queues;
	struct device *dev = &q->bce->pci->dev;
	struct bce_dma_buffer y_dma, uv_dma;
	bool y_mapped = false, uv_mapped = false;
	void *q3_data;
	size_t raw_size, out_pos = 0;
	int status;

	/* Disable Q3 auto-resubmit, reset tracking */
	q->q3_auto_resubmit = false;
	ave_q3_reset(q);

	/* Submit EncodeFrame command on Q0 */
	status = ave_submit_q0_cmd(q, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [frameN] Q0 EncodeFrame submit failed (%d)\n", status);
		goto fail;
	}

	/* Submit Y and UV plane data on Q0 (async) */
	status = ave_submit_frame_data_async(q, y_data, y_size, &y_dma);
	if (status) {
		pr_err("apple-ave: [frameN] Y plane submit failed (%d)\n", status);
		goto fail;
	}
	y_mapped = true;

	status = ave_submit_frame_data_async(q, uv_data, uv_size, &uv_dma);
	if (status) {
		pr_err("apple-ave: [frameN] UV plane submit failed (%d)\n", status);
		goto fail;
	}
	uv_mapped = true;

	/* Submit Q1 recv for EncodeFrame accept */
	status = ave_submit_q1_recv(q);
	if (status) {
		pr_err("apple-ave: [frameN] Q1 recv submit failed (%d)\n", status);
		goto fail;
	}

	/* === Wait Q1: EncodeFrame ACCEPT (~3ms, just queues the frame) === */
	pr_debug("apple-ave: [frameN] waiting for Q1 EncodeFrame accept...\n");
	status = ave_wait_q1(q, 10000);
	if (status) {
		pr_err("apple-ave: [frameN] Q1 EncodeFrame accept TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("apple-ave: [frameN] EncodeFrame accepted\n");

	/* Unmap DMA now — T2 has accepted the frame data */
	bce_unmap_dma_buffer(dev, &y_dma);
	y_mapped = false;
	bce_unmap_dma_buffer(dev, &uv_dma);
	uv_mapped = false;

	/* === Send CompleteFrames (triggers actual encoding) === */
	ave_build_cmd_complete_frames(session->cmd_buf);
	ave_stamp_session_token(session);

	status = ave_submit_q0_cmd(q, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [frameN] Q0 CompleteFrames submit failed (%d)\n", status);
		goto fail;
	}

	status = ave_submit_q1_recv(q);
	if (status) {
		pr_err("apple-ave: [frameN] Q1 recv for CompleteFrames failed (%d)\n", status);
		goto fail;
	}

	/* === Service Q3 FIRST (2 events for Frame 2+) === */

	/* Q3 #1: metadata (~8ms) */
	pr_debug("apple-ave: [frameN] waiting for Q3 #1 (metadata)...\n");
	status = ave_wait_q3(q, 10000);
	if (status) {
		pr_err("apple-ave: [frameN] Q3 #1 (metadata) TIMEOUT\n");
		goto fail;
	}
	pr_debug("apple-ave: [frameN] Q3 #1 (metadata) received\n");

	/* Q3 #2: encoded NAL data — result scalar = encoded size */
	pr_debug("apple-ave: [frameN] waiting for Q3 #2 (output)...\n");
	status = ave_wait_q3(q, 10000);
	if (status) {
		pr_err("apple-ave: [frameN] Q3 #2 (output) TIMEOUT\n");
		goto fail;
	}

	raw_size = ave_q3_completed_size(q, 1);
	pr_debug("apple-ave: [frameN] Q3 #2 (output) received, encoded_size=%zu\n", raw_size);

	if (raw_size > AVE_MAX_ENCODED_SIZE) {
		pr_err("apple-ave: [frameN] encoded size %zu exceeds buffer size %d\n",
		       raw_size, AVE_MAX_ENCODED_SIZE);
		status = -ENOSPC;
		goto fail;
	}

	/* Echo metadata callback on Q2 — echoes Q3 event #0 (metadata) */
	q3_data = ave_q3_completed_data(q, 0);
	status = ave_submit_q2_echo(q, q3_data);
	if (status)
		pr_warn("apple-ave: [frameN] Q2 echo (metadata) failed (%d)\n", status);

	/* === Wait Q1: CompleteFrames done (near-instant) === */
	pr_debug("apple-ave: [frameN] waiting for Q1 CompleteFrames...\n");
	status = ave_wait_q1(q, 10000);
	if (status) {
		pr_err("apple-ave: [frameN] Q1 CompleteFrames TIMEOUT or error (%d)\n", status);
		goto fail;
	}
	pr_debug("apple-ave: [frameN] CompleteFrames done\n");

	/* === Build output === */
	if (raw_size == 0) {
		pr_warn("apple-ave: [frameN] T2 returned 0 bytes\n");
		*encoded_size = 0;
		goto resubmit;
	}

	/* Prepend Annex B header for keyframes */
	if (is_keyframe && session->annex_b_header && session->annex_b_header_size > 0) {
		if (session->annex_b_header_size > out_buf_size) {
			status = -ENOSPC;
			goto fail;
		}
		memcpy(out_buf, session->annex_b_header, session->annex_b_header_size);
		out_pos = session->annex_b_header_size;
	}

	/* Convert length-prefixed NALs to Annex B */
	q3_data = ave_q3_completed_data(q, 1);
	status = ave_convert_to_annex_b(q3_data, raw_size,
					(u8 *)out_buf + out_pos,
					out_buf_size - out_pos, encoded_size);
	if (status) {
		pr_err("apple-ave: [frameN] Annex B conversion failed (%d)\n", status);
		goto fail;
	}
	*encoded_size += out_pos;

resubmit:
	ave_resubmit_q3_bufs(q, q->q3_event_count);
	return 0;

fail:
	if (y_mapped)
		bce_unmap_dma_buffer(dev, &y_dma);
	if (uv_mapped)
		bce_unmap_dma_buffer(dev, &uv_dma);
	session->state = AVE_STATE_ERROR;
	return status ? status : -EIO;
}

int ave_session_encode_frame(struct ave_session *session,
			     void *y_data, size_t y_size,
			     void *uv_data, size_t uv_size,
			     void *out_buf, size_t out_buf_size,
			     size_t *encoded_size, bool force_keyframe)
{
	int status;
	bool is_first, is_keyframe;

	if (session->state != AVE_STATE_CONFIGURED &&
	    session->state != AVE_STATE_ENCODING)
		return -EINVAL;

	session->state = AVE_STATE_ENCODING;
	session->frame_counter++;
	is_first = (session->frame_counter == 1);
	is_keyframe = force_keyframe || is_first;

	/* Pre-submit Q2/Q3 receive buffers on first frame (macOS pattern) */
	if (is_first) {
		pr_debug("apple-ave: [encode] first frame — pre-submitting Q2/Q3 buffers\n");
		ave_presubmit_recv_bufs(&session->queues);
	}

	pr_debug("apple-ave: === ENCODE FRAME %llu (%s) ===\n",
		session->frame_counter, is_first ? "Frame1" : "FrameN");

	/* Build EncodeFrame command */
	ave_build_cmd_encode_frame(session->cmd_buf, session->frame_counter,
				   session->width, session->height,
				   session->fps_num, session->fps_den,
				   is_keyframe,
				   (u64)(uintptr_t)y_data);
	ave_stamp_session_token(session);

	if (is_first)
		status = ave_encode_first_frame(session, y_data, y_size,
					    uv_data, uv_size,
					    out_buf, out_buf_size, encoded_size);
	else
		status = ave_encode_next_frame(session, y_data, y_size,
					    uv_data, uv_size,
					    out_buf, out_buf_size, encoded_size,
					    is_keyframe);

	if (status) {
		pr_err("apple-ave: === FRAME %llu FAILED (%d) ===\n",
		       session->frame_counter, status);
		return status;
	}

	pr_debug("apple-ave: === FRAME %llu ENCODED: %zu bytes ===\n",
		session->frame_counter, *encoded_size);
	return 0;
}

/*
 * Convert HEVC length-prefixed NAL units to Annex B format.
 * Input: [4-byte length][NAL data][4-byte length][NAL data]...
 * Output: [00 00 00 01][NAL data][00 00 00 01][NAL data]...
 */
int ave_convert_to_annex_b(const void *src, size_t src_size,
			   void *dst, size_t dst_size, size_t *out_size)
{
	const u8 *in = src;
	u8 *out = dst;
	size_t in_pos = 0, out_pos = 0;
	u32 nal_len;
	int nal_count = 0;
	static const u8 start_code[4] = {0x00, 0x00, 0x00, 0x01};

	pr_debug("apple-ave: [annexb] converting %zu bytes\n", src_size);

	while (in_pos + 4 <= src_size) {
		/* Read 4-byte big-endian NAL length */
		nal_len = ((u32)in[in_pos] << 24) |
			  ((u32)in[in_pos + 1] << 16) |
			  ((u32)in[in_pos + 2] << 8) |
			  ((u32)in[in_pos + 3]);
		in_pos += 4;

		if (nal_len == 0 || in_pos + nal_len > src_size) {
			pr_warn("apple-ave: [annexb] invalid NAL length %u at offset %zu (remaining %zu)\n",
				nal_len, in_pos - 4, src_size - in_pos);
			break;
		}

		if (out_pos + 4 + nal_len > dst_size) {
			pr_err("apple-ave: [annexb] output buffer too small\n");
			return -ENOSPC;
		}

		/* Log NAL type for HEVC: type is bits 1-6 of first byte */
		if (nal_len >= 2) {
			u8 nal_type = (in[in_pos] >> 1) & 0x3f;
			pr_debug("apple-ave: [annexb] NAL #%d: type=%u len=%u\n",
				nal_count, nal_type, nal_len);
		}

		/* Write Annex B start code */
		memcpy(out + out_pos, start_code, 4);
		out_pos += 4;

		/* Copy NAL data */
		memcpy(out + out_pos, in + in_pos, nal_len);
		out_pos += nal_len;

		in_pos += nal_len;
		nal_count++;
	}

	*out_size = out_pos;
	pr_debug("apple-ave: [annexb] converted %d NALs, %zu -> %zu bytes\n",
		nal_count, src_size, out_pos);
	return 0;
}
