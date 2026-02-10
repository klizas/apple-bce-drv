#include "vhci.h"
#include "../apple_bce.h"
#include "command.h"
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/module.h>
#include <linux/version.h>

static dev_t bce_vhci_chrdev;
static struct class *bce_vhci_class;
static const struct hc_driver bce_vhci_driver;
static u16 bce_vhci_port_mask = U16_MAX;

static int bce_vhci_create_event_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci);
static int bce_vhci_create_message_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci);
static void bce_vhci_handle_firmware_events_w(struct work_struct *ws);
static void bce_vhci_firmware_event_completion(struct bce_queue_sq *sq);

int bce_vhci_create(struct apple_bce_device *dev, struct bce_vhci *vhci)
{
    int status;

    spin_lock_init(&vhci->hcd_spinlock);

    vhci->dev = dev;

    vhci->vdevt = bce_vhci_chrdev;
    vhci->vdev = device_create(bce_vhci_class, dev->dev, vhci->vdevt, NULL, "bce-vhci");
    if (IS_ERR_OR_NULL(vhci->vdev)) {
        status = PTR_ERR(vhci->vdev);
        goto fail_dev;
    }

    if ((status = bce_vhci_create_message_queues(vhci)))
        goto fail_mq;
    if ((status = bce_vhci_create_event_queues(vhci)))
        goto fail_eq;

    vhci->tq_state_wq = alloc_ordered_workqueue("bce-vhci-tq-state", 0);
    INIT_WORK(&vhci->w_fw_events, bce_vhci_handle_firmware_events_w);

    vhci->hcd = usb_create_hcd(&bce_vhci_driver, vhci->vdev, "bce-vhci");
    if (!vhci->hcd) {
        status = -ENOMEM;
        goto fail_hcd;
    }
    vhci->hcd->self.sysdev = &dev->pci->dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
    vhci->hcd->self.uses_dma = 1;
#endif
    *((struct bce_vhci **) vhci->hcd->hcd_priv) = vhci;
    vhci->hcd->speed = HCD_USB2;

    if ((status = usb_add_hcd(vhci->hcd, 0, 0)))
        goto fail_hcd;

    return 0;

fail_hcd:
    bce_vhci_destroy_event_queues(vhci);
fail_eq:
    bce_vhci_destroy_message_queues(vhci);
fail_mq:
    device_destroy(bce_vhci_class, vhci->vdevt);
fail_dev:
    if (!status)
        status = -EINVAL;
    return status;
}

void bce_vhci_destroy(struct bce_vhci *vhci)
{
    usb_remove_hcd(vhci->hcd);
    cancel_work_sync(&vhci->w_fw_events);
    flush_workqueue(vhci->tq_state_wq);
    destroy_workqueue(vhci->tq_state_wq);
    bce_vhci_destroy_event_queues(vhci);
    bce_vhci_destroy_message_queues(vhci);
    device_destroy(bce_vhci_class, vhci->vdevt);
}

struct bce_vhci *bce_vhci_from_hcd(struct usb_hcd *hcd)
{
    return *((struct bce_vhci **) hcd->hcd_priv);
}

int bce_vhci_start(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;
    u16 port_mask = 0;
    bce_vhci_port_t port_no = 0;
    if ((status = bce_vhci_cmd_controller_enable(&vhci->cq, 1, &port_mask)))
        return status;
    vhci->port_mask = port_mask;
    vhci->port_power_mask = 0;
    if ((status = bce_vhci_cmd_controller_start(&vhci->cq)))
        return status;
    port_mask = vhci->port_mask;
    while (port_mask) {
        port_no += 1;
        port_mask >>= 1;
    }
    vhci->port_count = port_no;
    return 0;
}

void bce_vhci_stop(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_cmd_controller_disable(&vhci->cq);
}

static int bce_vhci_hub_status_data(struct usb_hcd *hcd, char *buf)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    unsigned long mask = READ_ONCE(vhci->port_resume_mask);
    int i, ret_len;

    if (!mask)
        return 0;

    ret_len = (vhci->port_count + 8) / 8;
    memset(buf, 0, ret_len);
    for (i = 1; i <= vhci->port_count; i++) {
        if (test_bit(i, &vhci->port_resume_mask))
            buf[i / 8] |= BIT(i % 8);
    }
    return ret_len;
}

static int bce_vhci_reset_device(struct bce_vhci *vhci, int index, u16 timeout);

