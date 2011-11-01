#include <netinet/sctp_os.h>
#include <netinet/sctp_pcb.h>
#include <netinet/sctputil.h>
#if defined(__Userspace_os_Linux)
#define __FAVOR_BSD    /* (on Ubuntu at least) enables UDP header field names like BSD in RFC 768 */
#endif
#include <netinet/udp.h>

/* Statically initializing accept_mtx and accept_cond since there is no call for ACCEPT_LOCK_INIT() */
pthread_mutex_t accept_mtx = PTHREAD_MUTEX_INITIALIZER; 
pthread_cond_t accept_cond = PTHREAD_COND_INITIALIZER;

/* Prototypes */
extern int sctp_sosend(struct socket *so, struct sockaddr *addr, struct uio *uio,
                       struct mbuf *top, struct mbuf *control, int flags,
                     /* proc is a dummy in __Userspace__ and will not be passed to sctp_lower_sosend */                       struct proc *p);

extern int sctp_attach(struct socket *so, int proto, uint32_t vrf_id);



/* Taken from  usr/src/sys/kern/uipc_sockbuf.c and modified for __Userspace__*/
/*
 * Socantsendmore indicates that no more data will be sent on the socket; it
 * would normally be applied to a socket when the user informs the system
 * that no more data is to be sent, by the protocol code (in case
 * PRU_SHUTDOWN).  Socantrcvmore indicates that no more data will be
 * received, and will normally be applied to the socket by a protocol when it
 * detects that the peer will send no more data.  Data queued for reading in
 * the socket may yet be read.
 */

void socantrcvmore_locked(struct socket *so)
{
	SOCKBUF_LOCK_ASSERT(&so->so_rcv);

	so->so_rcv.sb_state |= SBS_CANTRCVMORE;
	sorwakeup_locked(so);

}

void socantrcvmore(struct socket *so)
{
	SOCKBUF_LOCK(&so->so_rcv);
	socantrcvmore_locked(so);

}

void
socantsendmore_locked(struct socket *so)
{

	SOCKBUF_LOCK_ASSERT(&so->so_snd);

	so->so_snd.sb_state |= SBS_CANTSENDMORE;
	sowwakeup_locked(so);

}

void
socantsendmore(struct socket *so)
{

	SOCKBUF_LOCK(&so->so_snd);
	socantsendmore_locked(so);

}



/* Taken from  usr/src/sys/kern/uipc_sockbuf.c and called within sctp_lower_sosend. 
 */
int
sbwait(struct sockbuf *sb)
{
#if defined(__Userspace__) /* __Userspace__ */

        SOCKBUF_LOCK_ASSERT(sb);

	sb->sb_flags |= SB_WAIT;
        return (pthread_cond_wait(&(sb->sb_cond), &(sb->sb_mtx)));

#else
	SOCKBUF_LOCK_ASSERT(sb);

	sb->sb_flags |= SB_WAIT;
	return (msleep(&sb->sb_cc, &sb->sb_mtx,
	    (sb->sb_flags & SB_NOINTR) ? PSOCK : PSOCK | PCATCH, "sbwait",
	    sb->sb_timeo));
#endif
}




/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 */
static struct socket *
soalloc(void)
{
#if defined(__Userspace__)
	struct socket *so;

        /*
         * soalloc() sets of socket layer state for a socket,
         * called only by socreate() and sonewconn().
         *
         * sodealloc() tears down socket layer state for a socket,
         * called only by sofree() and sonewconn().
         * __Userspace__ TODO : Make sure so is properly deallocated
         * when tearing down the connection.
         */
        so = malloc(sizeof(struct socket));
	if (so == NULL)
		return (NULL);
        bzero(so, sizeof(struct socket));

        /* __Userspace__ Initializing the socket locks here */
	SOCKBUF_LOCK_INIT(&so->so_snd, "so_snd");
	SOCKBUF_LOCK_INIT(&so->so_rcv, "so_rcv");
        SOCKBUF_COND_INIT(&so->so_snd);
        SOCKBUF_COND_INIT(&so->so_rcv);
        SOCK_COND_INIT(so); /* timeo_cond */
        /* __Userspace__ Any ref counting required here? Will we have any use for aiojobq?
         What about gencnt and numopensockets?*/
	TAILQ_INIT(&so->so_aiojobq);
	return (so);
        
#else
        /* Putting the kernel version for reference. The #else
           should be removed once the __Userspace__
           version is tested.
        */
        struct socket *so;

	so = uma_zalloc(socket_zone, M_NOWAIT | M_ZERO);
	if (so == NULL)
		return (NULL);
#ifdef MAC
	if (mac_init_socket(so, M_NOWAIT) != 0) {
		uma_zfree(socket_zone, so);
		return (NULL);
	}
#endif
	SOCKBUF_LOCK_INIT(&so->so_snd, "so_snd");
	SOCKBUF_LOCK_INIT(&so->so_rcv, "so_rcv");
	sx_init(&so->so_snd.sb_sx, "so_snd_sx");
	sx_init(&so->so_rcv.sb_sx, "so_rcv_sx");
	TAILQ_INIT(&so->so_aiojobq);
	mtx_lock(&so_global_mtx);
	so->so_gencnt = ++so_gencnt;
	++numopensockets;
	mtx_unlock(&so_global_mtx);
	return (so);
#endif
}

#if defined(__Userspace__)
/*
 * Free the storage associated with a socket at the socket layer.
 */
static void
sodealloc(struct socket *so)
{

	assert(so->so_count == 0); 
	assert(so->so_pcb == NULL);

	SOCKBUF_LOCK_DESTROY(&so->so_snd);
	SOCKBUF_LOCK_DESTROY(&so->so_rcv);

	SOCKBUF_COND_DESTROY(&so->so_snd);
	SOCKBUF_COND_DESTROY(&so->so_rcv);

        SOCK_COND_DESTROY(so);
        
	free(so);
}

#else /* kernel version for reference. */
/*
 * Free the storage associated with a socket at the socket layer, tear down
 * locks, labels, etc.  All protocol state is assumed already to have been
 * torn down (and possibly never set up) by the caller.
 */
static void
sodealloc(struct socket *so)
{

	KASSERT(so->so_count == 0, ("sodealloc(): so_count %d", so->so_count));
	KASSERT(so->so_pcb == NULL, ("sodealloc(): so_pcb != NULL"));

	mtx_lock(&so_global_mtx);
	so->so_gencnt = ++so_gencnt;
	--numopensockets;	/* Could be below, but faster here. */
	mtx_unlock(&so_global_mtx);
	if (so->so_rcv.sb_hiwat)
		(void)chgsbsize(so->so_cred->cr_uidinfo,
		    &so->so_rcv.sb_hiwat, 0, RLIM_INFINITY);
	if (so->so_snd.sb_hiwat)
		(void)chgsbsize(so->so_cred->cr_uidinfo,
		    &so->so_snd.sb_hiwat, 0, RLIM_INFINITY);
#ifdef INET
	/* remove acccept filter if one is present. */
	if (so->so_accf != NULL)
		do_setopt_accept_filter(so, NULL);
#endif
#ifdef MAC
	mac_destroy_socket(so);
#endif
	crfree(so->so_cred);
	sx_destroy(&so->so_snd.sb_sx);
	sx_destroy(&so->so_rcv.sb_sx);
	SOCKBUF_LOCK_DESTROY(&so->so_snd);
	SOCKBUF_LOCK_DESTROY(&so->so_rcv);
	uma_zfree(socket_zone, so);
}
#endif

/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 */
void
sofree(struct socket *so)
{
    /* 	struct protosw *pr = so->so_proto; */
	struct socket *head;
        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
    	ACCEPT_LOCK_ASSERT();
    	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
	SOCK_LOCK_ASSERT(so);
        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
        /* SS_NOFDREF unset in accept call.  this condition seems irrelevent
         *  for __Userspace__...
         */
	if (/* (so->so_state & SS_NOFDREF) == 0 || */ so->so_count != 0 ||
	    (so->so_state & SS_PROTOREF) || (so->so_qstate & SQ_COMP)) {
	            /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
		SOCK_UNLOCK(so);
		        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
		ACCEPT_UNLOCK();
		        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
		return;
	}
	head = so->so_head;
	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
	if (head != NULL) {
	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
		KASSERT((so->so_qstate & SQ_COMP) != 0 ||
		    (so->so_qstate & SQ_INCOMP) != 0,
		    ("sofree: so_head != NULL, but neither SQ_COMP nor "
		    "SQ_INCOMP"));
		KASSERT((so->so_qstate & SQ_COMP) == 0 ||
		    (so->so_qstate & SQ_INCOMP) == 0,
		    ("sofree: so->so_qstate is SQ_COMP and also SQ_INCOMP"));
		TAILQ_REMOVE(&head->so_incomp, so, so_list);
		head->so_incqlen--;
		so->so_qstate &= ~SQ_INCOMP;
		so->so_head = NULL;
	}
	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
	KASSERT((so->so_qstate & SQ_COMP) == 0 &&
	    (so->so_qstate & SQ_INCOMP) == 0,
	    ("sofree: so_head == NULL, but still SQ_COMP(%d) or SQ_INCOMP(%d)",
	    so->so_qstate & SQ_COMP, so->so_qstate & SQ_INCOMP));
	if (so->so_options & SO_ACCEPTCONN) {
		KASSERT((TAILQ_EMPTY(&so->so_comp)), ("sofree: so_comp populated"));
		KASSERT((TAILQ_EMPTY(&so->so_incomp)), ("sofree: so_comp populated"));
	}
	SOCK_UNLOCK(so);
	ACCEPT_UNLOCK();
	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
        /*	if (pr->pr_flags & PR_RIGHTS && pr->pr_domain->dom_dispose != NULL)
        		(*pr->pr_domain->dom_dispose)(so->so_rcv.sb_mb);
        	if (pr->pr_usrreqs->pru_detach != NULL)
        		(*pr->pr_usrreqs->pru_detach)(so);
         */
        sctp_close(so); /* was...    sctp_detach(so); */
                /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
	/*
	 * From this point on, we assume that no other references to this
	 * socket exist anywhere else in the stack.  Therefore, no locks need
	 * to be acquired or held.
	 *
	 * We used to do a lot of socket buffer and socket locking here, as
	 * well as invoke sorflush() and perform wakeups.  The direct call to
	 * dom_dispose() and sbrelease_internal() are an inlining of what was
	 * necessary from sorflush().
	 *
	 * Notice that the socket buffer and kqueue state are torn down
	 * before calling pru_detach.  This means that protocols shold not
	 * assume they can perform socket wakeups, etc, in their detach code.
	 */
        /*	sbdestroy(&so->so_snd, so);
        	sbdestroy(&so->so_rcv, so);
        	knlist_destroy(&so->so_rcv.sb_sel.si_note);
        	knlist_destroy(&so->so_snd.sb_sel.si_note); */
        	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
	sodealloc(so);
	        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
}



