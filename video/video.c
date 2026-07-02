#define pr_fmt(fmt) "apple-ave: " fmt

#include "encoder.h"
#include "../apple_bce.h"

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/net.h>
#include <linux/un.h>
#include <net/sock.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

#define AVE_NAME		"apple-ave"
#define AVE_DEFAULT_WIDTH	1920
#define AVE_DEFAULT_HEIGHT	1080
#define AVE_MIN_WIDTH		128
#define AVE_MIN_HEIGHT		128
#define AVE_MAX_WIDTH		4096
#define AVE_MAX_HEIGHT		2304
#define AVE_DEFAULT_BITRATE	4000000
#define AVE_MIN_BITRATE		100000
#define AVE_MAX_BITRATE		100000000
#define AVE_DEFAULT_FPS_NUM	30
#define AVE_DEFAULT_FPS_DEN	1

/* Bound on all daemon socket operations — a stuck daemon must not hang
 * streamon/device_run in uninterruptible sleep */
#define AVE_XPC_TIMEOUT_MS	2000

/* NV12 frame size: Y + UV = w*h + w*h/2 = w*h*3/2 */
#define AVE_NV12_SIZE(w, h)	((w) * (h) * 3 / 2)
#define AVE_NV12_Y_SIZE(w, h)	((w) * (h))
#define AVE_NV12_UV_SIZE(w, h)	((w) * (h) / 2)

/* Capture (encoded) buffer size: worst case bounded by the raw frame size,
 * but floored so tiny resolutions still fit a first IDR with headers */
#define AVE_MIN_CAP_SIZEIMAGE	(256 * 1024)
#define AVE_CAP_SIZEIMAGE(w, h) \
	max_t(u32, AVE_NV12_SIZE(w, h), AVE_MIN_CAP_SIZEIMAGE)

static char *sock_path = "/run/aveserverd.sock";
module_param(sock_path, charp, 0644);
MODULE_PARM_DESC(sock_path, "Unix socket path for aveserverd daemon");

struct ave_device {
	struct apple_bce_device *bce;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex dev_mutex;
	struct mutex session_mutex;
	struct ave_session *session;
};

struct ave_ctx {
	struct v4l2_fh fh;
	struct ave_device *dev;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	struct v4l2_ctrl_handler ctrl_handler;
	u32 bitrate;
	/* Set by s_ctrl; device_run applies the new bitrate to the live
	 * session before the next frame (s_ctrl must not block on, or
	 * reach into, another fd's session). */
	bool bitrate_dirty;
	bool force_keyframe;
	u32 fps_num;
	u32 fps_den;
	s32 gop_size;
	s32 bitrate_mode;
	s32 quality;
	s32 min_qp;
	s32 max_qp;
	bool min_qp_set;
	bool max_qp_set;
	s32 profile;
	s32 level;
};

static const struct v4l2_event ave_eos_event = {
	.type = V4L2_EVENT_EOS
};

static int ave_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq);

/* === Format helpers === */

static void ave_set_default_src_fmt(struct v4l2_pix_format_mplane *f)
{
	memset(f, 0, sizeof(*f));
	f->width = AVE_DEFAULT_WIDTH;
	f->height = AVE_DEFAULT_HEIGHT;
	f->pixelformat = V4L2_PIX_FMT_NV12;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->num_planes = 1;
	f->plane_fmt[0].sizeimage = AVE_NV12_SIZE(AVE_DEFAULT_WIDTH, AVE_DEFAULT_HEIGHT);
	f->plane_fmt[0].bytesperline = AVE_DEFAULT_WIDTH;
}

static void ave_set_default_dst_fmt(struct v4l2_pix_format_mplane *f)
{
	memset(f, 0, sizeof(*f));
	f->width = AVE_DEFAULT_WIDTH;
	f->height = AVE_DEFAULT_HEIGHT;
	f->pixelformat = V4L2_PIX_FMT_HEVC;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_REC709;
	f->num_planes = 1;
	/* Allocate enough for worst-case encoded output */
	f->plane_fmt[0].sizeimage = AVE_CAP_SIZEIMAGE(AVE_DEFAULT_WIDTH, AVE_DEFAULT_HEIGHT);
	f->plane_fmt[0].bytesperline = 0;
}

