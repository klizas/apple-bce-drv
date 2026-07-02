#ifndef AVE_PROTOCOL_H
#define AVE_PROTOCOL_H

#include "../queue.h"
#include "../queue_dma.h"

#include <linux/wait.h>

#define AVE_CMD_BUF_SIZE	4096
#define AVE_CQ_DEPTH		256
#define AVE_SQ_DEPTH		128
/* Session token — returned by T2 in CodecID response[+0x18], written at +0x08 in all
 * subsequent commands. Previously hardcoded as 0xFEEDBEEF, but it is a dynamic value. */

/* Maximum encoded output size per frame (2MB covers 4K keyframes at high bitrates).
 * 8 of these are allocated as contiguous DMA buffers, so keep this reasonable.
 * Even at 100 Mbps / 30 fps, a worst-case IDR is well under 2 MB. */
#define AVE_MAX_ENCODED_SIZE	(2 * 1024 * 1024)

/*
 * How much of a receive buffer is filled with the 0xFF sentinel before
 * submission. Everything the driver ever inspects (headers, metadata, the
 * hvcC blob) lives in the first 4KB; the encoded payload beyond it is
 * validated against the completion's result scalar before being read.
 * Filling all 2MB of a Q3 buffer would burn milliseconds of uncached
 * writes per buffer, partly from the completion IRQ (auto-resubmit path).
 * Trade-off: bytes past this window keep stale data from the slot's
 * previous use, so a firmware that overstates its result scalar would be
 * parsed as bitstream instead of hitting a clean 0xFFFFFFFF NAL-length
 * bail-out.
 */
#define AVE_SENTINEL_FILL_SIZE	AVE_CMD_BUF_SIZE

/* hvcC parsing constants (ISO/IEC 14496-15) */
#define AVE_HVCC_OFFSET		0x68
#define AVE_HVCC_HEADER_SIZE	23

/* Timeout constants (milliseconds) */
#define AVE_SUBMIT_TIMEOUT_MS	5000
#define AVE_RESPONSE_TIMEOUT_MS	10000
/* Per-event budget during frame encoding. A frame normally completes in
 * ~20-50ms; keep this short so a wedged T2 doesn't hold the session mutex
 * (and thus streamoff) hostage for tens of seconds. */
#define AVE_FRAME_TIMEOUT_MS	2000

/* Command types */
#define AVE_CMD_CODEC_ID		0x00
#define AVE_CMD_SESSION_CONFIG		0x01
#define AVE_CMD_ENCODE_FRAME		0x02
#define AVE_CMD_COMPLETE_FRAMES		0x03
#define AVE_CMD_PREPARE			0x05
#define AVE_CMD_END_SESSION		0x06
#define AVE_CMD_COPY_PROPERTY		0x08
#define AVE_CMD_SET_PROPERTY		0x09
#define AVE_CMD_DATA_NOTIFY		0x0a  /* T2→Host: encoder finished notification */
#define AVE_CMD_ENCODED_FRAME		0x0b  /* T2→Host: encoded frame callback */

/* Number of pre-submitted receive buffers per queue */
#define AVE_RECV_BUF_COUNT	8

/* Number of encoded output buffers on Q3 */
#define AVE_OUTPUT_BUF_COUNT	8

struct apple_bce_device;

/*
 * DMA ring buffer. head/tail are free-running counters (never wrapped);
 * index the arrays with (counter % el_count). tail is advanced by the
 * submitting (process) context, head by the completion (IRQ) context, so
 * (tail - head) is the number of in-flight elements and the ring is full
 * when tail - head == el_count.
 *
 * The counters double as the wait/publication protocol: for Q1 and Q3 the
 * completion IRQ advances head with smp_store_release() AFTER writing the
 * per-completion data (cmd_status/cmd_resp_buf, q3_result), and waiters
 * read it with smp_load_acquire(). Because a completion belongs to the
 * oldest outstanding submission, "head >= target tail" identifies exactly
 * which submissions have been answered — a response that arrives after its
 * command already timed out only advances head towards (never past) a
 * later command's target, so stale responses cannot satisfy a newer wait.
 */
struct ave_queue_buf {
	void **data;
	dma_addr_t *dma_addrs;
	size_t el_size;
	size_t el_count;
	size_t head, tail;
};

struct ave_queues {
	struct apple_bce_device *bce;

	struct bce_queue_cq *cq[4]; /* one CQ per SQ, matching macOS topology */