/* Taken from  /src/sys/kern/uipc_socket.c */
int
soabort(so)
	struct socket *so;
{
	int error;

	error = sctp_abort(so);
	if (error) {
		sofree(so);
		return error;
	}
	return (0);
}


/* Taken from  usr/src/sys/kern/uipc_socket.c and called within sctp_connect (sctp_usrreq.c).
 *  We use sctp_connect for send_one_init_real in ms1.
 */
void
soisconnecting(struct socket *so)
{

	SOCK_LOCK(so);
	so->so_state &= ~(SS_ISCONNECTED|SS_ISDISCONNECTING);
	so->so_state |= SS_ISCONNECTING;
	SOCK_UNLOCK(so);
}

/* Taken from  usr/src/sys/kern/uipc_socket.c and called within sctp_disconnect (sctp_usrreq.c).
 *  TODO Do we use sctp_disconnect?
 */
void
soisdisconnecting(struct socket *so)
{

        /*
         * Note: This code assumes that SOCK_LOCK(so) and
         * SOCKBUF_LOCK(&so->so_rcv) are the same.
         */
        SOCKBUF_LOCK(&so->so_rcv);
        so->so_state &= ~SS_ISCONNECTING;
        so->so_state |= SS_ISDISCONNECTING;
        so->so_rcv.sb_state |= SBS_CANTRCVMORE;
        sorwakeup_locked(so);
        SOCKBUF_LOCK(&so->so_snd);
        so->so_snd.sb_state |= SBS_CANTSENDMORE;
        sowwakeup_locked(so);
        wakeup("dummy",so);
        // requires 2 args but this was in orig        wakeup(&so->so_timeo);
}


/* Taken from sys/kern/kern_synch.c and
   modified for __Userspace__
*/

/*
 * Make all threads sleeping on the specified identifier runnable.
 * Associating wakeup with so_timeo identifier and timeo_cond
 * condition variable. TODO. If we use iterator thread then we need to
 * modify wakeup so it can distinguish between iterator identifier and
 * timeo identifier.
 */
void
wakeup(ident, so)
	void *ident;
        struct socket *so;
{
    SOCK_LOCK(so); 
    pthread_cond_broadcast(&(so)->timeo_cond);
    SOCK_UNLOCK(so); 
}


/*
 * Make a thread sleeping on the specified identifier runnable.
 * May wake more than one thread if a target thread is currently
 * swapped out.
 */
void
wakeup_one(ident)
	void *ident;
{
    /* __Userspace__ Check: We are using accept_cond for wakeup_one.
       It seems that wakeup_one is only called within
       soisconnected() and sonewconn() with ident &head->so_timeo
       head is so->so_head, which is back pointer to listen socket
       This seems to indicate that the use of accept_cond is correct
       since socket where accepts occur is so_head in all
       subsidiary sockets.
     */
    ACCEPT_LOCK();
    pthread_cond_signal(&accept_cond);
    ACCEPT_UNLOCK();
}


/* Called within sctp_process_cookie_[existing/new] */
void
soisconnected(struct socket *so)
{
	struct socket *head;

	ACCEPT_LOCK();
	SOCK_LOCK(so);
	so->so_state &= ~(SS_ISCONNECTING|SS_ISDISCONNECTING|SS_ISCONFIRMING);
	so->so_state |= SS_ISCONNECTED;
	head = so->so_head;
	if (head != NULL && (so->so_qstate & SQ_INCOMP)) {
		if ((so->so_options & SO_ACCEPTFILTER) == 0) {
			SOCK_UNLOCK(so);
			TAILQ_REMOVE(&head->so_incomp, so, so_list);
			head->so_incqlen--;
			so->so_qstate &= ~SQ_INCOMP;
			TAILQ_INSERT_TAIL(&head->so_comp, so, so_list);
			head->so_qlen++;
			so->so_qstate |= SQ_COMP;
			ACCEPT_UNLOCK();
			sorwakeup(head);
                        wakeup_one(&head->so_timeo);
		} else {
			ACCEPT_UNLOCK();
                        /*			so->so_upcall =
                        			    head->so_accf->so_accept_filter->accf_callback;
                        			so->so_upcallarg = head->so_accf->so_accept_filter_arg; */
			so->so_rcv.sb_flags |= SB_UPCALL;
			so->so_options &= ~SO_ACCEPTFILTER;
			SOCK_UNLOCK(so);
                        /*			so->so_upcall(so, so->so_upcallarg, M_DONTWAIT); */
		}

		return;
	}
	SOCK_UNLOCK(so);
	ACCEPT_UNLOCK();
        wakeup(&so->so_timeo, so);
	sorwakeup(so);
	sowwakeup(so);

}

/* called within sctp_handle_cookie_echo */

struct socket *
sonewconn(struct socket *head, int connstatus)
{
	struct socket *so;
	int over;

	ACCEPT_LOCK();
	over = (head->so_qlen > 3 * head->so_qlimit / 2);
	ACCEPT_UNLOCK();
#ifdef REGRESSION
	if (regression_sonewconn_earlytest && over)
#else
	if (over)
#endif
		return (NULL);
	so = soalloc();
	if (so == NULL)
		return (NULL);
	if ((head->so_options & SO_ACCEPTFILTER) != 0)
		connstatus = 0;
	so->so_head = head;
	so->so_type = head->so_type;
	so->so_options = head->so_options &~ SO_ACCEPTCONN;
	so->so_linger = head->so_linger;
	so->so_state = head->so_state | SS_NOFDREF;
	so->so_proto = head->so_proto;
        /*	so->so_cred = crhold(head->so_cred); */
#ifdef MAC
	SOCK_LOCK(head);
	mac_create_socket_from_socket(head, so);
	SOCK_UNLOCK(head);
#endif
        /*	knlist_init(&so->so_rcv.sb_sel.si_note, SOCKBUF_MTX(&so->so_rcv),
        	    NULL, NULL, NULL);
        	knlist_init(&so->so_snd.sb_sel.si_note, SOCKBUF_MTX(&so->so_snd),
        	    NULL, NULL, NULL); */
	if (soreserve(so, head->so_snd.sb_hiwat, head->so_rcv.sb_hiwat) ||
            sctp_attach(so, IPPROTO_SCTP, SCTP_DEFAULT_VRFID) ) {
            /*  (*so->so_proto->pr_usrreqs->pru_attach)(so, 0, NULL)) { */
		sodealloc(so);
		return (NULL);
	}
	so->so_rcv.sb_lowat = head->so_rcv.sb_lowat;
	so->so_snd.sb_lowat = head->so_snd.sb_lowat;
	so->so_rcv.sb_timeo = head->so_rcv.sb_timeo;
	so->so_snd.sb_timeo = head->so_snd.sb_timeo;
	so->so_rcv.sb_flags |= head->so_rcv.sb_flags & SB_AUTOSIZE;
	so->so_snd.sb_flags |= head->so_snd.sb_flags & SB_AUTOSIZE;
	so->so_state |= connstatus;
	ACCEPT_LOCK();
	if (connstatus) {
		TAILQ_INSERT_TAIL(&head->so_comp, so, so_list);
		so->so_qstate |= SQ_COMP;
		head->so_qlen++;
	} else {
		/*
		 * Keep removing sockets from the head until there's room for
		 * us to insert on the tail.  In pre-locking revisions, this
		 * was a simple if(), but as we could be racing with other
		 * threads and soabort() requires dropping locks, we must
		 * loop waiting for the condition to be true.
		 */
		while (head->so_incqlen > head->so_qlimit) {
			struct socket *sp;
			sp = TAILQ_FIRST(&head->so_incomp);
			TAILQ_REMOVE(&head->so_incomp, sp, so_list);
			head->so_incqlen--;
			sp->so_qstate &= ~SQ_INCOMP;
			sp->so_head = NULL;
			ACCEPT_UNLOCK();
                        soabort(sp);
			ACCEPT_LOCK();
		}
		TAILQ_INSERT_TAIL(&head->so_incomp, so, so_list);
		so->so_qstate |= SQ_INCOMP;
		head->so_incqlen++;
	}
	ACCEPT_UNLOCK();
	if (connstatus) {
		sorwakeup(head);
                wakeup_one(&head->so_timeo);
	}
	return (so);

}

/* From /src/sys/sys/sysproto.h */
struct sctp_generic_sendmsg_args {
	int sd; 
	caddr_t msg; 
	int mlen; 
	caddr_t to;
        socklen_t tolen;  /* was __socklen_t */
	struct sctp_sndrcvinfo * sinfo;
        int flags;
};

struct sctp_generic_recvmsg_args {
        int sd; 
        struct iovec *iov; 
        int iovlen;
        struct sockaddr *from;
        socklen_t *fromlenaddr; /* was __socklen_t */
        struct sctp_sndrcvinfo *sinfo; 
        int *msg_flags;
};


 /*
   Source: /src/sys/gnu/fs/xfs/FreeBSD/xfs_ioctl.c
 */
 static __inline__ int
copy_to_user(void *dst, void *src, int len) {
	memcpy(dst,src,len);
	return 0;
}

 static __inline__ int
copy_from_user(void *dst, void *src, int len) {
	memcpy(dst,src,len);
	return 0;
}