static int bce_vhci_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue, u16 wIndex, char *buf, u16 wLength)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;
    struct usb_hub_descriptor *hd;
    struct usb_hub_status *hs;
    struct usb_port_status *ps;
    u32 port_status;
    // pr_info("bce-vhci: bce_vhci_hub_control %x %i %i [bufl=%i]\n", typeReq, wValue, wIndex, wLength);
    if (typeReq == GetHubDescriptor && wLength >= sizeof(struct usb_hub_descriptor)) {
        hd = (struct usb_hub_descriptor *) buf;
        memset(hd, 0, sizeof(*hd));
        hd->bDescLength = sizeof(struct usb_hub_descriptor);
        hd->bDescriptorType = USB_DT_HUB;
        hd->bNbrPorts = (u8) vhci->port_count;
        hd->wHubCharacteristics = HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_INDV_PORT_OCPM;
        hd->bPwrOn2PwrGood = 0;
        hd->bHubContrCurrent = 0;
        return 0;
    } else if (typeReq == GetHubStatus && wLength >= sizeof(struct usb_hub_status)) {
        hs = (struct usb_hub_status *) buf;
        memset(hs, 0, sizeof(*hs));
        hs->wHubStatus = 0;
        hs->wHubChange = 0;
        return 0;
    } else if (typeReq == GetPortStatus && wLength >= 4 /* usb 2.0 */) {
        ps = (struct usb_port_status *) buf;
        ps->wPortStatus = 0;
        ps->wPortChange = 0;

        if (vhci->port_power_mask & BIT(wIndex))
            ps->wPortStatus |= USB_PORT_STAT_POWER;

        if (!(bce_vhci_port_mask & BIT(wIndex)))
            return 0;

        /* If port needs forced re-enumeration, hide the connection so USB
         * core sees the device as disconnected without querying T2.
         * Avoids hundreds of wasted T2 PCIe round-trips during PM resume
         * when USB core polls all ports repeatedly. */
        if (test_bit(wIndex, &vhci->port_reenumerate_mask)) {
            ps->wPortChange |= USB_PORT_STAT_C_CONNECTION;
            return 0;
        }

        if ((status = bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0, &port_status)))
            return status;

        if (port_status & 16)
            ps->wPortStatus |= USB_PORT_STAT_ENABLE | USB_PORT_STAT_HIGH_SPEED;
        if (port_status & 4)
            ps->wPortStatus |= USB_PORT_STAT_CONNECTION;
        if (port_status & 2)
            ps->wPortStatus |= USB_PORT_STAT_OVERCURRENT;
        if (port_status & 8)
            ps->wPortStatus |= USB_PORT_STAT_RESET;
        if (port_status & 0x60)
            ps->wPortStatus |= USB_PORT_STAT_SUSPEND;

        if (port_status & 0x40000)
            ps->wPortChange |= USB_PORT_STAT_C_CONNECTION;
        if (test_bit(wIndex, &vhci->port_resume_mask))
            ps->wPortChange |= USB_PORT_STAT_C_CONNECTION;

        pr_debug("bce-vhci: GetPortStatus port %d: raw=0x%x usb_status=0x%x usb_change=0x%x\n",
                wIndex, port_status, ps->wPortStatus, ps->wPortChange);
        return 0;
    } else if (typeReq == SetPortFeature) {
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_on(&vhci->cq, (u8) wIndex);
            /* As far as I am aware, power status is not part of the port status so store it separately */
            if (!status)
                vhci->port_power_mask |= BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_RESET) {
            return bce_vhci_reset_device(vhci, wIndex, wValue);
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            /* TODO: Am I supposed to also suspend the endpoints? */
            pr_debug("bce-vhci: Suspending port %i\n", wIndex);
            return bce_vhci_cmd_port_suspend(&vhci->cq, (u8) wIndex);
        }
    } else if (typeReq == ClearPortFeature) {
        if (wValue == USB_PORT_FEAT_ENABLE)
            return bce_vhci_cmd_port_disable(&vhci->cq, (u8) wIndex);
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_off(&vhci->cq, (u8) wIndex);
            if (!status)
                vhci->port_power_mask &= ~BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_C_CONNECTION) {
            clear_bit(wIndex, &vhci->port_resume_mask);
            return bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0x40000, &port_status);
        }
        if (wValue == USB_PORT_FEAT_C_RESET) { /* I don't think I can transfer it in any way */
            return 0;
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            pr_debug("bce-vhci: Resuming port %i\n", wIndex);
            return bce_vhci_cmd_port_resume(&vhci->cq, (u8) wIndex);
        }
    }
    pr_err("bce-vhci: bce_vhci_hub_control unhandled request: %x %i %i [bufl=%i]\n", typeReq, wValue, wIndex, wLength);
    dump_stack();
    return -EIO;
}

static int bce_vhci_enable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    struct bce_vhci_device *vdev;
    bce_vhci_device_t devid;
    pr_debug("bce_vhci_enable_device\n");

    if (vhci->port_to_device[udev->portnum])
        return 0;

    /* We need to early address the device */
    if (bce_vhci_cmd_device_create(&vhci->cq, udev->portnum, &devid))
        return -EIO;

    pr_debug("bce_vhci_cmd_device_create %i -> %i\n", udev->portnum, devid);

    vdev = kzalloc(sizeof(struct bce_vhci_device), GFP_KERNEL);
    vhci->port_to_device[udev->portnum] = devid;
    vhci->devices[devid] = vdev;

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[0], &udev->ep0, devid, DMA_BIDIRECTIONAL);
    udev->ep0.hcpriv = &vdev->tq[0];
    vdev->tq_mask |= BIT(0);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &udev->ep0.desc);
    return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0)
static int bce_vhci_address_device(struct usb_hcd *hcd, struct usb_device *udev)
#else
static int bce_vhci_address_device(struct usb_hcd *hcd, struct usb_device *udev, unsigned int timeout_ms) //TODO: follow timeout
#endif
{
    /* This is the same as enable_device, but instead in the old scheme */
    return bce_vhci_enable_device(hcd, udev);
}

