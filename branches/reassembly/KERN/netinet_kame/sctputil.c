/*	$KAME$	*/
/*	Header: /home/sctpBsd/netinet/sctputil.c,v 1.153 2002/04/04 16:59:01 randall Exp	*/

/*
 * Copyright (c) 2001, 2002 Cisco Systems, Inc.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Cisco Systems, Inc.
 * 4. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CISCO SYSTEMS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL CISCO SYSTEMS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef __OpenBSD__
#include "opt_ipsec.h"
#endif
#if defined(__FreeBSD__) && __FreeBSD__ >= 3
#include "opt_compat.h"
#include "opt_inet6.h"
#include "opt_inet.h"
#endif
#if defined(__NetBSD__)
#include "opt_inet.h"
#endif
#ifndef __OpenBSD__
#include "opt_sctp.h"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#if defined (__OpenBSD__)
#include <netinet/sctp_callout.h>
#else
#include <sys/callout.h>
#endif
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#ifdef INET6
#ifndef __OpenBSD__
#include <sys/domain.h>
#endif
#endif

#include <machine/limits.h>

#if defined(__FreeBSD__)
#include <vm/vm_zone.h>
#endif

#if defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/pool.h>
#endif

#include <net/if.h>
#include <net/if_types.h>
#include <net/route.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>

#ifdef INET6
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>

#if defined(__FreeBSD__) || (__NetBSD__)
#include <netinet6/in6_pcb.h>
#elif defined(__OpenBSD__) || (defined(__bsdi__) && _BSDI_VERSION >= 199802)
#include <netinet/in_pcb.h>
#endif

#endif /* INET6 */

#include "faith.h"

#include <netinet/sctp_pcb.h>

#ifdef IPSEC
#ifndef __OpenBSD__
#include <netinet6/ipsec.h>
#include <netkey/key.h>
#endif
#endif /* IPSEC */

#include <netinet/sctputil.h>
#include <netinet/sctp_var.h>
#include <netinet6/sctp6_var.h>
#include <netinet/sctp_header.h>
#include <netinet/sctp_output.h>
#include <netinet/sctp_hashdriver.h>
#include <netinet/sctp_uio.h>
#include <netinet/sctp_timer.h>
#ifndef SCTP_USE_ADLER32
#include <netinet/sctp_crc32.h>
#endif /* SCTP_USE_ADLER32 */

#define NUMBER_OF_MTU_SIZES 18


#ifdef SCTP_DEBUG
extern u_int32_t sctp_debug_on;
#endif


/*
 * a list of sizes based on typical mtu's, used only if next hop
 * size not returned.
 */
static int sctp_mtu_sizes[] = {
	68,
	296,
	508,
	512,
	544,
	576,
	1006,
	1492,
	1500,
	1536,
	2002,
	2048,
	4352,
	4464,
	8166,
	17914,
	32000,
	65535
};


int
find_next_best_mtu(int totsz)
{
	int i, perfer;
	/*
	 * if we are in here we must find the next best fit based on the
	 * size of the dg that failed to be sent.
	 */
	perfer = 0;
	for (i=0; i<NUMBER_OF_MTU_SIZES; i++) {
		if (totsz < sctp_mtu_sizes[i]) {
			perfer = i - 1;
			if (perfer < 0)
				perfer = 0;
			break;
		}
	}
	return(sctp_mtu_sizes[perfer]);
}

void
sctp_fill_random_store(struct sctp_pcb *m)
{
	/*
	 * Here we use the MD5/SHA-1 to hash with our good randomNumbers
	 * and our counter. The result becomes our good random numbers and
	 * we then setup to give these out.
	 */
	m->store_at = 0;
	sctp_hash_digest((char *)m->random_numbers, sizeof(m->random_numbers),
			 (char *)&m->random_counter, sizeof(m->random_counter),
			 (char *)m->random_store);
	m->random_counter++;
}

u_int32_t sctp_select_initial_TSN(struct sctp_pcb *m)
{
	/*
	 * A true implementation should use random selection process to
	 * get the initial stream sequence number, using RFC1750 as a
	 * good guideline
	 */
	u_long x, *xp;
	unsigned char *p;

	if (m->initial_sequence_debug != 0) {
		u_int32_t ret;
		ret = m->initial_sequence_debug;
		m->initial_sequence_debug++;
		return(ret);
	}
	if ((m->store_at+sizeof(u_long)) > SCTP_SIGNATURE_SIZE) {
		/* Refill the random store */
		sctp_fill_random_store(m);
	}
	p = &m->random_store[(int)m->store_at];
	xp = (u_long *)p;
	x = *xp;
	m->store_at += sizeof(u_long);
	return(x);
}

u_int32_t sctp_select_a_tag(struct sctp_inpcb *m)
{
	u_long x, not_done;
	struct timeval now;

	SCTP_GETTIME_TIMEVAL(&now);
	not_done = 1;
	while (not_done) {
		x = sctp_select_initial_TSN(&m->sctp_ep);
		if (sctp_is_vtag_good(m, x, &now)) {
			not_done = 0;
		}
	}
	return(x);
}


void
sctp_init_asoc(struct sctp_inpcb *m, struct sctp_association *asoc,
	       int for_a_init)
{
	/*
	 * Anything set to zero is taken care of by the allocation
	 * routine's bzero
	 */

	/* Up front select what scoping to apply on addresses I tell my peer
	 * Not sure what to do with these right now, we will need to come up
	 * with a way to set them. We may need to pass them through from the
	 * caller in the sctp_aloc_assoc() function.
	 */
	int i;
	/* init all variables to a known value.*/
	asoc->max_burst = m->sctp_ep.max_burst;
	asoc->heart_beat_delay = m->sctp_ep.sctp_timeoutticks[SCTP_TIMER_HEARTBEAT];
	asoc->cookie_life = m->sctp_ep.def_cookie_life;

	asoc->my_vtag = sctp_select_a_tag(m);
	asoc->asconf_seq_out = asoc->init_seq_number = asoc->sending_seq =
		sctp_select_initial_TSN(&m->sctp_ep);
	asoc->t3timeout_highest_marked = asoc->asconf_seq_out;
	asoc->peer_supports_asconf = 1;
	asoc->peer_supports_asconf_setprim = 1;

	/* This will need to be adjusted */
	asoc->last_cwr_tsn = asoc->init_seq_number - 1;
	asoc->last_acked_seq = asoc->init_seq_number - 1;
	asoc->advanced_peer_ack_point = asoc->last_acked_seq;
	asoc->asconf_seq_in = asoc->last_acked_seq;
	asoc->initial_init_rto_max = m->sctp_ep.initial_init_rto_max;
	asoc->initial_rto = m->sctp_ep.initial_rto;

	asoc->max_init_times = m->sctp_ep.max_init_times;
	asoc->max_send_times = m->sctp_ep.max_send_times;
	asoc->def_net_failure = m->sctp_ep.def_net_failure;

	if (m->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
		struct inpcb *in_inp;

		/* Its a V6 socket */
		in_inp = (struct inpcb *)m;
		asoc->ipv6_addr_legal = 1;
		/* Now look at the binding flag to see if V4 will be legal */
		if (
#ifndef __OpenBSD__
		    (in_inp->inp_flags & IN6P_IPV6_V6ONLY) 
#else
		    (0)
#endif
		    == 0) {

			asoc->ipv4_addr_legal = 1;
		} else {
			/* V4 addresses are NOT legal on the association */
			asoc->ipv4_addr_legal = 0;
		}
	} else {
		/* Its a V4 socket, no - V6 */
		asoc->ipv4_addr_legal = 1;
		asoc->ipv6_addr_legal = 0;
	}


	asoc->my_rwnd = m->sctp_socket->so_rcv.sb_hiwat;
	asoc->peers_rwnd = m->sctp_socket->so_rcv.sb_hiwat;

	asoc->smallest_mtu = SCTP_DEFAULT_MAXSEGMENT;

	LIST_INIT(&asoc->sctp_local_addr_list);
	TAILQ_INIT(&asoc->nets);
	asoc->last_asconf_ack_sent = NULL;
	/* Setup to fill the hb random cache at first HB */
	asoc->hb_random_values[0] = 4;

	asoc->sctp_autoclose_ticks = m->sctp_ep.auto_close_time;

	/*
	 * Now the stream parameters, here we allocate space for all
	 * streams that we request by default. 
	 */
	asoc->streamoutcnt = asoc->pre_open_streams =
		m->sctp_ep.pre_open_stream_count;
	asoc->strmout = malloc((asoc->streamoutcnt *
				sizeof(struct sctp_stream_out)),
			       M_PCB,
			       M_NOWAIT);
	for (i=0; i<asoc->streamoutcnt; i++) {
		/*
		 * inbound side must be set to 0xffff,
		 * also NOTE when we get the INIT-ACK back (for INIT sender)
		 * we MUST reduce the count (streamoutcnt) but first check
		 * if we sent to any of the upper streams that were dropped
		 * (if some were). Those that were dropped must be notified
		 * to the upper layer as failed to send.
		 */
		asoc->strmout[i].next_sequence_sent = 0x0; 
		TAILQ_INIT(&asoc->strmout[i].outqueue);
		asoc->strmout[i].stream_no = i;
		asoc->strmout[i].next_spoke.tqe_next = 0;
		asoc->strmout[i].next_spoke.tqe_prev = 0;
	}
	/* Now the init of the other outqueues */
	TAILQ_INIT(&asoc->out_wheel);
	TAILQ_INIT(&asoc->unrel_wheel);
	TAILQ_INIT(&asoc->control_send_queue);
	TAILQ_INIT(&asoc->send_queue);
	TAILQ_INIT(&asoc->sent_queue);
	TAILQ_INIT(&asoc->reasmqueue);
	TAILQ_INIT(&asoc->delivery_queue);
	asoc->max_inbound_streams = m->sctp_ep.max_open_streams_intome;

	TAILQ_INIT(&asoc->asconf_queue);
}

