#define pr_fmt(fmt) "aaudio: " fmt

#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/random.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include "audio.h"
#include "pcm.h"

static int aaudio_alsa_index = SNDRV_DEFAULT_IDX1;
static char *aaudio_alsa_id = SNDRV_DEFAULT_STR1;

static struct class *aaudio_class;

static int aaudio_init_cmd(struct aaudio_device *a);
static int aaudio_init_bs(struct aaudio_device *a);
static void aaudio_init_dev(struct aaudio_device *a, aaudio_device_id_t dev_id);
static void aaudio_free_dev(struct aaudio_subdevice *sdev);
static void aaudio_ts_work(struct work_struct *ws);

static int aaudio_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct aaudio_device *aaudio = NULL;
    struct aaudio_subdevice *sdev = NULL;
    int status = 0;
    u32 cfg;

    pr_info("capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "aaudio")) {
        status = -ENODEV;
        goto fail;
    }
    pci_set_master(dev);

    aaudio = kzalloc(sizeof(struct aaudio_device), GFP_KERNEL);
    if (!aaudio) {
        status = -ENOMEM;
        goto fail;
    }
    init_completion(&aaudio->remote_alive);
    INIT_LIST_HEAD(&aaudio->subdevice_list);

    aaudio->bce = global_bce;
    if (!aaudio->bce) {
        dev_warn(&dev->dev, "No BCE available\n");
        status = -EINVAL;
        goto fail;
    }
    /* Force this device to unbind before apple-bce so global_bce cannot
     * dangle while aaudio is bound. */
    if (!device_link_add(&dev->dev, &aaudio->bce->pci->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
        dev_warn(&dev->dev, "failed to create device link to apple-bce\n");

    aaudio->pci = dev;
    pci_set_drvdata(dev, aaudio);

    aaudio->dev = device_create(aaudio_class, &dev->dev, MKDEV(0, 0), NULL, "aaudio");
    if (IS_ERR(aaudio->dev)) {
        status = PTR_ERR(aaudio->dev);
        aaudio->dev = NULL;
        goto fail;
    }
    device_link_add(aaudio->dev, aaudio->bce->dev, DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_CONSUMER);

    /* Init: set an unknown flag in the bitset */
    if (pci_read_config_dword(dev, 4, &cfg))
        dev_warn(&dev->dev, "pci_read_config_dword fail\n");
    if (pci_write_config_dword(dev, 4, cfg | 6u))
        dev_warn(&dev->dev, "pci_write_config_dword fail\n");

    dev_info(aaudio->dev, "bs len = %llx\n", pci_resource_len(dev, 0));
    aaudio->reg_mem_bs_dma = pci_resource_start(dev, 0);
    aaudio->reg_mem_bs = pci_iomap_wc(dev, 0, 0);
    aaudio->reg_mem_cfg = pci_iomap(dev, 4, 0);

    aaudio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) aaudio->reg_mem_cfg + 0xC000);

    if (IS_ERR_OR_NULL(aaudio->reg_mem_bs) || IS_ERR_OR_NULL(aaudio->reg_mem_cfg)) {
        dev_warn(&dev->dev, "Failed to pci_iomap required regions\n");
        goto fail;
    }

    if (aaudio_bce_init(aaudio)) {
        dev_warn(&dev->dev, "Failed to init BCE command transport\n");
        goto fail_bce;
    }

    if (snd_card_new(aaudio->dev, aaudio_alsa_index, aaudio_alsa_id, THIS_MODULE, 0, &aaudio->card)) {
        dev_err(&dev->dev, "Failed to create ALSA card\n");
        goto fail_bce;
    }

    strscpy(aaudio->card->shortname, "Apple T2 Audio", sizeof(aaudio->card->shortname));
    strscpy(aaudio->card->longname, "Apple T2 Audio", sizeof(aaudio->card->longname));
    strscpy(aaudio->card->mixername, "Apple T2 Audio", sizeof(aaudio->card->mixername));
    /* Dynamic alsa ids start at 100 */
    aaudio->next_alsa_id = 100;

    if (aaudio_init_cmd(aaudio)) {
        dev_err(&dev->dev, "Failed to initialize over BCE\n");
        goto fail_snd;
    }

    if (aaudio_init_bs(aaudio)) {
        dev_err(&dev->dev, "Failed to initialize BufferStruct\n");
        goto fail_snd;
    }

    if ((status = aaudio_cmd_set_remote_access(aaudio, AAUDIO_REMOTE_ACCESS_ON))) {
        dev_err(&dev->dev, "Failed to set remote access\n");
        goto fail_snd;
    }

    if (snd_card_register(aaudio->card)) {
        dev_err(&dev->dev, "Failed to register ALSA sound device\n");
        goto fail_snd;
    }

    list_for_each_entry(sdev, &aaudio->subdevice_list, list) {
        struct aaudio_buffer_struct_device *dev;

        if (sdev->buf_id >= ARRAY_SIZE(aaudio->bs->devices))
            continue;
        dev = &aaudio->bs->devices[sdev->buf_id];
        if (sdev->out_stream_cnt == 1 && !strcmp(dev->name, "Speaker")) {
            struct snd_pcm_hardware *hw = sdev->out_streams[0].alsa_hw_desc;

            if (hw)
                snprintf(aaudio->card->driver, sizeof(aaudio->card->driver) / sizeof(char), "AppleT2x%d", hw->channels_min);
        }
    }

    return 0;