static void bce_vhci_free_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int i;
    bce_vhci_device_t devid;
    struct bce_vhci_device *dev;
    pr_debug("bce_vhci_free_device %i\n", udev->portnum);
    if (!vhci->port_to_device[udev->portnum]) {
        /* Defensive: T2 device mapping already cleared.
         * If re-enumeration was requested, signal reconnect. */
        if (test_and_clear_bit(udev->portnum, &vhci->port_reenumerate_mask)) {
            pr_debug("bce_vhci_free_device: port %d (already cleared) signaling reconnect\n", udev->portnum);
            set_bit(udev->portnum, &vhci->port_resume_mask);
            usb_hcd_poll_rh_status(vhci->hcd);
        }
        return;
    }
    devid = vhci->port_to_device[udev->portnum];
    dev = vhci->devices[devid];

    /* Clear device mappings first to prevent concurrent access from
     * event handlers during teardown. */
    vhci->devices[devid] = NULL;
    vhci->port_to_device[udev->portnum] = 0;

    /* Host-side cleanup only: clear hcpriv, destroy transfer queues.
     * Skip per-endpoint T2 pause+destroy commands -- device_destroy
     * handles T2-side cleanup implicitly (same pattern as
     * bce_vhci_reset_device). USB core has already dequeued all URBs
     * before free_dev, so no DMA is in flight. */
    for (i = 0; i < 32; i++) {
        if (dev->tq_mask & BIT(i)) {
            if (dev->tq[i].endp)
                dev->tq[i].endp->hcpriv = NULL;
            bce_vhci_destroy_transfer_queue(vhci, &dev->tq[i]);
        }
    }
    dev->tq_mask = 0;
    /* Skip device_destroy when reenumerate_mask is set — bus_resume
     * already handled T2-side teardown (either T2 reconnected and
     * destroyed the old device itself, or we called device_destroy
     * explicitly to force re-enumeration). */
    if (!test_bit(udev->portnum, &vhci->port_reenumerate_mask))
        bce_vhci_cmd_device_destroy(&vhci->cq, devid);
    kfree(dev);

    /* If this port was marked for forced re-enumeration, the device is now
     * torn down. Clear the flag so the next GetPortStatus shows the real
     * connection, and set port_resume_mask so hub_event picks up the
     * "new" connection and re-enumerates. */
    if (test_and_clear_bit(udev->portnum, &vhci->port_reenumerate_mask)) {
        pr_info("bce_vhci_free_device: port %d cleared reenumerate, signaling reconnect\n", udev->portnum);
        set_bit(udev->portnum, &vhci->port_resume_mask);
        usb_hcd_poll_rh_status(vhci->hcd);
    }
}

static int bce_vhci_reset_device(struct bce_vhci *vhci, int index, u16 timeout)
{
    struct bce_vhci_device *dev = NULL;
    bce_vhci_device_t devid;
    int i;
    int status;
    enum dma_data_direction dir;

    /* After bus_resume refresh, skip reset_device calls from USB core's
     * reset_resume path — the T2 device was already refreshed in bus_resume.
     * USB core calls reset_device twice (hub_port_reset + hub_port_init),
     * so we skip both. The port is already enabled from the refresh. */
    if (vhci->port_resume_skip_reset[index] > 0) {
        pr_info("bce_vhci_reset_device: port %d skipped (resume, remaining=%d)\n",
                index, vhci->port_resume_skip_reset[index] - 1);
        vhci->port_resume_skip_reset[index]--;
        return 0;
    }

    devid = vhci->port_to_device[index];
    if (devid) {
        dev = vhci->devices[devid];
        pr_info("bce_vhci_reset_device: port %d devid=%d tq_mask=0x%x\n",
                index, devid, dev ? dev->tq_mask : 0);

        for (i = 0; i < 32; i++) {
            if (!(dev->tq_mask & BIT(i)))
                continue;
            /*
             * Suspend/resume fix: Clear hcpriv BEFORE destroying the queue
             * to prevent use-after-free if URB operations occur during reset.
             */
            if (dev->tq[i].endp)
                dev->tq[i].endp->hcpriv = NULL;
            bce_vhci_destroy_transfer_queue(vhci, &dev->tq[i]);
        }
        vhci->devices[devid] = NULL;
        vhci->port_to_device[index] = 0;
        /* T2 implicitly destroys endpoints with device_destroy */
        bce_vhci_cmd_device_destroy(&vhci->cq, devid);
    } else {
        pr_info("bce_vhci_reset_device: port %d (no device)\n", index);
    }
    status = bce_vhci_cmd_port_reset(&vhci->cq, (u8) index, timeout);

    if (dev) {
        if ((status = bce_vhci_cmd_device_create(&vhci->cq, index, &devid)))
            return status;
        vhci->devices[devid] = dev;
        vhci->port_to_device[index] = devid;

        for (i = 0; i < 32; i++) {
            if (dev->tq_mask & BIT(i)) {
                dir = usb_endpoint_dir_in(&dev->tq[i].endp->desc) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
                if (i == 0)
                    dir = DMA_BIDIRECTIONAL;
                bce_vhci_create_transfer_queue(vhci, &dev->tq[i], dev->tq[i].endp, devid, dir);
                /* Restore hcpriv after recreating the queue */
                dev->tq[i].endp->hcpriv = &dev->tq[i];
                bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &dev->tq[i].endp->desc);
            }
        }
    }

    return status;
}

static int bce_vhci_check_bandwidth(struct usb_hcd *hcd, struct usb_device *udev)
{
    return 0;
}

static int bce_vhci_get_frame_number(struct usb_hcd *hcd)
{
    return 0;
}