/*
 References:
 src/sys/dev/lmc/if_lmc.h:
 src/sys/powerpc/powerpc/copyinout.c
 src/sys/sys/systm.h
*/
# define copyin(u, k, len)	copy_from_user(k, u, len)

/* References:
   src/sys/powerpc/powerpc/copyinout.c
   src/sys/sys/systm.h
*/
# define copyout(k, u, len)	copy_to_user(u, k, len)


/* copyiniov definition copied/modified from src/sys/kern/kern_subr.c */
int
copyiniov(struct iovec *iovp, u_int iovcnt, struct iovec **iov, int error)
{
	u_int iovlen;

	*iov = NULL;
	if (iovcnt > UIO_MAXIOV)
		return (error);
	iovlen = iovcnt * sizeof (struct iovec);
	*iov = malloc(iovlen); /*, M_IOV, M_WAITOK); */
	error = copyin(iovp, *iov, iovlen);
	if (error) {
                free(*iov); /*, M_IOV); */
		*iov = NULL;
	}
	return (error);
}

/* (__Userspace__) version of uiomove */
int
uiomove(void *cp, int n, struct uio *uio)
{
	struct iovec *iov;
	u_int cnt;
	int error = 0;
	
	assert(uio->uio_rw == UIO_READ || uio->uio_rw == UIO_WRITE);

	while (n > 0 && uio->uio_resid) {
		iov = uio->uio_iov;
		cnt = iov->iov_len;
		if (cnt == 0) {
			uio->uio_iov++;
			uio->uio_iovcnt--;
			continue;
		}
		if (cnt > n)
			cnt = n;

		switch (uio->uio_segflg) {

		case UIO_USERSPACE:
			if (uio->uio_rw == UIO_READ)
				error = copyout(cp, iov->iov_base, cnt);
			else
				error = copyin(iov->iov_base, cp, cnt);
			if (error)
				goto out;
			break;

		case UIO_SYSSPACE:
			if (uio->uio_rw == UIO_READ)
				bcopy(cp, iov->iov_base, cnt);
			else
				bcopy(iov->iov_base, cp, cnt);
			break;
		case UIO_NOCOPY:
			break;
		}
		iov->iov_base = (char *)iov->iov_base + cnt;
		iov->iov_len -= cnt;
		uio->uio_resid -= cnt;
		uio->uio_offset += cnt;
		cp = (char *)cp + cnt;
		n -= cnt;
	}
out:
	return (error);
}


/* Source: src/sys/kern/uipc_syscalls.c */
int
getsockaddr(namp, uaddr, len)
	struct sockaddr **namp;
	caddr_t uaddr;
	size_t len;
{
	struct sockaddr *sa;
	int error;

	if (len > SOCK_MAXADDRLEN)
		return (ENAMETOOLONG);
	if (len < offsetof(struct sockaddr, sa_data[0]))
		return (EINVAL);
	MALLOC(sa, struct sockaddr *, len, M_SONAME, M_WAITOK);
	error = copyin(uaddr, sa, len);
	if (error) {
		FREE(sa, M_SONAME);
	} else {
#if !defined(__Userspace_os_Linux)
		sa->sa_len = len;
#endif
		*namp = sa;
	}
	return (error);
}



/* The original implementation of sctp_generic_sendmsg is in /src/sys/kern/uipc_syscalls.c
 * Modifying it for __Userspace__
 */
#if 0
static int
sctp_generic_sendmsg (so, uap, retval)
        struct socket *so;
	struct sctp_generic_sendmsg_args /* {
		int sd, 
		caddr_t msg, 
		int mlen, 
		caddr_t to, 
		socklen_t tolen, 
		struct sctp_sndrcvinfo *sinfo, 
		int flags
	} */ *uap;
        int *retval;
{

	struct sctp_sndrcvinfo sinfo, *u_sinfo = NULL;
	int error = 0, len;
	struct sockaddr *to = NULL;

	struct uio auio;
	struct iovec iov[1];

	if (uap->sinfo) {
		error = copyin(uap->sinfo, &sinfo, sizeof (sinfo));
		if (error)
			return (error);
		u_sinfo = &sinfo;
	}
	if (uap->tolen) {
		error = getsockaddr(&to, uap->to, uap->tolen);
		if (error) {
			to = NULL;
			goto sctp_bad2;
		}
	}


	iov[0].iov_base = uap->msg;
	iov[0].iov_len = uap->mlen;


	auio.uio_iov =  iov;
	auio.uio_iovcnt = 1;
	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_offset = 0;			/* XXX */
	auio.uio_resid = 0;
	len = auio.uio_resid = uap->mlen;
	error = sctp_lower_sosend(so, to, &auio,
		    (struct mbuf *)NULL, (struct mbuf *)NULL,
		    uap->flags, u_sinfo);
	if (error == 0)
		*retval = len - auio.uio_resid;
        else
                *retval = (-1);
sctp_bad2:
	if (to)
		FREE(to, M_SONAME);
	return (error);
}
#endif

/* Taken from  /src/lib/libc/net/sctp_sys_calls.c
 * and modified for __Userspace__
 * calling sctp_generic_sendmsg from this function
 */
ssize_t
userspace_sctp_sendmsg(struct socket *so, 
    const void *data,
    size_t len,
    struct sockaddr *to,
    socklen_t tolen,
    u_int32_t ppid,
    u_int32_t flags,
    u_int16_t stream_no,
    u_int32_t timetolive,
    u_int32_t context)
{
    
#if 1 /* def SYS_sctp_generic_sendmsg __Userspace__ */

    struct sctp_sndrcvinfo sndrcvinfo, *sinfo = &sndrcvinfo;
    struct uio auio;
    struct iovec iov[1];
    int error = 0;
    int uflags = 0;
    int retvalsendmsg;
    
    sinfo->sinfo_ppid = ppid;
    sinfo->sinfo_flags = flags;
    sinfo->sinfo_stream = stream_no;
    sinfo->sinfo_timetolive = timetolive;
    sinfo->sinfo_context = context;
    sinfo->sinfo_assoc_id = 0;

    
    /* Perform error checks on destination (to) */
    if (tolen > SOCK_MAXADDRLEN){
        error = (ENAMETOOLONG);
        goto sendmsg_return;
    }
    if (tolen < offsetof(struct sockaddr, sa_data[0])){
        error = (EINVAL);
        goto sendmsg_return;
    }
    /* Adding the following as part of defensive programming, in case the application
       does not do it when preparing the destination address.*/
#if !defined(__Userspace_os_Linux)
    to->sa_len = tolen;
#endif

    
    iov[0].iov_base = (caddr_t)data;
    iov[0].iov_len = len;
    
    auio.uio_iov =  iov;
    auio.uio_iovcnt = 1;
    auio.uio_segflg = UIO_USERSPACE;
    auio.uio_rw = UIO_WRITE;
    auio.uio_offset = 0;			/* XXX */
    auio.uio_resid = len;
    error = sctp_lower_sosend(so, to, &auio,
                              (struct mbuf *)NULL, (struct mbuf *)NULL,
                              uflags, sinfo);
sendmsg_return:
    if (0 == error)
        retvalsendmsg = len - auio.uio_resid;
    else if(error == EWOULDBLOCK) {
        errno = EWOULDBLOCK;
        retvalsendmsg = (-1);
    } else {
        printf("%s: error = %d\n", __func__, error);
        retvalsendmsg = (-1);
    }

    return retvalsendmsg;
    
    
#if 0 /* Removed: Old implementation that does unnecessary copying of sinfo and sockaddr */
    struct sctp_sndrcvinfo sndrcvinfo, *sinfo = &sndrcvinfo;
    struct sctp_generic_sendmsg_args ua, *uap = &ua;
    int retval;
    ssize_t sz;
    
    sinfo->sinfo_ppid = ppid;
    sinfo->sinfo_flags = flags;
    sinfo->sinfo_stream = stream_no;
    sinfo->sinfo_timetolive = timetolive;
    sinfo->sinfo_context = context;
    sinfo->sinfo_assoc_id = 0;
    /*__Userspace__ sd field is being arbitrarily set to 0
     * it appears not to be used later and not passes to
     * sctp_lower_sosend
     */
    uap->sd = 0; 
        uap->msg = (caddr_t)data;
        uap->mlen = len;
        uap->to = (caddr_t)to;
        uap->tolen = tolen;
        uap->sinfo = sinfo;
        uap->flags = 0;
        
	sz = sctp_generic_sendmsg(so, uap, &retval);
        
        if(sz) /*error*/
            printf("%s: errno = sz = %d\n", __func__, sz); /* should we exit here in case of error? */
        
        return retval;
#endif
        
#else

	ssize_t sz;
	struct msghdr msg;
	struct sctp_sndrcvinfo *s_info;
	struct iovec iov[SCTP_SMALL_IOVEC_SIZE];
	char controlVector[SCTP_CONTROL_VEC_SIZE_RCV];
	struct cmsghdr *cmsg;
	struct sockaddr *who = NULL;
	union {
		struct sockaddr_in in;
		struct sockaddr_in6 in6;
	}     addr;

/*
  fprintf(io, "sctp_sendmsg(sd:%d, data:%x, len:%d, to:%x, tolen:%d, ppid:%x, flags:%x str:%d ttl:%d ctx:%x\n",
  s,
  (u_int)data,
  (int)len,
  (u_int)to,
  (int)tolen,
  ppid, flags,
  (int)stream_no,
  (int)timetolive,
  (u_int)context);
  fflush(io);
*/
	if ((tolen > 0) && ((to == NULL) || (tolen < sizeof(struct sockaddr)))) {
		errno = EINVAL;
		return -1;
	}
	if (to && (tolen > 0)) {
		if (to->sa_family == AF_INET) {
			if (tolen != sizeof(struct sockaddr_in)) {
				errno = EINVAL;
				return -1;
			}
			if ((to->sa_len > 0) && (to->sa_len != sizeof(struct sockaddr_in))) {
				errno = EINVAL;
				return -1;
			}
			memcpy(&addr, to, sizeof(struct sockaddr_in));
			addr.in.sin_len = sizeof(struct sockaddr_in);
		} else if (to->sa_family == AF_INET6) {
			if (tolen != sizeof(struct sockaddr_in6)) {
				errno = EINVAL;
				return -1;
			}
			if ((to->sa_len > 0) && (to->sa_len != sizeof(struct sockaddr_in6))) {
				errno = EINVAL;
				return -1;
			}
			memcpy(&addr, to, sizeof(struct sockaddr_in6));
			addr.in6.sin6_len = sizeof(struct sockaddr_in6);
		} else {
			errno = EAFNOSUPPORT;
			return -1;
		}
		who = (struct sockaddr *)&addr;
	}
	iov[0].iov_base = (char *)data;
	iov[0].iov_len = len;
	iov[1].iov_base = NULL;
	iov[1].iov_len = 0;

	if (who) {
		msg.msg_name = (caddr_t)who;
		msg.msg_namelen = who->sa_len;
	} else {
		msg.msg_name = (caddr_t)NULL;
		msg.msg_namelen = 0;
	}
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = (caddr_t)controlVector;

	cmsg = (struct cmsghdr *)controlVector;

	cmsg->cmsg_level = IPPROTO_SCTP;
	cmsg->cmsg_type = SCTP_SNDRCV;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct sctp_sndrcvinfo));
	s_info = (struct sctp_sndrcvinfo *)CMSG_DATA(cmsg);

	s_info->sinfo_stream = stream_no;
	s_info->sinfo_ssn = 0;
	s_info->sinfo_flags = flags;
	s_info->sinfo_ppid = ppid;
	s_info->sinfo_context = context;
	s_info->sinfo_assoc_id = 0;
	s_info->sinfo_timetolive = timetolive;
	errno = 0;
	msg.msg_controllen = cmsg->cmsg_len;
	sz = sendmsg(s, &msg, 0);
	return (sz);