fail_snd:
    snd_card_free(aaudio->card);
fail_bce:
    aaudio_bce_free(aaudio);
fail:
    if (aaudio) {
        while (!list_empty(&aaudio->subdevice_list)) {
            sdev = list_first_entry(&aaudio->subdevice_list, struct aaudio_subdevice, list);
            list_del(&sdev->list);
            aaudio_free_dev(sdev);
        }
        if (!IS_ERR_OR_NULL(aaudio->reg_mem_bs))
            pci_iounmap(dev, aaudio->reg_mem_bs);
        if (!IS_ERR_OR_NULL(aaudio->reg_mem_cfg))
            pci_iounmap(dev, aaudio->reg_mem_cfg);
        if (aaudio->dev)
            device_destroy(aaudio_class, MKDEV(0, 0));
        kfree(aaudio);
    }

    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}



static void aaudio_remove(struct pci_dev *dev)
{
    struct aaudio_subdevice *sdev;
    struct aaudio_device *aaudio = pci_get_drvdata(dev);

    snd_card_free(aaudio->card);
    while (!list_empty(&aaudio->subdevice_list)) {
        sdev = list_first_entry(&aaudio->subdevice_list, struct aaudio_subdevice, list);
        list_del(&sdev->list);
        aaudio_free_dev(sdev);
    }
    aaudio_bce_free(aaudio);
    pci_iounmap(dev, aaudio->reg_mem_bs);
    pci_iounmap(dev, aaudio->reg_mem_cfg);
    device_destroy(aaudio_class, MKDEV(0, 0));
    pci_release_regions(dev);
    pci_disable_device(dev);
    kfree(aaudio);
}

static int aaudio_suspend(struct device *dev)
{
    struct aaudio_device *aaudio = pci_get_drvdata(to_pci_dev(dev));
    struct aaudio_subdevice *sdev;
    size_t i;

    /* Stop running streams and tell ALSA they are suspended.  Without this,
     * ALSA keeps polling aaudio_pcm_pointer() after resume while
     * remote_timestamp is stale, producing wildly wrong position values
     * ("invalid position" errors). */
    list_for_each_entry(sdev, &aaudio->subdevice_list, list) {
        bool stopped_io = false;
        for (i = 0; i < sdev->out_stream_cnt; i++) {
            if (sdev->out_streams[i].started) {
                stopped_io = true;
                sdev->out_streams[i].started = 0;
            }
        }
        for (i = 0; i < sdev->in_stream_cnt; i++) {
            if (sdev->in_streams[i].started) {
                stopped_io = true;
                sdev->in_streams[i].started = 0;
            }
        }
        if (stopped_io)
            aaudio_cmd_stop_io(sdev->a, sdev->dev_id);
        if (sdev->pcm)
            snd_pcm_suspend_all(sdev->pcm);
    }

    if (aaudio_cmd_set_remote_access(aaudio, AAUDIO_REMOTE_ACCESS_OFF))
        dev_warn(aaudio->dev, "Failed to reset remote access\n");

    pci_disable_device(aaudio->pci);
    return 0;
}