/* === V4L2 IOCTL ops === */

static int ave_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct ave_device *adev = video_drvdata(file);

	strscpy(cap->driver, AVE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple T2 HEVC Encoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(adev->bce->pci));
	return 0;
}

static int ave_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		switch (f->index) {
		case 0:
			f->pixelformat = V4L2_PIX_FMT_NV12;
			return 0;
		case 1:
			f->pixelformat = V4L2_PIX_FMT_NV12M;
			return 0;
		default:
			return -EINVAL;
		}
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		if (f->index != 0)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_HEVC;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		return 0;
	}
	return -EINVAL;
}

static int ave_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		f->fmt.pix_mp = ctx->src_fmt;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		f->fmt.pix_mp = ctx->dst_fmt;
	else
		return -EINVAL;
	return 0;
}

static void ave_fill_src_fmt(struct v4l2_pix_format_mplane *pix, u32 w, u32 h,
			     bool multiplanar)
{
	pix->width = w;
	pix->height = h;
	pix->field = V4L2_FIELD_NONE;

	if (multiplanar) {
		pix->pixelformat = V4L2_PIX_FMT_NV12M;
		pix->num_planes = 2;
		pix->plane_fmt[0].sizeimage = AVE_NV12_Y_SIZE(w, h);
		pix->plane_fmt[0].bytesperline = w;
		pix->plane_fmt[1].sizeimage = AVE_NV12_UV_SIZE(w, h);
		pix->plane_fmt[1].bytesperline = w;
	} else {
		pix->pixelformat = V4L2_PIX_FMT_NV12;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = AVE_NV12_SIZE(w, h);
		pix->plane_fmt[0].bytesperline = w;
	}
}

static int ave_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	bool multi = (pix->pixelformat == V4L2_PIX_FMT_NV12M);
	u32 w, h;

	/* Buffer sizes and the device_run plane math derive from the
	 * format — refuse changes while buffers exist on either queue
	 * (this also propagates into the capture format below). */
	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;

	w = clamp(pix->width, (u32)AVE_MIN_WIDTH, (u32)AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)AVE_MIN_HEIGHT, (u32)AVE_MAX_HEIGHT);
	/* Align to 2 for NV12 chroma subsampling */
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	ave_fill_src_fmt(pix, w, h, multi);
	/* Accept user-provided colorspace, default to REC709 */
	if (!pix->colorspace)
		pix->colorspace = V4L2_COLORSPACE_REC709;

	ctx->src_fmt = *pix;

	/* Update capture format to match */
	ctx->dst_fmt.width = w;
	ctx->dst_fmt.height = h;
	ctx->dst_fmt.colorspace = pix->colorspace;
	ctx->dst_fmt.ycbcr_enc = pix->ycbcr_enc;
	ctx->dst_fmt.xfer_func = pix->xfer_func;
	ctx->dst_fmt.plane_fmt[0].sizeimage = AVE_CAP_SIZEIMAGE(w, h);

	return 0;
}

static int ave_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;

	pix->pixelformat = V4L2_PIX_FMT_HEVC;
	pix->width = ctx->src_fmt.width;
	pix->height = ctx->src_fmt.height;
	pix->field = V4L2_FIELD_NONE;
	/* The bitstream inherits the source colorimetry */
	pix->colorspace = ctx->src_fmt.colorspace;
	pix->ycbcr_enc = ctx->src_fmt.ycbcr_enc;
	pix->xfer_func = ctx->src_fmt.xfer_func;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = AVE_CAP_SIZEIMAGE(pix->width, pix->height);
	pix->plane_fmt[0].bytesperline = 0;

	ctx->dst_fmt = *pix;
	return 0;
}

static int ave_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	bool multi = (pix->pixelformat == V4L2_PIX_FMT_NV12M);
	u32 w, h;

	w = clamp(pix->width, (u32)AVE_MIN_WIDTH, (u32)AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)AVE_MIN_HEIGHT, (u32)AVE_MAX_HEIGHT);
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	ave_fill_src_fmt(pix, w, h, multi);

	return 0;
}