#endif
}


struct mbuf* mbufalloc(size_t size, void* data, unsigned char fill)
{
    size_t left;
    int resv_upfront = sizeof(struct sctp_data_chunk);
    int cancpy, willcpy;
    struct mbuf *m, *head;
    int cpsz=0;
    
    /* First one gets a header equal to sizeof(struct sctp_data_chunk) */
    left = size;
    head = m = sctp_get_mbuf_for_msg((left + resv_upfront), 1, M_WAIT, 0, MT_DATA);
    if (m == NULL) {
        printf("%s: ENOMEN: Memory allocation failure\n", __func__);
        return (NULL);
    }
    /*-
     * Skipping space for chunk header. __Userspace__ Is this required?
     */
    SCTP_BUF_RESV_UF(m, resv_upfront);
    cancpy = M_TRAILINGSPACE(m);
    willcpy = min(cancpy, left);

    while (left > 0) {
        
        if (data != NULL){
            /* fill in user data */        
            memcpy(mtod(m, caddr_t), data+cpsz, willcpy);
        }else if (fill != '\0'){
            memset(mtod(m, caddr_t), fill, willcpy);            
        }

        SCTP_BUF_LEN(m) = willcpy;
        left -= willcpy;
        cpsz += willcpy;
        if (left > 0) {
            SCTP_BUF_NEXT(m) = sctp_get_mbuf_for_msg(left, 0, M_WAIT, 0, MT_DATA);
            if (SCTP_BUF_NEXT(m) == NULL) {
                /*
                 * the head goes back to caller, he can free
                 * the rest
                 */
                sctp_m_freem(head);
                SCTP_LTRACE_ERR_RET(NULL, NULL, NULL, SCTP_FROM_SCTP_OUTPUT, ENOMEM);
                printf("%s: ENOMEN: Memory allocation failure\n", __func__);
                return (NULL);
            }
            m = SCTP_BUF_NEXT(m);
            cancpy = M_TRAILINGSPACE(m);
            willcpy = min(cancpy, left);
        } else {
            SCTP_BUF_NEXT(m) = NULL;
        }
    }

    /* The following overwrites data in head->m_hdr.mh_data , if M_PKTHDR isn't set */
    SCTP_HEADER_LEN(head) = cpsz;

    return (head);
}



struct mbuf* mbufallocfromiov(int iovlen, struct iovec *srciov)
{
    size_t left = 0,total;
    int resv_upfront = sizeof(struct sctp_data_chunk);
    int cancpy, willcpy;
    struct mbuf *m, *head;
    int cpsz=0,i, cur=-1, currdsz=0, mbuffillsz;
    char *data;

    /* Get the total length */
    for(i=0; i < iovlen; i++) {
        left += srciov[i].iov_len;
        if(cur == -1 && srciov[i].iov_len > 0) {
            /* set the first field where there's data */
            cur = i;
            data = srciov[cur].iov_base;
        }
    }
    total = left;
    
    /* First one gets a header equal to sizeof(struct sctp_data_chunk) */
    head = m = sctp_get_mbuf_for_msg((left + resv_upfront), 1, M_WAIT, 0, MT_DATA);
    if (m == NULL) {
        printf("%s: ENOMEN: Memory allocation failure\n", __func__);
        return (NULL);
    }
    /*-
     * Skipping space for chunk header. __Userspace__ Is this required?
     */
    SCTP_BUF_RESV_UF(m, resv_upfront);
    cancpy = M_TRAILINGSPACE(m);
    willcpy = min(cancpy, left);

    while (left > 0) {        
        /* fill in user data */
        mbuffillsz = 0;
        while (mbuffillsz < willcpy) {
            
            if(cancpy < srciov[cur].iov_len - currdsz) {
                /* will fill mbuf before srciov[cur] is completely read */
                memcpy(SCTP_BUF_AT(m,mbuffillsz), data, cancpy);
                data += cancpy;
                currdsz += cancpy;
                break;
            } else {
                /* will completely read srciov[cur] */
                if(srciov[cur].iov_len != currdsz) {
                    memcpy(SCTP_BUF_AT(m,mbuffillsz), data, srciov[cur].iov_len - currdsz);
                    mbuffillsz += (srciov[cur].iov_len - currdsz);
                    cancpy -= (srciov[cur].iov_len - currdsz);
                }
                currdsz = 0;
                /* find next field with data */
                data = NULL;
                while(++cur < iovlen) {
                    if(srciov[cur].iov_len > 0) {
                        data = srciov[cur].iov_base;
                        break;
                    }
                }
            }
        }

        SCTP_BUF_LEN(m) = willcpy;
        left -= willcpy;
        cpsz += willcpy;
        if (left > 0) {
            SCTP_BUF_NEXT(m) = sctp_get_mbuf_for_msg(left, 0, M_WAIT, 0, MT_DATA);
            if (SCTP_BUF_NEXT(m) == NULL) {
                /*
                 * the head goes back to caller, he can free
                 * the rest
                 */
                sctp_m_freem(head);
                SCTP_LTRACE_ERR_RET(NULL, NULL, NULL, SCTP_FROM_SCTP_OUTPUT, ENOMEM);
                printf("%s: ENOMEN: Memory allocation failure\n", __func__);
                return (NULL);
            }
            m = SCTP_BUF_NEXT(m);
            cancpy = M_TRAILINGSPACE(m);
            willcpy = min(cancpy, left);
        } else {
            SCTP_BUF_NEXT(m) = NULL;
        }
    }

    /* The following overwrites data in head->m_hdr.mh_data , if M_PKTHDR isn't set */
    assert(cpsz == total);
    SCTP_HEADER_LEN(head) = total;

    return (head);
}




ssize_t
userspace_sctp_sendmbuf(struct socket *so, 
    struct mbuf* mbufdata,
    size_t len,
    struct sockaddr *to,
    socklen_t tolen,
    u_int32_t ppid,
    u_int32_t flags,
    u_int16_t stream_no,
    u_int32_t timetolive,
    u_int32_t context)
{
    
    struct sctp_sndrcvinfo sndrcvinfo, *sinfo = &sndrcvinfo;
    /*    struct uio auio;
          struct iovec iov[1]; */
    int error = 0;
    int uflags = 0;
    int retvalsendmsg;
    
    sinfo->sinfo_ppid = ppid;
    sinfo->sinfo_flags = flags;
    sinfo->sinfo_stream = stream_no;
    sinfo->sinfo_timetolive = timetolive;
    sinfo->sinfo_context = context;
    sinfo->sinfo_assoc_id = 0;
    
    /* Perform error checks on destination (to) */
    if (tolen > SOCK_MAXADDRLEN){
        error = (ENAMETOOLONG);
        goto sendmsg_return;
    }
    if (tolen < offsetof(struct sockaddr, sa_data[0])){
        error = (EINVAL);
        goto sendmsg_return;
    }
    /* Adding the following as part of defensive programming, in case the application
       does not do it when preparing the destination address.*/
#if !defined(__Userspace_os_Linux)
    to->sa_len = tolen;
#endif
    
    error = sctp_lower_sosend(so, to, NULL/*uio*/,
                              (struct mbuf *)mbufdata, (struct mbuf *)NULL,
                              uflags, sinfo);
sendmsg_return:
    /* TODO: Needs a condition for non-blocking when error is EWOULDBLOCK */
    if (0 == error)
        retvalsendmsg = len;
    else if(error == EWOULDBLOCK) {
        errno = EWOULDBLOCK;
        retvalsendmsg = (-1);
    } else {
        printf("%s: error = %d\n", __func__, error);
        errno = error;
        retvalsendmsg = (-1);
    }
    return retvalsendmsg;
    
}

