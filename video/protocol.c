#include "protocol.h"
#include "../apple_bce.h"

#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>

static void ave_q0_completion(struct bce_queue_sq *sq);
static void ave_q1_completion(struct bce_queue_sq *sq);
static void ave_q2_completion(struct bce_queue_sq *sq);
static void ave_q3_completion(struct bce_queue_sq *sq);

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
			pr_err("apple-ave: DMA alloc failed: element %zu (%zu bytes)\n",
			       i, el_size);
			goto fail;
		}
	}
	pr_debug("apple-ave: DMA buf alloc: %zu x %zu bytes (%zu elements)\n",
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

	pr_debug("apple-ave: creating BCE queues...\n");

	memset(queues, 0, sizeof(*queues));
	queues->bce = bce;
	init_completion(&queues->cmd_completion);
	init_completion(&queues->q3_completion);

	/* CQ0 + Q0: AVEParameterSubmitQueue — Host→T2 (flags=3) */
	queues->cq[0] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[0]) {
		pr_err("apple-ave: failed to create CQ0\n");
		return -ENOMEM;
	}
	queues->sq_submit = bce_create_sq_with_flags(bce, queues->cq[0],
					  "AVEParameterSubmitQueue",
					  AVE_SQ_DEPTH, 3,
					  ave_q0_completion, queues);
	if (!queues->sq_submit) {
		pr_err("apple-ave: FAILED to create Q0 (ParameterSubmit)\n");
		status = -EINVAL;
		goto fail_cq0;
	}
	pr_debug("apple-ave: Q0 created OK (cq=%d sq=%d)\n",
		 queues->cq[0]->qid, queues->sq_submit->qid);

	/* Q0 DMA ring buffer for commands */
	status = ave_alloc_queue_buf(bce, &queues->q0_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_sq0;

	/* CQ1 + Q1: AVEParameterReturnQueue — T2→Host (flags=2) */
	queues->cq[1] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[1]) {
		pr_err("apple-ave: failed to create CQ1\n");
		status = -ENOMEM;
		goto fail_q0_buf;
	}
	queues->sq_return = bce_create_sq_with_flags(bce, queues->cq[1],
					  "AVEParameterReturnQueue",
					  AVE_SQ_DEPTH, 2,
					  ave_q1_completion, queues);
	if (!queues->sq_return) {
		pr_err("apple-ave: FAILED to create Q1 (ParameterReturn)\n");
		status = -EINVAL;
		goto fail_cq1;
	}
	pr_debug("apple-ave: Q1 created OK (cq=%d sq=%d)\n",
		 queues->cq[1]->qid, queues->sq_return->qid);

	/* CQ2 + Q2: AVECallbackReturnQueue — Host→T2 callback echo (flags=3) */
	queues->cq[2] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[2]) {
		pr_err("apple-ave: failed to create CQ2\n");
		status = -ENOMEM;
		goto fail_sq1;
	}
	queues->sq_cb_return = bce_create_sq_with_flags(bce, queues->cq[2],
					     "AVECallbackReturnQueue",
					     AVE_SQ_DEPTH, 3,
					     ave_q2_completion, queues);
	if (!queues->sq_cb_return) {
		pr_err("apple-ave: FAILED to create Q2 (CallbackReturn)\n");
		status = -EINVAL;
		goto fail_cq2;
	}
	pr_debug("apple-ave: Q2 created OK (cq=%d sq=%d)\n",
		 queues->cq[2]->qid, queues->sq_cb_return->qid);

	/* CQ3 + Q3: AVECallbackSubmitQueue — T2→Host callback data (flags=2) */
	queues->cq[3] = bce_create_cq(bce, AVE_CQ_DEPTH);
	if (!queues->cq[3]) {
		pr_err("apple-ave: failed to create CQ3\n");
		status = -ENOMEM;
		goto fail_sq2;
	}
	queues->sq_cb_submit = bce_create_sq_with_flags(bce, queues->cq[3],
					     "AVECallbackSubmitQueue",
					     AVE_SQ_DEPTH, 2,
					     ave_q3_completion, queues);
	if (!queues->sq_cb_submit) {
		pr_err("apple-ave: FAILED to create Q3 (CallbackSubmit)\n");
		status = -EINVAL;
		goto fail_cq3;
	}
	pr_debug("apple-ave: Q3 created OK (cq=%d sq=%d)\n",
		 queues->cq[3]->qid, queues->sq_cb_submit->qid);

	/* Allocate DMA ring buffers for receive queues */
	pr_debug("apple-ave: allocating Q1 receive ring (%d x %d bytes)...\n",
		 AVE_RECV_BUF_COUNT, AVE_CMD_BUF_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q1_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_sq3;

	pr_debug("apple-ave: allocating Q2 receive ring (%d x %d bytes)...\n",
		 AVE_RECV_BUF_COUNT, AVE_CMD_BUF_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q2_buf,
				     AVE_CMD_BUF_SIZE, AVE_RECV_BUF_COUNT);
	if (status)
		goto fail_q1_buf;

	pr_debug("apple-ave: allocating Q3 output ring (%d x %d bytes)...\n",
		 AVE_OUTPUT_BUF_COUNT, AVE_MAX_ENCODED_SIZE);
	status = ave_alloc_queue_buf(bce, &queues->q3_buf,
				     AVE_MAX_ENCODED_SIZE, AVE_OUTPUT_BUF_COUNT);
	if (status)
		goto fail_q2_buf;

	pr_debug("apple-ave: all queues + buffers created successfully\n");
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

	pr_debug("apple-ave: destroying queues...\n");

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
	pr_debug("apple-ave: queues destroyed\n");
}