static void
sctp_timeout_handler(void *t)
{
	struct sctp_inpcb *ep;
	struct sctp_tcb *tcb;
	struct sctp_nets *net;
	struct sctp_timer *tmr;
	int s, did_output;

	tmr = (struct sctp_timer *)t;
	ep = (struct sctp_inpcb *)tmr->ep;
	tcb = (struct sctp_tcb *)tmr->tcb;
	net = (struct sctp_nets *)tmr->net;
	did_output = 1;
#ifdef __FreeBSD__
	s = splnet();
#else
	s = splsoftnet();
#endif
	sctp_pegs[SCTP_TIMERS_EXP]++;
	if (ep) {
		if (ep->sctp_socket == 0) {
			printf("GAKKK! timer race socket in ep is NULL typ:%d\n",
			       ((tmr == NULL) ? 2960 : tmr->type));
			splx(s);
			return;
		}
	}
	if (tcb) {
		if (tcb->asoc.state == 0) {
			printf("GAKKKKK! timer race, state in assoc is 0 typ:%d\n",
			       ((tmr == NULL) ? 2960 : tmr->type));
			splx(s);
			return;
		}
	}
  
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
		printf("Timer type %d goes off\n",tmr->type);
	}
#endif /* SCTP_DEBUG */
	if (!callout_active(&tmr->timer)) {
		splx(s);
		return;
	}
	switch(tmr->type) {
		/* call the handler for the appropriate timer type */
	case SCTP_TIMER_TYPE_SEND:
		sctp_pegs[SCTP_TMIT_TIMER]++;
		sctp_t3rxt_timer(ep, tcb, net);
		sctp_chunk_output(ep, tcb, 1);
		break;
	case SCTP_TIMER_TYPE_INIT:
		sctp_t1init_timer(ep,tcb,net);
		/* We do output but not here */
		did_output = 0;
		break;
	case SCTP_TIMER_TYPE_RECV:
		sctp_pegs[SCTP_RECV_TIMER]++;
		sctp_send_sack(tcb);
		sctp_chunk_output(ep, tcb, 0);
		break;
	case SCTP_TIMER_TYPE_SHUTDOWN:
		sctp_shutdown_timer(ep,tcb,net);
		sctp_chunk_output(ep, tcb, 0);
		break;
	case SCTP_TIMER_TYPE_HEARTBEAT:
		sctp_heartbeat_timer(ep,tcb,net);
		sctp_chunk_output(ep, tcb, 0);
		break;
	case SCTP_TIMER_TYPE_COOKIE:
		sctp_cookie_timer(ep,tcb,net);
		sctp_chunk_output(ep, tcb, 1);
		break;
	case SCTP_TIMER_TYPE_NEWCOOKIE:
		{
			struct timeval time;
			int i, secret;
			SCTP_GETTIME_TIMEVAL(&time);
			ep->sctp_ep.time_of_secret_change = time.tv_sec;
			ep->sctp_ep.last_secret_number = ep->sctp_ep.current_secret_number;
			ep->sctp_ep.current_secret_number++;
			if (ep->sctp_ep.current_secret_number >=
			    SCTP_HOW_MANY_SECRETS) {
				ep->sctp_ep.current_secret_number = 0;
			}
			secret = (int)ep->sctp_ep.current_secret_number;
			for(i=0; i<SCTP_NUMBER_OF_SECRETS; i++) {
				ep->sctp_ep.secret_key[secret][i] = sctp_select_initial_TSN(&ep->sctp_ep);
			}
			sctp_timer_start(SCTP_TIMER_TYPE_NEWCOOKIE, ep,
					 tcb, net);
		}
		did_output = 0;
		break;
	case SCTP_TIMER_TYPE_PATHMTURAISE:
		sctp_pathmtu_timer(ep, tcb, net);
		did_output = 0;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNACK:
		sctp_shutdownack_timer(ep, tcb, net);
		sctp_chunk_output(ep, tcb, 0);
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNGUARD:
		sctp_abort_an_association(ep, tcb,
					  SCTP_SHUTDOWN_GUARD_EXPIRES, NULL);
		did_output = 0;
		break;
	case SCTP_TIMER_TYPE_ASCONF:
		sctp_asconf_timer(ep, tcb, net);
		sctp_chunk_output(ep, tcb, 0);
		break;
	case SCTP_TIMER_TYPE_AUTOCLOSE:
		sctp_autoclose_timer(ep, tcb, net);
		did_output = 0;
		break;
	default:
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
			printf("sctp_timeout_handler:unknown timer %d\n",
			       tmr->type);
		}
#endif /* SCTP_DEBUG */
		break;
	};
	splx(s);
	if (did_output) {
		/*
		 * Now we need to clean up the control chunk chain if an
		 * ECNE is on it. It must be marked as UNSENT again so next
		 * call will continue to send it until such time that we get
		 * a CWR, to remove it. It is, however, less likely that we
		 * will find a ecn echo on the chain though.
		 */
		sctp_fix_ecn_echo(&tcb->asoc);
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
		printf("Timer now complete\n");
	}
#endif /* SCTP_DEBUG */
}

int
sctp_timer_start(int t_type,
		 struct sctp_inpcb *ep, 
		 struct sctp_tcb *tcb,
		 struct sctp_nets *net)
{
	int to_ticks;
	struct sctp_timer *tmr;

	if (ep == NULL)
		return(EFAULT);

	to_ticks = 0;