#if 0 /* Old version: To be removed */
/* This is purely experimental for now. It can't handle message sizes larger than MHLEN */
ssize_t userspace_sctp_sendmbuf(struct socket *so, const void *message, size_t length,
       int flags, const struct sockaddr *dest_addr, socklen_t dest_len)
{
    struct mbuf * m;
    struct mbuf * control = NULL;
    struct proc *p = NULL;
        
    if (so == NULL) {
        perror("sockset so passed to userspace_sctp_sendmbuf is NULL\n");
        exit(1);
    }

    /* Just getting a single mbuf for now for a short message.
     * Not appending any packet headers because that would be done
     * in sctp_med_chunk_output, which prepends common sctp header and
     * in sctp_lowlevel_chunk_output which attaches ip header
     */

    m = sctp_get_mbuf_for_msg(MHLEN, 0, M_DONTWAIT, 0, MT_DATA); 

    if (m == NULL){
        perror("out of memory in userspace_sctp_sendmbuf\n");
        exit(1);
    }

    bcopy((caddr_t)message, m->m_data, length);
    SCTP_HEADER_LEN(m) = SCTP_BUF_LEN(m) = length;
    
    return (sctp_sosend(so,
                        (struct sockaddr *) dest_addr,
                        (struct uio *)NULL,
                        m,
                        control,
                        flags,
                        p
			));

}
#endif


/* The original implementation of sctp_generic_recvmsg is in /src/sys/kern/uipc_syscalls.c
 * Modifying it for __Userspace__
 */
#if 0
static int
sctp_generic_recvmsg(so, uap, retval)
	struct socket *so;
	struct sctp_generic_recvmsg_args /* {
		int sd, 
		struct iovec *iov, 
		int iovlen,
		struct sockaddr *from, 
		socklen_t *fromlenaddr,
		struct sctp_sndrcvinfo *sinfo, 
		int *msg_flags
	} */ *uap;
        int *retval;
{
	u_int8_t sockbufstore[256];
	struct uio auio;
	struct iovec *iov, *tiov;
	struct sctp_sndrcvinfo sinfo;
	struct sockaddr *fromsa;
	int fromlen;
	int len, i, msg_flags;
	int error = 0;
	error = copyiniov(uap->iov, uap->iovlen, &iov, EMSGSIZE);
	if (error) {
                return (error);
	}

	if (uap->fromlenaddr) {
		error = copyin(uap->fromlenaddr,
		    &fromlen, sizeof (fromlen));
		if (error) {
			goto out;
		}
	} else {
		fromlen = 0;
	}
	if(uap->msg_flags) {
		error = copyin(uap->msg_flags, &msg_flags, sizeof (int));
		if (error) {
			goto out;
		}
	} else {
		msg_flags = 0;
	}
	auio.uio_iov = iov;
	auio.uio_iovcnt = uap->iovlen;
  	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_offset = 0;			/* XXX */
	auio.uio_resid = 0;
	tiov = iov;
	for (i = 0; i <uap->iovlen; i++, tiov++) {
		if ((auio.uio_resid += tiov->iov_len) < 0) {
			error = EINVAL;
			goto out;
		}
	}
	len = auio.uio_resid;
	fromsa = (struct sockaddr *)sockbufstore;

	error = sctp_sorecvmsg(so, &auio, (struct mbuf **)NULL,
		    fromsa, fromlen, &msg_flags,
		    (struct sctp_sndrcvinfo *)&sinfo, 1);
	if (error) {
		if (auio.uio_resid != (int)len && (error == ERESTART ||
		    error == EINTR || error == EWOULDBLOCK))
			error = 0;
	} else {
		if (uap->sinfo)
			error = copyout(&sinfo, uap->sinfo, sizeof (sinfo));
	}
	if (error)
		goto out;

        /* ready return value */
        /*	td->td_retval[0] = (int)len - auio.uio_resid; original */
        *retval = (int)len - auio.uio_resid;

	if (fromlen && uap->from) {
		len = fromlen;
		if (len <= 0 || fromsa == 0)
			len = 0;
		else {
#if !defined(__Userspace_os_Linux)
			len = min(len, fromsa->sa_len);
#else
			len = min(len, sizeof(*fromsa));
#endif
			error = copyout(fromsa, uap->from, (unsigned)len);
			if (error)
				goto out;
		}
		error = copyout(&len, uap->fromlenaddr, sizeof (socklen_t));
		if (error) {
			goto out;
		}
	}
	if (uap->msg_flags) {
		error = copyout(&msg_flags, uap->msg_flags, sizeof (int));
		if (error) {
			goto out;
		}
	}
out:
	free(iov); /* , M_IOV); */

	return (error);
}
#endif

/* taken from usr.lib/sctp_sys_calls.c and needed here */
#define        SCTP_SMALL_IOVEC_SIZE 2
        
/* Taken from  /src/lib/libc/net/sctp_sys_calls.c
 * and modified for __Userspace__
 * calling sctp_generic_recvmsg from this function
 */
ssize_t
userspace_sctp_recvmsg(struct socket *so,
    void *dbuf,
    size_t len,
    struct sockaddr *from,
    socklen_t * fromlen,
    struct sctp_sndrcvinfo *sinfo,
    int *msg_flags)
{
#if 1 /* def SYS_sctp_generic_recvmsg __Userspace__ */

    	struct uio auio;
        struct iovec iov[SCTP_SMALL_IOVEC_SIZE];
	struct iovec *tiov;
        int iovlen = 1;
	int error = 0;
        int ulen, i, retval;
        
        iov[0].iov_base = dbuf;
	iov[0].iov_len = len;

	auio.uio_iov = iov;
	auio.uio_iovcnt = iovlen;
  	auio.uio_segflg = UIO_USERSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_offset = 0;			/* XXX */
	auio.uio_resid = 0;
	tiov = iov;
	for (i = 0; i <iovlen; i++, tiov++) {
		if ((auio.uio_resid += tiov->iov_len) < 0) {
                    error = EINVAL;
                    printf("%s: error = %d\n", __func__, error);
                    return (-1);
		}
	}
	ulen = auio.uio_resid;
	error = sctp_sorecvmsg(so, &auio, (struct mbuf **)NULL,
		    from, *fromlen, msg_flags,
		    (struct sctp_sndrcvinfo *)sinfo, 1);

	if (error) {
		if (auio.uio_resid != (int)ulen && (error == ERESTART ||
		    error == EINTR || error == EWOULDBLOCK))
			error = 0;
        }
        
        if (0 == error){
        /* ready return value */
            retval = (int)ulen - auio.uio_resid;
            return retval;
        }else{
            printf("%s: error = %d\n", __func__, error);
            return (-1);
        }

     
#if 0 /*  Removed: Old implementation that does unnecessary copying of sinfo and sockaddr */

        struct iovec iov[SCTP_SMALL_IOVEC_SIZE];
        struct sctp_generic_recvmsg_args ua, *uap = &ua;
	ssize_t sz;
        int retval;

	iov[0].iov_base = dbuf;
	iov[0].iov_len = len;

        uap->sd = 0; 
        uap->iov = iov; 
        uap->iovlen = 1;
        uap->from  = from;
        uap->fromlenaddr = fromlen;
        uap->sinfo = sinfo; 
        uap->msg_flags = msg_flags;
        

        sz = sctp_generic_recvmsg(so, uap, &retval);
        if(sz == 0) {
            /* success */
            return retval;
        }
        
        /* error */
        errno = sz;
        return (-1);
#endif
        
#else
                
	struct sctp_sndrcvinfo *s_info;
	ssize_t sz;
	int sinfo_found = 0;
	struct msghdr msg;
	struct iovec iov[SCTP_SMALL_IOVEC_SIZE];
	char controlVector[SCTP_CONTROL_VEC_SIZE_RCV];
	struct cmsghdr *cmsg;

	if (msg_flags == NULL) {
		errno = EINVAL;
		return (-1);
	}
	msg.msg_flags = 0;
	iov[0].iov_base = dbuf;
	iov[0].iov_len = len;
	iov[1].iov_base = NULL;
	iov[1].iov_len = 0;
	msg.msg_name = (caddr_t)from;
	if (fromlen == NULL)
		msg.msg_namelen = 0;
	else
		msg.msg_namelen = *fromlen;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = (caddr_t)controlVector;
	msg.msg_controllen = sizeof(controlVector);
	errno = 0;
	sz = recvmsg(s, &msg, *msg_flags);
	if (sz <= 0)
		return (sz);

	s_info = NULL;
	len = sz;
	*msg_flags = msg.msg_flags;
	if (sinfo)
		sinfo->sinfo_assoc_id = 0;

	if ((msg.msg_controllen) && sinfo) {
		/*
		 * parse through and see if we find the sctp_sndrcvinfo (if
		 * the user wants it).
		 */
		cmsg = (struct cmsghdr *)controlVector;
		while (cmsg) {
			if ((cmsg->cmsg_len == 0) || (cmsg->cmsg_len > msg.msg_controllen)) {
				break;
			}
			if (cmsg->cmsg_level == IPPROTO_SCTP) {
				if (cmsg->cmsg_type == SCTP_SNDRCV) {
					/* Got it */
					s_info = (struct sctp_sndrcvinfo *)CMSG_DATA(cmsg);
					/* Copy it to the user */
					if (sinfo)
						*sinfo = *s_info;
					sinfo_found = 1;
					break;
				} else if (cmsg->cmsg_type == SCTP_EXTRCV) {
					/*
					 * Got it, presumably the user has
					 * asked for this extra info, so the
					 * structure holds more room :-D
					 */
					s_info = (struct sctp_sndrcvinfo *)CMSG_DATA(cmsg);
					/* Copy it to the user */
					if (sinfo) {
						memcpy(sinfo, s_info, sizeof(struct sctp_extrcvinfo));
					}
					sinfo_found = 1;
					break;

				}
			}
			cmsg = CMSG_NXTHDR(&msg, cmsg);
		}
	}
	return (sz);
#endif
}



#if defined(__Userspace__)
/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 * socreate returns a socket.  The socket should be
 * closed with soclose().
 */