static int ave_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	u32 w, h;

	w = clamp(pix->width, (u32)AVE_MIN_WIDTH, (u32)AVE_MAX_WIDTH);
	h = clamp(pix->height, (u32)AVE_MIN_HEIGHT, (u32)AVE_MAX_HEIGHT);
	w = ALIGN(w, 2);
	h = ALIGN(h, 2);

	pix->width = w;
	pix->height = h;
	pix->pixelformat = V4L2_PIX_FMT_HEVC;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->plane_fmt[0].sizeimage = AVE_CAP_SIZEIMAGE(w, h);
	pix->plane_fmt[0].bytesperline = 0;

	return 0;
}

static int ave_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_NV12 &&
	    fsize->pixel_format != V4L2_PIX_FMT_NV12M &&
	    fsize->pixel_format != V4L2_PIX_FMT_HEVC)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = AVE_MIN_WIDTH;
	fsize->stepwise.max_width = AVE_MAX_WIDTH;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = AVE_MIN_HEIGHT;
	fsize->stepwise.max_height = AVE_MAX_HEIGHT;
	fsize->stepwise.step_height = 2;

	return 0;
}

/* === Colorspace mapping: V4L2 → ISO 23001-8 for T2 encoder === */

static s32 ave_v4l2_to_t2_primaries(enum v4l2_colorspace cs)
{
	switch (cs) {
	case V4L2_COLORSPACE_SMPTE170M:
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		return 6; /* BT.601 */
	case V4L2_COLORSPACE_BT2020:
		return 9;
	case V4L2_COLORSPACE_SMPTE240M:
		return 7;
	case V4L2_COLORSPACE_REC709:
	default:
		return 1; /* BT.709 */
	}
}

static s32 ave_v4l2_to_t2_matrix(enum v4l2_colorspace cs,
				  enum v4l2_ycbcr_encoding enc)
{
	if (enc == V4L2_YCBCR_ENC_BT2020)
		return 9;
	if (enc == V4L2_YCBCR_ENC_601)
		return 5;
	if (enc == V4L2_YCBCR_ENC_709 || enc == V4L2_YCBCR_ENC_DEFAULT) {
		switch (cs) {
		case V4L2_COLORSPACE_SMPTE170M:
		case V4L2_COLORSPACE_470_SYSTEM_BG:
			return 5; /* BT.601 */
		case V4L2_COLORSPACE_BT2020:
			return 9;
		default:
			return 1; /* BT.709 */
		}
	}
	return 1;
}

static s32 ave_v4l2_to_t2_xfer(enum v4l2_colorspace cs,
				enum v4l2_xfer_func xfer)
{
	if (xfer == V4L2_XFER_FUNC_SMPTE2084)
		return 16; /* PQ / HDR10 */
	if (xfer == V4L2_XFER_FUNC_709 || xfer == V4L2_XFER_FUNC_DEFAULT) {
		if (cs == V4L2_COLORSPACE_BT2020)
			return 14; /* BT.2020-10 */
		return 1; /* BT.709 */
	}
	return 1;
}

/* === Frame rate (S_PARM / G_PARM) === */

static int ave_g_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);

	if (sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		memset(&sp->parm, 0, sizeof(sp->parm));
		sp->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.output.timeperframe.numerator = ctx->fps_den;
		sp->parm.output.timeperframe.denominator = ctx->fps_num;
		return 0;
	}
	if (sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		memset(&sp->parm, 0, sizeof(sp->parm));
		sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		sp->parm.capture.timeperframe.numerator = ctx->fps_den;
		sp->parm.capture.timeperframe.denominator = ctx->fps_num;
		return 0;
	}
	return -EINVAL;
}

static int ave_s_parm(struct file *file, void *priv, struct v4l2_streamparm *sp)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);
	u32 num = 0, den = 0;

	if (sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
	    sp->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		num = sp->parm.output.timeperframe.numerator;
		den = sp->parm.output.timeperframe.denominator;
	} else if (sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
		   sp->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		num = sp->parm.capture.timeperframe.numerator;
		den = sp->parm.capture.timeperframe.denominator;
	} else {
		return -EINVAL;
	}

	if (num && den) {
		ctx->fps_den = num;
		ctx->fps_num = den;
	} else {
		ctx->fps_num = AVE_DEFAULT_FPS_NUM;
		ctx->fps_den = AVE_DEFAULT_FPS_DEN;
	}

	return ave_g_parm(file, priv, sp);
}

