#define pr_fmt(fmt) "apple-ave: " fmt

#include "protocol.h"
#include "../apple_bce.h"

#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/string.h>

static void ave_q0_completion(struct bce_queue_sq *sq);
static void ave_q1_completion(struct bce_queue_sq *sq);
static void ave_q2_completion(struct bce_queue_sq *sq);
static void ave_q3_completion(struct bce_queue_sq *sq);

/*
 * Ring helpers. tail is only advanced by the (serialized) submitting context,
 * head only by the completion IRQ; both are free-running counters.
 */
static inline void *ave_ring_tail_data(struct ave_queue_buf *qb)
{
	return qb->data[qb->tail % qb->el_count];
}

static inline dma_addr_t ave_ring_tail_dma(struct ave_queue_buf *qb)
{
	return qb->dma_addrs[qb->tail % qb->el_count];
}

static inline bool ave_ring_full(struct ave_queue_buf *qb)
{
	return qb->tail - READ_ONCE(qb->head) >= qb->el_count;
}

/* Fill a receive buffer's inspected prefix with the 0xFF sentinel observed
 * in macOS traces (see AVE_SENTINEL_FILL_SIZE for the trade-off). */
static inline void ave_fill_sentinel(struct ave_queue_buf *qb, void *data)
{
	memset(data, 0xFF, min_t(size_t, qb->el_size, AVE_SENTINEL_FILL_SIZE));
}

/*
 * Reserve a submission on sq, fill the ring's tail slot (from copy_src, or
 * with the 0xFF sentinel when copy_src is NULL), queue submit_len bytes and
 * advance the tail. A NULL timeout makes the reservation non-blocking
 * (IRQ-safe). Does NOT ring the doorbell — callers batch via
 * bce_submit_to_device().
 */
static int ave_ring_submit_one(struct bce_queue_sq *sq, struct ave_queue_buf *qb,
			       const void *copy_src, size_t submit_len,
			       unsigned long *timeout)
{
	struct bce_qe_submission *s;

	if (WARN_ON_ONCE(ave_ring_full(qb)))
		return -ENOSPC;

	if (bce_reserve_submission(sq, timeout))
		return -ETIMEDOUT;

	if (copy_src)
		memcpy(ave_ring_tail_data(qb), copy_src, qb->el_size);
	else
		ave_fill_sentinel(qb, ave_ring_tail_data(qb));

	s = bce_next_submission(sq);
	bce_set_submission_single(s, ave_ring_tail_dma(qb), submit_len);
	qb->tail++;
	return 0;
}

static int ave_alloc_queue_buf(struct apple_bce_device *bce, struct ave_queue_buf *qb,
			       size_t el_size, size_t el_count)
{
	size_t i;

	qb->el_size = el_size;
	qb->el_count = el_count;
	qb->head = 0;
	qb->tail = 0;

	qb->data = kcalloc(el_count, sizeof(*qb->data), GFP_KERNEL);
	if (!qb->data)
		return -ENOMEM;

	qb->dma_addrs = kcalloc(el_count, sizeof(*qb->dma_addrs), GFP_KERNEL);
	if (!qb->dma_addrs) {
		kfree(qb->data);
		qb->data = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < el_count; i++) {
		qb->data[i] = dma_alloc_coherent(&bce->pci->dev, el_size,
						  &qb->dma_addrs[i], GFP_KERNEL);
		if (!qb->data[i]) {
			pr_err("DMA alloc failed: element %zu (%zu bytes)\n",
			       i, el_size);
			goto fail;
		}
	}
	pr_debug("DMA buf alloc: %zu x %zu bytes (%zu elements)\n",
		 el_size, el_count, el_count);
	return 0;

fail:
	while (i--)
		dma_free_coherent(&bce->pci->dev, el_size, qb->data[i], qb->dma_addrs[i]);
	kfree(qb->dma_addrs);
	qb->dma_addrs = NULL;
	kfree(qb->data);
	qb->data = NULL;
	return -ENOMEM;
}

static void ave_free_queue_buf(struct apple_bce_device *bce, struct ave_queue_buf *qb)
{
	size_t i;

	if (!qb->data)
		return;

	for (i = 0; i < qb->el_count; i++) {
		if (qb->data[i])
			dma_free_coherent(&bce->pci->dev, qb->el_size,
					  qb->data[i], qb->dma_addrs[i]);
	}
	kfree(qb->dma_addrs);
	qb->dma_addrs = NULL;
	kfree(qb->data);
	qb->data = NULL;
}

