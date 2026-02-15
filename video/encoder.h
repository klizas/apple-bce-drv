#ifndef AVE_ENCODER_H
#define AVE_ENCODER_H

#include "protocol.h"

enum ave_session_state {
	AVE_STATE_IDLE,
	AVE_STATE_CONFIGURED,
	AVE_STATE_ENCODING,
	AVE_STATE_ERROR,
};

struct ave_session {
	struct apple_bce_device *bce;
	struct ave_queues queues;

	/* Format parameters */
	u32 width;
	u32 height;
	u32 bitrate;
	u32 fps_num;
	u32 fps_den;

	/* Session state */
	enum ave_session_state state;
	u64 frame_counter;

	/* Session token returned by T2 in CodecID response[+0x18] */
	u64 session_token;

	/* Scratch buffer for building commands (4KB, kmalloc'd) */
	void *cmd_buf;

	/* hvcC decoder config extracted from Frame 1's first Q3 callback */
	void *hvcc_data;      /* ISO 14496-15 hvcC box content */
	size_t hvcc_size;

	/* Annex B header (VPS+SPS+PPS) derived from hvcC — prepended to IDR frames */
	void *annex_b_header;
	size_t annex_b_header_size;
};

int ave_session_setup(struct ave_session *session, struct apple_bce_device *bce,
		      u32 width, u32 height, u32 bitrate, u32 fps_num, u32 fps_den);
void ave_session_teardown(struct ave_session *session);

int ave_session_encode_frame(struct ave_session *session,
			     void *y_data, size_t y_size,
			     void *uv_data, size_t uv_size,
			     void *out_buf, size_t out_buf_size,
			     size_t *encoded_size, bool force_keyframe);

int ave_convert_to_annex_b(const void *src, size_t src_size,
			   void *dst, size_t dst_size, size_t *out_size);

#endif /* AVE_ENCODER_H */