static int ave_encoder_cmd(struct file *file, void *priv,
			   struct v4l2_encoder_cmd *ec)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);
	int ret;

	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;

	/* A STOP issued when no OUTPUT buffers are pending stops the
	 * context immediately inside the m2m core — device_run never runs
	 * for it, so the EOS event must be sent here. The draining case
	 * sends it from device_run instead. */
	if (ec->cmd == V4L2_ENC_CMD_STOP &&
	    v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
	return 0;
}

static int ave_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops ave_ioctl_ops = {
	.vidioc_querycap		= ave_querycap,

	.vidioc_enum_fmt_vid_cap	= ave_enum_fmt,
	.vidioc_enum_fmt_vid_out	= ave_enum_fmt,

	.vidioc_g_fmt_vid_cap_mplane	= ave_g_fmt,
	.vidioc_g_fmt_vid_out_mplane	= ave_g_fmt,

	.vidioc_s_fmt_vid_cap_mplane	= ave_s_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane	= ave_s_fmt_out,

	.vidioc_try_fmt_vid_cap_mplane	= ave_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane	= ave_try_fmt_out,

	.vidioc_enum_framesizes		= ave_enum_framesizes,

	.vidioc_g_parm			= ave_g_parm,
	.vidioc_s_parm			= ave_s_parm,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_encoder_cmd		= ave_encoder_cmd,
	.vidioc_try_encoder_cmd		= v4l2_m2m_ioctl_try_encoder_cmd,

	.vidioc_subscribe_event		= ave_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/* === vb2 queue ops === */

static int ave_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *fmt;
	int i;

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = &ctx->src_fmt;
	else
		fmt = &ctx->dst_fmt;

	if (*nplanes) {
		/* Verify existing configuration */
		if (*nplanes != fmt->num_planes)
			return -EINVAL;
		for (i = 0; i < fmt->num_planes; i++)
			if (sizes[i] < fmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*nplanes = fmt->num_planes;
	for (i = 0; i < fmt->num_planes; i++)
		sizes[i] = fmt->plane_fmt[i].sizeimage;

	return 0;
}

static int ave_buf_prepare(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *fmt;
	int i;

	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = &ctx->src_fmt;
	else
		fmt = &ctx->dst_fmt;

	for (i = 0; i < fmt->num_planes; i++) {
		if (vb2_plane_size(vb, i) < fmt->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	/* For OUTPUT buffers, userspace sets bytesused; for CAPTURE, we set it later */
	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		for (i = 0; i < fmt->num_planes; i++)
			vb2_set_plane_payload(vb, i, fmt->plane_fmt[i].sizeimage);
	}

	return 0;
}

static void ave_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

/* === Daemon socket IPC for T2 XPC lifecycle === */

static int ave_daemon_cmd(const char *cmd)
{
	struct socket *sock;
	struct sockaddr_un addr;
	struct kvec send_vec, recv_vec;
	struct msghdr msg = {};
	char send_buf[16];
	char recv_buf[64];
	int ret, len;

	/* During process teardown, current->fs may be NULL. Unix socket
	 * connect needs path lookup which dereferences fs_struct. */
	if (!current->fs) {
		pr_debug("skipping XPC %s (no fs context)\n", cmd);
		return -ENOENT;
	}

	ret = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &sock);
	if (ret) {
		pr_warn("sock_create failed (%d)\n", ret);
		return ret;
	}

	/* Bound every socket operation (connect included — AF_UNIX connect
	 * honors sk_sndtimeo when the daemon's backlog is full) */
	sock->sk->sk_sndtimeo = msecs_to_jiffies(AVE_XPC_TIMEOUT_MS);
	sock->sk->sk_rcvtimeo = msecs_to_jiffies(AVE_XPC_TIMEOUT_MS);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strscpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

	ret = kernel_connect(sock, (struct sockaddr_unsized *)&addr,
			     offsetof(struct sockaddr_un, sun_path) +
			     strlen(addr.sun_path) + 1, 0);
	if (ret) {
		pr_warn("connect to %s failed (%d)\n",
			sock_path, ret);
		goto out;
	}

	/* Send "start\n" or "stop\n" */
	len = snprintf(send_buf, sizeof(send_buf), "%s\n", cmd);
	send_vec.iov_base = send_buf;
	send_vec.iov_len = len;

	ret = kernel_sendmsg(sock, &msg, &send_vec, 1, len);
	if (ret < 0) {
		pr_warn("sendmsg failed (%d)\n", ret);
		goto out;
	}

	/* Read response */
	memset(recv_buf, 0, sizeof(recv_buf));
	recv_vec.iov_base = recv_buf;
	recv_vec.iov_len = sizeof(recv_buf) - 1;

	ret = kernel_recvmsg(sock, &msg, &recv_vec, 1,
			     sizeof(recv_buf) - 1, 0);
	if (ret < 0) {
		pr_warn("recvmsg failed (%d)\n", ret);
		goto out;
	}

	recv_buf[ret] = '\0';

	if (strncmp(recv_buf, "OK", 2) == 0) {
		pr_info("XPC %s OK\n", cmd);
		ret = 0;
	} else {
		pr_warn("XPC %s failed: %s", cmd, recv_buf);
		ret = -EIO;
	}

out:
	sock_release(sock);
	return ret;
}

static int ave_xpc_start(void)
{
	return ave_daemon_cmd("start");
}

static int ave_xpc_stop(void)
{
	return ave_daemon_cmd("stop");
}

static int ave_xpc_recover(void)
{
	int ret;

	/* Recovery: send start, then stop to reset aveservice state */
	ret = ave_xpc_start();
	if (ret)
		return ret;

	/* Small delay between start and stop */
	msleep(100);

	return ave_xpc_stop();
}

static void ave_build_params(struct ave_ctx *ctx, struct ave_enc_params *p)
{
	enum v4l2_colorspace cs = ctx->src_fmt.colorspace;

	p->bitrate = ctx->bitrate;
	p->fps_num = ctx->fps_num;
	p->fps_den = ctx->fps_den;
	p->gop_size = ctx->gop_size;
	p->bitrate_mode = ctx->bitrate_mode;
	p->quality = ctx->quality;
	p->min_qp = ctx->min_qp;
	p->max_qp = ctx->max_qp;
	p->min_qp_set = ctx->min_qp_set;
	p->max_qp_set = ctx->max_qp_set;
	p->profile = ctx->profile;
	p->level = ctx->level;
	p->color_primaries = ave_v4l2_to_t2_primaries(cs);
	p->ycbcr_matrix = ave_v4l2_to_t2_matrix(cs, ctx->src_fmt.ycbcr_enc);
	p->transfer_func = ave_v4l2_to_t2_xfer(cs, ctx->src_fmt.xfer_func);
}

static int ave_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct ave_device *adev = ctx->dev;
	struct ave_enc_params params;
	struct vb2_v4l2_buffer *vbuf;
	int status = 0;

	if (vq->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);
		return 0;
	}

	ave_build_params(ctx, &params);

	mutex_lock(&adev->session_mutex);
	if (adev->session) {
		mutex_unlock(&adev->session_mutex);
		pr_err("encoder session already active\n");
		status = -EBUSY;
		goto return_bufs;
	}

	/* Start the T2-side service only once we know we own the slot, so
	 * a failed/busy streamon never sends an XPC start that could
	 * disturb another fd's active session. */
	ave_xpc_start();

	adev->session = kzalloc(sizeof(struct ave_session), GFP_KERNEL);
	if (!adev->session) {
		mutex_unlock(&adev->session_mutex);
		status = -ENOMEM;
		goto fail_xpc;
	}

	status = ave_session_setup(adev->session, adev->bce,
				   ctx->src_fmt.width, ctx->src_fmt.height,
				   &params);
	if (status) {
		/* Session setup failed — T2 may be wedged. Recover and retry once. */
		pr_warn("session setup failed (%d), attempting XPC recovery\n", status);
		kfree(adev->session);
		adev->session = NULL;

		ave_xpc_recover();

		adev->session = kzalloc(sizeof(struct ave_session), GFP_KERNEL);
		if (!adev->session) {
			mutex_unlock(&adev->session_mutex);
			status = -ENOMEM;
			goto fail_xpc;
		}

		status = ave_session_setup(adev->session, adev->bce,
					   ctx->src_fmt.width, ctx->src_fmt.height,
					   &params);
		if (status) {
			/* Still failing after recovery — give up */
			kfree(adev->session);
			adev->session = NULL;
			mutex_unlock(&adev->session_mutex);
			goto fail_xpc;
		}
	}
	mutex_unlock(&adev->session_mutex);

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, vq);

	return 0;

