// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "encore_fas_evq.h"

#define FAS_EVQ_SIZE 512
#define FAS_EVQ_MASK (FAS_EVQ_SIZE - 1)
#define FAS_READ_BATCH 8

static struct {
	raw_spinlock_t lock;
	struct mutex read_lock;
	wait_queue_head_t wq;
	u32 head;
	u32 tail;
	u32 dropped;
	struct fas_event buf[FAS_EVQ_SIZE];
} evq;

/**
 * @brief Initializes event queue data structures.
 */
void fas_evq_init(void)
{
	BUILD_BUG_ON(sizeof(struct fas_event) != 48);
	BUILD_BUG_ON(FAS_EVQ_SIZE & FAS_EVQ_MASK);

	raw_spin_lock_init(&evq.lock);
	mutex_init(&evq.read_lock);
	init_waitqueue_head(&evq.wq);
	evq.head = 0;
	evq.tail = 0;
	evq.dropped = 0;
}

/**
 * @brief Copies pending events without removal from queue.
 *
 * @param out Output buffer for copied events.
 * @param max Maximum events to copy.
 * @return Count of events copied.
 */
static u32 fas_evq_peek(struct fas_event *out, u32 max)
{
	unsigned long flags;
	u32 n, i;

	raw_spin_lock_irqsave(&evq.lock, flags);
	n = min_t(u32, evq.head - evq.tail, max);
	for (i = 0; i < n; i++)
		out[i] = evq.buf[(evq.tail + i) & FAS_EVQ_MASK];
	raw_spin_unlock_irqrestore(&evq.lock, flags);

	return n;
}

/**
 * @brief Removes read events from queue.
 *
 * @param n Count of events to remove.
 */
static void fas_evq_release(u32 n)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&evq.lock, flags);
	/* Clamp event count to available queue size. */
	evq.tail += min_t(u32, n, evq.head - evq.tail);
	raw_spin_unlock_irqrestore(&evq.lock, flags);
}

/**
 * @brief Adds event to queue.
 *
 * @param ev Event structure to add.
 */
void fas_evq_push(const struct fas_event *ev)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&evq.lock, flags);
	if (unlikely(evq.head - evq.tail == FAS_EVQ_SIZE)) {
		evq.tail++;
		evq.dropped++;
	}
	evq.buf[evq.head & FAS_EVQ_MASK] = *ev;
	evq.head++;
	raw_spin_unlock_irqrestore(&evq.lock, flags);
}

/**
 * @brief Wakes processes waiting to read events.
 */
void fas_evq_wake(void)
{
	wake_up_interruptible(&evq.wq);
}

/**
 * @brief Gets count of dropped events.
 *
 * @return Total count of dropped events.
 */
u32 fas_evq_dropped(void)
{
	return READ_ONCE(evq.dropped);
}

static bool fas_evq_nonempty(void)
{
	return READ_ONCE(evq.head) != READ_ONCE(evq.tail);
}

/**
 * @brief Implements character device read operation.
 *
 * @param file File structure pointer.
 * @param buf User buffer pointer.
 * @param count User buffer size in bytes.
 * @param ppos File position offset (unused).
 * @return Bytes copied on success, or negative error code.
 */
ssize_t fas_evq_read(struct file *file, char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct fas_event batch[FAS_READ_BATCH];
	u32 n;
	ssize_t ret;

	if (count < sizeof(struct fas_event))
		return -EINVAL;

	if (mutex_lock_interruptible(&evq.read_lock))
		return -ERESTARTSYS;

	for (;;) {
		n = fas_evq_peek(batch,
				 min_t(size_t, count / sizeof(*batch), FAS_READ_BATCH));
		if (n)
			break;

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(evq.wq, fas_evq_nonempty())) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	if (copy_to_user(buf, batch, n * sizeof(*batch))) {
		ret = -EFAULT;   /* Retain events in queue if copy fails. */
		goto out;
	}

	fas_evq_release(n);
	ret = n * sizeof(*batch);

out:
	mutex_unlock(&evq.read_lock);
	return ret;
}

/**
 * @brief Implements poll operation for read readiness.
 *
 * @param file File structure pointer.
 * @param wait Poll table pointer.
 * @return Poll event mask indicating read availability.
 */
fas_poll_t fas_evq_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &evq.wq, wait);
	return fas_evq_nonempty() ? FAS_POLLIN : 0;
}
