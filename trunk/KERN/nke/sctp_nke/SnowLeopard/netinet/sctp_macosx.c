/*-
 * Copyright (C) 2002-2007 Cisco Systems Inc,
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
 *
 * @APPLE_LICENSE_OSREFERENCE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License.  The rights granted to you under the
 * License may not be used to create, or enable the creation or
 * redistribution of, unlawful or unlicensed copies of an Apple operating
 * system, or to circumvent, violate, or enable the circumvention or
 * violation of, any terms of an Apple operating system software license
 * agreement.
 *
 * Please obtain a copy of the License at
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
 * @APPLE_LICENSE_OSREFERENCE_HEADER_END@
 */


#include <sys/param.h>
#include <sys/domain.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/systm.h>
#include <net/route.h>
#include <netinet/ip.h>
#include <net/if_dl.h>
#ifdef INET6
#include <netinet/ip6.h>
#endif
#include <netinet/in_pcb.h>
#include <netinet/sctp.h>
#include <netinet/sctp_os.h>
#include <netinet/sctp_pcb.h>
#include <netinet/sctp_var.h>
#include <netinet/sctputil.h>
#include <netinet/sctp_peeloff.h>
#include <netinet/sctp_bsd_addr.h>
#include <net/kpi_interface.h>
#define APPLE_FILE_NO 5

/* sctp_peeloff() support via socket option */
#if defined(HAVE_SCTP_PEELOFF_SOCKOPT)
#include <sys/file.h>
#include <sys/filedesc.h>

extern struct fileops socketops;

#include <sys/proc_internal.h>
#include <sys/file_internal.h>
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
#if defined(APPLE_LEOPARD)
#define CONFIG_MACF_SOCKET_SUBSET 1
#endif
#if defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
#define CONFIG_MACF 1
#endif
#include <sys/namei.h>
#include <sys/vnode_internal.h>
#if CONFIG_MACF_SOCKET_SUBSET
#include <security/mac_framework.h>
#endif /* MAC_SOCKET_SUBSET */
#endif
#endif /* HAVE_SCTP_PEELOFF_SOCKOPT */

#ifdef SCTP_DEBUG
extern uint32_t sctp_debug_on;
#endif /* SCTP_DEBUG */

#if defined(HAVE_SCTP_PEELOFF_SOCKOPT)

/*
 * NOTE!! sctp_peeloff_option() MUST be kept in sync with the Apple accept()
 * call.
 */

#define f_flag f_fglob->fg_flag
#define f_type f_fglob->fg_type
#define f_msgcount f_fglob->fg_msgcount
#define f_cred f_fglob->fg_cred
#define f_ops f_fglob->fg_ops
#define f_offset f_fglob->fg_offset
#define f_data f_fglob->fg_data

int
sctp_peeloff_option(struct proc *p, struct sctp_peeloff_opt *uap)
{
	struct fileproc *fp;
	int error;
	struct socket *head, *so = NULL;
	int fd = uap->s;
	int newfd;
	short fflag;		/* type must match fp->f_flag */
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
	/* workaround sonewconn() issue where qlimits are checked.
	   i.e. sonewconn() can only be done on listening sockets */
	int old_qlimit;
#endif
	/* AUDIT_ARG(fd, uap->s); */
	error = fp_getfsock(p, fd, &fp, &head);
	if (error) {
		if (error == EOPNOTSUPP)
			error = ENOTSOCK;
		goto out;
	}
	if (head == NULL) {
		error = EBADF;
		goto out;
	}
#if (defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD)  || defined(APPLE_LION)) && CONFIG_MACF_SOCKET_SUBSET
	if ((error = mac_socket_check_accept(kauth_cred_get(), head)) != 0)
		goto out;
#endif /* MAC_SOCKET_SUBSET */

	error = sctp_can_peel_off(head, uap->assoc_id);
	if (error) {
		goto out;
	}

        socket_unlock(head, 0); /* unlock head to avoid deadlock with select, keep a ref on head */

	fflag = fp->f_flag;
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
	error = falloc(p, &fp, &newfd, vfs_context_current());
#else
	proc_fdlock(p);
	error = falloc_locked(p, &fp, &newfd, 1);
#endif
	if (error) {
		/*
		 * Probably ran out of file descriptors. Put the
		 * unaccepted connection back onto the queue and
		 * do another wakeup so some other process might
		 * have a chance at it.
		 */
		/* SCTP will NOT put the connection back onto queue */
#if !defined(APPLE_LEOPARD) && !defined(APPLE_SNOWLEOPARD) && !defined(APPLE_LION)
		proc_fdunlock(p);
#endif
		socket_lock(head, 0);
		goto out;
	}
#if !defined(APPLE_LEOPARD) && !defined(APPLE_SNOWLEOPARD) && !defined(APPLE_LION)
	*fdflags(p, newfd) &= ~UF_RESERVED;
#endif
	uap->new_sd = newfd;	/* return the new descriptor to the caller */

	/* sctp_get_peeloff() does sonewconn() which expects head to be locked */
	socket_lock(head, 0);
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
	old_qlimit = head->so_qlimit;	/* work around sonewconn() needing listen */
	head->so_qlimit = 1;
#endif
	so = sctp_get_peeloff(head, uap->assoc_id, &error);
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
	head->so_qlimit = old_qlimit;
#endif
	if (so == NULL) {
#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
		goto release_fd;
#else
		goto out;
#endif
	}
	socket_unlock(head, 0);

#if (defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)) && CONFIG_MACF_SOCKET_SUBSET
	/*
	 * Pass the pre-accepted socket to the MAC framework. This is
	 * cheaper than allocating a file descriptor for the socket,
	 * calling the protocol accept callback, and possibly freeing
	 * the file descriptor should the MAC check fails.
	 */
	if ((error = mac_socket_check_accepted(kauth_cred_get(), so)) != 0) {
		so->so_state &= ~(SS_NOFDREF | SS_COMP);
		so->so_head = NULL;
		soclose(so);
		/* Drop reference on listening socket */
		socket_lock(head, 0);
		goto out;
	}
#endif /* MAC_SOCKET_SUBSET */

	fp->f_type = DTYPE_SOCKET;
	fp->f_flag = fflag;
	fp->f_ops = &socketops;
	fp->f_data = (caddr_t)so;
#if !defined(APPLE_LEOPARD) && !defined(APPLE_SNOWLEOPARD) && !defined(APPLE_LION)
	fp_drop(p, newfd, fp, 1);
	proc_fdunlock(p);
#endif
	socket_lock(head, 0);
	/* sctp_get_peeloff() returns a new locked socket */
        so->so_state &= ~SS_COMP;
        so->so_state &= ~SS_NOFDREF;
        so->so_head = NULL;
	socket_unlock(so, 1);

#if defined(APPLE_LEOPARD) || defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
release_fd:
	proc_fdlock(p);
	procfdtbl_releasefd(p, newfd, NULL);
	fp_drop(p, newfd, fp, 1);
	proc_fdunlock(p);
#endif
out:
	file_drop(fd);
	return (error);
}
#endif				/* HAVE_SCTP_PEELOFF_SOCKOPT */