/*
 * Pre-submit empty receive buffers on Q1, Q2, Q3.
 * Similar to aaudio_bce_in_queue_submit_pending().
 */
static void ave_presubmit_queue(struct bce_queue_sq *sq, struct ave_queue_buf *qb, size_t count)
{
	struct bce_qe_submission *s;
	size_t i;

	for (i = 0; i < count; i++) {
		if (bce_reserve_submission(sq, NULL)) {
			pr_err("apple-ave: failed to reserve submission for pre-submit (i=%zu)\n", i);
			break;
		}
		/* Fill with 0xFF sentinel (as observed in macOS traces) */
		memset(qb->data[qb->tail], 0xFF, qb->el_size);

		s = bce_next_submission(sq);
		bce_set_submission_single(s,
					 qb->dma_addrs[qb->tail],
					 qb->el_size);
		qb->tail = (qb->tail + 1) % qb->el_count;
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
	pr_debug("apple-ave: pre-submitting receive buffers: Q3=%d (Q1 paired with Q0, Q2 echoed per-event)\n",
		 AVE_OUTPUT_BUF_COUNT);
	queues->q3_auto_resubmit = true;
	/* Q2 is NOT pre-submitted — it is only submitted as explicit callback echoes
	 * via ave_submit_q2_echo() during frame encoding. The T2 expects Q2 data to
	 * contain the echoed Q3 callback content (handshake/flow control). */
	ave_presubmit_queue(queues->sq_cb_submit, &queues->q3_buf, AVE_OUTPUT_BUF_COUNT);
	pr_debug("apple-ave: receive buffers pre-submitted\n");
}

/*
 * Submit a command buffer on Q0 and wait for the Q1 response.
 * Uses pre-allocated DMA ring (like audio driver) instead of per-call DMA mapping.
 */
int ave_cmd_send_sync(struct ave_queues *queues, void *cmd_buf, size_t cmd_size)
{
	struct bce_qe_submission *s;
	struct ave_queue_buf *q0 = &queues->q0_buf;
	struct ave_queue_buf *q1 = &queues->q1_buf;
	unsigned long timeout;
	int status;
	u32 cmd_type = *(u32 *)cmd_buf;

	pr_debug("apple-ave: cmd_send_sync: cmd=0x%02x size=%zu\n", cmd_type, cmd_size);

	if (cmd_size > AVE_CMD_BUF_SIZE)
		return -EINVAL;

	/* Copy command into pre-allocated DMA ring slot */
	memcpy(q0->data[q0->tail], cmd_buf, cmd_size);

	reinit_completion(&queues->cmd_completion);

	/* Reserve and submit on Q0 */
	timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	status = bce_reserve_submission(queues->sq_submit, &timeout);
	if (status) {
		pr_err("apple-ave: Q0 reservation timeout\n");
		return status;
	}

	s = bce_next_submission(queues->sq_submit);
	bce_set_submission_single(s, q0->dma_addrs[q0->tail], cmd_size);
	q0->tail = (q0->tail + 1) % q0->el_count;
	bce_submit_to_device(queues->sq_submit);
	pr_debug("apple-ave: cmd 0x%02x submitted on Q0\n", cmd_type);

	/*
	 * Submit one Q1 receive buffer immediately after Q0 command.
	 * macOS pairs each Q0 command with exactly one Q1 receive buffer
	 * (finding 26, investigation 2). The T2 firmware expects this pairing.
	 */
	timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	status = bce_reserve_submission(queues->sq_return, &timeout);
	if (status) {
		pr_err("apple-ave: Q1 reservation timeout\n");
		return status;
	}
	memset(q1->data[q1->tail], 0xFF, q1->el_size);
	s = bce_next_submission(queues->sq_return);
	bce_set_submission_single(s, q1->dma_addrs[q1->tail], q1->el_size);
	q1->tail = (q1->tail + 1) % q1->el_count;
	bce_submit_to_device(queues->sq_return);
	pr_debug("apple-ave: Q1 recv buf submitted, waiting for response...\n");

	/* Wait for Q1 response */
	if (!wait_for_completion_timeout(&queues->cmd_completion,
					 msecs_to_jiffies(AVE_RESPONSE_TIMEOUT_MS))) {
		pr_err("apple-ave: TIMEOUT waiting for Q1 response (cmd=0x%02x)\n", cmd_type);
		return -ETIMEDOUT;
	}

	pr_debug("apple-ave: cmd 0x%02x completed, status=%d resp_size=%zu\n",
		 cmd_type, queues->cmd_status, queues->cmd_resp_size);
	return queues->cmd_status;
}

/* === Completion callbacks === */

static void ave_q0_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct ave_queue_buf *qb = &queues->q0_buf;
	int cnt = 0;

	while (bce_next_completion(sq)) {
		bce_notify_submission_complete(sq);
		qb->head = (qb->head + 1) % qb->el_count;
		cnt++;
	}

	pr_debug("apple-ave: Q0 completion: %d items drained\n", cnt);
}