int
socreate(int dom, struct socket **aso, int type, int proto)
{
	struct socket *so;
	int error;

        assert((AF_INET == dom) || (AF_LOCAL == dom));
        assert((SOCK_STREAM == type) || (SOCK_SEQPACKET == type));
        assert(IPPROTO_SCTP == proto);

	so = soalloc();
	if (so == NULL)
		return (ENOBUFS);

        /*
         * so_incomp represents a queue of connections that
         * must be completed at protocol level before being
         * returned. so_comp field heads a list of sockets
         * that are ready to be returned to the listening process
         *__Userspace__ These queues are being used at a number of places like accept etc.
         */
	TAILQ_INIT(&so->so_incomp);
	TAILQ_INIT(&so->so_comp);
	so->so_type = type;
	so->so_count = 1;
	/*
	 * Auto-sizing of socket buffers is managed by the protocols and
	 * the appropriate flags must be set in the pru_attach function.
         * For __Userspace__ The pru_attach function in this case is sctp_attach.
	 */
	error = sctp_attach(so, proto, SCTP_DEFAULT_VRFID);
	if (error) {
		assert(so->so_count == 1);
		so->so_count = 0;
		sodealloc(so);
		return (error);
	}
	*aso = so;
	return (0);
}
#else
/* The kernel version for reference is below. The #else
   should be removed once the __Userspace__
   version is tested.
 * socreate returns a socket with a ref count of 1.  The socket should be
 * closed with soclose().
 */
int
socreate(int dom, struct socket **aso, int type, int proto,
    struct ucred *cred, struct thread *td)
{
	struct protosw *prp;
	struct socket *so;
	int error;

	if (proto)
		prp = pffindproto(dom, proto, type);
	else
		prp = pffindtype(dom, type);

	if (prp == NULL || prp->pr_usrreqs->pru_attach == NULL ||
	    prp->pr_usrreqs->pru_attach == pru_attach_notsupp)
		return (EPROTONOSUPPORT);

	if (jailed(cred) && jail_socket_unixiproute_only &&
	    prp->pr_domain->dom_family != PF_LOCAL &&
	    prp->pr_domain->dom_family != PF_INET &&
	    prp->pr_domain->dom_family != PF_ROUTE) {
		return (EPROTONOSUPPORT);
	}

	if (prp->pr_type != type)
		return (EPROTOTYPE);
	so = soalloc();
	if (so == NULL)
		return (ENOBUFS);

	TAILQ_INIT(&so->so_incomp);
	TAILQ_INIT(&so->so_comp);
	so->so_type = type;
	so->so_cred = crhold(cred);
	so->so_proto = prp;
#ifdef MAC
	mac_create_socket(cred, so);
#endif
	knlist_init(&so->so_rcv.sb_sel.si_note, SOCKBUF_MTX(&so->so_rcv),
	    NULL, NULL, NULL);
	knlist_init(&so->so_snd.sb_sel.si_note, SOCKBUF_MTX(&so->so_snd),
	    NULL, NULL, NULL);
	so->so_count = 1;
	/*
	 * Auto-sizing of socket buffers is managed by the protocols and
	 * the appropriate flags must be set in the pru_attach function.
	 */
	error = (*prp->pr_usrreqs->pru_attach)(so, proto, td);
	if (error) {
		KASSERT(so->so_count == 1, ("socreate: so_count %d",
		    so->so_count));
		so->so_count = 0;
		sodealloc(so);
		return (error);
	}
	*aso = so;
	return (0);
}
#endif




/* Taken from  /src/sys/kern/uipc_syscalls.c
 * and modified for __Userspace__
 * Removing struct thread td.
 */
struct socket *
userspace_socket(int domain, int type, int protocol)
{
	struct socket *so = NULL;
	int error;
        
	error = socreate(domain, &so, type, protocol);
	if (error) {
		perror("In user_socket(): socreate failed\n");
                exit(1);
	}
	/*
         * The original socket call returns the file descriptor fd.
         * td->td_retval[0] = fd.
         * We are returning struct socket *so.
         */
	return (so);
}


u_long	sb_max = SB_MAX;
u_long sb_max_adj =
       SB_MAX * MCLBYTES / (MSIZE + MCLBYTES); /* adjusted sb_max */

static	u_long sb_efficiency = 8;	/* parameter for sbreserve() */

#if defined (__Userspace__)
/*
 * Allot mbufs to a sockbuf.  Attempt to scale mbmax so that mbcnt doesn't
 * become limiting if buffering efficiency is near the normal case.
 */
int
sbreserve_locked(struct sockbuf *sb, u_long cc, struct socket *so)
{
    	SOCKBUF_LOCK_ASSERT(sb);
	sb->sb_mbmax = min(cc * sb_efficiency, sb_max);
	if (sb->sb_lowat > sb->sb_hiwat)
            sb->sb_lowat = sb->sb_hiwat;
	return (1);
}
#else /* kernel version for reference */
/*
 * Allot mbufs to a sockbuf.  Attempt to scale mbmax so that mbcnt doesn't
 * become limiting if buffering efficiency is near the normal case.
 */
int
sbreserve_locked(struct sockbuf *sb, u_long cc, struct socket *so,
    struct thread *td)
{
	rlim_t sbsize_limit;

	SOCKBUF_LOCK_ASSERT(sb);

	/*
	 * td will only be NULL when we're in an interrupt (e.g. in
	 * tcp_input()).
	 *
	 * XXXRW: This comment needs updating, as might the code.
	 */
	if (cc > sb_max_adj)
		return (0);
	if (td != NULL) {
		PROC_LOCK(td->td_proc);
		sbsize_limit = lim_cur(td->td_proc, RLIMIT_SBSIZE);
		PROC_UNLOCK(td->td_proc);
	} else
		sbsize_limit = RLIM_INFINITY;
	if (!chgsbsize(so->so_cred->cr_uidinfo, &sb->sb_hiwat, cc,
	    sbsize_limit))
		return (0);
	sb->sb_mbmax = min(cc * sb_efficiency, sb_max);
	if (sb->sb_lowat > sb->sb_hiwat)
		sb->sb_lowat = sb->sb_hiwat;
	return (1);
}
#endif



#if defined(__Userspace__)
int
soreserve(struct socket *so, u_long sndcc, u_long rcvcc)
{

    SOCKBUF_LOCK(&so->so_snd);
    SOCKBUF_LOCK(&so->so_rcv);
    so->so_snd.sb_hiwat = sndcc;
    so->so_rcv.sb_hiwat = rcvcc;
    
    if (sbreserve_locked(&so->so_snd, sndcc, so) == 0)
        goto bad;
    if (sbreserve_locked(&so->so_rcv, rcvcc, so) == 0)
        goto bad;
    if (so->so_rcv.sb_lowat == 0)
        so->so_rcv.sb_lowat = 1;
    if (so->so_snd.sb_lowat == 0)
        so->so_snd.sb_lowat = MCLBYTES;
    if (so->so_snd.sb_lowat > so->so_snd.sb_hiwat)
        so->so_snd.sb_lowat = so->so_snd.sb_hiwat;
    SOCKBUF_UNLOCK(&so->so_rcv);
    SOCKBUF_UNLOCK(&so->so_snd);
    return (0);

 bad:
    SOCKBUF_UNLOCK(&so->so_rcv);
    SOCKBUF_UNLOCK(&so->so_snd);    
    return (ENOBUFS);
}
#else /* kernel version for reference */
int
soreserve(struct socket *so, u_long sndcc, u_long rcvcc)
{
	struct thread *td = curthread;

	SOCKBUF_LOCK(&so->so_snd);
	SOCKBUF_LOCK(&so->so_rcv);
	if (sbreserve_locked(&so->so_snd, sndcc, so, td) == 0)
		goto bad;
	if (sbreserve_locked(&so->so_rcv, rcvcc, so, td) == 0)
		goto bad2;
	if (so->so_rcv.sb_lowat == 0)
		so->so_rcv.sb_lowat = 1;
	if (so->so_snd.sb_lowat == 0)
		so->so_snd.sb_lowat = MCLBYTES;
	if (so->so_snd.sb_lowat > so->so_snd.sb_hiwat)
		so->so_snd.sb_lowat = so->so_snd.sb_hiwat;
	SOCKBUF_UNLOCK(&so->so_rcv);
	SOCKBUF_UNLOCK(&so->so_snd);
	return (0);
bad2:
	sbrelease_locked(&so->so_snd, so);
bad:
	SOCKBUF_UNLOCK(&so->so_rcv);
	SOCKBUF_UNLOCK(&so->so_snd);
	return (ENOBUFS);
}
#endif





/* Taken from  /src/sys/kern/uipc_sockbuf.c
 * and modified for __Userspace__
 */

#if defined(__Userspace__)
void
sowakeup(struct socket *so, struct sockbuf *sb)
{

	SOCKBUF_LOCK_ASSERT(sb);

	sb->sb_flags &= ~SB_SEL;
	if (sb->sb_flags & SB_WAIT) {
		sb->sb_flags &= ~SB_WAIT;
		pthread_cond_signal(&(sb)->sb_cond);
	}
	SOCKBUF_UNLOCK(sb);
        /*__Userspace__ what todo about so_upcall?*/

}
#else /* kernel version for reference */
/*
 * Wakeup processes waiting on a socket buffer.  Do asynchronous notification
 * via SIGIO if the socket has the SS_ASYNC flag set.
 *
 * Called with the socket buffer lock held; will release the lock by the end
 * of the function.  This allows the caller to acquire the socket buffer lock
 * while testing for the need for various sorts of wakeup and hold it through
 * to the point where it's no longer required.  We currently hold the lock
 * through calls out to other subsystems (with the exception of kqueue), and
 * then release it to avoid lock order issues.  It's not clear that's
 * correct.
 */
void
sowakeup(struct socket *so, struct sockbuf *sb)
{

	SOCKBUF_LOCK_ASSERT(sb);

	selwakeuppri(&sb->sb_sel, PSOCK);
	sb->sb_flags &= ~SB_SEL;
	if (sb->sb_flags & SB_WAIT) {
		sb->sb_flags &= ~SB_WAIT;
		wakeup(&sb->sb_cc);
	}
	KNOTE_LOCKED(&sb->sb_sel.si_note, 0);
	SOCKBUF_UNLOCK(sb);
	if ((so->so_state & SS_ASYNC) && so->so_sigio != NULL)
		pgsigio(&so->so_sigio, SIGIO, 0);
	if (sb->sb_flags & SB_UPCALL)
		(*so->so_upcall)(so, so->so_upcallarg, M_DONTWAIT);
	if (sb->sb_flags & SB_AIO)
		aio_swake(so, sb);
	mtx_assert(SOCKBUF_MTX(sb), MA_NOTOWNED);
}
#endif



