/*
 *  __Userspace__ version of /usr/src/sys/kern/kern_mbuf.c
 *  We are initializing two zones for Mbufs and Clusters.
 *  
 */

#include <stdio.h>
#include <string.h>
/* #include <sys/param.h> This defines MSIZE 256 */
#include <assert.h>
#include <sys/queue.h>
#if !defined(SCTP_SIMPLE_ALLOCATOR)
#include "umem.h"
//#include "user_include/umem_impl.h"
#endif
#include "user_mbuf.h"
#include "user_environment.h"
#include "user_atomic.h"

struct mbstat mbstat;
#define KIPC_MAX_LINKHDR        4       /* int: max length of link header (see sys/sysclt.h) */
#define	KIPC_MAX_PROTOHDR	5	/* int: max length of network header (see sys/sysclt.h)*/
int     max_linkhdr = KIPC_MAX_LINKHDR;
int	max_protohdr = KIPC_MAX_PROTOHDR; /* Size of largest protocol layer header. */

/*
 * Zones from which we allocate.
 */
sctp_zone_t	zone_mbuf;
sctp_zone_t	zone_clust;
sctp_zone_t	zone_ext_refcnt;

/*__Userspace__
 * constructor callback_data 
 * mbuf_mb_args will be passed as callback data to umem_cache_create.
 * umem_cache_alloc will then be able to use this callback data when the constructor 
 * function mb_ctor_mbuf is called. See user_mbuf.c
 * This is important because mbuf_mb_args would specify flags like M_PKTHDR
 * and type like MT_DATA or MT_HEADER. This information is needed in mb_ctor_mbuf
 * to properly initialize the mbuf being allocated.
 *
 * Argument structure passed to UMA routines during mbuf and packet
 * allocations.
 */
struct mb_args mbuf_mb_args;

/* __Userspace__ clust_mb_args will be passed as callback data to mb_ctor_clust 
 * and mb_dtor_clust.
 * Note: I had to use struct clust_args as an encapsulation for an mbuf pointer.
 * struct mbuf * clust_mb_args; does not work.
 */
struct clust_args clust_mb_args;


/* __Userspace__
 * Local prototypes.
 */
static int	mb_ctor_mbuf(void *, void *, int);
static int      mb_ctor_clust(void *, void *, int);
static void	mb_dtor_mbuf(void *,  void *);
static void	mb_dtor_clust(void *, void *);


/***************** Functions taken from user_mbuf.h *************/

/* __Userspace__  Setter function for mbuf_mb_args */
static void set_mbuf_mb_args(int flags, short type) {
	mbuf_mb_args.flags = flags;
	mbuf_mb_args.type = type;
}
#if USING_MBUF_CONSTRUCTOR
/* __Userspace__  Setter function for clust_mb_args */
static void set_clust_mb_args(struct mbuf * mb) {
	clust_mb_args.parent_mbuf = mb;
}
#endif

static int mbuf_constructor_dup(struct mbuf *m, int pkthdr, short type)
{
	int flags = pkthdr;
	if (type == MT_NOINIT)
		return (0);
	
	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_len = 0;
	m->m_flags = flags;
	m->m_type = type;
	if (flags & M_PKTHDR) {
		m->m_data = m->m_pktdat;
		m->m_pkthdr.rcvif = NULL;
		m->m_pkthdr.len = 0;
		m->m_pkthdr.header = NULL;
		m->m_pkthdr.csum_flags = 0;
		m->m_pkthdr.csum_data = 0;
		m->m_pkthdr.tso_segsz = 0;
		m->m_pkthdr.ether_vtag = 0;
		SLIST_INIT(&m->m_pkthdr.tags);
	} else
		m->m_data = m->m_dat;
	
	return (0);
}

/* __Userspace__ */
struct mbuf *
m_get(int how, short type)
{
	struct mbuf *mret;
	/* The following setter function is not yet being enclosed within
	 * #if USING_MBUF_CONSTRUCTOR - #endif, until I have thoroughly tested
	 * mb_dtor_mbuf. See comment there
	 */
	set_mbuf_mb_args(0, type);
	
	/* Mbuf master zone, zone_mbuf, has already been 
	 * created in mbuf_init() */
	mret = SCTP_ZONE_GET(zone_mbuf, struct mbuf);
#if defined(SCTP_SIMPLE_ALLOCATOR)
	mb_ctor_mbuf(mret, &mbuf_mb_args, 0);
#endif
	/*mret =  ((struct mbuf *)umem_cache_alloc(zone_mbuf, UMEM_DEFAULT));*/
	
	/* There are cases when an object available in the current CPU's
	 * loaded magazine and in those cases the object's constructor is not applied.
	 * If that is the case, then we are duplicating constructor initialization here,
	 * so that the mbuf is properly constructed before returning it.
	 */
	if (mret) {
#if USING_MBUF_CONSTRUCTOR
		if (! (mret->m_type == type) ) {
			mbuf_constructor_dup(mret, 0, type);
		}
#else            
		mbuf_constructor_dup(mret, 0, type);
#endif
		
	}
	return mret;
}