	tmr = NULL;
	switch (t_type) {
	case SCTP_TIMER_TYPE_SEND:
		/* Here we use the RTO timer */
		{
			int rto_val;
			if ((tcb == NULL) || (net == NULL)) {
				return(EFAULT);
			}
			tmr = &net->rxt_timer;
			if (net->RTO == 0) {
				rto_val = tcb->asoc.initial_rto;
			} else {
				rto_val = net->RTO;
			}
			to_ticks = (rto_val * hz)/1000;
		}
		break;
	case SCTP_TIMER_TYPE_INIT:
		/*
		 * Here we use the INIT timer default
		 * usually about 1 minute.
		 */
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		if (net->RTO == 0) {
			to_ticks = (tcb->asoc.initial_rto * hz)/1000;
		}else{
			to_ticks = (net->RTO * hz)/1000;
		}
		break;
	case SCTP_TIMER_TYPE_RECV:
		/*
		 * Here we use the Delayed-Ack timer value from the ep
		 * ususually about 200ms.
		 */
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.dack_timer;
		to_ticks = ep->sctp_ep.sctp_timeoutticks[SCTP_TIMER_RECV];
		break;
	case SCTP_TIMER_TYPE_SHUTDOWN:
		/* Here we use the RTO of the destination. */
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		if (net->RTO == 0) {
			to_ticks = (tcb->asoc.initial_rto * hz)/1000;
		} else {
			to_ticks = (net->RTO * hz)/1000;
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_HEARTBEAT:
		/*
		 * the net is used here so that we can add in the RTO.
		 * Even though we use a different timer. We also add the
		 * HB timer PLUS a random jitter.
		 */
		if (tcb == NULL) {
			return(EFAULT);
		}
		{
			u_int rndval;
			u_int8_t this_random;
			if (tcb->asoc.hb_random_values[0] == 4) {
				rndval = sctp_select_initial_TSN(&ep->sctp_ep);
				memcpy(tcb->asoc.hb_random_values, &rndval,
				       sizeof(tcb->asoc.hb_random_values));
				this_random = tcb->asoc.hb_random_values[0];
				tcb->asoc.hb_random_values[0] = 1;
			} else {
				int indx;
				indx = tcb->asoc.hb_random_values[0];
				this_random = tcb->asoc.hb_random_values[indx];
				tcb->asoc.hb_random_values[0]++;
			}
			/*
			 * We divide by 4 to get a value between 0 - 63 ticks
			 * for the random factor..
			 * i.e. 0 - 630ms of random jitter
			 */
			if (net) {
				int rto_val;
				if (net->RTO == 0) {
					rto_val = tcb->asoc.initial_rto;
				} else {
					rto_val = net->RTO;
				}
				to_ticks = tcb->asoc.heart_beat_delay +
					((rto_val * hz)/1000) +
					(this_random >> 2);
			} else {
				to_ticks = (tcb->asoc.heart_beat_delay +
					    (this_random >> 2) +
					    tcb->asoc.initial_rto);
			}
			tmr = &tcb->asoc.hb_timer;
		}
		break;
	case SCTP_TIMER_TYPE_COOKIE:
		/*
		 * Here we can use the RTO timer from the network since
		 * one RTT was compelete. If a retran happened then we will
		 * be using the RTO initial value.
		 */
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		if (net->RTO == 0) {
			to_ticks = (tcb->asoc.initial_rto * hz)/1000;
		} else {
			to_ticks = (net->RTO * hz)/1000;
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_NEWCOOKIE:
		/*
		 * nothing needed but the endpoint here
		 * ususually about 60 minutes.
		 */
		tmr = &ep->sctp_ep.signature_change;
		to_ticks = ep->sctp_ep.sctp_timeoutticks[SCTP_TIMER_SIGNATURE];
		break;
	case SCTP_TIMER_TYPE_PATHMTURAISE:
		/*
		 * Here we use the value found in the EP for PMTU 
		 * ususually about 10 minutes.
		 */
		if (tcb == NULL) {
			return(EFAULT);
		}
		to_ticks = ep->sctp_ep.sctp_timeoutticks[SCTP_TIMER_PMTU];
		tmr = &tcb->asoc.pmtu;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNACK:
		/* Here we use the RTO of the destination */
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		if (net->RTO == 0) {
			to_ticks = (tcb->asoc.initial_rto * hz)/1000;
		} else {
			to_ticks = (net->RTO * hz)/1000;
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNGUARD:
		/*
		 * Here we use the endpoints shutdown guard timer 
		 * usually about 3 minutes.
		 */
		if (tcb == NULL) {
			return(EFAULT);
		}
		to_ticks = ep->sctp_ep.sctp_timeoutticks[SCTP_TIMER_MAXSHUTDOWN];
		tmr = &tcb->asoc.shut_guard_timer;
		break;
	case SCTP_TIMER_TYPE_ASCONF:
		/*
		 * Here the timer comes from the ep
		 * but its value is from the RTO.
		 */
		if ((tcb == NULL) && (net == NULL)) {
			return(EFAULT);
		}
		if (net->RTO == 0) {
			to_ticks = (tcb->asoc.initial_rto * hz)/1000;
		} else {
			to_ticks = (net->RTO * hz)/1000;
		}
		tmr = &tcb->asoc.asconf_timer;
		break;
	case SCTP_TIMER_TYPE_AUTOCLOSE:
		if (tcb == NULL) {
			return(EFAULT);
		}
		if (tcb->asoc.sctp_autoclose_ticks == 0) {
			/* Really an error since tcb is NOT set to autoclose */
			return(0);
		}
		to_ticks = tcb->asoc.sctp_autoclose_ticks;
		tmr = &tcb->asoc.autoclose_timer;
		break;
	default:
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
			printf("sctp_timer_start:Unknown timer type %d\n",
			       t_type);
		}
#endif /* SCTP_DEBUG */
		return(EFAULT);
		break;
	};
	if (callout_pending(&tmr->timer)) {
		/*
		 * we do NOT allow you to have it already running.
		 * if it is we leave the current one up unchanged
		 */
		return(EALREADY);
	}
	if ((to_ticks <= 0) || (tmr == NULL)) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
			printf("sctp_timer_start:%d:software error to_ticks:%d tmr:%x not set ??\n",
			       t_type,to_ticks, (u_int)tmr);
		}
#endif /* SCTP_DEBUG */
		return(EFAULT);
	}
	/* At this point we can proceed */
	tmr->type = t_type;
	tmr->ep = (void *)ep;
	tmr->tcb = (void *)tcb;
	tmr->net = (void *)net;
	callout_reset(&tmr->timer, to_ticks, sctp_timeout_handler, tmr);
	return(0);
}

int
sctp_timer_stop(int t_type,
		struct sctp_inpcb *ep, 
		struct sctp_tcb *tcb,
		struct sctp_nets *net)
{
	struct sctp_timer *tmr;

	if (ep == NULL)
		return(EFAULT);

	tmr = NULL;
	switch (t_type) {
	case SCTP_TIMER_TYPE_SEND:
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_INIT:
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_RECV:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.dack_timer;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWN:
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_HEARTBEAT:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.hb_timer;
		break;
	case SCTP_TIMER_TYPE_COOKIE:
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_NEWCOOKIE:
		/* nothing needed but the endpoint here */
		tmr = &ep->sctp_ep.signature_change;
		break;
	case SCTP_TIMER_TYPE_PATHMTURAISE:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.pmtu;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNACK:
		if ((tcb == NULL) || (net == NULL)) {
			return(EFAULT);
		}
		tmr = &net->rxt_timer;
		break;
	case SCTP_TIMER_TYPE_SHUTDOWNGUARD:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.shut_guard_timer;
		break;
	case SCTP_TIMER_TYPE_ASCONF:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.asconf_timer;
		break;
	case SCTP_TIMER_TYPE_AUTOCLOSE:
		if (tcb == NULL) {
			return(EFAULT);
		}
		tmr = &tcb->asoc.autoclose_timer;
		break;
	default:
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_TIMER1) {
			printf("sctp_timer_stop:Unknown timer type %d\n",
			       t_type);
		}
#endif /* SCTP_DEBUG */
		break;
	};
	if (tmr == NULL)
		return(EFAULT);

	callout_stop(&tmr->timer);
	return(0);
}

#ifdef SCTP_USE_ADLER32
static
unsigned int update_adler32(u_int32_t adler,
			    u_int8_t *buf,
			    int len)
{
	u_int32_t s1 = adler & 0xffff;
	u_int32_t s2 = (adler >> 16) & 0xffff;
	int n;

	for (n = 0; n < len; n++,buf++) {
		/* s1 = (s1 + buf[n]) % BASE */
		/* first we add */
		s1 = (s1 + *buf);
		/*
		 * now if we need to, we do a mod by subtracting. It seems
		 * a bit faster since I really will only ever do one subtract
		 * at the MOST, since buf[n] is a max of 255.
		 */
		if (s1 >= SCTP_ADLER32_BASE) {
			s1 -= SCTP_ADLER32_BASE;
		}
		/* s2 = (s2 + s1) % BASE */
		/* first we add */
		s2 = (s2 + s1);
		/*
		 * again, it is more efficent (it seems) to subtract since
		 * the most s2 will ever be is (BASE-1 + BASE-1) in the worse
		 * case. This would then be (2 * BASE) - 2, which will still
		 * only do one subtract. On Intel this is much better to do
		 * this way and avoid the divide. Have not -pg'd on sparc.
		 */
		if (s2 >= SCTP_ADLER32_BASE) {
			/*      s2 %= BASE; */
			s2 -= SCTP_ADLER32_BASE;
		}
	}
	/* Return the adler32 of the bytes buf[0..len-1] */
	return ((unsigned int)htonl((s2 << 16) + s1));
}
#endif /* SCTP_USE_ADLER32 */

u_int32_t
sctp_calculate_sum(m, pktlen, offset)
     struct mbuf *m;
     int32_t *pktlen;
     u_int32_t offset;

