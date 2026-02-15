#ifndef AVE_PROTOCOL_H
#define AVE_PROTOCOL_H

#include "../queue.h"
#include "../queue_dma.h"

#define AVE_CMD_BUF_SIZE	4096
#define AVE_CQ_DEPTH		256
#define AVE_SQ_DEPTH		128
/* Session token — returned by T2 in CodecID response[+0x18], written at +0x08 in all
 * subsequent commands. Previously hardcoded as 0xFEEDBEEF, but it is a dynamic value. */

/* Maximum encoded output size per frame (2MB covers 4K keyframes at high bitrates) */
#define AVE_MAX_ENCODED_SIZE	(2 * 1024 * 1024)

/* hvcC parsing constants (ISO/IEC 14496-15) */
#define AVE_HVCC_OFFSET		0x68
#define AVE_HVCC_HEADER_SIZE	23

/* Timeout constants (milliseconds) */
#define AVE_SUBMIT_TIMEOUT_MS	5000
#define AVE_RESPONSE_TIMEOUT_MS	10000

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

	/* Synchronous command completion (signalled by Q1 only) */
	struct completion cmd_completion;
	int cmd_status;
	size_t cmd_resp_size;
	void *cmd_resp_buf; /* points into q1_buf ring — valid until next command */

	/* Frame completion tracking */
	struct completion q3_completion;

	/* Per-event Q3 tracking for async encoding pipeline */
	size_t q3_result[AVE_OUTPUT_BUF_COUNT]; /* result scalar per completed element */
	size_t q3_event_count;                  /* number of Q3 events since last reset */
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
void ave_build_cmd_prepare(void *buf);
void ave_build_cmd_complete_frames(void *buf);
void ave_build_cmd_end_session(void *buf);

/* Pre-submit empty receive buffers on Q2/Q3 (Q1 is paired with Q0 per-command) */
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
int ave_wait_q3(struct ave_queues *queues, unsigned long timeout_ms);

/* Q3 buffer access after completion */
void *ave_q3_completed_data(struct ave_queues *queues, size_t event_index);
size_t ave_q3_completed_size(struct ave_queues *queues, size_t event_index);

/* Reset Q3 tracking for new frame */
void ave_q3_reset(struct ave_queues *queues);

#endif /* AVE_PROTOCOL_H */