static void ave_q1_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct bce_sq_completion_data *c;
	struct ave_queue_buf *qb = &queues->q1_buf;
	size_t cnt = 0;

	while ((c = bce_next_completion(sq))) {
		void *resp = qb->data[qb->head];

		pr_debug("apple-ave: Q1 response: status=%u data_size=%llu result=0x%llx\n",
			 c->status, c->data_size, c->result);

		queues->cmd_status = (c->status == BCE_COMPLETION_SUCCESS) ? 0 : -EIO;
		queues->cmd_resp_size = c->data_size;
		queues->cmd_resp_buf = resp;

		qb->head = (qb->head + 1) % qb->el_count;
		bce_notify_submission_complete(sq);
		cnt++;
	}

	/*
	 * Do NOT auto-resubmit Q1 buffers here. macOS pairs each Q1 buffer
	 * with a Q0 command in ave_cmd_send_sync(). Auto-resubmitting would
	 * break the strict pairing the T2 firmware expects.
	 */

	complete(&queues->cmd_completion);
}

static void ave_q2_completion(struct bce_queue_sq *sq)
{
	struct ave_queues *queues = sq->userdata;
	struct bce_sq_completion_data *c;
	struct ave_queue_buf *qb = &queues->q2_buf;
	size_t cnt = 0;

	while ((c = bce_next_completion(sq))) {
		pr_debug("apple-ave: Q2 completion: status=%u data_size=%llu result=0x%llx\n",
			 c->status, c->data_size, c->result);

		qb->head = (qb->head + 1) % qb->el_count;
		bce_notify_submission_complete(sq);
		cnt++;
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
		size_t idx = queues->q3_event_count;

		pr_debug("apple-ave: Q3 event[%zu]: status=%u data_size=%llu result=0x%llx\n",
			 idx, c->status, c->data_size, c->result);

		if (idx < AVE_OUTPUT_BUF_COUNT)
			queues->q3_result[idx] = c->data_size;
		queues->q3_event_count++;

		qb->head = (qb->head + 1) % qb->el_count;
		bce_notify_submission_complete(sq);
		cnt++;

		/* Signal per-element — encoder waits on this N times */
		complete(&queues->q3_completion);
	}

	/* Only auto-resubmit during setup, not during encoding */
	if (queues->q3_auto_resubmit && cnt) {
		pr_debug("apple-ave: Q3 auto-resubmitting %zu output buffers\n", cnt);
		ave_presubmit_queue(sq, qb, cnt);
	}
}