{
	/* 
	 * given a mbuf chain with a packetheader offset by 'offset'
	 * pointing at a sctphdr (with csum set to 0) go through
	 * the chain of m_next's and calculate the SCTP checksum.
	 * This is currently Adler32 but will change to CRC32x
	 * soon. Also has a side bonus calculate the total length
	 * of the mbuf chain.
	 * Note: if offset is greater than the total mbuf length,
	 * checksum=1, pktlen=0 is returned (ie. no real error code)
	 */
	register int32_t tlen=0;
#ifdef SCTP_USE_ADLER32
	register unsigned int base = 1L;
#else
	register u_int32_t base = 0xffffffff;
#endif /* SCTP_USE_ADLER32 */
	register struct mbuf *at;
	at = m;
	/* find the correct mbuf and offset into mbuf */
	while ((at != NULL) && (offset > at->m_len)) {
		offset -= at->m_len;	/* update remaining offset left */
		at = at->m_next;
	}

	while (at != NULL) {
#ifdef SCTP_USE_ADLER32
		base = update_adler32(base, at->m_data+offset,
				      at->m_len-offset);
#else
		base = update_crc32(base,at->m_data+offset, at->m_len-offset);
#endif /* SCTP_USE_ADLER32 */
		tlen += at->m_len - offset;
		/* we only offset once into the first mbuf */
		if (offset) {
			offset = 0;
		}
		at = at->m_next;
	}
	if (pktlen != NULL) {
		*pktlen = tlen;
	}
#ifndef SCTP_USE_ADLER32
	base = sctp_csum_finalize(base);
#endif
	return(base);
}

void
sctp_mtu_size_reset(struct sctp_association *asoc, u_long mtu)
{
	/*
	 * Reset the P-MTU size on this association, this involves changing
	 * the asoc MTU, going through ANY chunk+overhead larger than mtu
	 * to allow the DF flag to be cleared.
	 */
	struct sctp_tmit_chunk *chk;
	struct sctp_stream_out *strm;
	int eff_mtu;
	asoc->smallest_mtu = mtu;
	eff_mtu = mtu - SCTP_MAX_OVERHEAD;

	/* Now mark any chunks that need to let IP fragment */
	TAILQ_FOREACH(strm, &asoc->out_wheel, next_spoke) {
		TAILQ_FOREACH(chk, &strm->outqueue, sctp_next) {
			if (chk->send_size > eff_mtu) {
				chk->flags &= SCTP_DONT_FRAGMENT;
				chk->flags |= CHUNK_FLAGS_FRAGMENT_OK;
			}
		}
	}
	TAILQ_FOREACH(chk, &asoc->send_queue, sctp_next) {
		if (chk->send_size > eff_mtu) {
			chk->flags &= SCTP_DONT_FRAGMENT;
			chk->flags |= CHUNK_FLAGS_FRAGMENT_OK;
		}
	}
	TAILQ_FOREACH(chk, &asoc->sent_queue, sctp_next) {
		if (chk->send_size > eff_mtu) {
			chk->flags &= SCTP_DONT_FRAGMENT;
			chk->flags |= CHUNK_FLAGS_FRAGMENT_OK;
		}
	}
}


/*
 * given an association and starting time of the current RTT period
 * return RTO in number of usecs
 * net should point to the current network
 */
u_int32_t
sctp_calculate_rto(struct sctp_tcb *stcb,
		   struct sctp_association *assoc,
		   struct sctp_nets *net,
		   struct timeval *old) {
	/* 
	 * given an association and the starting time of the current RTT 
	 * period (in value1/value2) return RTO in number of usecs.
	 */
	int calc_time = 0;
	int o_calctime;
	int new_rto = 0;
	struct timeval now;

	/************************/
	/* 1. calculate new RTT */
	/************************/
	/* get the current time */
	SCTP_GETTIME_TIMEVAL(&now);
	/* compute the RTT value */
	if ((u_long)now.tv_sec > (u_long)old->tv_sec) {
		calc_time = ((u_long)now.tv_sec - (u_long)old->tv_sec) * 1000;
		if ((u_long)now.tv_usec > (u_long)old->tv_usec) {
			calc_time += (((u_long)now.tv_usec -
				       (u_long)old->tv_usec)/1000);
		} else if ((u_long)now.tv_usec < (u_long)old->tv_usec) {
			/* Borrow 1,000ms from current calculation */
			calc_time -= 1000;
			/* Add in the slop over */
			calc_time += ((int)now.tv_usec/1000);
			/* Add in the pre-second ms's */
			calc_time += (((int)1000000 - (int)old->tv_usec)/1000);
		}
	} else if ((u_long)now.tv_sec == (u_long)old->tv_sec) {
		if ((u_long)now.tv_usec > (u_long)old->tv_usec) {
			calc_time = ((u_long)now.tv_usec -
				     (u_long)old->tv_usec)/1000;
		} else if ((u_long)now.tv_usec < (u_long)old->tv_usec) {
			/* impossible .. garbage in nothing out */
			return(0);
		} else {
			/* impossible .. garbage in nothing out */
			return(0);
		}
	} else {
		/* Clock wrapped? */
		return(0);
	}

	/***************************/
	/* 2. update RTTVAR & SRTT */
	/***************************/
	/*	if (net->lastsv || net->lastsa) {*/
	/* per Section 5.3.1 C3 in SCTP */
	/*		net->lastsv = (int) 	*//* RTTVAR */
	/*			(((double)(1.0 - 0.25) * (double)net->lastsv) +
			(double)(0.25 * (double)abs(net->lastsa - calc_time)));
			net->lastsa = (int) */	/* SRTT */
	/*(((double)(1.0 - 0.125) * (double)net->lastsa) +
	   (double)(0.125 * (double)calc_time));
	   } else {
	*//* the first RTT calculation, per C2 Section 5.3.1 */
	/*		net->lastsa = calc_time;	*//* SRTT */
	/*		net->lastsv = calc_time / 2;	*//* RTTVAR */
	/*	}*/
	/* if RTTVAR goes to 0 you set to clock grainularity */
	/*	if (net->lastsv == 0) {
		net->lastsv = SCTP_CLOCK_GRANULARITY;
		}
		new_rto = net->lastsa + 4 * net->lastsv;
	*/
	o_calctime = calc_time;
	/* this is Van Jacobson's integer version */
	if (net->RTO) {
		calc_time -= (net->lastsa >> 3);
		net->lastsa += calc_time;
		if (calc_time < 0) {
			calc_time = -calc_time;
		}
		calc_time -= (net->lastsv >> 2);
		net->lastsv += calc_time;
		if (net->lastsv == 0) {
			net->lastsv = SCTP_CLOCK_GRANULARITY;
		}
	} else {
		/* First RTO measurment */
		net->lastsa = calc_time;
		net->lastsv = calc_time >> 1;
	}
	new_rto = ((net->lastsa >> 2) + net->lastsv) >> 1;
	/* bound it, per C6/C7 in Section 5.3.1 */
	if (new_rto < stcb->sctp_ep->sctp_ep.sctp_minrto) {
		new_rto = stcb->sctp_ep->sctp_ep.sctp_minrto;
	}
	if (new_rto > stcb->sctp_ep->sctp_ep.sctp_maxrto) {
		new_rto = stcb->sctp_ep->sctp_ep.sctp_maxrto;
	}
	/* we are now returning the RTT Smoothed */
	return((u_int32_t)new_rto);
}


/*
 * return a pointer to a contiguous piece of data from the given
 * mbuf chain starting at 'off' for 'len' bytes.  If the desired
 * piece spans more than one mbuf, a copy is made at 'ptr'.
 * caller must ensure that the buffer size is >= 'len'
 * returns NULL if there there isn't 'len' bytes in the chain.
 */