static int bce_vhci_bus_suspend(struct usb_hcd *hcd)
{
    int i, j;
    int status;
    unsigned long flags;
    struct bce_vhci_transfer_queue *tq;
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    pr_info("bce_vhci: suspend started\n");
    pr_info("bce_vhci: suspend: msg queue slots: cmd=%d/%d sys=%d/%d async=%d/%d int=%d/%d iso=%d/%d\n",
            atomic_read(&vhci->msg_commands.sq->available_commands), vhci->msg_commands.sq->el_count - 1,
            atomic_read(&vhci->msg_system.sq->available_commands), vhci->msg_system.sq->el_count - 1,
            atomic_read(&vhci->msg_asynchronous.sq->available_commands), vhci->msg_asynchronous.sq->el_count - 1,
            atomic_read(&vhci->msg_interrupt.sq->available_commands), vhci->msg_interrupt.sq->el_count - 1,
            atomic_read(&vhci->msg_isochronous.sq->available_commands), vhci->msg_isochronous.sq->el_count - 1);
    memset(vhci->port_resume_skip_reset, 0, sizeof(vhci->port_resume_skip_reset));
    WRITE_ONCE(vhci->port_resume_mask, 0);
    /* Clear any leftover re-enumeration state from a previous resume cycle
     * that didn't fully complete before this suspend. Stale bits would
     * confuse the next resume cycle. */
    WRITE_ONCE(vhci->port_reenumerate_mask, 0);

    /* Pause endpoints on T2 BEFORE flushing the workqueue. The lightweight
     * pause sets paused_by |= SUSPEND and drains pending_pause_count,
     * which short-circuits any pending w_pause workers — they'll see the
     * counter at zero and skip. Without this ordering, flush_workqueue
     * blocks on N × do_pause() (3 T2 round-trips per endpoint), causing
     * progressive suspend slowdown. */
    for (i = 0; i < 16; i++) {
        struct bce_vhci_device *vdev;
        if (!vhci->port_to_device[i])
            continue;
        vdev = vhci->devices[vhci->port_to_device[i]];
        for (j = 0; j < 32; j++) {
            if (!(vdev->tq_mask & BIT(j)))
                continue;
            tq = &vdev->tq[j];
            mutex_lock(&tq->pause_lock);
            if (!tq->paused_by) {
                spin_lock_irqsave(&tq->urb_lock, flags);
                tq->active = false;
                spin_unlock_irqrestore(&tq->urb_lock, flags);
                status = bce_vhci_cmd_endpoint_set_state(
                        &vhci->cq, tq->dev_addr, tq->endp_addr,
                        BCE_VHCI_ENDPOINT_PAUSED, &tq->state);
                if (status)
                    pr_warn("bce_vhci: suspend: endpoint_set_state failed %d:%d (err=%d)\n",
                            i, j, status);
            }
            tq->paused_by |= BCE_VHCI_PAUSE_SUSPEND;
            atomic_set(&tq->pending_pause_count, 0);
            mutex_unlock(&tq->pause_lock);
        }
    }

    flush_workqueue(vhci->tq_state_wq);

    pr_debug("bce_vhci: suspend: suspending ports\n");
    for (i = 0; i < 16; i++) {
        if (!vhci->port_to_device[i])
            continue;
        bce_vhci_cmd_port_suspend(&vhci->cq, i);
    }

    pr_debug("bce_vhci: suspend: pausing controller\n");
    if ((status = bce_vhci_cmd_controller_pause(&vhci->cq))) {
        pr_err("bce_vhci: suspend: controller_pause failed (err=%d)\n", status);
        return status;
    }

    /* Set PORT_CONNECT suppress mask BEFORE pausing event queues, so it's
     * in RAM through S3. When PCIe restores before bus_resume runs, T2 may
     * deliver PORT_CONNECT events via interrupt — the mask must already be
     * set or those events will trigger USB core re-enumeration. */
    {
        unsigned long suppress = 0;
        for (i = 0; i < 16; i++) {
            if (vhci->port_to_device[i])
                suppress |= BIT(i);
        }
        WRITE_ONCE(vhci->port_suppress_connect_mask, suppress);
    }

    pr_debug("bce_vhci: suspend: pausing event queues\n");
    bce_vhci_event_queue_pause(&vhci->ev_commands);
    bce_vhci_event_queue_pause(&vhci->ev_system);
    bce_vhci_event_queue_pause(&vhci->ev_isochronous);
    bce_vhci_event_queue_pause(&vhci->ev_interrupt);
    bce_vhci_event_queue_pause(&vhci->ev_asynchronous);
    pr_info("bce_vhci: suspend done\n");
    return 0;
}