int ave_queues_create(struct apple_bce_device *bce, struct ave_queues *queues)
{
	int status;

	pr_debug("creating BCE queues...\n");

	memset(queues, 0, sizeof(*queues));
	queues->bce = bce;
	init_waitqueue_head(&queues->cmd_wq);
	init_waitqueue_head(&queues->q3_wq);

	/* CQ0 + Q0: AVEParameterSubmitQueue — Host→T2 (flags=3) */
	queues->cq[0] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[0]) {
		pr_err("failed to create CQ0\n");
		return -ENOMEM;
	}
	queues->sq_submit = bce_create_sq_with_flags(bce, queues->cq[0],
					  "AVEParameterSubmitQueue",
					  AVE_SQ_DEPTH, 3,
					  ave_q0_completion, queues);
	if (!queues->sq_submit) {
		pr_err("FAILED to create Q0 (ParameterSubmit)\n");
		status = -EINVAL;
		goto fail_cq0;
	}
	pr_debug("Q0 created OK (cq=%d sq=%d)\n",
		 queues->cq[0]->qid, queues->sq_submit->qid);

	/* Q0 DMA ring buffer for commands */
	status = ave_alloc_queue_buf(bce, &queues->q0_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_sq0;

	/* CQ1 + Q1: AVEParameterReturnQueue — T2→Host (flags=2) */
	queues->cq[1] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[1]) {
		pr_err("failed to create CQ1\n");
		status = -ENOMEM;
		goto fail_q0_buf;
	}
	queues->sq_return = bce_create_sq_with_flags(bce, queues->cq[1],
					  "AVEParameterReturnQueue",
					  AVE_SQ_DEPTH, 2,
					  ave_q1_completion, queues);
	if (!queues->sq_return) {
		pr_err("FAILED to create Q1 (ParameterReturn)\n");
		status = -EINVAL;
		goto fail_cq1;
	}
	pr_debug("Q1 created OK (cq=%d sq=%d)\n",
		 queues->cq[1]->qid, queues->sq_return->qid);

	/* CQ2 + Q2: AVECallbackReturnQueue — Host→T2 callback echo (flags=3) */
	queues->cq[2] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[2]) {
		pr_err("failed to create CQ2\n");
		status = -ENOMEM;
		goto fail_sq1;
	}
	queues->sq_cb_return = bce_create_sq_with_flags(bce, queues->cq[2],
					     "AVECallbackReturnQueue",
					     AVE_SQ_DEPTH, 3,
					     ave_q2_completion, queues);
	if (!queues->sq_cb_return) {
		pr_err("FAILED to create Q2 (CallbackReturn)\n");
		status = -EINVAL;
		goto fail_cq2;
	}
	pr_debug("Q2 created OK (cq=%d sq=%d)\n",
		 queues->cq[2]->qid, queues->sq_cb_return->qid);

	/* CQ3 + Q3: AVECallbackSubmitQueue — T2→Host callback data (flags=2) */
	queues->cq[3] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[3]) {
		pr_err("failed to create CQ3\n");
		status = -ENOMEM;
		goto fail_sq2;
	}
	queues->sq_cb_submit = bce_create_sq_with_flags(bce, queues->cq[3],
					     "AVECallbackSubmitQueue",
					     AVE_SQ_DEPTH, 2,
					     ave_q3_completion, queues);
	if (!queues->sq_cb_submit) {
		pr_err("FAILED to create Q3 (CallbackSubmit)\n");
		status = -EINVAL;
		goto fail_cq3;
	}
	pr_debug("Q3 created OK (cq=%d sq=%d)\n",
		 queues->cq[3]->qid, queues->sq_cb_submit->qid);

	/* Allocate DMA ring buffers for receive queues */
	pr_debug("allocating Q1 receive ring (%d x %d bytes)...\n",
		 AVE_RECV_BUF_COUNT, AVE_CMD_BUF_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q1_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_sq3;

	pr_debug("allocating Q2 receive ring (%d x %d bytes)...\n",
		 AVE_RECV_BUF_COUNT, AVE_CMD_BUF_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q2_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_q1_buf;

	pr_debug("allocating Q3 output ring (%d x %d bytes)...\n",
		 AVE_OUTPUT_BUF_COUNT, AVE_MAX_ENCODED_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q3_buf,
				     AVE_MAX_ENCODED_SIZE, AVE_OUTPUT_BUF_COUNT);
	if (status)
		goto fail_q2_buf;

	pr_debug("all queues + buffers created successfully\n");
	return 0;

fail_q2_buf:
	ave_free_queue_buf(bce, &queues->q2_buf);
fail_q1_buf:
	ave_free_queue_buf(bce, &queues->q1_buf);
fail_sq3:
	bce_destroy_sq(bce, queues->sq_cb_submit);
fail_cq3:
	bce_destroy_cq(bce, queues->cq[3]);
fail_sq2:
	bce_destroy_sq(bce, queues->sq_cb_return);
fail_cq2:
	bce_destroy_cq(bce, queues->cq[2]);
fail_sq1:
	bce_destroy_sq(bce, queues->sq_return);
fail_cq1:
	bce_destroy_cq(bce, queues->cq[1]);
fail_q0_buf:
	ave_free_queue_buf(bce, &queues->q0_buf);
fail_sq0:
	bce_destroy_sq(bce, queues->sq_submit);
fail_cq0:
	bce_destroy_cq(bce, queues->cq[0]);
	return status;
}

void ave_queues_destroy(struct ave_queues *queues)
{
	struct apple_bce_device *bce = queues->bce;

	if (!bce)
		return;

	pr_debug("destroying queues...\n");

	/* The flushes below complete any in-flight Q3 submissions; with
	 * auto-resubmit still armed, ave_q3_completion would hand the
	 * freshly-completed buffers straight back to the T2 from the IRQ —
	 * and we are about to free them. Disarm it, then wait out any
	 * completion handler already running on another CPU that may have
	 * read the flag as true. */
	queues->q3_auto_resubmit = false;
	synchronize_irq(pci_irq_vector(bce->pci, 4));

	/* Flush all queues before freeing DMA buffers. This tells the T2
	 * to drain in-flight operations, preventing the firmware from
	 * DMA-ing to freed addresses. Without this, the AVE firmware may
	 * be stuck waiting for data that will never arrive. The VHCI
	 * subsystem does the same (bce_vhci_event_queue_pause). */
	if (queues->sq_cb_submit)
		bce_cmd_flush_memory_queue(bce->cmd_cmdq, queues->sq_cb_submit->qid);
	if (queues->sq_cb_return)
		bce_cmd_flush_memory_queue(bce->cmd_cmdq, queues->sq_cb_return->qid);
	if (queues->sq_return)
		bce_cmd_flush_memory_queue(bce->cmd_cmdq, queues->sq_return->qid);
	if (queues->sq_submit)
		bce_cmd_flush_memory_queue(bce->cmd_cmdq, queues->sq_submit->qid);

	ave_free_queue_buf(bce, &queues->q3_buf);
	ave_free_queue_buf(bce, &queues->q2_buf);
	ave_free_queue_buf(bce, &queues->q1_buf);
	ave_free_queue_buf(bce, &queues->q0_buf);

	if (queues->sq_cb_submit)
		bce_destroy_sq(bce, queues->sq_cb_submit);
	if (queues->cq[3])
		bce_destroy_cq(bce, queues->cq[3]);
	if (queues->sq_cb_return)
		bce_destroy_sq(bce, queues->sq_cb_return);
	if (queues->cq[2])
		bce_destroy_cq(bce, queues->cq[2]);
	if (queues->sq_return)
		bce_destroy_sq(bce, queues->sq_return);
	if (queues->cq[1])
		bce_destroy_cq(bce, queues->cq[1]);
	if (queues->sq_submit)
		bce_destroy_sq(bce, queues->sq_submit);
	if (queues->cq[0])
		bce_destroy_cq(bce, queues->cq[0]);

	queues->bce = NULL;
	pr_debug("queues destroyed\n");
}

/*
 * Pre-submit empty receive buffers on a T2→Host queue.
 * Similar to aaudio_bce_in_queue_submit_pending().
 * May be called from the completion IRQ (auto-resubmit), so reservation is
 * non-blocking (NULL timeout).
 */
static void ave_presubmit_queue(struct bce_queue_sq *sq, struct ave_queue_buf *qb, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (ave_ring_submit_one(sq, qb, NULL, qb->el_size, NULL)) {
			pr_err("failed to reserve submission for pre-submit (i=%zu)\n", i);
			break;
		}
	}
	bce_submit_to_device(sq);
}