caddr_t
sctp_m_getptr(struct mbuf *m, int off, int len, u_int8_t *in_ptr)
{
	u_int count;
	u_int8_t *ptr;
	ptr = in_ptr;
	if ((off < 0) || (len <= 0))
		return(NULL);

	/* find the desired start location */
	while ((m != NULL) && (off > 0)) {
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	if (m == NULL)
		return(NULL);

	/* is the current mbuf large enough (eg. contiguous)? */
	if ((m->m_len - off) >= len) {
		return(mtod(m, caddr_t) + off);
	} else {
		/* else, it spans more than one mbuf, so save a temp copy... */
		while ((m != NULL) && (len > 0)) {
			count = min(m->m_len - off, len);
			bcopy(mtod(m, caddr_t) + off, ptr, count);
			len -= count;
			ptr += count;
			off = 0;
			m = m->m_next;
		}
		if ((m == NULL) && (len > 0))
			return(NULL);
		else
			return((caddr_t)in_ptr);
	}
}


struct sctp_paramhdr *
sctp_get_next_param(struct mbuf *m, 
		    int offset, 
		    struct sctp_paramhdr *pull, 
		    int pull_limit)
{
	/* This just provides a typed signature to Peter's Pull routine */
	return((struct sctp_paramhdr *)sctp_m_getptr(m, offset, pull_limit,
						     (u_int8_t *)pull));
}


int
sctp_add_pad_tombuf(struct mbuf *m, int padlen)
{
	/*
	 * add padlen bytes of 0 filled padding to the end of the mbuf.
	 * If padlen is > 3 this routine will fail.
	 */
	u_int8_t *dp;
	int i;
	if (padlen > 3) {
		return(E2BIG);
	}
	if (M_TRAILINGSPACE(m)) {
		/*
		 * The easy way.
		 * We hope the majority of the time we hit here :)
		 */
		dp = (u_int8_t *)(mtod(m, caddr_t) + m->m_len);
		m->m_len += padlen;
	} else {
		/* Hard way we must grow the mbuf */
		struct mbuf *tmp;
		MGET(tmp, M_DONTWAIT, MT_DATA);
		if (tmp == NULL) {
			/* Out of space GAK! we are in big trouble. */
			return(ENOSPC);
		}
		/* setup and insert in middle */
		tmp->m_next = m->m_next;
		tmp->m_len = padlen;
		m->m_next = tmp;
		dp = mtod(tmp, u_int8_t *);
	}
	/* zero out the pad */
	for (i=0; i<padlen; i++) {
		*dp = 0;
		dp++;
	}
	return(0);
}

int
sctp_pad_lastmbuf(struct mbuf *m, int padval)
{
	/* find the last mbuf in chain and pad it */
	struct mbuf *m_at;
	m_at = m;
	while (m_at) {
		if (m_at->m_next == NULL) {
			return(sctp_add_pad_tombuf(m_at, padval));
		}
		m_at = m_at->m_next;
	}
	return(EFAULT);
}

#ifndef __FreeBSD__
/*
 * Don't know why but without this I get an unknown reference when
 * compiling NetBSD... hmm
 */
extern void in6_sin_2_v4mapsin6 (struct sockaddr_in *sin,
				 struct sockaddr_in6 *sin6);
#endif

static void
sctp_notify_assoc_change(u_int32_t event, struct sctp_tcb *stcb,
			 u_int32_t error) {
	struct mbuf *m_notify;
	struct sctp_assoc_change *sac;
	struct sockaddr *to;
	struct sockaddr_in6 sin6, lsa6;
	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_RECVASSOCEVNT)) {
		/* event not enabled */
#ifdef SCTP_TCP_MODEL_SUPPORT
		/*******Don't think we need anything here***********/
		/*	  if (event == SHUTDOWN_COMPLETE)             */ 
		/*	    stcb->sctp_socket->so_error = ECONNRESET;*/
		/***************************************************/

		/*
		 * For TCP model AND UDP connected sockets we will send
		 * an error up when an ABORT comes in. */
		if (((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_TCPTYPE) ||
		     (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_IN_TCPPOOL))
		    && (event == SCTP_COMM_LOST)) {
			stcb->sctp_socket->so_error = ECONNRESET;
		}
#endif /* SCTP_TCP_MODEL_SUPPORT */
		return;
		
	}

	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		/* no space left */
		return;
	sac = mtod(m_notify, struct sctp_assoc_change *);
	sac->sac_type = SCTP_ASSOC_CHANGE;
	sac->sac_flags = 0;
	sac->sac_length = sizeof(struct sctp_assoc_change);
	sac->sac_state = event;
	sac->sac_error = error;
	/* XXX verify these stream counts */
	sac->sac_outbound_streams = stcb->asoc.streamoutcnt;
	sac->sac_inbound_streams = stcb->asoc.streamincnt;
	sac->sac_assoc_id = (sctp_assoc_t)stcb;

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(struct sctp_assoc_change);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(struct sctp_assoc_change);
	m_notify->m_next = NULL;

	/* append to socket */
	to = (struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);
	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}
	if (sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, 
				 to, m_notify, NULL) == 0)
		/* not enough room */
		return;
	sctp_sorwakeup(stcb->sctp_ep,stcb->sctp_socket);
}

static void
sctp_notify_peer_addr_change(struct sctp_tcb *stcb, uint32_t state,
			     struct sockaddr *sa, uint32_t error) {
	struct mbuf *m_notify;
	struct sctp_paddr_change *spc;
	struct sockaddr *to;
	struct sockaddr_in6 sin6, lsa6;

	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_RECVPADDREVNT))
		/* event not enabled */
		return;

	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		return;
	
	spc = mtod(m_notify, struct sctp_paddr_change *);
	spc->spc_type = SCTP_PEER_ADDR_CHANGE;
	spc->spc_flags = 0;
	spc->spc_length = sizeof(struct sctp_paddr_change);
	if (sa->sa_family == AF_INET) {
		memcpy(&spc->spc_aaddr, sa, sizeof(struct sockaddr_in));
	} else {
		memcpy(&spc->spc_aaddr, sa, sizeof(struct sockaddr_in6));
	}
	spc->spc_state = state;
	spc->spc_error = error;
	spc->spc_assoc_id = (sctp_assoc_t)stcb;

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(struct sctp_paddr_change);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(struct sctp_paddr_change);
	m_notify->m_next = NULL;

	to = (struct sockaddr *)(struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);

	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}
	/* append to socket */
	if (!sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, to,
				  m_notify, NULL))
		/* not enough room */
		return;
	sctp_sorwakeup(stcb->sctp_ep, stcb->sctp_socket);
}


static void
sctp_notify_send_failed(struct sctp_tcb *stcb, u_int32_t error,
			struct sctp_tmit_chunk *chk) {
	struct mbuf *m_notify;
	struct sctp_send_failed *ssf;
	struct sockaddr_in6 sin6, lsa6;
	struct sockaddr *to;
	int length;

	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_RECVSENDFAILEVNT))
		/* event not enabled */
		return;

	length = sizeof(struct sctp_send_failed) + chk->send_size;
	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		/* no space left */
		return;
	ssf = mtod(m_notify, struct sctp_send_failed *);
	ssf->ssf_type = SCTP_SEND_FAILED;
	if (error == SCTP_NOTIFY_DATAGRAM_UNSENT)
		ssf->ssf_flags = SCTP_DATA_UNSENT;
	else
		ssf->ssf_flags = SCTP_DATA_SENT;
	ssf->ssf_length = length;
	ssf->ssf_error = error;
	/* not exactly what the user sent in, but should be close :) */
	ssf->ssf_info.sinfo_stream = chk->rec.data.stream_number;
	ssf->ssf_info.sinfo_ssn = chk->rec.data.stream_seq;
	ssf->ssf_info.sinfo_flags = chk->rec.data.rcv_flags;
	ssf->ssf_info.sinfo_ppid = chk->rec.data.payloadtype;
	ssf->ssf_info.sinfo_context = chk->rec.data.context;
	ssf->ssf_info.sinfo_assoc_id = (sctp_assoc_t)stcb;
	ssf->ssf_assoc_id = (sctp_assoc_t)stcb;
	m_notify->m_next = chk->data;
	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = length;
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = length;

	/* Steal off the mbuf */
	chk->data = NULL;
	to = (struct sockaddr *)(struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);

	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}

	/* append to socket */
	if (!sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, to,
				  m_notify, NULL)) {
		/* not enough room */
		m_freem(m_notify);
		return;
	}
	sctp_sorwakeup(stcb->sctp_ep, stcb->sctp_socket);
}

static void
sctp_notify_adaption_layer(struct sctp_tcb *stcb,
			   u_int32_t error)
{
	struct mbuf *m_notify;
	struct sctp_adaption_event *sai;
	struct sockaddr_in6 sin6, lsa6;
	struct sockaddr *to;

	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_ADAPTIONEVNT))
		/* event not enabled */
		return;
     
	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		/* no space left */
		return;
	sai = mtod(m_notify, struct sctp_adaption_event *);
	sai->sai_type = SCTP_ADAPTION_INDICATION;
	sai->sai_flags = 0;
	sai->sai_length = sizeof(struct sctp_adaption_event);
	sai->sai_adaption_bits = error;
	sai->sai_assoc_id = (sctp_assoc_t)stcb;

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(struct sctp_adaption_event);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(struct sctp_adaption_event);
	m_notify->m_next = NULL;

	to = (struct sockaddr *)(struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);
	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}
	/* append to socket */
	if (!sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, to,
				  m_notify, NULL))
		/* not enough room */
		return;
	sctp_sorwakeup(stcb->sctp_ep, stcb->sctp_socket);
}