/* __Userspace__ */
struct mbuf *
m_gethdr(int how, short type)
{
	struct mbuf *mret;
	/* The following setter function is not yet being enclosed within
	 * #if USING_MBUF_CONSTRUCTOR - #endif, until I have thoroughly tested
	 * mb_dtor_mbuf. See comment there
	 */
	set_mbuf_mb_args(M_PKTHDR, type);
	
	mret = SCTP_ZONE_GET(zone_mbuf, struct mbuf);
#if defined(SCTP_SIMPLE_ALLOCATOR)
	mb_ctor_mbuf(mret, &mbuf_mb_args, 0);
#endif
	/*mret = ((struct mbuf *)umem_cache_alloc(zone_mbuf, UMEM_DEFAULT));*/
	/* There are cases when an object available in the current CPU's
	 * loaded magazine and in those cases the object's constructor is not applied.
	 * If that is the case, then we are duplicating constructor initialization here,
	 * so that the mbuf is properly constructed before returning it.
	 */
	if (mret) {
#if USING_MBUF_CONSTRUCTOR
		if (! ((mret->m_flags & M_PKTHDR) && (mret->m_type == type)) ) {
			mbuf_constructor_dup(mret, M_PKTHDR, type);
		}
#else
		mbuf_constructor_dup(mret, M_PKTHDR, type);
#endif
	}
	return mret;
}

/* __Userspace__ */
struct mbuf *
m_free(struct mbuf *m)
{
	
	struct mbuf *n = m->m_next;
	
	if (m->m_flags & M_EXT)
		mb_free_ext(m);
	else if ((m->m_flags & M_NOFREE) == 0) {
#if defined(SCTP_SIMPLE_ALLOCATOR)
		mb_dtor_mbuf(m, &mbuf_mb_args);
#endif
		SCTP_ZONE_FREE(zone_mbuf, m);
	}
		/*umem_cache_free(zone_mbuf, m);*/
	return (n);
}


static int clust_constructor_dup(caddr_t m_clust, struct mbuf* m)
{
	u_int *refcnt;
	int type, size;
	sctp_zone_t zone;
	
	/* Assigning cluster of MCLBYTES. TODO: Add jumbo frame functionality */
	type = EXT_CLUSTER;
	zone = zone_clust;
	size = MCLBYTES;
	
	refcnt = SCTP_ZONE_GET(zone_ext_refcnt, u_int);
	/*refcnt = (u_int *)umem_cache_alloc(zone_ext_refcnt, UMEM_DEFAULT);*/
	if (refcnt == NULL) {
		printf("calling reap in %s\n", __func__);
#if !defined(SCTP_SIMPLE_ALLOCATOR)
		umem_reap();
#endif
		refcnt = SCTP_ZONE_GET(zone_ext_refcnt, u_int);
		/*refcnt = (u_int *)umem_cache_alloc(zone_ext_refcnt, UMEM_DEFAULT);*/
		assert(refcnt != NULL);
	}
	*refcnt = 1;
	if (m != NULL) {
		m->m_ext.ext_buf = (caddr_t)m_clust;
		m->m_data = m->m_ext.ext_buf;
		m->m_flags |= M_EXT;
		m->m_ext.ext_free = NULL;
		m->m_ext.ext_args = NULL;
		m->m_ext.ext_size = size; 
		m->m_ext.ext_type = type;
		m->m_ext.ref_cnt = refcnt;
	}
	
	return (0);
}



/* __Userspace__ */
void
m_clget(struct mbuf *m, int how)
{
	caddr_t mclust_ret;
	if (m->m_flags & M_EXT)
		printf("%s: %p mbuf already has cluster\n", __func__, m);
	m->m_ext.ext_buf = (char *)NULL;
#if USING_MBUF_CONSTRUCTOR
	set_clust_mb_args(m);
#endif
	mclust_ret = SCTP_ZONE_GET(zone_clust, char);
#if defined(SCTP_SIMPLE_ALLOCATOR)
	mb_ctor_clust(mclust_ret, &clust_mb_args, 0);
#endif
	/*mclust_ret = umem_cache_alloc(zone_clust, UMEM_DEFAULT);*/
	/* 
	 On a cluster allocation failure, call umem_reap() and retry.
	 */
	
	if ((mclust_ret == NULL)) {
		printf("calling reap in %s\n", __func__);

#if !defined(SCTP_SIMPLE_ALLOCATOR)
	/*	mclust_ret = SCTP_ZONE_GET(zone_clust, char);
		mb_ctor_clust(mclust_ret, &clust_mb_args, 0);
#else*/
		umem_reap();
		mclust_ret = SCTP_ZONE_GET(zone_clust, char);
#endif
		/*mclust_ret = umem_cache_alloc(zone_clust, UMEM_DEFAULT);*/
		if(NULL == mclust_ret)
		{
			printf("Memory allocation failure in %s\n", __func__);
			exit(1);
		}
	}
	
#if USING_MBUF_CONSTRUCTOR
	if ((m->m_ext.ext_buf == NULL)) {
		clust_constructor_dup(mclust_ret, m);
	}
#else
	clust_constructor_dup(mclust_ret, m);
#endif
}