/*
 * Pre-submit receive buffers on Q3 only.
 * Q1 is NOT pre-submitted here — it is submitted one buffer at a time
 * alongside each Q0 command in ave_cmd_send_sync() (macOS pairing pattern).
 * Q2 is NOT pre-submitted — it is echoed per-event via ave_submit_q2_echo().
 * Q3 buffers are only active during frame encoding, not during setup.
 */
void ave_presubmit_recv_bufs(struct ave_queues *queues)
{
	pr_debug("pre-submitting receive buffers: Q3=%d (Q1 paired with Q0, Q2 echoed per-event)\n",
		 AVE_OUTPUT_BUF_COUNT);
	queues->q3_auto_resubmit = true;
	/* Q2 is NOT pre-submitted — it is only submitted as explicit callback echoes
	 * via ave_submit_q2_echo() during frame encoding. The T2 expects Q2 data to
	 * contain the echoed Q3 callback content (handshake/flow control). */
	ave_presubmit_queue(queues->sq_cb_submit, &queues->q3_buf, AVE_OUTPUT_BUF_COUNT);
	pr_debug("receive buffers pre-submitted\n");
}

/*
 * Submit a command buffer on Q0 and wait for the Q1 response.
 * macOS pairs each Q0 command with exactly one Q1 receive buffer
 * (finding 26, investigation 2). The T2 firmware expects this pairing.
 */