static int bce_vhci_bus_resume(struct usb_hcd *hcd)
{
    static unsigned int resume_cycle;
    int i, j;
    int status;
    int need_poll = 0;
    u32 port_status;
    struct bce_vhci_device *vdev;
    bce_vhci_device_t devid, new_devid;
    struct usb_host_endpoint *ep0_endp;
    struct usb_device *udev;
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    ++resume_cycle;
    pr_info("bce_vhci: resume started (cycle %u)\n", resume_cycle);

    pr_debug("bce_vhci: resume: resuming event queues\n");
    bce_vhci_event_queue_resume(&vhci->ev_commands);
    bce_vhci_event_queue_resume(&vhci->ev_system);
    bce_vhci_event_queue_resume(&vhci->ev_asynchronous);
    bce_vhci_event_queue_resume(&vhci->ev_isochronous);
    bce_vhci_event_queue_resume(&vhci->ev_interrupt);

    pr_debug("bce_vhci: resume: starting controller\n");
    if ((status = bce_vhci_cmd_controller_start(&vhci->cq))) {
        pr_err("bce_vhci: resume: controller_start failed (err=%d)\n", status);
        WRITE_ONCE(vhci->port_suppress_connect_mask, 0);
        WRITE_ONCE(vhci->port_reenumerate_mask, 0);
        return status;
    }

    /* Flush stale DMA transfer submissions from before S3. Only
     * msg_asynchronous carries DMA transfers; the other queues
     * (commands, system, interrupt, isochronous) were proven to have
     * no slot leaks across suspend/resume. */
    bce_cmd_flush_memory_queue(vhci->dev->cmd_cmdq, (u16) vhci->msg_asynchronous.sq->qid);

    pr_debug("bce_vhci: resume: resuming ports\n");
    for (i = 0; i < 16; i++) {
        if (!vhci->port_to_device[i])
            continue;
        bce_vhci_cmd_port_resume(&vhci->cq, i);
    }

    /* Per-port classification: silent refresh + reset_resume for surviving
     * devices, re-enumeration for reconnected/error ports. */
    for (i = 0; i < 16; i++) {
        if (!vhci->port_to_device[i])
            continue;

        devid = vhci->port_to_device[i];
        vdev = vhci->devices[devid];

        bool connection_changed = false;
        status = bce_vhci_cmd_port_status(&vhci->cq, (u8) i, 0, &port_status);
        if (!status && (port_status & 0x40000)) {
            /* T2 reconnected — old device already destroyed on T2 side.
             * Clear the change bit and fall through to the refresh path
             * (same as surviving devices, but skip device_destroy). */
            pr_info("bce_vhci: resume: port %d connection changed, refreshing\n", i);
            bce_vhci_cmd_port_status(&vhci->cq, (u8) i, 0x40000, &port_status);
            connection_changed = true;
        }
        if (status || !(port_status & 0x4)) {
            /* Error querying port or device disconnected */
            pr_info("bce_vhci: resume: port %d error/disconnected (status=%d, port_status=0x%x)\n",
                    i, status, port_status);
            bce_vhci_cmd_device_destroy(&vhci->cq, devid);
            goto reenumerate;
        }

        /* Inline refresh for surviving devices (not reconnected).
         * Creating only EP0 matches boot-time state and prevents T2
         * from stalling on GET_DESCRIPTOR. USB core's reset_resume
         * sends SET_CONFIGURATION, then add_endpoint recreates
         * non-EP0 endpoints. */
        pr_info("bce_vhci: resume: port %d refreshing (devid=%d, tq_mask=0x%x)\n",
                i, devid, vdev->tq_mask);

        /* Save EP0 endpoint pointer before destroying queues */
        ep0_endp = vdev->tq[0].endp;

        /* Cancel all in-flight URBs and destroy ALL transfer queues */
        for (j = 0; j < 32; j++) {
            if (!(vdev->tq_mask & BIT(j)))
                continue;
            bce_vhci_transfer_queue_cancel_all(&vdev->tq[j]);
            if (vdev->tq[j].endp)
                vdev->tq[j].endp->hcpriv = NULL;
            bce_vhci_destroy_transfer_queue(vhci, &vdev->tq[j]);
        }
        vdev->tq_mask = 0;

        /* Clear mappings before T2 commands */
        vhci->devices[devid] = NULL;
        vhci->port_to_device[i] = 0;

        /* Destroy and recreate T2 device — port_reset required by T2
         * before device_create will succeed.
         * Skip destroy for reconnected devices: T2 already destroyed them. */
        if (!connection_changed) {
            status = bce_vhci_cmd_device_destroy(&vhci->cq, devid);
            if (status) {
                pr_err("bce_vhci: resume: port %d device_destroy failed (err=%d), falling back to re-enum\n", i, status);
                kfree(vdev);
                goto reenumerate;
            }
        }
        status = bce_vhci_cmd_port_reset(&vhci->cq, (u8) i, 0);
        if (status) {
            pr_err("bce_vhci: resume: port %d port_reset failed (err=%d), falling back to re-enum\n", i, status);
            kfree(vdev);
            goto reenumerate;
        }
        if (bce_vhci_cmd_device_create(&vhci->cq, i, &new_devid)) {
            pr_err("bce_vhci: resume: port %d device_create failed, falling back to re-enum\n", i);
            kfree(vdev);
            goto reenumerate;
        }

        /* Update mappings with new device ID */
        vhci->devices[new_devid] = vdev;
        vhci->port_to_device[i] = new_devid;

        /* Create ONLY EP0 — matches boot-time device state.
         * Non-EP0 endpoints will be recreated by add_endpoint
         * when USB core restores configuration during reset_resume. */
        bce_vhci_create_transfer_queue(vhci, &vdev->tq[0], ep0_endp, new_devid, DMA_BIDIRECTIONAL);
        ep0_endp->hcpriv = &vdev->tq[0];
        vdev->tq_mask = BIT(0);
        bce_vhci_cmd_endpoint_create(&vhci->cq, new_devid, &ep0_endp->desc);

        /* Tell USB core to do reset_resume: sends SET_CONFIGURATION
         * so devices are properly configured after port_reset.
         * Skip the 2 reset_device calls USB core will make —
         * T2 was already refreshed above. */
        vhci->port_resume_skip_reset[i] = 2;
        udev = usb_hub_find_child(hcd->self.root_hub, i);
        if (udev)
            udev->reset_resume = 1;

        pr_info("bce_vhci: resume: port %d refresh done (new devid=%d, EP0 only)\n", i, new_devid);
        continue;

    reenumerate:
        /* Leave port_to_device/devices intact for free_device cleanup.
         * free_device will skip device_destroy (reenumerate_mask set). */
        set_bit(i, &vhci->port_reenumerate_mask);
        set_bit(i, &vhci->port_resume_mask);
        need_poll = 1;
    }

    WRITE_ONCE(vhci->port_suppress_connect_mask, 0);

    if (need_poll)
        usb_hcd_poll_rh_status(vhci->hcd);

    pr_info("bce_vhci: resume done (cycle %u)\n", resume_cycle);
    return 0;
}