/*
 * Unlink a tag from the list of tags associated with an mbuf.
 */
static __inline void
m_tag_unlink(struct mbuf *m, struct m_tag *t)
{
	
	SLIST_REMOVE(&m->m_pkthdr.tags, t, m_tag, m_tag_link);
}

/*
 * Reclaim resources associated with a tag.
 */
static __inline void
m_tag_free(struct m_tag *t)
{
	
	(*t->m_tag_free)(t);
}

/*
 * Set up the contents of a tag.  Note that this does not fill in the free
 * method; the caller is expected to do that.
 *
 * XXX probably should be called m_tag_init, but that was already taken.
 */
static __inline void
m_tag_setup(struct m_tag *t, u_int32_t cookie, int type, int len)
{
	
	t->m_tag_id = type;
	t->m_tag_len = len;
	t->m_tag_cookie = cookie;
}

/************ End functions from user_mbuf.h  ******************/



/************ End functions to substitute umem_cache_alloc and umem_cache_free **************/

/* __Userspace__ 
 * TODO: mbuf_init must be called in the initialization routines
 * of userspace stack. 
 */
void
mbuf_init(void *dummy)
{

	/*
	 * __Userspace__Configure UMA zones for Mbufs and Clusters. 
	 * (TODO: m_getcl() - using packet secondary zone).
	 * There is no provision for trash_init and trash_fini in umem. 
	 * 
	 */
 /* zone_mbuf = umem_cache_create(MBUF_MEM_NAME, MSIZE, 0,
				mb_ctor_mbuf, mb_dtor_mbuf, NULL, 
				&mbuf_mb_args,
				NULL, 0);
	zone_mbuf = umem_cache_create(MBUF_MEM_NAME, MSIZE, 0, NULL, NULL, NULL, NULL, NULL, 0);*/
#if defined(SCTP_SIMPLE_ALLOCATOR)
	SCTP_ZONE_INIT(zone_mbuf, MBUF_MEM_NAME, MSIZE, 0);
#else
	zone_mbuf = umem_cache_create(MBUF_MEM_NAME, MSIZE, 0,
								  mb_ctor_mbuf, mb_dtor_mbuf, NULL, 
								  &mbuf_mb_args,
								  NULL, 0);
#endif
	/*zone_ext_refcnt = umem_cache_create(MBUF_EXTREFCNT_MEM_NAME, sizeof(u_int), 0,
				NULL, NULL, NULL, 
				NULL,
				NULL, 0);*/
	SCTP_ZONE_INIT(zone_ext_refcnt, MBUF_EXTREFCNT_MEM_NAME, sizeof(u_int), 0);
  
  /*zone_clust = umem_cache_create(MBUF_CLUSTER_MEM_NAME, MCLBYTES, 0,
				 mb_ctor_clust, mb_dtor_clust, NULL,
				 &clust_mb_args,
				 NULL, 0);
	zone_clust = umem_cache_create(MBUF_CLUSTER_MEM_NAME, MCLBYTES, 0, NULL, NULL, NULL, NULL, NULL,0);*/
#if defined(SCTP_SIMPLE_ALLOCATOR)
	SCTP_ZONE_INIT(zone_clust, MBUF_CLUSTER_MEM_NAME, MCLBYTES, 0);
#else
	zone_clust = umem_cache_create(MBUF_CLUSTER_MEM_NAME, MCLBYTES, 0,
								   mb_ctor_clust, mb_dtor_clust, NULL,
								   &clust_mb_args,
								   NULL, 0);
#endif

	/* uma_prealloc() goes here... */

	/* __Userspace__ Add umem_reap here for low memory situation?
	 *
	 */
	 

	/*
	 * [Re]set counters and local statistics knobs.
	 *  
	 */
	  
	mbstat.m_mbufs = 0;
	mbstat.m_mclusts = 0;
	mbstat.m_drain = 0;
	mbstat.m_msize = MSIZE;
	mbstat.m_mclbytes = MCLBYTES;
	mbstat.m_minclsize = MINCLSIZE;
	mbstat.m_mlen = MLEN;
	mbstat.m_mhlen = MHLEN;
	mbstat.m_numtypes = MT_NTYPES;

	mbstat.m_mcfail = mbstat.m_mpfail = 0;
	mbstat.sf_iocnt = 0;
	mbstat.sf_allocwait = mbstat.sf_allocfail = 0;
	  
}