fail_xpc:
	/* Balance the XPC start above — no session survived it */
	ave_xpc_stop();
return_bufs:
	while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
	return status;
}

/* Tear down the active session, if any. Returns whether one existed. */
static bool ave_shutdown_session(struct ave_device *adev)
{
	bool had_session = false;

	mutex_lock(&adev->session_mutex);
	if (adev->session) {
		ave_session_teardown(adev->session);
		kfree(adev->session);
		adev->session = NULL;
		had_session = true;
	}
	mutex_unlock(&adev->session_mutex);
	return had_session;
}

static void ave_stop_streaming(struct vb2_queue *vq)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vq);
	struct ave_device *adev = ctx->dev;
	struct vb2_v4l2_buffer *vbuf;

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, vq);

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		ave_shutdown_session(adev);
		ave_xpc_stop();

		while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	} else {
		while ((vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops ave_vb2_ops = {
	.queue_setup		= ave_queue_setup,
	.buf_prepare		= ave_buf_prepare,
	.buf_queue		= ave_buf_queue,
	.start_streaming	= ave_start_streaming,
	.stop_streaming		= ave_stop_streaming,
};

/* === M2M ops === */

static void ave_device_run(void *priv)
{
	struct ave_ctx *ctx = priv;
	struct ave_device *adev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct vb2_buffer *src_vb, *dst_vb;
	enum vb2_buffer_state state = VB2_BUF_STATE_ERROR;
	void *y_data, *uv_data, *out_data;
	size_t y_size, uv_size, encoded_size = 0;
	bool keyframe = false;
	int status;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (!src_buf || !dst_buf) {
		pr_err("device_run called with missing buffers\n");
		v4l2_m2m_job_finish(adev->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	src_vb = &src_buf->vb2_buf;
	dst_vb = &dst_buf->vb2_buf;

	if (ctx->src_fmt.num_planes == 1) {
		/* Single-plane NV12: Y and UV are contiguous in one buffer */
		u32 w = ctx->src_fmt.width;
		u32 h = ctx->src_fmt.height;

		y_data = vb2_plane_vaddr(src_vb, 0);
		y_size = AVE_NV12_Y_SIZE(w, h);
		uv_data = y_data ? y_data + y_size : NULL;
		uv_size = AVE_NV12_UV_SIZE(w, h);
	} else {
		/* Multi-plane NV12M: separate Y and UV planes */
		y_data = vb2_plane_vaddr(src_vb, 0);
		y_size = vb2_get_plane_payload(src_vb, 0);
		uv_data = vb2_plane_vaddr(src_vb, 1);
		uv_size = vb2_get_plane_payload(src_vb, 1);
	}
	out_data = vb2_plane_vaddr(dst_vb, 0);

	if (!y_data || !uv_data || !out_data) {
		pr_err("failed to get buffer vaddrs\n");
		goto done;
	}

	mutex_lock(&adev->session_mutex);
	if (!adev->session || adev->session->state == AVE_STATE_ERROR) {
		mutex_unlock(&adev->session_mutex);
		pr_err("no active session\n");
		goto done;
	}

	/* Apply a bitrate change requested via s_ctrl since the last frame */
	if (READ_ONCE(ctx->bitrate_dirty)) {
		WRITE_ONCE(ctx->bitrate_dirty, false);
		ave_session_set_bitrate(adev->session, ctx->bitrate);
	}

	status = ave_session_encode_frame(adev->session,
					  y_data, y_size, uv_data, uv_size,
					  out_data, vb2_plane_size(dst_vb, 0),
					  &encoded_size, ctx->force_keyframe,
					  &keyframe);
	mutex_unlock(&adev->session_mutex);

	if (status) {
		pr_err("encode failed (%d), triggering XPC recovery\n", status);
		ave_xpc_recover();
		goto done;
	}

	/* Only clear a pending keyframe request once it was actually encoded */
	ctx->force_keyframe = false;

	vb2_set_plane_payload(dst_vb, 0, encoded_size);
	v4l2_m2m_buf_copy_metadata(src_buf, dst_buf);
	dst_buf->sequence = src_buf->sequence;
	dst_buf->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
			    V4L2_BUF_FLAG_BFRAME);
	dst_buf->flags |= keyframe ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	state = VB2_BUF_STATE_DONE;

done:
	/* Drain (V4L2_ENC_CMD_STOP) handling: tag the capture buffer for the
	 * last queued output buffer with LAST and signal EOS */
	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src_buf)) {
		dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
	}
	v4l2_m2m_buf_done_and_job_finish(adev->m2m_dev, ctx->fh.m2m_ctx, state);
}

static void ave_job_abort(void *priv)
{
	/* M2M framework handles cancellation; just let current job finish */
}

/*
 * No .job_ready: once both queues stream, jobs always run. When the
 * session is dead (encode failure, suspend), device_run returns each
 * buffer pair as VB2_BUF_STATE_ERROR so userspace unblocks with an error
 * instead of waiting on jobs that would never be scheduled.
 */
static const struct v4l2_m2m_ops ave_m2m_ops = {
	.device_run	= ave_device_run,
	.job_abort	= ave_job_abort,
};

/* === V4L2 controls === */

static int ave_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ave_ctx *ctx = container_of(ctrl->handler, struct ave_ctx, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		ctx->bitrate = ctrl->val;
		/* Applied to the live session by device_run before the next
		 * frame. Never touch adev->session here: this fd may not own
		 * the stream, and a T2 round-trip would block the control
		 * handler lock for seconds on a wedged device. */
		WRITE_ONCE(ctx->bitrate_dirty, true);
		return 0;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		ctx->force_keyframe = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->gop_size = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		ctx->bitrate_mode = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		ctx->quality = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
		ctx->min_qp = ctrl->val;
		ctx->min_qp_set = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:
		ctx->max_qp = ctrl->val;
		ctx->max_qp_set = true;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		ctx->profile = ctrl->val;
		return 0;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		ctx->level = ctrl->val;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops ave_ctrl_ops = {
	.s_ctrl = ave_s_ctrl,
};

static int ave_init_ctrls(struct ave_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_handler;

	v4l2_ctrl_handler_init(hdl, 10);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE,
			  AVE_MIN_BITRATE, AVE_MAX_BITRATE, 1,
			  AVE_DEFAULT_BITRATE);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  0, 600, 1, 0);

	v4l2_ctrl_new_std_menu(hdl, &ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ, 0,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
			  1, 100, 1, 65);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
			  0, 51, 1, 0);

	v4l2_ctrl_new_std(hdl, &ave_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
			  0, 51, 1, 0);

	v4l2_ctrl_new_std_menu(hdl, &ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);

	v4l2_ctrl_new_std_menu(hdl, &ave_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1);

	if (hdl->error)
		return hdl->error;

	ctx->fh.ctrl_handler = hdl;
	return 0;
}