static void
sctp_notify_partial_delivery_indication(struct sctp_tcb *stcb,
					u_int32_t error)
{
	struct mbuf *m_notify;
	struct sctp_rcv_pdapi_event *pdapi;
	struct sockaddr_in6 sin6, lsa6;
	struct sockaddr *to;

	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_PDAPIEVNT))
		/* event not enabled */
		return;
     
	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		/* no space left */
		return;
	pdapi = mtod(m_notify, struct sctp_rcv_pdapi_event *);
	pdapi->pdapi_type = SCTP_PARTIAL_DELIVERY_EVENT;
	pdapi->pdapi_flags = 0;
	pdapi->pdapi_length = sizeof(struct sctp_rcv_pdapi_event);
	pdapi->pdapi_indication = error;
	pdapi->pdapi_assoc_id = (sctp_assoc_t)stcb;

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(struct sctp_rcv_pdapi_event);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(struct sctp_rcv_pdapi_event);
	m_notify->m_next = NULL;

	to = (struct sockaddr *)(struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);
	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}
	/* append to socket */
	if (!sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, to,
				  m_notify, NULL))
		/* not enough room */
		return;
	sctp_sorwakeup(stcb->sctp_ep, stcb->sctp_socket);
}

static void
sctp_notify_shutdown_event(struct sctp_tcb *stcb) {
	struct mbuf *m_notify;
	struct sctp_shutdown_event *sse;
	struct sockaddr_in6 sin6, lsa6;
	struct sockaddr *to;

	if (!(stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_RECVSHUTDOWNEVNT))
		/* event not enabled */
		return;
     
	MGETHDR(m_notify, M_DONTWAIT, MT_DATA);
	if (m_notify == NULL)
		/* no space left */
		return;
	sse = mtod(m_notify, struct sctp_shutdown_event *);
	sse->sse_type = SCTP_SHUTDOWN_EVENT;
	sse->sse_flags = 0;
	sse->sse_length = sizeof(struct sctp_shutdown_event);
	sse->sse_assoc_id = (sctp_assoc_t)stcb;

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(struct sctp_shutdown_event);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(struct sctp_shutdown_event);
	m_notify->m_next = NULL;

	to = (struct sockaddr *)(struct sockaddr *)&stcb->asoc.primary_destination->ra._l_addr;
	if ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_NEEDS_MAPPED_V4) &&
	    (to->sa_family == AF_INET)) {
		in6_sin_2_v4mapsin6((struct sockaddr_in *)to, &sin6);
		to = (struct sockaddr *)&sin6;
	}
	/* check and strip embedded scope junk */
	to = (struct sockaddr *)sctp_recover_scope((struct sockaddr_in6 *)to,
						   &lsa6);
	if (sctp_sbspace(&stcb->sctp_socket->so_rcv) < m_notify->m_len) {
		m_freem(m_notify);
		return;
	}
	/* append to socket */
	if (!sbappendaddr_nocheck(&stcb->sctp_socket->so_rcv, to,
				  m_notify, NULL))
		/* not enough room */
		return;
	sctp_sorwakeup(stcb->sctp_ep, stcb->sctp_socket);
}

void
sctp_ulp_notify(u_int32_t notification, struct sctp_tcb *stcb,
		u_int32_t error, void *data) {
	switch (notification) {
	case SCTP_NOTIFY_ASSOC_UP:
		sctp_notify_assoc_change(SCTP_COMM_UP, stcb, error);
		break;
	case SCTP_NOTIFY_ASSOC_DOWN:
		sctp_notify_assoc_change(SCTP_SHUTDOWN_COMP, stcb, error);
		break;
	case SCTP_NOTIFY_INTERFACE_DOWN:
		{
			struct sctp_nets *net;
			net = (struct sctp_nets *)data;
			sctp_notify_peer_addr_change(stcb, SCTP_ADDR_UNREACHABL,
						     (struct sockaddr *)&net->ra._l_addr,
						     error);

		}
		break;
	case SCTP_NOTIFY_INTERFACE_UP:
		{
			struct sctp_nets *net;
			net = (struct sctp_nets *)data;
			sctp_notify_peer_addr_change(stcb, SCTP_ADDR_AVAILABLE,
						     (struct sockaddr *)&net->ra._l_addr,
						     error);
		}
		break;
	case SCTP_NOTIFY_DG_FAIL:
		sctp_notify_send_failed(stcb, error,
					(struct sctp_tmit_chunk *)data);
		break;
	case SCTP_NOTIFY_ADAPTION_INDICATION:
		/* Here the error is the adaption indication */
		sctp_notify_adaption_layer(stcb, error);
		break;
	case SCTP_NOTIFY_PARTIAL_DELVIERY_INDICATION:
		sctp_notify_partial_delivery_indication(stcb, error);
		break;
	case SCTP_NOTIFY_STRDATA_ERR:
		break;
	case SCTP_NOTIFY_ASSOC_ABORTED:
		sctp_notify_assoc_change(SCTP_COMM_LOST, stcb, error);
		break;
	case SCTP_NOTIFY_PEER_OPENED_STREAM:
		break;
	case SCTP_NOTIFY_STREAM_OPENED_OK:
		break;
	case SCTP_NOTIFY_ASSOC_RESTART:
		sctp_notify_assoc_change(SCTP_RESTART, stcb, error);
		break;
	case SCTP_NOTIFY_HB_RESP:
		break;
	case SCTP_NOTIFY_ASCONF_ADD_IP:
		sctp_notify_peer_addr_change(stcb,SCTP_ADDR_ADDED, data, error);
		break;
	case SCTP_NOTIFY_ASCONF_DELETE_IP:
		sctp_notify_peer_addr_change(stcb, SCTP_ADDR_REMOVED, data,
					     error);
		break;
	case SCTP_NOTIFY_ASCONF_SET_PRIMARY:
		sctp_notify_peer_addr_change(stcb, SCTP_ADDR_MADE_PRIM, data,
					     error);
		break;
	case SCTP_NOTIFY_ASCONF_SUCCESS:
		break;
	case SCTP_NOTIFY_ASCONF_FAILED:
		break;
	case SCTP_NOTIFY_PEER_SHUTDOWN:
		sctp_notify_shutdown_event(stcb);
		break;
	default:
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_UTIL1) {
			printf("NOTIFY: unknown notification %xh (%u)\n",
			       notification, notification);
		}
#endif /* SCTP_DEBUG */
	} /* end switch */
}

extern struct sctp_epinfo sctppcbinfo;

void
sctp_report_all_outbound(struct sctp_tcb *stcb)
{
	struct sctp_association *asoc;
	struct sctp_stream_out *outs;
	struct sctp_tmit_chunk *chk;

	asoc = &stcb->asoc;
	/* now through all the gunk freeing chunks */
	TAILQ_FOREACH(outs, &asoc->out_wheel, next_spoke) {
		/* now clean up any chunks here */
		chk = TAILQ_FIRST(&outs->outqueue);
		while (chk) {
			TAILQ_REMOVE(&outs->outqueue, chk, sctp_next);
			sctp_ulp_notify(SCTP_NOTIFY_DG_FAIL, stcb,
					SCTP_NOTIFY_DATAGRAM_UNSENT, chk);
			if (chk->data) {
				m_freem(chk->data);
				chk->data = NULL;
			}
			if (chk->whoTo)
				sctp_free_remote_addr(chk->whoTo);
			chk->whoTo = NULL;
			chk->asoc = NULL;
			/* Free the chunk */
#if defined(__FreeBSD__)
			zfreei(sctppcbinfo.ipi_zone_chunk, chk);
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
			pool_put(&sctppcbinfo.ipi_zone_chunk, chk);
#endif
			sctppcbinfo.ipi_count_chunk--;
			if ((int)sctppcbinfo.ipi_count_chunk < 0) {
				panic("Chunk count is negative");
			}
			sctppcbinfo.ipi_gencnt_chunk++;
			chk = TAILQ_FIRST(&outs->outqueue);
		}
	}
	/* pending send queue SHOULD be empty */
	if (!TAILQ_EMPTY(&asoc->send_queue)) {
		chk = TAILQ_FIRST(&asoc->send_queue);
		while (chk) {
			TAILQ_REMOVE(&asoc->send_queue, chk, sctp_next);
			sctp_ulp_notify(SCTP_NOTIFY_DG_FAIL, stcb, SCTP_NOTIFY_DATAGRAM_UNSENT, chk);
			if (chk->data) {
				m_freem(chk->data);
				chk->data = NULL;
			}
			if (chk->whoTo)
				sctp_free_remote_addr(chk->whoTo);
			chk->whoTo = NULL;
#if defined(__FreeBSD__)
			zfreei(sctppcbinfo.ipi_zone_chunk, chk);
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
			pool_put(&sctppcbinfo.ipi_zone_chunk, chk);
#endif
			sctppcbinfo.ipi_count_chunk--;
			if ((int)sctppcbinfo.ipi_count_chunk < 0) {
				panic("Chunk count is negative");
			}
			sctppcbinfo.ipi_gencnt_chunk++;
			chk = TAILQ_FIRST(&asoc->send_queue);
		}
	}
	/* sent queue SHOULD be empty */
	if (!TAILQ_EMPTY(&asoc->sent_queue)) {
		chk = TAILQ_FIRST(&asoc->sent_queue);
		while (chk) {
			TAILQ_REMOVE(&asoc->sent_queue, chk, sctp_next);
			sctp_ulp_notify(SCTP_NOTIFY_DG_FAIL, stcb,
					SCTP_NOTIFY_DATAGRAM_SENT, chk);
			if (chk->data) {
				m_freem(chk->data);
				chk->data = NULL;
			}
			if (chk->whoTo)
				sctp_free_remote_addr(chk->whoTo);
			chk->whoTo = NULL;
#if defined(__FreeBSD__)
			zfreei(sctppcbinfo.ipi_zone_chunk, chk);
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
			pool_put(&sctppcbinfo.ipi_zone_chunk, chk);
#endif
			sctppcbinfo.ipi_count_chunk--;
			if ((int)sctppcbinfo.ipi_count_chunk < 0) {
				panic("Chunk count is negative");
			}
			sctppcbinfo.ipi_gencnt_chunk++;
			chk = TAILQ_FIRST(&asoc->sent_queue);
		}
	}
}

