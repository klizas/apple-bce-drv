#include "encoder.h"
#include "../apple_bce.h"

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>

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

int ave_session_setup(struct ave_session *session, struct apple_bce_device *bce,
		      u32 width, u32 height, u32 bitrate, u32 fps_num, u32 fps_den)
{
	int status;

	pr_debug("apple-ave: === SESSION SETUP START ===\n");
	pr_debug("apple-ave: params: %ux%u @ %u bps, %u/%u fps\n",
		 width, height, bitrate, fps_num, fps_den);

	memset(session, 0, sizeof(*session));
	session->bce = bce;
	session->width = width;
	session->height = height;
	session->bitrate = bitrate;
	session->fps_num = fps_num;
	session->fps_den = fps_den;
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

	/* Step 7: Cmd 0x09 — SetProperty "RealTime" = 1 */
	pr_debug("apple-ave: [setup] Step 7: SetProperty RealTime = 1\n");
	status = ave_send_set_property_bool(session, "RealTime", true);
	if (status) {
		pr_err("apple-ave: [setup] RealTime FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 8: Cmd 0x09 — SetProperty "AllowFrameReordering" = false
	 * We disable B-frames explicitly (see BPictures=0 after Prepare),
	 * so no frame reordering is needed. */
	pr_debug("apple-ave: [setup] Step 8: SetProperty AllowFrameReordering = 0\n");
	status = ave_send_set_property_bool(session, "AllowFrameReordering", false);
	if (status) {
		pr_err("apple-ave: [setup] AllowFrameReordering FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 9: Cmd 0x09 — SetProperty "AverageBitRate" */
	pr_debug("apple-ave: [setup] Step 9: SetProperty AverageBitRate = %u\n", bitrate);
	status = ave_send_set_property_s32(session, "AverageBitRate", bitrate);
	if (status) {
		pr_err("apple-ave: [setup] AverageBitRate FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 10: Cmd 0x08 — CopyProperty "MVHEVCVideoLayerIDs" */
	pr_debug("apple-ave: [setup] Step 10: CopyProperty MVHEVCVideoLayerIDs\n");
	status = ave_send_copy_property(session, "MVHEVCVideoLayerIDs");
	if (status) {
		pr_err("apple-ave: [setup] MVHEVCVideoLayerIDs FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 11: Cmd 0x05 — PrepareToEncodeFrames */
	pr_debug("apple-ave: [setup] Step 11: PrepareToEncodeFrames\n");
	ave_build_cmd_prepare(session->cmd_buf);
	ave_stamp_session_token(session);
	status = ave_cmd_send_sync(&session->queues, session->cmd_buf, AVE_CMD_BUF_SIZE);
	if (status) {
		pr_err("apple-ave: [setup] Prepare FAILED (%d)\n", status);
		goto fail_queues;
	}
	pr_debug("apple-ave: [setup] Step 11: PrepareToEncodeFrames OK\n");

	/* Step 11b: Cmd 0x09 — SetProperty "BPictures" = 0
	 * T2's processPrepareToEncodeFrames internally sets BPictures=1
	 * for resolutions >1080p (pixels > 0x1FE000). B-frames require
	 * frame reordering which our pipeline doesn't support, causing
	 * RPS (Reference Picture Set) errors during decode. Override to 0
	 * after Prepare to disable B-frames unconditionally. */
	if ((u64)width * height > 0x1FE000) {
		pr_debug("apple-ave: [setup] Step 11b: SetProperty BPictures = 0 (override >1080p)\n");
		status = ave_send_set_property_s32(session, "BPictures", 0);
		if (status) {
			pr_err("apple-ave: [setup] BPictures FAILED (%d)\n", status);
			goto fail_queues;
		}
	}

	/* Step 12: Cmd 0x09 — SetProperty "ColorPrimaries" = 1 (BT.709) */
	pr_debug("apple-ave: [setup] Step 12: SetProperty ColorPrimaries = 1\n");
	status = ave_send_set_property_s32(session, "ColorPrimaries", 1);
	if (status) {
		pr_err("apple-ave: [setup] ColorPrimaries FAILED (%d)\n", status);
		goto fail_queues;
	}

	/* Step 13: Cmd 0x09 — SetProperty "YCbCrMatrix" = 1 (BT.709) */
	pr_debug("apple-ave: [setup] Step 13: SetProperty YCbCrMatrix = 1\n");
	status = ave_send_set_property_s32(session, "YCbCrMatrix", 1);
	if (status) {
		pr_err("apple-ave: [setup] YCbCrMatrix FAILED (%d)\n", status);
		goto fail_queues;
	}

	session->state = AVE_STATE_CONFIGURED;
	session->frame_counter = 0;
	pr_debug("apple-ave: === SESSION SETUP COMPLETE (%ux%u @ %u bps) ===\n",
		 width, height, bitrate);
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