static int bce_vhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
{
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    if (!q)
        return -ENOENT;

    pr_debug("bce_vhci_urb_enqueue %i:%x\n", q->dev_addr, urb->ep->desc.bEndpointAddress);
    return bce_vhci_urb_create(q, urb, mem_flags);
}

static int bce_vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    if (!q)
        return -ENOENT;
    pr_debug("bce_vhci_urb_dequeue %x\n", urb->ep->desc.bEndpointAddress);
    return bce_vhci_urb_request_cancel(q, urb, status);
}

static void bce_vhci_endpoint_reset(struct usb_hcd *hcd, struct usb_host_endpoint *ep)
{
    struct bce_vhci_transfer_queue *q = ep->hcpriv;
    pr_debug("bce_vhci_endpoint_reset\n");
    if (q)
        bce_vhci_transfer_queue_request_reset(q);
}

static u8 bce_vhci_endpoint_index(u8 addr)
{
    if (addr & 0x80)
        return (u8) (0x10 + (addr & 0xf));
    return (u8) (addr & 0xf);
}

static int bce_vhci_add_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_device *vdev = vhci->devices[devid];
    pr_debug("bce_vhci_add_endpoint %x/%x:%x\n", udev->portnum, devid, endp_index);
    if (udev->bus->root_hub == udev) /* The USB hub */
        return 0;
    if (vdev == NULL)
        return -ENODEV;
    if (vdev->tq_mask & BIT(endp_index)) {
        endp->hcpriv = &vdev->tq[endp_index];
        return 0;
    }

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[endp_index], endp, devid,
            usb_endpoint_dir_in(&endp->desc) ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
    endp->hcpriv = &vdev->tq[endp_index];
    vdev->tq_mask |= BIT(endp_index);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &endp->desc);
    return 0;
}

static int bce_vhci_drop_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_transfer_queue *q = endp->hcpriv;
    struct bce_vhci_device *vdev;
    pr_debug("bce_vhci_drop_endpoint %x:%x\n", udev->portnum, endp_index);

    /*
     * Suspend/resume fix: Device may have been freed during reset.
     * Check validity before accessing device structures.
     */
    if (!devid || !vhci->devices[devid]) {
        endp->hcpriv = NULL;
        return 0;
    }
    vdev = vhci->devices[devid];

    if (!q) {
        if (vdev->tq_mask & BIT(endp_index)) {
            pr_err("something deleted the hcpriv?\n");
            q = &vdev->tq[endp_index];
        } else {
            return 0;
        }
    }

    /* During device disconnect, skip T2 endpoint_destroy command.
     * device_destroy in free_device handles T2-side cleanup implicitly
     * (same pattern as bce_vhci_reset_device). Avoids one T2 round-trip
     * per endpoint on the shared command queue mutex. */
    if (udev->state != USB_STATE_NOTATTACHED)
        bce_vhci_cmd_endpoint_destroy(&vhci->cq, devid, (u8) (endp->desc.bEndpointAddress & 0x8Fu));
    vdev->tq_mask &= ~BIT(endp_index);
    bce_vhci_destroy_transfer_queue(vhci, q);
    endp->hcpriv = NULL;
    return 0;
}

static int bce_vhci_create_message_queues(struct bce_vhci *vhci)
{
    if (bce_vhci_message_queue_create(vhci, &vhci->msg_commands, "VHC1HostCommands") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_system, "VHC1HostSystemEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_isochronous, "VHC1HostIsochronousEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_interrupt, "VHC1HostInterruptEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_asynchronous, "VHC1HostAsynchronousEvents")) {
        bce_vhci_destroy_message_queues(vhci);
        return -EINVAL;
    }
    spin_lock_init(&vhci->msg_asynchronous_lock);
    bce_vhci_command_queue_create(&vhci->cq, &vhci->msg_commands);
    return 0;
}

static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci)
{
    bce_vhci_command_queue_destroy(&vhci->cq);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_commands);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_system);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_isochronous);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_interrupt);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_asynchronous);
}

static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);
static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);

static int bce_vhci_create_event_queues(struct bce_vhci *vhci)
{
    vhci->ev_cq = bce_create_cq(vhci->dev, 0x100);
    if (!vhci->ev_cq)
        return -EINVAL;
#define CREATE_EVENT_QUEUE(field, name, cb) bce_vhci_event_queue_create(vhci, &vhci->field, name, cb)
    if (__bce_vhci_event_queue_create(vhci, &vhci->ev_commands, "VHC1FirmwareCommands",
            bce_vhci_firmware_event_completion) ||
        CREATE_EVENT_QUEUE(ev_system,       "VHC1FirmwareSystemEvents",       bce_vhci_handle_system_event) ||
        CREATE_EVENT_QUEUE(ev_isochronous,  "VHC1FirmwareIsochronousEvents",  bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_interrupt,    "VHC1FirmwareInterruptEvents",    bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_asynchronous, "VHC1FirmwareAsynchronousEvents", bce_vhci_handle_usb_event)) {
        bce_vhci_destroy_event_queues(vhci);
        return -EINVAL;
    }
#undef CREATE_EVENT_QUEUE
    return 0;
}