	/* Q0: AVEParameterSubmitQueue — Host→T2 (commands + frame data) */
	struct bce_queue_sq *sq_submit;
	/* Q1: AVEParameterReturnQueue — T2→Host (responses) */
	struct bce_queue_sq *sq_return;
	/* Q2: AVECallbackReturnQueue — Host→T2 (callback echo/ack, flags=3) */
	struct bce_queue_sq *sq_cb_return;
	/* Q3: AVECallbackSubmitQueue — T2→Host (callback data + encoded bitstream, flags=2) */
	struct bce_queue_sq *sq_cb_submit;

	/* Pre-allocated DMA ring buffer for Q0 commands (4KB each) */
	struct ave_queue_buf q0_buf;
	/* Pre-allocated DMA ring buffers for Q1/Q2 responses (4KB each) */
	struct ave_queue_buf q1_buf;
	struct ave_queue_buf q2_buf;
	/* Larger ring for Q3 encoded output */
	struct ave_queue_buf q3_buf;

	/* Q1 command/response synchronization (see struct ave_queue_buf on
	 * how q1_buf.head/tail double as the sequence counters). The status
	 * and response pointer are published by the release-store of
	 * q1_buf.head in the completion IRQ. */
	wait_queue_head_t cmd_wq;
	int cmd_status;
	void *cmd_resp_buf; /* points into q1_buf ring — valid until next command */

	/* Q3 event tracking: q3_base is the q3_buf.head snapshot taken by
	 * ave_q3_reset() at the start of a frame. Event k of the current
	 * frame has absolute sequence (q3_base + k), used ring slot
	 * ((q3_base + k) % el_count), and its result scalar lives in
	 * q3_result[(q3_base + k) % AVE_OUTPUT_BUF_COUNT]. */
	wait_queue_head_t q3_wq;
	u64 q3_base;
	size_t q3_result[AVE_OUTPUT_BUF_COUNT];
	bool q3_auto_resubmit;                  /* false during encoding to preserve data */
};

int ave_queues_create(struct apple_bce_device *bce, struct ave_queues *queues);
void ave_queues_destroy(struct ave_queues *queues);

int ave_cmd_send_sync(struct ave_queues *queues, void *cmd_buf, size_t cmd_size);

/* Command buffer builders — all operate on a 4096-byte buffer */
void ave_build_cmd_codec_id(void *buf);
void ave_build_cmd_session_config(void *buf, u32 width, u32 height);
void ave_build_cmd_encode_frame(void *buf, u64 frame_num, u32 width, u32 height,
				u32 fps_num, u32 fps_den, bool keyframe,
				u64 cookie);
void ave_build_cmd_copy_property(void *buf, const char *name);
void ave_build_cmd_set_property_bool(void *buf, const char *name, bool value);
void ave_build_cmd_set_property_s32(void *buf, const char *name, s32 value);
void ave_build_cmd_set_property_float32(void *buf, const char *name, u32 ieee754_bits);
void ave_build_cmd_set_property_string(void *buf, const char *name, const char *value);
void ave_build_cmd_prepare(void *buf);
void ave_build_cmd_complete_frames(void *buf);
void ave_build_cmd_end_session(void *buf);

/* Pre-submit empty receive buffers on Q3 (Q1 is paired with Q0 per-command) */
void ave_presubmit_recv_bufs(struct ave_queues *queues);

/* Low-level Q0/Q1/Q3 submission without blocking (for async encoding pipeline) */
int ave_submit_q0_cmd(struct ave_queues *queues, void *cmd_buf, size_t cmd_size);
int ave_submit_q1_recv(struct ave_queues *queues);
int ave_submit_q3_buf(struct ave_queues *queues);

/* Echo Q3 callback data back on Q2 (acknowledgment to T2) */
int ave_submit_q2_echo(struct ave_queues *queues, const void *q3_data);

/* Submit frame data (Y/UV) on Q0 without waiting — caller must unmap later */
int ave_submit_frame_data_async(struct ave_queues *queues, void *data, size_t size,
				struct bce_dma_buffer *dma_out);

/* Waiting primitives */
int ave_wait_q1(struct ave_queues *queues, unsigned long timeout_ms);
/* Wait until at least `count` Q3 events have arrived since ave_q3_reset() */
int ave_wait_q3(struct ave_queues *queues, size_t count, unsigned long timeout_ms);

/* Q3 buffer access after completion (event_index relative to ave_q3_reset) */
void *ave_q3_completed_data(struct ave_queues *queues, size_t event_index);
size_t ave_q3_completed_size(struct ave_queues *queues, size_t event_index);
/* Number of Q3 events received since ave_q3_reset() */
size_t ave_q3_event_count(struct ave_queues *queues);

/* Reset Q3 tracking for new frame */
void ave_q3_reset(struct ave_queues *queues);

#endif /* AVE_PROTOCOL_H */