/*
 * __Userspace__
 *
 * Constructor for Mbuf master zone. We have a different constructor
 * for allocating the cluster.
 *
 * The 'arg' pointer points to a mb_args structure which
 * contains call-specific information required to support the
 * mbuf allocation API.  See user_mbuf.h.
 *
 * The flgs parameter below can be UMEM_DEFAULT or UMEM_NOFAIL depending on what
 * was passed when umem_cache_alloc was called.
 * TODO: Use UMEM_NOFAIL in umem_cache_alloc and also define a failure handler
 * and call umem_nofail_callback(my_failure_handler) in the stack initialization routines
 * The advantage of using UMEM_NOFAIL is that we don't have to check if umem_cache_alloc 
 * was successful or not. The failure handler would take care of it, if we use the UMEM_NOFAIL
 * flag.
 * 
 * NOTE Ref: http://docs.sun.com/app/docs/doc/819-2243/6n4i099p2?l=en&a=view&q=umem_zalloc)
 * The umem_nofail_callback() function sets the **process-wide** UMEM_NOFAIL callback.
 * It also mentions that umem_nofail_callback is Evolving.
 * 
 */
static int
mb_ctor_mbuf(void *mem, void *arg, int flgs)
{
#if USING_MBUF_CONSTRUCTOR 
	struct mbuf *m;
	struct mb_args *args;

	int flags;
	short type;

	m = (struct mbuf *)mem;
	args = (struct mb_args *)arg;
	flags = args->flags;
	type = args->type;

	/*
	 * The mbuf is initialized later. 
	 * 
	 */
	if (type == MT_NOINIT)
		return (0);

	m->m_next = NULL;
	m->m_nextpkt = NULL;
	m->m_len = 0;
	m->m_flags = flags;
	m->m_type = type;
	if (flags & M_PKTHDR) {
		m->m_data = m->m_pktdat;
		m->m_pkthdr.rcvif = NULL;
		m->m_pkthdr.len = 0;
		m->m_pkthdr.header = NULL;
		m->m_pkthdr.csum_flags = 0;
		m->m_pkthdr.csum_data = 0;
		m->m_pkthdr.tso_segsz = 0;
		m->m_pkthdr.ether_vtag = 0;
		SLIST_INIT(&m->m_pkthdr.tags);
	} else
		m->m_data = m->m_dat;
#endif
	return (0);
}


/*
 * __Userspace__
 * The Mbuf master zone destructor.
 * This would be called in response to umem_cache_destroy
 * TODO: Recheck if this is what we want to do in this destructor.
 * (Note: the number of times mb_dtor_mbuf is called is equal to the
 * number of individual mbufs allocated from zone_mbuf.
 */
static void
mb_dtor_mbuf(void *mem, void *arg)
{

	struct mbuf *m;
	struct mb_args *args;
	int flags;

	m = (struct mbuf *)mem;
	args = (struct mb_args *)arg;
	flags = args->flags;
	
	if ((flags & MB_NOTAGS) == 0 && (m->m_flags & M_PKTHDR) != 0)
	  {
		m_tag_delete_chain(m, NULL);
	  }
	assert((m->m_flags & M_EXT) == 0);
	assert((m->m_flags & M_NOFREE) == 0);	

}


/* __Userspace__
 * The Cluster zone constructor.
 *
 * Here the 'arg' pointer points to the Mbuf which we
 * are configuring cluster storage for.  If 'arg' is
 * empty we allocate just the cluster without setting
 * the mbuf to it.  See mbuf.h.
 */
static int
mb_ctor_clust(void *mem, void *arg, int flgs)
{
    
#if USING_MBUF_CONSTRUCTOR 
	struct mbuf *m;
	struct clust_args * cla;
	u_int *refcnt;
	int type, size;
	sctp_zone_t zone;
	
	/* Assigning cluster of MCLBYTES. TODO: Add jumbo frame functionality */
	type = EXT_CLUSTER;
	zone = zone_clust;
	size = MCLBYTES;

	cla = (struct clust_args *)arg;
	m = cla->parent_mbuf;

	refcnt = SCTP_ZONE_GET(zone_ext_refcnt, u_int);
	/*refcnt = (u_int *)umem_cache_alloc(zone_ext_refcnt, UMEM_DEFAULT);*/
	*refcnt = 1;

	if (m != NULL) {
		m->m_ext.ext_buf = (caddr_t)mem;
		m->m_data = m->m_ext.ext_buf;
		m->m_flags |= M_EXT;
		m->m_ext.ext_free = NULL;
		m->m_ext.ext_args = NULL;
		m->m_ext.ext_size = size; 
		m->m_ext.ext_type = type;
		m->m_ext.ref_cnt = refcnt;
	}
#endif
	return (0);
}