static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci)
{
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_commands);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_system);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_isochronous);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_interrupt);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_asynchronous);
    if (vhci->ev_cq)
        bce_destroy_cq(vhci->dev, vhci->ev_cq);
}

static void bce_vhci_send_fw_event_response(struct bce_vhci *vhci, struct bce_vhci_message *req, u16 status)
{
    unsigned long timeout = 1000;
    struct bce_vhci_message r = *req;
    r.cmd = (u16) (req->cmd | 0x8000u);
    r.status = status;
    r.param1 = req->param1;
    r.param2 = 0;

    if (bce_reserve_submission(vhci->msg_system.sq, &timeout)) {
        pr_err("bce-vhci: Cannot reserve submision for FW event reply\n");
        return;
    }
    bce_vhci_message_queue_write(&vhci->msg_system, &r);
}

static int bce_vhci_handle_firmware_event(struct bce_vhci *vhci, struct bce_vhci_message *msg)
{
    unsigned long flags;
    bce_vhci_device_t devid;
    u8 endp;
    struct bce_vhci_device *dev;
    struct bce_vhci_transfer_queue *tq;
    if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_REQUEST_STATE || msg->cmd == BCE_VHCI_CMD_ENDPOINT_SET_STATE) {
        devid = (bce_vhci_device_t) (msg->param1 & 0xff);
        endp = bce_vhci_endpoint_index((u8) ((msg->param1 >> 8) & 0xff));
        dev = vhci->devices[devid];
        if (!dev || !(dev->tq_mask & BIT(endp)))
            return BCE_VHCI_BAD_ARGUMENT;
        tq = &dev->tq[endp];
    }

    if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_REQUEST_STATE) {
        if (msg->param2 == BCE_VHCI_ENDPOINT_ACTIVE) {
            bce_vhci_transfer_queue_resume(tq, BCE_VHCI_PAUSE_FIRMWARE);
            return BCE_VHCI_SUCCESS;
        } else if (msg->param2 == BCE_VHCI_ENDPOINT_PAUSED) {
            bce_vhci_transfer_queue_pause(tq, BCE_VHCI_PAUSE_FIRMWARE);
            return BCE_VHCI_SUCCESS;
        }
        return BCE_VHCI_BAD_ARGUMENT;
    } else if (msg->cmd == BCE_VHCI_CMD_ENDPOINT_SET_STATE) {
        if (msg->param2 == BCE_VHCI_ENDPOINT_STALLED) {
            tq->state = msg->param2;
            spin_lock_irqsave(&tq->urb_lock, flags);
            tq->stalled = true;
            spin_unlock_irqrestore(&tq->urb_lock, flags);
            return BCE_VHCI_SUCCESS;
        }
        return BCE_VHCI_BAD_ARGUMENT;
    }
    pr_warn("bce-vhci: Unhandled firmware event: %x s=%x p1=%x p2=%llx\n",
            msg->cmd, msg->status, msg->param1, msg->param2);
    return BCE_VHCI_BAD_ARGUMENT;
}

static void bce_vhci_handle_firmware_events_w(struct work_struct *ws)
{
    size_t cnt = 0;
    int result;
    struct bce_vhci *vhci = container_of(ws, struct bce_vhci, w_fw_events);
    struct bce_queue_sq *sq = vhci->ev_commands.sq;
    struct bce_sq_completion_data *cq;
    struct bce_vhci_message *msg, *msg2 = NULL;

    while (true) {
        if (msg2) {
            msg = msg2;
            msg2 = NULL;
        } else if ((cq = bce_next_completion(sq))) {
            if (cq->status == BCE_COMPLETION_ABORTED) {
                bce_notify_submission_complete(sq);
                continue;
            }
            msg = &vhci->ev_commands.data[sq->head];
        } else {
            break;
        }

        pr_debug("bce-vhci: Got fw event: %x s=%x p1=%x p2=%llx\n", msg->cmd, msg->status, msg->param1, msg->param2);
        if ((cq = bce_next_completion(sq))) {
            msg2 = &vhci->ev_commands.data[(sq->head + 1) % sq->el_count];
            pr_debug("bce-vhci: Got second fw event: %x s=%x p1=%x p2=%llx\n",
                    msg->cmd, msg->status, msg->param1, msg->param2);
            if (cq->status != BCE_COMPLETION_ABORTED &&
                msg2->cmd == (msg->cmd | 0x4000) && msg2->param1 == msg->param1) {
                /* Take two elements */
                pr_debug("bce-vhci: Cancelled\n");
                bce_vhci_send_fw_event_response(vhci, msg, BCE_VHCI_ABORT);

                bce_notify_submission_complete(sq);
                bce_notify_submission_complete(sq);
                msg2 = NULL;
                cnt += 2;
                continue;
            }

            pr_warn("bce-vhci: Handle fw event - unexpected cancellation\n");
        }

        result = bce_vhci_handle_firmware_event(vhci, msg);
        bce_vhci_send_fw_event_response(vhci, msg, (u16) result);


        bce_notify_submission_complete(sq);
        ++cnt;
    }
    bce_vhci_event_queue_submit_pending(&vhci->ev_commands, cnt);
    if (atomic_read(&sq->available_commands) == sq->el_count - 1) {
        pr_debug("bce-vhci: complete\n");
        complete(&vhci->ev_commands.queue_empty_completion);
    }
}