int ave_cmd_send_sync(struct ave_queues *queues, void *cmd_buf, size_t cmd_size)
{
	u32 cmd_type = *(u32 *)cmd_buf;
	int status;

	pr_debug("cmd_send_sync: cmd=0x%02x size=%zu\n", cmd_type, cmd_size);

	status = ave_submit_q0_cmd(queues, cmd_buf, cmd_size);
	if (status)
		return status;

	status = ave_submit_q1_recv(queues);
	if (status)
		return status;

	status = ave_wait_q1(queues, AVE_RESPONSE_TIMEOUT_MS);
	if (status == -ETIMEDOUT) {
		pr_err("TIMEOUT waiting for Q1 response (cmd=0x%02x)\n", cmd_type);
		return status;
	}

	pr_debug("cmd 0x%02x completed, status=%d\n", cmd_type, status);
	return status;
}

/* === Completion callbacks (run from the BCE completion IRQ) === */

static void ave_q0_completion(struct bce_queue_sq *sq)
{
	int cnt = 0;

	/*
	 * Q0 carries both ring-slot command submissions and out-of-line
	 * frame-data submissions (ave_submit_frame_data_async), and the
	 * completions do not identify which kind they acknowledge — so the
	 * q0_buf head is NOT advanced here and ave_ring_full() must never
	 * be applied to q0_buf (it would read permanently-full). Command
	 * slot reuse is instead guaranteed by serialization: all Q0 command
	 * flows run under the session mutex and wait for their Q1 response,
	 * so at most two of the eight command slots are ever in flight.
	 */
	while (bce_next_completion(sq)) {
		bce_notify_submission_complete(sq);
		cnt++;
	}

	pr_debug("Q0 completion: %d items drained\n", cnt);
}

static void ave_q1_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct bce_sq_completion_data *c;
	struct ave_queue_buf *qb = &queues->q1_buf;
	size_t cnt = 0;

	while ((c = bce_next_completion(sq))) {
		void *resp = qb->data[qb->head % qb->el_count];

		pr_debug("Q1 response: status=%u data_size=%llu result=0x%llx\n",
			 c->status, c->data_size, c->result);

		queues->cmd_status = (c->status == BCE_COMPLETION_SUCCESS) ? 0 : -EIO;
		queues->cmd_resp_buf = resp;

		bce_notify_submission_complete(sq);
		/* Publish the status/response written above */
		smp_store_release(&qb->head, qb->head + 1);
		cnt++;
	}

	/*
	 * Do NOT auto-resubmit Q1 buffers here. macOS pairs each Q1 buffer
	 * with a Q0 command in ave_cmd_send_sync(). Auto-resubmitting would
	 * break the strict pairing the T2 firmware expects.
	 */

	if (cnt)
		wake_up(&queues->cmd_wq);
}

static void ave_q2_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct bce_sq_completion_data *c;
	struct ave_queue_buf *qb = &queues->q2_buf;

	while ((c = bce_next_completion(sq))) {
		pr_debug("Q2 completion: status=%u data_size=%llu result=0x%llx\n",
			 c->status, c->data_size, c->result);

		WRITE_ONCE(qb->head, qb->head + 1);
		bce_notify_submission_complete(sq);
	}

	/* Q2 buffers are NOT auto-resubmitted. During encoding, Q2 is
	 * explicitly submitted as callback echoes via ave_submit_q2_echo().
	 * The T2 expects echoed Q3 callback data on Q2, not sentinel fills. */
}