void
sctp_abort_notification(struct sctp_tcb *stcb, int error)
{
	/* Tell them we lost the asoc */
	sctp_ulp_notify(SCTP_NOTIFY_ASSOC_ABORTED, stcb, error, NULL);
	sctp_report_all_outbound(stcb);
}

void
sctp_abort_association(struct sctp_inpcb *inp,
		       struct sctp_tcb *stcb,
		       struct mbuf *m,
		       int iphlen,
		       struct mbuf *operr)
{
	struct ip *iph;
	struct ip6_hdr *ip6;
	struct sctphdr *sh;
	u_int32_t vtag;
  
	iph = mtod(m, struct ip *);
	sh = (struct sctphdr *)((caddr_t)iph + iphlen);
	vtag = 0;
	if (stcb != NULL) {
		/* We have a TCB to abort, send notification too */
		vtag = stcb->asoc.peer_vtag;
		sctp_abort_notification(stcb, 0);
	}
	if (iph->ip_v == IPVERSION) {
		sctp_send_abort(m, iph, sh, iphlen, vtag, operr);
	} else {
		ip6 = mtod(m, struct ip6_hdr *);
		sctp6_send_abort(m, ip6, sh, iphlen, vtag, operr);
	}
	if (stcb != NULL) {
		/* Ok, now lets free it */
		sctp_free_assoc(inp, stcb);
	}
}

void
sctp_abort_an_association(struct sctp_inpcb *inp,
			  struct sctp_tcb *stcb,
			  int error,
			  struct mbuf *operr)
{
	u_int32_t vtag;

	if (stcb == NULL)
		/* Got to have a TCB */
		return;
	vtag = stcb->asoc.peer_vtag;
	/* notify the ulp */
	sctp_abort_notification(stcb, error);
	/* notify the peer */
	sctp_send_abort_tcb(stcb, operr);
	/* now free the asoc */
	sctp_free_assoc(inp, stcb);
}

void
sctp_handle_ootb(struct sctp_inpcb *ep,
		 struct mbuf *m, int iphlen,
		 int offset, int length,
		 struct mbuf *operr)
{
	struct sctp_chunkhdr *ch;
	struct sctphdr *sctphdr;
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	struct sockaddr *to;
  
	int ch_len;
	struct ip *iph;
	struct ip6_hdr *ip6h;

	u_int8_t chunk_buf[128];
	/* Generate a TO address for future reference */
	sctphdr = (struct sctphdr *)(mtod(m, caddr_t) + iphlen);
	iph = mtod(m,struct ip *);
	ip6h = mtod(m,struct ip6_hdr *);
	if (iph->ip_v == IPVERSION) {
		/* form a sockaddr_in to send to. */
		to = (struct sockaddr *)&sin;
		sin.sin_len = sizeof(sin);
		sin.sin_family = AF_INET;
		sin.sin_port = sctphdr->src_port;
		sin.sin_addr = iph->ip_src;
	} else {
		/* form a sockaddr_in6 to send to. */
		to = (struct sockaddr *)&sin6;
		sin6.sin6_len = sizeof(sin6);
		sin6.sin6_family = AF_INET6;
		sin6.sin6_port = sctphdr->src_port;
		sin6.sin6_addr = ip6h->ip6_src;
	}
	ch = (struct sctp_chunkhdr *)sctp_m_getptr(m, offset,
						   sizeof(struct sctp_chunkhdr),
						   chunk_buf);
	while (ch != NULL) {
		ch_len = ntohs(ch->chunk_length);
		if ((ch_len < sizeof(*ch)) || (ch_len > length)) {
			/* break to abort land */
			break;
		}
		switch (ch->chunk_type) {
		case SCTP_ABORT_ASSOCIATION:
			/* we don't respond with an ABORT to an ABORT */
			return;
		case SCTP_SHUTDOWN_COMPLETE:
			/*
			 * we ignore it since we are not waiting for it
			 * and peer is gone
			 */
			return;
		case SCTP_SHUTDOWN_ACK:
			sctp_send_shutdown_complete2(ep, to, sctphdr->v_tag);
			return;
		default: 
			break;
		}
		length -= SCTP_SIZE32(ch_len);
		if (length < sizeof(*ch)) {
			/* no more data left in the mbuf chain */
			break;
		}
		offset += SCTP_SIZE32(ch_len);
		ch = (struct sctp_chunkhdr *)sctp_m_getptr(m,
							   offset,
							   sizeof(struct sctp_chunkhdr),
							   chunk_buf);
	}
	if (to->sa_family == AF_INET) {
		sctp_send_abort(m, iph, sctphdr, iphlen, 0, operr);
	} else {
		sctp6_send_abort(m, ip6h, sctphdr, iphlen, 0, operr);
	}
}


int
sctp_is_there_an_abort_here(struct mbuf *m, int off)
{
	/*
	 * check the inbound datagram to make sure there is not an abort
	 * inside it, if there is return 1, else return 0.
	 */
	struct sctp_chunkhdr desc;
	int at, x;

	at = off + sizeof(struct sctphdr);
	if ((m->m_flags & M_PKTHDR) == 0) {
		printf("Gak, no header length huh?\n");
	}
	while ((at+sizeof(struct sctp_chunkhdr)) <= m->m_pkthdr.len) {
		m_copydata(m, at, sizeof(struct sctp_chunkhdr),
			   (caddr_t)&desc);
		x = desc.chunk_length;
		NTOHS(x);
		/* Is it to small? */
		if (x < sizeof(struct sctp_chunkhdr)) {
			/* packet is probably corrupt */
			printf("Gak, possible corrupt packet with chk len:%d\n", x);
			break;
		}

		/* is it to large? */
		if ((x+at) > m->m_pkthdr.len) {
			/* packet is probably corrupt */
			printf("Gak, possible corrupt packet with chk len:%d max:%d (at:%d)\n",
			       x, m->m_pkthdr.len, at);
			break;
		}
		/* we seem to be ok, is it an abort? */
		if (desc.chunk_type == SCTP_ABORT_ASSOCIATION) {
			/* yep, tell them */
			return(1);
		}
		/* Nope, move to the next chunk */
		at += x;
	}
	return(0);
}


/*
 * currently (2/02), ifa_addr embeds scope_id's and don't
 * have sin6_scope_id set (i.e. it's 0)
 * so, create this function to compare link local scopes
 */
uint32_t
sctp_is_same_scope(struct sockaddr_in6 *addr1, struct sockaddr_in6 *addr2) {
	struct sockaddr_in6 a, b;
	/* save copies */
	a = *addr1;
	b = *addr2;

	if (a.sin6_scope_id == 0)
		if (in6_recoverscope(&a, &a.sin6_addr, NULL))
			/* can't get scope, so can't match */
			return(0);

	if (b.sin6_scope_id == 0)
		if (in6_recoverscope(&b, &b.sin6_addr, NULL))
			/* can't get scope, so can't match */
			return(0);

	if (a.sin6_scope_id != b.sin6_scope_id)
		return(0);

	return(1);
}

