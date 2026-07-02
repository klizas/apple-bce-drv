#ifndef AVE_ENCODER_H
#define AVE_ENCODER_H

#include "protocol.h"

enum ave_session_state {
	AVE_STATE_IDLE,
	AVE_STATE_CONFIGURED,
	AVE_STATE_ENCODING,
	AVE_STATE_ERROR,
};

struct ave_enc_params {
	u32 bitrate;       /* AverageBitRate (bps) */
	u32 fps_num;       /* Frame rate numerator */
	u32 fps_den;       /* Frame rate denominator */
	s32 gop_size;      /* MaxKeyFrameInterval (0 = firmware default) */
	s32 bitrate_mode;  /* V4L2_MPEG_VIDEO_BITRATE_MODE_* (0=VBR, 1=CBR, 2=CQ) */
	s32 quality;       /* Quality 1-100 for CQ mode (maps to 0.01-1.0 float) */
	s32 min_qp;        /* MinAllowedFrameQP (only sent if min_qp_set) */
	s32 max_qp;        /* MaxAllowedFrameQP (only sent if max_qp_set) */
	bool min_qp_set;   /* user explicitly configured min_qp */
	bool max_qp_set;   /* user explicitly configured max_qp */
	s32 profile;       /* V4L2_MPEG_VIDEO_HEVC_PROFILE_* */
	s32 level;         /* V4L2_MPEG_VIDEO_HEVC_LEVEL_* */
	s32 color_primaries; /* ISO 23001-8 colour_primaries */
	s32 ycbcr_matrix;    /* ISO 23001-8 matrix_coefficients */
	s32 transfer_func;   /* ISO 23001-8 transfer_characteristics */
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

	/* Session state. AVE_STATE_IDLE means the queues are NOT alive
	 * (fresh session or torn down); teardown is a no-op in that state. */
	enum ave_session_state state;
	u64 frame_counter;

	/* Session token returned by T2 in CodecID response[+0x18] */
	u64 session_token;

	/* Scratch buffer for building commands */
	u8 cmd_buf[AVE_CMD_BUF_SIZE];

	/* hvcC decoder config extracted from Frame 1's first Q3 callback */
	void *hvcc_data;      /* ISO 14496-15 hvcC box content */
	size_t hvcc_size;

	/* Annex B header (VPS+SPS+PPS) derived from hvcC — prepended to IDR frames */
	void *annex_b_header;
	size_t annex_b_header_size;
};

int ave_session_setup(struct ave_session *session, struct apple_bce_device *bce,
		      u32 width, u32 height, const struct ave_enc_params *params);
void ave_session_teardown(struct ave_session *session);

/*
 * Encode one frame. *keyframe_out reports whether the produced bitstream
 * contains an IRAP (sync point) — this includes keyframes the firmware
 * generated on its own GOP schedule, not just requested ones.
 */
int ave_session_encode_frame(struct ave_session *session,
			     void *y_data, size_t y_size,
			     void *uv_data, size_t uv_size,
			     void *out_buf, size_t out_buf_size,
			     size_t *encoded_size, bool force_keyframe,
			     bool *keyframe_out);

/* Update AverageBitRate on a live session (between frames) */
int ave_session_set_bitrate(struct ave_session *session, u32 bitrate);

int ave_convert_to_annex_b(const void *src, size_t src_size,
			   void *dst, size_t dst_size, size_t *out_size);

#endif /* AVE_ENCODER_H */