/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 */

int
sobind(struct socket *so, struct sockaddr *nam)
{

	return (sctp_bind(so, nam));
}


/* Taken from  /src/sys/kern/uipc_syscalls.c
 * kern_bind modified for __Userspace__
 */

int
user_bind(so, sa)
     struct socket *so;
     struct sockaddr *sa;
{
	int error;
	error = sobind(so, sa);
	return (error);
}

/* Taken from  /src/sys/kern/uipc_syscalls.c
 * and modified for __Userspace__
 */

int
userspace_bind(so, name, namelen)
     struct socket *so;
     struct sockaddr *name;
     int	namelen;
    
{
	struct sockaddr *sa;
	int error;

	if ((error = getsockaddr(&sa, (caddr_t)name, namelen)) != 0)
		return (error);

	error = user_bind(so, sa);
	FREE(sa, M_SONAME);
	return (error);
}



/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 */

int
solisten(struct socket *so, int backlog)
{

	return (sctp_listen(so, backlog, NULL));
}


int
solisten_proto_check(struct socket *so)
{

	SOCK_LOCK_ASSERT(so);

	if (so->so_state & (SS_ISCONNECTED | SS_ISCONNECTING |
	    SS_ISDISCONNECTING))
		return (EINVAL);
	return (0);
}

static int somaxconn = SOMAXCONN;

void
solisten_proto(struct socket *so, int backlog)
{

	SOCK_LOCK_ASSERT(so);

	if (backlog < 0 || backlog > somaxconn)
		backlog = somaxconn;
	so->so_qlimit = backlog;
	so->so_options |= SO_ACCEPTCONN;
}




/* Taken from  /src/sys/kern/uipc_syscalls.c
 * and modified for __Userspace__
 */

int
userspace_listen(so, backlog)
     struct socket *so;
     int backlog;

{
	int error;

        error = solisten(so, backlog);
        
	return(error);
}


/* Taken from  /src/sys/kern/uipc_socket.c
 * and modified for __Userspace__
 */

int
soaccept(struct socket *so, struct sockaddr **nam)
{
	int error;

	SOCK_LOCK(so);
	KASSERT((so->so_state & SS_NOFDREF) != 0, ("soaccept: !NOFDREF"));
	so->so_state &= ~SS_NOFDREF;
	SOCK_UNLOCK(so);
	error = sctp_accept(so, nam);
	return (error);
}



/* Taken from  /src/sys/kern/uipc_syscalls.c
 * kern_accept modified for __Userspace__
 */
int
user_accept(struct socket *aso,  struct sockaddr **name, socklen_t *namelen, struct socket **ptr_accept_ret_sock)
{
	struct sockaddr *sa = NULL;
	int error;
	struct socket *head = aso;
        struct socket *so;
	

	if (name) {
		*name = NULL;
		if (*namelen < 0)
			return (EINVAL);
	}

	if ((head->so_options & SO_ACCEPTCONN) == 0) {
		error = EINVAL;
		goto done;
	}

	ACCEPT_LOCK();
	if ((head->so_state & SS_NBIO) && TAILQ_EMPTY(&head->so_comp)) {
		ACCEPT_UNLOCK();
		error = EWOULDBLOCK;
		goto noconnection;
	}
	while (TAILQ_EMPTY(&head->so_comp) && head->so_error == 0) {
		if (head->so_rcv.sb_state & SBS_CANTRCVMORE) {
			head->so_error = ECONNABORTED;
			break;
		}
		error = pthread_cond_wait(&accept_cond, &accept_mtx);
		if (error) {
			ACCEPT_UNLOCK();
			goto noconnection;
		}
	}
	if (head->so_error) {
		error = head->so_error;
		head->so_error = 0;
		ACCEPT_UNLOCK();
		goto noconnection;
	}
	so = TAILQ_FIRST(&head->so_comp);
	KASSERT(!(so->so_qstate & SQ_INCOMP), ("accept1: so SQ_INCOMP"));
	KASSERT(so->so_qstate & SQ_COMP, ("accept1: so not SQ_COMP"));

	/*
	 * Before changing the flags on the socket, we have to bump the
	 * reference count.  Otherwise, if the protocol calls sofree(),
	 * the socket will be released due to a zero refcount.
	 */
	SOCK_LOCK(so);			/* soref() and so_state update */
	soref(so);			/* file descriptor reference */

	TAILQ_REMOVE(&head->so_comp, so, so_list);
	head->so_qlen--;
	so->so_state |= (head->so_state & SS_NBIO);
	so->so_qstate &= ~SQ_COMP;
	so->so_head = NULL;

	SOCK_UNLOCK(so);
	ACCEPT_UNLOCK();


        /*
         * The original accept returns fd value via td->td_retval[0] = fd;
         * we will return the socket for accepted connection.
         */

	sa = 0;
	error = soaccept(so, &sa);
	if (error) {
		/*
		 * return a namelen of zero for older code which might
		 * ignore the return value from accept.
		 */
		if (name)
			*namelen = 0;
		goto noconnection;
	}
	if (sa == NULL) {
		if (name)
			*namelen = 0;
		goto done;
	}
	if (name) {
#if !defined(__Userspace_os_Linux)
		/* check sa_len before it is destroyed */
		if (*namelen > sa->sa_len)
			*namelen = sa->sa_len;
#endif
		*name = sa;
		sa = NULL;                    
	}
noconnection:
	if (sa)
            FREE(sa, M_SONAME);


done:
        *ptr_accept_ret_sock = so;
	return (error);
}



/* Taken from  /src/sys/kern/uipc_syscalls.c
 * and modified for __Userspace__
 */
/*
 * accept1()
 */
static int
accept1(so, aname, anamelen, ptr_accept_ret_sock)
     struct socket *so;
     struct sockaddr * aname; 
     socklen_t * anamelen;
     struct socket **ptr_accept_ret_sock;
{
	struct sockaddr *name;
	socklen_t namelen;
	int error;

	if (aname == NULL)
		return (user_accept(so, NULL, NULL, ptr_accept_ret_sock));

	error = copyin(anamelen, &namelen, sizeof (namelen));
	if (error)
		return (error);

	error = user_accept(so, &name, &namelen, ptr_accept_ret_sock);

	/*
	 * return a namelen of zero for older code which might
	 * ignore the return value from accept.
	 */
	if (error) {
		(void) copyout(&namelen,
		    anamelen, sizeof(*anamelen));
		return (error);
	}

	if (error == 0 && name != NULL) {
            
		error = copyout(name, aname, namelen);
	}
	if (error == 0)
		error = copyout(&namelen, anamelen,
		    sizeof(namelen));

        if(name)
            FREE(name, M_SONAME);    
	return (error);
}



struct socket *
userspace_accept(so, aname, anamelen)
     struct socket *so;	
     struct sockaddr * aname; 
     socklen_t * anamelen;
{
    int error;
    struct socket *accept_return_sock;
    error = accept1(so, aname, anamelen, &accept_return_sock);
    if(error)
        printf("%s: error=%d\n",__func__, error); /* should we exit here in case of error? */
    return (accept_return_sock);
}



int
sodisconnect(struct socket *so)
{
	int error;

	if ((so->so_state & SS_ISCONNECTED) == 0)
		return (ENOTCONN);
	if (so->so_state & SS_ISDISCONNECTING)
		return (EALREADY);
	error = sctp_disconnect(so);
	return (error);
}


int
soconnect(struct socket *so, struct sockaddr *nam)
{
	int error;

	if (so->so_options & SO_ACCEPTCONN)
		return (EOPNOTSUPP);
	/*
	 * If protocol is connection-based, can only connect once.
	 * Otherwise, if connected, try to disconnect first.  This allows
	 * user to disconnect by connecting to, e.g., a null address.
	 */
	if (so->so_state & (SS_ISCONNECTED|SS_ISCONNECTING) &&
            (error = sodisconnect(so))) {
		error = EISCONN;
	} else {
		/*
		 * Prevent accumulated error from previous connection from
		 * biting us.
		 */
		so->so_error = 0;
		error = sctp_connect(so, nam);
	}

	return (error);
}



int user_connect(so, sa)
     struct socket *so;
     struct sockaddr *sa;
{
	int error;
	int interrupted = 0;

	if (so->so_state & SS_ISCONNECTING) {
		error = EALREADY;
		goto done1;
	}

	error = soconnect(so, sa);
	if (error)
		goto bad;
	if ((so->so_state & SS_NBIO) && (so->so_state & SS_ISCONNECTING)) {
		error = EINPROGRESS;
		goto done1;
	}

	SOCK_LOCK(so);
	while ((so->so_state & SS_ISCONNECTING) && so->so_error == 0) {
		error = pthread_cond_wait(SOCK_COND(so), SOCK_MTX(so));
		if (error) {
			if (error == EINTR || error == ERESTART)
				interrupted = 1;
			break;
		}
	}
	if (error == 0) {
		error = so->so_error;
		so->so_error = 0;
	}
	SOCK_UNLOCK(so);

bad:
	if (!interrupted)
		so->so_state &= ~SS_ISCONNECTING;
	if (error == ERESTART)
		error = EINTR;
done1:
	return (error);
}



int userspace_connect(so, name, namelen)
     struct socket *so;
     struct sockaddr *name;
     int namelen;
{

	struct sockaddr *sa;
	int error;

	error = getsockaddr(&sa, (caddr_t)name, namelen);
	if (error)
		return (error);

	error = user_connect(so, sa);
	FREE(sa, M_SONAME);
	return (error);

}

void userspace_close(struct socket *so) {
        ACCEPT_LOCK();
        SOCK_LOCK(so);
        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
        sorele(so);
        /*printf("%s::%s:%d\n", __FILE__, __FUNCTION__, __LINE__);*/
}

/* needed from sctp_usrreq.c */
int
sctp_setopt(struct socket *so, int optname, void *optval, size_t optsize, void *p);
int
userspace_setsockopt(struct socket *so, int level, int option_name,
                     const void *option_value, socklen_t option_len)
{
    return (sctp_setopt(so, option_name, (void *) option_value, option_len, NULL));
}