/*
 * returns a sockaddr_in6 with embedded scope recovered and removed
 */
struct sockaddr_in6 *
sctp_recover_scope(struct sockaddr_in6 *addr, struct sockaddr_in6 *store) {
	/* check and strip embedded scope junk */
	if (addr->sin6_family == AF_INET6) {
		if (IN6_IS_SCOPE_LINKLOCAL(&addr->sin6_addr)) {
			if (addr->sin6_scope_id == 0) {
				*store = *addr;
				if (!in6_recoverscope(store, &store->sin6_addr,
						      NULL)) {
					/* use the recovered scope */
					addr = store;
				}
				/* else, return the original "to" addr */
			}
		}
	}
	return(addr);
}


/*
 * ntop() routines
 */
#define SPRINTF(x)	((size_t)sprintf x)
#define NS_INT16SZ    2       /* #/bytes of data in a u_int16_t */
#define NS_IN6ADDRSZ  16      /* IPv6 T_AAAA */

const char *
sctp_ntop4(const u_char *src, char *dst, size_t size) {
        char tmp[sizeof("255.255.255.255")];

        if (SPRINTF((tmp, "%u.%u.%u.%u", src[0], src[1], src[2], src[3])) >
	    size) {
                return (NULL);
        }
        strcpy(dst, tmp);
        return (dst);
}

const char *
sctp_ntop6(const u_char *src, char *dst, size_t size) {
        /*
         * Note that int32_t and int16_t need only be "at least" large enough
         * to contain a value of the specified size.  On some systems, like
         * Crays, there is no such thing as an integer variable with 16 bits.
         * Keep this in mind if you think this function should have been coded
         * to use pointer overlays.  All the world's not a VAX.
         */
        char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"], *tp;
        struct { int base, len; } best, cur;
        u_int words[NS_IN6ADDRSZ / NS_INT16SZ];
        int i;

        /*
         * Preprocess:
         *      Copy the input (bytewise) array into a wordwise array.
         *      Find the longest run of 0x00's in src[] for :: shorthanding.
         */
        memset(words, '\0', sizeof words);
        for (i = 0; i < NS_IN6ADDRSZ; i++)
                words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
        best.base = -1;
        cur.base = -1;
        for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
                if (words[i] == 0) {
                        if (cur.base == -1)
                                cur.base = i, cur.len = 1;
                        else
                                cur.len++;
                } else {
                        if (cur.base != -1) {
                                if (best.base == -1 || cur.len > best.len)
                                        best = cur;
                                cur.base = -1;
                        }
                }
        }
        if (cur.base != -1) {
                if (best.base == -1 || cur.len > best.len)
                        best = cur;
        }
        if (best.base != -1 && best.len < 2)
                best.base = -1;

        /*
         * Format the result.
         */
        tp = tmp;
        for (i = 0; i < (NS_IN6ADDRSZ / NS_INT16SZ); i++) {
                /* Are we inside the best run of 0x00's? */
                if (best.base != -1 && i >= best.base &&
                    i < (best.base + best.len)) {
                        if (i == best.base)
                                *tp++ = ':';
                        continue;
                }
                /* Are we following an initial run of 0x00s or any real hex? */
                if (i != 0)
                        *tp++ = ':';
                /* Is this address an encapsulated IPv4? */
                if (i == 6 && best.base == 0 &&
                    (best.len == 6 || (best.len == 5 && words[5] == 0xffff))) {
                        if (!sctp_ntop4(src+12, tp, sizeof tmp - (tp - tmp)))
                                return (NULL);
                        tp += strlen(tp);
                        break;
                }
                tp += SPRINTF((tp, "%x", words[i]));
        }
        /* Was it a trailing run of 0x00's? */
        if (best.base != -1 && (best.base + best.len) ==
            (NS_IN6ADDRSZ / NS_INT16SZ))
                *tp++ = ':';
        *tp++ = '\0';

        /*
         * Check for overflow, copy, and we're done.
         */
        if ((size_t)(tp - tmp) > size) {
                return (NULL);
        }
        strcpy(dst, tmp);
        return (dst);
}

void
sctp_print_address(struct sockaddr *sa)
{
	char buf[128];

	if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sin6;
		sin6 = (struct sockaddr_in6 *)sa;
		sctp_ntop6((char *)&sin6->sin6_addr, buf, sizeof(buf));	
		printf("IPv6 address: %s scope:%u\n", buf,
		       sin6->sin6_scope_id);
	} else if (sa->sa_family == AF_INET) {
		struct sockaddr_in *sin;
		sin = (struct sockaddr_in *)sa;
		sctp_ntop4((char *)&sin->sin_addr, buf, sizeof(buf));
		printf("IPv4 address: %s\n", buf);
	} else {
		printf("\n");
	}
}

int
sbappendaddr_nocheck(sb, asa, m0, control)
	struct sockbuf *sb;
	struct sockaddr *asa;
	struct mbuf *m0, *control;
{
#ifdef __FreeBSD__
	register struct mbuf *m, *n;
	int space = asa->sa_len;

if (m0 && (m0->m_flags & M_PKTHDR) == 0)
panic("sbappendaddr");
	if (m0)
		space += m0->m_pkthdr.len;
	for (n = control; n; n = n->m_next) {
		space += n->m_len;
		if (n->m_next == 0)	/* keep pointer to last control buf */
			break;
	}
	if (asa->sa_len > MLEN)
		return (0);
	MGET(m, M_DONTWAIT, MT_SONAME);
	if (m == 0)
		return (0);
	m->m_len = asa->sa_len;
	bcopy((caddr_t)asa, mtod(m, caddr_t), asa->sa_len);
	if (n)
		n->m_next = m0;		/* concatenate data to control */
	else
		control = m0;
	m->m_next = control;
	for (n = m; n; n = n->m_next)
		sballoc(sb, n);
	n = sb->sb_mb;
	if (n) {
		while (n->m_nextpkt)
			n = n->m_nextpkt;
		n->m_nextpkt = m;
	} else
		sb->sb_mb = m;
	return (1);
#endif
#ifdef __NetBSD__
	struct mbuf *m, *n;
	int space = asa->sa_len;

if (m0 && (m0->m_flags & M_PKTHDR) == 0)
panic("sbappendaddr");
	if (m0)
		space += m0->m_pkthdr.len;
	for (n = control; n; n = n->m_next) {
		space += n->m_len;
		if (n->m_next == 0)	/* keep pointer to last control buf */
			break;
	}
	MGET(m, M_DONTWAIT, MT_SONAME);
	if (m == 0)
		return (0);
	if (asa->sa_len > MLEN) {
		MEXTMALLOC(m, asa->sa_len, M_NOWAIT);
		if ((m->m_flags & M_EXT) == 0) {
			m_free(m);
			return (0);
		}
	}
	m->m_len = asa->sa_len;
	memcpy(mtod(m, caddr_t), (caddr_t)asa, asa->sa_len);
	if (n)
		n->m_next = m0;		/* concatenate data to control */
	else
		control = m0;
	m->m_next = control;
	for (n = m; n; n = n->m_next)
		sballoc(sb, n);
	if ((n = sb->sb_mb) != NULL) {
		while (n->m_nextpkt)
			n = n->m_nextpkt;
		n->m_nextpkt = m;
	} else
		sb->sb_mb = m;
	return (1);
#endif
#ifdef __OpenBSD__
	register struct mbuf *m, *n;
	int space = asa->sa_len;

	if (m0 && (m0->m_flags & M_PKTHDR) == 0)
		panic("sbappendaddr");
	if (m0)
		space += m0->m_pkthdr.len;
	for (n = control; n; n = n->m_next) {
		space += n->m_len;
		if (n->m_next == 0)	/* keep pointer to last control buf */
			break;
	}
	if (asa->sa_len > MLEN)
		return (0);
	MGET(m, M_DONTWAIT, MT_SONAME);
	if (m == 0)
		return (0);
	m->m_len = asa->sa_len;
	bcopy((caddr_t)asa, mtod(m, caddr_t), asa->sa_len);
	if (n)
		n->m_next = m0;		/* concatenate data to control */
	else
		control = m0;
	m->m_next = control;
	for (n = m; n; n = n->m_next)
		sballoc(sb, n);
	if ((n = sb->sb_mb) != NULL) {
		while (n->m_nextpkt)
			n = n->m_nextpkt;
		n->m_nextpkt = m;
	} else
		sb->sb_mb = m;
	return (1);
#endif
}