static void ave_q3_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct bce_sq_completion_data *c;
	struct ave_queue_buf *qb = &queues->q3_buf;
	size_t cnt = 0;

	while ((c = bce_next_completion(sq))) {
		u64 seq = qb->head;

		pr_debug("Q3 event[%llu]: status=%u data_size=%llu result=0x%llx\n",
			 seq, c->status, c->data_size, c->result);

		queues->q3_result[seq % AVE_OUTPUT_BUF_COUNT] = c->data_size;

		bce_notify_submission_complete(sq);
		/* Publish the result scalar and buffer contents */
		smp_store_release(&qb->head, qb->head + 1);
		cnt++;
	}

	if (cnt)
		wake_up(&queues->q3_wq);

	/* Only auto-resubmit during setup/teardown, not during encoding */
	if (queues->q3_auto_resubmit && cnt) {
		pr_debug("Q3 auto-resubmitting %zu output buffers\n", cnt);
		ave_presubmit_queue(sq, qb, cnt);
	}
}

/* === Async submit/wait primitives for encoding pipeline === */

/*
 * Submit a command on Q0 without pairing Q1 (for async EncodeFrame pipeline).
 * Deliberately not ave_ring_submit_one(): q0_buf's head never advances (see
 * ave_q0_completion), so the ring-full check would misfire — slot reuse is
 * safe by command serialization instead.
 */
int ave_submit_q0_cmd(struct ave_queues *queues, void *cmd_buf, size_t cmd_size)
{
	struct bce_qe_submission *s;
	struct ave_queue_buf *q0 = &queues->q0_buf;
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	int status;

	if (cmd_size > AVE_CMD_BUF_SIZE)
		return -EINVAL;

	status = bce_reserve_submission(queues->sq_submit, &timeout);
	if (status) {
		pr_err("Q0 reservation timeout\n");
		return -ETIMEDOUT;
	}

	memcpy(ave_ring_tail_data(q0), cmd_buf, cmd_size);

	s = bce_next_submission(queues->sq_submit);
	bce_set_submission_single(s, ave_ring_tail_dma(q0), cmd_size);
	q0->tail++;
	bce_submit_to_device(queues->sq_submit);
	pr_debug("cmd 0x%02x submitted on Q0\n", *(u32 *)cmd_buf);
	return 0;
}

/*
 * Submit one receive buffer on Q1 for the response to the Q0 command that
 * was just submitted. ave_wait_q1() waits for this submission's completion.
 */
int ave_submit_q1_recv(struct ave_queues *queues)
{
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	int status;

	status = ave_ring_submit_one(queues->sq_return, &queues->q1_buf,
				     NULL, queues->q1_buf.el_size, &timeout);
	if (status) {
		pr_err("Q1 recv submit failed (%d)\n", status);
		return status;
	}
	bce_submit_to_device(queues->sq_return);
	return 0;
}

/*
 * Submit one empty buffer on Q3 for T2 to fill with callback data.
 */
int ave_submit_q3_buf(struct ave_queues *queues)
{
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	int status;

	status = ave_ring_submit_one(queues->sq_cb_submit, &queues->q3_buf,
				     NULL, queues->q3_buf.el_size, &timeout);
	if (status)
		return status;
	bce_submit_to_device(queues->sq_cb_submit);
	return 0;
}

/*
 * Echo Q3 callback data back on Q2 (callback acknowledgment).
 * The T2 expects the host to copy Q3 callback content to Q2 as a handshake.
 * Without this echo, the T2 stalls and never signals Q1 completion.
 * Copies the first 4KB (Q2 buffer size) of q3_data into the next Q2 ring slot.
 */
int ave_submit_q2_echo(struct ave_queues *queues, const void *q3_data)
{
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	int status;

	if (!q3_data)
		return -EINVAL;

	status = ave_ring_submit_one(queues->sq_cb_return, &queues->q2_buf,
				     q3_data, queues->q2_buf.el_size, &timeout);
	if (status)
		return status;
	bce_submit_to_device(queues->sq_cb_return);

	pr_debug("Q2 echo submitted\n");
	return 0;
}

/*
 * Wait for the Q1 response to the most recent ave_submit_q1_recv().
 * Responses complete the oldest outstanding buffer first, so a response
 * belonging to an older, timed-out command only advances q1_buf.head
 * towards (never past) this command's target — it cannot satisfy the wait.
 */
