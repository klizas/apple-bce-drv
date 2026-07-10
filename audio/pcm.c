#define pr_fmt(fmt) "aaudio: " fmt

#include "pcm.h"
#include "audio.h"
#include <linux/dma-mapping.h>
#include <linux/version.h>

static u64 aaudio_get_alsa_fmtbit(struct aaudio_apple_description *desc)
{
    if (desc->format_flags & AAUDIO_FORMAT_FLAG_FLOAT) {
        if (desc->bits_per_channel == 32) {
            if (desc->format_flags & AAUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT_LE;
        } else if (desc->bits_per_channel == 64) {
            if (desc->format_flags & AAUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT64_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT64_LE;
        } else {
            pr_err("unsupported bits per channel for float format: %u\n", desc->bits_per_channel);
            return 0;
        }
    }
#define DEFINE_BPC_OPTION(val, b) \
    case val: \
        if (desc->format_flags & AAUDIO_FORMAT_FLAG_BIG_ENDIAN) { \
            if (desc->format_flags & AAUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## BE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## BE; \
        } else { \
            if (desc->format_flags & AAUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## LE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## LE; \
        }
    if (desc->format_flags & AAUDIO_FORMAT_FLAG_PACKED) {
        switch (desc->bits_per_channel) {
            case 8:
            case 16:
            case 32:
                break;
            DEFINE_BPC_OPTION(24, 24_3)
            default:
                pr_err("unsupported bits per channel for packed format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    if (desc->format_flags & AAUDIO_FORMAT_FLAG_ALIGNED_HIGH) {
        switch (desc->bits_per_channel) {
            DEFINE_BPC_OPTION(24, 32_)
            default:
                pr_err("unsupported bits per channel for high-aligned format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    switch (desc->bits_per_channel) {
        case 8:
            if (desc->format_flags & AAUDIO_FORMAT_FLAG_SIGNED)
                return SNDRV_PCM_FMTBIT_S8;
            else
                return SNDRV_PCM_FMTBIT_U8;
        DEFINE_BPC_OPTION(16, 16_)
        DEFINE_BPC_OPTION(24, 24_)
        DEFINE_BPC_OPTION(32, 32_)
        default:
            pr_err("unsupported bits per channel: %u\n", desc->bits_per_channel);
            return 0;
    }
}
int aaudio_create_hw_info(struct aaudio_apple_description *desc, struct snd_pcm_hardware *alsa_hw,
        size_t buf_size)
{
    uint rate;
    /* The descriptor comes from a device property query; if that query
     * failed it is all zeros and would cause divisions by zero here, in
     * the period-size constraint and in the timestamp handler. */
    if (!desc->bytes_per_packet || !desc->frames_per_packet || !desc->channels_per_frame) {
        pr_err("invalid stream descriptor (bpp=%u fpp=%u ch=%u)\n",
                desc->bytes_per_packet, desc->frames_per_packet, desc->channels_per_frame);
        return -EINVAL;
    }
    alsa_hw->info = (SNDRV_PCM_INFO_MMAP |
                     SNDRV_PCM_INFO_BLOCK_TRANSFER |
                     SNDRV_PCM_INFO_MMAP_VALID |
                     SNDRV_PCM_INFO_DOUBLE);
    if (desc->format_flags & AAUDIO_FORMAT_FLAG_NON_MIXABLE)
        pr_warn("unsupported hw flag: NON_MIXABLE\n");
    if (!(desc->format_flags & AAUDIO_FORMAT_FLAG_NON_INTERLEAVED))
        alsa_hw->info |= SNDRV_PCM_INFO_INTERLEAVED;
    alsa_hw->formats = aaudio_get_alsa_fmtbit(desc);
    if (!alsa_hw->formats)
        return -EINVAL;
    rate = (uint) aaudio_double_to_u64(desc->sample_rate_double);
    if (!rate) {
        pr_err("invalid stream descriptor sample rate\n");
        return -EINVAL;
    }
    alsa_hw->rates = snd_pcm_rate_to_rate_bit(rate);
    alsa_hw->rate_min = rate;
    alsa_hw->rate_max = rate;
    alsa_hw->channels_min = desc->channels_per_frame;
    alsa_hw->channels_max = desc->channels_per_frame;
    alsa_hw->buffer_bytes_max = buf_size;
    alsa_hw->period_bytes_min = desc->bytes_per_packet;
    alsa_hw->period_bytes_max = buf_size / 2;
    alsa_hw->periods_min = 2;
    alsa_hw->periods_max = (uint) (buf_size / desc->bytes_per_packet);
    pr_debug("aaudio_create_hw_info: format = %llu, rate = %u/%u. channels = %u, periods = %u, period size = %lu\n",
            alsa_hw->formats, alsa_hw->rate_min, alsa_hw->rates, alsa_hw->channels_min, alsa_hw->periods_min,
            alsa_hw->period_bytes_min);
    return 0;
}

static struct aaudio_stream *aaudio_pcm_stream(struct snd_pcm_substream *substream)
{
    struct aaudio_subdevice *sdev = snd_pcm_substream_chip(substream);
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        return &sdev->out_streams[substream->number];
    else
        return &sdev->in_streams[substream->number];
}

static void aaudio_pcm_period_work(struct work_struct *ws)
{
    struct aaudio_stream *stream = container_of(ws, struct aaudio_stream, period_work);

    if (!READ_ONCE(stream->started))
        return;
    snd_pcm_period_elapsed(stream->pcm_substream);
}

static enum hrtimer_restart aaudio_pcm_period_timer(struct hrtimer *timer)
{
    struct aaudio_stream *stream = container_of(timer, struct aaudio_stream, period_timer);

    if (!READ_ONCE(stream->started))
        return HRTIMER_NORESTART;
    schedule_work(&stream->period_work);
    hrtimer_forward_now(timer, ns_to_ktime(stream->period_time_ns));
    return HRTIMER_RESTART;
}

static int aaudio_pcm_open(struct snd_pcm_substream *substream)
{
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    pr_debug("aaudio_pcm_open\n");
    /* The PCM device is registered before the BufferStruct is parsed; if the
     * stream never got usable buffers or its descriptor was rejected, the hw
     * description is missing and the stream cannot be used. */
    if (!stream->alsa_hw_desc)
        return -ENXIO;
    substream->runtime->hw = *stream->alsa_hw_desc;

    stream->pcm_substream = substream;
    INIT_WORK(&stream->period_work, aaudio_pcm_period_work);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
    hrtimer_init(&stream->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    stream->period_timer.function = aaudio_pcm_period_timer;
#else
    hrtimer_setup(&stream->period_timer, aaudio_pcm_period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#endif

    /* Period size must be a multiple of the hardware packet size */
    snd_pcm_hw_constraint_step(substream->runtime, 0,
            SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
            stream->desc.frames_per_packet);

    /* The T2 always traverses the whole ring; there is no way to tell it
     * about a smaller buffer. A sub-ring ALSA buffer would leave ring
     * regions the application never writes and break the once-per-ring
     * timestamp interpolation in the pointer callback. */
    snd_pcm_hw_constraint_single(substream->runtime,
            SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
            stream->buffers[0].size);

    return 0;
}

static int aaudio_pcm_close(struct snd_pcm_substream *substream)
{
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    pr_debug("aaudio_pcm_close\n");
    hrtimer_cancel(&stream->period_timer);
    cancel_work_sync(&stream->period_work);
    return 0;
}

static int aaudio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);

    /* Expose the device/stream pipeline latency through runtime->delay
     * (added to the buffer fill by snd_pcm_calc_delay()) instead of
     * skewing the hw pointer by it in aaudio_pcm_pointer(). */
    substream->runtime->delay = stream->latency;
    stream->buffer_time_ns = (s64) NSEC_PER_SEC * substream->runtime->buffer_size /
                             substream->runtime->rate;
    stream->period_time_ns = div_u64((u64) substream->runtime->period_size * NSEC_PER_SEC,
                                     substream->runtime->rate);
    return 0;
}

static int aaudio_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct aaudio_stream *astream = aaudio_pcm_stream(substream);
    pr_debug("aaudio_pcm_hw_params\n");

    if (!astream->buffer_cnt || !astream->buffers)
        return -EINVAL;

    substream->runtime->dma_area = astream->buffers[0].ptr;
    substream->runtime->dma_addr = astream->buffers[0].dma_addr;
    substream->runtime->dma_bytes = astream->buffers[0].size;
    return 0;
}

static int aaudio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    pr_debug("aaudio_pcm_hw_free\n");
    return 0;
}

static void aaudio_pcm_start(struct snd_pcm_substream *substream)
{
    struct aaudio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    void *buf = NULL;
    size_t s;
    ktime_t time_start, time_end;
    time_start = ktime_get();

    stream->waiting_for_first_ts = true;
    /* The pointer is no longer skewed back by the latency, so the startup
     * floor is 0 — the old latency floor existed to keep the skewed value
     * from going negative. */
    stream->frame_min = 0;

    s = frames_to_bytes(substream->runtime, substream->runtime->control->appl_ptr);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        /* Backup MMIO buffer before start_io (which may clear it),
         * then restore the pre-filled audio data afterwards. */
        buf = kvmalloc(s, GFP_KERNEL);
        if (buf)
            memcpy_fromio(buf, substream->runtime->dma_area, s);
        aaudio_cmd_start_io(sdev->a, sdev->dev_id);
        if (buf) {
            memcpy_toio(substream->runtime->dma_area, buf, s);
            kvfree(buf);
        }
    } else {
        aaudio_cmd_start_io(sdev->a, sdev->dev_id);
    }

    time_end = ktime_get();
    pr_debug("Started the audio device in %lluns\n", ktime_to_ns(time_end - time_start));
}

static int aaudio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct aaudio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    pr_debug("aaudio_pcm_trigger %x\n", cmd);

    /* We only supports triggers on the #0 buffer */
    if (substream->number != 0)
        return 0;
    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
            aaudio_pcm_start(substream);
            stream->started = 1;
            hrtimer_start(&stream->period_timer, ns_to_ktime(stream->period_time_ns),
                          HRTIMER_MODE_REL);
            break;
        case SNDRV_PCM_TRIGGER_STOP:
            aaudio_cmd_stop_io(sdev->a, sdev->dev_id);
            stream->started = 0;
            hrtimer_cancel(&stream->period_timer);
            break;
        case SNDRV_PCM_TRIGGER_SUSPEND:
            /* IO already stopped by aaudio_suspend */
            hrtimer_cancel(&stream->period_timer);
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t aaudio_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    ktime_t time_from_start;
    snd_pcm_sframes_t frames;
    snd_pcm_sframes_t buffer_time_length;

    if (!stream->started || stream->waiting_for_first_ts)
        return 0;

    /* Approximate the pointer based on the last received timestamp */
    time_from_start = ktime_get_boottime() - stream->remote_timestamp;
    if (ktime_to_ns(time_from_start) < 0)
        return 0;
    buffer_time_length = stream->buffer_time_ns;
    if (buffer_time_length <= 0)
        return 0;
    frames = (ktime_to_ns(time_from_start) % buffer_time_length) * (snd_pcm_sframes_t)substream->runtime->buffer_size / buffer_time_length;
    if (ktime_to_ns(time_from_start) < buffer_time_length) {
        if (frames < stream->frame_min)
            frames = stream->frame_min;
        else
            stream->frame_min = 0;
    } else {
        if (ktime_to_ns(time_from_start) < 2 * buffer_time_length)
            stream->frame_min = frames;
        else
            stream->frame_min = 0; /* Heavy desync */
    }
    /* Return the true consumption position; the pipeline latency is
     * reported separately via runtime->delay (set in prepare). */
    return (snd_pcm_uframes_t) frames;
}

static int aaudio_pcm_mmap(struct snd_pcm_substream *substream,
                           struct vm_area_struct *vma)
{
    struct aaudio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct aaudio_stream *stream = aaudio_pcm_stream(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;

    /* Host-allocated (capture) buffers are coherent system RAM: map them
     * cacheable via the DMA API, which also translates correctly when the
     * dma_addr is an IOMMU IOVA rather than a physical address. */
    if (stream->host_allocated)
        return dma_mmap_coherent(&sdev->a->pci->dev, vma,
                stream->buffers[0].ptr, stream->buffers[0].dma_addr,
                stream->buffers[0].size);

    /* Use write-combining for playback (T2 BAR MMIO): PipeWire's stores are
     * batched into efficient PCI transactions instead of individual uncached
     * writes. */
    vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    return vm_iomap_memory(vma, runtime->dma_addr, runtime->dma_bytes);
}

static int aaudio_pcm_ack(struct snd_pcm_substream *substream)
{
    /* Flush write-combine buffers so the T2 sees fresh audio data. */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        wmb();
    return 0;
}

static const struct snd_pcm_ops aaudio_pcm_ops = {
        .open =        aaudio_pcm_open,
        .close =       aaudio_pcm_close,
        .ioctl =       snd_pcm_lib_ioctl,
        .hw_params =   aaudio_pcm_hw_params,
        .hw_free =     aaudio_pcm_hw_free,
        .prepare =     aaudio_pcm_prepare,
        .trigger =     aaudio_pcm_trigger,
        .pointer =     aaudio_pcm_pointer,
        .mmap    =     aaudio_pcm_mmap,
        .ack     =     aaudio_pcm_ack
};

int aaudio_create_pcm(struct aaudio_subdevice *sdev)
{
    struct snd_pcm *pcm;
    struct aaudio_alsa_pcm_id_mapping *id_mapping;
    int err;

    if (!sdev->is_pcm || (sdev->in_stream_cnt == 0 && sdev->out_stream_cnt == 0)) {
        return -EINVAL;
    }

    for (id_mapping = aaudio_alsa_id_mappings; id_mapping->name; id_mapping++) {
        if (!strcmp(sdev->uid, id_mapping->name)) {
            sdev->alsa_id = id_mapping->alsa_id;
            break;
        }
    }
    if (!id_mapping->name)
        sdev->alsa_id = sdev->a->next_alsa_id++;
    err = snd_pcm_new(sdev->a->card, sdev->uid, sdev->alsa_id,
            (int) sdev->out_stream_cnt, (int) sdev->in_stream_cnt, &pcm);
    if (err < 0)
        return err;
    pcm->private_data = sdev;
    pcm->nonatomic = 1;
    sdev->pcm = pcm;
    strscpy(pcm->name, sdev->uid, sizeof(pcm->name));
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &aaudio_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &aaudio_pcm_ops);
    return 0;
}

static void aaudio_handle_stream_timestamp(struct snd_pcm_substream *substream, ktime_t timestamp)
{
    unsigned long flags;
    struct aaudio_stream *stream;
    struct snd_pcm_runtime *runtime;

    stream = aaudio_pcm_stream(substream);
    snd_pcm_stream_lock_irqsave(substream, flags);
    runtime = substream->runtime;
    if (!runtime) {
        /* PCM was closed (e.g. PipeWire tore down the substream) while a
         * timestamp message was still in flight from the T2 firmware. */
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    stream->remote_timestamp = timestamp;
    if (stream->waiting_for_first_ts) {
        stream->waiting_for_first_ts = false;
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    snd_pcm_stream_unlock_irqrestore(substream, flags);

    /* The T2 sends one timestamp per pass over the whole ring (16640
     * frames, ~347ms at 48kHz), not per hardware packet. */
    snd_pcm_period_elapsed(substream);
}

void aaudio_handle_timestamp(struct aaudio_subdevice *sdev, ktime_t os_timestamp, u64 dev_timestamp)
{
    struct snd_pcm_substream *substream;

    if (!sdev->pcm)
        return;
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    if (substream)
        aaudio_handle_stream_timestamp(substream, dev_timestamp);
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
    if (substream)
        aaudio_handle_stream_timestamp(substream, os_timestamp);
}