/* __Userspace__ */
static void
mb_dtor_clust(void *mem, void *arg)
{
    
  /* mem is of type caddr_t.  In sys/types.h we have typedef char * caddr_t;  */
  /* mb_dtor_clust is called at time of umem_cache_destroy() (the number of times
   * mb_dtor_clust is called is equal to the number of individual mbufs allocated 
   * from zone_clust. Similarly for mb_dtor_mbuf).
   * At this point the following:
   *  struct mbuf *m;
   *   m = (struct mbuf *)arg;
   *  assert (*(m->m_ext.ref_cnt) == 0); is not meaningful since  m->m_ext.ref_cnt = NULL;
   *  has been done in mb_free_ext().
   */

}




/* Unlink and free a packet tag. */
void
m_tag_delete(struct mbuf *m, struct m_tag *t)
{
  
	assert(m && t);
	m_tag_unlink(m, t);
	m_tag_free(t);
}


/* Unlink and free a packet tag chain, starting from given tag. */
void
m_tag_delete_chain(struct mbuf *m, struct m_tag *t)
{
  
	struct m_tag *p, *q;

	assert(m);
	if (t != NULL)
		p = t;
	else
		p = SLIST_FIRST(&m->m_pkthdr.tags);
	if (p == NULL)
		return;
	while ((q = SLIST_NEXT(p, m_tag_link)) != NULL)
		m_tag_delete(m, q);
	m_tag_delete(m, p);
}

#if 0
static void
sctp_print_mbuf_chain(struct mbuf *m)
{
	printf("Printing mbuf chain %p.\n", m);
	for(; m; m=m->m_next) {
		printf("%p: m_len = %ld, m_type = %x, m_next = %p.\n", m, m->m_len, m->m_type, m->m_next);
		if (m->m_flags & M_EXT)
			printf("%p: extend_size = %d, extend_buffer = %p, ref_cnt = %d.\n", m, m->m_ext.ext_size, m->m_ext.ext_buf, *(m->m_ext.ref_cnt));
	}  
}
#endif

/*
 * Free an entire chain of mbufs and associated external buffers, if
 * applicable.
 */
void
m_freem(struct mbuf *mb)
{
	while (mb != NULL)
		mb = m_free(mb);
}

/*
 * __Userspace__
 * clean mbufs with M_EXT storage attached to them 
 * if the reference count hits 1.
 */
void
mb_free_ext(struct mbuf *m)
{
  
	int skipmbuf;
	
	assert((m->m_flags & M_EXT) == M_EXT);
	assert(m->m_ext.ref_cnt != NULL);

	/*
	 * check if the header is embedded in the cluster
	 */     
	skipmbuf = (m->m_flags & M_NOFREE);

	/* Free the external attached storage if this 
	 * mbuf is the only reference to it. 
	 *__Userspace__ TODO: jumbo frames
	 * 
	*/
        /* NOTE: We had the same code that SCTP_DECREMENT_AND_CHECK_REFCOUNT
                 reduces to here before but the IPHONE malloc commit had changed
                 this to compare to 0 instead of 1 (see next line).  Why?

                 ... this caused a huge memory leak in Linux.
         */
#ifdef IPHONE
        if (atomic_fetchadd_int(m->m_ext.ref_cnt, -1) == 0)
#else
        if (SCTP_DECREMENT_AND_CHECK_REFCOUNT(m->m_ext.ref_cnt))
#endif
        {    
		if (m->m_ext.ext_type == EXT_CLUSTER){
#if defined(SCTP_SIMPLE_ALLOCATOR)
			mb_dtor_clust(m->m_ext.ext_buf, &clust_mb_args);
#endif
			SCTP_ZONE_FREE(zone_clust, m->m_ext.ext_buf);
			SCTP_ZONE_FREE(zone_ext_refcnt, (u_int*)m->m_ext.ref_cnt);
			m->m_ext.ref_cnt = NULL;
		}
        }
        
	if (skipmbuf)
		return;

	
	/* __Userspace__ Also freeing the storage for ref_cnt
	 * Free this mbuf back to the mbuf zone with all m_ext
	 * information purged.
	 */
	m->m_ext.ext_buf = NULL;
	m->m_ext.ext_free = NULL;
	m->m_ext.ext_args = NULL;
	m->m_ext.ref_cnt = NULL;
	m->m_ext.ext_size = 0;
	m->m_ext.ext_type = 0;
	m->m_flags &= ~M_EXT;
#if defined(SCTP_SIMPLE_ALLOCATOR)
	mb_dtor_mbuf(m, &mbuf_mb_args);
#endif
	SCTP_ZONE_FREE(zone_mbuf, m);
	
	/*umem_cache_free(zone_mbuf, m);*/
}

/*
 * "Move" mbuf pkthdr from "from" to "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 */