static int aaudio_resume(struct device *dev)
{
    int status;
    struct aaudio_device *aaudio = pci_get_drvdata(to_pci_dev(dev));

    if ((status = pci_enable_device(aaudio->pci)))
        return status;
    pci_set_master(aaudio->pci);

    if ((status = aaudio_cmd_set_remote_access(aaudio, AAUDIO_REMOTE_ACCESS_ON))) {
        dev_err(aaudio->dev, "Failed to set remote access\n");
        return status;
    }

    return 0;
}

static int aaudio_init_cmd(struct aaudio_device *a)
{
    int status;
    struct aaudio_send_ctx sctx;
    struct aaudio_msg buf;
    u64 dev_cnt, dev_max, dev_i;
    size_t dev_off;
    aaudio_device_id_t *dev_l;

    if ((status = aaudio_send(a, &sctx, 500,
                              aaudio_msg_write_alive_notification, 1, 3))) {
        dev_err(a->dev, "Sending alive notification failed\n");
        return status;
    }

    if (wait_for_completion_timeout(&a->remote_alive, msecs_to_jiffies(500)) == 0) {
        dev_err(a->dev, "Timed out waiting for remote\n");
        return -ETIMEDOUT;
    }
    dev_info(a->dev, "Continuing init\n");

    buf = aaudio_reply_alloc();
    if (!buf.data)
        return -ENOMEM;
    if ((status = aaudio_cmd_get_device_list(a, &buf, &dev_l, &dev_cnt))) {
        dev_err(a->dev, "Failed to get device list\n");
        aaudio_reply_free(&buf);
        return status;
    }
    /* dev_cnt comes from the device; make sure the id list actually fits in the reply */
    dev_off = (size_t) ((u8 *) dev_l - (u8 *) buf.data);
    dev_max = buf.size > dev_off ? (buf.size - dev_off) / sizeof(aaudio_device_id_t) : 0;
    if (dev_cnt > dev_max) {
        dev_warn(a->dev, "Device list count %llu exceeds reply size, clamping to %llu\n", dev_cnt, dev_max);
        dev_cnt = dev_max;
    }
    for (dev_i = 0; dev_i < dev_cnt; ++dev_i)
        aaudio_init_dev(a, dev_l[dev_i]);
    aaudio_reply_free(&buf);

    return 0;
}

static void aaudio_init_stream_info(struct aaudio_subdevice *sdev, struct aaudio_stream *strm);
static void aaudio_handle_jack_connection_change(struct aaudio_subdevice *sdev);

