/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Scooter Morris at Genentech Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)smbfs_lockf.c
 *	derived from @(#)ufs_lockf.c	8.4 (Berkeley) 10/26/94
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>

#include "smbfs_node.h"
#include "smbfs_lockf.h"

/*
 * This variable controls the maximum number of processes that will
 * be checked in doing deadlock detection.
 */
int smbfsmaxlockdepth = MAXDEPTH;

#ifdef LOCKF_DEBUG
int	lockf_debug = 1;
#endif

#define NOLOCKF (struct smbfs_lockf *)0
#define SELF	0x1
#define OTHERS	0x2

/*
 * Set a byte-range lock.
 */
int
smbfs_setlock(lock)
	register struct smbfs_lockf *lock;
{
	register struct smbfs_lockf *block;
	struct smbnode *smbp = lock->lf_smbnode;
	struct smbfs_lockf **prev, *overlap, *ltmp;
	static char lockstr[] = "smbfs_lockf";
	int ovcase, priority, needtolink, error;

#ifdef LOCKF_DEBUG
	if (lockf_debug & 1)
		smbfs_lprint("smbfs_setlock", lock);
#endif /* LOCKF_DEBUG */

	/*
	 * Set the priority
	 */
	priority = PLOCK;
	if (lock->lf_type == F_WRLCK)
		priority += 4;
	priority |= PCATCH;
	/*
	 * Scan lock list for this file looking for locks that would block us.
	 */
	while ((block = smbfs_getblock(lock))) {
		/*
		 * Free the structure and return if nonblocking.
		 */
		if ((lock->lf_flags & F_WAIT) == 0) {
			FREE(lock, M_LOCKF);
			return (EAGAIN);
		}
		/*
		 * We are blocked. Since flock style locks cover
		 * the whole file, there is no chance for deadlock.
		 * For byte-range locks we must check for deadlock.
		 *
		 * Deadlock detection is done by looking through the
		 * wait channels to see if there are any cycles that
		 * involve us. MAXDEPTH is set just to make sure we
		 * do not go off into neverland.
		 */
		if ((lock->lf_flags & F_POSIX) &&
		    (block->lf_flags & F_POSIX)) {
			register struct proc *wproc;
			register struct smbfs_lockf *waitblock;
			int i = 0;

			/* The block is waiting on something */
			wproc = (struct proc *)block->lf_id;
			while (wproc->p_wchan &&
			       (wproc->p_wmesg == lockstr) &&
			       (i++ < smbfsmaxlockdepth)) {
				waitblock = (struct smbfs_lockf *)wproc->p_wchan;
				/* Get the owner of the blocking lock */
				waitblock = waitblock->lf_next;
				if ((waitblock->lf_flags & F_POSIX) == 0)
					break;
				wproc = (struct proc *)waitblock->lf_id;
				if (wproc == (struct proc *)lock->lf_id) {
					_FREE(lock, M_LOCKF);
					return (EDEADLK);
				}
			}
		}
		/*
		 * For flock type locks, we must first remove
		 * any shared locks that we hold before we sleep
		 * waiting for an exclusive lock.
		 */
		if ((lock->lf_flags & F_FLOCK) &&
		    lock->lf_type == F_WRLCK) {
			lock->lf_type = F_UNLCK;
			(void) smbfs_clearlock(lock);
			lock->lf_type = F_WRLCK;
		}
		/*
		 * Add our lock to the blocked list and sleep until we're free.
		 * Remember who blocked us (for deadlock detection).
		 */
		lock->lf_next = block;
		TAILQ_INSERT_TAIL(&block->lf_blkhd, lock, lf_block);
#ifdef LOCKF_DEBUG
		if (lockf_debug & 1) {
			smbfs_lprint("smbfs_setlock: blocking on", block);
			smbfs_lprintlist("smbfs_setlock", block);
		}
#endif /* LOCKF_DEBUG */
		if ((error = tsleep((caddr_t)lock, priority, lockstr, 0))) {
			/*
			 * We may have been awakened by a signal (in
			 * which case we must remove ourselves from the
			 * blocked list) and/or by another process
			 * releasing a lock (in which case we have already
			 * been removed from the blocked list and our
			 * lf_next field set to NOLOCKF).
			 */
			if (lock->lf_next)
				TAILQ_REMOVE(&lock->lf_next->lf_blkhd, lock,
				    lf_block);
			_FREE(lock, M_LOCKF);
			return (error);
		}
	}
	/*
	 * No blocks!!  Add the lock.  Note that we will
	 * downgrade or upgrade any overlapping locks this
	 * process already owns.
	 *
	 * Skip over locks owned by other processes.
	 * Handle any locks that overlap and are owned by ourselves.
	 */
	prev = &smbp->smb_lockf;
	block = smbp->smb_lockf;
	needtolink = 1;
	for (;;) {
		if ((ovcase = smbfs_findoverlap(block, lock, SELF, &prev, &overlap)))
			block = overlap->lf_next;
		/*
		 * Six cases:
		 *	0) no overlap
		 *	1) overlap == lock
		 *	2) overlap contains lock
		 *	3) lock contains overlap
		 *	4) overlap starts before lock
		 *	5) overlap ends after lock
		 */
		switch (ovcase) {
		case 0: /* no overlap */
			if (needtolink) {
				*prev = lock;
				lock->lf_next = overlap;
			}
			break;

		case 1: /* overlap == lock */
			/*
			 * If downgrading lock, others may be
			 * able to acquire it.
			 */
			if (lock->lf_type == F_RDLCK &&
			    overlap->lf_type == F_WRLCK)
				smbfs_wakelock(overlap);
			overlap->lf_type = lock->lf_type;
			FREE(lock, M_LOCKF);
			lock = overlap; /* for debug output below */
			break;

		case 2: /* overlap contains lock */
			/*
			 * Check for common starting point and different types.
			 */
			if (overlap->lf_type == lock->lf_type) {
				_FREE(lock, M_LOCKF);
				lock = overlap; /* for debug output below */
				break;
			}
			if (overlap->lf_start == lock->lf_start) {
				*prev = lock;
				lock->lf_next = overlap;
				overlap->lf_start = lock->lf_end + 1;
			} else
				smbfs_split(overlap, lock);
			smbfs_wakelock(overlap);
			break;

		case 3: /* lock contains overlap */
			/*
			 * If downgrading lock, others may be able to
			 * acquire it, otherwise take the list.
			 */
			if (lock->lf_type == F_RDLCK &&
			    overlap->lf_type == F_WRLCK) {
				smbfs_wakelock(overlap);
			} else {
				while ((ltmp = overlap->lf_blkhd.tqh_first)) {
					TAILQ_REMOVE(&overlap->lf_blkhd, ltmp,
					    lf_block);
					TAILQ_INSERT_TAIL(&lock->lf_blkhd,
					    ltmp, lf_block);
				}
			}
			/*
			 * Add the new lock if necessary and delete the overlap.
			 */
			if (needtolink) {
				*prev = lock;
				lock->lf_next = overlap->lf_next;
				prev = &lock->lf_next;
				needtolink = 0;
			} else
				*prev = overlap->lf_next;
			_FREE(overlap, M_LOCKF);
			continue;

		case 4: /* overlap starts before lock */
			/*
			 * Add lock after overlap on the list.
			 */
			lock->lf_next = overlap->lf_next;
			overlap->lf_next = lock;
			overlap->lf_end = lock->lf_start - 1;
			prev = &lock->lf_next;
			smbfs_wakelock(overlap);
			needtolink = 0;
			continue;

		case 5: /* overlap ends after lock */
			/*
			 * Add the new lock before overlap.
			 */
			if (needtolink) {
				*prev = lock;
				lock->lf_next = overlap;
			}
			overlap->lf_start = lock->lf_end + 1;
			smbfs_wakelock(overlap);
			break;
		}
		break;
	}
#ifdef LOCKF_DEBUG
	if (lockf_debug & 1) {
		smbfs_lprint("smbfs_setlock: got the lock", lock);
		smbfs_lprintlist("smbfs_setlock", lock);
	}
#endif /* LOCKF_DEBUG */
	return (0);
}

/*
 * Remove a byte-range lock on a smbnode.
 *
 * Generally, find the lock (or an overlap to that lock)
 * and remove it (or shrink it), then wakeup anyone we can.
 */
int
smbfs_clearlock(unlock)
	register struct smbfs_lockf *unlock;
{
	struct smbnode *smbp = unlock->lf_smbnode;
	register struct smbfs_lockf *lf = smbp->smb_lockf;
	struct smbfs_lockf *overlap, **prev;
	int ovcase;

	if (lf == NOLOCKF)
		return (0);
#ifdef LOCKF_DEBUG
	if (unlock->lf_type != F_UNLCK)
		panic("smbfs_clearlock: bad type");
	if (lockf_debug & 1)
		smbfs_lprint("smbfs_clearlock", unlock);
#endif /* LOCKF_DEBUG */
	prev = &smbp->smb_lockf;
	while ((ovcase = smbfs_findoverlap(lf, unlock, SELF, &prev, &overlap))) {
		/*
		 * Wakeup the list of locks to be retried.
		 */
		smbfs_wakelock(overlap);

		switch (ovcase) {

		case 1: /* overlap == lock */
			*prev = overlap->lf_next;
			FREE(overlap, M_LOCKF);
			break;

		case 2: /* overlap contains lock: split it */
			if (overlap->lf_start == unlock->lf_start) {
				overlap->lf_start = unlock->lf_end + 1;
				break;
			}
			smbfs_split(overlap, unlock);
			overlap->lf_next = unlock->lf_next;
			break;

		case 3: /* lock contains overlap */
			*prev = overlap->lf_next;
			lf = overlap->lf_next;
			_FREE(overlap, M_LOCKF);
			continue;

		case 4: /* overlap starts before lock */
			overlap->lf_end = unlock->lf_start - 1;
			prev = &overlap->lf_next;
			lf = overlap->lf_next;
			continue;

		case 5: /* overlap ends after lock */
			overlap->lf_start = unlock->lf_end + 1;
			break;
		}
		break;
	}
#ifdef LOCKF_DEBUG
	if (lockf_debug & 1)
		smbfs_lprintlist("smbfs_clearlock", unlock);
#endif /* LOCKF_DEBUG */
	return (0);
}

/*
 * Check whether there is a blocking lock,
 * and if so return its process identifier.
 */
int
smbfs_getlock(lock, fl)
	register struct smbfs_lockf *lock;
	register struct flock *fl;
{
	register struct smbfs_lockf *block;

#ifdef LOCKF_DEBUG
	if (lockf_debug & 1)
		smbfs_lprint("smbfs_getlock", lock);
#endif /* LOCKF_DEBUG */

	if ((block = smbfs_getblock(lock))) {
		fl->l_type = block->lf_type;
		fl->l_whence = SEEK_SET;
		fl->l_start = block->lf_start;
		if (block->lf_end == -1)
			fl->l_len = 0;
		else
			fl->l_len = block->lf_end - block->lf_start + 1;
		if (block->lf_flags & F_POSIX)
			fl->l_pid = ((struct proc *)(block->lf_id))->p_pid;
		else
			fl->l_pid = -1;
	} else {
		fl->l_type = F_UNLCK;
	}
	return (0);
}

/*
 * Walk the list of locks for a smbnode and
 * return the first blocking lock.
 */
struct smbfs_lockf *
smbfs_getblock(lock)
	register struct smbfs_lockf *lock;
{
	struct smbfs_lockf **prev, *overlap, *lf = lock->lf_smbnode->smb_lockf;
	int ovcase;

	prev = &lock->lf_smbnode->smb_lockf;
	while ((ovcase = smbfs_findoverlap(lf, lock, OTHERS, &prev, &overlap))) {
		/*
		 * We've found an overlap, see if it blocks us
		 */
		if ((lock->lf_type == F_WRLCK || overlap->lf_type == F_WRLCK))
			return (overlap);
		/*
		 * Nope, point to the next one on the list and
		 * see if it blocks us
		 */
		lf = overlap->lf_next;
	}
	return (NOLOCKF);
}

/*
 * Walk the list of locks for a smbnode to
 * find an overlapping lock (if any).
 *
 * NOTE: this returns only the FIRST overlapping lock.  There
 *	 may be more than one.
 */
int
smbfs_findoverlap(lf, lock, type, prev, overlap)
	register struct smbfs_lockf *lf;
	struct smbfs_lockf *lock;
	int type;
	struct smbfs_lockf ***prev;
	struct smbfs_lockf **overlap;
{
	off_t start, end;

	*overlap = lf;
	if (lf == NOLOCKF)
		return (0);
#ifdef LOCKF_DEBUG
	if (lockf_debug & 2)
		smbfs_lprint("smbfs_findoverlap: looking for overlap in", lock);
#endif /* LOCKF_DEBUG */
	start = lock->lf_start;
	end = lock->lf_end;
	while (lf != NOLOCKF) {
		if (((type & SELF) && lf->lf_id != lock->lf_id) ||
		    ((type & OTHERS) && lf->lf_id == lock->lf_id)) {
			*prev = &lf->lf_next;
			*overlap = lf = lf->lf_next;
			continue;
		}
#ifdef LOCKF_DEBUG
		if (lockf_debug & 2)
			smbfs_lprint("\tchecking", lf);
#endif /* LOCKF_DEBUG */
		/*
		 * OK, check for overlap
		 *
		 * Six cases:
		 *	0) no overlap
		 *	1) overlap == lock
		 *	2) overlap contains lock
		 *	3) lock contains overlap
		 *	4) overlap starts before lock
		 *	5) overlap ends after lock
		 */
		if ((lf->lf_end != -1 && start > lf->lf_end) ||
		    (end != -1 && lf->lf_start > end)) {
			/* Case 0 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("no overlap\n");
#endif /* LOCKF_DEBUG */
			if ((type & SELF) && end != -1 && lf->lf_start > end)
				return (0);
			*prev = &lf->lf_next;
			*overlap = lf = lf->lf_next;
			continue;
		}
		if ((lf->lf_start == start) && (lf->lf_end == end)) {
			/* Case 1 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("overlap == lock\n");
#endif /* LOCKF_DEBUG */
			return (1);
		}
		if ((lf->lf_start <= start) &&
		    (end != -1) &&
		    ((lf->lf_end >= end) || (lf->lf_end == -1))) {
			/* Case 2 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("overlap contains lock\n");
#endif /* LOCKF_DEBUG */
			return (2);
		}
		if (start <= lf->lf_start &&
		           (end == -1 ||
			   (lf->lf_end != -1 && end >= lf->lf_end))) {
			/* Case 3 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("lock contains overlap\n");
#endif /* LOCKF_DEBUG */
			return (3);
		}
		if ((lf->lf_start < start) &&
			((lf->lf_end >= start) || (lf->lf_end == -1))) {
			/* Case 4 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("overlap starts before lock\n");
#endif /* LOCKF_DEBUG */
			return (4);
		}
		if ((lf->lf_start > start) &&
			(end != -1) &&
			((lf->lf_end > end) || (lf->lf_end == -1))) {
			/* Case 5 */
#ifdef LOCKF_DEBUG
			if (lockf_debug & 2)
				printf("overlap ends after lock\n");
#endif /* LOCKF_DEBUG */
			return (5);
		}
		panic("smbfs_findoverlap: default");
	}
	return (0);
}

/*
 * Split a lock and a contained region into
 * two or three locks as necessary.
 */
void
smbfs_split(lock1, lock2)
	register struct smbfs_lockf *lock1;
	register struct smbfs_lockf *lock2;
{
	register struct smbfs_lockf *splitlock;

#ifdef LOCKF_DEBUG
	if (lockf_debug & 2) {
		smbfs_lprint("smbfs_split", lock1);
		smbfs_lprint("splitting from", lock2);
	}
#endif /* LOCKF_DEBUG */
	/*
	 * Check to see if spliting into only two pieces.
	 */
	if (lock1->lf_start == lock2->lf_start) {
		lock1->lf_start = lock2->lf_end + 1;
		lock2->lf_next = lock1;
		return;
	}
	if (lock1->lf_end == lock2->lf_end) {
		lock1->lf_end = lock2->lf_start - 1;
		lock2->lf_next = lock1->lf_next;
		lock1->lf_next = lock2;
		return;
	}
	/*
	 * Make a new lock consisting of the last part of
	 * the encompassing lock
	 */
	MALLOC(splitlock, struct smbfs_lockf *, sizeof *splitlock, M_LOCKF, M_WAITOK);
	bcopy((caddr_t)lock1, (caddr_t)splitlock, sizeof *splitlock);
	splitlock->lf_start = lock2->lf_end + 1;
	TAILQ_INIT(&splitlock->lf_blkhd);
	lock1->lf_end = lock2->lf_start - 1;
	/*
	 * OK, now link it in
	 */
	splitlock->lf_next = lock1->lf_next;
	lock2->lf_next = splitlock;
	lock1->lf_next = lock2;
}

/*
 * Wakeup a blocklist
 */
void
smbfs_wakelock(listhead)
	struct smbfs_lockf *listhead;
{
	register struct smbfs_lockf *wakelock;

	while ((wakelock = listhead->lf_blkhd.tqh_first)) {
		TAILQ_REMOVE(&listhead->lf_blkhd, wakelock, lf_block);
		wakelock->lf_next = NOLOCKF;
#ifdef LOCKF_DEBUG
		if (lockf_debug & 2)
			smbfs_lprint("smbfs_wakelock: awakening", wakelock);
#endif /* LOCKF_DEBUG */
		wakeup((caddr_t)wakelock);
	}
}

#ifdef LOCKF_DEBUG
/*
 * Print out a lock.
 */
void smbfs_lprint(tag, lock)
	char *tag;
	register struct smbfs_lockf *lock;
{
	printf("%s: lock 0x%lx for ", tag, (unsigned long)lock);
	if (lock->lf_flags & F_POSIX)
		printf("proc %d", ((struct proc *)(lock->lf_id))->p_pid);
	else
		printf("id 0x%lx", (unsigned long)lock->lf_id);
	printf(" in ino %ld on mount 0x%lx, %s, start 0x%lx, end 0x%lx",
		lock->lf_smbnode->n_ino,
		(unsigned long)lock->lf_smbnode->n_mount,
		lock->lf_type == F_RDLCK ? "shared" :
		lock->lf_type == F_WRLCK ? "exclusive" :
		lock->lf_type == F_UNLCK ? "unlock" :
		"unknown", (unsigned long)lock->lf_start, (unsigned long)lock->lf_end);
	if (lock->lf_blkhd.tqh_first)
		printf(" block 0x%lx\n", (unsigned long)lock->lf_blkhd.tqh_first);
	else
		printf("\n");
}

void smbfs_lprintlist(tag, lock)
	char *tag;
	struct smbfs_lockf *lock;
{
	register struct smbfs_lockf *lf, *blk;

	printf("%s: Lock list for ino %ld on on mount 0x%lx:\n",
		tag, lock->lf_smbnode->n_ino,
		(unsigned long)lock->lf_smbnode->n_mount);
	for (lf = lock->lf_smbnode->smb_lockf; lf; lf = lf->lf_next) {
		printf("\tlock 0x%lx for ", (unsigned long)lf);
		if (lf->lf_flags & F_POSIX)
			printf("proc %d", ((struct proc *)(lf->lf_id))->p_pid);
		else
			printf("id 0x%lx", (unsigned long)lf->lf_id);
		printf(", %s, start 0x%lx, end 0x%lx",
			lf->lf_type == F_RDLCK ? "shared" :
			lf->lf_type == F_WRLCK ? "exclusive" :
			lf->lf_type == F_UNLCK ? "unlock" :
			"unknown", (unsigned long)lf->lf_start, (unsigned long)lf->lf_end);
		for (blk = lf->lf_blkhd.tqh_first; blk;
		     blk = blk->lf_block.tqe_next) {
			printf("\n\t\tlock request 0x%lx for ", (unsigned long)blk);
			if (blk->lf_flags & F_POSIX)
				printf("proc %d",
				    ((struct proc *)(blk->lf_id))->p_pid);
			else
				printf("id 0x%lx", (unsigned long)blk->lf_id);
			printf(", %s, start 0x%lx, end 0x%lx",
				blk->lf_type == F_RDLCK ? "shared" :
				blk->lf_type == F_WRLCK ? "exclusive" :
				blk->lf_type == F_UNLCK ? "unlock" :
				"unknown", (unsigned long)blk->lf_start,(unsigned long) blk->lf_end);
			if (blk->lf_blkhd.tqh_first)
				panic("smbfs_lprintlist: bad list");
		}
		printf("\n");
	}
}
#endif /* LOCKF_DEBUG */