void
m_move_pkthdr(struct mbuf *to, struct mbuf *from)
{

	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;		/* especially tags */
	SLIST_INIT(&from->m_pkthdr.tags);	/* purge tags from src */
	from->m_flags &= ~M_PKTHDR;
}


/*
 * Rearange an mbuf chain so that len bytes are contiguous
 * and in the data area of an mbuf (so that mtod and dtom
 * will work for a structure of size len).  Returns the resulting
 * mbuf chain on success, frees it and returns null on failure.
 * If there is room, it will add up to max_protohdr-len extra bytes to the
 * contiguous region in an attempt to avoid being called next time.
 */
struct mbuf *
m_pullup(struct mbuf *n, int len)
{
	struct mbuf *m;
	int count;
	int space;

	/*
	 * If first mbuf has no cluster, and has room for len bytes
	 * without shifting current data, pullup into it,
	 * otherwise allocate a new mbuf to prepend to the chain.
	 */
	if ((n->m_flags & M_EXT) == 0 &&
	    n->m_data + len < &n->m_dat[MLEN] && n->m_next) {
            if (n->m_len >= len)
			return (n);
		m = n;
		n = n->m_next;
		len -= m->m_len;
	} else {
		if (len > MHLEN)
			goto bad;
		MGET(m, M_DONTWAIT, n->m_type);
		if (m == NULL)
			goto bad;
		m->m_len = 0;
		if (n->m_flags & M_PKTHDR)
			M_MOVE_PKTHDR(m, n);
	}
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		  (u_int)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
bad:
	m_freem(n);
        mbstat.m_mpfail++;	/* XXX: No consistency. */
	return (NULL);
}


/*
 * Attach the the cluster from *m to *n, set up m_ext in *n
 * and bump the refcount of the cluster.
 */
static void
mb_dupcl(struct mbuf *n, struct mbuf *m)
{
	assert((m->m_flags & M_EXT) == M_EXT);
	assert(m->m_ext.ref_cnt != NULL);
	assert((n->m_flags & M_EXT) == 0);

	if (*(m->m_ext.ref_cnt) == 1)
            *(m->m_ext.ref_cnt) += 1;
	else
            atomic_add_int(m->m_ext.ref_cnt, 1);
	n->m_ext.ext_buf = m->m_ext.ext_buf;
	n->m_ext.ext_free = m->m_ext.ext_free;
	n->m_ext.ext_args = m->m_ext.ext_args;
	n->m_ext.ext_size = m->m_ext.ext_size;
	n->m_ext.ref_cnt = m->m_ext.ref_cnt;
	n->m_ext.ext_type = m->m_ext.ext_type;
	n->m_flags |= M_EXT;
}


/*
 * Make a copy of an mbuf chain starting "off0" bytes from the beginning,
 * continuing for "len" bytes.  If len is M_COPYALL, copy to end of mbuf.
 * The wait parameter is a choice of M_TRYWAIT/M_DONTWAIT from caller.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 */