static void aaudio_init_dev(struct aaudio_device *a, aaudio_device_id_t dev_id)
{
    struct aaudio_subdevice *sdev;
    struct aaudio_msg buf = aaudio_reply_alloc();
    u64 uid_len, stream_cnt, i;
    aaudio_object_id_t *stream_list;
    char *uid;

    sdev = kzalloc(sizeof(struct aaudio_subdevice), GFP_KERNEL);
    if (!sdev || !buf.data)
        goto fail;

    if (aaudio_cmd_get_property(a, &buf, dev_id, dev_id, AAUDIO_PROP(AAUDIO_PROP_SCOPE_GLOBAL, AAUDIO_PROP_UID, 0),
            NULL, 0, (void **) &uid, &uid_len) || uid_len > AAUDIO_DEVICE_MAX_UID_LEN) {
        dev_err(a->dev, "Failed to get device uid for device %llx\n", dev_id);
        goto fail;
    }
    dev_info(a->dev, "Remote device %llx %.*s\n", dev_id, (int) uid_len, uid);

    sdev->a = a;
    INIT_LIST_HEAD(&sdev->list);
    INIT_WORK(&sdev->ts_work, aaudio_ts_work);
    spin_lock_init(&sdev->ts_lock);
    sdev->dev_id = dev_id;
    sdev->buf_id = AAUDIO_BUFFER_ID_NONE;
    strncpy(sdev->uid, uid, uid_len);
    sdev->uid[uid_len] = '\0';

    if (aaudio_cmd_get_primitive_property(a, dev_id, dev_id,
            AAUDIO_PROP(AAUDIO_PROP_SCOPE_INPUT, AAUDIO_PROP_LATENCY, 0), NULL, 0, &sdev->in_latency, sizeof(u32)))
        dev_warn(a->dev, "Failed to query device input latency\n");
    if (aaudio_cmd_get_primitive_property(a, dev_id, dev_id,
            AAUDIO_PROP(AAUDIO_PROP_SCOPE_OUTPUT, AAUDIO_PROP_LATENCY, 0), NULL, 0, &sdev->out_latency, sizeof(u32)))
        dev_warn(a->dev, "Failed to query device output latency\n");

    if (aaudio_cmd_get_input_stream_list(a, &buf, dev_id, &stream_list, &stream_cnt)) {
        dev_err(a->dev, "Failed to get input stream list for device %llx\n", dev_id);
        goto fail;
    }
    if (stream_cnt > AAUDIO_DEVICE_MAX_INPUT_STREAMS) {
        dev_warn(a->dev, "Device %s input stream count %llu is larger than the supported count of %u\n",
                sdev->uid, stream_cnt, AAUDIO_DEVICE_MAX_INPUT_STREAMS);
        stream_cnt = AAUDIO_DEVICE_MAX_INPUT_STREAMS;
    }
    sdev->in_stream_cnt = stream_cnt;
    for (i = 0; i < stream_cnt; i++) {
        sdev->in_streams[i].id = stream_list[i];
        sdev->in_streams[i].buffer_cnt = 0;
        aaudio_init_stream_info(sdev, &sdev->in_streams[i]);
        sdev->in_streams[i].latency += sdev->in_latency;
    }

    if (aaudio_cmd_get_output_stream_list(a, &buf, dev_id, &stream_list, &stream_cnt)) {
        dev_err(a->dev, "Failed to get output stream list for device %llx\n", dev_id);
        goto fail;
    }
    if (stream_cnt > AAUDIO_DEVICE_MAX_OUTPUT_STREAMS) {
        dev_warn(a->dev, "Device %s output stream count %llu is larger than the supported count of %u\n",
                 sdev->uid, stream_cnt, AAUDIO_DEVICE_MAX_OUTPUT_STREAMS);
        stream_cnt = AAUDIO_DEVICE_MAX_OUTPUT_STREAMS;
    }
    sdev->out_stream_cnt = stream_cnt;
    for (i = 0; i < stream_cnt; i++) {
        sdev->out_streams[i].id = stream_list[i];
        sdev->out_streams[i].buffer_cnt = 0;
        aaudio_init_stream_info(sdev, &sdev->out_streams[i]);
        sdev->out_streams[i].latency += sdev->out_latency;
    }

    if (sdev->is_pcm)
        aaudio_create_pcm(sdev);
    /* Headphone Jack status */
    if (!strcmp(sdev->uid, "Codec Output")) {
        if (snd_jack_new(a->card, sdev->uid, SND_JACK_HEADPHONE, &sdev->jack, true, false))
            dev_warn(a->dev, "Failed to create an attached jack for %s\n", sdev->uid);
        aaudio_cmd_property_listener(a, sdev->dev_id, sdev->dev_id,
                AAUDIO_PROP(AAUDIO_PROP_SCOPE_OUTPUT, AAUDIO_PROP_JACK_PLUGGED, 0));
        aaudio_handle_jack_connection_change(sdev);
    }

    aaudio_reply_free(&buf);

    list_add_tail(&sdev->list, &a->subdevice_list);
    return;

fail:
    aaudio_reply_free(&buf);
    kfree(sdev);
}

static void aaudio_init_stream_info(struct aaudio_subdevice *sdev, struct aaudio_stream *strm)
{
    if (aaudio_cmd_get_primitive_property(sdev->a, sdev->dev_id, strm->id,
            AAUDIO_PROP(AAUDIO_PROP_SCOPE_GLOBAL, AAUDIO_PROP_PHYS_FORMAT, 0), NULL, 0,
            &strm->desc, sizeof(strm->desc)))
        dev_warn(sdev->a->dev, "Failed to query stream descriptor\n");
    if (aaudio_cmd_get_primitive_property(sdev->a, sdev->dev_id, strm->id,
            AAUDIO_PROP(AAUDIO_PROP_SCOPE_GLOBAL, AAUDIO_PROP_LATENCY, 0), NULL, 0, &strm->latency, sizeof(u32)))
        dev_warn(sdev->a->dev, "Failed to query stream latency\n");
    if (strm->desc.format_id == AAUDIO_FORMAT_LPCM)
        sdev->is_pcm = true;
}

