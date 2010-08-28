/*
 * Copyright (C) 2004-2010 Michael Tuexen, tuexen@fh-muenster.de
 *
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

#include <sys/sysctl.h>
#include <netinet/in.h>
#include <sys/protosw.h>
#include <netinet/ip.h>
#include <sys/lock.h>
#include <sys/domain.h>
#include <net/route.h>
#include <sys/socketvar.h>
#include <netinet/in_pcb.h>
#include <netinet/sctp_os.h>
#include <netinet/sctp_pcb.h>
#include <netinet/sctp_var.h>
#include <netinet/sctp_timer.h>
#ifdef INET6
#include <netinet6/sctp6_var.h>
#endif
#include <netinet/sctp.h>
#if defined(APPLE_TIGER) || defined(APPLE_LEOPARD)
#include <netinet/udp.h>
#include <netinet/udp_var.h>

extern struct pr_usrreqs udp_usrreqs;
#ifdef INET6
extern struct pr_usrreqs udp6_usrreqs;
#endif
extern struct inpcbinfo udbinfo;
#endif

SYSCTL_DECL(_net_inet);
#ifdef INET6
SYSCTL_DECL(_net_inet6);
#endif
SYSCTL_NODE(_net_inet, IPPROTO_SCTP,    sctp,   CTLFLAG_RW, 0,  "SCTP")
#ifdef INET6
SYSCTL_NODE(_net_inet6, IPPROTO_SCTP,   sctp6,  CTLFLAG_RW, 0,  "SCTP6")
#endif

extern struct sysctl_oid sysctl__net_inet_sctp_sendspace;
extern struct sysctl_oid sysctl__net_inet_sctp_recvspace;
#if defined(SCTP_APPLE_AUTO_ASCONF)
extern struct sysctl_oid sysctl__net_inet_sctp_auto_asconf;
#endif
extern struct sysctl_oid sysctl__net_inet_sctp_ecn_enable;
extern struct sysctl_oid sysctl__net_inet_sctp_ecn_nonce;
extern struct sysctl_oid sysctl__net_inet_sctp_strict_sacks;
extern struct sysctl_oid sysctl__net_inet_sctp_loopback_nocsum;
extern struct sysctl_oid sysctl__net_inet_sctp_strict_init;
extern struct sysctl_oid sysctl__net_inet_sctp_peer_chkoh;
extern struct sysctl_oid sysctl__net_inet_sctp_maxburst;
extern struct sysctl_oid sysctl__net_inet_sctp_maxchunks;
extern struct sysctl_oid sysctl__net_inet_sctp_delayed_sack_time;
extern struct sysctl_oid sysctl__net_inet_sctp_sack_freq;
extern struct sysctl_oid sysctl__net_inet_sctp_heartbeat_interval;
extern struct sysctl_oid sysctl__net_inet_sctp_pmtu_raise_time;
extern struct sysctl_oid sysctl__net_inet_sctp_shutdown_guard_time;
extern struct sysctl_oid sysctl__net_inet_sctp_secret_lifetime;
extern struct sysctl_oid sysctl__net_inet_sctp_rto_max;
extern struct sysctl_oid sysctl__net_inet_sctp_rto_min;
extern struct sysctl_oid sysctl__net_inet_sctp_rto_initial;
extern struct sysctl_oid sysctl__net_inet_sctp_init_rto_max;
extern struct sysctl_oid sysctl__net_inet_sctp_valid_cookie_life;
extern struct sysctl_oid sysctl__net_inet_sctp_init_rtx_max;
extern struct sysctl_oid sysctl__net_inet_sctp_assoc_rtx_max;
extern struct sysctl_oid sysctl__net_inet_sctp_path_rtx_max;
extern struct sysctl_oid sysctl__net_inet_sctp_outgoing_streams;
extern struct sysctl_oid sysctl__net_inet_sctp_cmt_on_off;
extern struct sysctl_oid sysctl__net_inet_sctp_nr_sack_on_off;
extern struct sysctl_oid sysctl__net_inet_sctp_cmt_use_dac;
extern struct sysctl_oid sysctl__net_inet_sctp_cmt_pf;
extern struct sysctl_oid sysctl__net_inet_sctp_cwnd_maxburst;
extern struct sysctl_oid sysctl__net_inet_sctp_early_fast_retran;
extern struct sysctl_oid sysctl__net_inet_sctp_early_fast_retran_msec;
extern struct sysctl_oid sysctl__net_inet_sctp_asconf_auth_nochk;
extern struct sysctl_oid sysctl__net_inet_sctp_auth_disable;
extern struct sysctl_oid sysctl__net_inet_sctp_nat_friendly;
extern struct sysctl_oid sysctl__net_inet_sctp_abc_l_var;
extern struct sysctl_oid sysctl__net_inet_sctp_max_chained_mbufs;
extern struct sysctl_oid sysctl__net_inet_sctp_do_sctp_drain;
extern struct sysctl_oid sysctl__net_inet_sctp_hb_max_burst;
extern struct sysctl_oid sysctl__net_inet_sctp_abort_at_limit;
extern struct sysctl_oid sysctl__net_inet_sctp_strict_data_order;
extern struct sysctl_oid sysctl__net_inet_sctp_min_residual;
extern struct sysctl_oid sysctl__net_inet_sctp_max_retran_chunk;
extern struct sysctl_oid sysctl__net_inet_sctp_log_level;
extern struct sysctl_oid sysctl__net_inet_sctp_default_cc_module;
extern struct sysctl_oid sysctl__net_inet_sctp_default_frag_interleave;
#if defined(SCTP_APPLE_MOBILITY_BASE)
extern struct sysctl_oid sysctl__net_inet_sctp_mobility_base;
#endif
#if defined(SCTP_APPLE_MOBILITY_FASTHANDOFF)
extern struct sysctl_oid sysctl__net_inet_sctp_mobility_fasthandoff;
#endif
#if defined(SCTP_LOCAL_TRACE_BUF)
extern struct sysctl_oid sysctl__net_inet_sctp_log;
extern struct sysctl_oid sysctl__net_inet_sctp_clear_trace;
#endif
extern struct sysctl_oid sysctl__net_inet_sctp_udp_tunneling_for_client_enable;
extern struct sysctl_oid sysctl__net_inet_sctp_udp_tunneling_port;
extern struct sysctl_oid sysctl__net_inet_sctp_enable_sack_immediately;
extern struct sysctl_oid sysctl__net_inet_sctp_nat_friendly_init;
extern struct sysctl_oid sysctl__net_inet_sctp_vtag_time_wait;
#if defined(SCTP_DEBUG)
extern struct sysctl_oid sysctl__net_inet_sctp_debug;
#endif
extern struct sysctl_oid sysctl__net_inet_sctp_stats;
extern struct sysctl_oid sysctl__net_inet_sctp_assoclist;
extern struct sysctl_oid sysctl__net_inet_sctp_main_timer;
extern struct sysctl_oid sysctl__net_inet_sctp_ignore_vmware_interfaces;
extern struct sysctl_oid sysctl__net_inet_sctp_output_unlocked;
extern struct sysctl_oid sysctl__net_inet_sctp_addr_watchdog_limit;
extern struct sysctl_oid sysctl__net_inet_sctp_vtag_watchdog_limit;

extern struct domain inetdomain;
#ifdef INET6
extern struct domain inet6domain;
#endif
extern struct protosw *ip_protox[];
#ifdef INET6
extern struct protosw *ip6_protox[];
#endif
extern struct sctp_epinfo sctppcinfo;

struct protosw sctp4_dgram;
struct protosw sctp4_seqpacket;
struct protosw sctp4_stream;
#ifdef INET6
struct protosw sctp6_dgram;
struct protosw sctp6_seqpacket;
struct protosw sctp6_stream;
#endif

struct protosw *old_pr4;
#ifdef INET6
struct protosw *old_pr6;
#endif

#if defined(APPLE_TIGER) || defined(APPLE_LEOPARD)
static int
soreceive_fix(struct socket *so, struct sockaddr **psa, struct uio *uio,  struct mbuf **mp0, struct mbuf **controlp, int *flagsp)
{
	if ((controlp) && (*controlp)) {
		m_freem(*controlp);
	}
	return soreceive(so, psa, uio, mp0, controlp, flagsp);
}
#endif

kern_return_t 
SCTP_start (kmod_info_t * ki __attribute__((unused)), void * d __attribute__((unused)))
{
	int err;

	old_pr4  = ip_protox [IPPROTO_SCTP];
#ifdef INET6
	old_pr6  = ip6_protox[IPPROTO_SCTP];
#endif INET6

	bzero(&sctp4_dgram,     sizeof(struct protosw));
	bzero(&sctp4_seqpacket, sizeof(struct protosw));
	bzero(&sctp4_stream,    sizeof(struct protosw));
#ifdef INET6
	bzero(&sctp6_dgram,     sizeof(struct protosw));
	bzero(&sctp6_seqpacket, sizeof(struct protosw));
	bzero(&sctp6_stream,    sizeof(struct protosw));
#endif

	sctp4_dgram.pr_type          = SOCK_DGRAM;
	sctp4_dgram.pr_domain        = &inetdomain;
	sctp4_dgram.pr_protocol      = IPPROTO_SCTP;
	sctp4_dgram.pr_flags         = PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp4_dgram.pr_input         = sctp_input;
	sctp4_dgram.pr_output        = 0;
	sctp4_dgram.pr_ctlinput      = sctp_ctlinput;
	sctp4_dgram.pr_ctloutput     = sctp_ctloutput;
	sctp4_dgram.pr_ousrreq       = 0;
	sctp4_dgram.pr_init          = sctp_init;
	sctp4_dgram.pr_fasttimo      = 0;
	sctp4_dgram.pr_slowtimo      = sctp_slowtimo;
	sctp4_dgram.pr_drain         = sctp_drain;
	sctp4_dgram.pr_sysctl        = 0;
	sctp4_dgram.pr_usrreqs       = &sctp_usrreqs;
	sctp4_dgram.pr_lock          = sctp_lock;
	sctp4_dgram.pr_unlock        = sctp_unlock;
	sctp4_dgram.pr_getlock       = sctp_getlock;

	sctp4_seqpacket.pr_type	     = SOCK_SEQPACKET;
	sctp4_seqpacket.pr_domain    = &inetdomain;
	sctp4_seqpacket.pr_protocol  = IPPROTO_SCTP;
	sctp4_seqpacket.pr_flags     = PR_CONNREQUIRED|PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp4_seqpacket.pr_input     = sctp_input;
	sctp4_seqpacket.pr_output    = 0;
	sctp4_seqpacket.pr_ctlinput  = sctp_ctlinput;
	sctp4_seqpacket.pr_ctloutput = sctp_ctloutput;
	sctp4_seqpacket.pr_ousrreq   = 0;
	sctp4_seqpacket.pr_init      = 0;
	sctp4_seqpacket.pr_fasttimo  = 0;
	sctp4_seqpacket.pr_slowtimo  = 0;
	sctp4_seqpacket.pr_drain     = sctp_drain;
	sctp4_seqpacket.pr_sysctl    = 0;
	sctp4_seqpacket.pr_usrreqs   = &sctp_usrreqs;
	sctp4_seqpacket.pr_lock      = sctp_lock;
	sctp4_seqpacket.pr_unlock    = sctp_unlock;
	sctp4_seqpacket.pr_getlock   = sctp_getlock;

	sctp4_stream.pr_type         = SOCK_STREAM;
	sctp4_stream.pr_domain       = &inetdomain;
	sctp4_stream.pr_protocol     = IPPROTO_SCTP;
	sctp4_stream.pr_flags        = PR_CONNREQUIRED|PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp4_stream.pr_input        = sctp_input;
	sctp4_stream.pr_output       = 0;
	sctp4_stream.pr_ctlinput     = sctp_ctlinput;
	sctp4_stream.pr_ctloutput    = sctp_ctloutput;
	sctp4_stream.pr_ousrreq      = 0;
	sctp4_stream.pr_init         = 0;
	sctp4_stream.pr_fasttimo     = 0;
	sctp4_stream.pr_slowtimo     = 0;
	sctp4_stream.pr_drain        = sctp_drain;
	sctp4_stream.pr_sysctl       = 0;
	sctp4_stream.pr_usrreqs      = &sctp_usrreqs;
	sctp4_stream.pr_lock         = sctp_lock;
	sctp4_stream.pr_unlock       = sctp_unlock;
	sctp4_stream.pr_getlock      = sctp_getlock;

#ifdef INET6
	sctp6_dgram.pr_type          = SOCK_DGRAM;
	sctp6_dgram.pr_domain        = &inet6domain;
	sctp6_dgram.pr_protocol      = IPPROTO_SCTP;
	sctp6_dgram.pr_flags         = PR_CONNREQUIRED|PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp6_dgram.pr_input         = (void (*) (struct mbuf *, int)) sctp6_input;
	sctp6_dgram.pr_output        = 0;
	sctp6_dgram.pr_ctlinput      = sctp6_ctlinput;
	sctp6_dgram.pr_ctloutput     = sctp_ctloutput;
	sctp6_dgram.pr_ousrreq       = 0;
	sctp6_dgram.pr_init          = 0;
	sctp6_dgram.pr_fasttimo      = 0;
	sctp6_dgram.pr_slowtimo      = 0;
	sctp6_dgram.pr_drain         = sctp_drain;
	sctp6_dgram.pr_sysctl        = 0;
	sctp6_dgram.pr_usrreqs       = &sctp6_usrreqs;
	sctp6_dgram.pr_lock          = sctp_lock;
	sctp6_dgram.pr_unlock        = sctp_unlock;
	sctp6_dgram.pr_getlock       = sctp_getlock;

	sctp6_seqpacket.pr_type      = SOCK_SEQPACKET;
	sctp6_seqpacket.pr_domain    = &inet6domain;
	sctp6_seqpacket.pr_protocol  = IPPROTO_SCTP;
	sctp6_seqpacket.pr_flags     = PR_CONNREQUIRED|PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp6_seqpacket.pr_input     = (void (*) (struct mbuf *, int)) sctp6_input;
	sctp6_seqpacket.pr_output    = 0;
	sctp6_seqpacket.pr_ctlinput  = sctp6_ctlinput;
	sctp6_seqpacket.pr_ctloutput = sctp_ctloutput;
	sctp6_seqpacket.pr_ousrreq   = 0;
	sctp6_seqpacket.pr_init      = 0;
	sctp6_seqpacket.pr_fasttimo  = 0;
	sctp6_seqpacket.pr_slowtimo  = 0;
	sctp6_seqpacket.pr_drain     = sctp_drain;
	sctp6_seqpacket.pr_sysctl    = 0;
	sctp6_seqpacket.pr_usrreqs   = &sctp6_usrreqs;
	sctp6_seqpacket.pr_lock      = sctp_lock;
	sctp6_seqpacket.pr_unlock    = sctp_unlock;
	sctp6_seqpacket.pr_getlock   = sctp_getlock;

	sctp6_stream.pr_type         = SOCK_STREAM;
	sctp6_stream.pr_domain       = &inet6domain;
	sctp6_stream.pr_protocol     = IPPROTO_SCTP;
	sctp6_stream.pr_flags        = PR_CONNREQUIRED|PR_WANTRCVD|PR_PCBLOCK|PR_PROTOLOCK;
	sctp6_stream.pr_input        = (void (*) (struct mbuf *, int)) sctp6_input;
	sctp6_stream.pr_output       = 0;
	sctp6_stream.pr_ctlinput     = sctp6_ctlinput;
	sctp6_stream.pr_ctloutput    = sctp_ctloutput;
	sctp6_stream.pr_ousrreq      = 0;
	sctp6_stream.pr_init         = 0;
	sctp6_stream.pr_fasttimo     = 0;
	sctp6_stream.pr_slowtimo     = 0;
	sctp6_stream.pr_drain        = sctp_drain;
	sctp6_stream.pr_sysctl       = 0;
	sctp6_stream.pr_usrreqs      = &sctp6_usrreqs;
	sctp6_stream.pr_lock         = sctp_lock;
	sctp6_stream.pr_unlock       = sctp_unlock;
	sctp6_stream.pr_getlock      = sctp_getlock;
#endif

	lck_mtx_lock(inetdomain.dom_mtx);
#ifdef INET6
	lck_mtx_lock(inet6domain.dom_mtx);
#endif

	err  = net_add_proto(&sctp4_dgram,     &inetdomain);
	err |= net_add_proto(&sctp4_seqpacket, &inetdomain);
	err |= net_add_proto(&sctp4_stream,    &inetdomain);
#ifdef INET6
	err |= net_add_proto(&sctp6_dgram,     &inet6domain);
	err |= net_add_proto(&sctp6_seqpacket, &inet6domain);
	err |= net_add_proto(&sctp6_stream,    &inet6domain);
#endif
	if (err) {
#ifdef INET6
		lck_mtx_unlock(inet6domain.dom_mtx);
#endif
		lck_mtx_unlock(inetdomain.dom_mtx);
		printf("SCTP NKE: Not all protocol handlers could be installed.\n");
		return KERN_FAILURE;
	}

	ip_protox[IPPROTO_SCTP]  = &sctp4_dgram;
#ifdef INET6
	ip6_protox[IPPROTO_SCTP] = &sctp6_dgram;
#endif

#ifdef INET6
	lck_mtx_unlock(inet6domain.dom_mtx);
#endif
	lck_mtx_unlock(inetdomain.dom_mtx);
	
	sysctl_register_oid(&sysctl__net_inet_sctp);
	sysctl_register_oid(&sysctl__net_inet_sctp_sendspace);
	sysctl_register_oid(&sysctl__net_inet_sctp_recvspace);
#if defined(SCTP_APPLE_AUTO_ASCONF)
	sysctl_register_oid(&sysctl__net_inet_sctp_auto_asconf);
#endif
	sysctl_register_oid(&sysctl__net_inet_sctp_ecn_enable);
	sysctl_register_oid(&sysctl__net_inet_sctp_ecn_nonce);
	sysctl_register_oid(&sysctl__net_inet_sctp_strict_sacks);
	sysctl_register_oid(&sysctl__net_inet_sctp_loopback_nocsum);
	sysctl_register_oid(&sysctl__net_inet_sctp_strict_init);
	sysctl_register_oid(&sysctl__net_inet_sctp_peer_chkoh);
	sysctl_register_oid(&sysctl__net_inet_sctp_maxburst);
	sysctl_register_oid(&sysctl__net_inet_sctp_maxchunks);
	sysctl_register_oid(&sysctl__net_inet_sctp_delayed_sack_time);
	sysctl_register_oid(&sysctl__net_inet_sctp_sack_freq);
	sysctl_register_oid(&sysctl__net_inet_sctp_heartbeat_interval);
	sysctl_register_oid(&sysctl__net_inet_sctp_pmtu_raise_time);
	sysctl_register_oid(&sysctl__net_inet_sctp_shutdown_guard_time);
	sysctl_register_oid(&sysctl__net_inet_sctp_secret_lifetime);
	sysctl_register_oid(&sysctl__net_inet_sctp_rto_max);
	sysctl_register_oid(&sysctl__net_inet_sctp_rto_min);
	sysctl_register_oid(&sysctl__net_inet_sctp_rto_initial);
	sysctl_register_oid(&sysctl__net_inet_sctp_init_rto_max);
	sysctl_register_oid(&sysctl__net_inet_sctp_valid_cookie_life);
	sysctl_register_oid(&sysctl__net_inet_sctp_init_rtx_max);
	sysctl_register_oid(&sysctl__net_inet_sctp_assoc_rtx_max);
	sysctl_register_oid(&sysctl__net_inet_sctp_path_rtx_max);
	sysctl_register_oid(&sysctl__net_inet_sctp_outgoing_streams);
	sysctl_register_oid(&sysctl__net_inet_sctp_cmt_on_off);
	sysctl_register_oid(&sysctl__net_inet_sctp_nr_sack_on_off);
	sysctl_register_oid(&sysctl__net_inet_sctp_cmt_use_dac);
	sysctl_register_oid(&sysctl__net_inet_sctp_cmt_pf);
	sysctl_register_oid(&sysctl__net_inet_sctp_cwnd_maxburst);
	sysctl_register_oid(&sysctl__net_inet_sctp_early_fast_retran);
	sysctl_register_oid(&sysctl__net_inet_sctp_early_fast_retran_msec);
	sysctl_register_oid(&sysctl__net_inet_sctp_asconf_auth_nochk);
	sysctl_register_oid(&sysctl__net_inet_sctp_auth_disable);
	sysctl_register_oid(&sysctl__net_inet_sctp_nat_friendly);
	sysctl_register_oid(&sysctl__net_inet_sctp_abc_l_var);
	sysctl_register_oid(&sysctl__net_inet_sctp_max_chained_mbufs);
	sysctl_register_oid(&sysctl__net_inet_sctp_do_sctp_drain);
	sysctl_register_oid(&sysctl__net_inet_sctp_hb_max_burst);
	sysctl_register_oid(&sysctl__net_inet_sctp_abort_at_limit);
	sysctl_register_oid(&sysctl__net_inet_sctp_strict_data_order);
	sysctl_register_oid(&sysctl__net_inet_sctp_min_residual);
	sysctl_register_oid(&sysctl__net_inet_sctp_max_retran_chunk);
	sysctl_register_oid(&sysctl__net_inet_sctp_log_level);
	sysctl_register_oid(&sysctl__net_inet_sctp_default_cc_module);
	sysctl_register_oid(&sysctl__net_inet_sctp_default_frag_interleave);
#if defined(SCTP_APPLE_MOBILITY_BASE)
	sysctl_register_oid(&sysctl__net_inet_sctp_mobility_base);
#endif
#if defined(SCTP_APPLE_MOBILITY_FASTHANDOFF)
	sysctl_register_oid(&sysctl__net_inet_sctp_mobility_fasthandoff);
#endif
#if defined(SCTP_LOCAL_TRACE_BUF)
	sysctl_register_oid(&sysctl__net_inet_sctp_log);
	sysctl_register_oid(&sysctl__net_inet_sctp_clear_trace);
#endif
	sysctl_register_oid(&sysctl__net_inet_sctp_udp_tunneling_for_client_enable);
	sysctl_register_oid(&sysctl__net_inet_sctp_udp_tunneling_port);
	sysctl_register_oid(&sysctl__net_inet_sctp_enable_sack_immediately);
	sysctl_register_oid(&sysctl__net_inet_sctp_nat_friendly_init);
	sysctl_register_oid(&sysctl__net_inet_sctp_vtag_time_wait);
#ifdef SCTP_DEBUG
	sysctl_register_oid(&sysctl__net_inet_sctp_debug);
#endif
	sysctl_register_oid(&sysctl__net_inet_sctp_stats);
	sysctl_register_oid(&sysctl__net_inet_sctp_assoclist);
	sysctl_register_oid(&sysctl__net_inet_sctp_main_timer);
	sysctl_register_oid(&sysctl__net_inet_sctp_ignore_vmware_interfaces);
	sysctl_register_oid(&sysctl__net_inet_sctp_output_unlocked);
	sysctl_register_oid(&sysctl__net_inet_sctp_addr_watchdog_limit);
	sysctl_register_oid(&sysctl__net_inet_sctp_vtag_watchdog_limit);

#if defined(APPLE_TIGER) || defined(APPLE_LEOPARD)
	lck_rw_lock_exclusive(udbinfo.mtx);
	udp_usrreqs.pru_soreceive = soreceive_fix;
#ifdef INET6
	udp6_usrreqs.pru_soreceive = soreceive_fix;
#endif
	lck_rw_done(udbinfo.mtx);
#endif
	printf("SCTP NKE: NKE loaded.\n");
	return KERN_SUCCESS;
}


kern_return_t 
SCTP_stop (kmod_info_t * ki __attribute__((unused)), void * d __attribute__((unused)))
{
	struct inpcb *inp;
	int err;
	
	if (!lck_rw_try_lock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx))) {
		printf("SCTP NKE: Someone else holds the lock\n");
		return KERN_FAILURE;
	}
	if (!LIST_EMPTY(&SCTP_BASE_INFO(listhead))) {
		printf("SCTP NKE: There are still SCTP endpoints. NKE not unloaded\n");
		lck_rw_unlock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx));
		return KERN_FAILURE;
	}

	if (!LIST_EMPTY(&SCTP_BASE_INFO(inplisthead))) {
		printf("SCTP NKE: There are still not deleted SCTP endpoints. NKE not unloaded\n");
		LIST_FOREACH(inp, &SCTP_BASE_INFO(inplisthead), inp_list) {
			printf("inp = %p: inp_wantcnt = %d, inp_state = %d, inp_socket->so_usecount = %d\n", inp, inp->inp_wantcnt, inp->inp_state, inp->inp_socket->so_usecount);
		}
		lck_rw_unlock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx));
		return KERN_FAILURE;
	}

#if defined(APPLE_TIGER) || defined(APPLE_LEOPARD)
	lck_rw_lock_exclusive(udbinfo.mtx);
	udp_usrreqs.pru_soreceive = soreceive;
#ifdef INET6
	udp6_usrreqs.pru_soreceive = soreceive;
#endif
	lck_rw_done(udbinfo.mtx);
#endif
	sysctl_unregister_oid(&sysctl__net_inet_sctp_sendspace);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_recvspace);
#if defined(SCTP_APPLE_AUTO_ASCONF)
	sysctl_unregister_oid(&sysctl__net_inet_sctp_auto_asconf);
#endif
	sysctl_unregister_oid(&sysctl__net_inet_sctp_ecn_enable);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_ecn_nonce);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_strict_sacks);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_loopback_nocsum);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_strict_init);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_peer_chkoh);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_maxburst);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_maxchunks);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_delayed_sack_time);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_sack_freq);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_heartbeat_interval);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_pmtu_raise_time);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_shutdown_guard_time);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_secret_lifetime);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_rto_max);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_rto_min);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_rto_initial);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_init_rto_max);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_valid_cookie_life);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_init_rtx_max);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_assoc_rtx_max);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_path_rtx_max);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_outgoing_streams);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_cmt_on_off);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_nr_sack_on_off);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_cmt_use_dac);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_cmt_pf);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_cwnd_maxburst);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_early_fast_retran);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_early_fast_retran_msec);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_asconf_auth_nochk);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_auth_disable);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_nat_friendly);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_abc_l_var);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_max_chained_mbufs);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_do_sctp_drain);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_hb_max_burst);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_abort_at_limit);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_strict_data_order);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_min_residual);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_max_retran_chunk);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_log_level);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_default_cc_module);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_default_frag_interleave);
#if defined(SCTP_APPLE_MOBILITY_BASE)
	sysctl_unregister_oid(&sysctl__net_inet_sctp_mobility_base);
#endif
#if defined(SCTP_APPLE_MOBILITY_FASTHANDOFF)
	sysctl_unregister_oid(&sysctl__net_inet_sctp_mobility_fasthandoff);
#endif
#if defined(SCTP_LOCAL_TRACE_BUF)
	sysctl_unregister_oid(&sysctl__net_inet_sctp_log);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_clear_trace);
#endif
	sysctl_unregister_oid(&sysctl__net_inet_sctp_udp_tunneling_for_client_enable);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_udp_tunneling_port);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_enable_sack_immediately);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_nat_friendly_init);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_vtag_time_wait);
#ifdef SCTP_DEBUG
	sysctl_unregister_oid(&sysctl__net_inet_sctp_debug);
#endif
	sysctl_unregister_oid(&sysctl__net_inet_sctp_stats);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_assoclist);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_main_timer);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_ignore_vmware_interfaces);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_output_unlocked);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_addr_watchdog_limit);
	sysctl_unregister_oid(&sysctl__net_inet_sctp_vtag_watchdog_limit);
	sysctl_unregister_oid(&sysctl__net_inet_sctp);

	lck_mtx_lock(inetdomain.dom_mtx);
#ifdef INET6
	lck_mtx_lock(inet6domain.dom_mtx);
#endif

	ip_protox[IPPROTO_SCTP]  = old_pr4;
#ifdef INET6
	ip6_protox[IPPROTO_SCTP] = old_pr6;
#endif

	err  = net_del_proto(sctp4_dgram.pr_type,     sctp4_dgram.pr_protocol,     &inetdomain);
	err |= net_del_proto(sctp4_seqpacket.pr_type, sctp4_seqpacket.pr_protocol, &inetdomain);
	err |= net_del_proto(sctp4_stream.pr_type,    sctp4_stream.pr_protocol,    &inetdomain);
#ifdef INET6
	err |= net_del_proto(sctp6_dgram.pr_type,     sctp6_dgram.pr_protocol,     &inet6domain);
	err |= net_del_proto(sctp6_seqpacket.pr_type, sctp6_seqpacket.pr_protocol, &inet6domain);
	err |= net_del_proto(sctp6_stream.pr_type,    sctp6_stream.pr_protocol,    &inet6domain);
#endif
	lck_rw_unlock_exclusive(SCTP_BASE_INFO(ipi_ep_mtx));
	sctp_finish();
#ifdef INET6
	lck_mtx_unlock(inet6domain.dom_mtx);
#endif
	lck_mtx_unlock(inetdomain.dom_mtx);

	if (err) {
		printf("SCTP NKE: Not all protocol handlers could be removed.\n");
		return KERN_FAILURE;
	} else {
		printf("SCTP NKE: NKE unloaded.\n");
		return KERN_SUCCESS;
	}
}