struct mbuf *
m_copym(struct mbuf *m, int off0, int len, int wait)
{
	struct mbuf *n, **np;
	int off = off0;
	struct mbuf *top;
	int copyhdr = 0;

	assert(off >= 0);
	assert(len >= 0);
     
	if (off == 0 && m->m_flags & M_PKTHDR)
		copyhdr = 1;
	while (off > 0) {
		assert(m != NULL);
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	np = &top;
	top = 0;
	while (len > 0) {
		if (m == NULL) {
		  assert(len == M_COPYALL);
		  break;
		}
		if (copyhdr)
			MGETHDR(n, wait, m->m_type);
		else
			MGET(n, wait, m->m_type);
		*np = n;
		if (n == NULL)
			goto nospace;
		if (copyhdr) {
			if (!m_dup_pkthdr(n, m, wait))
				goto nospace;
			if (len == M_COPYALL)
				n->m_pkthdr.len -= off0;
			else
				n->m_pkthdr.len = len;
			copyhdr = 0;
		}
		n->m_len = min(len, m->m_len - off);
		if (m->m_flags & M_EXT) {
			n->m_data = m->m_data + off;
			mb_dupcl(n, m);
		} else
			bcopy(mtod(m, caddr_t)+off, mtod(n, caddr_t),
			    (u_int)n->m_len);
		if (len != M_COPYALL)
			len -= n->m_len;
		off = 0;
		m = m->m_next;
		np = &n->m_next;
	}
	if (top == NULL)
            mbstat.m_mcfail++;	/* XXX: No consistency. */

	return (top);
nospace:
	m_freem(top);
	mbstat.m_mcfail++;	/* XXX: No consistency. */
	return (NULL);
}


int
m_tag_copy_chain(struct mbuf *to, struct mbuf *from, int how)
{
	struct m_tag *p, *t, *tprev = NULL;

	assert(to && from);
	m_tag_delete_chain(to, NULL);
	SLIST_FOREACH(p, &from->m_pkthdr.tags, m_tag_link) {
		t = m_tag_copy(p, how);
		if (t == NULL) {
			m_tag_delete_chain(to, NULL);
			return 0;
		}
		if (tprev == NULL)
			SLIST_INSERT_HEAD(&to->m_pkthdr.tags, t, m_tag_link);
		else
			SLIST_INSERT_AFTER(tprev, t, m_tag_link);
		tprev = t;
	}
	return 1;
}

/*
 * Duplicate "from"'s mbuf pkthdr in "to".
 * "from" must have M_PKTHDR set, and "to" must be empty.
 * In particular, this does a deep copy of the packet tags.
 */
int
m_dup_pkthdr(struct mbuf *to, struct mbuf *from, int how)
{

	to->m_flags = (from->m_flags & M_COPYFLAGS) | (to->m_flags & M_EXT);
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;
	SLIST_INIT(&to->m_pkthdr.tags);
	return (m_tag_copy_chain(to, from, MBTOM(how)));
}

/* Copy a single tag. */
struct m_tag *
m_tag_copy(struct m_tag *t, int how)
{
	struct m_tag *p;

	assert(t);
	p = m_tag_alloc(t->m_tag_cookie, t->m_tag_id, t->m_tag_len, how);
	if (p == NULL)
		return (NULL);
	bcopy(t + 1, p + 1, t->m_tag_len); /* Copy the data */
	return p;
}

/* Get a packet tag structure along with specified data following. */
struct m_tag *
m_tag_alloc(u_int32_t cookie, int type, int len, int wait)
{
	struct m_tag *t;

	if (len < 0)
		return NULL;
	t = malloc(len + sizeof(struct m_tag));
	if (t == NULL)
		return NULL;
	m_tag_setup(t, cookie, type, len);
	t->m_tag_free = m_tag_free_default;
	return t;
}

/* Free a packet tag. */
void
m_tag_free_default(struct m_tag *t)
{
  free(t);
}

/*
 * Copy data from a buffer back into the indicated mbuf chain,
 * starting "off" bytes from the beginning, extending the mbuf
 * chain if necessary.
 */
void
m_copyback(struct mbuf *m0, int off, int len, c_caddr_t cp)
{
	int mlen;
	struct mbuf *m = m0, *n;
	int totlen = 0;

	if (m0 == NULL)
		return;
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == NULL) {
			n = m_get(M_DONTWAIT, m->m_type);
			if (n == NULL)
				goto out;
			bzero(mtod(n, caddr_t), MLEN);
			n->m_len = min(MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	while (len > 0) {
		mlen = min (m->m_len - off, len);
		bcopy(cp, off + mtod(m, caddr_t), (u_int)mlen);
		cp += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == NULL) {
			n = m_get(M_DONTWAIT, m->m_type);
			if (n == NULL)
				break;
			n->m_len = min(MLEN, len);
			m->m_next = n;
		}
		m = m->m_next;
	}
out:	if (((m = m0)->m_flags & M_PKTHDR) && (m->m_pkthdr.len < totlen))
		m->m_pkthdr.len = totlen;
}


/*
 * Lesser-used path for M_PREPEND:
 * allocate new mbuf to prepend to chain,
 * copy junk along.
 */
struct mbuf *
m_prepend(struct mbuf *m, int len, int how)
{
	struct mbuf *mn;

	if (m->m_flags & M_PKTHDR)
		MGETHDR(mn, how, m->m_type);
	else
		MGET(mn, how, m->m_type);
	if (mn == NULL) {
		m_freem(m);
		return (NULL);
	}
	if (m->m_flags & M_PKTHDR)
		M_MOVE_PKTHDR(mn, m);
	mn->m_next = m;
	m = mn;
	if(m->m_flags & M_PKTHDR) {
		if (len < MHLEN)
			MH_ALIGN(m, len);
	} else {
		if (len < MLEN) 
			M_ALIGN(m, len);
	}
	m->m_len = len;
	return (m);
}

/*
 * Copy data from an mbuf chain starting "off" bytes from the beginning,
 * continuing for "len" bytes, into the indicated buffer.
 */
void
m_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	u_int count;

	assert(off >= 0);
	assert(len >= 0);
	while (off > 0) {
                assert(m != NULL);
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
                assert(m != NULL);
		count = min(m->m_len - off, len);
		bcopy(mtod(m, caddr_t) + off, cp, count);
		len -= count;
		cp += count;
		off = 0;
		m = m->m_next;
	}
}


/*
 * Concatenate mbuf chain n to m.
 * Both chains must be of the same type (e.g. MT_DATA).
 * Any m_pkthdr is not updated.
 */