/* === Async submit/wait primitives for encoding pipeline === */

/*
 * Submit a command on Q0 without pairing Q1 (for async EncodeFrame pipeline).
 */
int ave_submit_q0_cmd(struct ave_queues *queues, void *cmd_buf, size_t cmd_size)
{
	struct bce_qe_submission *s;
	struct ave_queue_buf *q0 = &queues->q0_buf;
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);

	if (cmd_size > AVE_CMD_BUF_SIZE)
		return -EINVAL;

	memcpy(q0->data[q0->tail], cmd_buf, cmd_size);

	if (bce_reserve_submission(queues->sq_submit, &timeout))
		return -ETIMEDOUT;

	s = bce_next_submission(queues->sq_submit);
	bce_set_submission_single(s, q0->dma_addrs[q0->tail], cmd_size);
	q0->tail = (q0->tail + 1) % q0->el_count;
	bce_submit_to_device(queues->sq_submit);
	return 0;
}

/*
 * Submit one receive buffer on Q1 and arm cmd_completion.
 * The reinit happens BEFORE submission to avoid racing with the callback.
 */
int ave_submit_q1_recv(struct ave_queues *queues)
{
	struct bce_qe_submission *s;
	struct ave_queue_buf *q1 = &queues->q1_buf;
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);

	memset(q1->data[q1->tail], 0xFF, q1->el_size);

	reinit_completion(&queues->cmd_completion);

	if (bce_reserve_submission(queues->sq_return, &timeout))
		return -ETIMEDOUT;

	s = bce_next_submission(queues->sq_return);
	bce_set_submission_single(s, q1->dma_addrs[q1->tail], q1->el_size);
	q1->tail = (q1->tail + 1) % q1->el_count;
	bce_submit_to_device(queues->sq_return);
	return 0;
}

/*
 * Submit one empty buffer on Q3 for T2 to fill with callback data.
 */
int ave_submit_q3_buf(struct ave_queues *queues)
{
	struct bce_qe_submission *s;
	struct ave_queue_buf *qb = &queues->q3_buf;
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);

	memset(qb->data[qb->tail], 0xFF, qb->el_size);

	if (bce_reserve_submission(queues->sq_cb_submit, &timeout))
		return -ETIMEDOUT;

	s = bce_next_submission(queues->sq_cb_submit);
	bce_set_submission_single(s, qb->dma_addrs[qb->tail], qb->el_size);
	qb->tail = (qb->tail + 1) % qb->el_count;
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
	struct bce_qe_submission *s;
	struct ave_queue_buf *qb = &queues->q2_buf;
	unsigned long timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);

	memcpy(qb->data[qb->tail], q3_data, qb->el_size);

	if (bce_reserve_submission(queues->sq_cb_return, &timeout))
		return -ETIMEDOUT;

	s = bce_next_submission(queues->sq_cb_return);
	bce_set_submission_single(s, qb->dma_addrs[qb->tail], qb->el_size);
	qb->tail = (qb->tail + 1) % qb->el_count;
	bce_submit_to_device(queues->sq_cb_return);

	pr_debug("apple-ave: Q2 echo submitted\n");
	return 0;
}

/*
 * Wait for Q1 response (cmd_completion). Must call ave_submit_q1_recv() first.
 */