/* === File operations === */

static int ave_open(struct file *file)
{
	struct ave_device *adev = video_drvdata(file);
	struct ave_ctx *ctx;
	int status;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = adev;
	ctx->bitrate = AVE_DEFAULT_BITRATE;
	ctx->fps_num = AVE_DEFAULT_FPS_NUM;
	ctx->fps_den = AVE_DEFAULT_FPS_DEN;
	ctx->gop_size = 0;
	ctx->bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
	ctx->quality = 65;
	ctx->profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
	ctx->level = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
	v4l2_fh_init(&ctx->fh, &adev->vdev);

	status = ave_init_ctrls(ctx);
	if (status)
		goto err_fh;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(adev->m2m_dev, ctx, ave_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		status = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrl;
	}

	ave_set_default_src_fmt(&ctx->src_fmt);
	ave_set_default_dst_fmt(&ctx->dst_fmt);

	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);

	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return status;
}

static int ave_release(struct file *file)
{
	struct ave_ctx *ctx = container_of(file->private_data, struct ave_ctx, fh);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations ave_fops = {
	.owner		= THIS_MODULE,
	.open		= ave_open,
	.release	= ave_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

/* === M2M queue init callback === */

static int ave_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct ave_ctx *ctx = priv;
	int status;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &ave_vb2_ops;
	src_vq->mem_ops = &vb2_vmalloc_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = &ctx->dev->bce->pci->dev;

	status = vb2_queue_init(src_vq);
	if (status)
		return status;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &ave_vb2_ops;
	dst_vq->mem_ops = &vb2_vmalloc_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = &ctx->dev->bce->pci->dev;

	return vb2_queue_init(dst_vq);
}

/* === Init/exit called from apple_bce.c === */

int bce_ave_create(struct apple_bce_device *bce)
{
	struct ave_device *adev;
	int status;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->bce = bce;
	mutex_init(&adev->dev_mutex);
	mutex_init(&adev->session_mutex);

	/* Register V4L2 device */
	status = v4l2_device_register(&adev->bce->pci->dev, &adev->v4l2_dev);
	if (status) {
		pr_err("v4l2_device_register failed (%d)\n", status);
		goto err_free;
	}

	/* Create M2M device */
	adev->m2m_dev = v4l2_m2m_init(&ave_m2m_ops);
	if (IS_ERR(adev->m2m_dev)) {
		status = PTR_ERR(adev->m2m_dev);
		pr_err("v4l2_m2m_init failed (%d)\n", status);
		goto err_v4l2;
	}

	/* Initialize video device */
	adev->vdev.fops = &ave_fops;
	adev->vdev.ioctl_ops = &ave_ioctl_ops;
	adev->vdev.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	adev->vdev.v4l2_dev = &adev->v4l2_dev;
	adev->vdev.release = video_device_release_empty;
	adev->vdev.vfl_dir = VFL_DIR_M2M;
	adev->vdev.lock = &adev->dev_mutex;
	strscpy(adev->vdev.name, "apple-ave-enc", sizeof(adev->vdev.name));

	video_set_drvdata(&adev->vdev, adev);

	status = video_register_device(&adev->vdev, VFL_TYPE_VIDEO, -1);
	if (status) {
		pr_err("video_register_device failed (%d)\n", status);
		goto err_m2m;
	}

	bce->ave = adev;
	pr_info("HEVC encoder registered as /dev/video%d\n",
		adev->vdev.num);
	return 0;

err_m2m:
	v4l2_m2m_release(adev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&adev->v4l2_dev);
err_free:
	kfree(adev);
	return status;
}

/*
 * Tear down any active session before the T2 goes to sleep: the firmware
 * session does not survive S3, and pre-submitted receive buffers must not
 * remain mapped for a device that will be power-cycled. After resume,
 * device_run finds no session and returns the queued buffers as errors,
 * so userspace unblocks and can restart streaming.
 *
 * No XPC stop here: userspace (including aveserverd) is already frozen at
 * this point in the suspend sequence, so the socket exchange could never
 * succeed — and the T2 power cycle resets the service state anyway.
 */
void bce_ave_suspend(struct apple_bce_device *bce)
{
	struct ave_device *adev = bce->ave;

	if (!adev)
		return;

	if (ave_shutdown_session(adev))
		pr_info("session torn down for suspend; streaming must be restarted after resume\n");
}

void bce_ave_destroy(struct apple_bce_device *bce)
{
	struct ave_device *adev = bce->ave;

	if (!adev)
		return;

	video_unregister_device(&adev->vdev);
	v4l2_m2m_release(adev->m2m_dev);
	v4l2_device_unregister(&adev->v4l2_dev);

	if (ave_shutdown_session(adev))
		ave_xpc_stop();

	kfree(adev);
	bce->ave = NULL;
	pr_info("encoder unregistered\n");
}