static void aaudio_free_dev(struct aaudio_subdevice *sdev)
{
    size_t i;
    cancel_work_sync(&sdev->ts_work);
    for (i = 0; i < sdev->in_stream_cnt; i++) {
        kfree(sdev->in_streams[i].alsa_hw_desc);
        if (sdev->in_streams[i].buffers) {
            if (sdev->in_streams[i].host_allocated)
                dma_free_coherent(&sdev->a->pci->dev,
                        sdev->in_streams[i].buffers[0].size,
                        sdev->in_streams[i].buffers[0].ptr,
                        sdev->in_streams[i].buffers[0].dma_addr);
            kfree(sdev->in_streams[i].buffers);
        }
    }
    for (i = 0; i < sdev->out_stream_cnt; i++) {
        kfree(sdev->out_streams[i].alsa_hw_desc);
        kfree(sdev->out_streams[i].buffers);
    }
    kfree(sdev);
}

static struct aaudio_subdevice *aaudio_find_dev_by_dev_id(struct aaudio_device *a, aaudio_device_id_t dev_id)
{
    struct aaudio_subdevice *sdev;
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (dev_id == sdev->dev_id)
            return sdev;
    }
    return NULL;
}

static struct aaudio_subdevice *aaudio_find_dev_by_uid(struct aaudio_device *a, const char *uid)
{
    struct aaudio_subdevice *sdev;
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (!strcmp(uid, sdev->uid))
            return sdev;
    }
    return NULL;
}

static void aaudio_init_bs_stream(struct aaudio_device *a, struct aaudio_stream *strm,
        struct aaudio_buffer_struct_stream *bs_strm);
static void aaudio_init_bs_stream_host(struct aaudio_device *a, struct aaudio_stream *strm,
        struct aaudio_buffer_struct_stream *bs_strm);

static int aaudio_init_bs(struct aaudio_device *a)
{
    int i, j;
    struct aaudio_buffer_struct_device *dev;
    struct aaudio_subdevice *sdev;
    u32 ver, sig, bs_base;

    ver = ioread32(&a->reg_mem_gpr[0]);
    if (ver < 3) {
        dev_err(a->dev, "Bad GPR version (%u)\n", ver);
        return -EINVAL;
    }
    sig = ioread32(&a->reg_mem_gpr[1]);
    if (sig != AAUDIO_SIG) {
        dev_err(a->dev, "Bad GPR sig (%x)\n", sig);
        return -EINVAL;
    }
    bs_base = ioread32(&a->reg_mem_gpr[2]);
    a->bs = (__force struct aaudio_buffer_struct *) ((u8 __iomem *) a->reg_mem_bs + bs_base);
    if (a->bs->signature != AAUDIO_SIG) {
        dev_err(a->dev, "Bad BufferStruct sig (%x)\n", a->bs->signature);
        return -EINVAL;
    }
    dev_info(a->dev, "BufferStruct ver = %i\n", a->bs->version);
    dev_info(a->dev, "Num devices = %i\n", a->bs->num_devices);
    for (i = 0; i < a->bs->num_devices && i < ARRAY_SIZE(a->bs->devices); i++) {
        dev = &a->bs->devices[i];
        dev_info(a->dev, "Device %i %s\n", i, dev->name);

        sdev = aaudio_find_dev_by_uid(a, dev->name);
        if (!sdev) {
            dev_err(a->dev, "Subdevice not found for BufferStruct device %s\n", dev->name);
            continue;
        }
        sdev->buf_id = (u8) i;
        dev->num_input_streams = 0;
        for (j = 0; j < dev->num_output_streams; j++) {
            dev_info(a->dev, "Device %i Stream %i: Output; Buffer Count = %i\n", i, j,
                     dev->output_streams[j].num_buffers);
            if (j < sdev->out_stream_cnt)
                aaudio_init_bs_stream(a, &sdev->out_streams[j], &dev->output_streams[j]);
        }
    }

    list_for_each_entry(sdev, &a->subdevice_list, list) {
        if (sdev->buf_id != AAUDIO_BUFFER_ID_NONE)
            continue;
        if (i >= ARRAY_SIZE(a->bs->devices)) {
            dev_warn(a->dev, "Too many devices, skipping %s\n", sdev->uid);
            break;
        }
        sdev->buf_id = i;
        dev_info(a->dev, "Created device %i %s\n", i, sdev->uid);
        strscpy(a->bs->devices[i].name, sdev->uid, sizeof(a->bs->devices[i].name));
        a->bs->devices[i].num_input_streams = 0;
        a->bs->devices[i].num_output_streams = 0;
        a->bs->num_devices = ++i;
    }
    list_for_each_entry(sdev, &a->subdevice_list, list) {
        /* Subdevices that did not get a BufferStruct slot (e.g. when the
         * device array was full) must not index bs->devices with 0xff. */
        if (sdev->buf_id == AAUDIO_BUFFER_ID_NONE)
            continue;
        if (sdev->in_stream_cnt == 1) {
            dev_info(a->dev, "Device %i Host Stream; Input\n", sdev->buf_id);
            aaudio_init_bs_stream_host(a, &sdev->in_streams[0], &a->bs->devices[sdev->buf_id].input_streams[0]);
            a->bs->devices[sdev->buf_id].num_input_streams = 1;
            wmb();

            if (aaudio_cmd_set_input_stream_address_ranges(a, sdev->dev_id)) {
                dev_err(a->dev, "Failed to set input stream address ranges\n");
            }
        }
    }

    return 0;
}