/* needed from sctp_usrreq.c */
int
sctp_getopt(struct socket *so, int optname, void *optval, size_t *optsize,
	    void *p);
int
userspace_getsockopt(struct socket *so, int level, int option_name,
                     void *option_value, socklen_t option_len)
{
	return (sctp_getopt(so, option_name, option_value, (size_t*)&option_len, NULL));
}

#if 1 /* using iovec to sendmsg */
/* "result" is mapped to an errno if something bad occurs */
void sctp_userspace_ip_output(int *result, struct mbuf *o_pak,
                                            struct route *ro, void *stcb,
                                            uint32_t vrf_id)
{
	const int MAXLEN_MBUF_CHAIN = 32; /* What should this value be? */
	struct iovec send_iovec[MAXLEN_MBUF_CHAIN];
	struct mbuf *m;
	struct mbuf *m_orig;
	int iovcnt;
	int send_len;
	int len;
	int send_count;
	int i;
	struct ip *ip;
	struct udphdr *udp;
	int res;
	struct sockaddr_in dst;
	struct msghdr msg_hdr;
	int use_udp_tunneling;
	
	*result = 0;
	i=0;
	iovcnt = 0;
	send_count = 0;

	m = SCTP_HEADER_TO_CHAIN(o_pak);
	m_orig = m;

	len = sizeof(struct ip);
	if (SCTP_BUF_LEN(m) < len) {
		if ((m = m_pullup(m, len)) == 0) {
			printf("Can not get the IP header in the first mbuf.\n");
			return;
		}
	}
	ip = mtod(m, struct ip *);
	use_udp_tunneling = (ip->ip_p == IPPROTO_UDP);

	if (use_udp_tunneling) {
		len = sizeof(struct ip) + sizeof(struct udphdr);
		if (SCTP_BUF_LEN(m) < len) {
			if ((m = m_pullup(m, len)) == 0) {
				printf("Can not get the UDP/IP header in the first mbuf.\n");
				return;
			}
			ip = mtod(m, struct ip *);
		}
		udp = (struct udphdr *)(ip + 1);
	}

	if (!use_udp_tunneling) {
		if (ip->ip_src.s_addr == INADDR_ANY) {
			/* TODO get addr of outgoing interface */
			printf("Why did the SCTP implementation did not choose a source address?\n");
		}
		/* TODO need to worry about ro->ro_dst as in ip_output? */
#if defined(__Userspace_os_Linux)
		/* need to put certain fields into network order for Linux */
		ip->ip_len = htons(ip->ip_len);
		ip->ip_tos = htons(ip->ip_tos);
		ip->ip_off = 0;
#endif
	}

	memset((void *)&dst, 0, sizeof(struct sockaddr_in));
	dst.sin_family = AF_INET;
	dst.sin_addr.s_addr = ip->ip_dst.s_addr;
#if !defined(__Userspace_os_Linux)
	dst.sin_len = sizeof(struct sockaddr_in); 
#endif
	if (use_udp_tunneling) {
		dst.sin_port = udp->uh_dport;
	} else {
		dst.sin_port = 0;
	}

	/*tweak the mbuf chain */
	if (use_udp_tunneling) {
		m_adj(m, sizeof(struct ip) + sizeof(struct udphdr));
	}

	send_len = SCTP_HEADER_LEN(m); /* length of entire packet */
	send_count = 0;
	do {
		send_iovec[i].iov_base = (caddr_t)m->m_data;
		send_iovec[i].iov_len = SCTP_BUF_LEN(m);
		send_count += send_iovec[i].iov_len;
		iovcnt++;
		i++;
		m = m->m_next;
	} while(m);
	assert(send_count == send_len);
	msg_hdr.msg_name = (struct sockaddr *) &dst;
	msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
	msg_hdr.msg_iov = send_iovec;
	msg_hdr.msg_iovlen = iovcnt;
	msg_hdr.msg_control = NULL;
	msg_hdr.msg_controllen = 0;
	msg_hdr.msg_flags = 0;

	if ((!use_udp_tunneling) && (userspace_rawsctp > -1)) {
		if ((res = sendmsg(userspace_rawsctp, &msg_hdr, MSG_DONTWAIT)) != send_len) {
			*result = errno;
		}
	}
	if ((use_udp_tunneling) && (userspace_udpsctp > -1)) {
		if ((res = sendmsg(userspace_udpsctp, &msg_hdr, MSG_DONTWAIT)) != send_len) {
			*result = errno;
		}
	}
	sctp_m_freem(m_orig);
}

#else  /* old version of sctp_userspace_ip_output that makes a copy using pack_send_buffer */
/*extern int pack_send_buffer(caddr_t buffer, struct mbuf* mb, int len); */
extern int pack_send_buffer(caddr_t buffer, struct mbuf* mb);

/* "result" is mapped to an errno if something bad occurs */
void sctp_userspace_ip_output(int *result, struct mbuf *o_pak,
                                            struct route *ro, void *stcb,
                                            uint32_t vrf_id)
{ 
    struct mbuf;
    struct ip *ip;
    struct sctphdr *sh;
    struct udphdr *udp;
    struct sockaddr_in dst;
    int o_flgs = 0, res;
    /*    const int hdrincl = 1; comment when using separate receive thread */
    char *ptr;
    char *send_buf = NULL, *psend_buf = NULL;
    send_buf = (char*) malloc(SCTP_DEFAULT_MAXSEGMENT);
    if(NULL == send_buf){
        perror("malloc failure");
        exit(1);
    }        
    psend_buf = mtod(o_pak, char *);
    int send_len = SCTP_BUF_LEN(o_pak);
    printf("%s:%d send_len=%d\n", __FUNCTION__, __LINE__, send_len);
    int count_copied;
    
    *result = 0;

#if 0 /*  raw socket is being created in recv_thread_init() */
    /* use raw socket, create if not initialized */
    if (userspace_rawsctp == -1) {
        if ((userspace_rawsctp = socket(AF_INET, SOCK_RAW, IPPROTO_SCTP)) < 0) {
            *result = errno;
        }
	if (setsockopt(userspace_rawsctp, IPPROTO_IP, IP_HDRINCL, &hdrincl, sizeof(int)) < 0) {
            *result = errno;
	}
    }
#endif
    
    if (userspace_rawsctp > -1 || userspace_udpsctp > -1) {
        
        /* may not all be in head mbuf */
        if (o_pak->m_flags & M_PKTHDR &&
            o_pak->m_next &&
            MHLEN > send_len) {
            /* m_pullup currently causes assertion failure on second iteration
             *  of sending an INIT, so for now, copy chain into a contiguous
             *  buffer manually.
             */
            /*o_pak = m_pullup(o_pak, SCTP_BUF_LEN(o_pak) + SCTP_BUF_LEN(o_pak->m_next)); */
            
            send_len = SCTP_HEADER_LEN(o_pak); /*  need the full chain len... */

            /*            count_copied = pack_send_buffer(send_buf, o_pak, send_len);
                          assert(count_copied == send_len); */
            send_len = count_copied = pack_send_buffer(send_buf, o_pak);
            printf("%s:%d send_len=%d\n", __FUNCTION__, __LINE__, send_len);
            psend_buf = send_buf;
        }

        /* TODO if m_next or M_EXT still exists, use iovec */
        
        ip = (struct ip *) psend_buf;
	ptr = (char *) ip;
	ptr += sizeof(struct ip);
        bzero(&dst, sizeof(dst));
        dst.sin_family = AF_INET; 
        dst.sin_addr.s_addr = ip->ip_dst.s_addr;
#if !defined(__Userspace_os_Linux)
        dst.sin_len = sizeof(dst); 
#endif

        if(ip->ip_p == IPPROTO_UDP) {
            /* if the upper layer protocol is UDP then take away the IP and UDP header
             *  and send the remaining stuff over the UDP socket.
             */
            udp = (struct udphdr *) ptr;
            ptr += sizeof(struct udphdr);

            dst.sin_port = udp->uh_dport;
            
            /* sendto call to UDP socket and do error handling */
            if((res = sendto (userspace_udpsctp, ptr, send_len - (sizeof(struct udphdr) + sizeof(struct ip)),
                              o_flgs,(struct sockaddr *) &dst,
                              sizeof(struct sockaddr_in)))
               != send_len - (sizeof(struct udphdr) + sizeof(struct ip))) {
                *result = errno;
            }

            
        } else {
            /* doing SCTP over IP so use the raw socket... */
            
            sh = (struct sctphdr *) ptr;

            if(sh->dest_port == 0) {
                SCTPDBG(SCTP_DEBUG_OUTPUT1, "Sending %d bytes to port 0! Assigning port...\n", send_len);
                dst.sin_port = htons(8898); /* OOTB only - arbitrary TODO assign available port */
            } else {
                SCTPDBG(SCTP_DEBUG_OUTPUT1, "Sending %d bytes to supplied non-zero port!\n", send_len);
                dst.sin_port = sh->dest_port;
            }

            if (ip->ip_src.s_addr == INADDR_ANY) {
                /* TODO get addr of outgoing interface */
            }

            /* TODO IP handles fragmentation? */
            
            /* TODO need to worry about ro->ro_dst as in ip_output? */
            
#if defined(__Userspace_os_Linux)
            /* need to put certain fields into network order for Linux */
            struct ip *iphdr;
            iphdr = (struct ip *) psend_buf;
            iphdr->ip_len = htons(iphdr->ip_len);
            iphdr->ip_tos = htons(iphdr->ip_tos);
            iphdr->ip_off = 0; /* when is this non-zero!?? TODO - FIX THIS HACK... */
#endif

            if((res = sendto (userspace_rawsctp, psend_buf, send_len,
                              o_flgs,(struct sockaddr *) &dst,
                              sizeof(struct sockaddr_in)))
               != send_len) {
                *result = errno;
            }
        }
    }
    
    if(psend_buf)
        free(psend_buf);
}
#endif

