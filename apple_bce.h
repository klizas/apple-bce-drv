#ifndef APPLE_BCE_H
#define APPLE_BCE_H

#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/srcu.h>
#include "mailbox.h"
#include "queue.h"
#include "vhci/vhci.h"

#define BC_PROTOCOL_VERSION 0x20001
#define BCE_MAX_QUEUE_COUNT 0x100

#define BCE_QUEUE_USER_MIN 2
#define BCE_QUEUE_USER_MAX (BCE_MAX_QUEUE_COUNT - 1)

/* Per-irq-context state for DMA completion processing. The default context
 * (aux = false) serves MSI vector 4 and polls all CQs registered with
 * vector_or_cq == 0; the aux context is shared by every spare MSI vector and
 * polls the CQs registered with a nonzero vector field. lock serializes the
 * handler bodies sharing a context; sq_list is that context's private
 * dispatch scratch. */
struct bce_dma_irq_ctx {
    struct apple_bce_device *dev;
    bool aux;
    struct mutex lock;
    struct bce_queue_sq *sq_list[BCE_MAX_QUEUE_COUNT];
};

struct apple_bce_device {
    struct pci_dev *pci, *pci0;
    struct device_link *pci0_link;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_mb;
    void __iomem *reg_mem_dma;
    struct bce_mailbox mbox;
    struct bce_timestamp timestamp;
    struct bce_queue *queues[BCE_MAX_QUEUE_COUNT];
    struct list_head cq_list; /* live CQs, polled by the DMA interrupt */
    struct mutex queues_lock; /* serializes queue publish/unpublish (writers) */
    struct srcu_struct queues_srcu; /* protects readers (DMA irq) against queue teardown */
    struct ida queue_ida;
    struct bce_queue_cq *cmd_cq;
    struct bce_queue_cmdq *cmd_cmdq;
    struct bce_dma_irq_ctx dma_irq;
    struct bce_dma_irq_ctx dma_irq_aux;
    int nvec;
    bool is_being_removed;
    dma_addr_t saved_data_dma_addr;
    void *saved_data_dma_ptr;
    size_t saved_data_dma_size;

    struct bce_vhci vhci;
};

extern struct apple_bce_device *global_bce;

#endif //APPLE_BCE_H