void
m_cat(struct mbuf *m, struct mbuf *n)
{
        while (m->m_next)
                m = m->m_next;
        while (n) {
                if (m->m_flags & M_EXT ||
                    m->m_data + m->m_len + n->m_len >= &m->m_dat[MLEN]) {
                        /* just join the two chains */
                        m->m_next = n;
                        return;
                }
                /* splat the data from one into the other */
                bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
                    (u_int)n->m_len);
                m->m_len += n->m_len;
                n = m_free(n);
        }
}


void
m_adj(struct mbuf *mp, int req_len)
{
	int len = req_len;
	struct mbuf *m;
	int count;

	if ((m = mp) == NULL)
		return;
	if (len >= 0) {
		/*
		 * Trim from head.
		 */
		while (m != NULL && len > 0) {
			if (m->m_len <= len) {
				len -= m->m_len;
				m->m_len = 0;
				m = m->m_next;
			} else {
				m->m_len -= len;
				m->m_data += len;
				len = 0;
			}
		}
		m = mp;
		if (mp->m_flags & M_PKTHDR)
			m->m_pkthdr.len -= (req_len - len);
	} else {
		/*
		 * Trim from tail.  Scan the mbuf chain,
		 * calculating its length and finding the last mbuf.
		 * If the adjustment only affects this mbuf, then just
		 * adjust and return.  Otherwise, rescan and truncate
		 * after the remaining size.
		 */
		len = -len;
		count = 0;
		for (;;) {
			count += m->m_len;
			if (m->m_next == (struct mbuf *)0)
				break;
			m = m->m_next;
		}
		if (m->m_len >= len) {
			m->m_len -= len;
			if (mp->m_flags & M_PKTHDR)
				mp->m_pkthdr.len -= len;
			return;
		}
		count -= len;
		if (count < 0)
			count = 0;
		/*
		 * Correct length for chain is "count".
		 * Find the mbuf with last data, adjust its length,
		 * and toss data from remaining mbufs on chain.
		 */
		m = mp;
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len = count;
		for (; m; m = m->m_next) {
			if (m->m_len >= count) {
				m->m_len = count;
				if (m->m_next != NULL) {
					m_freem(m->m_next);
					m->m_next = NULL;
				}
				break;
			}
			count -= m->m_len;
		}
	}
}


/* m_split is used within sctp_handle_cookie_echo. */

/*
 * Partition an mbuf chain in two pieces, returning the tail --
 * all but the first len0 bytes.  In case of failure, it returns NULL and
 * attempts to restore the chain to its original state.
 *
 * Note that the resulting mbufs might be read-only, because the new
 * mbuf can end up sharing an mbuf cluster with the original mbuf if
 * the "breaking point" happens to lie within a cluster mbuf. Use the
 * M_WRITABLE() macro to check for this case.
 */
struct mbuf *
m_split(struct mbuf *m0, int len0, int wait)
{
	struct mbuf *m, *n;
	u_int len = len0, remain;

	/* MBUF_CHECKSLEEP(wait); */
	for (m = m0; m && len > m->m_len; m = m->m_next)
		len -= m->m_len;
	if (m == NULL)
		return (NULL);
	remain = m->m_len - len;
	if (m0->m_flags & M_PKTHDR) {
		MGETHDR(n, wait, m0->m_type);
		if (n == NULL)
			return (NULL);
		n->m_pkthdr.rcvif = m0->m_pkthdr.rcvif;
		n->m_pkthdr.len = m0->m_pkthdr.len - len0;
		m0->m_pkthdr.len = len0;
		if (m->m_flags & M_EXT)
			goto extpacket;
		if (remain > MHLEN) {
			/* m can't be the lead packet */
			MH_ALIGN(n, 0);
			n->m_next = m_split(m, len, wait);
			if (n->m_next == NULL) {
				(void) m_free(n);
				return (NULL);
			} else {
				n->m_len = 0;
				return (n);
			}
		} else
			MH_ALIGN(n, remain);
	} else if (remain == 0) {
		n = m->m_next;
		m->m_next = NULL;
		return (n);
	} else {
		MGET(n, wait, m->m_type);
		if (n == NULL)
			return (NULL);
		M_ALIGN(n, remain);
	}
extpacket:
	if (m->m_flags & M_EXT) {
		n->m_data = m->m_data + len;
		mb_dupcl(n, m);
	} else {
		bcopy(mtod(m, caddr_t) + len, mtod(n, caddr_t), remain);
	}
	n->m_len = remain;
	m->m_len = len;
	n->m_next = m->m_next;
	m->m_next = NULL;
	return (n);
}




int pack_send_buffer(caddr_t buffer, struct mbuf* mb){

    int count_to_copy;
    int total_count_copied = 0;
    int offset = 0;
    do{
        count_to_copy = mb->m_len;
        bcopy(mtod(mb, caddr_t), buffer+offset, count_to_copy);
        offset += count_to_copy;
        total_count_copied += count_to_copy;
        mb = mb->m_next;
    }while(mb);

    return (total_count_copied);
}
