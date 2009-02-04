/*
 * lockhdeq.c: simple lock-based parallel hashed deq implementation.
 * 	This is similar to lockdeq.c, but expresses the parallel
 * 	implementation in terms of a simple single-lock-based deq.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (c) 2009 Paul E. McKenney, IBM Corporation.
 */

#include "../api.h"

/* First do the underlying single-locked deq implementation. */

struct deq {
	spinlock_t lock;
	struct list_head chain;
} ____cacheline_internodealigned_in_smp;

void init_deq(struct deq *p)
{
	spin_lock_init(&p->lock);
	INIT_LIST_HEAD(&p->chain);
}

struct list_head *deq_dequeue_l(struct deq *p)
{
	struct list_head *e;

	spin_lock(&p->lock);
	if (list_empty(&p->chain))
		e = NULL;
	else {
		e = p->chain.prev;
		list_del_init(e);
	}
	spin_unlock(&p->lock);
	return e;
}

void deq_enqueue_l(struct list_head *e, struct deq *p)
{
	spin_lock(&p->lock);
	list_add_tail(e, &p->chain);
	spin_unlock(&p->lock);
}

struct list_head *deq_dequeue_r(struct deq *p)
{
	struct list_head *e;

	spin_lock(&p->lock);
	if (list_empty(&p->chain))
		e = NULL;
	else {
		e = p->chain.next;
		list_del_init(e);
	}
	spin_unlock(&p->lock);
	return e;
}

void deq_enqueue_r(struct list_head *e, struct deq *p)
{
	spin_lock(&p->lock);
	list_add(e, &p->chain);
	spin_unlock(&p->lock);
}

/*
 * And now the concurrent implementation.
 *
 * Pdeq structure, empty list:
 *
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *     |   |   |   |   |   |   |   |   |   |   |   |   |   |   |   |
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *                               ^   ^
 *                               |   |
 *                            lidx   ridx
 *
 *
 * List after three pdeq_enqueue_l() invocations of "a", "b", and "c":
 *
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *     |   |   |   |   | c | b | a |   |   |   |   |   |   |   |   |
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *                   ^               ^
 *                   |               |
 *                lidx               ridx
 *
 * List after one pdeq_dequeue_r() invocations (removing "a"):
 *
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *     |   |   |   |   | c | b |   |   |   |   |   |   |   |   |   |
 *     +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *                   ^           ^
 *                   |           |
 *                lidx           ridx
 *
 * This is pretty standard.  The trick is that only the low-order bits
 * of lidx and ridx are used to index into a power-of-two sized hash
 * table.  Each bucket of the hash table is a circular doubly linked
 * list (AKA Linux-kernel list_head structure).  Left-hand operations
 * manipulate the tail of the selected list, while right-hand operations
 * manipulate the head of the selected list.  Each bucket has its own
 * lock, minimizing lock contention.  Each of the two indexes also has
 * its own lock.
 */

/*
 * This must be a power of two.  If you want something else, also adjust
 * the moveleft() and moveright() functions.
 */

#define PDEQ_N_BKTS 4

struct pdeq {
	spinlock_t llock;
	int lidx;
	/* char pad1[CACHE_LINE_SIZE - sizeof(spinlock_t) - sizeof(int)]; */
	spinlock_t rlock ____cacheline_internodealigned_in_smp;
	int ridx;
	/* char pad2[CACHE_LINE_SIZE - sizeof(spinlock_t) - sizeof(int)]; */
	struct deq bkt[PDEQ_N_BKTS];
};

static int moveleft(int idx)
{
	return (idx - 1) & (PDEQ_N_BKTS - 1);
}

static int moveright(int idx)
{
	return (idx + 1) & (PDEQ_N_BKTS - 1);
}

void init_pdeq(struct pdeq *d)
{
	int i;

	d->lidx = 0;
	spin_lock_init(&d->llock);
	d->ridx = 1;
	spin_lock_init(&d->rlock);
	for (i = 0; i < PDEQ_N_BKTS; i++)
		init_deq(&d->bkt[i]);
}

struct list_head *pdeq_dequeue_l(struct pdeq *d)
{
	struct list_head *e;
	int i;

	spin_lock(&d->llock);
	i = moveright(d->lidx);
	e = deq_dequeue_l(&d->bkt[i]);
	if (e != NULL)
		d->lidx = i;
	spin_unlock(&d->llock);
	return e;
}

struct list_head *pdeq_dequeue_r(struct pdeq *d)
{
	struct list_head *e;
	int i;

	spin_lock(&d->rlock);
	i = moveleft(d->ridx);
	e = deq_dequeue_r(&d->bkt[i]);
	if (e != NULL)
		d->ridx = i;
	spin_unlock(&d->rlock);
	return e;
}

void pdeq_enqueue_l(struct list_head *e, struct pdeq *d)
{
	int i;

	spin_lock(&d->llock);
	i = d->lidx;
	deq_enqueue_l(e, &d->bkt[i]);
	d->lidx = moveleft(d->lidx);
	spin_unlock(&d->llock);
}

void pdeq_enqueue_r(struct list_head *e, struct pdeq *d)
{
	int i;

	spin_lock(&d->rlock);
	i = d->ridx;
	deq_enqueue_r(e, &d->bkt[i]);
	d->ridx = moveright(d->ridx);
	spin_unlock(&d->rlock);
}

#ifdef TEST
#define DEQ_AND_PDEQ
#include "deqtorture.h"
#endif /* #ifdef TEST */