int ave_wait_q1(struct ave_queues *queues, unsigned long timeout_ms)
{
	size_t target = queues->q1_buf.tail;

	if (!wait_event_timeout(queues->cmd_wq,
				smp_load_acquire(&queues->q1_buf.head) >= target,
				msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return queues->cmd_status;
}

/*
 * Wait until at least `count` Q3 events have arrived since ave_q3_reset().
 */
int ave_wait_q3(struct ave_queues *queues, size_t count, unsigned long timeout_ms)
{
	u64 target = queues->q3_base + count;

	if (!wait_event_timeout(queues->q3_wq,
				smp_load_acquire(&queues->q3_buf.head) >= target,
				msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return 0;
}

/*
 * Access completed Q3 data for a given event index (relative to the last
 * ave_q3_reset()). Buffers are consumed strictly in submission order, and
 * the ring tail free-runs from 0, so the buffer for absolute event sequence
 * s is simply ring slot (s % el_count).
 */
void *ave_q3_completed_data(struct ave_queues *queues, size_t event_index)
{
	struct ave_queue_buf *qb = &queues->q3_buf;
	u64 seq = queues->q3_base + event_index;

	if (seq >= smp_load_acquire(&qb->head)) {
		pr_err("Q3 event %zu not yet completed (seq %llu >= head %zu)\n",
		       event_index, seq, qb->head);
		return NULL;
	}

	return qb->data[seq % qb->el_count];
}

size_t ave_q3_completed_size(struct ave_queues *queues, size_t event_index)
{
	u64 seq = queues->q3_base + event_index;

	if (seq >= smp_load_acquire(&queues->q3_buf.head))
		return 0;
	return queues->q3_result[seq % AVE_OUTPUT_BUF_COUNT];
}

size_t ave_q3_event_count(struct ave_queues *queues)
{
	return (size_t)(smp_load_acquire(&queues->q3_buf.head) - queues->q3_base);
}

/*
 * Reset Q3 tracking for a new frame. Must be called before submitting
 * Q3 buffers for a new encoding round.
 */
void ave_q3_reset(struct ave_queues *queues)
{
	queues->q3_base = READ_ONCE(queues->q3_buf.head);
}

/*
 * Submit frame data (Y/UV plane) on Q0 without waiting for Q0 ack.
 * DMA mapping is returned in dma_out; caller MUST call
 * bce_unmap_dma_buffer() after the T2 has finished reading (i.e., after
 * Q1 response). This avoids the race condition in ave_submit_frame_data()
 * where a Q0 command ack could prematurely signal the frame data ack.
 */
int ave_submit_frame_data_async(struct ave_queues *queues, void *data, size_t size,
				struct bce_dma_buffer *dma_out)
{
	struct bce_qe_submission *s;
	unsigned long timeout;
	int status;

	status = bce_map_dma_buffer_vm(&queues->bce->pci->dev, dma_out,
				       data, size, DMA_TO_DEVICE);
	if (status) {
		pr_err("failed to map frame data DMA (%zu bytes, err=%d)\n",
		       size, status);
		return status;
	}

	timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	status = bce_reserve_submission(queues->sq_submit, &timeout);
	if (status) {
		pr_err("Q0 reservation timeout for async frame data\n");
		bce_unmap_dma_buffer(&queues->bce->pci->dev, dma_out);
		return -ETIMEDOUT;
	}

	s = bce_next_submission(queues->sq_submit);
	bce_set_submission_buf(s, dma_out, 0, size);
	bce_submit_to_device(queues->sq_submit);
	return 0;
}

/* === Command buffer builders === */

/*
 * Wire layouts of the T2 AVE commands (from disassembly of
 * AppleAVEEncoder.bundle / aveserverd and macOS packet captures).
 * All commands live in a 4096-byte buffer; bytes not named below keep the
 * fill pattern set by the builder (0x00, 0xBB or 0xFF — the T2 firmware
 * rejects all-zero SetProperty buffers, finding 27/28).
 *
 * The session token at +0x08 is stamped separately by the encoder
 * (ave_stamp_session_token()) after the CodecID response provides it.
 */

struct ave_cmd_hdr {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u64 token;	/* 0x08 */
} __packed;

struct ave_cmd_codec_id {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u32 fourcc;	/* 0x08: "hvc1" as LE bytes: 31 63 76 68 */
} __packed;

struct ave_cmd_session_config {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u64 token;	/* 0x08 */
	u64 pad10;
	u32 width;	/* 0x18 */
	u32 height;	/* 0x1c */
} __packed;

struct ave_cmd_encode_frame {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u64 token;	/* 0x08 */
	u64 frame_num;	/* 0x10 */
	u32 keyframe;	/* 0x18 */
	u32 pad1c;
	u32 fps_num;	/* 0x20 */
	u32 fps_den;	/* 0x24 */
	u64 pad28;
	u32 unk30;	/* 0x30: always 1 */
	u32 pad34;
	u32 duration_num; /* 0x38 */
	u32 duration_den; /* 0x3c */
	u64 pad40;
	u32 width;	/* 0x48 */
	u32 pad4c;
	u32 height;	/* 0x50 */
	u32 pad54;
	u64 pad58;
	u32 bit_depth;	/* 0x60 */
	u32 pad64;
	u32 stride;	/* 0x68 */
	u32 pad6c;
	char pixfmt[4];	/* 0x70: CoreVideo pixel format FourCC, byte-reversed */
	u32 pad74;	/* dead data — aveserverd never reads this field.
			 * Previously hardcoded to 0x7000 from a packet capture. */
	u64 pad78;
	u64 cookie;	/* 0x80: echoed back by the T2, opaque to firmware */
} __packed;

struct ave_cmd_copy_property {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u64 token;	/* 0x08 */
	char name[AVE_CMD_BUF_SIZE - 0x10]; /* 0x10 */
} __packed;

/*
 * SetProperty command layout (from T2 readCFKeyValuePair):
 *   +0x00: cmd_type (0x09)
 *   +0x08: session token (stamped by caller)
 *   +0x10: property name string
 *   +0x30: buffer size (0x1000)
 *   +0x64: value-present flag (1)
 *   +0x74: type code (1=bool, 2=SInt32, 4=Float32, 6=CFString)
 *   +0x78: value length in bytes
 *   +0x80: value data
 */
struct ave_cmd_set_property {
	u32 cmd;	/* 0x00 */
	u32 pad04;
	u64 token;	/* 0x08 */
	char name[0x20]; /* 0x10 */
	u32 buf_size;	/* 0x30: 0x1000 */
	u32 pad34;	/* 0x34: 0 */
	u8  pad38[0x2c];
	u32 value_present; /* 0x64: 1 */
	u8  pad68[0x0c];
	u32 type;	/* 0x74 */
	u32 length;	/* 0x78 */
	u32 pad7c;
	u8  value[];	/* 0x80 */
} __packed;

struct ave_cmd_end_session {
	u32 cmd;	/* 0x00 */
	u32 pad04;	/* 0x04: 0 */
	u64 token;	/* 0x08: stamped by ave_stamp_session_token() */
} __packed;

static_assert(offsetof(struct ave_cmd_hdr, token) == 0x08);
static_assert(offsetof(struct ave_cmd_codec_id, fourcc) == 0x08);
static_assert(offsetof(struct ave_cmd_session_config, width) == 0x18);
static_assert(offsetof(struct ave_cmd_session_config, height) == 0x1c);
static_assert(offsetof(struct ave_cmd_encode_frame, frame_num) == 0x10);
static_assert(offsetof(struct ave_cmd_encode_frame, keyframe) == 0x18);
static_assert(offsetof(struct ave_cmd_encode_frame, fps_num) == 0x20);
static_assert(offsetof(struct ave_cmd_encode_frame, unk30) == 0x30);
static_assert(offsetof(struct ave_cmd_encode_frame, duration_num) == 0x38);
static_assert(offsetof(struct ave_cmd_encode_frame, width) == 0x48);
static_assert(offsetof(struct ave_cmd_encode_frame, height) == 0x50);
static_assert(offsetof(struct ave_cmd_encode_frame, bit_depth) == 0x60);
static_assert(offsetof(struct ave_cmd_encode_frame, stride) == 0x68);
static_assert(offsetof(struct ave_cmd_encode_frame, pixfmt) == 0x70);
static_assert(offsetof(struct ave_cmd_encode_frame, cookie) == 0x80);
static_assert(offsetof(struct ave_cmd_copy_property, name) == 0x10);
static_assert(offsetof(struct ave_cmd_set_property, name) == 0x10);
static_assert(offsetof(struct ave_cmd_set_property, buf_size) == 0x30);
static_assert(offsetof(struct ave_cmd_set_property, value_present) == 0x64);
static_assert(offsetof(struct ave_cmd_set_property, type) == 0x74);
static_assert(offsetof(struct ave_cmd_set_property, length) == 0x78);
static_assert(sizeof(struct ave_cmd_set_property) == 0x80);
static_assert(offsetof(struct ave_cmd_end_session, token) == 0x08);

void ave_build_cmd_codec_id(void *buf)
{
	struct ave_cmd_codec_id *c = buf;

	memset(buf, 0xBB, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_CODEC_ID;
	c->fourcc = 0x68766331; /* "hvc1" as LE bytes: 31 63 76 68 */
}

void ave_build_cmd_session_config(void *buf, u32 width, u32 height)
{
	struct ave_cmd_session_config *c = buf;

	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_SESSION_CONFIG;
	/* Resolution — only fields the T2 firmware actually reads */
	c->width = width;
	c->height = height;
}

void ave_build_cmd_encode_frame(void *buf, u64 frame_num, u32 width, u32 height,
				u32 fps_num, u32 fps_den, bool keyframe,
				u64 cookie)
{
	struct ave_cmd_encode_frame *c = buf;

	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_ENCODE_FRAME;
	c->frame_num = frame_num;
	c->keyframe = keyframe ? 1 : 0;
	c->fps_num = fps_num;
	c->fps_den = fps_den;
	c->unk30 = 1;
	c->duration_num = fps_num;
	c->duration_den = fps_den;
	c->width = width;
	c->height = height;
	c->bit_depth = 8;
	c->stride = width;
	/* "v024" = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ('420v').
	 * aveserverd also accepts 'x420' (10-bit video range) and
	 * 'xf20' (10-bit full range). */
	memcpy(c->pixfmt, "v024", 4);
	c->cookie = cookie;
}

void ave_build_cmd_copy_property(void *buf, const char *name)
{
	struct ave_cmd_copy_property *c = buf;

	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_COPY_PROPERTY;
	strscpy(c->name, name, sizeof(c->name));
}

static struct ave_cmd_set_property *ave_build_cmd_set_property_common(void *buf,
								      const char *name)
{
	struct ave_cmd_set_property *c = buf;

	/* T2 firmware rejects all-zero buffers — must use 0xBB fill for
	 * don't-care bytes, then zero only the header (finding 27/28). */
	memset(buf, 0xBB, AVE_CMD_BUF_SIZE);
	memset(buf, 0x00, 0x20);

	c->cmd = AVE_CMD_SET_PROPERTY;
	strscpy(c->name, name, sizeof(c->name));

	c->buf_size = 0x1000;
	c->pad34 = 0x0000;
	c->value_present = 0x00000001;
	return c;
}

void ave_build_cmd_set_property_bool(void *buf, const char *name, bool value)
{
	struct ave_cmd_set_property *c = ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000001;	/* bool */
	c->length = 0x00000001;	/* 1 byte */
	c->value[0] = value ? 1 : 0;
}

void ave_build_cmd_set_property_s32(void *buf, const char *name, s32 value)
{
	struct ave_cmd_set_property *c = ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000002;	/* SInt32 */
	c->length = 0x00000004;	/* 4 bytes */
	*(s32 *)c->value = value;
}

void ave_build_cmd_set_property_float32(void *buf, const char *name, u32 ieee754_bits)
{
	struct ave_cmd_set_property *c = ave_build_cmd_set_property_common(buf, name);

	c->type = 0x00000004;	/* Float32 */
	c->length = 0x00000004;	/* 4 bytes */
	*(u32 *)c->value = ieee754_bits;
}

void ave_build_cmd_set_property_string(void *buf, const char *name, const char *value)
{
	struct ave_cmd_set_property *c = ave_build_cmd_set_property_common(buf, name);
	size_t len = strlen(value);

	c->type = 0x00000006;	/* CFString */
	c->length = len;
	memcpy(c->value, value, min_t(size_t, len + 1, AVE_CMD_BUF_SIZE - 0x80));
}

void ave_build_cmd_prepare(void *buf)
{
	struct ave_cmd_hdr *c = buf;

	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_PREPARE;
}

void ave_build_cmd_complete_frames(void *buf)
{
	struct ave_cmd_hdr *c = buf;

	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_COMPLETE_FRAMES;
}

void ave_build_cmd_end_session(void *buf)
{
	struct ave_cmd_end_session *c = buf;

	memset(buf, 0xFF, AVE_CMD_BUF_SIZE);
	c->cmd = AVE_CMD_END_SESSION;
	c->pad04 = 0;
	/* token is stamped by the caller over 0x08..0x0f */
}