static void aaudio_init_bs_stream(struct aaudio_device *a, struct aaudio_stream *strm,
                                  struct aaudio_buffer_struct_stream *bs_strm)
{
    size_t i;
    strm->buffer_cnt = bs_strm->num_buffers;
    if (bs_strm->num_buffers > AAUDIO_DEVICE_MAX_BUFFER_COUNT) {
        dev_warn(a->dev, "BufferStruct buffer count %u exceeds driver limit of %u\n", bs_strm->num_buffers,
                AAUDIO_DEVICE_MAX_BUFFER_COUNT);
        strm->buffer_cnt = AAUDIO_DEVICE_MAX_BUFFER_COUNT;
    }
    if (!strm->buffer_cnt)
        return;
    strm->buffers = kmalloc_array(strm->buffer_cnt, sizeof(struct aaudio_dma_buf), GFP_KERNEL);
    if (!strm->buffers) {
        dev_err(a->dev, "Buffer list allocation failed\n");
        return;
    }
    for (i = 0; i < strm->buffer_cnt; i++) {
        strm->buffers[i].dma_addr = a->reg_mem_bs_dma + (dma_addr_t) bs_strm->buffers[i].address;
        strm->buffers[i].ptr = a->reg_mem_bs + bs_strm->buffers[i].address;
        strm->buffers[i].size = bs_strm->buffers[i].size;
    }

    if (strm->buffer_cnt == 1) {
        strm->alsa_hw_desc = kmalloc(sizeof(struct snd_pcm_hardware), GFP_KERNEL);
        if (!strm->alsa_hw_desc ||
            aaudio_create_hw_info(&strm->desc, strm->alsa_hw_desc, strm->buffers[0].size)) {
            kfree(strm->alsa_hw_desc);
            strm->alsa_hw_desc = NULL;
        }
    }
}

static void aaudio_init_bs_stream_host(struct aaudio_device *a, struct aaudio_stream *strm,
        struct aaudio_buffer_struct_stream *bs_strm)
{
    size_t size;
    dma_addr_t dma_addr;
    void *dma_ptr;
    size = strm->desc.bytes_per_packet * 16640;
    dma_ptr = dma_alloc_coherent(&a->pci->dev, size, &dma_addr, GFP_KERNEL);
    if (!dma_ptr) {
        dev_err(a->dev, "dma_alloc_coherent failed\n");
        return;
    }
    bs_strm->buffers[0].address = dma_addr;
    bs_strm->buffers[0].size = size;
    bs_strm->num_buffers = 1;

    memset(dma_ptr, 0, size);