static void bce_vhci_firmware_event_completion(struct bce_queue_sq *sq)
{
    struct bce_vhci_event_queue *q = sq->userdata;
    queue_work(q->vhci->tq_state_wq, &q->vhci->w_fw_events);
}

static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    struct usb_hcd *hcd = q->vhci->hcd;
    u8 port;

    if (msg->cmd & 0x8000) {
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
        return;
    }

    switch (msg->cmd) {
    case BCE_VHCI_CMD_PORT_CONNECT:
        /*
         * T2 notifies us that a port connection state changed.
         * This happens during boot and resume. Mark the port as changed
         * so hub_status_data reports it, then notify USB core to poll.
         *
         * During session refresh (resume), suppress these events for
         * ports being refreshed — we don't want USB core to see a
         * connection change and trigger re-enumeration.
         */
        port = (u8)(msg->param1 & 0xff);
        if (test_bit(port, &q->vhci->port_suppress_connect_mask)) {
            pr_debug("bce-vhci: port %d connect notification suppressed (session refresh)\n", port);
            break;
        }
        pr_info("bce-vhci: port %d connect notification (status=0x%llx)\n", port, msg->param2);
        set_bit(port, &q->vhci->port_resume_mask);
        if (hcd)
            usb_hcd_poll_rh_status(hcd);
        break;

    case BCE_VHCI_CMD_PORT_RESUME:
        port = (u8)(msg->param1 & 0xff);
        pr_info("bce-vhci: T2 initiated port %d resume (status=0x%llx)\n", port, msg->param2);
        if (hcd)
            usb_hcd_poll_rh_status(hcd);
        break;

    case BCE_VHCI_CMD_PORT_SUSPEND:
        port = (u8)(msg->param1 & 0xff);
        pr_info("bce-vhci: T2 initiated port %d suspend (status=0x%llx)\n", port, msg->param2);
        break;

    default:
        pr_warn("bce-vhci: Unhandled system event: %x s=%x p1=%x p2=%llx\n",
                msg->cmd, msg->status, msg->param1, msg->param2);
        break;
    }
}

static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    bce_vhci_device_t devid;
    u8 endp;
    struct bce_vhci_device *dev;
    if (msg->cmd & 0x8000) {
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
    } else if (msg->cmd == BCE_VHCI_CMD_TRANSFER_REQUEST || msg->cmd == BCE_VHCI_CMD_CONTROL_TRANSFER_STATUS) {
        devid = (bce_vhci_device_t) (msg->param1 & 0xff);
        endp = bce_vhci_endpoint_index((u8) ((msg->param1 >> 8) & 0xff));
        dev = q->vhci->devices[devid];
        if (!dev || (dev->tq_mask & BIT(endp)) == 0) {
            pr_err("bce-vhci: Didn't find destination for transfer queue event\n");
            return;
        }
        bce_vhci_transfer_queue_event(&dev->tq[endp], msg);
    } else {
        pr_warn("bce-vhci: Unhandled USB event: %x s=%x p1=%x p2=%llx\n",
                msg->cmd, msg->status, msg->param1, msg->param2);
    }
}



static const struct hc_driver bce_vhci_driver = {
        .description = "bce-vhci",
        .product_desc = "BCE VHCI Host Controller",
        .hcd_priv_size = sizeof(struct bce_vhci *),

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,4,0)
        .flags = HCD_USB2,
#else
        .flags = HCD_USB2 | HCD_DMA,
#endif

        .start = bce_vhci_start,
        .stop = bce_vhci_stop,
        .hub_status_data = bce_vhci_hub_status_data,
        .hub_control = bce_vhci_hub_control,
        .urb_enqueue = bce_vhci_urb_enqueue,
        .urb_dequeue = bce_vhci_urb_dequeue,
        .enable_device = bce_vhci_enable_device,
        .free_dev = bce_vhci_free_device,
        .address_device = bce_vhci_address_device,
        .add_endpoint = bce_vhci_add_endpoint,
        .drop_endpoint = bce_vhci_drop_endpoint,
        .endpoint_reset = bce_vhci_endpoint_reset,
        .check_bandwidth = bce_vhci_check_bandwidth,
        .get_frame_number = bce_vhci_get_frame_number,
        .bus_suspend = bce_vhci_bus_suspend,
        .bus_resume = bce_vhci_bus_resume
};


int __init bce_vhci_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&bce_vhci_chrdev, 0, 1, "bce-vhci")))
        goto fail_chrdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,4,0)
    bce_vhci_class = class_create(THIS_MODULE, "bce-vhci");
#else
    bce_vhci_class = class_create("bce-vhci");
#endif
    if (IS_ERR(bce_vhci_class)) {
        result = PTR_ERR(bce_vhci_class);
        goto fail_class;
    }
    return 0;

fail_class:
    class_destroy(bce_vhci_class);
fail_chrdev:
    unregister_chrdev_region(bce_vhci_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}
void __exit bce_vhci_module_exit(void)
{
    class_destroy(bce_vhci_class);
    unregister_chrdev_region(bce_vhci_chrdev, 1);
}

module_param_named(vhci_port_mask, bce_vhci_port_mask, ushort, 0444);
MODULE_PARM_DESC(vhci_port_mask, "Specifies which VHCI ports are enabled");