/* socket lock pr_xxx functions */
/* Tiger only */
#if defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
int
sctp_lock(struct socket *so, int refcount, void *debug)
#else
int
sctp_lock(struct socket *so, int refcount, int lr)
#endif
{
	if (so->so_pcb) {
#if defined(APPLE_LION)
		lck_mtx_assert(&((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_NOTOWNED);
		lck_mtx_lock(&((struct inpcb *)so->so_pcb)->inpcb_mtx);
#else
		lck_mtx_assert(((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_NOTOWNED);
		lck_mtx_lock(((struct inpcb *)so->so_pcb)->inpcb_mtx);
#endif
	} else {
		panic("sctp_lock: so=%p has so_pcb == NULL.", so);
		lck_mtx_assert(so->so_proto->pr_domain->dom_mtx, LCK_MTX_ASSERT_NOTOWNED);
		lck_mtx_lock(so->so_proto->pr_domain->dom_mtx);
	}

	if (so->so_usecount < 0)
		panic("sctp_lock: so=%p so_pcb=%p ref=%x.", so, so->so_pcb, so->so_usecount);

	if (refcount)
		so->so_usecount++;

	SAVE_CALLERS(((struct sctp_inpcb *)so->so_pcb)->lock_caller1,
		     ((struct sctp_inpcb *)so->so_pcb)->lock_caller2,
		     ((struct sctp_inpcb *)so->so_pcb)->lock_caller3);
#if (!defined(APPLE_SNOWLEOPARD) && !defined(APPLE_LION))&& defined(__ppc__)
	((struct sctp_inpcb *)so->so_pcb)->lock_caller1 = lr;
	((struct sctp_inpcb *)so->so_pcb)->lock_caller2 = refcount;
#endif
	((struct sctp_inpcb *)so->so_pcb)->lock_gen_count = ((struct sctp_inpcb *)so->so_pcb)->gen_count++;
	return (0);
}

#if defined(APPLE_SNOWLEOPARD) || defined(APPLE_LION)
int
sctp_unlock(struct socket *so, int refcount, void *debug)
#else
int
sctp_unlock(struct socket *so, int refcount, int lr)
#endif
{
	if (so->so_pcb) {	
		SAVE_CALLERS(((struct sctp_inpcb *)so->so_pcb)->unlock_caller1,
			     ((struct sctp_inpcb *)so->so_pcb)->unlock_caller2,
			     ((struct sctp_inpcb *)so->so_pcb)->unlock_caller3);
#if !defined(APPLE_SNOWLEOPARD) && !defined(APPLE_LION) && defined(__ppc__)
		((struct sctp_inpcb *)so->so_pcb)->unlock_caller1 = lr;
		((struct sctp_inpcb *)so->so_pcb)->unlock_caller2 = refcount;
#endif
		((struct sctp_inpcb *)so->so_pcb)->unlock_gen_count = ((struct sctp_inpcb *)so->so_pcb)->gen_count++;
	}
	
	if (refcount)
		so->so_usecount--;

	if (so->so_usecount < 0)
		panic("sctp_unlock: so=%p usecount=%x.", so, so->so_usecount);

	if (so->so_pcb == NULL) {
		panic("sctp_unlock: so=%p has so_pcb == NULL.", so);
		lck_mtx_assert(so->so_proto->pr_domain->dom_mtx, LCK_MTX_ASSERT_OWNED);
		lck_mtx_unlock(so->so_proto->pr_domain->dom_mtx);
	} else {
#if defined (APPLE_LION)
		lck_mtx_assert(&((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_OWNED);
		lck_mtx_unlock(&((struct inpcb *)so->so_pcb)->inpcb_mtx);
#else
		lck_mtx_assert(((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_OWNED);
		lck_mtx_unlock(((struct inpcb *)so->so_pcb)->inpcb_mtx);
#endif
	}
	return (0);
}

lck_mtx_t *
sctp_getlock(struct socket *so, int locktype)
{
	/* WARNING: we do not own the socket lock here... */
	/* We do not have always enough callers */
	/*
	SAVE_CALLERS(((struct sctp_inpcb *)so->so_pcb)->getlock_caller1,
		     ((struct sctp_inpcb *)so->so_pcb)->getlock_caller2,
		     ((struct sctp_inpcb *)so->so_pcb)->getlock_caller3);
	((struct sctp_inpcb *)so->so_pcb)->getlock_gen_count = ((struct sctp_inpcb *)so->so_pcb)->gen_count++;
	*/
	if (so->so_pcb) {
		if (so->so_usecount < 0)
			panic("sctp_getlock: so=%p usecount=%x.", so, so->so_usecount);
#if defined(APPLE_LION)
		return (&((struct inpcb *)so->so_pcb)->inpcb_mtx);
#else
		return (((struct inpcb *)so->so_pcb)->inpcb_mtx);
#endif
	} else {
		panic("sctp_getlock: so=%p has so_pcb == NULL.", so);
		return (so->so_proto->pr_domain->dom_mtx);
	}
}

void
sctp_lock_assert(struct socket *so)
{
	if (so->so_pcb) {
#if defined(APPLE_LION)
		lck_mtx_assert(&((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_OWNED);
#else
		lck_mtx_assert(((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_OWNED);
#endif
	} else {
		panic("sctp_lock_assert: so=%p has so->so_pcb == NULL.", so);
	}
}

void
sctp_unlock_assert(struct socket *so)
{
	if (so) {
		if (so->so_pcb) {
#if defined(APPLE_LION)
			lck_mtx_assert(&((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_NOTOWNED);
#else
			lck_mtx_assert(((struct inpcb *)so->so_pcb)->inpcb_mtx, LCK_MTX_ASSERT_NOTOWNED);
#endif
		} else {
			panic("sctp_unlock_assert: so=%p has so->so_pcb == NULL.", so);
		}
	} else {
		printf("sctp_unlock_assert() called with so == NULL.\n");
	}
}

/*
 * timer functions
 */

void
sctp_start_main_timer(void) {
	/* bound the timer (in msec) */
	if ((int)SCTP_BASE_SYSCTL(sctp_main_timer) <= 1000/hz)
		SCTP_BASE_SYSCTL(sctp_main_timer) = 1000/hz;
	SCTP_BASE_VAR(sctp_main_timer_ticks) = MSEC_TO_TICKS(SCTP_BASE_SYSCTL(sctp_main_timer));
	timeout(sctp_timeout, NULL, SCTP_BASE_VAR(sctp_main_timer_ticks));
}

void
sctp_stop_main_timer(void) {
	untimeout(sctp_timeout, NULL);
}


/*
 * locks
 */
#ifdef _KERN_LOCKS_H_
lck_rw_t *sctp_calloutq_mtx;
#else
void *sctp_calloutq_mtx;
#endif

#if !defined(APPLE_LEOPARD) && !defined(APPLE_SNOWLEOPARD) &&!defined(APPLE_LION)

/*
 * here we hack in a fix for Apple's m_copym for the case where the first
 * mbuf in the chain is a M_PKTHDR and the length is zero.
 * NOTE: this is fixed in Leopard
 */
static void
sctp_pkthdr_fix(struct mbuf *m)
{
	struct mbuf *m_nxt;

	if ((SCTP_BUF_GET_FLAGS(m) & M_PKTHDR) == 0) {
		/* not a PKTHDR */
		return;
	}
	if (SCTP_BUF_LEN(m) != 0) {
		/* not a zero length PKTHDR mbuf */
		return;
	}
	/* let's move in a word into the first mbuf... yes, ugly! */
	m_nxt = SCTP_BUF_NEXT(m);
	if (m_nxt == NULL) {
		/* umm... not a very useful mbuf chain... */
		return;
	}
	if ((size_t)SCTP_BUF_LEN(m_nxt) > sizeof(int)) {
		/* move over an int */
		bcopy(mtod(m_nxt, caddr_t), mtod(m, caddr_t), sizeof(int));
		/* update mbuf data pointers and lengths */
		SCTP_BUF_LEN(m) += sizeof(int);
		SCTP_BUF_RESV_UF(m_nxt, sizeof(int));
		SCTP_BUF_LEN(m_nxt) -= sizeof(int);
	}
}

inline struct mbuf *
sctp_m_copym(struct mbuf *m, int off0, int len, int wait)
{
	struct mbuf *n, **np;
	int off = off0;
	struct mbuf *top;
	int copyhdr = 0;

	sctp_pkthdr_fix(m);

/*	return (m_copym(m, off, len, wait));*/
	if (off < 0 || len < 0)
		panic("sctp_m_copym: bad offset or length");
	if (off == 0 && m->m_flags & M_PKTHDR)
		copyhdr = 1;

	while (off >= m->m_len) {
		if (m == 0)
			panic("sctp_m_copym: null m");
		off -= m->m_len;
		m = m->m_next;
	}
	np = &top;
	top = 0;

	MBUF_LOCK();

	while (len > 0) {
/*
		m_range_check(mfree);
		m_range_check(mclfree);
		m_range_check(mbigfree);*/

		if (m == 0) {
			if (len != M_COPYALL)
				panic("sctp_m_copym: not M_COPYALL");
			break;
		}
		if ((n = mfree)) {
			MCHECK(n);
			++mclrefcnt[mtocl(n)];
			mbstat.m_mtypes[MT_FREE]--;
			mbstat.m_mtypes[m->m_type]++;
			mfree = n->m_next;
			n->m_next = n->m_nextpkt = 0;
			n->m_type = m->m_type;
			n->m_data = n->m_dat;
			n->m_flags = 0;
		} else {
		        MBUF_UNLOCK();
		        n = m_retry(wait, m->m_type);
		        MBUF_LOCK();
		}
		*np = n;

		if (n == 0)
			goto nospace;
		if (copyhdr) {
			M_COPY_PKTHDR(n, m);
			if (len == M_COPYALL)
				n->m_pkthdr.len -= off0;
			else
				n->m_pkthdr.len = len;
			copyhdr = 0;
		}
		if (len == M_COPYALL) {
		    if (min(len, (m->m_len - off)) == len) {
			printf("m->m_len %d - off %d = %d, %d\n",
			       m->m_len, off, m->m_len - off,
			       min(len, (m->m_len - off)));
		    }
		}
		n->m_len = min(len, (m->m_len - off));
		if (n->m_len == M_COPYALL) {
		    printf("n->m_len == M_COPYALL, fixing\n");
		    n->m_len = MHLEN;
		}
		if (m->m_flags & M_EXT) {
			n->m_ext = m->m_ext;
			insque((queue_t)&n->m_ext.ext_refs, (queue_t)&m->m_ext.ext_refs);
			n->m_data = m->m_data + off;
			n->m_flags |= M_EXT;
		} else {
			bcopy(mtod(m, caddr_t)+off, mtod(n, caddr_t),
			    (unsigned)n->m_len);
		}
		if (len != M_COPYALL)
			len -= n->m_len;
		off = 0;
		m = m->m_next;
		np = &n->m_next;
	}
	MBUF_UNLOCK();

/*	if (top == 0)
		MCFail++;
*/

	return (top);
nospace:
	MBUF_UNLOCK();

	m_freem(top);
/*	MCFail++;*/
	return (0);
}
#endif

/*
 * here we fix up Apple's m_prepend() and m_prepend_2().
 * See FreeBSD uipc_mbuf.c, version 1.170.
 * This is still needed for Leopard.
 */
struct mbuf *
sctp_m_prepend(struct mbuf *m, int len, int how)
{
	struct mbuf *mn;

	MGET(mn, how, m->m_type);
	if (mn == (struct mbuf *)NULL) {
		m_freem(m);
		return ((struct mbuf *)NULL);
	}
	if (m->m_flags & M_PKTHDR) {
		M_COPY_PKTHDR(mn, m);
		m->m_flags &= ~M_PKTHDR;
	}
	mn->m_next = m;
	m = mn;
	if (m->m_flags & M_PKTHDR) {
		if ((size_t)len < MHLEN)
			MH_ALIGN(m, len);
	} else {
		if ((size_t)len < MLEN)
			M_ALIGN(m, len);
	}
	m->m_len = len;
	return (m);
}

struct mbuf *
sctp_m_prepend_2(struct mbuf *m, int len, int how)
{
        if (M_LEADINGSPACE(m) >= len) {
                m->m_data -= len;
                m->m_len += len;
        } else {
		m = sctp_m_prepend(m, len, how);
        }
        if ((m) && (m->m_flags & M_PKTHDR))
                m->m_pkthdr.len += len;
        return (m);
}

#if defined(APPLE_LION) || defined(APPLE_SNOWLEOPARD)
inline struct mbuf *
m_pulldown(struct mbuf *mbuf, int offset, int len, int *offsetp)
{
	mbuf_t *result;

	*offsetp = offset;
	(void)mbuf_pulldown(mbuf, (size_t *)offsetp, len, result);
	return (*result);
}
#endif

static void
sctp_print_addr(struct sockaddr *sa)
{
	char ip6buf[INET6_ADDRSTRLEN];
	ip6buf[0] = 0;
	
	switch (sa->sa_family) {
	case AF_INET6:
	{
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)sa;
		printf("%s", ip6_sprintf(&sin6->sin6_addr));
		break;
	}
	case AF_INET:
	{
		struct sockaddr_in *sin;
		unsigned char *p;

		sin = (struct sockaddr_in *)sa;
		p = (unsigned char *)&sin->sin_addr;
		printf("%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
		break;
	}
	default:
		break;
	}
}

static void
sctp_addr_watchdog()
{
	errno_t error;
	ifnet_t *ifnetlist;
	uint32_t i, count;
	struct ifnet *ifn;	
	struct ifaddr *ifa;
	struct sockaddr *sa;
	struct sctp_vrf *vrf;
	struct sctp_ifn *sctp_ifn;
	struct sctp_ifa *sctp_ifa;

	ifnetlist = NULL;
	count = 0;
	SCTP_IPI_ADDR_RLOCK();
	vrf = sctp_find_vrf(SCTP_DEFAULT_VRFID);
	if (vrf == NULL) {
		SCTP_IPI_ADDR_RUNLOCK();
		printf("SCTP-NKE: Can't find default VRF.\n");
		goto out;
	}
	printf("SCTP-NKE: Interfaces available for SCTP:\n");
	LIST_FOREACH(sctp_ifn, &vrf->ifnlist, next_ifn) {
		printf("SCTP-NKE: \tInterface %s (index %d): ", sctp_ifn->ifn_name, sctp_ifn->ifn_index);
		LIST_FOREACH(sctp_ifa, &sctp_ifn->ifalist, next_ifa) {
			sa = &sctp_ifa->address.sa;
			if (sa == NULL) {
				continue;
			}
			switch (sa->sa_family) {
			case AF_INET:
			case AF_INET6:
				sctp_print_addr(sa);
				printf(" ");
				break;
			default:
				break;
			}
		}
		printf("\n");
	}
	SCTP_IPI_ADDR_RUNLOCK();
	
	ifnetlist = NULL;
	count = 0;
	error = ifnet_list_get(IFNET_FAMILY_ANY, &ifnetlist, &count);
	if (error != 0) {
		printf("SCTP-NKE: ifnet_list_get failed %d\n", error);
		goto out;
	}
	printf("SCTP-NKE: Interfaces available on the system:\n");
	for (i = 0; i < count; i++) {
		ifn = ifnetlist[i];
		printf("SCTP-NKE: \tInterface %s%d (index %d): ", ifn->if_name, ifn->if_unit, ifn->if_index);
		TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
			sa = ifa->ifa_addr;
			if (sa == NULL) {
				continue;
			}
			switch (sa->sa_family) {
			case AF_INET:
			case AF_INET6:
				sctp_print_addr(sa);
				if (sctp_find_ifa_by_addr(sa, SCTP_DEFAULT_VRFID, SCTP_ADDR_NOT_LOCKED) == NULL) {
					printf("! ");
				} else {
					printf(" ");
				}
				break;
			default:
				break;
			}
		}
		printf("\n");
	}
out:
	if (ifnetlist != NULL) {
		ifnet_list_free(ifnetlist);
	}
	return;
}

static void
sctp_vtag_watchdog()
{
	struct timeval now;
	uint32_t i, j, free_cnt, expired_cnt, inuse_cnt, other_cnt;
	struct sctpvtaghead *chain;
	struct sctp_tagblock *twait_block;

	(void)SCTP_GETTIME_TIMEVAL(&now);
	SCTP_INP_INFO_RLOCK();
	for (i = 0; i < SCTP_STACK_VTAG_HASH_SIZE; i++) {
		chain = &SCTP_BASE_INFO(vtag_timewait)[i];
		free_cnt = 0;
		expired_cnt = 0;
		inuse_cnt = 0;
		other_cnt = 0;
		if (!LIST_EMPTY(chain)) {
			LIST_FOREACH(twait_block, chain, sctp_nxt_tagblock) {
				for (j = 0; j < SCTP_NUMBER_IN_VTAG_BLOCK; j++) {
					if ((twait_block->vtag_block[j].v_tag == 0) &&
					    (twait_block->vtag_block[j].lport == 0) &&
					    (twait_block->vtag_block[j].rport == 0) &&
					    (twait_block->vtag_block[j].tv_sec_at_expire == 0)) {
						free_cnt++;
					} else if ((twait_block->vtag_block[j].v_tag != 0) &&
					           (twait_block->vtag_block[j].tv_sec_at_expire < (uint32_t)now.tv_sec)) {
						expired_cnt++;
					} else if ((twait_block->vtag_block[j].v_tag != 0) &&
					           (twait_block->vtag_block[j].tv_sec_at_expire >= (uint32_t)now.tv_sec)) {
						inuse_cnt++;
					} else {
						other_cnt++;
					}
				}
			}
		}
		if ((i % 16) == 0) {
			printf("vtag_timewait[%04x] (f/e/i): ", i);
		}
		printf(" %d/%d/%d", free_cnt, expired_cnt, inuse_cnt);
		if (((i + 1) % 16) == 0) {
			printf("\n");
		}
	}
	SCTP_INP_INFO_RUNLOCK();
	return;
}

void
sctp_slowtimo()
{
	struct inpcb *inp, *ninp;
	struct socket *so;
	static uint32_t sctp_addr_watchdog_cnt = 0;
	static uint32_t sctp_vtag_watchdog_cnt = 0;
#ifdef SCTP_DEBUG
	unsigned int n = 0;
#endif

	if ((SCTP_BASE_SYSCTL(sctp_addr_watchdog_limit) > 0) &&
	    (++sctp_addr_watchdog_cnt >= SCTP_BASE_SYSCTL(sctp_addr_watchdog_limit))) {
		sctp_addr_watchdog_cnt = 0;
		sctp_addr_watchdog();
	}
	if ((SCTP_BASE_SYSCTL(sctp_vtag_watchdog_limit) > 0) &&
	    (++sctp_vtag_watchdog_cnt >= SCTP_BASE_SYSCTL(sctp_vtag_watchdog_limit))) {
		sctp_vtag_watchdog_cnt = 0;
		sctp_vtag_watchdog();
	}

	lck_rw_lock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx));
	LIST_FOREACH_SAFE(inp, &SCTP_BASE_INFO(inplisthead), inp_list, ninp) {
#ifdef SCTP_DEBUG
		n++;
#endif
#if defined(APPLE_LION)
		if ((inp->inp_wantcnt == WNT_STOPUSING) && (lck_mtx_try_lock(&inp->inpcb_mtx))) {
			so = inp->inp_socket;
			if ((so->so_usecount != 0) || (inp->inp_state != INPCB_STATE_DEAD)) {
				lck_mtx_unlock(&inp->inpcb_mtx);
			} else {
				LIST_REMOVE(inp, inp_list);
				inp->inp_socket = NULL;
				so->so_pcb      = NULL;
				lck_mtx_unlock(&inp->inpcb_mtx);
				lck_mtx_destroy(&inp->inpcb_mtx, SCTP_BASE_INFO(mtx_grp));
				SCTP_ZONE_FREE(SCTP_BASE_INFO(ipi_zone_ep), inp);
				sodealloc(so);
				SCTP_DECR_EP_COUNT();
			}
		}
#else
		if ((inp->inp_wantcnt == WNT_STOPUSING) && (lck_mtx_try_lock(inp->inpcb_mtx))) {
			so = inp->inp_socket;
			if ((so->so_usecount != 0) || (inp->inp_state != INPCB_STATE_DEAD)) {
				lck_mtx_unlock(inp->inpcb_mtx);
			} else {
				LIST_REMOVE(inp, inp_list);
				inp->inp_socket = NULL;
				so->so_pcb      = NULL;
				lck_mtx_unlock(inp->inpcb_mtx);
				lck_mtx_free(inp->inpcb_mtx, SCTP_BASE_INFO(mtx_grp));
				SCTP_ZONE_FREE(SCTP_BASE_INFO(ipi_zone_ep), inp);
				sodealloc(so);
				SCTP_DECR_EP_COUNT();
			}
		}
#endif
	}
	lck_rw_unlock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx));
#ifdef SCTP_DEBUG
	if ((SCTP_BASE_SYSCTL(sctp_debug_on) & SCTP_DEBUG_PCB2) && (n > 0)) {
		printf("sctp_slowtimo: inps: %u\n", n);
	}
#endif
}

#if defined(SCTP_APPLE_AUTO_ASCONF)
socket_t sctp_address_monitor_so = NULL;

#define ROUNDUP(a, size) (((a) & ((size)-1)) ? (1 + ((a) | ((size)-1))) : (a))
#define NEXT_SA(sa) sa = (struct sockaddr *) \
	((caddr_t) sa + (sa->sa_len ? ROUNDUP(sa->sa_len, sizeof(unsigned long)) : sizeof(unsigned long)))

static void
sctp_get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int i;
	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			NEXT_SA(sa);
		} else {
			rti_info[i] = NULL;
		}
	}
}

static void sctp_handle_ifamsg(struct ifa_msghdr *ifa_msg) {
	errno_t error;
	ifnet_t *ifnetlist;
	uint32_t i, count;
	struct sockaddr *sa;
	struct sockaddr *rti_info[RTAX_MAX];
	struct ifnet *ifn, *found_ifn = NULL;
	struct ifaddr *ifa, *found_ifa = NULL;

	/* handle only the types we want */
	if ((ifa_msg->ifam_type != RTM_NEWADDR) && (ifa_msg->ifam_type != RTM_DELADDR)) {
		return;
	}
	
	/* parse the list of addreses reported */
	sa = (struct sockaddr *)(ifa_msg + 1);
	sctp_get_rtaddrs(ifa_msg->ifam_addrs, sa, rti_info);

	/* we only want the interface address */
	sa = rti_info[RTAX_IFA];
	/*
	 * find the actual kernel ifa/ifn for this address.
	 * we need this primarily for the v6 case to get the ifa_flags.
	 */
	ifnetlist = NULL;
	count = 0;
	error = ifnet_list_get(IFNET_FAMILY_ANY, &ifnetlist, &count);
	if (error != 0) {
		printf("SCTP-NKE: ifnet_list_get failed %d\n", error);
		goto out;
	}
	for (i = 0; i < count; i++) {
		ifn = ifnetlist[i];
		/* find the interface by index */
		if (ifa_msg->ifam_index == ifn->if_index) {
			found_ifn = ifn;
			break;
		}
	}

	if (found_ifn == NULL) {
		/* TSNH */
		printf("SCTP-NKE: if_index %u not found?!", ifa_msg->ifam_index);
		goto out;
	}

	/* verify the address on the interface */
	TAILQ_FOREACH(ifa, &found_ifn->if_addrlist, ifa_list) {
		if (found_ifa) {
			break;
		}
		if (ifa->ifa_addr == NULL) {
			continue;
		}
		switch (ifa->ifa_addr->sa_family) {
		case AF_INET:
			if (((struct sockaddr_in *)sa)->sin_addr.s_addr == ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr) {
				found_ifa = ifa;
 			}
			break;
#if defined(INET6)
		case AF_INET6:
			if (SCTP6_ARE_ADDR_EQUAL((struct sockaddr_in6 *)sa,  (struct sockaddr_in6 *)ifa->ifa_addr)) {
				found_ifa = ifa;
			}
			break;
#endif
		default:
			break;
		}
	}
	if (found_ifa == NULL) {
		/* TSNH */
		printf("SCTP-NKE: ifa not found?!");
		goto out;
	}

	/* relay the appropriate address change to the base code */
	if (ifa_msg->ifam_type == RTM_NEWADDR) {
		printf("SCTP-NKE: Adding ");
		sctp_print_addr(sa);
		printf(" to interface %s%d (index %d).\n", found_ifn->if_name, found_ifn->if_unit, found_ifn->if_index);
		sctp_addr_change(found_ifa, RTM_ADD);
	} else {
		printf("SCTP-NKE: Deleting ");
		sctp_print_addr(sa);
		printf(" from interface %s%d (index %d).\n", found_ifn->if_name, found_ifn->if_unit, found_ifn->if_index);
		sctp_addr_change(found_ifa, RTM_DELETE);
	}
out:
	if (ifnetlist != 0) {
		ifnet_list_free(ifnetlist);
	}
}


void sctp_address_monitor_cb(socket_t rt_sock, void *cookie, int watif)
{
	struct msghdr msg;
	struct iovec iov;
	size_t length;
	errno_t error;
	struct rt_msghdr *rt_msg;
	char rt_buffer[1024];

	/* setup the receive iovec and msghdr */
	iov.iov_base = rt_buffer;
	iov.iov_len = sizeof(rt_buffer);
	bzero(&msg, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	length = 0;
	/* read the routing socket */
	error = sock_receive(rt_sock, &msg, 0, &length);
	if (error) {
		printf("Routing socket read error: length %d, errno %d\n", (int)length, error);
		return;
	}
	if (length == 0) {
		return;
	}

	/* process the routing event */
	rt_msg = (struct rt_msghdr *)rt_buffer;
	switch (rt_msg->rtm_type) {
	case RTM_DELADDR:
	case RTM_NEWADDR:
		sctp_handle_ifamsg((struct ifa_msghdr *)rt_buffer);
		break;
	default:
		/* ignore this routing event */
		break;
	}
}

void sctp_address_monitor_start(void)
{
	errno_t error;

	if (sctp_address_monitor_so) {
		sock_close(sctp_address_monitor_so);
		sctp_address_monitor_so = NULL;
	}

	error = sock_socket(PF_ROUTE, SOCK_RAW, 0, sctp_address_monitor_cb, NULL, &sctp_address_monitor_so);
	if (error) {
		printf("Failed to create routing socket\n");
	}
}

void sctp_address_monitor_stop(void)
{
	if (sctp_address_monitor_so) {
		sock_close(sctp_address_monitor_so);
		sctp_address_monitor_so = NULL;
	}
	return;
}
#else
void sctp_address_monitor_start(void)
{
	return;
}

void sctp_address_monitor_stop(void)
{
	return;
}
#endif

#if 0
static void
sctp_print_mbuf_chain(mbuf_t m)
{
	for(; m; m = SCTP_BUF_NEXT(m)) {
		printf("%p: m_len = %ld, m_type = %x\n", m, SCTP_BUF_LEN(m), m->m_type);
		if (SCTP_BUF_IS_EXTENDED(m))
			printf("%p: extend_size = %d\n", m, SCTP_BUF_EXTEND_SIZE(m));
	}
}
#endif

void
sctp_over_udp_ipv4_cb(socket_t udp_sock, void *cookie, int watif)
{
	errno_t error;
	size_t length;
	mbuf_t packet;
	struct msghdr msg;
	struct sockaddr_in src, dst;
	char cmsgbuf[CMSG_SPACE(sizeof (struct in_addr))];
	struct cmsghdr *cmsg;
	struct ip *ip;
	struct mbuf *ip_m;
	
	bzero((void *)&msg, sizeof(struct msghdr));
	bzero((void *)&src, sizeof(struct sockaddr_in));
	bzero((void *)&dst, sizeof(struct sockaddr_in));
	bzero((void *)cmsgbuf, CMSG_SPACE(sizeof (struct in_addr)));
	
	msg.msg_name = (void *)&src;
	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_iov = NULL;
	msg.msg_iovlen = 0;
	msg.msg_control = (void *)cmsgbuf;
	msg.msg_controllen = (socklen_t)CMSG_LEN(sizeof (struct in_addr));
	msg.msg_flags = 0;

	length = (1<<16);
	error = sock_receivembuf(udp_sock, &msg, &packet, 0, &length);
	if (error) {
		printf("sock_receivembuf returned error %d.\n", error);
		return;
	}
	if (length == 0) {
		return;
	}
	if ((packet->m_flags & M_PKTHDR) != M_PKTHDR) {
		mbuf_freem(packet);
		return;
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if ((cmsg->cmsg_level == IPPROTO_IP) && (cmsg->cmsg_type == IP_RECVDSTADDR)) {
			dst.sin_family = AF_INET;
			dst.sin_len = sizeof(struct sockaddr_in);
			dst.sin_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
			memcpy((void *)&dst.sin_addr, (const void *)CMSG_DATA(cmsg), sizeof(struct in_addr));
		}
	}
	
	ip_m = sctp_get_mbuf_for_msg(sizeof(struct ip), 1, M_DONTWAIT, 1, MT_DATA);
	if (ip_m == NULL) {
		mbuf_freem(packet);
		return;
	}
	ip_m->m_pkthdr.rcvif = packet->m_pkthdr.rcvif;
	ip = mtod(ip_m, struct ip *);
	bzero((void *)ip, sizeof(struct ip));
	ip->ip_v = IPVERSION;
	ip->ip_len = length;
	ip->ip_src = src.sin_addr;
	ip->ip_dst = dst.sin_addr;
	SCTP_HEADER_LEN(ip_m) = (int)(sizeof(struct ip) + length);
	SCTP_BUF_LEN(ip_m) = sizeof(struct ip);
	SCTP_BUF_NEXT(ip_m) = packet;

	/*
	printf("Received a UDP packet of length %d from ", (int)length);
	print_address((struct sockaddr *)&src);
	printf(" to ");
	print_address((struct sockaddr *)&dst);
	printf(".\n");
	printf("packet = \n");
	sctp_print_mbuf_chain(packet);
	printf("ip_m = \n");
	sctp_print_mbuf_chain(ip_m);
	*/
	
	sctp_input_with_port(ip_m, sizeof(struct ip), src.sin_port);
	return;
}

void
sctp_over_udp_ipv6_cb(socket_t udp_sock, void *cookie, int watif)
{
	errno_t error;
	size_t length;
	mbuf_t packet;
	struct msghdr msg;
	struct sockaddr_in6 src, dst;
	char cmsgbuf[CMSG_SPACE(sizeof (struct in6_pktinfo))];
	struct cmsghdr *cmsg;
	struct ip6_hdr *ip6;
	struct mbuf *ip6_m;
	int offset;

	bzero((void *)&msg, sizeof(struct msghdr));
	bzero((void *)&src, sizeof(struct sockaddr_in6));
	bzero((void *)&dst, sizeof(struct sockaddr_in6));
	bzero((void *)cmsgbuf, CMSG_SPACE(sizeof (struct in6_pktinfo)));
	
	msg.msg_name = (void *)&src;
	msg.msg_namelen = sizeof(struct sockaddr_in6);
	msg.msg_iov = NULL;
	msg.msg_iovlen = 0;
	msg.msg_control = (void *)cmsgbuf;
	msg.msg_controllen = (socklen_t)CMSG_LEN(sizeof (struct in6_pktinfo));
	msg.msg_flags = 0;
	
	length = (1<<16);
	error = sock_receivembuf(udp_sock, &msg, &packet, 0, &length);
	if (error) {
		printf("sock_receivembuf returned error %d.\n", error);
		return;
	}
	if (length == 0) {
		return;
	}
	if ((packet->m_flags & M_PKTHDR) != M_PKTHDR) {
		mbuf_freem(packet);
		return;
	}
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if ((cmsg->cmsg_level == IPPROTO_IPV6) && (cmsg->cmsg_type == IPV6_PKTINFO)) {
			dst.sin6_family = AF_INET6;
			dst.sin6_len = sizeof(struct sockaddr_in6);
			dst.sin6_port = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
			memcpy((void *)&dst.sin6_addr, (const void *)(&((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr), sizeof(struct in6_addr));
		}
	}
	ip6_m = sctp_get_mbuf_for_msg(sizeof(struct ip6_hdr), 1, M_DONTWAIT, 1, MT_DATA);
	if (ip6_m == NULL) {
		mbuf_freem(packet);
		return;
	}
	ip6_m->m_pkthdr.rcvif = packet->m_pkthdr.rcvif;
	ip6 = mtod(ip6_m, struct ip6_hdr *);
	bzero((void *)ip6, sizeof(struct ip6_hdr));
	ip6->ip6_vfc = IPV6_VERSION;
	ip6->ip6_plen = htons(length);
	ip6->ip6_src = src.sin6_addr;
	ip6->ip6_dst = dst.sin6_addr;
	SCTP_HEADER_LEN(ip6_m) = (int)(sizeof(struct ip6_hdr) + length);
	SCTP_BUF_LEN(ip6_m) = sizeof(struct ip6_hdr);
	SCTP_BUF_NEXT(ip6_m) = packet;
	
	/*
	printf("Received a UDP packet of length %d from ", (int)length);
	print_address((struct sockaddr *)&src);
	printf(" to ");
	print_address((struct sockaddr *)&dst);
	printf(".\n");
	printf("packet = \n");
	sctp_print_mbuf_chain(packet);
	printf("ip_m = \n");
	sctp_print_mbuf_chain(ip6_m);
	*/
	
	offset = sizeof(struct ip6_hdr);
	sctp6_input_with_port(&ip6_m, &offset, src.sin6_port);
}

socket_t sctp_over_udp_ipv4_so = NULL;
socket_t sctp_over_udp_ipv6_so = NULL;

errno_t
sctp_over_udp_start(void)
{
	errno_t error;
	struct sockaddr_in addr_ipv4;
	struct sockaddr_in6 addr_ipv6;
	const int on = 1;
	
	if (sctp_over_udp_ipv4_so) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
	}

	if (sctp_over_udp_ipv6_so) {
		sock_close(sctp_over_udp_ipv6_so);
		sctp_over_udp_ipv6_so = NULL;
	}

	error = sock_socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP, sctp_over_udp_ipv4_cb, NULL, &sctp_over_udp_ipv4_so);
	if (error) {
		sctp_over_udp_ipv4_so = NULL;
		printf("Failed to create SCTP/UDP/IPv4 tunneling socket: errno = %d.\n", error);
		return error;
	}
	
	error = sock_setsockopt(sctp_over_udp_ipv4_so, IPPROTO_IP, IP_RECVDSTADDR, (const void *)&on, (int)sizeof(int));
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		printf("Failed to setsockopt() on SCTP/UDP/IPv4 tunneling socket: errno = %d.\n", error);
		return error;
	}
	
	memset((void *)&addr_ipv4, 0, sizeof(struct sockaddr_in));
	addr_ipv4.sin_len         = sizeof(struct sockaddr_in);
	addr_ipv4.sin_family      = AF_INET;
	addr_ipv4.sin_port        = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
	addr_ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
	error = sock_bind(sctp_over_udp_ipv4_so, (const struct sockaddr *)&addr_ipv4);
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		printf("Failed to bind SCTP/UDP/IPv4 tunneling socket: errno = %d.\n", error);
		return error;
	}
	
	error = sock_socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP, sctp_over_udp_ipv6_cb, NULL, &sctp_over_udp_ipv6_so);
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		sctp_over_udp_ipv6_so = NULL;
		printf("Failed to create SCTP/UDP/IPv6 tunneling socket: errno = %d.\n", error);
		return error;
	}

	error = sock_setsockopt(sctp_over_udp_ipv6_so, IPPROTO_IPV6, IPV6_V6ONLY, (const void *)&on, (int)sizeof(int));
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		sock_close(sctp_over_udp_ipv6_so);
		sctp_over_udp_ipv6_so = NULL;
		printf("Failed to setsockopt() on SCTP/UDP/IPv6 tunneling socket: errno = %d.\n", error);
		return error;
	}

#if defined(APPLE_LION)
	error = sock_setsockopt(sctp_over_udp_ipv6_so, IPPROTO_IPV6, IPV6_RECVPKTINFO, (const void *)&on, (int)sizeof(int));
#else
	error = sock_setsockopt(sctp_over_udp_ipv6_so, IPPROTO_IPV6, IPV6_PKTINFO, (const void *)&on, (int)sizeof(int));
#endif
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		sock_close(sctp_over_udp_ipv6_so);
		sctp_over_udp_ipv6_so = NULL;
		printf("Failed to setsockopt() on SCTP/UDP/IPv6 tunneling socket: errno = %d.\n", error);
		return error;
	}

	memset((void *)&addr_ipv6, 0, sizeof(struct sockaddr_in6));
	addr_ipv6.sin6_len    = sizeof(struct sockaddr_in6);
	addr_ipv6.sin6_family = AF_INET6;
	addr_ipv6.sin6_port   = htons(SCTP_BASE_SYSCTL(sctp_udp_tunneling_port));
	addr_ipv6.sin6_addr   = in6addr_any;
	error = sock_bind(sctp_over_udp_ipv6_so, (const struct sockaddr *)&addr_ipv6);
	if (error) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
		sock_close(sctp_over_udp_ipv6_so);
		sctp_over_udp_ipv6_so = NULL;
		printf("Failed to bind SCTP/UDP/IPv6 tunneling socket: errno = %d.\n", error);
		return error;
	}

	return (0);
}

void sctp_over_udp_stop(void)
{
	if (sctp_over_udp_ipv4_so) {
		sock_close(sctp_over_udp_ipv4_so);
		sctp_over_udp_ipv4_so = NULL;
	}
	if (sctp_over_udp_ipv6_so) {
		sock_close(sctp_over_udp_ipv6_so);
		sctp_over_udp_ipv6_so = NULL;
	}
	return;
}