    strm->buffer_cnt = 1;
    strm->buffers = kmalloc_array(strm->buffer_cnt, sizeof(struct aaudio_dma_buf), GFP_KERNEL);
    if (!strm->buffers) {
        dev_err(a->dev, "Buffer list allocation failed\n");
        /* Untell the device about the buffer we are about to free */
        bs_strm->num_buffers = 0;
        bs_strm->buffers[0].address = 0;
        bs_strm->buffers[0].size = 0;
        strm->buffer_cnt = 0;
        dma_free_coherent(&a->pci->dev, size, dma_ptr, dma_addr);
        return;
    }
    strm->buffers[0].dma_addr = dma_addr;
    strm->buffers[0].ptr = dma_ptr;
    strm->buffers[0].size = size;
    strm->host_allocated = true;

    strm->alsa_hw_desc = kmalloc(sizeof(struct snd_pcm_hardware), GFP_KERNEL);
    if (!strm->alsa_hw_desc ||
        aaudio_create_hw_info(&strm->desc, strm->alsa_hw_desc, strm->buffers[0].size)) {
        kfree(strm->alsa_hw_desc);
        strm->alsa_hw_desc = NULL;
    }
}

static void aaudio_handle_prop_change(struct aaudio_device *a, struct aaudio_msg *msg);

void aaudio_handle_notification(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_send_ctx sctx;
    struct aaudio_msg_base base;
    if (aaudio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case AAUDIO_MSG_NOTIFICATION_BOOT:
            dev_info(a->dev, "Received boot notification from remote\n");

            /* Resend the alive notify */
            if (aaudio_send(a, &sctx, 500,
                    aaudio_msg_write_alive_notification, 1, 3)) {
                pr_err("Sending alive notification failed\n");
            }
            break;
        case AAUDIO_MSG_NOTIFICATION_ALIVE:
            dev_info(a->dev, "Received alive notification from remote\n");
            complete_all(&a->remote_alive);
            break;
        case AAUDIO_MSG_PROPERTY_CHANGED:
            aaudio_handle_prop_change(a, msg);
            break;
        default:
            dev_info(a->dev, "Unhandled notification %i", base.msg);
            break;
    }
}

struct aaudio_prop_change_work_struct {
    struct work_struct ws;
    struct aaudio_device *a;
    aaudio_device_id_t dev;
    aaudio_object_id_t obj;
    struct aaudio_prop_addr prop;
};

static void aaudio_handle_jack_connection_change(struct aaudio_subdevice *sdev)
{
    u32 plugged;
    if (!sdev->jack)
        return;
    /* NOTE: Apple made the plug status scoped to the input and output streams. This makes no sense for us, so I just
     * always pick the OUTPUT status. */
    if (aaudio_cmd_get_primitive_property(sdev->a, sdev->dev_id, sdev->dev_id,
            AAUDIO_PROP(AAUDIO_PROP_SCOPE_OUTPUT, AAUDIO_PROP_JACK_PLUGGED, 0), NULL, 0, &plugged, sizeof(plugged))) {
        dev_err(sdev->a->dev, "Failed to get jack enable status\n");
        return;
    }
    dev_dbg(sdev->a->dev, "Jack is now %s\n", plugged ? "plugged" : "unplugged");
    snd_jack_report(sdev->jack, plugged ? sdev->jack->type : 0);
}

void aaudio_handle_prop_change_work(struct work_struct *ws)
{
    struct aaudio_prop_change_work_struct *work = container_of(ws, struct aaudio_prop_change_work_struct, ws);
    struct aaudio_subdevice *sdev;

    sdev = aaudio_find_dev_by_dev_id(work->a, work->dev);
    if (!sdev) {
        dev_err(work->a->dev, "Property notification change: device not found\n");
        goto done;
    }
    dev_dbg(work->a->dev, "Property changed for device: %s\n", sdev->uid);

    if (work->prop.scope == AAUDIO_PROP_SCOPE_OUTPUT && work->prop.selector == AAUDIO_PROP_JACK_PLUGGED) {
        aaudio_handle_jack_connection_change(sdev);
    }

done:
    kfree(work);
}

void aaudio_handle_prop_change(struct aaudio_device *a, struct aaudio_msg *msg)
{
    /* NOTE: This is a scheduled work because this callback will generally need to query device information and this
     * is not possible when we are in the reply parsing code's context. */
    struct aaudio_prop_change_work_struct *work;
    work = kmalloc(sizeof(struct aaudio_prop_change_work_struct), GFP_KERNEL);
    if (!work)
        return;
    work->a = a;
    INIT_WORK(&work->ws, aaudio_handle_prop_change_work);
    if (aaudio_msg_read_property_changed(msg, &work->dev, &work->obj, &work->prop)) {
        dev_err(a->dev, "Failed to read property changed notification\n");
        kfree(work);
        return;
    }
    schedule_work(&work->ws);
}