int ave_wait_q1(struct ave_queues *queues, unsigned long timeout_ms)
{
	if (!wait_for_completion_timeout(&queues->cmd_completion,
					 msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return queues->cmd_status;
}

/*
 * Wait for one Q3 event. Can be called N times to wait for N events.
 * Each call consumes one complete() signal from ave_q3_completion().
 */
int ave_wait_q3(struct ave_queues *queues, unsigned long timeout_ms)
{
	if (!wait_for_completion_timeout(&queues->q3_completion,
					 msecs_to_jiffies(timeout_ms)))
		return -ETIMEDOUT;
	return 0;
}

/*
 * Access completed Q3 data for a given event index.
 * The buffer index corresponds to the ring slot that was completed.
 */
void *ave_q3_completed_data(struct ave_queues *queues, size_t event_index)
{
	struct ave_queue_buf *qb = &queues->q3_buf;
	size_t buf_idx;

	if (event_index >= queues->q3_event_count) {
		pr_err("apple-ave: Q3 event_index %zu >= event_count %zu\n",
		       event_index, queues->q3_event_count);
		return NULL;
	}

	/* Events are consumed in head order; the first event after reset
	 * was at head position when reset was called. We track the starting
	 * head in q3_reset_head for this purpose. But simpler: the ring
	 * head has already been advanced by the callback, so the data for
	 * event N is at (current_head - total_events + event_index). */
	buf_idx = (qb->head + qb->el_count - queues->q3_event_count + event_index) % qb->el_count;
	return qb->data[buf_idx];
}

size_t ave_q3_completed_size(struct ave_queues *queues, size_t event_index)
{
	if (event_index < AVE_OUTPUT_BUF_COUNT)
		return queues->q3_result[event_index];
	return 0;
}

/*
 * Reset Q3 tracking for a new frame. Must be called before submitting
 * Q3 buffers for a new encoding round.
 */
void ave_q3_reset(struct ave_queues *queues)
{
	queues->q3_event_count = 0;
	memset(queues->q3_result, 0, sizeof(queues->q3_result));
	reinit_completion(&queues->q3_completion);
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
		pr_err("apple-ave: failed to map frame data DMA (%zu bytes, err=%d)\n",
		       size, status);
		return status;
	}

	timeout = msecs_to_jiffies(AVE_SUBMIT_TIMEOUT_MS);
	status = bce_reserve_submission(queues->sq_submit, &timeout);
	if (status) {
		pr_err("apple-ave: Q0 reservation timeout for async frame data\n");
		bce_unmap_dma_buffer(&queues->bce->pci->dev, dma_out);
		return -ETIMEDOUT;
	}

	s = bce_next_submission(queues->sq_submit);
	bce_set_submission_buf(s, dma_out, 0, size);
	bce_submit_to_device(queues->sq_submit);
	return 0;
}

/* === Command buffer builders === */

void ave_build_cmd_codec_id(void *buf)
{
	memset(buf, 0xBB, AVE_CMD_BUF_SIZE);
	/* cmd_type = 0 at +0x00 */
	*(u32 *)(buf + 0x00) = 0x00000000;
	/* "1cvh" = FourCC "hvc1" little-endian at +0x08 */
	*(u32 *)(buf + 0x08) = 0x68766331; /* "hvc1" as LE bytes: 31 63 76 68 */
}

void ave_build_cmd_session_config(void *buf, u32 width, u32 height)
{
	memset(buf, 0x00, AVE_CMD_BUF_SIZE);

	/* Header — session token at +0x08 is stamped by caller */
	*(u32 *)(buf + 0x00) = AVE_CMD_SESSION_CONFIG;

	/* Resolution — only fields the T2 firmware actually reads */
	*(u32 *)(buf + 0x18) = width;
	*(u32 *)(buf + 0x1C) = height;
}

void ave_build_cmd_encode_frame(void *buf, u64 frame_num, u32 width, u32 height,
				u32 fps_num, u32 fps_den, bool keyframe,
				u64 cookie)
{
	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	*(u32 *)(buf + 0x00) = AVE_CMD_ENCODE_FRAME;
	/* +0x08: session token stamped by caller */
	*(u64 *)(buf + 0x10) = frame_num;
	*(u32 *)(buf + 0x18) = keyframe ? 1 : 0;
	*(u32 *)(buf + 0x20) = fps_num;
	*(u32 *)(buf + 0x24) = fps_den;
	*(u32 *)(buf + 0x30) = 1;
	*(u32 *)(buf + 0x38) = fps_num;
	*(u32 *)(buf + 0x3C) = fps_den;
	*(u32 *)(buf + 0x48) = width;
	*(u32 *)(buf + 0x50) = height;
	*(u32 *)(buf + 0x60) = 8; /* bit depth */
	*(u32 *)(buf + 0x68) = width; /* stride */
	/* "v024" = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ('420v').
	 * aveserverd also accepts 'x420' (10-bit video range) and
	 * 'xf20' (10-bit full range). */
	memcpy(buf + 0x70, "v024", 4);
	/* +0x74: dead data — aveserverd never reads this field.
	 * Previously hardcoded to 0x7000 from a packet capture. */
	*(u64 *)(buf + 0x80) = cookie;
}

void ave_build_cmd_copy_property(void *buf, const char *name)
{
	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	*(u32 *)(buf + 0x00) = AVE_CMD_COPY_PROPERTY;
	/* +0x08: session token stamped by caller */
	strscpy(buf + 0x10, name, AVE_CMD_BUF_SIZE - 0x10);
}

/*
 * SetProperty command layout (from T2 readCFKeyValuePair):
 *   +0x00: cmd_type (0x09)
 *   +0x08: session token (stamped by caller)
 *   +0x10: property name string
 *   +0x30: buffer size (0x1000)
 *   +0x64: value-present flag (1)
 *   +0x74: type code (1=bool, 2=SInt32)
 *   +0x78: value length in bytes
 *   +0x80: value data
 *
 * T2 firmware rejects all-zero buffers — must use 0xBB fill for
 * don't-care bytes, then zero only the header (finding 27/28).
 */
static void ave_build_cmd_set_property_common(void *buf, const char *name)
{
	memset(buf, 0xBB, AVE_CMD_BUF_SIZE);
	memset(buf, 0x00, 0x20);

	*(u32 *)(buf + 0x00) = AVE_CMD_SET_PROPERTY;
	strscpy(buf + 0x10, name, 0x20);

	*(u32 *)(buf + 0x30) = 0x1000;
	*(u32 *)(buf + 0x34) = 0x0000;
	*(u32 *)(buf + 0x64) = 0x00000001;
}

void ave_build_cmd_set_property_bool(void *buf, const char *name, bool value)
{
	ave_build_cmd_set_property_common(buf, name);
	*(u32 *)(buf + 0x74) = 0x00000001; /* type = bool */
	*(u32 *)(buf + 0x78) = 0x00000001; /* length = 1 byte */
	*(u8  *)(buf + 0x80) = value ? 1 : 0;
}

void ave_build_cmd_set_property_s32(void *buf, const char *name, s32 value)
{
	ave_build_cmd_set_property_common(buf, name);
	*(u32 *)(buf + 0x74) = 0x00000002; /* type = SInt32 */
	*(u32 *)(buf + 0x78) = 0x00000004; /* length = 4 bytes */
	*(s32 *)(buf + 0x80) = value;
}

void ave_build_cmd_set_property_float32(void *buf, const char *name, u32 ieee754_bits)
{
	ave_build_cmd_set_property_common(buf, name);
	*(u32 *)(buf + 0x74) = 0x00000004; /* type = Float32 */
	*(u32 *)(buf + 0x78) = 0x00000004; /* length = 4 bytes */
	*(u32 *)(buf + 0x80) = ieee754_bits;
}

void ave_build_cmd_set_property_string(void *buf, const char *name, const char *value)
{
	size_t len = strlen(value);

	ave_build_cmd_set_property_common(buf, name);
	*(u32 *)(buf + 0x74) = 0x00000006; /* type = CFString */
	*(u32 *)(buf + 0x78) = len;
	memcpy(buf + 0x80, value, min_t(size_t, len + 1, AVE_CMD_BUF_SIZE - 0x80));
}

void ave_build_cmd_prepare(void *buf)
{
	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	*(u32 *)(buf + 0x00) = AVE_CMD_PREPARE;
	/* +0x08: session token stamped by caller */
}

void ave_build_cmd_complete_frames(void *buf)
{
	memset(buf, 0x00, AVE_CMD_BUF_SIZE);
	*(u32 *)(buf + 0x00) = AVE_CMD_COMPLETE_FRAMES;
	/* +0x08: session token stamped by caller */
}

void ave_build_cmd_end_session(void *buf)
{
	memset(buf, 0xFF, AVE_CMD_BUF_SIZE);
	*(u32 *)(buf + 0x00) = AVE_CMD_END_SESSION;
	*(u32 *)(buf + 0x04) = 0x00000000;
	/* +0x08: session token stamped by caller */
	*(u32 *)(buf + 0x0C) = 0x00000000;
}