/* Runs in the DMA irq thread (completion context). Timeout 0 makes the
 * submission reserve fail fast instead of sleeping up to 500ms — a full
 * out-queue means the T2 stopped consuming and blocking here would stall
 * completion processing for the entire driver. */
#define aaudio_send_cmd_response(a, sctx, msg, fn, ...) \
    do { \
        if (aaudio_send_with_tag(a, sctx, ((struct aaudio_msg_header *) msg->data)->tag, 0, fn, ##__VA_ARGS__)) \
            pr_err_ratelimited("Failed to reply to a command\n"); \
    } while (0)

static void aaudio_ts_work(struct work_struct *ws)
{
    struct aaudio_subdevice *sdev = container_of(ws, struct aaudio_subdevice, ts_work);
    ktime_t time_os;
    u64 dev_timestamp;

    spin_lock(&sdev->ts_lock);
    time_os = sdev->ts_time_os;
    dev_timestamp = sdev->ts_dev_timestamp;
    spin_unlock(&sdev->ts_lock);
    aaudio_handle_timestamp(sdev, time_os, dev_timestamp);
}

void aaudio_handle_cmd_timestamp(struct aaudio_device *a, struct aaudio_msg *msg)
{
    ktime_t time_os = ktime_get_boottime();
    struct aaudio_send_ctx sctx;
    struct aaudio_subdevice *sdev;
    u64 devid, timestamp, update_seed;
    aaudio_msg_read_update_timestamp(msg, &devid, &timestamp, &update_seed);
    dev_dbg(a->dev, "Received timestamp update for dev=%llx ts=%llx seed=%llx\n", devid, timestamp, update_seed);

    sdev = aaudio_find_dev_by_dev_id(a, devid);
    if (sdev) {
        spin_lock(&sdev->ts_lock);
        sdev->ts_time_os = time_os;
        sdev->ts_dev_timestamp = timestamp;
        spin_unlock(&sdev->ts_lock);
        schedule_work(&sdev->ts_work);
    }

    aaudio_send_cmd_response(a, &sctx, msg,
            aaudio_msg_write_update_timestamp_response);
}

void aaudio_handle_command(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_msg_base base;
    if (aaudio_msg_read_base(msg, &base))
        return;
    switch (base.msg) {
        case AAUDIO_MSG_UPDATE_TIMESTAMP:
            aaudio_handle_cmd_timestamp(a, msg);
            break;
        default:
            dev_info(a->dev, "Unhandled device command %i", base.msg);
            break;
    }
}

static const struct pci_device_id aaudio_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1803) },
        { 0, },
};

static SIMPLE_DEV_PM_OPS(aaudio_pci_driver_pm, aaudio_suspend, aaudio_resume);
static struct pci_driver aaudio_pci_driver = {
        .name = "aaudio",
        .id_table = aaudio_ids,
        .probe = aaudio_probe,
        .remove = aaudio_remove,
        .driver = {
                .pm = &aaudio_pci_driver_pm
        }
};


int aaudio_module_init(void)
{
    int result;
    aaudio_class = class_create("aaudio");
    if (IS_ERR(aaudio_class))
        return PTR_ERR(aaudio_class);

    if ((result = pci_register_driver(&aaudio_pci_driver))) {
        class_destroy(aaudio_class);
        return result;
    }
    return 0;
}

void aaudio_module_exit(void)
{
    pci_unregister_driver(&aaudio_pci_driver);
    class_destroy(aaudio_class);
}

struct aaudio_alsa_pcm_id_mapping aaudio_alsa_id_mappings[] = {
        {"Speaker", 0},
        {"Digital Mic", 1},
        {"Codec Output", 2},
        {"Codec Input", 3},
        {"Bridge Loopback", 4},
        {}
};

module_param_named(index, aaudio_alsa_index, int, 0444);
MODULE_PARM_DESC(index, "Index value for Apple Internal Audio soundcard.");
module_param_named(id, aaudio_alsa_id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Apple Internal Audio soundcard.");
