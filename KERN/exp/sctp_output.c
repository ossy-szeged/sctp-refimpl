/*-
 * Copyright (C) 2002-2006 Cisco Systems Inc,
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

/* $KAME: sctp_output.c,v 1.46 2005/03/06 16:04:17 itojun Exp $	 */

#ifdef __FreeBSD__
#include <sys/cdefs.h>
__FBSDID("$FreeBSD:$");
#endif

#if !(defined(__OpenBSD__) || defined (__APPLE__))
#include "opt_ipsec.h"
#endif
#if defined(__FreeBSD__)
#include "opt_compat.h"
#include "opt_inet6.h"
#include "opt_inet.h"
#endif
#if defined(__NetBSD__)
#include "opt_inet.h"
#endif
#ifdef __APPLE__
#include <sctp.h>
#elif !defined(__OpenBSD__)
#include "opt_sctp.h"
#endif
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#ifndef __OpenBSD__
#include <sys/domain.h>
#endif
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/resourcevar.h>
#include <sys/uio.h>
#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
#include <sys/proc_internal.h>
#include <sys/uio_internal.h>
#endif
#ifdef INET6
#include <sys/domain.h>
#endif

#if (defined(__FreeBSD__) && __FreeBSD_version >= 500000)
#include <sys/limits.h>
#else
#include <machine/limits.h>
#endif
#ifndef __APPLE__
#include <machine/cpu.h>
#endif

#include <net/if.h>
#include <net/if_types.h>

#if defined(__FreeBSD__)
#include <net/if_var.h>
#endif

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
#include <netinet6/scope6_var.h>
#include <netinet6/nd6.h>

#if defined(__FreeBSD__) || defined(__NetBSD__)
#include <netinet6/in6_pcb.h>
#elif defined(__OpenBSD__)
#include <netinet/in_pcb.h>
#endif

#include <netinet/icmp6.h>

#endif				/* INET6 */

#ifndef __APPLE__
#include <net/net_osdep.h>
#endif

#if defined(HAVE_NRL_INPCB)
#ifndef in6pcb
#define in6pcb		inpcb
#endif
#endif

#if defined(__FreeBSD__)
#ifndef in6pcb
#define in6pcb		inpcb
#endif
#endif


#include <netinet/sctp_pcb.h>

#ifdef IPSEC
#ifndef __OpenBSD__
#include <netinet6/ipsec.h>
#include <netkey/key.h>
#else
#undef IPSEC
#endif
#endif				/* IPSEC */

#include <netinet/sctp_var.h>
#include <netinet/sctp_header.h>
#include <netinet/sctputil.h>
#include <netinet/sctp_pcb.h>
#include <netinet/sctp_output.h>
#include <netinet/sctp_uio.h>
#include <netinet/sctputil.h>
#include <netinet/sctp_hashdriver.h>
#include <netinet/sctp_timer.h>
#include <netinet/sctp_asconf.h>
#include <netinet/sctp_indata.h>

#ifdef SCTP_DEBUG
extern uint32_t sctp_debug_on;
#endif

#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
#define APPLE_FILE_NO 3
#endif


#define SCTP_MAX_GAPS_INARRAY 4
struct sack_track {
	uint8_t right_edge;	/* mergable on the right edge */
	uint8_t left_edge;	/* mergable on the left edge */
	uint8_t num_entries;
	uint8_t spare;
	struct sctp_gap_ack_block gaps[SCTP_MAX_GAPS_INARRAY];
};

struct sack_track sack_array[256] = {
	{0, 0, 0, 0,		/* 0x00 */
		{{0, 0},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x01 */
		{{0, 0},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x02 */
		{{1, 1},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x03 */
		{{0, 1},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x04 */
		{{2, 2},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x05 */
		{{0, 0},
		{2, 2},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x06 */
		{{1, 2},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x07 */
		{{0, 2},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x08 */
		{{3, 3},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x09 */
		{{0, 0},
		{3, 3},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x0a */
		{{1, 1},
		{3, 3},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x0b */
		{{0, 1},
		{3, 3},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x0c */
		{{2, 3},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x0d */
		{{0, 0},
		{2, 3},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x0e */
		{{1, 3},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x0f */
		{{0, 3},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x10 */
		{{4, 4},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x11 */
		{{0, 0},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x12 */
		{{1, 1},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x13 */
		{{0, 1},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x14 */
		{{2, 2},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x15 */
		{{0, 0},
		{2, 2},
		{4, 4},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x16 */
		{{1, 2},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x17 */
		{{0, 2},
		{4, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x18 */
		{{3, 4},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x19 */
		{{0, 0},
		{3, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x1a */
		{{1, 1},
		{3, 4},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x1b */
		{{0, 1},
		{3, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x1c */
		{{2, 4},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x1d */
		{{0, 0},
		{2, 4},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x1e */
		{{1, 4},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x1f */
		{{0, 4},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x20 */
		{{5, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x21 */
		{{0, 0},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x22 */
		{{1, 1},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x23 */
		{{0, 1},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x24 */
		{{2, 2},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x25 */
		{{0, 0},
		{2, 2},
		{5, 5},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x26 */
		{{1, 2},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x27 */
		{{0, 2},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x28 */
		{{3, 3},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x29 */
		{{0, 0},
		{3, 3},
		{5, 5},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x2a */
		{{1, 1},
		{3, 3},
		{5, 5},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x2b */
		{{0, 1},
		{3, 3},
		{5, 5},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x2c */
		{{2, 3},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x2d */
		{{0, 0},
		{2, 3},
		{5, 5},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x2e */
		{{1, 3},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x2f */
		{{0, 3},
		{5, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x30 */
		{{4, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x31 */
		{{0, 0},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x32 */
		{{1, 1},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x33 */
		{{0, 1},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x34 */
		{{2, 2},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x35 */
		{{0, 0},
		{2, 2},
		{4, 5},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x36 */
		{{1, 2},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x37 */
		{{0, 2},
		{4, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x38 */
		{{3, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x39 */
		{{0, 0},
		{3, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x3a */
		{{1, 1},
		{3, 5},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x3b */
		{{0, 1},
		{3, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x3c */
		{{2, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x3d */
		{{0, 0},
		{2, 5},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x3e */
		{{1, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x3f */
		{{0, 5},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x40 */
		{{6, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x41 */
		{{0, 0},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x42 */
		{{1, 1},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x43 */
		{{0, 1},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x44 */
		{{2, 2},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x45 */
		{{0, 0},
		{2, 2},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x46 */
		{{1, 2},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x47 */
		{{0, 2},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x48 */
		{{3, 3},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x49 */
		{{0, 0},
		{3, 3},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x4a */
		{{1, 1},
		{3, 3},
		{6, 6},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x4b */
		{{0, 1},
		{3, 3},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x4c */
		{{2, 3},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x4d */
		{{0, 0},
		{2, 3},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x4e */
		{{1, 3},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x4f */
		{{0, 3},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x50 */
		{{4, 4},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x51 */
		{{0, 0},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x52 */
		{{1, 1},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x53 */
		{{0, 1},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x54 */
		{{2, 2},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{1, 0, 4, 0,		/* 0x55 */
		{{0, 0},
		{2, 2},
		{4, 4},
		{6, 6}
		}
	},
	{0, 0, 3, 0,		/* 0x56 */
		{{1, 2},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x57 */
		{{0, 2},
		{4, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x58 */
		{{3, 4},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x59 */
		{{0, 0},
		{3, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x5a */
		{{1, 1},
		{3, 4},
		{6, 6},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x5b */
		{{0, 1},
		{3, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x5c */
		{{2, 4},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x5d */
		{{0, 0},
		{2, 4},
		{6, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x5e */
		{{1, 4},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x5f */
		{{0, 4},
		{6, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x60 */
		{{5, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x61 */
		{{0, 0},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x62 */
		{{1, 1},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x63 */
		{{0, 1},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x64 */
		{{2, 2},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x65 */
		{{0, 0},
		{2, 2},
		{5, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x66 */
		{{1, 2},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x67 */
		{{0, 2},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x68 */
		{{3, 3},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x69 */
		{{0, 0},
		{3, 3},
		{5, 6},
		{0, 0}
		}
	},
	{0, 0, 3, 0,		/* 0x6a */
		{{1, 1},
		{3, 3},
		{5, 6},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x6b */
		{{0, 1},
		{3, 3},
		{5, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x6c */
		{{2, 3},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x6d */
		{{0, 0},
		{2, 3},
		{5, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x6e */
		{{1, 3},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x6f */
		{{0, 3},
		{5, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x70 */
		{{4, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x71 */
		{{0, 0},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x72 */
		{{1, 1},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x73 */
		{{0, 1},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x74 */
		{{2, 2},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 3, 0,		/* 0x75 */
		{{0, 0},
		{2, 2},
		{4, 6},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x76 */
		{{1, 2},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x77 */
		{{0, 2},
		{4, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x78 */
		{{3, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x79 */
		{{0, 0},
		{3, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 2, 0,		/* 0x7a */
		{{1, 1},
		{3, 6},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x7b */
		{{0, 1},
		{3, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x7c */
		{{2, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 2, 0,		/* 0x7d */
		{{0, 0},
		{2, 6},
		{0, 0},
		{0, 0}
		}
	},
	{0, 0, 1, 0,		/* 0x7e */
		{{1, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 0, 1, 0,		/* 0x7f */
		{{0, 6},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0x80 */
		{{7, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0x81 */
		{{0, 0},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x82 */
		{{1, 1},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0x83 */
		{{0, 1},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x84 */
		{{2, 2},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x85 */
		{{0, 0},
		{2, 2},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x86 */
		{{1, 2},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0x87 */
		{{0, 2},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x88 */
		{{3, 3},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x89 */
		{{0, 0},
		{3, 3},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0x8a */
		{{1, 1},
		{3, 3},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x8b */
		{{0, 1},
		{3, 3},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x8c */
		{{2, 3},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x8d */
		{{0, 0},
		{2, 3},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x8e */
		{{1, 3},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0x8f */
		{{0, 3},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x90 */
		{{4, 4},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x91 */
		{{0, 0},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0x92 */
		{{1, 1},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x93 */
		{{0, 1},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0x94 */
		{{2, 2},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0x95 */
		{{0, 0},
		{2, 2},
		{4, 4},
		{7, 7}
		}
	},
	{0, 1, 3, 0,		/* 0x96 */
		{{1, 2},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x97 */
		{{0, 2},
		{4, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x98 */
		{{3, 4},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x99 */
		{{0, 0},
		{3, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0x9a */
		{{1, 1},
		{3, 4},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x9b */
		{{0, 1},
		{3, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x9c */
		{{2, 4},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0x9d */
		{{0, 0},
		{2, 4},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0x9e */
		{{1, 4},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0x9f */
		{{0, 4},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xa0 */
		{{5, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xa1 */
		{{0, 0},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xa2 */
		{{1, 1},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xa3 */
		{{0, 1},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xa4 */
		{{2, 2},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0xa5 */
		{{0, 0},
		{2, 2},
		{5, 5},
		{7, 7}
		}
	},
	{0, 1, 3, 0,		/* 0xa6 */
		{{1, 2},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xa7 */
		{{0, 2},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xa8 */
		{{3, 3},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0xa9 */
		{{0, 0},
		{3, 3},
		{5, 5},
		{7, 7}
		}
	},
	{0, 1, 4, 0,		/* 0xaa */
		{{1, 1},
		{3, 3},
		{5, 5},
		{7, 7}
		}
	},
	{1, 1, 4, 0,		/* 0xab */
		{{0, 1},
		{3, 3},
		{5, 5},
		{7, 7}
		}
	},
	{0, 1, 3, 0,		/* 0xac */
		{{2, 3},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0xad */
		{{0, 0},
		{2, 3},
		{5, 5},
		{7, 7}
		}
	},
	{0, 1, 3, 0,		/* 0xae */
		{{1, 3},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xaf */
		{{0, 3},
		{5, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xb0 */
		{{4, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xb1 */
		{{0, 0},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xb2 */
		{{1, 1},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xb3 */
		{{0, 1},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xb4 */
		{{2, 2},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0xb5 */
		{{0, 0},
		{2, 2},
		{4, 5},
		{7, 7}
		}
	},
	{0, 1, 3, 0,		/* 0xb6 */
		{{1, 2},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xb7 */
		{{0, 2},
		{4, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xb8 */
		{{3, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xb9 */
		{{0, 0},
		{3, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xba */
		{{1, 1},
		{3, 5},
		{7, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xbb */
		{{0, 1},
		{3, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xbc */
		{{2, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xbd */
		{{0, 0},
		{2, 5},
		{7, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xbe */
		{{1, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xbf */
		{{0, 5},
		{7, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xc0 */
		{{6, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xc1 */
		{{0, 0},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xc2 */
		{{1, 1},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xc3 */
		{{0, 1},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xc4 */
		{{2, 2},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xc5 */
		{{0, 0},
		{2, 2},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xc6 */
		{{1, 2},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xc7 */
		{{0, 2},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xc8 */
		{{3, 3},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xc9 */
		{{0, 0},
		{3, 3},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xca */
		{{1, 1},
		{3, 3},
		{6, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xcb */
		{{0, 1},
		{3, 3},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xcc */
		{{2, 3},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xcd */
		{{0, 0},
		{2, 3},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xce */
		{{1, 3},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xcf */
		{{0, 3},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xd0 */
		{{4, 4},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xd1 */
		{{0, 0},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xd2 */
		{{1, 1},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xd3 */
		{{0, 1},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xd4 */
		{{2, 2},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{1, 1, 4, 0,		/* 0xd5 */
		{{0, 0},
		{2, 2},
		{4, 4},
		{6, 7}
		}
	},
	{0, 1, 3, 0,		/* 0xd6 */
		{{1, 2},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xd7 */
		{{0, 2},
		{4, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xd8 */
		{{3, 4},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xd9 */
		{{0, 0},
		{3, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xda */
		{{1, 1},
		{3, 4},
		{6, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xdb */
		{{0, 1},
		{3, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xdc */
		{{2, 4},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xdd */
		{{0, 0},
		{2, 4},
		{6, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xde */
		{{1, 4},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xdf */
		{{0, 4},
		{6, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xe0 */
		{{5, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xe1 */
		{{0, 0},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xe2 */
		{{1, 1},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xe3 */
		{{0, 1},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xe4 */
		{{2, 2},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xe5 */
		{{0, 0},
		{2, 2},
		{5, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xe6 */
		{{1, 2},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xe7 */
		{{0, 2},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xe8 */
		{{3, 3},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xe9 */
		{{0, 0},
		{3, 3},
		{5, 7},
		{0, 0}
		}
	},
	{0, 1, 3, 0,		/* 0xea */
		{{1, 1},
		{3, 3},
		{5, 7},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xeb */
		{{0, 1},
		{3, 3},
		{5, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xec */
		{{2, 3},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xed */
		{{0, 0},
		{2, 3},
		{5, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xee */
		{{1, 3},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xef */
		{{0, 3},
		{5, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xf0 */
		{{4, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xf1 */
		{{0, 0},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xf2 */
		{{1, 1},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xf3 */
		{{0, 1},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xf4 */
		{{2, 2},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 3, 0,		/* 0xf5 */
		{{0, 0},
		{2, 2},
		{4, 7},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xf6 */
		{{1, 2},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xf7 */
		{{0, 2},
		{4, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xf8 */
		{{3, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xf9 */
		{{0, 0},
		{3, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 2, 0,		/* 0xfa */
		{{1, 1},
		{3, 7},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xfb */
		{{0, 1},
		{3, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xfc */
		{{2, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 2, 0,		/* 0xfd */
		{{0, 0},
		{2, 7},
		{0, 0},
		{0, 0}
		}
	},
	{0, 1, 1, 0,		/* 0xfe */
		{{1, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	},
	{1, 1, 1, 0,		/* 0xff */
		{{0, 7},
		{0, 0},
		{0, 0},
		{0, 0}
		}
	}
};




extern int sctp_peer_chunk_oh;

static int
sctp_find_cmsg(int c_type, void *data, struct mbuf *control, int cpsize)
{
	struct cmsghdr cmh;
	int tlen, at;

	tlen = control->m_len;
	at = 0;
	/*
	 * Independent of how many mbufs, find the c_type inside the control
	 * structure and copy out the data.
	 */
	while (at < tlen) {
		if ((tlen - at) < (int)CMSG_ALIGN(sizeof(cmh))) {
			/* not enough room for one more we are done. */
			return (0);
		}
		m_copydata(control, at, sizeof(cmh), (caddr_t)&cmh);
		if ((cmh.cmsg_len + at) > tlen) {
			/*
			 * this is real messed up since there is not enough
			 * data here to cover the cmsg header. We are done.
			 */
			return (0);
		}
		if ((cmh.cmsg_level == IPPROTO_SCTP) &&
		    (c_type == cmh.cmsg_type)) {
			/* found the one we want, copy it out */
			at += CMSG_ALIGN(sizeof(struct cmsghdr));
			if ((int)(cmh.cmsg_len - CMSG_ALIGN(sizeof(struct cmsghdr))) < cpsize) {
				/*
				 * space of cmsg_len after header not big
				 * enough
				 */
				return (0);
			}
			m_copydata(control, at, cpsize, data);
			return (1);
		} else {
			at += CMSG_ALIGN(cmh.cmsg_len);
			if (cmh.cmsg_len == 0) {
				break;
			}
		}
	}
	/* not found */
	return (0);
}

static struct mbuf *
sctp_add_addr_to_mbuf(struct mbuf *m, struct ifaddr *ifa)
{
	struct sctp_paramhdr *parmh;
	struct mbuf *mret;
	int len;

	if (ifa->ifa_addr->sa_family == AF_INET) {
		len = sizeof(struct sctp_ipv4addr_param);
	} else if (ifa->ifa_addr->sa_family == AF_INET6) {
		len = sizeof(struct sctp_ipv6addr_param);
	} else {
		/* unknown type */
		return (m);
	}

	if (M_TRAILINGSPACE(m) >= len) {
		/* easy side we just drop it on the end */
		parmh = (struct sctp_paramhdr *)(m->m_data + m->m_len);
		mret = m;
	} else {
		/* Need more space */
		mret = m;
		while (mret->m_next != NULL) {
			mret = mret->m_next;
		}
		mret->m_next = sctp_get_mbuf_for_msg(len, 0, M_DONTWAIT, 1, MT_DATA);
		if (mret->m_next == NULL) {
			/* We are hosed, can't add more addresses */
			return (m);
		}
		mret = mret->m_next;
		parmh = mtod(mret, struct sctp_paramhdr *);
	}
	/* now add the parameter */
	if (ifa->ifa_addr->sa_family == AF_INET) {
		struct sctp_ipv4addr_param *ipv4p;
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)ifa->ifa_addr;
		ipv4p = (struct sctp_ipv4addr_param *)parmh;
		parmh->param_type = htons(SCTP_IPV4_ADDRESS);
		parmh->param_length = htons(len);
		ipv4p->addr = sin->sin_addr.s_addr;
		mret->m_len += len;
	} else if (ifa->ifa_addr->sa_family == AF_INET6) {
		struct sctp_ipv6addr_param *ipv6p;
		struct sockaddr_in6 *sin6;

		sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
		ipv6p = (struct sctp_ipv6addr_param *)parmh;
		parmh->param_type = htons(SCTP_IPV6_ADDRESS);
		parmh->param_length = htons(len);
		memcpy(ipv6p->addr, &sin6->sin6_addr,
		    sizeof(ipv6p->addr));
		/* clear embedded scope in the address */
		in6_clearscope((struct in6_addr *)ipv6p->addr);
		mret->m_len += len;
	} else {
		return (m);
	}
	return (mret);
}



static struct mbuf *
sctp_add_cookie(struct sctp_inpcb *inp, struct mbuf *init, int init_offset,
    struct mbuf *initack, int initack_offset, struct sctp_state_cookie *stc_in)
{
	struct mbuf *copy_init, *copy_initack, *m_at, *sig, *mret;
	struct sctp_state_cookie *stc;
	struct sctp_paramhdr *ph;
	uint8_t *signature;
	int sig_offset;
	uint16_t cookie_sz;

	mret = NULL;


	mret = sctp_get_mbuf_for_msg((sizeof(struct sctp_state_cookie) +
				      sizeof(struct sctp_paramhdr)), 0, M_DONTWAIT, 1, MT_DATA);
	if (mret == NULL) {
		return (NULL);
	}
	copy_init = sctp_m_copym(init, init_offset, M_COPYALL, M_DONTWAIT);
	if (copy_init == NULL) {
		sctp_m_freem(mret);
		return (NULL);
	}
	copy_initack = sctp_m_copym(initack, initack_offset, M_COPYALL,
	    M_DONTWAIT);
	if (copy_initack == NULL) {
		sctp_m_freem(mret);
		sctp_m_freem(copy_init);
		return (NULL);
	}
	/* easy side we just drop it on the end */
	ph = mtod(mret, struct sctp_paramhdr *);
	mret->m_len = sizeof(struct sctp_state_cookie) +
	    sizeof(struct sctp_paramhdr);
	stc = (struct sctp_state_cookie *)((caddr_t)ph +
	    sizeof(struct sctp_paramhdr));
	ph->param_type = htons(SCTP_STATE_COOKIE);
	ph->param_length = 0;	/* fill in at the end */
	/* Fill in the stc cookie data */
	*stc = *stc_in;

	/* tack the INIT and then the INIT-ACK onto the chain */
	cookie_sz = 0;
	m_at = mret;
	for (m_at = mret; m_at; m_at = m_at->m_next) {
		cookie_sz += m_at->m_len;
		if (m_at->m_next == NULL) {
			m_at->m_next = copy_init;
			break;
		}
	}

	for (m_at = copy_init; m_at; m_at = m_at->m_next) {
		cookie_sz += m_at->m_len;
		if (m_at->m_next == NULL) {
			m_at->m_next = copy_initack;
			break;
		}
	}

	for (m_at = copy_initack; m_at; m_at = m_at->m_next) {
		cookie_sz += m_at->m_len;
		if (m_at->m_next == NULL) {
			break;
		}
	}
	sig = sctp_get_mbuf_for_msg(SCTP_SECRET_SIZE, 0, M_DONTWAIT, 1, MT_DATA);
	if (sig == NULL) {
		/* no space */
		sctp_m_freem(mret);
		sctp_m_freem(copy_init);
		sctp_m_freem(copy_initack);
		return (NULL);
	}
	sig->m_len = 0;
	m_at->m_next = sig;
	sig_offset = 0;
	signature = (uint8_t *) (mtod(sig, caddr_t)+sig_offset);
	/* Time to sign the cookie */
	sctp_hash_digest_m((char *)inp->sctp_ep.secret_key[
	    (int)(inp->sctp_ep.current_secret_number)],
	    SCTP_SECRET_SIZE, mret, sizeof(struct sctp_paramhdr),
	    (uint8_t *) signature);
	sig->m_len += SCTP_SIGNATURE_SIZE;
	cookie_sz += SCTP_SIGNATURE_SIZE;

	ph->param_length = htons(cookie_sz);
	return (mret);
}


static struct sockaddr_in *
sctp_is_v4_ifa_addr_prefered(struct ifaddr *ifa, uint8_t loopscope, uint8_t ipv4_scope, uint8_t * sin_loop, uint8_t * sin_local)
{
	struct sockaddr_in *sin;

	/*
	 * Here we determine if its a prefered address. A prefered address
	 * means it is the same scope or higher scope then the destination.
	 * L = loopback, P = private, G = global
	 * ----------------------------------------- src    |      dest
	 * | result ----------------------------------------- L     |
	 * L |    yes ----------------------------------------- P     |
	 * L |    yes ----------------------------------------- G     |
	 * L |    yes ----------------------------------------- L     |
	 * P |    no ----------------------------------------- P     |
	 * P |    yes ----------------------------------------- G     |
	 * P |    no ----------------------------------------- L     |
	 * G |    no ----------------------------------------- P     |
	 * G |    no ----------------------------------------- G     |
	 * G |    yes -----------------------------------------
	 */

	if (ifa->ifa_addr->sa_family != AF_INET) {
		/* forget non-v4 */
		return (NULL);
	}
	/* Ok the address may be ok */
	sin = (struct sockaddr_in *)ifa->ifa_addr;
	if (sin->sin_addr.s_addr == 0) {
		return (NULL);
	}
	*sin_local = *sin_loop = 0;
	if ((ifa->ifa_ifp->if_type == IFT_LOOP) ||
	    (IN4_ISLOOPBACK_ADDRESS(&sin->sin_addr))) {
		*sin_loop = 1;
		*sin_local = 1;
	}
	if ((IN4_ISPRIVATE_ADDRESS(&sin->sin_addr))) {
		*sin_local = 1;
	}
	if (!loopscope && *sin_loop) {
		/* Its a loopback address and we don't have loop scope */
		return (NULL);
	}
	if (!ipv4_scope && *sin_local) {
		/*
		 * Its a private address, and we don't have private address
		 * scope
		 */
		return (NULL);
	}
	if (((ipv4_scope == 0) && (loopscope == 0)) && (*sin_local)) {
		/* its a global src and a private dest */
		return (NULL);
	}
	/* its a prefered address */
	return (sin);
}

static struct sockaddr_in *
sctp_is_v4_ifa_addr_acceptable(struct ifaddr *ifa, uint8_t loopscope, uint8_t ipv4_scope, uint8_t * sin_loop, uint8_t * sin_local)
{
	struct sockaddr_in *sin;

	/*
	 * Here we determine if its a acceptable address. A acceptable
	 * address means it is the same scope or higher scope but we can
	 * allow for NAT which means its ok to have a global dest and a
	 * private src.
	 * 
	 * L = loopback, P = private, G = global
	 * ----------------------------------------- src    |      dest
	 * | result ----------------------------------------- L     |
	 * L |    yes ----------------------------------------- P     |
	 * L |    yes ----------------------------------------- G     |
	 * L |    yes ----------------------------------------- L     |
	 * P |    no ----------------------------------------- P     |
	 * P |    yes ----------------------------------------- G     |
	 * P |    yes - probably this won't work.
	 * ----------------------------------------- L     |       G       |
	 * no ----------------------------------------- P     |       G |
	 * yes ----------------------------------------- G     |       G |
	 * yes -----------------------------------------
	 */

	if (ifa->ifa_addr->sa_family != AF_INET) {
		/* forget non-v4 */
		return (NULL);
	}
	/* Ok the address may be ok */
	sin = (struct sockaddr_in *)ifa->ifa_addr;
	if (sin->sin_addr.s_addr == 0) {
		return (NULL);
	}
	*sin_local = *sin_loop = 0;
	if ((ifa->ifa_ifp->if_type == IFT_LOOP) ||
	    (IN4_ISLOOPBACK_ADDRESS(&sin->sin_addr))) {
		*sin_loop = 1;
		*sin_local = 1;
	}
	if ((IN4_ISPRIVATE_ADDRESS(&sin->sin_addr))) {
		*sin_local = 1;
	}
	if (!loopscope && *sin_loop) {
		/* Its a loopback address and we don't have loop scope */
		return (NULL);
	}
	/* its an acceptable address */
	return (sin);
}

/*
 * This treats the address list on the ep as a restricted list (negative
 * list). If a the passed address is listed, then the address is NOT allowed
 * on the association.
 */
int
sctp_is_addr_restricted(struct sctp_tcb *stcb, struct sockaddr *addr)
{
	struct sctp_laddr *laddr;

#ifdef SCTP_DEBUG
	int cnt = 0;

#endif
	if (stcb == NULL) {
		/* There are no restrictions, no TCB :-) */
		return (0);
	}
#ifdef SCTP_DEBUG
	LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list, sctp_nxt_addr) {
		cnt++;
	}
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("There are %d addresses on the restricted list\n", cnt);
	}
	cnt = 0;
#endif
	LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list, sctp_nxt_addr) {
		if (laddr->ifa == NULL) {
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Help I have fallen and I can't get up!\n");
			}
#endif
			continue;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
			cnt++;
			printf("Restricted address[%d]:", cnt);
			sctp_print_address(laddr->ifa->ifa_addr);
		}
#endif
		if (sctp_cmpaddr(addr, laddr->ifa->ifa_addr) == 1) {
			/* Yes it is on the list */
			return (1);
		}
	}
	return (0);
}

static int
sctp_is_addr_in_ep(struct sctp_inpcb *inp, struct ifaddr *ifa)
{
	struct sctp_laddr *laddr;

	if (ifa == NULL)
		return (0);
	LIST_FOREACH(laddr, &inp->sctp_addr_list, sctp_nxt_addr) {
		if (laddr->ifa == NULL) {
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Help I have fallen and I can't get up!\n");
			}
#endif
			continue;
		}
		if (laddr->ifa->ifa_addr == NULL)
			continue;
		if (laddr->ifa == ifa)
			/* same pointer */
			return (1);
		if (laddr->ifa->ifa_addr->sa_family != ifa->ifa_addr->sa_family) {
			/* skip non compatible address comparison */
			continue;
		}
		if (sctp_cmpaddr(ifa->ifa_addr, laddr->ifa->ifa_addr) == 1) {
			/* Yes it is restricted */
			return (1);
		}
	}
	return (0);
}



static struct in_addr
sctp_choose_v4_boundspecific_inp(struct sctp_inpcb *inp,
    struct route *ro,
    uint8_t ipv4_scope,
    uint8_t loopscope)
{
	struct in_addr ans;
	struct sctp_laddr *laddr;
	struct sockaddr_in *sin;
	struct ifnet *ifn;
	struct ifaddr *ifa;
	uint8_t sin_loop, sin_local;
	struct rtentry *rt;

	/*
	 * first question, is the ifn we will emit on in our list, if so, we
	 * want that one.
	 */
	rt = ro->ro_rt;
	ifn = rt->rt_ifp;
	if (ifn) {
		/* is a prefered one on the interface we route out? */
		TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
			sin = sctp_is_v4_ifa_addr_prefered(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			if (sctp_is_addr_in_ep(inp, ifa)) {
				return (sin->sin_addr);
			}
		}
		/* is an acceptable one on the interface we route out? */
		TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
			sin = sctp_is_v4_ifa_addr_acceptable(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			if (sctp_is_addr_in_ep(inp, ifa)) {
				return (sin->sin_addr);
			}
		}
	}
	/* ok, what about a prefered address in the inp */
	for (laddr = LIST_FIRST(&inp->sctp_addr_list);
	    laddr && (laddr != inp->next_addr_touse);
	    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
		if (laddr->ifa == NULL) {
			/* address has been removed */
			continue;
		}
		sin = sctp_is_v4_ifa_addr_prefered(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
		if (sin == NULL)
			continue;
		return (sin->sin_addr);

	}
	/* ok, what about an acceptable address in the inp */
	for (laddr = LIST_FIRST(&inp->sctp_addr_list);
	    laddr && (laddr != inp->next_addr_touse);
	    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
		if (laddr->ifa == NULL) {
			/* address has been removed */
			continue;
		}
		sin = sctp_is_v4_ifa_addr_acceptable(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
		if (sin == NULL)
			continue;
		return (sin->sin_addr);

	}

	/*
	 * no address bound can be a source for the destination we are in
	 * trouble
	 */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Src address selection for EP, no acceptable src address found for address\n");
	}
#endif
	RTFREE(ro->ro_rt);
	ro->ro_rt = NULL;
	memset(&ans, 0, sizeof(ans));
	return (ans);
}



static struct in_addr
sctp_choose_v4_boundspecific_stcb(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_nets *net,
    struct route *ro,
    uint8_t ipv4_scope,
    uint8_t loopscope,
    int non_asoc_addr_ok)
{
	/*
	 * Here we have two cases, bound all asconf allowed. bound all
	 * asconf not allowed.
	 * 
	 */
	struct sctp_laddr *laddr, *starting_point;
	struct in_addr ans;
	struct ifnet *ifn;
	struct ifaddr *ifa;
	uint8_t sin_loop, sin_local, start_at_beginning = 0;
	struct sockaddr_in *sin;
	struct rtentry *rt;

	/*
	 * first question, is the ifn we will emit on in our list, if so, we
	 * want that one.
	 */
	rt = ro->ro_rt;
	ifn = rt->rt_ifp;

	if (sctp_is_feature_on(inp, SCTP_PCB_FLAGS_DO_ASCONF)) {
		/*
		 * Here we use the list of addresses on the endpoint. Then
		 * the addresses listed on the "restricted" list is just
		 * that, address that have not been added and can't be used
		 * (unless the non_asoc_addr_ok is set).
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Have a STCB - asconf allowed, not bound all have a netgative list\n");
		}
#endif
		/*
		 * first question, is the ifn we will emit on in our list,
		 * if so, we want that one.
		 */
		if (ifn) {
			/* first try for an prefered address on the ep */
			TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
				if (sctp_is_addr_in_ep(inp, ifa)) {
					sin = sctp_is_v4_ifa_addr_prefered(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
					if (sin == NULL)
						continue;
					if ((non_asoc_addr_ok == 0) &&
					    (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin))) {
						/* on the no-no list */
						continue;
					}
					return (sin->sin_addr);
				}
			}
			/* next try for an acceptable address on the ep */
			TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
				if (sctp_is_addr_in_ep(inp, ifa)) {
					sin = sctp_is_v4_ifa_addr_acceptable(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
					if (sin == NULL)
						continue;
					if ((non_asoc_addr_ok == 0) &&
					    (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin))) {
						/* on the no-no list */
						continue;
					}
					return (sin->sin_addr);
				}
			}

		}
		/*
		 * if we can't find one like that then we must look at all
		 * addresses bound to pick one at first prefereable then
		 * secondly acceptable.
		 */
		starting_point = stcb->asoc.last_used_address;
sctpv4_from_the_top:
		if (stcb->asoc.last_used_address == NULL) {
			start_at_beginning = 1;
			stcb->asoc.last_used_address = LIST_FIRST(&inp->sctp_addr_list);
		}
		/* search beginning with the last used address */
		for (laddr = stcb->asoc.last_used_address; laddr;
		    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_prefered(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			if ((non_asoc_addr_ok == 0) &&
			    (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin))) {
				/* on the no-no list */
				continue;
			}
			return (sin->sin_addr);

		}
		if (start_at_beginning == 0) {
			stcb->asoc.last_used_address = NULL;
			goto sctpv4_from_the_top;
		}
		/* now try for any higher scope than the destination */
		stcb->asoc.last_used_address = starting_point;
		start_at_beginning = 0;
sctpv4_from_the_top2:
		if (stcb->asoc.last_used_address == NULL) {
			start_at_beginning = 1;
			stcb->asoc.last_used_address = LIST_FIRST(&inp->sctp_addr_list);
		}
		/* search beginning with the last used address */
		for (laddr = stcb->asoc.last_used_address; laddr;
		    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_acceptable(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			if ((non_asoc_addr_ok == 0) &&
			    (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin))) {
				/* on the no-no list */
				continue;
			}
			return (sin->sin_addr);
		}
		if (start_at_beginning == 0) {
			stcb->asoc.last_used_address = NULL;
			goto sctpv4_from_the_top2;
		}
	} else {
		/*
		 * Here we have an address list on the association, thats
		 * the only valid source addresses that we can use.
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Have a STCB - no asconf allowed, not bound all have a postive list\n");
		}
#endif
		/*
		 * First look at all addresses for one that is on the
		 * interface we route out
		 */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_prefered(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			/*
			 * first question, is laddr->ifa an address
			 * associated with the emit interface
			 */
			if (ifn) {
				TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
					if (laddr->ifa == ifa) {
						sin = (struct sockaddr_in *)laddr->ifa->ifa_addr;
						return (sin->sin_addr);
					}
					if (sctp_cmpaddr(ifa->ifa_addr, laddr->ifa->ifa_addr) == 1) {
						sin = (struct sockaddr_in *)laddr->ifa->ifa_addr;
						return (sin->sin_addr);
					}
				}
			}
		}
		/* what about an acceptable one on the interface? */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_acceptable(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			/*
			 * first question, is laddr->ifa an address
			 * associated with the emit interface
			 */
			if (ifn) {
				TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
					if (laddr->ifa == ifa) {
						sin = (struct sockaddr_in *)laddr->ifa->ifa_addr;
						return (sin->sin_addr);
					}
					if (sctp_cmpaddr(ifa->ifa_addr, laddr->ifa->ifa_addr) == 1) {
						sin = (struct sockaddr_in *)laddr->ifa->ifa_addr;
						return (sin->sin_addr);
					}
				}
			}
		}
		/* ok, next one that is preferable in general */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_prefered(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			return (sin->sin_addr);
		}

		/* last, what about one that is acceptable */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin = sctp_is_v4_ifa_addr_acceptable(laddr->ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			return (sin->sin_addr);
		}
	}
	RTFREE(ro->ro_rt);
	ro->ro_rt = NULL;
	memset(&ans, 0, sizeof(ans));
	return (ans);
}

static struct sockaddr_in *
sctp_select_v4_nth_prefered_addr_from_ifn_boundall(struct ifnet *ifn, struct sctp_tcb *stcb, int non_asoc_addr_ok,
    uint8_t loopscope, uint8_t ipv4_scope, int cur_addr_num)
{
	struct ifaddr *ifa;
	struct sockaddr_in *sin;
	uint8_t sin_loop, sin_local;
	int num_eligible_addr = 0;

	TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
		sin = sctp_is_v4_ifa_addr_prefered(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
		if (sin == NULL)
			continue;
		if (stcb) {
			if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin)) {
				/*
				 * It is restricted for some reason..
				 * probably not yet added.
				 */
				continue;
			}
		}
		if (cur_addr_num == num_eligible_addr) {
			return (sin);
		}
	}
	return (NULL);
}


static int
sctp_count_v4_num_prefered_boundall(struct ifnet *ifn, struct sctp_tcb *stcb, int non_asoc_addr_ok,
    uint8_t loopscope, uint8_t ipv4_scope, uint8_t * sin_loop, uint8_t * sin_local)
{
	struct ifaddr *ifa;
	struct sockaddr_in *sin;
	int num_eligible_addr = 0;

	TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
		sin = sctp_is_v4_ifa_addr_prefered(ifa, loopscope, ipv4_scope, sin_loop, sin_local);
		if (sin == NULL)
			continue;
		if (stcb) {
			if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin)) {
				/*
				 * It is restricted for some reason..
				 * probably not yet added.
				 */
				continue;
			}
		}
		num_eligible_addr++;
	}
	return (num_eligible_addr);

}

static struct in_addr
sctp_choose_v4_boundall(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_nets *net,
    struct route *ro,
    uint8_t ipv4_scope,
    uint8_t loopscope,
    int non_asoc_addr_ok)
{
	int cur_addr_num = 0, num_prefered = 0;
	uint8_t sin_loop, sin_local;
	struct ifnet *ifn;
	struct sockaddr_in *sin;
	struct in_addr ans;
	struct ifaddr *ifa;
	struct rtentry *rt;

	/*
	 * For v4 we can use (in boundall) any address in the association.
	 * If non_asoc_addr_ok is set we can use any address (at least in
	 * theory). So we look for prefered addresses first. If we find one,
	 * we use it. Otherwise we next try to get an address on the
	 * interface, which we should be able to do (unless non_asoc_addr_ok
	 * is false and we are routed out that way). In these cases where we
	 * can't use the address of the interface we go through all the
	 * ifn's looking for an address we can use and fill that in. Punting
	 * means we send back address 0, which will probably cause problems
	 * actually since then IP will fill in the address of the route ifn,
	 * which means we probably already rejected it.. i.e. here comes an
	 * abort :-<.
	 */
	rt = ro->ro_rt;
	ifn = rt->rt_ifp;
	if (net) {
		cur_addr_num = net->indx_of_eligible_next_to_use;
	}
	if (ifn == NULL) {
		goto bound_all_v4_plan_c;
	}
	num_prefered = sctp_count_v4_num_prefered_boundall(ifn, stcb, non_asoc_addr_ok, loopscope, ipv4_scope, &sin_loop, &sin_local);
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Found %d prefered source addresses\n", num_prefered);
	}
#endif
	if (num_prefered == 0) {
		/*
		 * no eligible addresses, we must use some other interface
		 * address if we can find one.
		 */
		goto bound_all_v4_plan_b;
	}
	/*
	 * Ok we have num_eligible_addr set with how many we can use, this
	 * may vary from call to call due to addresses being deprecated
	 * etc..
	 */
	if (cur_addr_num >= num_prefered) {
		cur_addr_num = 0;
	}
	/*
	 * select the nth address from the list (where cur_addr_num is the
	 * nth) and 0 is the first one, 1 is the second one etc...
	 */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("cur_addr_num:%d\n", cur_addr_num);
	}
#endif
	sin = sctp_select_v4_nth_prefered_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope,
	    ipv4_scope, cur_addr_num);

	/* if sin is NULL something changed??, plan_a now */
	if (sin) {
		return (sin->sin_addr);
	}
	/*
	 * plan_b: Look at the interface that we emit on and see if we can
	 * find an acceptable address.
	 */
bound_all_v4_plan_b:
	TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
		sin = sctp_is_v4_ifa_addr_acceptable(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
		if (sin == NULL)
			continue;
		if (stcb) {
			if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin)) {
				/*
				 * It is restricted for some reason..
				 * probably not yet added.
				 */
				continue;
			}
		}
		return (sin->sin_addr);
	}
	/*
	 * plan_c: Look at all interfaces and find a prefered address. If we
	 * reache here we are in trouble I think.
	 */
bound_all_v4_plan_c:
	for (ifn = TAILQ_FIRST(&ifnet);
	    ifn && (ifn != inp->next_ifn_touse);
	    ifn = TAILQ_NEXT(ifn, if_list)) {
		if (loopscope == 0 && ifn->if_type == IFT_LOOP) {
			/* wrong base scope */
			continue;
		}
		if (ifn == rt->rt_ifp)
			/* already looked at this guy */
			continue;
		num_prefered = sctp_count_v4_num_prefered_boundall(ifn, stcb, non_asoc_addr_ok,
		    loopscope, ipv4_scope, &sin_loop, &sin_local);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Found ifn:%x %d prefered source addresses\n", (uint32_t) ifn, num_prefered);
		}
#endif
		if (num_prefered == 0) {
			/*
			 * None on this interface.
			 */
			continue;
		}
		/*
		 * Ok we have num_eligible_addr set with how many we can
		 * use, this may vary from call to call due to addresses
		 * being deprecated etc..
		 */
		if (cur_addr_num >= num_prefered) {
			cur_addr_num = 0;
		}
		sin = sctp_select_v4_nth_prefered_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope,
		    ipv4_scope, cur_addr_num);
		if (sin == NULL)
			continue;
		return (sin->sin_addr);

	}

	/*
	 * plan_d: We are in deep trouble. No prefered address on any
	 * interface. And the emit interface does not even have an
	 * acceptable address. Take anything we can get! If this does not
	 * work we are probably going to emit a packet that will illicit an
	 * ABORT, falling through.
	 */

	for (ifn = TAILQ_FIRST(&ifnet);
	    ifn && (ifn != inp->next_ifn_touse);
	    ifn = TAILQ_NEXT(ifn, if_list)) {
		if (loopscope == 0 && ifn->if_type == IFT_LOOP) {
			/* wrong base scope */
			continue;
		}
		if (ifn == rt->rt_ifp)
			/* already looked at this guy */
			continue;

		TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
			sin = sctp_is_v4_ifa_addr_acceptable(ifa, loopscope, ipv4_scope, &sin_loop, &sin_local);
			if (sin == NULL)
				continue;
			if (stcb) {
				if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin)) {
					/*
					 * It is restricted for some
					 * reason.. probably not yet added.
					 */
					continue;
				}
			}
			return (sin->sin_addr);
		}
	}
	/*
	 * Ok we can find NO address to source from that is not on our
	 * negative list. It is either the special ASCONF case where we are
	 * sourceing from a intf that has been ifconfig'd to a different
	 * address (i.e. it holds a ADD/DEL/SET-PRIM and the proper lookup
	 * address. OR we are hosed, and this baby is going to abort the
	 * association.
	 */
	if (non_asoc_addr_ok) {
		return (((struct sockaddr_in *)(rt->rt_ifa->ifa_addr))->sin_addr);
	} else {
		RTFREE(ro->ro_rt);
		ro->ro_rt = NULL;
		memset(&ans, 0, sizeof(ans));
		return (ans);
	}
}



/* tcb may be NULL */
struct in_addr
sctp_ipv4_source_address_selection(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb, struct route *ro, struct sctp_nets *net,
    int non_asoc_addr_ok)
{
	struct in_addr ans;
	struct sockaddr_in *to = (struct sockaddr_in *)&ro->ro_dst;
	uint8_t ipv4_scope, loopscope;

	/*
	 * Rules: - Find the route if needed, cache if I can. - Look at
	 * interface address in route, Is it in the bound list. If so we
	 * have the best source. - If not we must rotate amongst the
	 * addresses.
	 * 
	 * Cavets and issues
	 * 
	 * Do we need to pay attention to scope. We can have a private address
	 * or a global address we are sourcing or sending to. So if we draw
	 * it out source     *      dest   *  result
	 * ------------------------------------------ a   Private    *
	 * Global  *  NAT? ------------------------------------------ b
	 * Private    *     Private *  No problem
	 * ------------------------------------------ c   Global     *
	 * Private *  Huh, How will this work?
	 * ------------------------------------------ d   Global     *
	 * Global  *  No Problem ------------------------------------------
	 * 
	 * And then we add to that what happens if there are multiple addresses
	 * assigned to an interface. Remember the ifa on a ifn is a linked
	 * list of addresses. So one interface can have more than one IPv4
	 * address. What happens if we have both a private and a global
	 * address? Do we then use context of destination to sort out which
	 * one is best? And what about NAT's sending P->G may get you a NAT
	 * translation, or should you select the G thats on the interface in
	 * preference.
	 * 
	 * Decisions:
	 * 
	 * - count the number of addresses on the interface. - if its one, no
	 * problem except case <c>. For <a> we will assume a NAT out there.
	 * - if there are more than one, then we need to worry about scope P
	 * or G. We should prefer G -> G and P -> P if possible. Then as a
	 * secondary fall back to mixed types G->P being a last ditch one. -
	 * The above all works for bound all, but bound specific we need to
	 * use the same concept but instead only consider the bound
	 * addresses. If the bound set is NOT assigned to the interface then
	 * we must use rotation amongst them.
	 * 
	 * Notes: For v4, we can always punt and let ip_output decide by
	 * sending back a source of 0.0.0.0
	 */

	if (ro->ro_rt == NULL) {
		/*
		 * Need a route to cache.
		 * 
		 */
#if defined(__FreeBSD__) || defined(__APPLE__)
		rtalloc_ign(ro, 0UL);
#else
		rtalloc(ro);
#endif
	}
	if (ro->ro_rt == NULL) {
		/* No route to host .. punt */
		memset(&ans, 0, sizeof(ans));
		return (ans);
	}
	/* Setup our scopes */
	if (stcb) {
		ipv4_scope = stcb->asoc.ipv4_local_scope;
		loopscope = stcb->asoc.loopback_scope;
	} else {
		/* Scope based on outbound address */
		if ((IN4_ISPRIVATE_ADDRESS(&to->sin_addr))) {
			ipv4_scope = 1;
			loopscope = 0;
		} else if (IN4_ISLOOPBACK_ADDRESS(&to->sin_addr)) {
			ipv4_scope = 1;
			loopscope = 1;
		} else {
			ipv4_scope = 0;
			loopscope = 0;
		}
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Scope setup loop:%d ipv4_scope:%d\n",
		    loopscope, ipv4_scope);
	}
#endif
	if (inp->sctp_flags & SCTP_PCB_FLAGS_BOUNDALL) {
		/*
		 * When bound to all if the address list is set it is a
		 * negative list. Addresses being added by asconf.
		 */
		return (sctp_choose_v4_boundall(inp, stcb, net, ro,
		    ipv4_scope, loopscope, non_asoc_addr_ok));
	}
	/*
	 * Three possiblities here:
	 * 
	 * a) stcb is NULL, which means we operate only from the list of
	 * addresses (ifa's) bound to the assoc and we care not about the
	 * list. b) stcb is NOT-NULL, which means we have an assoc structure
	 * and auto-asconf is on. This means that the list of addresses is a
	 * NOT list. We use the list from the inp, but any listed address in
	 * our list is NOT yet added. However if the non_asoc_addr_ok is set
	 * we CAN use an address NOT available (i.e. being added). Its a
	 * negative list. c) stcb is NOT-NULL, which means we have an assoc
	 * structure and auto-asconf is off. This means that the list of
	 * addresses is the ONLY addresses I can use.. its positive.
	 * 
	 * Note we collapse b & c into the same function just like in the v6
	 * address selection.
	 */
	if (stcb) {
		return (sctp_choose_v4_boundspecific_stcb(inp, stcb, net,
		    ro, ipv4_scope, loopscope, non_asoc_addr_ok));
	} else {
		return (sctp_choose_v4_boundspecific_inp(inp, ro,
		    ipv4_scope, loopscope));
	}
	/* this should not be reached */
	memset(&ans, 0, sizeof(ans));
	return (ans);
}



static struct sockaddr_in6 *
sctp_is_v6_ifa_addr_acceptable(struct ifaddr *ifa, int loopscope, int loc_scope, int *sin_loop, int *sin_local)
{
	struct in6_ifaddr *ifa6;
	struct sockaddr_in6 *sin6;

#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
	struct timeval timenow;

	getmicrotime(&timenow);
#endif

	if (ifa->ifa_addr->sa_family != AF_INET6) {
		/* forget non-v6 */
		return (NULL);
	}
	ifa6 = (struct in6_ifaddr *)ifa;
	/* ok to use deprecated addresses? */
	if (!ip6_use_deprecated) {
		if (IFA6_IS_DEPRECATED(ifa6)) {
			/* can't use this type */
			return (NULL);
		}
	}
	/* are we ok, with the current state of this address? */
	if (ifa6->ia6_flags &
	    (IN6_IFF_DETACHED | IN6_IFF_NOTREADY | IN6_IFF_ANYCAST)) {
		/* Can't use these types */
		return (NULL);
	}
	/* Ok the address may be ok */
	sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
	*sin_local = *sin_loop = 0;
	if ((ifa->ifa_ifp->if_type == IFT_LOOP) ||
	    (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))) {
		*sin_loop = 1;
	}
	if (!loopscope && *sin_loop) {
		/* Its a loopback address and we don't have loop scope */
		return (NULL);
	}
	if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
		/* we skip unspecifed addresses */
		return (NULL);
	}
	if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
		*sin_local = 1;
	}
	if (!loc_scope && *sin_local) {
		/*
		 * Its a link local address, and we don't have link local
		 * scope
		 */
		return (NULL);
	}
	return (sin6);
}


static struct sockaddr_in6 *
sctp_choose_v6_boundspecific_stcb(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_nets *net,
    struct route *ro,
    uint8_t loc_scope,
    uint8_t loopscope,
    int non_asoc_addr_ok)
{
	/*
	 * Each endpoint has a list of local addresses associated with it.
	 * The address list is either a "negative list" i.e. those addresses
	 * that are NOT allowed to be used as a source OR a "postive list"
	 * i.e. those addresses that CAN be used.
	 * 
	 * Its a negative list if asconf is allowed. What we do in this case is
	 * use the ep address list BUT we have to cross check it against the
	 * negative list.
	 * 
	 * In the case where NO asconf is allowed, we have just a straight
	 * association level list that we must use to find a source address.
	 */
	struct sctp_laddr *laddr, *starting_point;
	struct sockaddr_in6 *sin6;
	int sin_loop, sin_local;
	int start_at_beginning = 0;
	struct ifnet *ifn;
	struct ifaddr *ifa;
	struct rtentry *rt;

	rt = ro->ro_rt;
	ifn = rt->rt_ifp;
	if (sctp_is_feature_on(inp, SCTP_PCB_FLAGS_DO_ASCONF)) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Have a STCB - asconf allowed, not bound all have a netgative list\n");
		}
#endif
		/*
		 * first question, is the ifn we will emit on in our list,
		 * if so, we want that one.
		 */
		if (ifn) {
			TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
				if (sctp_is_addr_in_ep(inp, ifa)) {
					sin6 = sctp_is_v6_ifa_addr_acceptable(ifa, loopscope, loc_scope, &sin_loop, &sin_local);
					if (sin6 == NULL)
						continue;
					if ((non_asoc_addr_ok == 0) &&
					    (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin6))) {
						/* on the no-no list */
						continue;
					}
					return (sin6);
				}
			}
		}
		starting_point = stcb->asoc.last_used_address;
		/* First try for matching scope */
sctp_from_the_top:
		if (stcb->asoc.last_used_address == NULL) {
			start_at_beginning = 1;
			stcb->asoc.last_used_address = LIST_FIRST(&inp->sctp_addr_list);
		}
		/* search beginning with the last used address */
		for (laddr = stcb->asoc.last_used_address; laddr;
		    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;
			if ((non_asoc_addr_ok == 0) && (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin6))) {
				/* on the no-no list */
				continue;
			}
			/* is it of matching scope ? */
			if ((loopscope == 0) &&
			    (loc_scope == 0) &&
			    (sin_loop == 0) &&
			    (sin_local == 0)) {
				/* all of global scope we are ok with it */
				return (sin6);
			}
			if (loopscope && sin_loop)
				/* both on the loopback, thats ok */
				return (sin6);
			if (loc_scope && sin_local)
				/* both local scope */
				return (sin6);

		}
		if (start_at_beginning == 0) {
			stcb->asoc.last_used_address = NULL;
			goto sctp_from_the_top;
		}
		/* now try for any higher scope than the destination */
		stcb->asoc.last_used_address = starting_point;
		start_at_beginning = 0;
sctp_from_the_top2:
		if (stcb->asoc.last_used_address == NULL) {
			start_at_beginning = 1;
			stcb->asoc.last_used_address = LIST_FIRST(&inp->sctp_addr_list);
		}
		/* search beginning with the last used address */
		for (laddr = stcb->asoc.last_used_address; laddr;
		    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;
			if ((non_asoc_addr_ok == 0) && (sctp_is_addr_restricted(stcb, (struct sockaddr *)sin6))) {
				/* on the no-no list */
				continue;
			}
			return (sin6);
		}
		if (start_at_beginning == 0) {
			stcb->asoc.last_used_address = NULL;
			goto sctp_from_the_top2;
		}
	} else {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Have a STCB - no asconf allowed, not bound all have a postive list\n");
		}
#endif
		/* First try for interface output match */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;
			/*
			 * first question, is laddr->ifa an address
			 * associated with the emit interface
			 */
			if (ifn) {
				TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
					if (laddr->ifa == ifa) {
						sin6 = (struct sockaddr_in6 *)laddr->ifa->ifa_addr;
						return (sin6);
					}
					if (sctp_cmpaddr(ifa->ifa_addr, laddr->ifa->ifa_addr) == 1) {
						sin6 = (struct sockaddr_in6 *)laddr->ifa->ifa_addr;
						return (sin6);
					}
				}
			}
		}
		/* Next try for matching scope */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;

			if ((loopscope == 0) &&
			    (loc_scope == 0) &&
			    (sin_loop == 0) &&
			    (sin_local == 0)) {
				/* all of global scope we are ok with it */
				return (sin6);
			}
			if (loopscope && sin_loop)
				/* both on the loopback, thats ok */
				return (sin6);
			if (loc_scope && sin_local)
				/* both local scope */
				return (sin6);
		}
		/* ok, now try for a higher scope in the source address */
		/* First try for matching scope */
		LIST_FOREACH(laddr, &stcb->asoc.sctp_local_addr_list,
		    sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				/* address has been removed */
				continue;
			}
			sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;
			return (sin6);
		}
	}
	RTFREE(ro->ro_rt);
	ro->ro_rt = NULL;
	return (NULL);
}

static struct sockaddr_in6 *
sctp_choose_v6_boundspecific_inp(struct sctp_inpcb *inp,
    struct route *ro,
    uint8_t loc_scope,
    uint8_t loopscope)
{
	/*
	 * Here we are bound specific and have only an inp. We must find an
	 * address that is bound that we can give out as a src address. We
	 * prefer two addresses of same scope if we can find them that way.
	 */
	struct sctp_laddr *laddr;
	struct sockaddr_in6 *sin6;
	struct ifnet *ifn;
	struct ifaddr *ifa;
	int sin_loop, sin_local;
	struct rtentry *rt;

	/*
	 * first question, is the ifn we will emit on in our list, if so, we
	 * want that one.
	 */

	rt = ro->ro_rt;
	ifn = rt->rt_ifp;
	if (ifn) {
		TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
			sin6 = sctp_is_v6_ifa_addr_acceptable(ifa, loopscope, loc_scope, &sin_loop, &sin_local);
			if (sin6 == NULL)
				continue;
			if (sctp_is_addr_in_ep(inp, ifa)) {
				return (sin6);
			}
		}
	}
	for (laddr = LIST_FIRST(&inp->sctp_addr_list);
	    laddr && (laddr != inp->next_addr_touse);
	    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
		if (laddr->ifa == NULL) {
			/* address has been removed */
			continue;
		}
		sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
		if (sin6 == NULL)
			continue;

		if ((loopscope == 0) &&
		    (loc_scope == 0) &&
		    (sin_loop == 0) &&
		    (sin_local == 0)) {
			/* all of global scope we are ok with it */
			return (sin6);
		}
		if (loopscope && sin_loop)
			/* both on the loopback, thats ok */
			return (sin6);
		if (loc_scope && sin_local)
			/* both local scope */
			return (sin6);

	}
	/*
	 * if we reach here, we could not find two addresses of the same
	 * scope to give out. Lets look for any higher level scope for a
	 * source address.
	 */
	for (laddr = LIST_FIRST(&inp->sctp_addr_list);
	    laddr && (laddr != inp->next_addr_touse);
	    laddr = LIST_NEXT(laddr, sctp_nxt_addr)) {
		if (laddr->ifa == NULL) {
			/* address has been removed */
			continue;
		}
		sin6 = sctp_is_v6_ifa_addr_acceptable(laddr->ifa, loopscope, loc_scope, &sin_loop, &sin_local);
		if (sin6 == NULL)
			continue;
		return (sin6);
	}
	/* no address bound can be a source for the destination */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Src address selection for EP, no acceptable src address found for address\n");
	}
#endif
	RTFREE(ro->ro_rt);
	ro->ro_rt = NULL;
	return (NULL);
}


static struct sockaddr_in6 *
sctp_select_v6_nth_addr_from_ifn_boundall(struct ifnet *ifn, struct sctp_tcb *stcb, int non_asoc_addr_ok, uint8_t loopscope,
    uint8_t loc_scope, int cur_addr_num, int match_scope)
{
	struct ifaddr *ifa;
	struct sockaddr_in6 *sin6;
	int sin_loop, sin_local;
	int num_eligible_addr = 0;

	TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
		sin6 = sctp_is_v6_ifa_addr_acceptable(ifa, loopscope, loc_scope, &sin_loop, &sin_local);
		if (sin6 == NULL)
			continue;
		if (stcb) {
			if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin6)) {
				/*
				 * It is restricted for some reason..
				 * probably not yet added.
				 */
				continue;
			}
		}
		if (match_scope) {
			/* Here we are asked to match scope if possible */
			if (loopscope && sin_loop)
				/* src and destination are loopback scope */
				return (sin6);
			if (loc_scope && sin_local)
				/* src and destination are local scope */
				return (sin6);
			if ((loopscope == 0) &&
			    (loc_scope == 0) &&
			    (sin_loop == 0) &&
			    (sin_local == 0)) {
				/* src and destination are global scope */
				return (sin6);
			}
			continue;
		}
		if (num_eligible_addr == cur_addr_num) {
			/* this is it */
			return (sin6);
		}
		num_eligible_addr++;
	}
	return (NULL);
}


static int
sctp_count_v6_num_eligible_boundall(struct ifnet *ifn, struct sctp_tcb *stcb,
    int non_asoc_addr_ok, uint8_t loopscope, uint8_t loc_scope)
{
	struct ifaddr *ifa;
	struct sockaddr_in6 *sin6;
	int num_eligible_addr = 0;
	int sin_loop, sin_local;

	TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
		sin6 = sctp_is_v6_ifa_addr_acceptable(ifa, loopscope, loc_scope, &sin_loop, &sin_local);
		if (sin6 == NULL)
			continue;
		if (stcb) {
			if ((non_asoc_addr_ok == 0) && sctp_is_addr_restricted(stcb, (struct sockaddr *)sin6)) {
				/*
				 * It is restricted for some reason..
				 * probably not yet added.
				 */
				continue;
			}
		}
		num_eligible_addr++;
	}
	return (num_eligible_addr);
}


static struct sockaddr_in6 *
sctp_choose_v6_boundall(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_nets *net,
    struct route *ro,
    uint8_t loc_scope,
    uint8_t loopscope,
    int non_asoc_addr_ok)
{
	/*
	 * Ok, we are bound all SO any address is ok to use as long as it is
	 * NOT in the negative list.
	 */
	int num_eligible_addr;
	int cur_addr_num = 0;
	int started_at_beginning = 0;
	int match_scope_prefered;

	/*
	 * first question is, how many eligible addresses are there for the
	 * destination ifn that we are using that are within the proper
	 * scope?
	 */
	struct ifnet *ifn;
	struct sockaddr_in6 *sin6;
	struct rtentry *rt;

	rt = ro->ro_rt;
	ifn = rt->rt_ifp;
	if (net) {
		cur_addr_num = net->indx_of_eligible_next_to_use;
	}
	if (cur_addr_num == 0) {
		match_scope_prefered = 1;
	} else {
		match_scope_prefered = 0;
	}
	num_eligible_addr = sctp_count_v6_num_eligible_boundall(ifn, stcb, non_asoc_addr_ok, loopscope, loc_scope);
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Found %d eligible source addresses\n", num_eligible_addr);
	}
#endif
	if (num_eligible_addr == 0) {
		/*
		 * no eligible addresses, we must use some other interface
		 * address if we can find one.
		 */
		goto bound_all_v6_plan_b;
	}
	/*
	 * Ok we have num_eligible_addr set with how many we can use, this
	 * may vary from call to call due to addresses being deprecated
	 * etc..
	 */
	if (cur_addr_num >= num_eligible_addr) {
		cur_addr_num = 0;
	}
	/*
	 * select the nth address from the list (where cur_addr_num is the
	 * nth) and 0 is the first one, 1 is the second one etc...
	 */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("cur_addr_num:%d match_scope_prefered:%d select it\n",
		    cur_addr_num, match_scope_prefered);
	}
#endif
	sin6 = sctp_select_v6_nth_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope,
	    loc_scope, cur_addr_num, match_scope_prefered);
	if (match_scope_prefered && (sin6 == NULL)) {
		/* retry without the preference for matching scope */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("retry with no match_scope_prefered\n");
		}
#endif
		sin6 = sctp_select_v6_nth_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope,
		    loc_scope, cur_addr_num, 0);
	}
	if (sin6) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Selected address %d ifn:%x for the route\n", cur_addr_num, (uint32_t) ifn);
		}
#endif
		if (net) {
			/* store so we get the next one */
			if (cur_addr_num < 255)
				net->indx_of_eligible_next_to_use = cur_addr_num + 1;
			else
				net->indx_of_eligible_next_to_use = 0;
		}
		return (sin6);
	}
	num_eligible_addr = 0;
bound_all_v6_plan_b:
	/*
	 * ok, if we reach here we either fell through due to something
	 * changing during an interupt (unlikely) or we have NO eligible
	 * source addresses for the ifn of the route (most likely). We must
	 * look at all the other interfaces EXCEPT rt->rt_ifp and do the
	 * same game.
	 */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("bound-all Plan B\n");
	}
#endif
	if (inp->next_ifn_touse == NULL) {
		started_at_beginning = 1;
		inp->next_ifn_touse = TAILQ_FIRST(&ifnet);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Start at first IFN:%x\n", (uint32_t) inp->next_ifn_touse);
		}
#endif
	} else {
		inp->next_ifn_touse = TAILQ_NEXT(inp->next_ifn_touse, if_list);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Resume at IFN:%x\n", (uint32_t) inp->next_ifn_touse);
		}
#endif
		if (inp->next_ifn_touse == NULL) {
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("IFN Resets\n");
			}
#endif
			started_at_beginning = 1;
			inp->next_ifn_touse = TAILQ_FIRST(&ifnet);
		}
	}
	for (ifn = inp->next_ifn_touse; ifn;
	    ifn = TAILQ_NEXT(ifn, if_list)) {
		if (loopscope == 0 && ifn->if_type == IFT_LOOP) {
			/* wrong base scope */
			continue;
		}
		if (loc_scope && (ifn->if_index != loc_scope)) {
			/*
			 * by definition the scope (from to->sin6_scopeid)
			 * must match that of the interface. If not then we
			 * could pick a wrong scope for the address.
			 * Ususally we don't hit plan-b since the route
			 * handles this. However we can hit plan-b when we
			 * send to local-host so the route is the loopback
			 * interface, but the destination is a link local.
			 */
			continue;
		}
		if (ifn == rt->rt_ifp) {
			/* already looked at this guy */
			continue;
		}
		/*
		 * Address rotation will only work when we are not rotating
		 * sourced interfaces and are using the interface of the
		 * route. We would need to have a per interface index in
		 * order to do proper rotation.
		 */
		num_eligible_addr = sctp_count_v6_num_eligible_boundall(ifn, stcb, non_asoc_addr_ok, loopscope, loc_scope);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("IFN:%x has %d eligible\n", (uint32_t) ifn, num_eligible_addr);
		}
#endif
		if (num_eligible_addr == 0) {
			/* none we can use */
			continue;
		}
		/*
		 * Ok we have num_eligible_addr set with how many we can
		 * use, this may vary from call to call due to addresses
		 * being deprecated etc..
		 */
		inp->next_ifn_touse = ifn;

		/*
		 * select the first one we can find with perference for
		 * matching scope.
		 */
		sin6 = sctp_select_v6_nth_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope, loc_scope, 0, 1);
		if (sin6 == NULL) {
			/*
			 * can't find one with matching scope how about a
			 * source with higher scope
			 */
			sin6 = sctp_select_v6_nth_addr_from_ifn_boundall(ifn, stcb, non_asoc_addr_ok, loopscope, loc_scope, 0, 0);
			if (sin6 == NULL)
				/* Hmm, can't find one in the interface now */
				continue;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Selected the %d'th address of ifn:%x\n",
			    cur_addr_num,
			    (uint32_t) ifn);
		}
#endif
		return (sin6);
	}
	if (started_at_beginning == 0) {
		/*
		 * we have not been through all of them yet, force us to go
		 * through them all.
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Force a recycle\n");
		}
#endif
		inp->next_ifn_touse = NULL;
		goto bound_all_v6_plan_b;
	}
	RTFREE(ro->ro_rt);
	ro->ro_rt = NULL;
	return (NULL);

}

/* stcb and net may be NULL */
struct in6_addr
sctp_ipv6_source_address_selection(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb, struct route *ro, struct sctp_nets *net,
    int non_asoc_addr_ok)
{
	struct in6_addr ans;
	struct sockaddr_in6 *rt_addr;
	uint8_t loc_scope, loopscope;
	struct sockaddr_in6 *to = (struct sockaddr_in6 *)&ro->ro_dst;

	/*
	 * This routine is tricky standard v6 src address selection cannot
	 * take into account what we have bound etc, so we can't use it.
	 * 
	 * Instead here is what we must do: 1) Make sure we have a route, if we
	 * don't have a route we can never reach the peer. 2) Once we have a
	 * route, determine the scope of the route. Link local, loopback or
	 * global. 3) Next we divide into three types. Either we are bound
	 * all.. which means we want to use one of the addresses of the
	 * interface we are going out. <or> 4a) We have not stcb, which
	 * means we are using the specific addresses bound on an inp, in
	 * this case we are similar to the stcb case (4b below) accept the
	 * list is always a positive list.<or> 4b) We are bound specific
	 * with a stcb, which means we have a list of bound addresses and we
	 * must see if the ifn of the route is actually one of the bound
	 * addresses. If not, then we must rotate addresses amongst properly
	 * scoped bound addresses, if so we use the address of the
	 * interface. 5) Always, no matter which path we take through the
	 * above we must be sure the source address we use is allowed to be
	 * used. I.e. IN6_IFF_DETACHED, IN6_IFF_NOTREADY, and
	 * IN6_IFF_ANYCAST addresses cannot be used. 6) Addresses that are
	 * deprecated MAY be used if (!ip6_use_deprecated) { if
	 * (IFA6_IS_DEPRECATED(ifa6)) { skip the address } }
	 */

	/*** 1> determine route, if not already done */
	if (ro->ro_rt == NULL) {
		/*
		 * Need a route to cache.
		 */
#ifndef SCOPEDROUTING
		int scope_save;

		scope_save = to->sin6_scope_id;
		to->sin6_scope_id = 0;
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
		rtalloc_ign(ro, 0UL);
#else
		rtalloc(ro);
#endif
#ifndef SCOPEDROUTING
		to->sin6_scope_id = scope_save;
#endif
	}
	if (ro->ro_rt == NULL) {
		/*
		 * no route to host. this packet is going no-where. We
		 * probably should make sure we arrange to send back an
		 * error.
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("No route to host, this packet cannot be sent!\n");
		}
#endif
		memset(&ans, 0, sizeof(ans));
		return (ans);
	}
	/*** 2a> determine scope for outbound address/route */
	loc_scope = loopscope = 0;
	/*
	 * We base our scope on the outbound packet scope and route, NOT the
	 * TCB (if there is one). This way in local scope we will only use a
	 * local scope src address when we send to a local address.
	 */

	if (IN6_IS_ADDR_LOOPBACK(&to->sin6_addr)) {
		/*
		 * If the route goes to the loopback address OR the address
		 * is a loopback address, we are loopback scope.
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Loopback scope is set\n");
		}
#endif
		loc_scope = 0;
		loopscope = 1;
		if (net != NULL) {
			/* mark it as local */
			net->addr_is_local = 1;
		}
	} else if (IN6_IS_ADDR_LINKLOCAL(&to->sin6_addr)) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Link local scope is set, id:%d\n", to->sin6_scope_id);
		}
#endif
		if (to->sin6_scope_id)
			loc_scope = to->sin6_scope_id;
		else {
			loc_scope = 1;
		}
		loopscope = 0;
	} else {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Global scope is set\n");
		}
#endif
	}

	/*
	 * now, depending on which way we are bound we call the appropriate
	 * routine to do steps 3-6
	 */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Destination address:");
		sctp_print_address((struct sockaddr *)to);
	}
#endif

	if (inp->sctp_flags & SCTP_PCB_FLAGS_BOUNDALL) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Calling bound-all src addr selection for v6\n");
		}
#endif
		rt_addr = sctp_choose_v6_boundall(inp, stcb, net, ro, loc_scope, loopscope, non_asoc_addr_ok);
	} else {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Calling bound-specific src addr selection for v6\n");
		}
#endif
		if (stcb)
			rt_addr = sctp_choose_v6_boundspecific_stcb(inp, stcb, net, ro, loc_scope, loopscope, non_asoc_addr_ok);
		else
			/*
			 * we can't have a non-asoc address since we have no
			 * association
			 */
			rt_addr = sctp_choose_v6_boundspecific_inp(inp, ro, loc_scope, loopscope);
	}
	if (rt_addr == NULL) {
		/* no suitable address? */
		struct in6_addr in6;

#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("V6 packet will reach dead-end no suitable src address\n");
		}
#endif
		memset(&in6, 0, sizeof(in6));
		return (in6);
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Source address selected is:");
		sctp_print_address((struct sockaddr *)rt_addr);
	}
#endif
	return (rt_addr->sin6_addr);
}

static uint8_t
sctp_get_ect(struct sctp_tcb *stcb,
    struct sctp_tmit_chunk *chk)
{
	uint8_t this_random;

	/* Huh? */
	if (sctp_ecn_enable == 0)
		return (0);

	if (sctp_ecn_nonce == 0)
		/* no nonce, always return ECT0 */
		return (SCTP_ECT0_BIT);

	if (stcb->asoc.peer_supports_ecn_nonce == 0) {
		/* Peer does NOT support it, so we send a ECT0 only */
		return (SCTP_ECT0_BIT);
	}
	if (chk == NULL)
		return (SCTP_ECT0_BIT);

	if (((stcb->asoc.hb_random_idx == 3) &&
	    (stcb->asoc.hb_ect_randombit > 7)) ||
	    (stcb->asoc.hb_random_idx > 3)) {
		uint32_t rndval;

		rndval = sctp_select_initial_TSN(&stcb->sctp_ep->sctp_ep);
		memcpy(stcb->asoc.hb_random_values, &rndval,
		    sizeof(stcb->asoc.hb_random_values));
		this_random = stcb->asoc.hb_random_values[0];
		stcb->asoc.hb_random_idx = 0;
		stcb->asoc.hb_ect_randombit = 0;
	} else {
		if (stcb->asoc.hb_ect_randombit > 7) {
			stcb->asoc.hb_ect_randombit = 0;
			stcb->asoc.hb_random_idx++;
		}
		this_random = stcb->asoc.hb_random_values[stcb->asoc.hb_random_idx];
	}
	if ((this_random >> stcb->asoc.hb_ect_randombit) & 0x01) {
		if (chk != NULL)
			/* ECN Nonce stuff */
			chk->rec.data.ect_nonce = SCTP_ECT1_BIT;
		stcb->asoc.hb_ect_randombit++;
		return (SCTP_ECT1_BIT);
	} else {
		stcb->asoc.hb_ect_randombit++;
		return (SCTP_ECT0_BIT);
	}
}

extern int sctp_no_csum_on_loopback;

static int
sctp_lowlevel_chunk_output(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,	/* may be NULL */
    struct sctp_nets *net,
    struct sockaddr *to,
    struct mbuf *m,
    uint32_t auth_offset,
    struct sctp_auth_chunk *auth,
    int nofragment_flag,
    int ecn_ok,
    struct sctp_tmit_chunk *chk,
    int out_of_asoc_ok)
/* nofragment_flag to tell if IP_DF should be set (IPv4 only) */
{
	/*
	 * Given a mbuf chain (via m_next) that holds a packet header WITH a
	 * SCTPHDR but no IP header, endpoint inp and sa structure. - fill
	 * in the HMAC digest of any AUTH chunk in the packet - calculate
	 * SCTP checksum and fill in - prepend a IP address header - if
	 * boundall use INADDR_ANY - if boundspecific do source address
	 * selection - set fragmentation option for ipV4 - On return from IP
	 * output, check/adjust mtu size - of output interface and
	 * smallest_mtu size as well.
	 */
	struct sctphdr *sctphdr;
	int o_flgs;
	uint32_t csum;
	int ret;
	unsigned int have_mtu;
	struct route *ro;

	if ((net) && (net->dest_state & SCTP_ADDR_OUT_OF_SCOPE)) {
		sctp_m_freem(m);
		return (EFAULT);
	}
	if ((m->m_flags & M_PKTHDR) == 0) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Software error: sctp_lowlevel_chunk_output() called with non pkthdr!\n");
		}
#endif
		sctp_m_freem(m);
		return (EFAULT);
	}
#ifdef RANDYS_SPECIAL_AUDIT
	/* special audit */
	{
		struct mbuf *foo;
		int foo_len = 0;
		int foo_goal = 0;

		foo = m;
		foo_goal = m->m_pkthdr.len;

		if ((foo_goal <= 0) || (foo_goal > 65535)) {
			panic("pkt header len not set");
		}
		while (foo) {
			foo_len += foo->m_len;
			if (foo_len == foo_goal) {
				if (foo->m_next != NULL) {
					panic("Bad chain");
				}
				break;
			} else if (foo_len > foo_goal) {
				panic("Bad chain - overflow");
			}
			foo = foo->m_next;
		}
	}
#endif
	/* fill in the HMAC digest for any AUTH chunk in the packet */
	if ((auth != NULL) && (stcb != NULL)) {
		sctp_fill_hmac_digest_m(m, auth_offset, auth, stcb);
	}
	/* Calculate the csum and fill in the length of the packet */
	sctphdr = mtod(m, struct sctphdr *);
	have_mtu = 0;
	if (sctp_no_csum_on_loopback &&
	    (stcb) &&
	    (stcb->asoc.loopback_scope)) {
		sctphdr->checksum = 0;
		/*
		 * This can probably now be taken out since my audit shows
		 * no more bad pktlen's coming in. But we will wait a while
		 * yet.
		 */
		m->m_pkthdr.len = sctp_calculate_len(m);
	} else {
		sctphdr->checksum = 0;
		csum = sctp_calculate_sum(m, &m->m_pkthdr.len, 0);
		sctphdr->checksum = csum;
	}
	if (to->sa_family == AF_INET) {
		struct ip *ip;
		struct route iproute;
		uint8_t tos_value;

		M_PREPEND(m, sizeof(struct ip), M_DONTWAIT);
		if (m == NULL) {
			/* failed to prepend data, give up */
			return (ENOMEM);
		}
		ip = mtod(m, struct ip *);
		ip->ip_v = IPVERSION;
		ip->ip_hl = (sizeof(struct ip) >> 2);
		if (net) {
			tos_value = net->tos_flowlabel & 0x000000ff;
		} else {
#if defined(__FreeBSD__) || defined(__APPLE__)
			tos_value = inp->ip_inp.inp.inp_ip_tos;
#elif defined(__NetBSD__)
			tos_value = inp->ip_inp.inp.inp_ip.ip_tos;
#else
			tos_value = inp->inp_ip_tos;
#endif
		}
		if (nofragment_flag) {
#if defined(WITH_CONVERT_IP_OFF) || defined(__FreeBSD__) || defined(__APPLE__)
#ifdef __OpenBSD__
			/* OpenBSD has WITH_CONVERT_IP_OFF defined?? */
			ip->ip_off = htons(IP_DF);
#else
			ip->ip_off = IP_DF;
#endif
#else
			ip->ip_off = htons(IP_DF);
#endif
		} else
			ip->ip_off = 0;


#if defined(__FreeBSD__)
		/* FreeBSD has a function for ip_id's */
		ip->ip_id = ip_newid();
#elif defined(RANDOM_IP_ID) || defined(__NetBSD__) || defined(__OpenBSD__)
		/* Apple has RANDOM_IP_ID switch */
		ip->ip_id = htons(ip_randomid());
#else
		ip->ip_id = htons(ip_id++);
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
		ip->ip_ttl = inp->ip_inp.inp.inp_ip_ttl;
#else
		ip->ip_ttl = inp->inp_ip_ttl;
#endif
#if defined(__OpenBSD__) || defined(__NetBSD__)
		ip->ip_len = htons(m->m_pkthdr.len);
#else
		ip->ip_len = m->m_pkthdr.len;
#endif
		if (stcb) {
			if ((stcb->asoc.ecn_allowed) && ecn_ok) {
				/* Enable ECN */
				ip->ip_tos = ((u_char)(tos_value & 0xfc) | sctp_get_ect(stcb, chk));
			} else {
				/* No ECN */
				ip->ip_tos = (u_char)(tos_value & 0xfc);
			}
		} else {
			/* no association at all */
			ip->ip_tos = (tos_value & 0xfc);
		}
		ip->ip_p = IPPROTO_SCTP;
		ip->ip_sum = 0;
		if (net == NULL) {
			ro = &iproute;
			memset(&iproute, 0, sizeof(iproute));
			memcpy(&ro->ro_dst, to, to->sa_len);
		} else {
			ro = (struct route *)&net->ro;
		}
		/* Now the address selection part */
		ip->ip_dst.s_addr = ((struct sockaddr_in *)to)->sin_addr.s_addr;

		/* call the routine to select the src address */
		if (net) {
			if (net->src_addr_selected == 0) {
				/* Cache the source address */
				((struct sockaddr_in *)&net->ro._s_addr)->sin_addr = sctp_ipv4_source_address_selection(inp,
				    stcb,
				    ro, net, out_of_asoc_ok);
				if (ro->ro_rt)
					net->src_addr_selected = 1;
			}
			ip->ip_src = ((struct sockaddr_in *)&net->ro._s_addr)->sin_addr;
		} else {
			ip->ip_src = sctp_ipv4_source_address_selection(inp,
			    stcb, ro, net, out_of_asoc_ok);
		}
		/*
		 * If source address selection fails and we find no route
		 * then the ip_ouput should fail as well with a
		 * NO_ROUTE_TO_HOST type error. We probably should catch
		 * that somewhere and abort the association right away
		 * (assuming this is an INIT being sent).
		 */
		if ((ro->ro_rt == NULL)) {
			/*
			 * src addr selection failed to find a route (or
			 * valid source addr), so we can't get there from
			 * here!
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("low_level_output: dropped v4 packet- no valid source addr\n");
				printf("Destination was %x\n", (uint32_t) (ntohl(ip->ip_dst.s_addr)));
			}
#endif				/* SCTP_DEBUG */
			if (net) {
				if ((net->dest_state & SCTP_ADDR_REACHABLE) && stcb)
					sctp_ulp_notify(SCTP_NOTIFY_INTERFACE_DOWN,
					    stcb,
					    SCTP_FAILED_THRESHOLD,
					    (void *)net);
				net->dest_state &= ~SCTP_ADDR_REACHABLE;
				net->dest_state |= SCTP_ADDR_NOT_REACHABLE;
				if (stcb) {
					if (net == stcb->asoc.primary_destination) {
						/* need a new primary */
						struct sctp_nets *alt;

						alt = sctp_find_alternate_net(stcb, net, 0);
						if (alt != net) {
							if (sctp_set_primary_addr(stcb,
							    (struct sockaddr *)NULL,
							    alt) == 0) {
								net->dest_state |= SCTP_ADDR_WAS_PRIMARY;
								net->src_addr_selected = 0;
							}
						}
					}
				}
			}
			sctp_m_freem(m);
			return (EHOSTUNREACH);
		} else {
			have_mtu = ro->ro_rt->rt_ifp->if_mtu;
		}

		if(inp->sctp_socket) {
			o_flgs = (IP_RAWOUTPUT | (inp->sctp_socket->so_options & (SO_DONTROUTE | SO_BROADCAST)));
		} else {
			o_flgs = IP_RAWOUTPUT;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("Calling ipv4 output routine from low level src addr:%x\n",
			    (uint32_t) (ntohl(ip->ip_src.s_addr)));
			printf("Destination is %x\n", (uint32_t) (ntohl(ip->ip_dst.s_addr)));
			printf("RTP route is %p through\n", ro->ro_rt);
		}
#endif
		if ((have_mtu) && (net) && (have_mtu > net->mtu)) {
			ro->ro_rt->rt_ifp->if_mtu = net->mtu;
		}
		if (ro != &iproute) {
			memcpy(&iproute, ro, sizeof(*ro));
		}
		ret = ip_output(m, inp->ip_inp.inp.inp_options,
		    ro, o_flgs, inp->ip_inp.inp.inp_moptions
#if defined(__OpenBSD__) || (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,(struct inpcb *)NULL
#endif
		    );
		if ((ro->ro_rt) && (have_mtu) && (net) && (have_mtu > net->mtu)) {
			ro->ro_rt->rt_ifp->if_mtu = have_mtu;
		}
		SCTP_STAT_INCR(sctps_sendpackets);
		if(ret)
			SCTP_STAT_INCR(sctps_senderrors);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("Ip output returns %d\n", ret);
		}
#endif
		if (net == NULL) {
			/* free tempy routes */
			if (ro->ro_rt)
				RTFREE(ro->ro_rt);
		} else {
			/* PMTU check versus smallest asoc MTU goes here */
			if (ro->ro_rt != NULL) {
				if (ro->ro_rt->rt_rmx.rmx_mtu &&
				    (stcb->asoc.smallest_mtu > ro->ro_rt->rt_rmx.rmx_mtu)) {
					sctp_mtu_size_reset(inp, &stcb->asoc,
					    ro->ro_rt->rt_rmx.rmx_mtu);
				}
			} else {
				/* route was freed */
				net->src_addr_selected = 0;
			}
		}
		return (ret);
	}
#ifdef INET6
	else if (to->sa_family == AF_INET6) {
		uint32_t flowlabel;
		struct ip6_hdr *ip6h;

#ifdef NEW_STRUCT_ROUTE
		struct route ip6route;

#else
		struct route_in6 ip6route;

#endif
		struct ifnet *ifp;
		u_char flowTop;
		uint16_t flowBottom;
		u_char tosBottom, tosTop;
		struct sockaddr_in6 *sin6, tmp, *lsa6, lsa6_tmp;
		struct sockaddr_in6 lsa6_storage;
		int prev_scope = 0;
		int error;
		u_short prev_port = 0;

		if (net != NULL) {
			flowlabel = net->tos_flowlabel;
		} else {
			flowlabel = ((struct in6pcb *)inp)->in6p_flowinfo;
		}
		M_PREPEND(m, sizeof(struct ip6_hdr), M_DONTWAIT);
		if (m == NULL) {
			/* failed to prepend data, give up */
			return (ENOMEM);
		}
		ip6h = mtod(m, struct ip6_hdr *);

		/*
		 * We assume here that inp_flow is in host byte order within
		 * the TCB!
		 */
		flowBottom = flowlabel & 0x0000ffff;
		flowTop = ((flowlabel & 0x000f0000) >> 16);
		tosTop = (((flowlabel & 0xf0) >> 4) | IPV6_VERSION);
		/* protect *sin6 from overwrite */
		sin6 = (struct sockaddr_in6 *)to;
		tmp = *sin6;
		sin6 = &tmp;

		/* KAME hack: embed scopeid */
#if defined(SCTP_BASE_FREEBSD) || defined(__APPLE__)
		if (in6_embedscope(&sin6->sin6_addr, sin6, NULL, NULL) != 0)
#else
#ifdef SCTP_KAME
		if (sa6_embedscope(sin6, ip6_use_defzone) != 0)
#else
		if (in6_embedscope(&sin6->sin6_addr, sin6) != 0)
#endif				/* SCTP_KAME */
#endif
			return (EINVAL);
		if (net == NULL) {
			memset(&ip6route, 0, sizeof(ip6route));
			ro = (struct route *)&ip6route;
			memcpy(&ro->ro_dst, sin6, sin6->sin6_len);
		} else {
			ro = (struct route *)&net->ro;
		}
		if (stcb != NULL) {
			if ((stcb->asoc.ecn_allowed) && ecn_ok) {
				/* Enable ECN */
				tosBottom = (((((struct in6pcb *)inp)->in6p_flowinfo & 0x0c) | sctp_get_ect(stcb, chk)) << 4);
			} else {
				/* No ECN */
				tosBottom = ((((struct in6pcb *)inp)->in6p_flowinfo & 0x0c) << 4);
			}
		} else {
			/* we could get no asoc if it is a O-O-T-B packet */
			tosBottom = ((((struct in6pcb *)inp)->in6p_flowinfo & 0x0c) << 4);
		}
		ip6h->ip6_flow = htonl(((tosTop << 24) | ((tosBottom | flowTop) << 16) | flowBottom));
		ip6h->ip6_nxt = IPPROTO_SCTP;
		ip6h->ip6_plen = m->m_pkthdr.len;
		ip6h->ip6_dst = sin6->sin6_addr;

		/*
		 * Add SRC address selection here: we can only reuse to a
		 * limited degree the kame src-addr-sel, since we can try
		 * their selection but it may not be bound.
		 */
		bzero(&lsa6_tmp, sizeof(lsa6_tmp));
		lsa6_tmp.sin6_family = AF_INET6;
		lsa6_tmp.sin6_len = sizeof(lsa6_tmp);
		lsa6 = &lsa6_tmp;
		if (net) {
			if (net->src_addr_selected == 0) {
				/* Cache the source address */
				((struct sockaddr_in6 *)&net->ro._s_addr)->sin6_addr = sctp_ipv6_source_address_selection(inp,
				    stcb, ro, net, out_of_asoc_ok);

				if (ro->ro_rt)
					net->src_addr_selected = 1;
			}
			lsa6->sin6_addr = ((struct sockaddr_in6 *)&net->ro._s_addr)->sin6_addr;
		} else {
			lsa6->sin6_addr = sctp_ipv6_source_address_selection(
			    inp, stcb, ro, net, out_of_asoc_ok);
		}
		lsa6->sin6_port = inp->sctp_lport;

		if ((ro->ro_rt == NULL)) {
			/*
			 * src addr selection failed to find a route (or
			 * valid source addr), so we can't get there from
			 * here!
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("low_level_output: dropped v6 pkt- no valid source addr\n");
			}
#endif
			sctp_m_freem(m);
			if (net) {
				if ((net->dest_state & SCTP_ADDR_REACHABLE) && stcb)
					sctp_ulp_notify(SCTP_NOTIFY_INTERFACE_DOWN,
					    stcb,
					    SCTP_FAILED_THRESHOLD,
					    (void *)net);
				net->dest_state &= ~SCTP_ADDR_REACHABLE;
				net->dest_state |= SCTP_ADDR_NOT_REACHABLE;
				if (stcb) {
					if (net == stcb->asoc.primary_destination) {
						/* need a new primary */
						struct sctp_nets *alt;

						alt = sctp_find_alternate_net(stcb, net, 0);
						if (alt != net) {
							if (sctp_set_primary_addr(stcb,
							    (struct sockaddr *)NULL,
							    alt) == 0) {
								net->dest_state |= SCTP_ADDR_WAS_PRIMARY;
								net->src_addr_selected = 0;
							}
						}
					}
				}
			}
			return (EHOSTUNREACH);
		}
#ifndef SCOPEDROUTING
		/*
		 * XXX: sa6 may not have a valid sin6_scope_id in the
		 * non-SCOPEDROUTING case.
		 */
		bzero(&lsa6_storage, sizeof(lsa6_storage));
		lsa6_storage.sin6_family = AF_INET6;
		lsa6_storage.sin6_len = sizeof(lsa6_storage);
#ifdef SCTP_KAME
		if ((error = sa6_recoverscope(&lsa6_storage)) != 0) {
#else
		if ((error = in6_recoverscope(&lsa6_storage, &lsa6->sin6_addr,
		    NULL)) != 0) {
#endif				/* SCTP_KAME */
			sctp_m_freem(m);
			return (error);
		}
		/* XXX */
		lsa6_storage.sin6_addr = lsa6->sin6_addr;
		lsa6_storage.sin6_port = inp->sctp_lport;
		lsa6 = &lsa6_storage;
#endif				/* SCOPEDROUTING */
		ip6h->ip6_src = lsa6->sin6_addr;

		/*
		 * We set the hop limit now since there is a good chance
		 * that our ro pointer is now filled
		 */
		ip6h->ip6_hlim = in6_selecthlim((struct in6pcb *)&inp->ip_inp.inp,
		    (ro ?
		    (ro->ro_rt ? (ro->ro_rt->rt_ifp) : (NULL)) :
		    (NULL)));
		o_flgs = 0;
		ifp = ro->ro_rt->rt_ifp;
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			/* Copy to be sure something bad is not happening */
			sin6->sin6_addr = ip6h->ip6_dst;
			lsa6->sin6_addr = ip6h->ip6_src;

			printf("Calling ipv6 output routine from low level\n");
			printf("src: ");
			sctp_print_address((struct sockaddr *)lsa6);
			printf("dst: ");
			sctp_print_address((struct sockaddr *)sin6);
		}
#endif				/* SCTP_DEBUG */
		if (net) {
			sin6 = (struct sockaddr_in6 *)&net->ro._l_addr;
			/* preserve the port and scope for link local send */
			prev_scope = sin6->sin6_scope_id;
			prev_port = sin6->sin6_port;
		}
		ret = ip6_output(m, ((struct in6pcb *)inp)->in6p_outputopts,
#ifdef NEW_STRUCT_ROUTE
		    ro,
#else
		    (struct route_in6 *)ro,
#endif
		    o_flgs,
		    ((struct in6pcb *)inp)->in6p_moptions,
#if defined(__NetBSD__)
		    (struct socket *)inp->sctp_socket,
#endif
		    &ifp
#if (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
		    ,0
#endif
		    );
		if (net) {
			/* for link local this must be done */
			sin6->sin6_scope_id = prev_scope;
			sin6->sin6_port = prev_port;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("return from send is %d\n", ret);
		}
#endif				/* SCTP_DEBUG_OUTPUT */
		SCTP_STAT_INCR(sctps_sendpackets);
		if(ret)
			SCTP_STAT_INCR(sctps_senderrors);
		if (net == NULL) {
			/* Now if we had a temp route free it */
			if (ro->ro_rt) {
				RTFREE(ro->ro_rt);
			}
		} else {
			/* PMTU check versus smallest asoc MTU goes here */
			if (ro->ro_rt == NULL) {
				/* Route was freed */
				net->src_addr_selected = 0;
			}
			if (ro->ro_rt != NULL) {
				if (ro->ro_rt->rt_rmx.rmx_mtu &&
				    (stcb->asoc.smallest_mtu > ro->ro_rt->rt_rmx.rmx_mtu)) {
					sctp_mtu_size_reset(inp,
					    &stcb->asoc,
					    ro->ro_rt->rt_rmx.rmx_mtu);
				}
			} else if (ifp) {
#if (defined(SCTP_BASE_FREEBSD) &&  __FreeBSD_version < 500000) || defined(__APPLE__)
#define ND_IFINFO(ifp) (&nd_ifinfo[ifp->if_index])
#endif				/* SCTP_BASE_FREEBSD */
				if (ND_IFINFO(ifp)->linkmtu &&
				    (stcb->asoc.smallest_mtu > ND_IFINFO(ifp)->linkmtu)) {
					sctp_mtu_size_reset(inp,
					    &stcb->asoc,
					    ND_IFINFO(ifp)->linkmtu);
				}
			}
		}
		return (ret);
	}
#endif
	else {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Unknown protocol (TSNH) type %d\n", ((struct sockaddr *)to)->sa_family);
		}
#endif
		sctp_m_freem(m);
		return (EFAULT);
	}
}

static
int
sctp_is_address_in_scope(struct ifaddr *ifa,
    int ipv4_addr_legal,
    int ipv6_addr_legal,
    int loopback_scope,
    int ipv4_local_scope,
    int local_scope,
    int site_scope)
{
	if ((loopback_scope == 0) &&
	    (ifa->ifa_ifp) &&
	    (ifa->ifa_ifp->if_type == IFT_LOOP)) {
		/*
		 * skip loopback if not in scope *
		 */
		return (0);
	}
	if ((ifa->ifa_addr->sa_family == AF_INET) && ipv4_addr_legal) {
		struct sockaddr_in *sin;

		sin = (struct sockaddr_in *)ifa->ifa_addr;
		if (sin->sin_addr.s_addr == 0) {
			/* not in scope , unspecified */
			return (0);
		}
		if ((ipv4_local_scope == 0) &&
		    (IN4_ISPRIVATE_ADDRESS(&sin->sin_addr))) {
			/* private address not in scope */
			return (0);
		}
	} else if ((ifa->ifa_addr->sa_family == AF_INET6) && ipv6_addr_legal) {
		struct sockaddr_in6 *sin6;
		struct in6_ifaddr *ifa6;

		ifa6 = (struct in6_ifaddr *)ifa;
		/* ok to use deprecated addresses? */
		if (!ip6_use_deprecated) {
			if (ifa6->ia6_flags &
			    IN6_IFF_DEPRECATED) {
				return (0);
			}
		}
		if (ifa6->ia6_flags &
		    (IN6_IFF_DETACHED |
		    IN6_IFF_ANYCAST |
		    IN6_IFF_NOTREADY)) {
			return (0);
		}
		sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
		if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
			/* skip unspecifed addresses */
			return (0);
		}
		if (		/* (local_scope == 0) && */
		    (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))) {
			return (0);
		}
		if ((site_scope == 0) &&
		    (IN6_IS_ADDR_SITELOCAL(&sin6->sin6_addr))) {
			return (0);
		}
	} else {
		return (0);
	}
	return (1);
}


void
sctp_send_initiate(struct sctp_inpcb *inp, struct sctp_tcb *stcb)
{
	struct mbuf *m, *m_at, *m_last;
	struct sctp_nets *net;
	struct sctp_init_msg *initm;
	struct sctp_supported_addr_param *sup_addr;
	struct sctp_ecn_supported_param *ecn;
	struct sctp_prsctp_supported_param *prsctp;
	struct sctp_ecn_nonce_supported_param *ecn_nonce;
	struct sctp_supported_chunk_types_param *pr_supported;
	int cnt_inits_to = 0;
	int padval, ret;
	int num_ext;
	int p_len;

	/* INIT's always go to the primary (and usually ONLY address) */
	m_last = NULL;
	net = stcb->asoc.primary_destination;
	if (net == NULL) {
		net = TAILQ_FIRST(&stcb->asoc.nets);
		if (net == NULL) {
			/* TSNH */
			return;
		}
		/* we confirm any address we send an INIT to */
		net->dest_state &= ~SCTP_ADDR_UNCONFIRMED;
		sctp_set_primary_addr(stcb, NULL, net);
	} else {
		/* we confirm any address we send an INIT to */
		net->dest_state &= ~SCTP_ADDR_UNCONFIRMED;
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("Sending INIT to ");
		sctp_print_address((struct sockaddr *)&net->ro._l_addr);
	}
#endif
	if (((struct sockaddr *)&(net->ro._l_addr))->sa_family == AF_INET6) {
		/*
		 * special hook, if we are sending to link local it will not
		 * show up in our private address count.
		 */
		struct sockaddr_in6 *sin6l;

		sin6l = &net->ro._l_addr.sin6;
		if (IN6_IS_ADDR_LINKLOCAL(&sin6l->sin6_addr))
			cnt_inits_to = 1;
	}
	if (callout_pending(&net->rxt_timer.timer)) {
		/* This case should not happen */
		return;
	}
	/* start the INIT timer */
	if (sctp_timer_start(SCTP_TIMER_TYPE_INIT, inp, stcb, net)) {
		/* we are hosed since I can't start the INIT timer? */
		return;
	}
	m = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if (m == NULL) {
		/* No memory, INIT timer will re-attempt. */
		return;
	}
	m->m_pkthdr.len = 0;
	m->m_data += SCTP_MIN_OVERHEAD;
	m->m_len = sizeof(struct sctp_init_msg);
	/* Now lets put the SCTP header in place */
	initm = mtod(m, struct sctp_init_msg *);
	initm->sh.src_port = inp->sctp_lport;
	initm->sh.dest_port = stcb->rport;
	initm->sh.v_tag = 0;
	initm->sh.checksum = 0;	/* calculate later */
	/* now the chunk header */
	initm->msg.ch.chunk_type = SCTP_INITIATION;
	initm->msg.ch.chunk_flags = 0;
	/* fill in later from mbuf we build */
	initm->msg.ch.chunk_length = 0;
	/* place in my tag */
	initm->msg.init.initiate_tag = htonl(stcb->asoc.my_vtag);
	/* set up some of the credits. */
	initm->msg.init.a_rwnd = htonl(max(inp->sctp_socket->so_rcv.sb_hiwat,
	    SCTP_MINIMAL_RWND));

	initm->msg.init.num_outbound_streams = htons(stcb->asoc.pre_open_streams);
	initm->msg.init.num_inbound_streams = htons(stcb->asoc.max_inbound_streams);
	initm->msg.init.initial_tsn = htonl(stcb->asoc.init_seq_number);
	/* now the address restriction */
	sup_addr = (struct sctp_supported_addr_param *)((caddr_t)initm +
	    sizeof(*initm));
	sup_addr->ph.param_type = htons(SCTP_SUPPORTED_ADDRTYPE);
	/* we support 2 types IPv6/IPv4 */
	sup_addr->ph.param_length = htons(sizeof(*sup_addr) +
	    sizeof(uint16_t));
	sup_addr->addr_type[0] = htons(SCTP_IPV4_ADDRESS);
	sup_addr->addr_type[1] = htons(SCTP_IPV6_ADDRESS);
	m->m_len += sizeof(*sup_addr) + sizeof(uint16_t);

	if (inp->sctp_ep.adaptation_layer_indicator) {
		struct sctp_adaptation_layer_indication *ali;

		ali = (struct sctp_adaptation_layer_indication *)(
		    (caddr_t)sup_addr + sizeof(*sup_addr) + sizeof(uint16_t));
		ali->ph.param_type = htons(SCTP_ULP_ADAPTATION);
		ali->ph.param_length = htons(sizeof(*ali));
		ali->indication = ntohl(inp->sctp_ep.adaptation_layer_indicator);
		m->m_len += sizeof(*ali);
		ecn = (struct sctp_ecn_supported_param *)((caddr_t)ali +
		    sizeof(*ali));
	} else {
		ecn = (struct sctp_ecn_supported_param *)((caddr_t)sup_addr +
		    sizeof(*sup_addr) + sizeof(uint16_t));
	}

	/* now any cookie time extensions */
	if (stcb->asoc.cookie_preserve_req) {
		struct sctp_cookie_perserve_param *cookie_preserve;

		cookie_preserve = (struct sctp_cookie_perserve_param *)(ecn);
		cookie_preserve->ph.param_type = htons(SCTP_COOKIE_PRESERVE);
		cookie_preserve->ph.param_length = htons(
		    sizeof(*cookie_preserve));
		cookie_preserve->time = htonl(stcb->asoc.cookie_preserve_req);
		m->m_len += sizeof(*cookie_preserve);
		ecn = (struct sctp_ecn_supported_param *)(
		    (caddr_t)cookie_preserve + sizeof(*cookie_preserve));
		stcb->asoc.cookie_preserve_req = 0;
	}
	/* ECN parameter */
	if (sctp_ecn_enable == 1) {
		ecn->ph.param_type = htons(SCTP_ECN_CAPABLE);
		ecn->ph.param_length = htons(sizeof(*ecn));
		m->m_len += sizeof(*ecn);
		prsctp = (struct sctp_prsctp_supported_param *)((caddr_t)ecn +
		    sizeof(*ecn));
	} else {
		prsctp = (struct sctp_prsctp_supported_param *)((caddr_t)ecn);
	}
	/* And now tell the peer we do pr-sctp */
	prsctp->ph.param_type = htons(SCTP_PRSCTP_SUPPORTED);
	prsctp->ph.param_length = htons(sizeof(*prsctp));
	m->m_len += sizeof(*prsctp);

	/* And now tell the peer we do all the extensions */
	pr_supported = (struct sctp_supported_chunk_types_param *)
	    ((caddr_t)prsctp + sizeof(*prsctp));
	pr_supported->ph.param_type = htons(SCTP_SUPPORTED_CHUNK_EXT);
	num_ext = 0;
	pr_supported->chunk_types[num_ext++] = SCTP_ASCONF;
	pr_supported->chunk_types[num_ext++] = SCTP_ASCONF_ACK;
	pr_supported->chunk_types[num_ext++] = SCTP_FORWARD_CUM_TSN;
	pr_supported->chunk_types[num_ext++] = SCTP_PACKET_DROPPED;
	pr_supported->chunk_types[num_ext++] = SCTP_STREAM_RESET;
	if (!sctp_auth_disable)
		pr_supported->chunk_types[num_ext++] = SCTP_AUTHENTICATION;
	p_len = sizeof(*pr_supported) + num_ext;
	pr_supported->ph.param_length = htons(p_len);
	bzero((caddr_t)pr_supported + p_len, SCTP_SIZE32(p_len) - p_len);
	m->m_len += SCTP_SIZE32(p_len);

	/* ECN nonce: And now tell the peer we support ECN nonce */
	if (sctp_ecn_nonce) {
		ecn_nonce = (struct sctp_ecn_nonce_supported_param *)
		    ((caddr_t)pr_supported + SCTP_SIZE32(p_len));
		ecn_nonce->ph.param_type = htons(SCTP_ECN_NONCE_SUPPORTED);
		ecn_nonce->ph.param_length = htons(sizeof(*ecn_nonce));
		m->m_len += sizeof(*ecn_nonce);
	}
	/* add authentication parameters */
	if (!sctp_auth_disable) {
		struct sctp_auth_random *random;
		struct sctp_auth_hmac_algo *hmacs;
		struct sctp_auth_chunk_list *chunks;

		/* attach RANDOM parameter, if available */
		if (stcb->asoc.authinfo.random != NULL) {
			random = (struct sctp_auth_random *)(mtod(m, caddr_t)+m->m_len);
			random->ph.param_type = htons(SCTP_RANDOM);
			p_len = sizeof(*random) + stcb->asoc.authinfo.random->keylen;
			random->ph.param_length = htons(p_len);
			bcopy(stcb->asoc.authinfo.random->key, random->random_data,
			    stcb->asoc.authinfo.random->keylen);
			/* zero out any padding required */
			bzero((caddr_t)random + p_len, SCTP_SIZE32(p_len) - p_len);
			m->m_len += SCTP_SIZE32(p_len);
		}
		/* add HMAC_ALGO parameter */
		hmacs = (struct sctp_auth_hmac_algo *)(mtod(m, caddr_t)+m->m_len);
		p_len = sctp_serialize_hmaclist(stcb->asoc.local_hmacs,
		    (uint8_t *) hmacs->hmac_ids);
		if (p_len > 0) {
			p_len += sizeof(*hmacs);
			hmacs->ph.param_type = htons(SCTP_HMAC_LIST);
			hmacs->ph.param_length = htons(p_len);
			/* zero out any padding required */
			bzero((caddr_t)hmacs + p_len, SCTP_SIZE32(p_len) - p_len);
			m->m_len += SCTP_SIZE32(p_len);
		}
		/* add CHUNKS parameter */
		chunks = (struct sctp_auth_chunk_list *)(mtod(m, caddr_t)+m->m_len);
		p_len = sctp_serialize_auth_chunks(stcb->asoc.local_auth_chunks,
		    chunks->chunk_types);
		if (p_len > 0) {
			p_len += sizeof(*chunks);
			chunks->ph.param_type = htons(SCTP_CHUNK_LIST);
			chunks->ph.param_length = htons(p_len);
			/* zero out any padding required */
			bzero((caddr_t)chunks + p_len, SCTP_SIZE32(p_len) - p_len);
			m->m_len += SCTP_SIZE32(p_len);
		}
	}
	m_at = m;
	/* now the addresses */
	if (inp->sctp_flags & SCTP_PCB_FLAGS_BOUNDALL) {
		struct ifnet *ifn;
		struct ifaddr *ifa;
		int cnt;

		cnt = cnt_inits_to;
		TAILQ_FOREACH(ifn, &ifnet, if_list) {
			if ((stcb->asoc.loopback_scope == 0) &&
			    (ifn->if_type == IFT_LOOP)) {
				/*
				 * Skip loopback devices if loopback_scope
				 * not set
				 */
				continue;
			}
			TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
				if (sctp_is_address_in_scope(ifa,
				    stcb->asoc.ipv4_addr_legal,
				    stcb->asoc.ipv6_addr_legal,
				    stcb->asoc.loopback_scope,
				    stcb->asoc.ipv4_local_scope,
				    stcb->asoc.local_scope,
				    stcb->asoc.site_scope) == 0) {
					continue;
				}
				cnt++;
			}
		}
		if (cnt > 1) {
			TAILQ_FOREACH(ifn, &ifnet, if_list) {
				if ((stcb->asoc.loopback_scope == 0) &&
				    (ifn->if_type == IFT_LOOP)) {
					/*
					 * Skip loopback devices if
					 * loopback_scope not set
					 */
					continue;
				}
				TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
					if (sctp_is_address_in_scope(ifa,
					    stcb->asoc.ipv4_addr_legal,
					    stcb->asoc.ipv6_addr_legal,
					    stcb->asoc.loopback_scope,
					    stcb->asoc.ipv4_local_scope,
					    stcb->asoc.local_scope,
					    stcb->asoc.site_scope) == 0) {
						continue;
					}
					m_at = sctp_add_addr_to_mbuf(m_at, ifa);
				}
			}
		}
	} else {
		struct sctp_laddr *laddr;
		int cnt;

		cnt = cnt_inits_to;
		/* First, how many ? */
		LIST_FOREACH(laddr, &inp->sctp_addr_list, sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				continue;
			}
			if (laddr->ifa->ifa_addr == NULL)
				continue;
			if (sctp_is_address_in_scope(laddr->ifa,
			    stcb->asoc.ipv4_addr_legal,
			    stcb->asoc.ipv6_addr_legal,
			    stcb->asoc.loopback_scope,
			    stcb->asoc.ipv4_local_scope,
			    stcb->asoc.local_scope,
			    stcb->asoc.site_scope) == 0) {
				continue;
			}
			cnt++;
		}
		/*
		 * To get through a NAT we only list addresses if we have
		 * more than one. That way if you just bind a single address
		 * we let the source of the init dictate our address.
		 */
		if (cnt > 1) {
			LIST_FOREACH(laddr, &inp->sctp_addr_list, sctp_nxt_addr) {
				if (laddr->ifa == NULL) {
					continue;
				}
				if (laddr->ifa->ifa_addr == NULL) {
					continue;
				}
				if (sctp_is_address_in_scope(laddr->ifa,
				    stcb->asoc.ipv4_addr_legal,
				    stcb->asoc.ipv6_addr_legal,
				    stcb->asoc.loopback_scope,
				    stcb->asoc.ipv4_local_scope,
				    stcb->asoc.local_scope,
				    stcb->asoc.site_scope) == 0) {
					continue;
				}
				m_at = sctp_add_addr_to_mbuf(m_at, laddr->ifa);
			}
		}
	}
	/* calulate the size and update pkt header and chunk header */
	m->m_pkthdr.len = 0;
	for (m_at = m; m_at; m_at = m_at->m_next) {
		if (m_at->m_next == NULL)
			m_last = m_at;
		m->m_pkthdr.len += m_at->m_len;
	}
	initm->msg.ch.chunk_length = htons((m->m_pkthdr.len -
	    sizeof(struct sctphdr)));
	/*
	 * We pass 0 here to NOT set IP_DF if its IPv4, we ignore the return
	 * here since the timer will drive a retranmission.
	 */

	/* I don't expect this to execute but we will be safe here */
	padval = m->m_pkthdr.len % 4;
	if ((padval) && (m_last)) {
		/*
		 * The compiler worries that m_last may not be set even
		 * though I think it is impossible :-> however we add m_last
		 * here just in case.
		 */
		int ret;

		ret = sctp_add_pad_tombuf(m_last, (4 - padval));
		if (ret) {
			/* Houston we have a problem, no space */
			sctp_m_freem(m);
			return;
		}
		m->m_pkthdr.len += padval;
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("Calling lowlevel output stcb:%x net:%x\n",
		    (uint32_t) stcb, (uint32_t) net);
	}
#endif
	ret = sctp_lowlevel_chunk_output(inp, stcb, net,
	    (struct sockaddr *)&net->ro._l_addr,
	    m, 0, NULL, 0, 0, NULL, 0);
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("Low level output returns %d\n", ret);
	}
#endif
	sctp_timer_start(SCTP_TIMER_TYPE_INIT, inp, stcb, net);
	SCTP_GETTIME_TIMEVAL(&net->last_sent_time);
}

struct mbuf *
sctp_arethere_unrecognized_parameters(struct mbuf *in_initpkt,
    int param_offset, int *abort_processing, struct sctp_chunkhdr *cp)
{
	/*
	 * Given a mbuf containing an INIT or INIT-ACK with the param_offset
	 * being equal to the beginning of the params i.e. (iphlen +
	 * sizeof(struct sctp_init_msg) parse through the parameters to the
	 * end of the mbuf verifying that all parameters are known.
	 * 
	 * For unknown parameters build and return a mbuf with
	 * UNRECOGNIZED_PARAMETER errors. If the flags indicate to stop
	 * processing this chunk stop, and set *abort_processing to 1.
	 * 
	 * By having param_offset be pre-set to where parameters begin it is
	 * hoped that this routine may be reused in the future by new
	 * features.
	 */
	struct sctp_paramhdr *phdr, params;

	struct mbuf *mat, *op_err;
	char tempbuf[2048];
	int at, limit, pad_needed;
	uint16_t ptype, plen;
	int err_at;

	*abort_processing = 0;
	mat = in_initpkt;
	err_at = 0;
	limit = ntohs(cp->chunk_length) - sizeof(struct sctp_init_chunk);
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("Limit is %d bytes\n", limit);
	}
#endif
	at = param_offset;
	op_err = NULL;

	phdr = sctp_get_next_param(mat, at, &params, sizeof(params));
	while ((phdr != NULL) && ((size_t)limit >= sizeof(struct sctp_paramhdr))) {
		ptype = ntohs(phdr->param_type);
		plen = ntohs(phdr->param_length);
		limit -= SCTP_SIZE32(plen);
		if (plen < sizeof(struct sctp_paramhdr)) {
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("sctp_output.c:Impossible length in parameter < %d\n", plen);
			}
#endif
			*abort_processing = 1;
			break;
		}
		/*
		 * All parameters for all chunks that we know/understand are
		 * listed here. We process them other places and make
		 * appropriate stop actions per the upper bits. However this
		 * is the generic routine processor's can call to get back
		 * an operr.. to either incorporate (init-ack) or send.
		 */
		if ((ptype == SCTP_HEARTBEAT_INFO) ||
		    (ptype == SCTP_IPV4_ADDRESS) ||
		    (ptype == SCTP_IPV6_ADDRESS) ||
		    (ptype == SCTP_STATE_COOKIE) ||
		    (ptype == SCTP_UNRECOG_PARAM) ||
		    (ptype == SCTP_COOKIE_PRESERVE) ||
		    (ptype == SCTP_SUPPORTED_ADDRTYPE) ||
		    (ptype == SCTP_PRSCTP_SUPPORTED) ||
		    (ptype == SCTP_ADD_IP_ADDRESS) ||
		    (ptype == SCTP_DEL_IP_ADDRESS) ||
		    (ptype == SCTP_ECN_CAPABLE) ||
		    (ptype == SCTP_ULP_ADAPTATION) ||
		    (ptype == SCTP_ERROR_CAUSE_IND) ||
		    (ptype == SCTP_SET_PRIM_ADDR) ||
		    (ptype == SCTP_SUCCESS_REPORT) ||
		    (ptype == SCTP_ULP_ADAPTATION) ||
		    (ptype == SCTP_SUPPORTED_CHUNK_EXT) ||
		    (ptype == SCTP_ECN_NONCE_SUPPORTED)
		    ) {
			/* no skip it */
			at += SCTP_SIZE32(plen);
		} else if (ptype == SCTP_HOSTNAME_ADDRESS) {
			/* We can NOT handle HOST NAME addresses!! */
			int l_len;
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("Can't handle hostname addresses.. abort processing\n");
			}
#endif
			*abort_processing = 1;
			if (op_err == NULL) {
				/* Ok need to try to get a mbuf */
				l_len = sizeof(struct ip6_hdr) + sizeof(struct sctphdr) + sizeof(struct sctp_chunkhdr);
				l_len += plen;
				l_len += sizeof(struct sctp_paramhdr);
				op_err = sctp_get_mbuf_for_msg(l_len, 1, M_DONTWAIT, 1, MT_DATA);
				if (op_err) {
					op_err->m_len = 0;
					op_err->m_pkthdr.len = 0;
					/*
					 * pre-reserve space for ip and sctp
					 * header  and chunk hdr
					 */
					op_err->m_data += sizeof(struct ip6_hdr);
					op_err->m_data += sizeof(struct sctphdr);
					op_err->m_data += sizeof(struct sctp_chunkhdr);
				}
			}
			if (op_err) {
				/* If we have space */
				struct sctp_paramhdr s;

				if (err_at % 4) {
					uint32_t cpthis = 0;

					pad_needed = 4 - (err_at % 4);
					m_copyback(op_err, err_at, pad_needed, (caddr_t)&cpthis);
					err_at += pad_needed;
				}
				s.param_type = htons(SCTP_CAUSE_UNRESOLVABLE_ADDR);
				s.param_length = htons(sizeof(s) + plen);
				m_copyback(op_err, err_at, sizeof(s), (caddr_t)&s);
				err_at += sizeof(s);
				phdr = sctp_get_next_param(mat, at, (struct sctp_paramhdr *)tempbuf, plen);
				if (phdr == NULL) {
					sctp_m_freem(op_err);
					/*
					 * we are out of memory but we still
					 * need to have a look at what to do
					 * (the system is in trouble
					 * though).
					 */
					return (NULL);
				}
				m_copyback(op_err, err_at, plen, (caddr_t)phdr);
				err_at += plen;
			}
			return (op_err);
		} else {
			/*
			 * we do not recognize the parameter figure out what
			 * we do.
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("Got parameter type %x - unknown\n",
				    (uint32_t) ptype);
			}
#endif
			if ((ptype & 0x4000) == 0x4000) {
				/* Report bit is set?? */
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
					printf("Report bit is set\n");
				}
#endif
				if (op_err == NULL) {
					int l_len;
					/* Ok need to try to get an mbuf */
					l_len = sizeof(struct ip6_hdr) + sizeof(struct sctphdr) + sizeof(struct sctp_chunkhdr);
					l_len += plen;
					l_len += sizeof(struct sctp_paramhdr);
					op_err = sctp_get_mbuf_for_msg(l_len, 1, M_DONTWAIT, 1, MT_DATA);
					if (op_err) {
						op_err->m_len = 0;
						op_err->m_pkthdr.len = 0;
						op_err->m_data += sizeof(struct ip6_hdr);
						op_err->m_data += sizeof(struct sctphdr);
						op_err->m_data += sizeof(struct sctp_chunkhdr);
					}
				}
				if (op_err) {
					/* If we have space */
					struct sctp_paramhdr s;

					if (err_at % 4) {
						uint32_t cpthis = 0;

						pad_needed = 4 - (err_at % 4);
						m_copyback(op_err, err_at, pad_needed, (caddr_t)&cpthis);
						err_at += pad_needed;
					}
					s.param_type = htons(SCTP_UNRECOG_PARAM);
					s.param_length = htons(sizeof(s) + plen);
					m_copyback(op_err, err_at, sizeof(s), (caddr_t)&s);
					err_at += sizeof(s);
					if (plen > sizeof(tempbuf)) {
						plen = sizeof(tempbuf);
					}
					phdr = sctp_get_next_param(mat, at, (struct sctp_paramhdr *)tempbuf, plen);
					if (phdr == NULL) {
						sctp_m_freem(op_err);
						/*
						 * we are out of memory but
						 * we still need to have a
						 * look at what to do (the
						 * system is in trouble
						 * though).
						 */
						goto more_processing;
					}
					m_copyback(op_err, err_at, plen, (caddr_t)phdr);
					err_at += plen;
				}
			}
	more_processing:
			if ((ptype & 0x8000) == 0x0000) {
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
					printf("Abort bit is now setting1\n");
				}
#endif
				return (op_err);
			} else {
				/* skip this chunk and continue processing */
				at += SCTP_SIZE32(plen);
			}

		}
		phdr = sctp_get_next_param(mat, at, &params, sizeof(params));
	}
	return (op_err);
}

static int
sctp_are_there_new_addresses(struct sctp_association *asoc,
    struct mbuf *in_initpkt, int iphlen, int offset)
{
	/*
	 * Given a INIT packet, look through the packet to verify that there
	 * are NO new addresses. As we go through the parameters add reports
	 * of any un-understood parameters that require an error.  Also we
	 * must return (1) to drop the packet if we see a un-understood
	 * parameter that tells us to drop the chunk.
	 */
	struct sockaddr_in sin4, *sa4;
	struct sockaddr_in6 sin6, *sa6;
	struct sockaddr *sa_touse;
	struct sockaddr *sa;
	struct sctp_paramhdr *phdr, params;
	struct ip *iph;
	struct mbuf *mat;
	uint16_t ptype, plen;
	int err_at;
	uint8_t fnd;
	struct sctp_nets *net;

	memset(&sin4, 0, sizeof(sin4));
	memset(&sin6, 0, sizeof(sin6));
	sin4.sin_family = AF_INET;
	sin4.sin_len = sizeof(sin4);
	sin6.sin6_family = AF_INET6;
	sin6.sin6_len = sizeof(sin6);

	sa_touse = NULL;
	/* First what about the src address of the pkt ? */
	iph = mtod(in_initpkt, struct ip *);
	if (iph->ip_v == IPVERSION) {
		/* source addr is IPv4 */
		sin4.sin_addr = iph->ip_src;
		sa_touse = (struct sockaddr *)&sin4;
	} else if (iph->ip_v == (IPV6_VERSION >> 4)) {
		/* source addr is IPv6 */
		struct ip6_hdr *ip6h;

		ip6h = mtod(in_initpkt, struct ip6_hdr *);
		sin6.sin6_addr = ip6h->ip6_src;
		sa_touse = (struct sockaddr *)&sin6;
	} else {
		return (1);
	}

	fnd = 0;
	TAILQ_FOREACH(net, &asoc->nets, sctp_next) {
		sa = (struct sockaddr *)&net->ro._l_addr;
		if (sa->sa_family == sa_touse->sa_family) {
			if (sa->sa_family == AF_INET) {
				sa4 = (struct sockaddr_in *)sa;
				if (sa4->sin_addr.s_addr ==
				    sin4.sin_addr.s_addr) {
					fnd = 1;
					break;
				}
			} else if (sa->sa_family == AF_INET6) {
				sa6 = (struct sockaddr_in6 *)sa;
				if (SCTP6_ARE_ADDR_EQUAL(&sa6->sin6_addr,
				    &sin6.sin6_addr)) {
					fnd = 1;
					break;
				}
			}
		}
	}
	if (fnd == 0) {
		/* New address added! no need to look futher. */
		return (1);
	}
	/* Ok so far lets munge through the rest of the packet */
	mat = in_initpkt;
	err_at = 0;
	sa_touse = NULL;
	offset += sizeof(struct sctp_init_chunk);
	phdr = sctp_get_next_param(mat, offset, &params, sizeof(params));
	while (phdr) {
		ptype = ntohs(phdr->param_type);
		plen = ntohs(phdr->param_length);
		if (ptype == SCTP_IPV4_ADDRESS) {
			struct sctp_ipv4addr_param *p4, p4_buf;

			phdr = sctp_get_next_param(mat, offset,
			    (struct sctp_paramhdr *)&p4_buf, sizeof(p4_buf));
			if (plen != sizeof(struct sctp_ipv4addr_param) ||
			    phdr == NULL) {
				return (1);
			}
			p4 = (struct sctp_ipv4addr_param *)phdr;
			sin4.sin_addr.s_addr = p4->addr;
			sa_touse = (struct sockaddr *)&sin4;
		} else if (ptype == SCTP_IPV6_ADDRESS) {
			struct sctp_ipv6addr_param *p6, p6_buf;

			phdr = sctp_get_next_param(mat, offset,
			    (struct sctp_paramhdr *)&p6_buf, sizeof(p6_buf));
			if (plen != sizeof(struct sctp_ipv6addr_param) ||
			    phdr == NULL) {
				return (1);
			}
			p6 = (struct sctp_ipv6addr_param *)phdr;
			memcpy((caddr_t)&sin6.sin6_addr, p6->addr,
			    sizeof(p6->addr));
			sa_touse = (struct sockaddr *)&sin4;
		}
		if (sa_touse) {
			/* ok, sa_touse points to one to check */
			fnd = 0;
			TAILQ_FOREACH(net, &asoc->nets, sctp_next) {
				sa = (struct sockaddr *)&net->ro._l_addr;
				if (sa->sa_family != sa_touse->sa_family) {
					continue;
				}
				if (sa->sa_family == AF_INET) {
					sa4 = (struct sockaddr_in *)sa;
					if (sa4->sin_addr.s_addr ==
					    sin4.sin_addr.s_addr) {
						fnd = 1;
						break;
					}
				} else if (sa->sa_family == AF_INET6) {
					sa6 = (struct sockaddr_in6 *)sa;
					if (SCTP6_ARE_ADDR_EQUAL(
					    &sa6->sin6_addr, &sin6.sin6_addr)) {
						fnd = 1;
						break;
					}
				}
			}
			if (!fnd) {
				/* New addr added! no need to look further */
				return (1);
			}
		}
		offset += SCTP_SIZE32(plen);
		phdr = sctp_get_next_param(mat, offset, &params, sizeof(params));
	}
	return (0);
}

/*
 * Given a MBUF chain that was sent into us containing an INIT. Build a
 * INIT-ACK with COOKIE and send back. We assume that the in_initpkt has done
 * a pullup to include IPv6/4header, SCTP header and initial part of INIT
 * message (i.e. the struct sctp_init_msg).
 */
void
sctp_send_initiate_ack(struct sctp_inpcb *inp, struct sctp_tcb *stcb,
    struct mbuf *init_pkt, int iphlen, int offset, struct sctphdr *sh,
    struct sctp_init_chunk *init_chk)
{
	struct sctp_association *asoc;
	struct mbuf *m, *m_at, *m_tmp, *m_cookie, *op_err, *m_last;
	struct sctp_init_msg *initackm_out;
	struct sctp_ecn_supported_param *ecn;
	struct sctp_prsctp_supported_param *prsctp;
	struct sctp_ecn_nonce_supported_param *ecn_nonce;
	struct sctp_supported_chunk_types_param *pr_supported;
	struct sockaddr_storage store;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	struct route *ro;
	struct ip *iph;
	struct ip6_hdr *ip6;
	struct sockaddr *to;
	struct sctp_state_cookie stc;
	struct sctp_nets *net = NULL;
	int cnt_inits_to = 0;
	uint16_t his_limit, i_want;
	int abort_flag, padval, sz_of;
	int num_ext;
	int p_len;

	if (stcb) {
		asoc = &stcb->asoc;
	} else {
		asoc = NULL;
	}
	m_last = NULL;
	if ((asoc != NULL) &&
	    (SCTP_GET_STATE(asoc) != SCTP_STATE_COOKIE_WAIT) &&
	    (sctp_are_there_new_addresses(asoc, init_pkt, iphlen, offset))) {
		/* new addresses, out of here in non-cookie-wait states */
		/*
		 * Send a ABORT, we don't add the new address error clause
		 * though we even set the T bit and copy in the 0 tag.. this
		 * looks no different than if no listener was present.
		 */
		sctp_send_abort(init_pkt, iphlen, sh, 0, NULL);
		return;
	}
	abort_flag = 0;
	op_err = sctp_arethere_unrecognized_parameters(init_pkt,
	    (offset + sizeof(struct sctp_init_chunk)),
	    &abort_flag, (struct sctp_chunkhdr *)init_chk);
	if (abort_flag) {
		sctp_send_abort(init_pkt, iphlen, sh, init_chk->init.initiate_tag, op_err);
		return;
	}
	m = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if (m == NULL) {
		/* No memory, INIT timer will re-attempt. */
		if (op_err)
			sctp_m_freem(op_err);
		return;
	}
	m->m_data += SCTP_MIN_OVERHEAD;
	m->m_pkthdr.rcvif = 0;
	m->m_len = sizeof(struct sctp_init_msg);

	/* the time I built cookie */
	SCTP_GETTIME_TIMEVAL(&stc.time_entered);

	/* populate any tie tags */
	if (asoc != NULL) {
		/* unlock before tag selections */
		SCTP_TCB_UNLOCK(stcb);
		if (asoc->my_vtag_nonce == 0)
			asoc->my_vtag_nonce = sctp_select_a_tag(inp);
		stc.tie_tag_my_vtag = asoc->my_vtag_nonce;

		if (asoc->peer_vtag_nonce == 0)
			asoc->peer_vtag_nonce = sctp_select_a_tag(inp);
		stc.tie_tag_peer_vtag = asoc->peer_vtag_nonce;

		stc.cookie_life = asoc->cookie_life;
		net = asoc->primary_destination;
		/* now we must relock */
		SCTP_INP_RLOCK(inp);
		/*
		 * we may be in trouble here if the inp got freed most
		 * likely this set of tests will protect us but there is a
		 * chance not.
		 */
		if (inp->sctp_flags & (SCTP_PCB_FLAGS_SOCKET_GONE |
				       SCTP_PCB_FLAGS_SOCKET_ALLGONE)) {
			if (op_err)
				sctp_m_freem(op_err);
			sctp_m_freem(m);
			sctp_send_abort(init_pkt, iphlen, sh, 0, NULL);
			return;
		}
		SCTP_TCB_LOCK(stcb);
		SCTP_INP_RUNLOCK(stcb->sctp_ep);
	} else {
		stc.tie_tag_my_vtag = 0;
		stc.tie_tag_peer_vtag = 0;
		/* life I will award this cookie */
		stc.cookie_life = inp->sctp_ep.def_cookie_life;
	}

	/* copy in the ports for later check */
	stc.myport = sh->dest_port;
	stc.peerport = sh->src_port;

	/*
	 * If we wanted to honor cookie life extentions, we would add to
	 * stc.cookie_life. For now we should NOT honor any extension
	 */
	stc.site_scope = stc.local_scope = stc.loopback_scope = 0;
	if (inp->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
		struct inpcb *in_inp;

		/* Its a V6 socket */
		in_inp = (struct inpcb *)inp;
		stc.ipv6_addr_legal = 1;
		/* Now look at the binding flag to see if V4 will be legal */
		if (
#if defined(__FreeBSD__) || defined(__APPLE__)
		    (in_inp->inp_flags & IN6P_IPV6_V6ONLY)
#elif defined(__OpenBSD__)
		    (0)		/* For openbsd we do dual bind only */
#else
		    (((struct in6pcb *)in_inp)->in6p_flags & IN6P_IPV6_V6ONLY)
#endif
		    == 0) {
			stc.ipv4_addr_legal = 1;
		} else {
			/* V4 addresses are NOT legal on the association */
			stc.ipv4_addr_legal = 0;
		}
	} else {
		/* Its a V4 socket, no - V6 */
		stc.ipv4_addr_legal = 1;
		stc.ipv6_addr_legal = 0;
	}

#ifdef SCTP_DONT_DO_PRIVADDR_SCOPE
	stc.ipv4_scope = 1;
#else
	stc.ipv4_scope = 0;
#endif
	/* now for scope setup */
	memset((caddr_t)&store, 0, sizeof(store));
	sin = (struct sockaddr_in *)&store;
	sin6 = (struct sockaddr_in6 *)&store;
	if (net == NULL) {
		to = (struct sockaddr *)&store;
		iph = mtod(init_pkt, struct ip *);
		if (iph->ip_v == IPVERSION) {
			struct in_addr addr;
			struct route iproute;

			sin->sin_family = AF_INET;
			sin->sin_len = sizeof(struct sockaddr_in);
			sin->sin_port = sh->src_port;
			sin->sin_addr = iph->ip_src;
			/* lookup address */
			stc.address[0] = sin->sin_addr.s_addr;
			stc.address[1] = 0;
			stc.address[2] = 0;
			stc.address[3] = 0;
			stc.addr_type = SCTP_IPV4_ADDRESS;
			/* local from address */
			memset(&iproute, 0, sizeof(iproute));
			ro = &iproute;
			memcpy(&ro->ro_dst, sin, sizeof(*sin));
			addr = sctp_ipv4_source_address_selection(inp, NULL,
			    ro, NULL, 0);
			if (ro->ro_rt) {
				RTFREE(ro->ro_rt);
			}
			stc.laddress[0] = addr.s_addr;
			stc.laddress[1] = 0;
			stc.laddress[2] = 0;
			stc.laddress[3] = 0;
			stc.laddr_type = SCTP_IPV4_ADDRESS;
			/* scope_id is only for v6 */
			stc.scope_id = 0;
#ifndef SCTP_DONT_DO_PRIVADDR_SCOPE
			if (IN4_ISPRIVATE_ADDRESS(&sin->sin_addr)) {
				stc.ipv4_scope = 1;
			}
#else
			stc.ipv4_scope = 1;
#endif				/* SCTP_DONT_DO_PRIVADDR_SCOPE */
			/* Must use the address in this case */
			if (sctp_is_address_on_local_host((struct sockaddr *)sin)) {
				stc.loopback_scope = 1;
				stc.ipv4_scope = 1;
				stc.site_scope = 1;
				stc.local_scope = 1;
			}
		} else if (iph->ip_v == (IPV6_VERSION >> 4)) {
			struct in6_addr addr;

#ifdef NEW_STRUCT_ROUTE
			struct route iproute6;

#else
			struct route_in6 iproute6;

#endif
			ip6 = mtod(init_pkt, struct ip6_hdr *);
			sin6->sin6_family = AF_INET6;
			sin6->sin6_len = sizeof(struct sockaddr_in6);
			sin6->sin6_port = sh->src_port;
			sin6->sin6_addr = ip6->ip6_src;
			/* lookup address */
			memcpy(&stc.address, &sin6->sin6_addr,
			    sizeof(struct in6_addr));
			sin6->sin6_scope_id = 0;
			stc.addr_type = SCTP_IPV6_ADDRESS;
			stc.scope_id = 0;
			if (sctp_is_address_on_local_host((struct sockaddr *)sin6)) {
				stc.loopback_scope = 1;
				stc.local_scope = 1;
				stc.site_scope = 1;
				stc.ipv4_scope = 1;
			} else if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
				/*
				 * If the new destination is a LINK_LOCAL we
				 * must have common both site and local
				 * scope. Don't set local scope though since
				 * we must depend on the source to be added
				 * implicitly. We cannot assure just because
				 * we share one link that all links are
				 * common.
				 */
				stc.local_scope = 0;
				stc.site_scope = 1;
				stc.ipv4_scope = 1;
				/*
				 * we start counting for the private address
				 * stuff at 1. since the link local we
				 * source from won't show up in our scoped
				 * count.
				 */
				cnt_inits_to = 1;
				/* pull out the scope_id from incoming pkt */
#ifdef SCTP_KAME
				/* FIX ME: does this have scope from rcvif? */
				(void)sa6_recoverscope(sin6);
#else
				(void)in6_recoverscope(sin6, &ip6->ip6_src,
				    init_pkt->m_pkthdr.rcvif);
#endif				/* SCTP_KAME */
#if defined(SCTP_BASE_FREEBSD) || defined(__APPLE__)
				in6_embedscope(&sin6->sin6_addr, sin6, NULL,
				    NULL);
#else
#ifdef SCTP_KAME
				sa6_embedscope(sin6, ip6_use_defzone);
#else
				in6_embedscope(&sin6->sin6_addr, sin6);
#endif				/* SCTP_KAME */
#endif
				stc.scope_id = sin6->sin6_scope_id;
			} else if (IN6_IS_ADDR_SITELOCAL(&sin6->sin6_addr)) {
				/*
				 * If the new destination is SITE_LOCAL then
				 * we must have site scope in common.
				 */
				stc.site_scope = 1;
			}
			/* local from address */
			memset(&iproute6, 0, sizeof(iproute6));
			ro = (struct route *)&iproute6;
			memcpy(&ro->ro_dst, sin6, sizeof(*sin6));
			addr = sctp_ipv6_source_address_selection(inp, NULL,
			    ro, NULL, 0);
			if (ro->ro_rt) {
				RTFREE(ro->ro_rt);
			}
			memcpy(&stc.laddress, &addr, sizeof(struct in6_addr));
			stc.laddr_type = SCTP_IPV6_ADDRESS;
		}
	} else {
		/* set the scope per the existing tcb */
		struct sctp_nets *lnet;

		stc.loopback_scope = asoc->loopback_scope;
		stc.ipv4_scope = asoc->ipv4_local_scope;
		stc.site_scope = asoc->site_scope;
		stc.local_scope = asoc->local_scope;
		TAILQ_FOREACH(lnet, &asoc->nets, sctp_next) {
			if (lnet->ro._l_addr.sin6.sin6_family == AF_INET6) {
				if (IN6_IS_ADDR_LINKLOCAL(&lnet->ro._l_addr.sin6.sin6_addr)) {
					/*
					 * if we have a LL address, start
					 * counting at 1.
					 */
					cnt_inits_to = 1;
				}
			}
		}

		/* use the net pointer */
		to = (struct sockaddr *)&net->ro._l_addr;
		if (to->sa_family == AF_INET) {
			sin = (struct sockaddr_in *)to;
			stc.address[0] = sin->sin_addr.s_addr;
			stc.address[1] = 0;
			stc.address[2] = 0;
			stc.address[3] = 0;
			stc.addr_type = SCTP_IPV4_ADDRESS;
			if (net->src_addr_selected == 0) {
				/*
				 * strange case here, the INIT should have
				 * did the selection.
				 */
				net->ro._s_addr.sin.sin_addr =
				    sctp_ipv4_source_address_selection(inp,
				    stcb, (struct route *)&net->ro, net, 0);
				net->src_addr_selected = 1;

			}
			stc.laddress[0] = net->ro._s_addr.sin.sin_addr.s_addr;
			stc.laddress[1] = 0;
			stc.laddress[2] = 0;
			stc.laddress[3] = 0;
			stc.laddr_type = SCTP_IPV4_ADDRESS;
		} else if (to->sa_family == AF_INET6) {
			sin6 = (struct sockaddr_in6 *)to;
			memcpy(&stc.address, &sin6->sin6_addr,
			    sizeof(struct in6_addr));
			stc.addr_type = SCTP_IPV6_ADDRESS;
			if (net->src_addr_selected == 0) {
				/*
				 * strange case here, the INIT should have
				 * did the selection.
				 */
				net->ro._s_addr.sin6.sin6_addr =
				    sctp_ipv6_source_address_selection(inp,
				    stcb, (struct route *)&net->ro, net, 0);
				net->src_addr_selected = 1;
			}
			memcpy(&stc.laddress, &net->ro._l_addr.sin6.sin6_addr,
			    sizeof(struct in6_addr));
			stc.laddr_type = SCTP_IPV6_ADDRESS;
		}
	}
	/* Now lets put the SCTP header in place */
	initackm_out = mtod(m, struct sctp_init_msg *);
	initackm_out->sh.src_port = inp->sctp_lport;
	initackm_out->sh.dest_port = sh->src_port;
	initackm_out->sh.v_tag = init_chk->init.initiate_tag;
	/* Save it off for quick ref */
	stc.peers_vtag = init_chk->init.initiate_tag;
	initackm_out->sh.checksum = 0;	/* calculate later */
	/* who are we */
	memcpy(stc.identification, SCTP_VERSION_STRING,
	    min(strlen(SCTP_VERSION_STRING), sizeof(stc.identification)));
	/* now the chunk header */
	initackm_out->msg.ch.chunk_type = SCTP_INITIATION_ACK;
	initackm_out->msg.ch.chunk_flags = 0;
	/* fill in later from mbuf we build */
	initackm_out->msg.ch.chunk_length = 0;
	/* place in my tag */
	if ((asoc != NULL) &&
	    ((SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_WAIT) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_ECHOED))) {
		/* re-use the v-tags and init-seq here */
		initackm_out->msg.init.initiate_tag = htonl(asoc->my_vtag);
		initackm_out->msg.init.initial_tsn = htonl(asoc->init_seq_number);
	} else {
		initackm_out->msg.init.initiate_tag = htonl(sctp_select_a_tag(inp));
		/* get a TSN to use too */
		initackm_out->msg.init.initial_tsn = htonl(sctp_select_initial_TSN(&inp->sctp_ep));
	}
	/* save away my tag to */
	stc.my_vtag = initackm_out->msg.init.initiate_tag;

	/* set up some of the credits. */
	initackm_out->msg.init.a_rwnd = htonl(max(inp->sctp_socket->so_rcv.sb_hiwat, SCTP_MINIMAL_RWND));
	/* set what I want */
	his_limit = ntohs(init_chk->init.num_inbound_streams);
	/* choose what I want */
	if (asoc != NULL) {
		if (asoc->streamoutcnt > inp->sctp_ep.pre_open_stream_count) {
			i_want = asoc->streamoutcnt;
		} else {
			i_want = inp->sctp_ep.pre_open_stream_count;
		}
	} else {
		i_want = inp->sctp_ep.pre_open_stream_count;
	}
	if (his_limit < i_want) {
		/* I Want more :< */
		initackm_out->msg.init.num_outbound_streams = init_chk->init.num_inbound_streams;
	} else {
		/* I can have what I want :> */
		initackm_out->msg.init.num_outbound_streams = htons(i_want);
	}
	/* tell him his limt. */
	initackm_out->msg.init.num_inbound_streams =
	    htons(inp->sctp_ep.max_open_streams_intome);
	/* setup the ECN pointer */

	if (inp->sctp_ep.adaptation_layer_indicator) {
		struct sctp_adaptation_layer_indication *ali;

		ali = (struct sctp_adaptation_layer_indication *)(
		    (caddr_t)initackm_out + sizeof(*initackm_out));
		ali->ph.param_type = htons(SCTP_ULP_ADAPTATION);
		ali->ph.param_length = htons(sizeof(*ali));
		ali->indication = ntohl(inp->sctp_ep.adaptation_layer_indicator);
		m->m_len += sizeof(*ali);
		ecn = (struct sctp_ecn_supported_param *)((caddr_t)ali +
		    sizeof(*ali));
	} else {
		ecn = (struct sctp_ecn_supported_param *)(
		    (caddr_t)initackm_out + sizeof(*initackm_out));
	}

	/* ECN parameter */
	if (sctp_ecn_enable == 1) {
		ecn->ph.param_type = htons(SCTP_ECN_CAPABLE);
		ecn->ph.param_length = htons(sizeof(*ecn));
		m->m_len += sizeof(*ecn);

		prsctp = (struct sctp_prsctp_supported_param *)((caddr_t)ecn +
		    sizeof(*ecn));
	} else {
		prsctp = (struct sctp_prsctp_supported_param *)((caddr_t)ecn);
	}
	/* And now tell the peer we do  pr-sctp */
	prsctp->ph.param_type = htons(SCTP_PRSCTP_SUPPORTED);
	prsctp->ph.param_length = htons(sizeof(*prsctp));
	m->m_len += sizeof(*prsctp);

	/* And now tell the peer we do all the extensions */
	pr_supported = (struct sctp_supported_chunk_types_param *)
	    ((caddr_t)prsctp + sizeof(*prsctp));

	pr_supported->ph.param_type = htons(SCTP_SUPPORTED_CHUNK_EXT);
	num_ext = 0;
	pr_supported->chunk_types[num_ext++] = SCTP_ASCONF;
	pr_supported->chunk_types[num_ext++] = SCTP_ASCONF_ACK;
	pr_supported->chunk_types[num_ext++] = SCTP_FORWARD_CUM_TSN;
	pr_supported->chunk_types[num_ext++] = SCTP_PACKET_DROPPED;
	pr_supported->chunk_types[num_ext++] = SCTP_STREAM_RESET;
	if (!sctp_auth_disable)
		pr_supported->chunk_types[num_ext++] = SCTP_AUTHENTICATION;
	p_len = sizeof(*pr_supported) + num_ext;
	pr_supported->ph.param_length = htons(p_len);
	bzero((caddr_t)pr_supported + p_len, SCTP_SIZE32(p_len) - p_len);
	m->m_len += SCTP_SIZE32(p_len);

	/* ECN nonce: And now tell the peer we support ECN nonce */
	if (sctp_ecn_nonce) {
		ecn_nonce = (struct sctp_ecn_nonce_supported_param *)
		    ((caddr_t)pr_supported + SCTP_SIZE32(p_len));
		ecn_nonce->ph.param_type = htons(SCTP_ECN_NONCE_SUPPORTED);
		ecn_nonce->ph.param_length = htons(sizeof(*ecn_nonce));
		m->m_len += sizeof(*ecn_nonce);
	}
	/* add authentication parameters */
	if (!sctp_auth_disable) {
		struct sctp_key *random_key;
		struct sctp_auth_random *random;
		struct sctp_auth_hmac_algo *hmacs;
		struct sctp_auth_chunk_list *chunks;

		/* generate and add RANDOM parameter */
		random_key = sctp_generate_random_key(sctp_auth_random_len);
		random = (struct sctp_auth_random *)(mtod(m, caddr_t)+m->m_len);
		random->ph.param_type = htons(SCTP_RANDOM);
		p_len = sizeof(*random) + random_key->keylen;
		random->ph.param_length = htons(p_len);
		bcopy(random_key->key, random->random_data, random_key->keylen);
		/* zero out any padding required */
		bzero((caddr_t)random + p_len, SCTP_SIZE32(p_len) - p_len);
		m->m_len += SCTP_SIZE32(p_len);
		sctp_free_key(random_key);

		/* add HMAC_ALGO parameter */
		hmacs = (struct sctp_auth_hmac_algo *)(mtod(m, caddr_t)+m->m_len);
		p_len = sctp_serialize_hmaclist(inp->sctp_ep.local_hmacs,
		    (uint8_t *) hmacs->hmac_ids);
		if (p_len > 0) {
			p_len += sizeof(*hmacs);
			hmacs->ph.param_type = htons(SCTP_HMAC_LIST);
			hmacs->ph.param_length = htons(p_len);
			/* zero out any padding required */
			bzero((caddr_t)hmacs + p_len, SCTP_SIZE32(p_len) - p_len);
			m->m_len += SCTP_SIZE32(p_len);
		}
		/* add CHUNKS parameter */
		chunks = (struct sctp_auth_chunk_list *)(mtod(m, caddr_t)+m->m_len);
		p_len = sctp_serialize_auth_chunks(inp->sctp_ep.local_auth_chunks,
		    chunks->chunk_types);
		if (p_len > 0) {
			p_len += sizeof(*chunks);
			chunks->ph.param_type = htons(SCTP_CHUNK_LIST);
			chunks->ph.param_length = htons(p_len);
			/* zero out any padding required */
			bzero((caddr_t)chunks + p_len, SCTP_SIZE32(p_len) - p_len);
			m->m_len += SCTP_SIZE32(p_len);
		}
	}
	m_at = m;
	/* now the addresses */
	if (inp->sctp_flags & SCTP_PCB_FLAGS_BOUNDALL) {
		struct ifnet *ifn;
		struct ifaddr *ifa;
		int cnt = cnt_inits_to;

		TAILQ_FOREACH(ifn, &ifnet, if_list) {
			if ((stc.loopback_scope == 0) &&
			    (ifn->if_type == IFT_LOOP)) {
				/*
				 * Skip loopback devices if loopback_scope
				 * not set
				 */
				continue;
			}
			TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
				if (sctp_is_address_in_scope(ifa,
				    stc.ipv4_addr_legal, stc.ipv6_addr_legal,
				    stc.loopback_scope, stc.ipv4_scope,
				    stc.local_scope, stc.site_scope) == 0) {
					continue;
				}
				cnt++;
			}
		}
		if (cnt > 1) {
			TAILQ_FOREACH(ifn, &ifnet, if_list) {
				if ((stc.loopback_scope == 0) &&
				    (ifn->if_type == IFT_LOOP)) {
					/*
					 * Skip loopback devices if
					 * loopback_scope not set
					 */
					continue;
				}
				TAILQ_FOREACH(ifa, &ifn->if_addrlist, ifa_list) {
					if (sctp_is_address_in_scope(ifa,
					    stc.ipv4_addr_legal,
					    stc.ipv6_addr_legal,
					    stc.loopback_scope, stc.ipv4_scope,
					    stc.local_scope, stc.site_scope) == 0) {
						continue;
					}
					m_at = sctp_add_addr_to_mbuf(m_at, ifa);
				}
			}
		}
	} else {
		struct sctp_laddr *laddr;
		int cnt;

		cnt = cnt_inits_to;
		/* First, how many ? */
		LIST_FOREACH(laddr, &inp->sctp_addr_list, sctp_nxt_addr) {
			if (laddr->ifa == NULL) {
				continue;
			}
			if (laddr->ifa->ifa_addr == NULL)
				continue;
			if (sctp_is_address_in_scope(laddr->ifa,
			    stc.ipv4_addr_legal, stc.ipv6_addr_legal,
			    stc.loopback_scope, stc.ipv4_scope,
			    stc.local_scope, stc.site_scope) == 0) {
				continue;
			}
			cnt++;
		}
		/*
		 * If we bind a single address only we won't list any. This
		 * way you can get through a NAT
		 */
		if (cnt > 1) {
			LIST_FOREACH(laddr, &inp->sctp_addr_list, sctp_nxt_addr) {
				if (laddr->ifa == NULL) {
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
						printf("Help I have fallen and I can't get up!\n");
					}
#endif
					continue;
				}
				if (laddr->ifa->ifa_addr == NULL)
					continue;
				if (sctp_is_address_in_scope(laddr->ifa,
				    stc.ipv4_addr_legal, stc.ipv6_addr_legal,
				    stc.loopback_scope, stc.ipv4_scope,
				    stc.local_scope, stc.site_scope) == 0) {
					continue;
				}
				m_at = sctp_add_addr_to_mbuf(m_at, laddr->ifa);
			}
		}
	}

	/* tack on the operational error if present */
	if (op_err) {
		if (op_err->m_pkthdr.len % 4) {
			/* must add a pad to the param */
			uint32_t cpthis = 0;
			int padlen;

			padlen = 4 - (op_err->m_pkthdr.len % 4);
			m_copyback(op_err, op_err->m_pkthdr.len, padlen, (caddr_t)&cpthis);
		}
		while (m_at->m_next != NULL) {
			m_at = m_at->m_next;
		}
		m_at->m_next = op_err;
		while (m_at->m_next != NULL) {
			m_at = m_at->m_next;
		}
	}
	/* Get total size of init packet */
	sz_of = SCTP_SIZE32(ntohs(init_chk->ch.chunk_length));
	/* pre-calulate the size and update pkt header and chunk header */
	m->m_pkthdr.len = 0;
	for (m_tmp = m; m_tmp; m_tmp = m_tmp->m_next) {
		m->m_pkthdr.len += m_tmp->m_len;
		if (m_tmp->m_next == NULL) {
			/* m_tmp should now point to last one */
			break;
		}
	}
	/*
	 * Figure now the size of the cookie. We know the size of the
	 * INIT-ACK. The Cookie is going to be the size of INIT, INIT-ACK,
	 * COOKIE-STRUCTURE and SIGNATURE.
	 */

	/*
	 * take our earlier INIT calc and add in the sz we just calculated
	 * minus the size of the sctphdr (its not included in chunk size
	 */

	/* add once for the INIT-ACK */
	sz_of += (m->m_pkthdr.len - sizeof(struct sctphdr));

	/* add a second time for the INIT-ACK in the cookie */
	sz_of += (m->m_pkthdr.len - sizeof(struct sctphdr));

	/* Now add the cookie header and cookie message struct */
	sz_of += sizeof(struct sctp_state_cookie_param);
	/* ...and add the size of our signature */
	sz_of += SCTP_SIGNATURE_SIZE;
	initackm_out->msg.ch.chunk_length = htons(sz_of);

	/* Now we must build a cookie */
	m_cookie = sctp_add_cookie(inp, init_pkt, offset, m,
	    sizeof(struct sctphdr), &stc);
	if (m_cookie == NULL) {
		/* memory problem */
		sctp_m_freem(m);
		return;
	}
	/* Now append the cookie to the end and update the space/size */
	m_tmp->m_next = m_cookie;
	for (; m_tmp; m_tmp = m_tmp->m_next) {
		m->m_pkthdr.len += m_tmp->m_len;
		if (m_tmp->m_next == NULL) {
			/* m_tmp should now point to last one */
			m_last = m_tmp;
			break;
		}
	}

	/*
	 * We pass 0 here to NOT set IP_DF if its IPv4, we ignore the return
	 * here since the timer will drive a retranmission.
	 */
	padval = m->m_pkthdr.len % 4;
	if ((padval) && (m_last)) {
		/* see my previous comments on m_last */
		int ret;

		ret = sctp_add_pad_tombuf(m_last, (4 - padval));
		if (ret) {
			/* Houston we have a problem, no space */
			sctp_m_freem(m);
			return;
		}
		m->m_pkthdr.len += padval;
	}
	sctp_lowlevel_chunk_output(inp, NULL, NULL, to, m, 0, NULL, 0, 0,
	    NULL, 0);
}


void
sctp_insert_on_wheel(struct sctp_association *asoc,
    struct sctp_stream_out *strq)
{
	struct sctp_stream_out *stre, *strn;

	if ((strq->next_spoke.tqe_next) ||
	    (strq->next_spoke.tqe_prev)) {
		/* already on wheel */
		return;
	}
	stre = TAILQ_FIRST(&asoc->out_wheel);
	if (stre == NULL) {
		/* only one on wheel */
		TAILQ_INSERT_HEAD(&asoc->out_wheel, strq, next_spoke);
		return;
	}
	for (; stre; stre = strn) {
		strn = TAILQ_NEXT(stre, next_spoke);
		if (stre->stream_no > strq->stream_no) {
			TAILQ_INSERT_BEFORE(stre, strq, next_spoke);
			return;
		} else if (stre->stream_no == strq->stream_no) {
			/* huh, should not happen */
			return;
		} else if (strn == NULL) {
			/* next one is null */
			TAILQ_INSERT_AFTER(&asoc->out_wheel, stre, strq,
			    next_spoke);
		}
	}
}

static void
sctp_remove_from_wheel(struct sctp_association *asoc,
    struct sctp_stream_out *strq)
{
	/* take off and then setup so we know it is not on the wheel */
	TAILQ_REMOVE(&asoc->out_wheel, strq, next_spoke);
	strq->next_spoke.tqe_next = NULL;
	strq->next_spoke.tqe_prev = NULL;
}


static void
sctp_prune_prsctp(struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    struct sctp_sndrcvinfo *srcv,
    int dataout)
{
	int freed_spc = 0;
	struct sctp_tmit_chunk *chk, *nchk;

	SCTP_TCB_LOCK_ASSERT(stcb);
	if ((asoc->peer_supports_prsctp) &&
	    (asoc->sent_queue_cnt_removeable > 0)) {
		TAILQ_FOREACH(chk, &asoc->sent_queue, sctp_next) {
			/*
			 * Look for chunks marked with the PR_SCTP flag AND
			 * the buffer space flag. If the one being sent is
			 * equal or greater priority then purge the old one
			 * and free some space.
			 */
			if (PR_SCTP_BUF_ENABLED(chk->flags)) {
				/*
				 * This one is PR-SCTP AND buffer space
				 * limited type
				 */
				if (chk->rec.data.timetodrop.tv_sec >= (long)srcv->sinfo_timetolive) {
					/*
					 * Lower numbers equates to higher
					 * priority so if the one we are
					 * looking at has a larger or equal
					 * priority we want to drop the data
					 * and NOT retransmit it.
					 */
					if (chk->data) {
						/*
						 * We release the book_size
						 * if the mbuf is here
						 */
						int ret_spc;
						int cause;

						if (chk->sent > SCTP_DATAGRAM_UNSENT)
							cause = SCTP_RESPONSE_TO_USER_REQ | SCTP_NOTIFY_DATAGRAM_SENT;
						else
							cause = SCTP_RESPONSE_TO_USER_REQ | SCTP_NOTIFY_DATAGRAM_UNSENT;
						ret_spc = sctp_release_pr_sctp_chunk(stcb, chk,
						    cause,
						    &asoc->sent_queue);
						freed_spc += ret_spc;
						if (freed_spc >= dataout) {
							return;
						}
					}	/* if chunk was present */
				}	/* if of sufficent priority */
			}	/* if chunk has enabled */
		}		/* tailqforeach */

		chk = TAILQ_FIRST(&asoc->send_queue);
		while (chk) {
			nchk = TAILQ_NEXT(chk, sctp_next);
			/* Here we must move to the sent queue and mark */
			if (PR_SCTP_TTL_ENABLED(chk->flags)) {
				if (chk->rec.data.timetodrop.tv_sec >= (long)srcv->sinfo_timetolive) {
					if (chk->data) {
						/*
						 * We release the book_size
						 * if the mbuf is here
						 */
						int ret_spc;

						ret_spc = sctp_release_pr_sctp_chunk(stcb, chk,
						    SCTP_RESPONSE_TO_USER_REQ | SCTP_NOTIFY_DATAGRAM_UNSENT,
						    &asoc->send_queue);

						freed_spc += ret_spc;
						if (freed_spc >= dataout) {
							return;
						}
					}	/* end if chk->data */
				}	/* end if right class */
			}	/* end if chk pr-sctp */
			chk = nchk;
		}		/* end while (chk) */
	}			/* if enabled in asoc */
}

int
sctp_get_frag_point(struct sctp_tcb *stcb,
    struct sctp_association *asoc)
{
	int siz, ovh;

	/*
	 * For endpoints that have both v6 and v4 addresses we must reserve
	 * room for the ipv6 header, for those that are only dealing with V4
	 * we use a larger frag point.
	 */
	if (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
		ovh = SCTP_MED_OVERHEAD;
	} else {
		ovh = SCTP_MED_V4_OVERHEAD;
	}

	if (stcb->sctp_ep->sctp_frag_point > asoc->smallest_mtu)
		siz = asoc->smallest_mtu - ovh;
	else
		siz = (stcb->sctp_ep->sctp_frag_point - ovh);
	/*
	 * if (siz > (MCLBYTES-sizeof(struct sctp_data_chunk))) {
	 */
	/* A data chunk MUST fit in a cluster */
	/* siz = (MCLBYTES - sizeof(struct sctp_data_chunk)); */
	/* } */

	/* adjust for an AUTH chunk if DATA requires auth */
	if (sctp_auth_is_required_chunk(SCTP_DATA, stcb->asoc.peer_auth_chunks))
		siz -= sctp_get_auth_chunk_len(stcb->asoc.peer_hmac_id);

	if (siz % 4) {
		/* make it an even word boundary please */
		siz -= (siz % 4);
	}
	return (siz);
}
extern unsigned int sctp_max_chunks_on_queue;

static void 
sctp_set_prsctp_policy(struct sctp_tcb *stcb,
		       struct sctp_stream_queue_pending *sp)
{
	if (stcb->asoc.peer_supports_prsctp) {
		/*
		 * We assume that the user wants PR_SCTP_TTL if the user
		 * provides a positive lifetime but does not specify any
		 * PR_SCTP policy. This is a BAD assumption and causes
		 * problems at least with the U-Vancovers MPI folks. I will
		 * change this to be no policy means NO PR-SCTP.
		 */
		if ((sp->timetolive > 0) && (!PR_SCTP_ENABLED(sp->sinfo_flags))) {
			sp->act_flags |= CHUNK_FLAGS_PR_SCTP_TTL;
		} else {
			sp->act_flags |= PR_SCTP_POLICY(sp->sinfo_flags);
			goto sctp_no_policy;
		}
		switch (PR_SCTP_POLICY(sp->sinfo_flags)) {
		case CHUNK_FLAGS_PR_SCTP_BUF:
			/*
			 * Time to live is a priority stored in tv_sec when
			 * doing the buffer drop thing.
			 */
			sp->ts.tv_sec = sp->timetolive;
			sp->ts.tv_usec = 0;
			break;
		case CHUNK_FLAGS_PR_SCTP_TTL:
		{
			struct timeval tv;
			SCTP_GETTIME_TIMEVAL(&sp->ts);
			tv.tv_sec = sp->timetolive / 1000;
			tv.tv_usec = (sp->timetolive * 1000) % 1000000;
#ifndef __FreeBSD__
			timeradd(&sp->ts, &tv,
				 &sp->ts);
#else
			timevaladd(&sp->ts, &tv);
#endif
		}
			break;
		case CHUNK_FLAGS_PR_SCTP_RTX:
			/*
			 * Time to live is a the number or retransmissions
			 * stored in tv_sec.
			 */
			sp->ts.tv_sec = sp->timetolive;
			sp->ts.tv_usec = 0;
			break;
		default:
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_USRREQ1) {
				printf("Unknown PR_SCTP policy %u.\n", PR_SCTP_POLICY(sp->sinfo_sflags));
			}
#endif
			break;
		}
	}
 sctp_no_policy:
	if (sp->sinfo_flags & SCTP_UNORDERED)
		sp->act_flags |= SCTP_DATA_UNORDERED;

}


static int
sctp_msg_append(struct sctp_tcb *stcb,
		struct sctp_nets *net,
		struct mbuf *m,
		struct sctp_sndrcvinfo *srcv, int hold_stcb_lock)
{
	int error=0, holds_lock;
	struct mbuf *at;
	struct sctp_stream_queue_pending *sp = NULL;
	struct sctp_stream_out *strm;
#if defined(__NetBSD__) || defined(__OpenBSD__)
	int s;
	s = splsoftnet();
#endif

	/* Given an mbuf chain, put it
	 * into the association send queue and
	 * place it on the wheel 
	 */
	holds_lock = hold_stcb_lock;
	if (srcv->sinfo_stream >= stcb->asoc.streamoutcnt) {
		/* Invalid stream number */
		error = EINVAL;
		goto out_now;
	}
	if ((stcb->asoc.stream_locked) &&  
	    (stcb->asoc.stream_locked_on  != srcv->sinfo_stream)) {
		error = EAGAIN;
		goto out_now;
	}
	strm = &stcb->asoc.strmout[srcv->sinfo_stream];
	/* Now can we send this? */
	if ((SCTP_GET_STATE(&stcb->asoc) == SCTP_STATE_SHUTDOWN_SENT) ||
	    (SCTP_GET_STATE(&stcb->asoc) == SCTP_STATE_SHUTDOWN_ACK_SENT) ||
	    (SCTP_GET_STATE(&stcb->asoc) == SCTP_STATE_SHUTDOWN_RECEIVED) ||
	    (stcb->asoc.state & SCTP_STATE_SHUTDOWN_PENDING)) {
		/* got data while shutting down */
		error = ECONNRESET;
		goto out_now;
	}
	sp = (struct sctp_stream_queue_pending *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_strmoq);
	if(sp == NULL) {
		error = ENOMEM;
		goto out_now;
	}
	SCTP_INCR_STRMOQ_COUNT();
	sp->act_flags = 0;
	sp->sinfo_flags = srcv->sinfo_flags;
	sp->timetolive = srcv->sinfo_timetolive;
	sp->ppid = srcv->sinfo_ppid;
	sp->context = srcv->sinfo_context;
	sp->strseq = 0;
	if(sp->sinfo_flags & SCTP_ADDR_OVER) {
		sp->net = net;
		sp->addr_over = 1;
	} else {
		sp->net = stcb->asoc.primary_destination;
		sp->addr_over = 0;
	}
	atomic_add_int(&sp->net->ref_count, 1);
	SCTP_GETTIME_TIMEVAL(&sp->ts);
	sp->stream = srcv->sinfo_stream;
	sp->msg_is_complete = 1;
	sp->some_taken = 0;
	sp->data = m;
	sp->tail_mbuf = NULL;
	sp->length = 0;
	at = m;
	sctp_set_prsctp_policy(stcb, sp);
	while(at) {
		if(at->m_next == NULL)
			sp->tail_mbuf = at;
		sp->length += at->m_len;
		at = at->m_next;
	}
	if(sp->data->m_flags & M_PKTHDR) {
		sp->data->m_pkthdr.len = sp->length;
	} else {
		/* Get an HDR in front please */
		at = sctp_get_mbuf_for_msg(1, 1, M_DONTWAIT, 1, MT_DATA);
		if(at) {
			at->m_pkthdr.len = sp->length;
			at->m_len = 0;
			at->m_next = sp->data;
			sp->data = at;
		}
	}
	if(holds_lock == 0) {
		printf("Msg append gets a lock\n");
		SCTP_TCB_LOCK(stcb);
	}
	sctp_snd_sb_alloc(stcb, sp->length);
	stcb->asoc.stream_queue_cnt++;
	TAILQ_INSERT_TAIL(&strm->outqueue, sp, next);
	if ((srcv->sinfo_flags & SCTP_UNORDERED) == 0) {
		sp->strseq = strm->next_sequence_sent;
		strm->next_sequence_sent++;
	}

	if ((strm->next_spoke.tqe_next == NULL) &&
	    (strm->next_spoke.tqe_prev == NULL)) {
		/* Not on wheel, insert */
		sctp_insert_on_wheel(&stcb->asoc, strm);
	}
	m = NULL;
	if(hold_stcb_lock == 0) {
		printf("msg append frees the lock\n");
		SCTP_TCB_UNLOCK(stcb);
	}
out_now:
	if(m) {
		sctp_m_freem(m);
	}
#if defined(__NetBSD__) || defined(__OpenBSD__)
	splx(s);
#endif
	return (error);
}


static struct mbuf *
sctp_copy_mbufchain(struct mbuf *clonechain,
    struct mbuf *outchain,
    struct mbuf **endofchain,
    int insert_leading_mbuf_for_headers,
    int dont_clone)
{
	struct mbuf *m;
	struct mbuf *appendchain;

	if (dont_clone) {
		appendchain = clonechain;
	} else {
#if defined(__FreeBSD__) || defined(__NetBSD__)
		/*
		 * Supposedly m_copypacket is an optimization, use it if we
		 * can
		 */
		if (clonechain->m_flags & M_PKTHDR) {
			appendchain = m_copypacket(clonechain, M_DONTWAIT);
		} else
			appendchain = m_copy(clonechain, 0, M_COPYALL);
#elif defined(__APPLE__)
		appendchain = sctp_m_copym(clonechain, 0, M_COPYALL, M_DONTWAIT);
#else
		appendchain = m_copy(clonechain, 0, M_COPYALL);
#endif

		if (appendchain == NULL) {
			/* error */
			if (outchain)
				sctp_m_freem(outchain);
			return (NULL);
		}
	}
	/* if outchain is null, check our special reservation flag */
	if ((outchain == NULL) && (insert_leading_mbuf_for_headers)) {
		/*
		 * need a lead mbuf in this one if we don't have space for:
		 * - E-net header (12+2+2) - IP header (20/40) - SCTP Common
		 * Header (12)
		 */
		if (M_LEADINGSPACE(appendchain) < (SCTP_FIRST_MBUF_RESV)) {
			outchain = sctp_get_mbuf_for_msg(8, 1, M_DONTWAIT, 1, MT_HEADER);
			if (outchain) {
				/*
				 * if we don't hit here we have a problem
				 * anyway :o We reserve all the mbuf for
				 * prepends.
				 */
				outchain->m_data += (MHLEN - 8);
				outchain->m_pkthdr.len = 0;
				outchain->m_len = 0;
				outchain->m_next = NULL;
				if (endofchain) {
					*endofchain = outchain;
				}
			}
		}
	}
	if (outchain) {
		/* tack on to the end */
		if ((endofchain != NULL) &&
		    (*endofchain != NULL)) {
			(*endofchain)->m_next = appendchain;
		} else {
			m = outchain;
			while (m) {
				if (m->m_next == NULL) {
					m->m_next = appendchain;
					break;
				}
				m = m->m_next;
			}
		}
		if (outchain->m_flags & M_PKTHDR) {
			int append_tot;

			m = appendchain;
			append_tot = 0;
			while (m) {
				append_tot += m->m_len;
				if (endofchain && (m->m_next == NULL)) {
					*endofchain = m;
				}
				m = m->m_next;
			}
			outchain->m_pkthdr.len += append_tot;
		} else {
			if (endofchain) {
				/*
				 * save off the end and update the end-chain
				 * postion
				 */
				m = appendchain;
				while (m) {
					if (m->m_next == NULL) {
						*endofchain = m;
						break;
					}
					m = m->m_next;
				}
			}
		}
		return (outchain);
	} else {
		if (endofchain) {
			/* save off the end and update the end-chain postion */
			m = appendchain;
			while (m) {
				if (m->m_next == NULL) {
					*endofchain = m;
					break;
				}
				m = m->m_next;
			}
		}
		return (appendchain);
	}
}

int
sctp_med_chunk_output(struct sctp_inpcb *inp,
		      struct sctp_tcb *stcb,
		      struct sctp_association *asoc,
		      int *num_out,
		      int *reason_code,
		      int control_only, int *cwnd_full, int from_where,
		      struct timeval *now, int *now_filled);

static void
sctp_sendall_iterator(struct sctp_inpcb *inp, struct sctp_tcb *stcb, void *ptr,
    uint32_t val)
{
	struct sctp_copy_all *ca;
	struct mbuf *m;
	int ret=0;
	int added_control=0;
	int un_sent, do_chunk_output=1;
	struct sctp_association *asoc;
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	sctp_lock_assert(inp->ip_inp.inp.inp_socket);
#endif
	ca = (struct sctp_copy_all *)ptr;
	if (ca->m == NULL) {
		return;
	}
	if (ca->inp != inp) {
		/* TSNH */
		return;
	}
	if ((ca->m) && (ca->m->m_pkthdr.len)) {
		m = m_copym(ca->m, 0, M_COPYALL, M_DONTWAIT);
		if (m == NULL) {
			/* can't copy so we are done */
			ca->cnt_failed++;
			return;
		}
	} else {
		m = NULL;
	}
	SCTP_TCB_LOCK_ASSERT(stcb);
	if(ca->sndrcv.sinfo_flags & SCTP_ABORT) {
		/* Abort this assoc with m as the user defined reason */
		if (m) {
			struct sctp_paramhdr *ph;
			
			M_PREPEND(m, sizeof(struct sctp_paramhdr), M_DONTWAIT);
			if(m) {
				ph = mtod(m, struct sctp_paramhdr *);
				ph->param_type = htons(SCTP_CAUSE_USER_INITIATED_ABT);
				ph->param_length = htons(m->m_pkthdr.len);
			} 
			/* We add one here to keep the assoc from
			 * dis-appearing on us. 
			 */
			atomic_add_16(&stcb->asoc.refcnt, 1);
			sctp_abort_an_association(inp, stcb,
						  SCTP_RESPONSE_TO_USER_REQ,
						  m);
			/* sctp_abort_an_association calls sctp_free_asoc() 
			 * free association will NOT free it since we
			 * incremented the refcnt .. we do this to prevent
			 * it being freed and things getting tricky since
			 * we could end up (from free_asoc) calling inpcb_free
			 * which would get a recursive lock call to the
			 * iterator lock.. But as a consequence of that the
			 * stcb will return to us un-locked.. since free_asoc
			 * returns with either no TCB or the TCB unlocked, we
			 * must relock.. to unlock in the iterator timer :-0
			 */
			SCTP_TCB_LOCK(stcb);
			atomic_add_16(&stcb->asoc.refcnt, -1);
			goto no_chunk_output;
		}
	} else {
		if(m) {
			ret = sctp_msg_append(stcb, stcb->asoc.primary_destination, m,
					      &ca->sndrcv, 1);
		}
		asoc = &stcb->asoc;
		if(ca->sndrcv.sinfo_flags & SCTP_EOF) {
			/* shutdown this assoc */
			if (TAILQ_EMPTY(&asoc->send_queue) &&
			    TAILQ_EMPTY(&asoc->sent_queue) &&
			    (asoc->stream_queue_cnt == 0)) {
				if(asoc->locked_on_sending) {
					goto abort_anyway;
				}
				/* there is nothing queued to send, so I'm done... */
				if ((SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_SENT) &&
				    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_RECEIVED) &&
				    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_ACK_SENT)) {
					/* only send SHUTDOWN the first time through */
					sctp_send_shutdown(stcb, stcb->asoc.primary_destination);
					asoc->state = SCTP_STATE_SHUTDOWN_SENT;
					sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWN, stcb->sctp_ep, stcb,
							 asoc->primary_destination);
					sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWNGUARD, stcb->sctp_ep, stcb,
							 asoc->primary_destination);
					added_control = 1;
					do_chunk_output = 0;
				}
			} else {
				/*
				 * we still got (or just got) data to send, so set
				 * SHUTDOWN_PENDING
				 */
				/*
				 * XXX sockets draft says that SCTP_EOF should be
				 * sent with no data.  currently, we will allow user
				 * data to be sent first and move to
				 * SHUTDOWN-PENDING
				 */
				if ((SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_SENT) &&
				    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_RECEIVED) &&
				    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_ACK_SENT)) {
					if (asoc->locked_on_sending) {
						/* Locked to send out the data */
						struct sctp_stream_queue_pending *sp;
						sp = TAILQ_LAST(&asoc->locked_on_sending->outqueue, sctp_streamhead);
						if(sp) {
							if ((sp->length == 0) && (sp->msg_is_complete == 0))
								asoc->state |= SCTP_STATE_PARTIAL_MSG_LEFT;
						}
					}
					asoc->state |= SCTP_STATE_SHUTDOWN_PENDING;
					if (TAILQ_EMPTY(&asoc->send_queue) &&
					    TAILQ_EMPTY(&asoc->sent_queue) &&
					    (asoc->state & SCTP_STATE_PARTIAL_MSG_LEFT)) {
					abort_anyway:
						atomic_add_16(&stcb->asoc.refcnt, 1);
						sctp_abort_an_association(stcb->sctp_ep, stcb,
									  SCTP_RESPONSE_TO_USER_REQ,
									  NULL);
						atomic_add_16(&stcb->asoc.refcnt, -1);
						goto no_chunk_output;
					}
					sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWNGUARD, stcb->sctp_ep, stcb,
							 asoc->primary_destination);
				}
			}

		}
	}
	un_sent = ((stcb->asoc.total_output_queue_size - stcb->asoc.total_flight) +
		   ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) * sizeof(struct sctp_data_chunk)));

	if ((sctp_is_feature_off(inp, SCTP_PCB_FLAGS_NODELAY)) &&
	    (stcb->asoc.total_flight > 0) &&
	    (un_sent < (int)(stcb->asoc.smallest_mtu - SCTP_MIN_OVERHEAD)) &&
	    ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) < SCTP_MAX_DATA_BUNDLING)) {
		do_chunk_output = 0;
	}
	if(do_chunk_output)
	   sctp_chunk_output(inp, stcb, SCTP_OUTPUT_FROM_USR_SEND);
	else if(added_control) {
		int num_out=0, reason=0, cwnd_full=0, now_filled=0;
		struct timeval now;
		
		sctp_med_chunk_output(inp, stcb, &stcb->asoc, &num_out,
				      &reason, 1, &cwnd_full, 1, &now, &now_filled);
	}
 no_chunk_output:
	if (ret) {
		ca->cnt_failed++;
	} else {
		ca->cnt_sent++;
	}
}

static void
sctp_sendall_completes(void *ptr, uint32_t val)
{
	struct sctp_copy_all *ca;

	ca = (struct sctp_copy_all *)ptr;
	/*
	 * Do a notify here? Kacheong suggests that the notify be done at
	 * the send time.. so you would push up a notification if any send
	 * failed. Don't know if this is feasable since the only failures we
	 * have is "memory" related and if you cannot get an mbuf to send
	 * the data you surely can't get an mbuf to send up to notify the
	 * user you can't send the data :->
	 */

	/* now free everything */
	sctp_m_freem(ca->m);
	FREE(ca, M_PCB);
}


#define	MC_ALIGN(m, len) do {						\
	(m)->m_data += (MCLBYTES - (len)) & ~(sizeof(long) - 1);		\
} while (0)



static struct mbuf *
sctp_copy_out_all(struct uio *uio, int len)
{
	struct mbuf *ret, *at;
	int left, willcpy, cancpy, error;

	ret = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_WAIT, 1, MT_DATA);
	if (ret == NULL) {
		/* TSNH */
		return (NULL);
	}
	left = len;
	ret->m_len = 0;
	ret->m_pkthdr.len = len;
	/* save space for the data chunk header */
	cancpy = M_TRAILINGSPACE(ret);
	willcpy = min(cancpy, left);
	at = ret;
	while (left > 0) {
		/* Align data to the end */
		error = uiomove(mtod(at, caddr_t), willcpy, uio);
		if (error) {
	err_out_now:
			sctp_m_freem(at);
			return (NULL);
		}
		at->m_len = willcpy;
		at->m_nextpkt = at->m_next = 0;
		left -= willcpy;
		if (left > 0) {
			at->m_next = sctp_get_mbuf_for_msg(left, 0, M_WAIT, 1, MT_DATA);
			if (at->m_next == NULL) {
				goto err_out_now;
			}
			at = at->m_next;
			at->m_len = 0;
			cancpy = M_TRAILINGSPACE(at);
			willcpy = min(cancpy, left);
		}
	}
	return (ret);
}

static int
sctp_sendall(struct sctp_inpcb *inp, struct uio *uio, struct mbuf *m,
    struct sctp_sndrcvinfo *srcv)
{
	int ret;
	struct sctp_copy_all *ca;

	MALLOC(ca, struct sctp_copy_all *,
	    sizeof(struct sctp_copy_all), M_PCB, M_WAIT);
	if (ca == NULL) {
		sctp_m_freem(m);
		return (ENOMEM);
	}
	memset(ca, 0, sizeof(struct sctp_copy_all));

	ca->inp = inp;
	ca->sndrcv = *srcv;
	/*
	 * take off the sendall flag, it would be bad if we failed to do
	 * this :-0
	 */
	ca->sndrcv.sinfo_flags &= ~SCTP_SENDALL;
	/* get length and mbuf chain */
	if (uio) {
		ca->sndlen = uio->uio_resid;
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
		socket_unlock(inp->ip_inp.inp.inp_socket, 0);
#endif
		ca->m = sctp_copy_out_all(uio, ca->sndlen);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
		socket_lock(inp->ip_inp.inp.inp_socket, 0);
#endif
		if (ca->m == NULL) {
		out_no_mem:
			FREE(ca, M_PCB);
			return (ENOMEM);
		}
	} else {
		if ((m->m_flags & M_PKTHDR) == 0) {
			struct mbuf *mat;

			mat = m;
			ca->sndlen = 0;
			while (m) {
				ca->sndlen += m->m_len;
				m = m->m_next;
			}
			mat = sctp_get_mbuf_for_msg(1, 1, M_WAIT, 1, MT_DATA);
			if(mat) {
				sctp_m_freem(m);
				goto out_no_mem;
			}
			/* We MUST have a header on the front */
			mat->m_next = m;
			mat->m_len = 0;
			mat->m_pkthdr.len = ca->sndlen;
			ca->m = mat;
		} else {
			ca->sndlen = m->m_pkthdr.len;
		}
		ca->m = m;
	}
	ret = sctp_initiate_iterator(NULL, sctp_sendall_iterator,
	    SCTP_PCB_ANY_FLAGS, SCTP_PCB_ANY_FEATURES, SCTP_ASOC_ANY_STATE,
	    (void *)ca, 0,
	    sctp_sendall_completes, inp, 1);
	if (ret) {
#ifdef SCTP_DEBUG
		printf("Failed to initiate iterator for sendall\n");
#endif
		FREE(ca, M_PCB);
		return (EFAULT);
	}
	return (0);
}


void
sctp_toss_old_cookies(struct sctp_association *asoc)
{
	struct sctp_tmit_chunk *chk, *nchk;

	chk = TAILQ_FIRST(&asoc->control_send_queue);
	while (chk) {
		nchk = TAILQ_NEXT(chk, sctp_next);
		if (chk->rec.chunk_id == SCTP_COOKIE_ECHO) {
			TAILQ_REMOVE(&asoc->control_send_queue, chk, sctp_next);
			if (chk->data) {
				sctp_m_freem(chk->data);
				chk->data = NULL;
			}
			asoc->ctrl_queue_cnt--;
			if (chk->whoTo)
				sctp_free_remote_addr(chk->whoTo);
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
			SCTP_DECR_CHK_COUNT();
		}
		chk = nchk;
	}
}

void
sctp_toss_old_asconf(struct sctp_tcb *stcb)
{
	struct sctp_association *asoc;
	struct sctp_tmit_chunk *chk, *chk_tmp;

	asoc = &stcb->asoc;
	for (chk = TAILQ_FIRST(&asoc->control_send_queue); chk != NULL;
	    chk = chk_tmp) {
		/* get next chk */
		chk_tmp = TAILQ_NEXT(chk, sctp_next);
		/* find SCTP_ASCONF chunk in queue (only one ever in queue) */
		if (chk->rec.chunk_id == SCTP_ASCONF) {
			TAILQ_REMOVE(&asoc->control_send_queue, chk, sctp_next);
			if (chk->data) {
				sctp_m_freem(chk->data);
				chk->data = NULL;
			}
			asoc->ctrl_queue_cnt--;
			if (chk->whoTo)
				sctp_free_remote_addr(chk->whoTo);
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
			SCTP_DECR_CHK_COUNT();
		}
	}
}


static void
sctp_clean_up_datalist(struct sctp_tcb *stcb,

    struct sctp_association *asoc,
    struct sctp_tmit_chunk **data_list,
    int bundle_at,
    struct sctp_nets *net)
{
	int i;
	struct sctp_tmit_chunk *tp1;

	for (i = 0; i < bundle_at; i++) {
		/* off of the send queue */
		if (i) {
			/*
			 * Any chunk NOT 0 you zap the time chunk 0 gets
			 * zapped or set based on if a RTO measurment is
			 * needed.
			 */
			data_list[i]->do_rtt = 0;
		}
		/* record time */
		data_list[i]->sent_rcv_time = net->last_sent_time;
		data_list[i]->rec.data.fast_retran_tsn = data_list[i]->rec.data.TSN_seq;
		TAILQ_REMOVE(&asoc->send_queue,
		    data_list[i],
		    sctp_next);
		/* on to the sent queue */
		tp1 = TAILQ_LAST(&asoc->sent_queue, sctpchunk_listhead);
		if ((tp1) && (compare_with_wrap(tp1->rec.data.TSN_seq,
		    data_list[i]->rec.data.TSN_seq, MAX_TSN))) {
			struct sctp_tmit_chunk *tpp;

			/* need to move back */
	back_up_more:
			tpp = TAILQ_PREV(tp1, sctpchunk_listhead, sctp_next);
			if (tpp == NULL) {
				TAILQ_INSERT_BEFORE(tp1, data_list[i], sctp_next);
				goto all_done;
			}
			tp1 = tpp;
			if (compare_with_wrap(tp1->rec.data.TSN_seq,
			    data_list[i]->rec.data.TSN_seq, MAX_TSN)) {
				goto back_up_more;
			}
			TAILQ_INSERT_AFTER(&asoc->sent_queue, tp1, data_list[i], sctp_next);
		} else {
			TAILQ_INSERT_TAIL(&asoc->sent_queue,
			    data_list[i],
			    sctp_next);
		}
all_done:
		/* This does not lower until the cum-ack passes it */
		asoc->sent_queue_cnt++;
		asoc->send_queue_cnt--;
		if ((asoc->peers_rwnd <= 0) &&
		    (asoc->total_flight == 0) &&
		    (bundle_at == 1)) {
			/* Mark the chunk as being a window probe */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("WINDOW PROBE SET\n");
			}
#endif
			SCTP_STAT_INCR(sctps_windowprobed);
			data_list[i]->rec.data.state_flags |= SCTP_WINDOW_PROBE;
		} else {
			data_list[i]->rec.data.state_flags &= ~SCTP_WINDOW_PROBE;
		}
#ifdef SCTP_AUDITING_ENABLED
		sctp_audit_log(0xC2, 3);
#endif
		data_list[i]->sent = SCTP_DATAGRAM_SENT;
		data_list[i]->snd_count = 1;
		data_list[i]->rec.data.chunk_was_revoked = 0;
		net->flight_size += data_list[i]->book_size;
		asoc->total_flight += data_list[i]->book_size;
		asoc->total_flight_count++;
#ifdef SCTP_LOG_RWND
		sctp_log_rwnd(SCTP_DECREASE_PEER_RWND,
		    asoc->peers_rwnd, data_list[i]->send_size, sctp_peer_chunk_oh);
#endif
		asoc->peers_rwnd = sctp_sbspace_sub(asoc->peers_rwnd,
		    (uint32_t) (data_list[i]->send_size + sctp_peer_chunk_oh));
		if (asoc->peers_rwnd < stcb->sctp_ep->sctp_ep.sctp_sws_sender) {
			/* SWS sender side engages */
			asoc->peers_rwnd = 0;
		}
	}
}

static void
sctp_clean_up_ctl(struct sctp_association *asoc)
{
	struct sctp_tmit_chunk *chk, *nchk;

	for (chk = TAILQ_FIRST(&asoc->control_send_queue);
	    chk; chk = nchk) {
		nchk = TAILQ_NEXT(chk, sctp_next);
		if ((chk->rec.chunk_id == SCTP_SELECTIVE_ACK) ||
		    (chk->rec.chunk_id == SCTP_HEARTBEAT_REQUEST) ||
		    (chk->rec.chunk_id == SCTP_HEARTBEAT_ACK) ||
		    (chk->rec.chunk_id == SCTP_SHUTDOWN) ||
		    (chk->rec.chunk_id == SCTP_SHUTDOWN_ACK) ||
		    (chk->rec.chunk_id == SCTP_OPERATION_ERROR) ||
		    (chk->rec.chunk_id == SCTP_PACKET_DROPPED) ||
		    (chk->rec.chunk_id == SCTP_COOKIE_ACK) ||
		    (chk->rec.chunk_id == SCTP_ECN_CWR) ||
		    (chk->rec.chunk_id == SCTP_ASCONF_ACK)) {
			/* Stray chunks must be cleaned up */
	clean_up_anyway:
			TAILQ_REMOVE(&asoc->control_send_queue, chk, sctp_next);
			if (chk->data) {
				sctp_m_freem(chk->data);
				chk->data = NULL;
			}
			asoc->ctrl_queue_cnt--;
			sctp_free_remote_addr(chk->whoTo);
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
			SCTP_DECR_CHK_COUNT();
		} else if (chk->rec.chunk_id == SCTP_STREAM_RESET) {
			/* special handling, we must look into the param */
			if (chk != asoc->str_reset) {
				goto clean_up_anyway;
			}
		}
	}
}

extern int sctp_min_split_point;

static int
sctp_can_we_split_this(struct sctp_stream_queue_pending *sp,
		       int goal_mtu, int frag_point)
{
	/* Make a decision on if I should split a
	 * msg into multiple parts.
	 */
	if (goal_mtu >= sp->length) {
		/* Its not complete but
		 * we may want to take it if 
		 * it is big enough.
		 */
		if(sp->length > sctp_min_split_point) {
			return (sp->length);
		}
	}
	/* If we reach here sp->length is larger
	 * than the goal_mtu. Do we wish to split
	 * it for the sake of packet putting together?
	 */
	if (goal_mtu > sctp_min_split_point) {
		/* Its ok to split it */
		return(min(goal_mtu, frag_point));
	}
	/* Nope, can't split */
	return(0);

}


extern int sctp_mbuf_threshold_count;

static int sctp_mbuf_stats[10];

struct mbuf *
sctp_get_mbuf_for_msg(unsigned int space_needed, int want_header, 
		      int how, int allonebuf, int type)
{
	struct mbuf *m = NULL;
	int aloc_size;
	int index=0;
	int mbuf_threshold;
	if (want_header) {
		MGETHDR(m, how, type);
	} else {
		MGET(m, how, type);
	}
	if (m == NULL) {
		return (NULL);
	}
	if(allonebuf == 0)
		mbuf_threshold = sctp_mbuf_threshold_count;
	else
		mbuf_threshold = 1;


	if (space_needed > (((mbuf_threshold - 1) * MLEN) + MHLEN)) {
#if defined(__FreeBSD__) && __FreeBSD_version > 690000
	try_again:
		index = 4;
		if(space_needed <= MCLBYTES){ 
			aloc_size = MCLBYTES;
			sctp_mbuf_stats[0]++;
		} else if (space_needed <=  MJUMPAGESIZE) {
			aloc_size = MJUMPAGESIZE;
			sctp_mbuf_stats[1]++;
			index = 5;
		} else if (space_needed <= MJUM9BYTES) {
			aloc_size = MJUM9BYTES;
			sctp_mbuf_stats[2]++;
			index = 6;
		} else { 
			aloc_size = MJUM16BYTES;
			sctp_mbuf_stats[3]++;
			index = 7;
		}
		m_cljget(m, how, aloc_size);
		if (m == NULL) {
			return (NULL);
		}
		if ((m->m_flags & M_EXT) == 0) {
			if((aloc_size != MCLBYTES) &&
			   (allonebuf == 0)){
				aloc_size -= 10;
				sctp_mbuf_stats[index]++;
				goto try_again;
			}
			sctp_m_freem(m);
			return (NULL);
		} 
#else
		sctp_mbuf_stats[0]++;
		MCLGET(m, how);
		if (m == NULL) {
			return (NULL);
		}
		if ((m->m_flags & M_EXT) == 0) {
			sctp_m_freem(m);
			return (NULL);
		}
#endif
	} else {
		sctp_mbuf_stats[8]++;
	}
	sctp_mbuf_stats[9]++;
	m->m_len = 0;
	m->m_next = m->m_nextpkt = NULL;
	if (want_header) {
		m->m_pkthdr.len = 0;
	}
	return (m);
}



static int
sctp_move_to_outqueue(struct sctp_tcb *stcb, struct sctp_nets *net,
	struct sctp_stream_out *strq,
	int goal_mtu,
	int frag_point,
	int *locked,
        int *giveup)
{
	/* Move from the stream to the send_queue keeping track of the total */
	struct sctp_association *asoc;
	struct sctp_stream_queue_pending *sp;
	struct sctp_tmit_chunk *chk;
	struct sctp_data_chunk *dchkh;
	int to_move;
	uint8_t rcv_flags=0;
	SCTP_TCB_LOCK_ASSERT(stcb);
	asoc = &stcb->asoc;
	sp = TAILQ_FIRST(&strq->outqueue);

	if(sp == NULL) {
		*locked = 0;
		if(strq->last_msg_incomplete) {
			printf("Huh? Stream:%d lm_in_c=%d but queue is NULL\n",
			       strq->stream_no, strq->last_msg_incomplete);
			strq->last_msg_incomplete = 0;
		}
		return(0);
	}
	if ((sp->length == 0) && (sp->msg_is_complete == 0)) {
		/* Must wait for more data, must be last msg */
		*locked = 1;
		*giveup = 1;
		return (0);
	} else if(sp->length == 0) {
		/* This should not happen */
		panic("sp length is 0?");
	}
        /* We may not have a m_pkthder due to the way the sbwait/awake
	 * thing works... assure that we do!
	 */
	if ((goal_mtu >= sp->length) && (sp->msg_is_complete)) {
		/* It all fits and its a complete msg, no brainer */
		to_move = min(sp->length, frag_point);
		if(to_move == sp->length) {
			/* Getting it all */
			if (sp->some_taken)  {
				rcv_flags |= SCTP_DATA_LAST_FRAG;
			} else {
				rcv_flags |= SCTP_DATA_NOT_FRAG;
			}
		} else {
			/* Not getting it all, frag point overrides */
			if(sp->some_taken == 0) {
				rcv_flags |= SCTP_DATA_FIRST_FRAG;
			}
			sp->some_taken = 1;
		}
	} else {
		to_move = sctp_can_we_split_this(sp, goal_mtu, frag_point);
		if (to_move) {
			if (to_move > sp->length) {
				to_move = sp->length;
			}
			if(sp->some_taken == 0) {
				rcv_flags |= SCTP_DATA_FIRST_FRAG;
			}
			sp->some_taken = 1;
		} else {
			return (0);
		}
	}
	/* If we reach here, we can copy out a chunk */
        chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* No chunk memory */
	out_gu:
		*giveup = 1;
		return(0);
	}
	SCTP_INCR_CHK_COUNT();
	/* clear it */
	memset(chk, sizeof(*chk), 0);
	chk->rec.data.rcv_flags = rcv_flags;
	sctp_snd_sb_alloc(stcb, sizeof(struct sctp_data_chunk));
	if (to_move >= sp->length) {
		/* we can steal the whole thing */
		chk->data = sp->data;
		chk->last_mbuf = sp->tail_mbuf;
		sp->data = sp->tail_mbuf = NULL;
	} else {
		struct mbuf *m;
		chk->data = m_copym(sp->data, 0, to_move, M_DONTWAIT);
		chk->last_mbuf = NULL;
		if(chk->data == NULL) {
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
			SCTP_DECR_CHK_COUNT();
			goto out_gu;
		}
		/* Pull off the data */
		m_adj(sp->data, to_move);
		/* Now lets work our way down and compact it 
		 */
		m = sp->data;
		while(m && (m->m_len == 0)) {
			sp->data  = m->m_next;
			m->m_next = NULL;
			if(sp->tail_mbuf == m) {
				/* freeing tail */
				sp->tail_mbuf = sp->data;
			}
			sctp_m_free(m);
			if(sp->data->m_next)
				m = sp->data;
			else
				m = NULL;
		}
	}
	if(to_move > sp->length) {
		panic("Huh, how can to_move be larger?");
	} else
		sp->length -= to_move;

	if ((chk->data->m_flags & M_PKTHDR) == 0) {
		struct mbuf *m;
		m = sctp_get_mbuf_for_msg(1, 1, M_DONTWAIT, 0, MT_DATA);
		if(m == NULL) {
			printf("We will Panic maybe, out of mbufs\n");
		} else {
			m->m_len = 0;
			m->m_next = chk->data;
			chk->data = m;
			chk->data->m_pkthdr.len = to_move;
		}
	}
	M_PREPEND(chk->data, sizeof(struct sctp_data_chunk), M_DONTWAIT);
	if (chk->data == NULL) {
		/* HELP */
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		goto out_gu;
	}
	chk->book_size = chk->send_size = (to_move + sizeof(struct sctp_data_chunk));
	chk->sent = SCTP_DATAGRAM_UNSENT;

	/* get last_mbuf and counts of mb useage
	 * This is ugly but hopefully its only one mbuf.
	 */
	if(chk->last_mbuf == NULL) {
		chk->last_mbuf = chk->data;
		while(chk->last_mbuf->m_next != NULL) {
			chk->last_mbuf = chk->last_mbuf->m_next;
		}
	}
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->pad_inplace = 0;
	chk->no_fr_allowed = 0;
	chk->rec.data.stream_seq = sp->strseq;
	chk->rec.data.stream_number = sp->stream;
	chk->rec.data.payloadtype = sp->ppid;
	chk->rec.data.context = sp->context;
	chk->rec.data.doing_fast_retransmit = 0;
	chk->rec.data.ect_nonce = 0;	/* ECN Nonce */

	chk->rec.data.timetodrop = sp->ts;
	chk->flags = sp->act_flags;
	chk->addr_over = sp->addr_over;

	chk->whoTo = net;
	atomic_add_int(&chk->whoTo->ref_count, 1);

	chk->rec.data.TSN_seq = asoc->sending_seq++;
	dchkh = mtod(chk->data, struct sctp_data_chunk *);
	/*
	 * Put the rest of the things in place now. Size was done
	 * earlier in previous loop prior to padding.
	 */
	dchkh->ch.chunk_type = SCTP_DATA;
	dchkh->ch.chunk_flags = chk->rec.data.rcv_flags;
	dchkh->dp.tsn = htonl(chk->rec.data.TSN_seq);
	dchkh->dp.stream_id = htons(strq->stream_no);
	dchkh->dp.stream_sequence = htons(chk->rec.data.stream_seq);
	dchkh->dp.protocol_id = chk->rec.data.payloadtype;
	dchkh->ch.chunk_length = htons(chk->send_size);
	/* Now advance the chk->send_size by the actual
	 * pad needed.
	 */
	if(chk->send_size < SCTP_SIZE32(chk->book_size)) {
		/* need a pad */
		struct mbuf *lm;
		int pads;
		pads = SCTP_SIZE32(chk->book_size) - chk->send_size;
		if (sctp_pad_lastmbuf(chk->data, pads, chk->last_mbuf) == 0) {
			chk->pad_inplace = 1;
		}
		if((lm = chk->last_mbuf->m_next) != NULL) {
			/* pad added an mbuf */
			chk->last_mbuf = lm;
		}
		if(chk->data->m_flags & M_PKTHDR) {
			chk->data->m_pkthdr.len += pads;
		}
		chk->send_size += pads;
	}
	sctp_set_prsctp_policy(stcb, sp);

	if(sp->msg_is_complete && (sp->length == 0)) {
		/* All done pull and kill the message */
		asoc->stream_queue_cnt--;
		TAILQ_REMOVE(&strq->outqueue, sp, next);
		sctp_free_remote_addr(sp->net);
		if(sp->data) {
			sctp_m_freem(sp->data);
		}
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_strmoq, sp);
		SCTP_DECR_STRMOQ_COUNT();
		/* we can't be locked to it */
		*locked = 0;		
		stcb->asoc.locked_on_sending = NULL;
	} else {
		/* more to go, we are locked */
		*locked = 1;
	}
	asoc->chunks_on_out_queue++;
	TAILQ_INSERT_TAIL(&asoc->send_queue, chk, sctp_next);
	asoc->send_queue_cnt++;
	return (to_move);
}


static struct sctp_stream_out *
sctp_select_a_stream(struct sctp_tcb *stcb, struct sctp_association *asoc)
{
	struct sctp_stream_out *strq;

	/* Find the next stream to use */
	if (asoc->last_out_stream == NULL) {
		strq = asoc->last_out_stream = TAILQ_FIRST(&asoc->out_wheel);
		if (asoc->last_out_stream == NULL) {
			/* huh nothing on the wheel, TSNH */
			return(NULL);
		}
		goto done_it;
	}
	strq = TAILQ_NEXT(asoc->last_out_stream, next_spoke);
 done_it:
	if (strq == NULL) {
		strq = asoc->last_out_stream = TAILQ_FIRST(&asoc->out_wheel);
	}
	return(strq);

}

static void
sctp_fill_outqueue(struct sctp_tcb *stcb,
    struct sctp_nets *net, int frag_point)
{
	struct sctp_association *asoc;
	struct sctp_stream_out *strq, *strqn;
	int goal_mtu, moved_how_much;
	int locked, giveup;
	struct sctp_stream_queue_pending *sp;

	SCTP_TCB_LOCK_ASSERT(stcb);
	asoc = &stcb->asoc;
#ifdef AF_INET6
	if (net->ro._l_addr.sin6.sin6_family == AF_INET6) {
		goal_mtu = net->mtu - SCTP_MIN_OVERHEAD;
	} else {
		/* ?? not sure what else to do */
		goal_mtu = net->mtu - SCTP_MIN_V4_OVERHEAD;
	}
#else
	goal_mtu = net->mtu - SCTP_MIN_OVERHEAD;
	mtu_fromwheel = 0;
#endif
	/* must make even word boundary */
	goal_mtu &= 0xffffffffc;
	if(asoc->locked_on_sending ) {
		/* We are stuck on one stream until the message completes. */
		strqn = strq = asoc->locked_on_sending;
		locked = 1;
	} else {
		strqn = strq = sctp_select_a_stream(stcb, asoc);
		locked = 0;
	}

	while ((goal_mtu > 0) && strq) {
		sp = TAILQ_FIRST(&strq->outqueue);
		/* If CMT is off, we must validate that
		 * the stream in question has the first
		 * item pointed towards are network destionation 
		 * requested by the caller. Note that if we
		 * turn out to be locked to a stream (assigning
		 * TSN's then we must stop, since we cannot
		 * look for another stream with data to send
		 * to that destination). In CMT's case, by
		 * skipping this check, we will send one
		 * data packet towards the requested net.
		 */
		if(sp == NULL) {
			break;
		}
		if ((sp->net != net) && (sctp_cmt_on_off == 0)){
			/* none for this network */
			if(locked) {
				break;
			} else {
				strq = sctp_select_a_stream(stcb, asoc);
				if(strq == NULL)
					/* none left */
					break;
				if(strqn == strq) {
					/* I have circled */
					break;
				}
				continue;
			}
		}
		giveup = 0;
		moved_how_much = sctp_move_to_outqueue(stcb, net, strq, goal_mtu, frag_point, &locked, &giveup);
		asoc->last_out_stream = strq;
		if (locked) {
			asoc->locked_on_sending = strq;
			if ((moved_how_much == 0) || (giveup))
				/* no more to move for now */
				break;
		} else {
			asoc->locked_on_sending = NULL;
			if(TAILQ_FIRST(&strq->outqueue) == NULL) {
				sctp_remove_from_wheel(asoc, strq);
			}
			if(giveup)
				break;
			strq = sctp_select_a_stream(stcb, asoc);
			if(strq == NULL) 
				break;
			if(strqn == strq) {
				/* I have circled */
				break;
			}
		}
		goal_mtu -= moved_how_much;
		goal_mtu &= 0xffffffffc;
	}
}

void
sctp_fix_ecn_echo(struct sctp_association *asoc)
{
	struct sctp_tmit_chunk *chk;

	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if (chk->rec.chunk_id == SCTP_ECN_ECHO) {
			chk->sent = SCTP_DATAGRAM_UNSENT;
		}
	}
}

static void
sctp_move_to_an_alt(struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    struct sctp_nets *net)
{
	struct sctp_tmit_chunk *chk;
	struct sctp_nets *a_net;

	SCTP_TCB_LOCK_ASSERT(stcb);
	a_net = sctp_find_alternate_net(stcb, net, 0);
	if ((a_net != net) &&
	    ((a_net->dest_state & SCTP_ADDR_REACHABLE) == SCTP_ADDR_REACHABLE)) {
		/*
		 * We only proceed if a valid alternate is found that is not
		 * this one and is reachable. Here we must move all chunks
		 * queued in the send queue off of the destination address
		 * to our alternate.
		 */
		TAILQ_FOREACH(chk, &asoc->send_queue, sctp_next) {
			if (chk->whoTo == net) {
				/* Move the chunk to our alternate */
				sctp_free_remote_addr(chk->whoTo);
				chk->whoTo = a_net;
				atomic_add_int(&a_net->ref_count, 1);
			}
		}
	}
}

extern int sctp_early_fr;

int
sctp_med_chunk_output(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    int *num_out,
    int *reason_code,
    int control_only, int *cwnd_full, int from_where,
    struct timeval *now, int *now_filled)
{
	/*
	 * Ok this is the generic chunk service queue. we must do the
	 * following: - Service the stream queue that is next, moving any
	 * message (note I must get a complete message i.e. FIRST/MIDDLE and
	 * LAST to the out queue in one pass) and assigning TSN's - Check to
	 * see if the cwnd/rwnd allows any output, if so we go ahead and
	 * fomulate and send the low level chunks. Making sure to combine
	 * any control in the control chunk queue also.
	 */
	struct sctp_nets *net;
	struct mbuf *outchain, *endoutchain;
	struct sctp_tmit_chunk *chk, *nchk;
	struct sctphdr *shdr;

	/* temp arrays for unlinking */
	struct sctp_tmit_chunk *data_list[SCTP_MAX_DATA_BUNDLING];
	int no_fragmentflg, error;
	int one_chunk, hbflag;
	int asconf, cookie, no_out_cnt;
	int bundle_at, ctl_cnt, no_data_chunks, cwnd_full_ind;
	unsigned int mtu, r_mtu, omtu;
	int frag_point;
	*num_out = 0;
	struct sctp_nets *start_at, *old_startat = NULL, *send_start_at;

	cwnd_full_ind = 0;
	int tsns_sent = 0;
	uint32_t auth_offset = 0;
	struct sctp_auth_chunk *auth = NULL;

	ctl_cnt = no_out_cnt = asconf = cookie = 0;
	/*
	 * First lets prime the pump. For each destination, if there is room
	 * in the flight size, attempt to pull an MTU's worth out of the
	 * stream queues into the general send_queue
	 */
#ifdef SCTP_AUDITING_ENABLED
	sctp_audit_log(0xC2, 2);
#endif
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
		printf("***********************\n");
	}
#endif
	SCTP_TCB_LOCK_ASSERT(stcb);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	sctp_lock_assert(inp->ip_inp.inp.inp_socket);
#endif
	hbflag = 0;
	if ((control_only) || (asoc->stream_reset_outstanding))
		no_data_chunks = 1;
	else
		no_data_chunks = 0;

	/* Nothing to possible to send? */
	if (TAILQ_EMPTY(&asoc->control_send_queue) &&
	    TAILQ_EMPTY(&asoc->send_queue) &&
	    TAILQ_EMPTY(&asoc->out_wheel)) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("All wheels empty\n");
		}
#endif
		return (0);
	}
	if (asoc->peers_rwnd == 0) {
		/* No room in peers rwnd */
		*cwnd_full = 1;
		*reason_code = 1;
		if (asoc->total_flight > 0) {
			/* we are allowed one chunk in flight */
			no_data_chunks = 1;
		}
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
		printf("Ok we have done the fillup no_data_chunk=%d tf=%d prw:%d\n",
		    (int)no_data_chunks,
		    (int)asoc->total_flight, (int)asoc->peers_rwnd);
	}
#endif
	if ((no_data_chunks == 0) && (!TAILQ_EMPTY(&asoc->out_wheel))) {
		if (sctp_cmt_on_off) {
			/*
			 * for CMT we start at the next one past the one we
			 * last added data to.
			 */
			if (TAILQ_FIRST(&asoc->send_queue) != NULL) {
				goto skip_the_fill_from_streams;
			}
			if (asoc->last_net_data_came_from) {
				net = TAILQ_NEXT(asoc->last_net_data_came_from, sctp_next);
				if (net == NULL) {
					net = TAILQ_FIRST(&asoc->nets);
				}
			} else {
				/* back to start */
				net = TAILQ_FIRST(&asoc->nets);
			}

		} else {
			net = asoc->primary_destination;
			if (net == NULL) {
				/* TSNH */
				net = TAILQ_FIRST(&asoc->nets);
			}
		}
		start_at = net;
one_more_time:
		frag_point = sctp_get_frag_point(stcb, asoc);
		for (; net != NULL; net = TAILQ_NEXT(net, sctp_next)) {
			if (old_startat && (old_startat == net)) {
				break;
			}
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
				printf("net:%p fs:%d  cwnd:%d\n",
				    net, net->flight_size, net->cwnd);
			}
#endif
			if ((sctp_cmt_on_off == 0) && (net->ref_count < 2)) {
				/* nothing can be in queue for this guy */
				continue;
			}
			if (net->flight_size >= net->cwnd) {
				/* skip this network, no room */
				cwnd_full_ind++;
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
					printf("Ok skip fillup->fs:%d > cwnd:%d\n",
					    net->flight_size,
					    net->cwnd);
				}
#endif
				continue;
			}
			/*
			 * @@@ JRI : this loops through all nets
			 * 
			 * and calls sctp_fill_outqueue for all nets.
			 * 
			 * spin through the stream queues moving one message
			 * and assign TSN's as appropriate.
			 */
#ifdef SCTP_CWND_LOGGING
			sctp_log_cwnd(stcb, net, 0, SCTP_CWND_LOG_FILL_OUTQ_CALLED);
#endif
			sctp_fill_outqueue(stcb, net, frag_point);
		}
		if (start_at != TAILQ_FIRST(&asoc->nets)) {
			/* got to pick up the beginning stuff. */
			old_startat = start_at;
			start_at = net = TAILQ_FIRST(&asoc->nets);
			goto one_more_time;
		}
	}
skip_the_fill_from_streams:
	*cwnd_full = cwnd_full_ind;
	/* now service each destination and send out what we can for it */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
		int chk_cnt = 0;

		TAILQ_FOREACH(chk, &asoc->send_queue, sctp_next) {
			chk_cnt++;
		}
		printf("We have %d chunks on the send_queue\n", chk_cnt);
		chk_cnt = 0;
		TAILQ_FOREACH(chk, &asoc->sent_queue, sctp_next) {
			chk_cnt++;
		}
		printf("We have %d chunks on the sent_queue\n", chk_cnt);
		TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
			chk_cnt++;
		}
		printf("We have %d chunks on the control_queue\n", asoc->ctrl_queue_cnt);
	}
#endif
	/* Nothing to send? */
	if ((TAILQ_FIRST(&asoc->control_send_queue) == NULL) &&
	    (TAILQ_FIRST(&asoc->send_queue) == NULL)) {
		return (0);
	}
	chk = TAILQ_FIRST(&asoc->send_queue);
	if (chk) {
		send_start_at = chk->whoTo;
	} else {
		send_start_at = TAILQ_FIRST(&asoc->nets);
	}
	old_startat = NULL;
	if ((no_data_chunks == 0) &&
	    (sctp_is_sack_timer_running(stcb)) &&
	    (TAILQ_EMPTY(&asoc->control_send_queue)) &&
	    (!TAILQ_EMPTY(&asoc->send_queue))
	    ) {
		/*
		 * We need to send a sack, and we know there is NOT one in
		 * queue and we know there IS data to send. Look at the
		 * first data chunk on the queue, if a sack could fit with
		 * it, we will add one now.
		 */
		int oh = 0;

		chk = TAILQ_FIRST(&asoc->send_queue);
#ifdef AF_INET
		if (chk->whoTo->ro._l_addr.sa.sa_family == AF_INET) {
			oh = SCTP_MIN_V4_OVERHEAD;
		}
#endif
#ifdef AF_INET6
		if (chk->whoTo->ro._l_addr.sa.sa_family == AF_INET6) {
			oh = SCTP_MIN_OVERHEAD;
		}
#endif
		if (chk->whoTo->mtu >= (sizeof(struct sctp_sack_chunk) + chk->send_size + oh)) {
			struct sctp_nets *t_net;

			t_net = asoc->last_data_chunk_from;
			asoc->last_data_chunk_from = chk->whoTo;
			sctp_timer_stop(SCTP_TIMER_TYPE_RECV,
			    stcb->sctp_ep,
			    stcb, NULL);
			sctp_send_sack(stcb);
			asoc->last_data_chunk_from = t_net;
		}
	}
again_one_more_time:
	for (net = send_start_at; net != NULL; net = TAILQ_NEXT(net, sctp_next)) {
		/* how much can we send? */
		/* printf("Examine for sending net:%x\n", (uint32_t)net); */
		if (old_startat && (old_startat == net)) {
			/* through list ocmpletely. */
			break;
		}
		tsns_sent = 0;
		if (net->ref_count < 2) {
			/*
			 * Ref-count of 1 so we cannot have data or control
			 * queued to this address. Skip it.
			 */
			continue;
		}
		ctl_cnt = bundle_at = 0;
		endoutchain = outchain = NULL;
		no_fragmentflg = 1;
		one_chunk = 0;

		if ((net->ro.ro_rt) && (net->ro.ro_rt->rt_ifp)) {
			/*
			 * if we have a route and an ifp check to see if we
			 * have room to send to this guy
			 */
			struct ifnet *ifp;

			ifp = net->ro.ro_rt->rt_ifp;
			if ((ifp->if_snd.ifq_len + 2) >= ifp->if_snd.ifq_maxlen) {
				SCTP_STAT_INCR(sctps_ifnomemqueued);
#ifdef SCTP_LOG_MAXBURST
				sctp_log_maxburst(stcb, net, ifp->if_snd.ifq_len, ifp->if_snd.ifq_maxlen, SCTP_MAX_IFP_APPLIED);
#endif
				continue;
			}
		}
		if (((struct sockaddr *)&net->ro._l_addr)->sa_family == AF_INET) {
			mtu = net->mtu - (sizeof(struct ip) + sizeof(struct sctphdr));
		} else {
			mtu = net->mtu - (sizeof(struct ip6_hdr) + sizeof(struct sctphdr));
		}
		if (mtu > asoc->peers_rwnd) {
			if (asoc->total_flight > 0) {
				/* We have a packet in flight somewhere */
				r_mtu = asoc->peers_rwnd;
			} else {
				/* We are always allowed to send one MTU out */
				one_chunk = 1;
				r_mtu = mtu;
			}
		} else {
			r_mtu = mtu;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("Ok r_mtu is %d mtu is %d for this net:%p one_chunk:%d\n",
			    r_mtu, mtu, net, one_chunk);
		}
#endif
		/************************/
		/* Control transmission */
		/************************/
		/* Now first lets go through the control queue */
		for (chk = TAILQ_FIRST(&asoc->control_send_queue);
		    chk; chk = nchk) {
			nchk = TAILQ_NEXT(chk, sctp_next);
			if (chk->whoTo != net) {
				/*
				 * No, not sent to the network we are
				 * looking at
				 */
				continue;
			}
			if (chk->data == NULL) {
				continue;
			}
			if ((chk->data->m_flags & M_PKTHDR) == 0) {
				/*
				 * NOTE: the chk queue MUST have the PKTHDR
				 * flag set on it with a total in the
				 * m_pkthdr.len field!! else the chunk will
				 * ALWAYS be skipped
				 */
				continue;
			}
			if (chk->sent != SCTP_DATAGRAM_UNSENT) {
				/*
				 * It must be unsent. Cookies and ASCONF's
				 * hang around but there timers will force
				 * when marked for resend.
				 */
				continue;
			}
			/*
			 * if no AUTH is yet included and this chunk
			 * requires it, make sure to account for it.  We
			 * don't apply the size until the AUTH chunk is
			 * actually added below in case there is no room for
			 * this chunk. NOTE: we overload the use of "omtu"
			 * here
			 */
			if ((auth == NULL) &&
			    sctp_auth_is_required_chunk(chk->rec.chunk_id,
			    stcb->asoc.peer_auth_chunks)) {
				omtu = sctp_get_auth_chunk_len(stcb->asoc.peer_hmac_id);
			} else
				omtu = 0;
			/* Here we do NOT factor the r_mtu */
			if ((chk->data->m_pkthdr.len < (int)(mtu - omtu)) ||
			    (chk->flags & CHUNK_FLAGS_FRAGMENT_OK)) {
				/*
				 * We probably should glom the mbuf chain
				 * from the chk->data for control but the
				 * problem is it becomes yet one more level
				 * of tracking to do if for some reason
				 * output fails. Then I have got to
				 * reconstruct the merged control chain.. el
				 * yucko.. for now we take the easy way and
				 * do the copy
				 */
				/*
				 * Add an AUTH chunk, if chunk requires it
				 * save the offset into the chain for AUTH
				 */
				if (auth == NULL) {
					outchain = sctp_add_auth_chunk(outchain,
					    &endoutchain,
					    &auth,
					    &auth_offset,
					    stcb,
					    chk->rec.chunk_id);
				}
				outchain = sctp_copy_mbufchain(chk->data,
				    outchain,
				    &endoutchain,
				    1, 0);
				if (outchain == NULL) {
					return (ENOMEM);
				}
				/* update our MTU size */
				if (mtu > (chk->data->m_pkthdr.len + omtu))
					mtu -= (chk->data->m_pkthdr.len + omtu);
				else
					mtu = 0;

				/* Do clear IP_DF ? */
				if (chk->flags & CHUNK_FLAGS_FRAGMENT_OK) {
					no_fragmentflg = 0;
				}
				/* Mark things to be removed, if needed */
				if ((chk->rec.chunk_id == SCTP_SELECTIVE_ACK) ||
				    (chk->rec.chunk_id == SCTP_HEARTBEAT_REQUEST) ||
				    (chk->rec.chunk_id == SCTP_HEARTBEAT_ACK) ||
				    (chk->rec.chunk_id == SCTP_SHUTDOWN) ||
				    (chk->rec.chunk_id == SCTP_SHUTDOWN_ACK) ||
				    (chk->rec.chunk_id == SCTP_OPERATION_ERROR) ||
				    (chk->rec.chunk_id == SCTP_COOKIE_ACK) ||
				    (chk->rec.chunk_id == SCTP_ECN_CWR) ||
				    (chk->rec.chunk_id == SCTP_PACKET_DROPPED) ||
				    (chk->rec.chunk_id == SCTP_ASCONF_ACK)) {

					if (chk->rec.chunk_id == SCTP_HEARTBEAT_REQUEST)
						hbflag = 1;
					/* remove these chunks at the end */
					if (chk->rec.chunk_id == SCTP_SELECTIVE_ACK) {
						/* turn off the timer */
						if (callout_pending(&stcb->asoc.dack_timer.timer)) {
							sctp_timer_stop(SCTP_TIMER_TYPE_RECV,
							    inp, stcb, net);
						}
					}
					ctl_cnt++;
				} else {
					/*
					 * Other chunks, since they have
					 * timers running (i.e. COOKIE or
					 * ASCONF) we just "trust" that it
					 * gets sent or retransmitted.
					 */
					ctl_cnt++;
					if (chk->rec.chunk_id == SCTP_COOKIE_ECHO) {
						cookie = 1;
						no_out_cnt = 1;
					} else if (chk->rec.chunk_id == SCTP_ASCONF) {
						/*
						 * set hb flag since we can
						 * use these for RTO
						 */
						hbflag = 1;
						asconf = 1;
					}
					chk->sent = SCTP_DATAGRAM_SENT;
					chk->snd_count++;
				}
				if (mtu == 0) {
					/*
					 * Ok we are out of room but we can
					 * output without effecting the
					 * flight size since this little guy
					 * is a control only packet.
					 */
					if (asconf) {
						sctp_timer_start(SCTP_TIMER_TYPE_ASCONF, inp, stcb, net);
						asconf = 0;
					}
					if (cookie) {
						sctp_timer_start(SCTP_TIMER_TYPE_COOKIE, inp, stcb, net);
						cookie = 0;
					}
					M_PREPEND(outchain, sizeof(struct sctphdr), M_DONTWAIT);
					if (outchain == NULL) {
						/* no memory */
						error = ENOBUFS;
						goto error_out_again;
					}
					shdr = mtod(outchain, struct sctphdr *);
					shdr->src_port = inp->sctp_lport;
					shdr->dest_port = stcb->rport;
					shdr->v_tag = htonl(stcb->asoc.peer_vtag);
					shdr->checksum = 0;
					auth_offset += sizeof(struct sctphdr);
					if ((error = sctp_lowlevel_chunk_output(inp, stcb, net,
					    (struct sockaddr *)&net->ro._l_addr,
					    outchain, auth_offset, auth,
					    no_fragmentflg, 0, NULL, asconf))) {
						if (error == ENOBUFS) {
							asoc->ifp_had_enobuf = 1;
						}
						SCTP_STAT_INCR(sctps_lowlevelerr);
						if (from_where == 0) {
							SCTP_STAT_INCR(sctps_lowlevelerrusr);
						}
				error_out_again:
#ifdef SCTP_DEBUG
						if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
							printf("Gak got ctrl error %d\n", error);
						}
#endif
						/* error, could not output */
						if (hbflag) {
#ifdef SCTP_DEBUG
							if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
								printf("Update HB anyway\n");
							}
#endif
							if (*now_filled == 0) {
								SCTP_GETTIME_TIMEVAL(&net->last_sent_time);
								*now_filled = 1;
								*now = net->last_sent_time;
							} else {
								net->last_sent_time = *now;
							}
							hbflag = 0;
						}
						if (error == EHOSTUNREACH) {
							/*
							 * Destination went
							 * unreachable
							 * during this send
							 */
#ifdef SCTP_DEBUG
							if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
								printf("Moving data to an alterante\n");
							}
#endif
							sctp_move_to_an_alt(stcb, asoc, net);
						}
						sctp_clean_up_ctl(asoc);
						return (error);
					} else
						asoc->ifp_had_enobuf = 0;
					/* Only HB or ASCONF advances time */
					if (hbflag) {
						if (*now_filled == 0) {
							SCTP_GETTIME_TIMEVAL(&net->last_sent_time);
							*now_filled = 1;
							*now = net->last_sent_time;
						} else {
							net->last_sent_time = *now;
						}
						hbflag = 0;
					}
					/*
					 * increase the number we sent, if a
					 * cookie is sent we don't tell them
					 * any was sent out.
					 */
					outchain = endoutchain = NULL;
					auth = NULL;
					auth_offset = 0;
					if (!no_out_cnt)
						*num_out += ctl_cnt;
					/* recalc a clean slate and setup */
					if (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
						mtu = (net->mtu - SCTP_MIN_OVERHEAD);
					} else {
						mtu = (net->mtu - SCTP_MIN_V4_OVERHEAD);
					}
					no_fragmentflg = 1;
				}
			}
		}
		/*********************/
		/* Data transmission */
		/*********************/
		/*
		 * if AUTH for DATA is required and no AUTH has been added
		 * yet, account for this in the mtu now... if no data can be
		 * bundled, this adjustment won't matter anyways since the
		 * packet will be going out...
		 */
		if ((auth == NULL) &&
		    sctp_auth_is_required_chunk(SCTP_DATA,
		    stcb->asoc.peer_auth_chunks)) {
			mtu -= sctp_get_auth_chunk_len(stcb->asoc.peer_hmac_id);
		}
		/* now lets add any data within the MTU constraints */
		if (((struct sockaddr *)&net->ro._l_addr)->sa_family == AF_INET) {
			if (net->mtu > (sizeof(struct ip) + sizeof(struct sctphdr)))
				omtu = net->mtu - (sizeof(struct ip) + sizeof(struct sctphdr));
			else
				omtu = 0;
		} else {
			if (net->mtu > (sizeof(struct ip6_hdr) + sizeof(struct sctphdr)))
				omtu = net->mtu - (sizeof(struct ip6_hdr) + sizeof(struct sctphdr));
			else
				omtu = 0;
		}

#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("Now to data transmission\n");
		}
#endif

		if (((asoc->state & SCTP_STATE_OPEN) == SCTP_STATE_OPEN) ||
		    (cookie)) {
			for (chk = TAILQ_FIRST(&asoc->send_queue); chk; chk = nchk) {
				if (no_data_chunks) {
					/* let only control go out */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("Either nothing to send or we are full\n");
					}
#endif
					break;
				}
				if (net->flight_size >= net->cwnd) {
					/* skip this net, no room for data */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("fs:%d > cwnd:%d\n",
						    net->flight_size, net->cwnd);
					}
#endif

					*reason_code = 2;
					break;
				}
				nchk = TAILQ_NEXT(chk, sctp_next);
				if (chk->whoTo != net) {
					/* No, not sent to this net */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("chk->whoTo:%p not %p\n",
						    chk->whoTo, net);

					}
#endif
					continue;
				}
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
					printf("Can we pick up a chunk?\n");
				}
#endif
				if ((chk->send_size > omtu) && ((chk->flags & CHUNK_FLAGS_FRAGMENT_OK) == 0)) {
					/*
					 * strange, we have a chunk that is
					 * to bit for its destination and
					 * yet no fragment ok flag.
					 * Something went wrong when the
					 * PMTU changed...we did not mark
					 * this chunk for some reason?? I
					 * will fix it here by letting IP
					 * fragment it for now and printing
					 * a warning. This really should not
					 * happen ...
					 */
#ifdef SCTP_DEBUG
					printf("Warning chunk of %d bytes > mtu:%d and yet PMTU disc missed\n",
					    chk->send_size, mtu);
#endif
					chk->flags |= CHUNK_FLAGS_FRAGMENT_OK;
				}
				if (((chk->send_size <= mtu) && (chk->send_size <= r_mtu)) ||
				    ((chk->flags & CHUNK_FLAGS_FRAGMENT_OK) && (chk->send_size <= asoc->peers_rwnd))) {
					/* ok we will add this one */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("Picking up the chunk\n");
					}
#endif
					/*
					 * Add an AUTH chunk, if chunk
					 * requires it, save the offset into
					 * the chain for AUTH
					 */
					if (auth == NULL) {
						outchain = sctp_add_auth_chunk(outchain,
						    &endoutchain,
						    &auth,
						    &auth_offset,
						    stcb,
						    SCTP_DATA);
					}
					outchain = sctp_copy_mbufchain(chk->data, outchain, &endoutchain, 1, 0);
					if (outchain == NULL) {
#ifdef SCTP_DEBUG
						if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
							printf("Gakk no memory\n");
						}
#endif
						if (!callout_pending(&net->rxt_timer.timer)) {
							sctp_timer_start(SCTP_TIMER_TYPE_SEND, inp, stcb, net);
						}
						return (ENOMEM);
					}
					/* upate our MTU size */
					/* Do clear IP_DF ? */
					if (chk->flags & CHUNK_FLAGS_FRAGMENT_OK) {
						no_fragmentflg = 0;
					}
					/* unsigned subtraction of mtu */
					if (mtu > chk->send_size)
						mtu -= chk->send_size;
					else
						mtu = 0;
					/* unsigned subtraction of r_mtu */
					if (r_mtu > chk->send_size)
						r_mtu -= chk->send_size;
					else
						r_mtu = 0;

					data_list[bundle_at++] = chk;
					if (bundle_at >= SCTP_MAX_DATA_BUNDLING) {
						mtu = 0;
						break;
					}
					if ((mtu == 0) || (r_mtu == 0) || (one_chunk)) {
						break;
					}
				} else {
					/*
					 * Must be sent in order of the
					 * TSN's (on a network)
					 */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("ok no more chk:%d > mtu:%d || < r_mtu:%d\n",
						    chk->send_size, mtu, r_mtu);
					}
#endif

					break;
				}
			}	/* for () */
		}		/* if asoc.state OPEN */
		/* Is there something to send for this destination? */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("ok now is chain assembled? %p\n",
			    outchain);
		}
#endif

		if (outchain) {
			/* We may need to start a control timer or two */
			if (asconf) {
				sctp_timer_start(SCTP_TIMER_TYPE_ASCONF, inp, stcb, net);
				asconf = 0;
			}
			if (cookie) {
				sctp_timer_start(SCTP_TIMER_TYPE_COOKIE, inp, stcb, net);
				cookie = 0;
			}
			/* must start a send timer if data is being sent */
			if (bundle_at && (!callout_pending(&net->rxt_timer.timer))) {
				/*
				 * no timer running on this destination
				 * restart it.
				 */
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
					printf("ok lets start a send timer .. we will transmit %p\n",
					    outchain);
				}
#endif
				sctp_timer_start(SCTP_TIMER_TYPE_SEND, inp, stcb, net);
			}
			/* Now send it, if there is anything to send :> */
			M_PREPEND(outchain, sizeof(struct sctphdr), M_DONTWAIT);
			if (outchain == NULL) {
				/* out of mbufs */
				error = ENOBUFS;
				goto errored_send;
			}
			shdr = mtod(outchain, struct sctphdr *);
			shdr->src_port = inp->sctp_lport;
			shdr->dest_port = stcb->rport;
			shdr->v_tag = htonl(stcb->asoc.peer_vtag);
			shdr->checksum = 0;
			auth_offset += sizeof(struct sctphdr);
			if ((error = sctp_lowlevel_chunk_output(inp, stcb, net,
			    (struct sockaddr *)&net->ro._l_addr,
			    outchain,
			    auth_offset,
			    auth,
			    no_fragmentflg,
			    bundle_at,
			    data_list[0],
			    asconf))) {
				/* error, we could not output */
				if (error == ENOBUFS) {
					asoc->ifp_had_enobuf = 1;
				}
				SCTP_STAT_INCR(sctps_lowlevelerr);
				if (from_where == 0) {
					SCTP_STAT_INCR(sctps_lowlevelerrusr);
				}
		errored_send:
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
					printf("Gak send error %d\n", error);
				}
#endif
				if (hbflag) {
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("Update HB time anyway\n");
					}
#endif
					if (*now_filled == 0) {
						SCTP_GETTIME_TIMEVAL(&net->last_sent_time);
						*now_filled = 1;
						*now = net->last_sent_time;
					} else {
						net->last_sent_time = *now;
					}
					hbflag = 0;
				}
				if (error == EHOSTUNREACH) {
					/*
					 * Destination went unreachable
					 * during this send
					 */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
						printf("Calling the movement routine\n");

					}
#endif
					sctp_move_to_an_alt(stcb, asoc, net);
				}
				sctp_clean_up_ctl(asoc);
				return (error);
			} else {
				asoc->ifp_had_enobuf = 0;
			}
			outchain = endoutchain = NULL;
			auth = NULL;
			auth_offset = 0;
			if (bundle_at || hbflag) {
				/* For data/asconf and hb set time */
				if (*now_filled == 0) {
					SCTP_GETTIME_TIMEVAL(&net->last_sent_time);
					*now_filled = 1;
					*now = net->last_sent_time;
				} else {
					net->last_sent_time = *now;
				}
			}
			if (!no_out_cnt) {
				*num_out += (ctl_cnt + bundle_at);
			}
			if (bundle_at) {
				/* if (!net->rto_pending) { */
				/* setup for a RTO measurement */
				/* net->rto_pending = 1; */
				tsns_sent = data_list[0]->rec.data.TSN_seq;

				data_list[0]->do_rtt = 1;
				/* } else { */
				/* data_list[0]->do_rtt = 0; */
				/* } */
				SCTP_STAT_INCR_BY(sctps_senddata, bundle_at);
				sctp_clean_up_datalist(stcb, asoc, data_list, bundle_at, net);
				if (sctp_early_fr) {
					if (net->flight_size < net->cwnd) {
						/* start or restart it */
						if (callout_pending(&net->fr_timer.timer)) {
							sctp_timer_stop(SCTP_TIMER_TYPE_EARLYFR, inp, stcb, net);
						}
						SCTP_STAT_INCR(sctps_earlyfrstrout);
						sctp_timer_start(SCTP_TIMER_TYPE_EARLYFR, inp, stcb, net);
					} else {
						/* stop it if its running */
						if (callout_pending(&net->fr_timer.timer)) {
							SCTP_STAT_INCR(sctps_earlyfrstpout);
							sctp_timer_stop(SCTP_TIMER_TYPE_EARLYFR, inp, stcb, net);
						}
					}
				}
			}
			if (one_chunk) {
				break;
			}
		}
#ifdef SCTP_CWND_LOGGING
		sctp_log_cwnd(stcb, net, tsns_sent, SCTP_CWND_LOG_FROM_SEND);
#endif
	}
	if (old_startat == NULL) {
		old_startat = send_start_at;
		send_start_at = TAILQ_FIRST(&asoc->nets);
		goto again_one_more_time;
	}
	/*
	 * At the end there should be no NON timed chunks hanging on this
	 * queue.
	 */
#ifdef SCTP_CWND_LOGGING
	sctp_log_cwnd(stcb, net, *num_out, SCTP_CWND_LOG_FROM_SEND);
#endif
	if ((*num_out == 0) && (*reason_code == 0)) {
		*reason_code = 3;
	}
	sctp_clean_up_ctl(asoc);
	return (0);
}

void
sctp_queue_op_err(struct sctp_tcb *stcb, struct mbuf *op_err)
{
	/*
	 * Prepend a OPERATIONAL_ERROR chunk header and put on the end of
	 * the control chunk queue.
	 */
	struct sctp_chunkhdr *hdr;
	struct sctp_tmit_chunk *chk;
	struct mbuf *mat;

	SCTP_TCB_LOCK_ASSERT(stcb);
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(op_err);
		return;
	}
	SCTP_INCR_CHK_COUNT();
	M_PREPEND(op_err, sizeof(struct sctp_chunkhdr), M_DONTWAIT);
	if (op_err == NULL) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return;
	}
	chk->send_size = 0;
	mat = op_err;
	while (mat != NULL) {
		chk->send_size += mat->m_len;
		mat = mat->m_next;
	}
	chk->rec.chunk_id = SCTP_OPERATION_ERROR;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = op_err;
	chk->whoTo = chk->asoc->primary_destination;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	hdr = mtod(op_err, struct sctp_chunkhdr *);
	hdr->chunk_type = SCTP_OPERATION_ERROR;
	hdr->chunk_flags = 0;
	hdr->chunk_length = htons(chk->send_size);
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue,
	    chk,
	    sctp_next);
	chk->asoc->ctrl_queue_cnt++;
}

int
sctp_send_cookie_echo(struct mbuf *m,
    int offset,
    struct sctp_tcb *stcb,
    struct sctp_nets *net)
{
	/*
	 * pull out the cookie and put it at the front of the control chunk
	 * queue.
	 */
	int at;
	struct mbuf *cookie, *mat;
	struct sctp_paramhdr parm, *phdr;
	struct sctp_chunkhdr *hdr;
	struct sctp_tmit_chunk *chk;
	uint16_t ptype, plen;

	/* First find the cookie in the param area */
	cookie = NULL;
	at = offset + sizeof(struct sctp_init_chunk);

	SCTP_TCB_LOCK_ASSERT(stcb);
	do {
		phdr = sctp_get_next_param(m, at, &parm, sizeof(parm));
		if (phdr == NULL) {
			return (-3);
		}
		ptype = ntohs(phdr->param_type);
		plen = ntohs(phdr->param_length);
		if (ptype == SCTP_STATE_COOKIE) {
			int pad;

			/* found the cookie */
			if ((pad = (plen % 4))) {
				plen += 4 - pad;
			}
			cookie = sctp_m_copym(m, at, plen, M_DONTWAIT);
			if (cookie == NULL) {
				/* No memory */
				return (-2);
			}
			break;
		}
		at += SCTP_SIZE32(plen);
	} while (phdr);
	if (cookie == NULL) {
		/* Did not find the cookie */
		return (-3);
	}
	/* ok, we got the cookie lets change it into a cookie echo chunk */

	/* first the change from param to cookie */
	hdr = mtod(cookie, struct sctp_chunkhdr *);
	hdr->chunk_type = SCTP_COOKIE_ECHO;
	hdr->chunk_flags = 0;
	/* now we MUST have a PKTHDR on it */
	if ((cookie->m_flags & M_PKTHDR) != M_PKTHDR) {
		/* we hope this happens rarely */
		mat = sctp_get_mbuf_for_msg(8, 1, M_DONTWAIT, 1, MT_HEADER);
		if (mat == NULL) {
			sctp_m_freem(cookie);
			return (-4);
		}
		mat->m_len = 0;
		mat->m_pkthdr.rcvif = 0;
		mat->m_next = cookie;
		cookie = mat;
	}
	cookie->m_pkthdr.len = plen;
	/* get the chunk stuff now and place it in the FRONT of the queue */
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(cookie);
		return (-5);
	}
	SCTP_INCR_CHK_COUNT();
	chk->send_size = cookie->m_pkthdr.len;
	chk->rec.chunk_id = SCTP_COOKIE_ECHO;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = cookie;
	chk->whoTo = chk->asoc->primary_destination;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	TAILQ_INSERT_HEAD(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
	return (0);
}

void
sctp_send_heartbeat_ack(struct sctp_tcb *stcb,
    struct mbuf *m,
    int offset,
    int chk_length,
    struct sctp_nets *net)
{
	/*
	 * take a HB request and make it into a HB ack and send it.
	 */
	struct mbuf *outchain;
	struct sctp_chunkhdr *chdr;
	struct sctp_tmit_chunk *chk;


	if (net == NULL)
		/* must have a net pointer */
		return;

	outchain = sctp_m_copym(m, offset, chk_length, M_DONTWAIT);
	if (outchain == NULL) {
		/* gak out of memory */
		return;
	}
	chdr = mtod(outchain, struct sctp_chunkhdr *);
	chdr->chunk_type = SCTP_HEARTBEAT_ACK;
	chdr->chunk_flags = 0;
	if ((outchain->m_flags & M_PKTHDR) != M_PKTHDR) {
		/* should not happen but we are cautious. */
		struct mbuf *tmp;

		tmp = sctp_get_mbuf_for_msg(1, 1, M_DONTWAIT, 1, MT_HEADER);
		if (tmp == NULL) {
			return;
		}
		tmp->m_len = 0;
		tmp->m_pkthdr.rcvif = 0;
		tmp->m_next = outchain;
		outchain = tmp;
	}
	outchain->m_pkthdr.len = chk_length;
	if (chk_length % 4) {
		/* need pad */
		uint32_t cpthis = 0;
		int padlen;

		padlen = 4 - (outchain->m_pkthdr.len % 4);
		m_copyback(outchain, outchain->m_pkthdr.len, padlen,
		    (caddr_t)&cpthis);
	}
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(outchain);
		return;
	}
	SCTP_INCR_CHK_COUNT();
	chk->send_size = chk_length;
	chk->rec.chunk_id = SCTP_HEARTBEAT_ACK;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = outchain;
	chk->whoTo = net;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
}

int
sctp_send_cookie_ack(struct sctp_tcb *stcb)
{
	/* formulate and queue a cookie-ack back to sender */
	struct mbuf *cookie_ack;
	struct sctp_chunkhdr *hdr;
	struct sctp_tmit_chunk *chk;

	cookie_ack = NULL;
	SCTP_TCB_LOCK_ASSERT(stcb);

	cookie_ack = sctp_get_mbuf_for_msg(sizeof(struct sctp_chunkhdr), 1, M_DONTWAIT, 1, MT_HEADER);
	if (cookie_ack == NULL) {
		/* no mbuf's */
		return (-1);
	}
	cookie_ack->m_data += SCTP_MIN_OVERHEAD;
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(cookie_ack);
		return (-1);
	}
	SCTP_INCR_CHK_COUNT();

	chk->send_size = sizeof(struct sctp_chunkhdr);
	chk->rec.chunk_id = SCTP_COOKIE_ACK;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = cookie_ack;
	if (chk->asoc->last_control_chunk_from != NULL) {
		chk->whoTo = chk->asoc->last_control_chunk_from;
	} else {
		chk->whoTo = chk->asoc->primary_destination;
	}
	atomic_add_int(&chk->whoTo->ref_count, 1);
	hdr = mtod(cookie_ack, struct sctp_chunkhdr *);
	hdr->chunk_type = SCTP_COOKIE_ACK;
	hdr->chunk_flags = 0;
	hdr->chunk_length = htons(chk->send_size);
	cookie_ack->m_pkthdr.len = cookie_ack->m_len = chk->send_size;
	cookie_ack->m_pkthdr.rcvif = 0;
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
	return (0);
}


int
sctp_send_shutdown_ack(struct sctp_tcb *stcb, struct sctp_nets *net)
{
	/* formulate and queue a SHUTDOWN-ACK back to the sender */
	struct mbuf *m_shutdown_ack;
	struct sctp_shutdown_ack_chunk *ack_cp;
	struct sctp_tmit_chunk *chk;

	m_shutdown_ack = sctp_get_mbuf_for_msg(sizeof(struct sctp_shutdown_ack_chunk), 1, M_DONTWAIT, 1, MT_HEADER);
	if (m_shutdown_ack == NULL) {
		/* no mbuf's */
		return (-1);
	}
	m_shutdown_ack->m_data += SCTP_MIN_OVERHEAD;
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(m_shutdown_ack);
		return (-1);
	}
	SCTP_INCR_CHK_COUNT();

	chk->send_size = sizeof(struct sctp_chunkhdr);
	chk->rec.chunk_id = SCTP_SHUTDOWN_ACK;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = m_shutdown_ack;
	chk->whoTo = net;
	atomic_add_int(&net->ref_count, 1);

	ack_cp = mtod(m_shutdown_ack, struct sctp_shutdown_ack_chunk *);
	ack_cp->ch.chunk_type = SCTP_SHUTDOWN_ACK;
	ack_cp->ch.chunk_flags = 0;
	ack_cp->ch.chunk_length = htons(chk->send_size);
	m_shutdown_ack->m_pkthdr.len = m_shutdown_ack->m_len = chk->send_size;
	m_shutdown_ack->m_pkthdr.rcvif = 0;
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
	return (0);
}

int
sctp_send_shutdown(struct sctp_tcb *stcb, struct sctp_nets *net)
{
	/* formulate and queue a SHUTDOWN to the sender */
	struct mbuf *m_shutdown;
	struct sctp_shutdown_chunk *shutdown_cp;
	struct sctp_tmit_chunk *chk;

	m_shutdown = sctp_get_mbuf_for_msg(sizeof(struct sctp_shutdown_chunk), 1, M_DONTWAIT, 1, MT_HEADER);
	if (m_shutdown == NULL) {
		/* no mbuf's */
		return (-1);
	}
	m_shutdown->m_data += SCTP_MIN_OVERHEAD;
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(m_shutdown);
		return (-1);
	}
	SCTP_INCR_CHK_COUNT();

	chk->send_size = sizeof(struct sctp_shutdown_chunk);
	chk->rec.chunk_id = SCTP_SHUTDOWN;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->data = m_shutdown;
	chk->whoTo = net;
	atomic_add_int(&net->ref_count, 1);

	shutdown_cp = mtod(m_shutdown, struct sctp_shutdown_chunk *);
	shutdown_cp->ch.chunk_type = SCTP_SHUTDOWN;
	shutdown_cp->ch.chunk_flags = 0;
	shutdown_cp->ch.chunk_length = htons(chk->send_size);
	shutdown_cp->cumulative_tsn_ack = htonl(stcb->asoc.cumulative_tsn);
	m_shutdown->m_pkthdr.len = m_shutdown->m_len = chk->send_size;
	m_shutdown->m_pkthdr.rcvif = 0;
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;

	
	if ((stcb->sctp_socket) && 
	    (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_TCPTYPE) &&
	    (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_CONNECTED)) {
		stcb->sctp_ep->sctp_socket->so_snd.sb_cc = 0;
		if (((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_SOCKET_ALLGONE) == 0) &&
		    ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_SOCKET_GONE) == 0))
			soisdisconnecting(stcb->sctp_ep->sctp_socket);
	}

	return (0);
}

int
sctp_send_asconf(struct sctp_tcb *stcb, struct sctp_nets *net)
{
	/*
	 * formulate and queue an ASCONF to the peer ASCONF parameters
	 * should be queued on the assoc queue
	 */
	struct sctp_tmit_chunk *chk;
	struct mbuf *m_asconf;
	struct sctp_asconf_chunk *acp;


	SCTP_TCB_LOCK_ASSERT(stcb);
	/* compose an ASCONF chunk, maximum length is PMTU */
	m_asconf = sctp_compose_asconf(stcb);
	if (m_asconf == NULL) {
		return (-1);
	}
	acp = mtod(m_asconf, struct sctp_asconf_chunk *);
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		sctp_m_freem(m_asconf);
		return (-1);
	}
	SCTP_INCR_CHK_COUNT();

	chk->data = m_asconf;
	chk->send_size = m_asconf->m_pkthdr.len;
	chk->rec.chunk_id = SCTP_ASCONF;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	chk->whoTo = chk->asoc->primary_destination;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
	return (0);
}

int
sctp_send_asconf_ack(struct sctp_tcb *stcb, uint32_t retrans)
{
	/*
	 * formulate and queue a asconf-ack back to sender the asconf-ack
	 * must be stored in the tcb
	 */
	struct sctp_tmit_chunk *chk;
	struct mbuf *m_ack;

	SCTP_TCB_LOCK_ASSERT(stcb);
	/* is there a asconf-ack mbuf chain to send? */
	if (stcb->asoc.last_asconf_ack_sent == NULL) {
		return (-1);
	}
	/* copy the asconf_ack */
#if defined(__FreeBSD__) || defined(__NetBSD__)
	/*
	 * Supposedly the m_copypacket is a optimzation, use it if we can.
	 */
	if (stcb->asoc.last_asconf_ack_sent->m_flags & M_PKTHDR) {
		m_ack = m_copypacket(stcb->asoc.last_asconf_ack_sent, M_DONTWAIT);
	} else
		m_ack = m_copy(stcb->asoc.last_asconf_ack_sent, 0, M_COPYALL);
#elif defined(__APPLE__)
	m_ack = sctp_m_copym(stcb->asoc.last_asconf_ack_sent, 0, M_COPYALL, M_DONTWAIT);
#else
	m_ack = m_copy(stcb->asoc.last_asconf_ack_sent, 0, M_COPYALL);
#endif

	if (m_ack == NULL) {
		/* couldn't copy it */

		return (-1);
	}
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		/* no memory */
		if (m_ack)
			sctp_m_freem(m_ack);
		return (-1);
	}
	SCTP_INCR_CHK_COUNT();

	/* figure out where it goes to */
	if (retrans) {
		/* we're doing a retransmission */
		if (stcb->asoc.used_alt_asconfack > 2) {
			/* tried alternate nets already, go back */
			chk->whoTo = NULL;
		} else {
			/* need to try and alternate net */
			chk->whoTo = sctp_find_alternate_net(stcb, stcb->asoc.last_control_chunk_from, 0);
			stcb->asoc.used_alt_asconfack++;
		}
		if (chk->whoTo == NULL) {
			/* no alternate */
			if (stcb->asoc.last_control_chunk_from == NULL)
				chk->whoTo = stcb->asoc.primary_destination;
			else
				chk->whoTo = stcb->asoc.last_control_chunk_from;
			stcb->asoc.used_alt_asconfack = 0;
		}
	} else {
		/* normal case */
		if (stcb->asoc.last_control_chunk_from == NULL)
			chk->whoTo = stcb->asoc.primary_destination;
		else
			chk->whoTo = stcb->asoc.last_control_chunk_from;
		stcb->asoc.used_alt_asconfack = 0;
	}
	chk->data = m_ack;
	chk->send_size = m_ack->m_pkthdr.len;
	chk->rec.chunk_id = SCTP_ASCONF_ACK;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->flags = 0;
	chk->asoc = &stcb->asoc;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	TAILQ_INSERT_TAIL(&chk->asoc->control_send_queue, chk, sctp_next);
	chk->asoc->ctrl_queue_cnt++;
	return (0);
}


static int
sctp_chunk_retransmission(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    int *cnt_out, struct timeval *now, int *now_filled)
{
	/*
	 * send out one MTU of retransmission. If fast_retransmit is
	 * happening we ignore the cwnd. Otherwise we obey the cwnd and
	 * rwnd. For a Cookie or Asconf in the control chunk queue we
	 * retransmit them by themselves.
	 * 
	 * For data chunks we will pick out the lowest TSN's in the sent_queue
	 * marked for resend and bundle them all together (up to a MTU of
	 * destination). The address to send to should have been
	 * selected/changed where the retransmission was marked (i.e. in FR
	 * or t3-timeout routines).
	 */
	struct sctp_tmit_chunk *data_list[SCTP_MAX_DATA_BUNDLING];
	struct sctp_tmit_chunk *chk, *fwd;
	struct mbuf *m, *endofchain;
	struct sctphdr *shdr;
	int asconf;
	struct sctp_nets *net;
	uint32_t tsns_sent = 0;
	int no_fragmentflg, bundle_at, cnt_thru;
	unsigned int mtu;
	int error, i, one_chunk, fwd_tsn, ctl_cnt, tmr_started;
	struct sctp_auth_chunk *auth = NULL;
	uint32_t auth_offset = 0;
	uint32_t dmtu = 0;

	SCTP_TCB_LOCK_ASSERT(stcb);
	tmr_started = ctl_cnt = bundle_at = error = 0;
	no_fragmentflg = 1;
	asconf = 0;
	fwd_tsn = 0;
	*cnt_out = 0;
	fwd = NULL;
	endofchain = m = NULL;
#ifdef SCTP_AUDITING_ENABLED
	sctp_audit_log(0xC3, 1);
#endif
	if (TAILQ_EMPTY(&asoc->sent_queue)) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("SCTP hits empty queue with cnt set to %d?\n",
			    asoc->sent_queue_retran_cnt);
		}
#endif
		asoc->sent_queue_cnt = 0;
		asoc->sent_queue_cnt_removeable = 0;
	}
	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if ((chk->rec.chunk_id == SCTP_COOKIE_ECHO) ||
		    (chk->rec.chunk_id == SCTP_ASCONF) ||
		    (chk->rec.chunk_id == SCTP_STREAM_RESET) ||
		    (chk->rec.chunk_id == SCTP_FORWARD_CUM_TSN)) {
			if (chk->rec.chunk_id == SCTP_STREAM_RESET) {
				if (chk != asoc->str_reset) {
					/*
					 * not eligible for retran if its
					 * not ours
					 */
					continue;
				}
			}
			ctl_cnt++;
			if (chk->rec.chunk_id == SCTP_ASCONF) {
				no_fragmentflg = 1;
				asconf = 1;
			}
			if (chk->rec.chunk_id == SCTP_FORWARD_CUM_TSN) {
				fwd_tsn = 1;
				fwd = chk;
			}
			/*
			 * Add an AUTH chunk, if chunk requires it save the
			 * offset into the chain for AUTH
			 */
			if (auth == NULL) {
				m = sctp_add_auth_chunk(m, &endofchain,
				    &auth, &auth_offset,
				    stcb,
				    chk->rec.chunk_id);
			}
			m = sctp_copy_mbufchain(chk->data, m, &endofchain, 1,
			    0);
			break;
		}
	}
	one_chunk = 0;
	cnt_thru = 0;
	/* do we have control chunks to retransmit? */
	if (m != NULL) {
		/* Start a timer no matter if we suceed or fail */
		if (chk->rec.chunk_id == SCTP_COOKIE_ECHO) {
			sctp_timer_start(SCTP_TIMER_TYPE_COOKIE, inp, stcb, chk->whoTo);
		} else if (chk->rec.chunk_id == SCTP_ASCONF)
			sctp_timer_start(SCTP_TIMER_TYPE_ASCONF, inp, stcb, chk->whoTo);

		M_PREPEND(m, sizeof(struct sctphdr), M_DONTWAIT);
		if (m == NULL) {
			return (ENOBUFS);
		}
		shdr = mtod(m, struct sctphdr *);
		shdr->src_port = inp->sctp_lport;
		shdr->dest_port = stcb->rport;
		shdr->v_tag = htonl(stcb->asoc.peer_vtag);
		shdr->checksum = 0;
		auth_offset += sizeof(struct sctphdr);
		chk->snd_count++;	/* update our count */

		if ((error = sctp_lowlevel_chunk_output(inp, stcb, chk->whoTo,
		    (struct sockaddr *)&chk->whoTo->ro._l_addr, m, auth_offset,
		    auth, no_fragmentflg, 0, NULL, asconf))) {
			SCTP_STAT_INCR(sctps_lowlevelerr);
			return (error);
		}
		m = endofchain = NULL;
		auth = NULL;
		auth_offset = 0;
		/*
		 * We don't want to mark the net->sent time here since this
		 * we use this for HB and retrans cannot measure RTT
		 */
		/* SCTP_GETTIME_TIMEVAL(&chk->whoTo->last_sent_time); */
		*cnt_out += 1;
		chk->sent = SCTP_DATAGRAM_SENT;
		sctp_ucount_decr(asoc->sent_queue_retran_cnt);
		if (fwd_tsn == 0) {
			return (0);
		} else {
			/* Clean up the fwd-tsn list */
			sctp_clean_up_ctl(asoc);
			return (0);
		}
	}
	/*
	 * Ok, it is just data retransmission we need to do or that and a
	 * fwd-tsn with it all.
	 */
	if (TAILQ_EMPTY(&asoc->sent_queue)) {
		return (-1);
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Normal chunk retransmission cnt:%d\n",
		    asoc->sent_queue_retran_cnt);
	}
#endif
	if ((SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_ECHOED) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_WAIT)) {
		/* not yet open, resend the cookie and that is it */
		return (1);
	}
#ifdef SCTP_AUDITING_ENABLED
	sctp_auditing(20, inp, stcb, NULL);
#endif
	TAILQ_FOREACH(chk, &asoc->sent_queue, sctp_next) {
		if (chk->sent != SCTP_DATAGRAM_RESEND) {
			/* No, not sent to this net or not ready for rtx */
			continue;

		}
		/* pick up the net */
		net = chk->whoTo;
		if (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
			mtu = (net->mtu - SCTP_MIN_OVERHEAD);
		} else {
			mtu = net->mtu - SCTP_MIN_V4_OVERHEAD;
		}

		if ((asoc->peers_rwnd < mtu) && (asoc->total_flight > 0)) {
			/* No room in peers rwnd */
			uint32_t tsn;

			tsn = asoc->last_acked_seq + 1;
			if (tsn == chk->rec.data.TSN_seq) {
				/*
				 * we make a special exception for this
				 * case. The peer has no rwnd but is missing
				 * the lowest chunk.. which is probably what
				 * is holding up the rwnd.
				 */
				goto one_chunk_around;
			}
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("blocked-peers_rwnd:%d tf:%d\n",
				    (int)asoc->peers_rwnd,
				    (int)asoc->total_flight);
			}
#endif
			return (1);
		}
one_chunk_around:
		if (asoc->peers_rwnd < mtu) {
			one_chunk = 1;
		}
#ifdef SCTP_AUDITING_ENABLED
		sctp_audit_log(0xC3, 2);
#endif
		bundle_at = 0;
		m = NULL;
		net->fast_retran_ip = 0;
		if (chk->rec.data.doing_fast_retransmit == 0) {
			/*
			 * if no FR in progress skip destination that have
			 * flight_size > cwnd.
			 */
			if (net->flight_size >= net->cwnd) {
				continue;
			}
		} else {
			/*
			 * Mark the destination net to have FR recovery
			 * limits put on it.
			 */
			net->fast_retran_ip = 1;
		}

		/*
		 * if no AUTH is yet included and this chunk requires it,
		 * make sure to account for it.  We don't apply the size
		 * until the AUTH chunk is actually added below in case
		 * there is no room for this chunk.
		 */
		if ((auth == NULL) &&
		    sctp_auth_is_required_chunk(SCTP_DATA,
		    stcb->asoc.peer_auth_chunks)) {
			dmtu = sctp_get_auth_chunk_len(stcb->asoc.peer_hmac_id);
		} else
			dmtu = 0;

		if ((chk->send_size <= (mtu - dmtu)) ||
		    (chk->flags & CHUNK_FLAGS_FRAGMENT_OK)) {
			/* ok we will add this one */
			if (auth == NULL) {
				m = sctp_add_auth_chunk(m, &endofchain,
				    &auth, &auth_offset,
				    stcb, SCTP_DATA);
			}
			m = sctp_copy_mbufchain(chk->data, m, &endofchain, 1,
			    0);
			if (m == NULL) {
				return (ENOMEM);
			}
			/* Do clear IP_DF ? */
			if (chk->flags & CHUNK_FLAGS_FRAGMENT_OK) {
				no_fragmentflg = 0;
			}
			/* upate our MTU size */
			if (mtu > (chk->send_size + dmtu))
				mtu -= (chk->send_size + dmtu);
			else
				mtu = 0;
			data_list[bundle_at++] = chk;
			if (one_chunk && (asoc->total_flight <= 0)) {
				SCTP_STAT_INCR(sctps_windowprobed);
				chk->rec.data.state_flags |= SCTP_WINDOW_PROBE;
			}
		}
		if (one_chunk == 0) {
			/*
			 * now are there anymore forward from chk to pick
			 * up?
			 */
			fwd = TAILQ_NEXT(chk, sctp_next);
			while (fwd) {
				if (fwd->sent != SCTP_DATAGRAM_RESEND) {
					/* Nope, not for retran */
					fwd = TAILQ_NEXT(fwd, sctp_next);
					continue;
				}
				if (fwd->whoTo != net) {
					/* Nope, not the net in question */
					fwd = TAILQ_NEXT(fwd, sctp_next);
					continue;
				}
				if ((auth == NULL) &&
				    sctp_auth_is_required_chunk(SCTP_DATA,
				    stcb->asoc.peer_auth_chunks)) {
					dmtu = sctp_get_auth_chunk_len(stcb->asoc.peer_hmac_id);
				} else
					dmtu = 0;
				if (fwd->send_size <= (mtu - dmtu)) {
					if (auth == NULL) {
						m = sctp_add_auth_chunk(m,
						    &endofchain,
						    &auth, &auth_offset,
						    stcb,
						    SCTP_DATA);
					}
					m = sctp_copy_mbufchain(fwd->data, m,
					    &endofchain, 1,
					    0);
					if (m == NULL) {
						return (ENOMEM);
					}
					/* Do clear IP_DF ? */
					if (fwd->flags & CHUNK_FLAGS_FRAGMENT_OK) {
						no_fragmentflg = 0;
					}
					/* upate our MTU size */
					if (mtu > (fwd->send_size + dmtu))
						mtu -= (fwd->send_size + dmtu);
					else
						mtu = 0;
					data_list[bundle_at++] = fwd;
					if (bundle_at >= SCTP_MAX_DATA_BUNDLING) {
						break;
					}
					fwd = TAILQ_NEXT(fwd, sctp_next);
				} else {
					/* can't fit so we are done */
					break;
				}
			}
		}
		/* Is there something to send for this destination? */
		if (m) {
			/*
			 * No matter if we fail/or suceed we should start a
			 * timer. A failure is like a lost IP packet :-)
			 */
			if (!callout_pending(&net->rxt_timer.timer)) {
				/*
				 * no timer running on this destination
				 * restart it.
				 */
				sctp_timer_start(SCTP_TIMER_TYPE_SEND, inp, stcb, net);
				tmr_started = 1;
			}
			M_PREPEND(m, sizeof(struct sctphdr), M_DONTWAIT);
			if (m == NULL) {
				return (ENOBUFS);
			}
			shdr = mtod(m, struct sctphdr *);
			shdr->src_port = inp->sctp_lport;
			shdr->dest_port = stcb->rport;
			shdr->v_tag = htonl(stcb->asoc.peer_vtag);
			shdr->checksum = 0;
			auth_offset += sizeof(struct sctphdr);
			/* Now lets send it, if there is anything to send :> */
			if ((error = sctp_lowlevel_chunk_output(inp, stcb, net,
			    (struct sockaddr *)&net->ro._l_addr, m, auth_offset,
			    auth, no_fragmentflg, 0, NULL, asconf))) {
				/* error, we could not output */
				SCTP_STAT_INCR(sctps_lowlevelerr);
				return (error);
			}
			m = endofchain = NULL;
			auth = NULL;
			auth_offset = 0;
			/* For HB's */
			/*
			 * We don't want to mark the net->sent time here
			 * since this we use this for HB and retrans cannot
			 * measure RTT
			 */
			/* SCTP_GETTIME_TIMEVAL(&net->last_sent_time); */

			/* For auto-close */
			cnt_thru++;
			if (*now_filled == 0) {
				SCTP_GETTIME_TIMEVAL(&asoc->time_last_sent);
				*now = asoc->time_last_sent;
				*now_filled = 1;
			} else {
				asoc->time_last_sent = *now;
			}
			*cnt_out += bundle_at;
#ifdef SCTP_AUDITING_ENABLED
			sctp_audit_log(0xC4, bundle_at);
#endif
			if (bundle_at) {
				tsns_sent = data_list[0]->rec.data.TSN_seq;
			}
			for (i = 0; i < bundle_at; i++) {
				SCTP_STAT_INCR(sctps_sendretransdata);
				data_list[i]->sent = SCTP_DATAGRAM_SENT;
				/*
				 * When we have a revoked data, and we
				 * retransmit it, then we clear the revoked
				 * flag since this flag dictates if we
				 * subtracted from the fs
				 */
				data_list[i]->rec.data.chunk_was_revoked = 0;
				data_list[i]->snd_count++;
				sctp_ucount_decr(asoc->sent_queue_retran_cnt);
				/* record the time */
				data_list[i]->sent_rcv_time = asoc->time_last_sent;
				if (asoc->sent_queue_retran_cnt < 0) {
					asoc->sent_queue_retran_cnt = 0;
				}
				net->flight_size += data_list[i]->book_size;
				asoc->total_flight += data_list[i]->book_size;
				if (data_list[i]->book_size_scale) {
					/*
					 * need to double the book size on
					 * this one
					 */
					data_list[i]->book_size_scale = 0;
					data_list[i]->book_size *= 2;
				} else {
					sctp_ucount_incr(asoc->total_flight_count);
#ifdef SCTP_LOG_RWND
					sctp_log_rwnd(SCTP_DECREASE_PEER_RWND,
					    asoc->peers_rwnd, data_list[i]->send_size, sctp_peer_chunk_oh);
#endif
					asoc->peers_rwnd = sctp_sbspace_sub(asoc->peers_rwnd,
					    (uint32_t) (data_list[i]->send_size +
					    sctp_peer_chunk_oh));
				}
				if (asoc->peers_rwnd < stcb->sctp_ep->sctp_ep.sctp_sws_sender) {
					/* SWS sender side engages */
					asoc->peers_rwnd = 0;
				}
				if ((i == 0) &&
				    (data_list[i]->rec.data.doing_fast_retransmit)) {
					SCTP_STAT_INCR(sctps_sendfastretrans);
					if ((data_list[i] == TAILQ_FIRST(&asoc->sent_queue)) &&
					    (tmr_started == 0)) {
						/*
						 * ok we just fast-retrans'd
						 * the lowest TSN, i.e the
						 * first on the list. In
						 * this case we want to give
						 * some more time to get a
						 * SACK back without a
						 * t3-expiring.
						 */
						sctp_timer_stop(SCTP_TIMER_TYPE_SEND, inp, stcb, net);
						sctp_timer_start(SCTP_TIMER_TYPE_SEND, inp, stcb, net);
					}
				}
			}
#ifdef SCTP_CWND_LOGGING
			sctp_log_cwnd(stcb, net, tsns_sent, SCTP_CWND_LOG_FROM_RESEND);
#endif
#ifdef SCTP_AUDITING_ENABLED
			sctp_auditing(21, inp, stcb, NULL);
#endif
		} else {
			/* None will fit */
			return (1);
		}
		if (asoc->sent_queue_retran_cnt <= 0) {
			/* all done we have no more to retran */
			asoc->sent_queue_retran_cnt = 0;
			break;
		}
		if (one_chunk) {
			/* No more room in rwnd */
			return (1);
		}
		/* stop the for loop here. we sent out a packet */
		break;
	}
	return (0);
}


static int
sctp_timer_validation(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    int ret)
{
	struct sctp_nets *net;

	/* Validate that a timer is running somewhere */
	TAILQ_FOREACH(net, &asoc->nets, sctp_next) {
		if (callout_pending(&net->rxt_timer.timer)) {
			/* Here is a timer */
			return (ret);
		}
	}
	SCTP_TCB_LOCK_ASSERT(stcb);
	/* Gak, we did not have a timer somewhere */
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
		printf("Deadlock avoided starting timer on a dest at retran\n");
	}
#endif
	sctp_timer_start(SCTP_TIMER_TYPE_SEND, inp, stcb, asoc->primary_destination);
	return (ret);
}

int
sctp_chunk_output(struct sctp_inpcb *inp,
    struct sctp_tcb *stcb,
    int from_where)
{
	/*
	 * Ok this is the generic chunk service queue. we must do the
	 * following: - See if there are retransmits pending, if so we must
	 * do these first and return. - Service the stream queue that is
	 * next, moving any message (note I must get a complete message i.e.
	 * FIRST/MIDDLE and LAST to the out queue in one pass) and assigning
	 * TSN's - Check to see if the cwnd/rwnd allows any output, if so we
	 * go ahead and fomulate and send the low level chunks. Making sure
	 * to combine any control in the control chunk queue also.
	 */
	struct sctp_association *asoc;
	struct sctp_nets *net;
	int error, num_out, tot_out, ret, reason_code, burst_cnt, burst_limit;
	struct timeval now;
	int now_filled = 0;
	int cwnd_full = 0;
	int nagle_on = 0;

	asoc = &stcb->asoc;
	tot_out = 0;
	num_out = 0;
	reason_code = 0;
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
		printf("in co - retran count:%d\n", asoc->sent_queue_retran_cnt);
	}
#endif
	if(from_where == SCTP_OUTPUT_FROM_USR_SEND) {
		if(sctp_is_feature_on(inp, SCTP_PCB_FLAGS_NODELAY)) {
			nagle_on = 0;
		} else {
			nagle_on = 1;
		}
	}
	SCTP_TCB_LOCK_ASSERT(stcb);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	sctp_lock_assert(inp->ip_inp.inp.inp_socket);
#endif
	while (asoc->sent_queue_retran_cnt) {
		/*
		 * Ok, it is retransmission time only, we send out only ONE
		 * packet with a single call off to the retran code.
		 */
		if (from_where != SCTP_OUTPUT_FROM_HB_TMR) {
			/* if its not from a HB then do it */
			ret = sctp_chunk_retransmission(inp, stcb, asoc, &num_out, &now, &now_filled);
		} else {
			/*
			 * its from any other place, we don't allow retran
			 * output (only control)
			 */
			ret = 1;
		}
		if (ret > 0) {
			/* Can't send anymore */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("retransmission ret:%d -- full\n", ret);
			}
#endif
			/*
			 * now lets push out control by calling med-level
			 * output once. this assures that we WILL send HB's
			 * if queued too.
			 */
			(void)sctp_med_chunk_output(inp, stcb, asoc, &num_out, &reason_code, 1,
			    &cwnd_full, from_where,
			    &now, &now_filled);
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Control send outputs:%d@full\n", num_out);
			}
#endif
#ifdef SCTP_AUDITING_ENABLED
			sctp_auditing(8, inp, stcb, NULL);
#endif
			return (sctp_timer_validation(inp, stcb, asoc, ret));
		}
		if (ret < 0) {
			/*
			 * The count was off.. retran is not happening so do
			 * the normal retransmission.
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Done with retrans, none left fill up window\n");
			}
#endif
#ifdef SCTP_AUDITING_ENABLED
			sctp_auditing(9, inp, stcb, NULL);
#endif
			break;
		}
		if (from_where == SCTP_OUTPUT_FROM_T3) {
			/* Only one transmission allowed out of a timeout */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Only one packet allowed out\n");
			}
#endif
#ifdef SCTP_AUDITING_ENABLED
			sctp_auditing(10, inp, stcb, NULL);
#endif
			/* Push out any control */
			(void)sctp_med_chunk_output(inp, stcb, asoc, &num_out, &reason_code, 1, &cwnd_full, from_where,
			    &now, &now_filled);
			return (ret);
		}
		if ((num_out == 0) && (ret == 0)) {
			/* No more retrans to send */
			break;
		}
	}
#ifdef SCTP_AUDITING_ENABLED
	sctp_auditing(12, inp, stcb, NULL);
#endif
	/* Check for bad destinations, if they exist move chunks around. */
	burst_limit = asoc->max_burst;
	TAILQ_FOREACH(net, &asoc->nets, sctp_next) {
		if ((net->dest_state & SCTP_ADDR_NOT_REACHABLE) ==
		    SCTP_ADDR_NOT_REACHABLE) {
			/*
			 * if possible move things off of this address we
			 * still may send below due to the dormant state but
			 * we try to find an alternate address to send to
			 * and if we have one we move all queued data on the
			 * out wheel to this alternate address.
			 */
			if (net->ref_count > 1)
				sctp_move_to_an_alt(stcb, asoc, net);
		} else {
			/*
			 * if ((asoc->sat_network) || (net->addr_is_local))
			 * { burst_limit = asoc->max_burst *
			 * SCTP_SAT_NETWORK_BURST_INCR; }
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
				printf("examined net:%p burst limit:%d\n", net, asoc->max_burst);
			}
#endif

			if (sctp_use_cwnd_based_maxburst) {
				if ((net->flight_size + (burst_limit * net->mtu)) < net->cwnd) {
					int old_cwnd;

					if (net->ssthresh < net->cwnd)
						net->ssthresh = net->cwnd;
					old_cwnd = net->cwnd;
					net->cwnd = (net->flight_size + (burst_limit * net->mtu));

#ifdef SCTP_CWND_MONITOR
					sctp_log_cwnd(stcb, net, (net->cwnd - old_cwnd), SCTP_CWND_LOG_FROM_BRST);
#endif

#ifdef SCTP_LOG_MAXBURST
					sctp_log_maxburst(stcb, net, 0, burst_limit, SCTP_MAX_BURST_APPLIED);
#endif
					SCTP_STAT_INCR(sctps_maxburstqueued);
				}
				net->fast_retran_ip = 0;
			} else {
				if (net->flight_size == 0) {
					/* Should be decaying the cwnd here */
					;
				}
			}
		}

	}
	burst_cnt = 0;
	cwnd_full = 0;
	do {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("Burst count:%d - call m-c-o\n", burst_cnt);
		}
#endif
		error = sctp_med_chunk_output(inp, stcb, asoc, &num_out,
		    &reason_code, 0, &cwnd_full, from_where,
		    &now, &now_filled);
		if (error) {
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
				printf("Error %d was returned from med-c-op\n", error);
			}
#endif
#ifdef SCTP_LOG_MAXBURST
			sctp_log_maxburst(stcb, asoc->primary_destination, error, burst_cnt, SCTP_MAX_BURST_ERROR_STOP);
#endif
#ifdef SCTP_CWND_LOGGING
			sctp_log_cwnd(stcb, NULL, error, SCTP_SEND_NOW_COMPLETES);
			sctp_log_cwnd(stcb, NULL, 0xdeadbeef, SCTP_SEND_NOW_COMPLETES);
#endif

			break;
		}
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT3) {
			printf("m-c-o put out %d\n", num_out);
		}
#endif
		tot_out += num_out;
		burst_cnt++;
#ifdef SCTP_CWND_LOGGING
		sctp_log_cwnd(stcb, NULL, num_out, SCTP_SEND_NOW_COMPLETES);
		if (num_out == 0) {
			sctp_log_cwnd(stcb, NULL, reason_code, SCTP_SEND_NOW_COMPLETES);
		}
#endif
		if (nagle_on) {
			int un_sent;
			/* When nagle is on, we look at how much is un_sent, then
			 * if its smaller than an MTU and we have data in
			 * flight we stop.
			 */
			un_sent = ((stcb->asoc.total_output_queue_size - stcb->asoc.total_flight) +
				   ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) 
				    * sizeof(struct sctp_data_chunk)));
			if((un_sent < (int)(stcb->asoc.smallest_mtu - SCTP_MIN_OVERHEAD)) &&
			   (stcb->asoc.total_flight > 0)){
				break;
			}
		}
	} while (num_out && (sctp_use_cwnd_based_maxburst ||
	    (burst_cnt < burst_limit)));

	if (sctp_use_cwnd_based_maxburst == 0) {
		if (burst_cnt >= burst_limit) {
			SCTP_STAT_INCR(sctps_maxburstqueued);
			asoc->burst_limit_applied = 1;
#ifdef SCTP_LOG_MAXBURST
			sctp_log_maxburst(stcb, asoc->primary_destination, 0, burst_cnt, SCTP_MAX_BURST_APPLIED);
#endif
		} else {
			asoc->burst_limit_applied = 0;
		}
	}
#ifdef SCTP_CWND_LOGGING
	sctp_log_cwnd(stcb, NULL, tot_out, SCTP_SEND_NOW_COMPLETES);
#endif
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("Ok, we have put out %d chunks\n", tot_out);
	}
#endif
	/*
	 * Now we need to clean up the control chunk chain if a ECNE is on
	 * it. It must be marked as UNSENT again so next call will continue
	 * to send it until such time that we get a CWR, to remove it.
	 */
	sctp_fix_ecn_echo(asoc);
	return (error);
}


int
sctp_output(inp, m, addr, control, p, flags)
	struct sctp_inpcb *inp;
	struct mbuf *m;
	struct sockaddr *addr;
	struct mbuf *control;

#if defined(__FreeBSD__) && __FreeBSD_version >= 500000
	struct thread *p;
#else
	struct proc *p;
#endif
	int flags;
{
	if(inp == NULL) {
		return(EINVAL);
	}

	if(inp->sctp_socket == NULL) {
		return(EINVAL);
	}
	return(sctp_sosend(inp->sctp_socket,
		    addr,
		    (struct uio *)NULL,
		    m,
		    control,
		    flags,
		    p));
}

void
send_forward_tsn(struct sctp_tcb *stcb,
		 struct sctp_association *asoc)
{
	struct sctp_tmit_chunk *chk;
	struct sctp_forward_tsn_chunk *fwdtsn;

	SCTP_TCB_LOCK_ASSERT(stcb);
	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if (chk->rec.chunk_id == SCTP_FORWARD_CUM_TSN) {
			/* mark it to unsent */
			chk->sent = SCTP_DATAGRAM_UNSENT;
			chk->snd_count = 0;
			/* Do we correct its output location? */
			if (chk->whoTo != asoc->primary_destination) {
				sctp_free_remote_addr(chk->whoTo);
				chk->whoTo = asoc->primary_destination;
				atomic_add_int(&chk->whoTo->ref_count, 1);
			}
			goto sctp_fill_in_rest;
		}
	}
	/* Ok if we reach here we must build one */
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		return;
	}
	SCTP_INCR_CHK_COUNT();
	chk->rec.chunk_id = SCTP_FORWARD_CUM_TSN;
	chk->asoc = asoc;
	chk->data = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if (chk->data == NULL) {
		atomic_subtract_int(&chk->whoTo->ref_count, 1);
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return;
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->whoTo = asoc->primary_destination;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	TAILQ_INSERT_TAIL(&asoc->control_send_queue, chk, sctp_next);
	asoc->ctrl_queue_cnt++;
sctp_fill_in_rest:
	/*
	 * Here we go through and fill out the part that deals with
	 * stream/seq of the ones we skip.
	 */
	chk->data->m_pkthdr.len = chk->data->m_len = 0;
	{
		struct sctp_tmit_chunk *at, *tp1, *last;
		struct sctp_strseq *strseq;
		unsigned int cnt_of_space, i, ovh;
		unsigned int space_needed;
		unsigned int cnt_of_skipped = 0;

		TAILQ_FOREACH(at, &asoc->sent_queue, sctp_next) {
			if (at->sent != SCTP_FORWARD_TSN_SKIP) {
				/* no more to look at */
				break;
			}
			if (at->rec.data.rcv_flags & SCTP_DATA_UNORDERED) {
				/* We don't report these */
				continue;
			}
			cnt_of_skipped++;
		}
		space_needed = (sizeof(struct sctp_forward_tsn_chunk) +
		    (cnt_of_skipped * sizeof(struct sctp_strseq)));

		cnt_of_space = M_TRAILINGSPACE(chk->data);

		if (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) {
			ovh = SCTP_MIN_OVERHEAD;
		} else {
			ovh = SCTP_MIN_V4_OVERHEAD;
		}
		if (cnt_of_space > (asoc->smallest_mtu - ovh)) {
			/* trim to a mtu size */
			cnt_of_space = asoc->smallest_mtu - ovh;
		}
		if (cnt_of_space < space_needed) {
			/*
			 * ok we must trim down the chunk by lowering the
			 * advance peer ack point.
			 */
			cnt_of_skipped = (cnt_of_space -
			    ((sizeof(struct sctp_forward_tsn_chunk)) /
			    sizeof(struct sctp_strseq)));
			/*
			 * Go through and find the TSN that will be the one
			 * we report.
			 */
			at = TAILQ_FIRST(&asoc->sent_queue);
			for (i = 0; i < cnt_of_skipped; i++) {
				tp1 = TAILQ_NEXT(at, sctp_next);
				at = tp1;
			}
			last = at;
			/*
			 * last now points to last one I can report, update
			 * peer ack point
			 */
			asoc->advanced_peer_ack_point = last->rec.data.TSN_seq;
			space_needed -= (cnt_of_skipped * sizeof(struct sctp_strseq));
		}
		chk->send_size = space_needed;
		/* Setup the chunk */
		fwdtsn = mtod(chk->data, struct sctp_forward_tsn_chunk *);
		fwdtsn->ch.chunk_length = htons(chk->send_size);
		fwdtsn->ch.chunk_flags = 0;
		fwdtsn->ch.chunk_type = SCTP_FORWARD_CUM_TSN;
		fwdtsn->new_cumulative_tsn = htonl(asoc->advanced_peer_ack_point);
		chk->send_size = (sizeof(struct sctp_forward_tsn_chunk) +
		    (cnt_of_skipped * sizeof(struct sctp_strseq)));
		chk->data->m_pkthdr.len = chk->data->m_len = chk->send_size;
		fwdtsn++;
		/*
		 * Move pointer to after the fwdtsn and transfer to the
		 * strseq pointer.
		 */
		strseq = (struct sctp_strseq *)fwdtsn;
		/*
		 * Now populate the strseq list. This is done blindly
		 * without pulling out duplicate stream info. This is
		 * inefficent but won't harm the process since the peer will
		 * look at these in sequence and will thus release anything.
		 * It could mean we exceed the PMTU and chop off some that
		 * we could have included.. but this is unlikely (aka 1432/4
		 * would mean 300+ stream seq's would have to be reported in
		 * one FWD-TSN. With a bit of work we can later FIX this to
		 * optimize and pull out duplcates.. but it does add more
		 * overhead. So for now... not!
		 */
		at = TAILQ_FIRST(&asoc->sent_queue);
		for (i = 0; i < cnt_of_skipped; i++) {
			tp1 = TAILQ_NEXT(at, sctp_next);
			if (at->rec.data.rcv_flags & SCTP_DATA_UNORDERED) {
				/* We don't report these */
				i--;
				at = tp1;
				continue;
			}
			strseq->stream = ntohs(at->rec.data.stream_number);
			strseq->sequence = ntohs(at->rec.data.stream_seq);
			strseq++;
			at = tp1;
		}
	}
	return;

}

void
sctp_send_sack(struct sctp_tcb *stcb)
{
	/*
	 * Queue up a SACK in the control queue. We must first check to see
	 * if a SACK is somehow on the control queue. If so, we will take
	 * and and remove the old one.
	 */
	struct sctp_association *asoc;
	struct sctp_tmit_chunk *chk, *a_chk;
	struct sctp_sack_chunk *sack;
	struct sctp_gap_ack_block *gap_descriptor;
	struct sack_track *selector;
	int mergeable = 0;
	int offset;
	caddr_t limit;
	uint32_t *dup;
	int limit_reached = 0;
	unsigned int i, jstart, siz, j;
	unsigned int num_gap_blocks = 0, space;
	int num_dups = 0;

	a_chk = NULL;
	asoc = &stcb->asoc;
	SCTP_TCB_LOCK_ASSERT(stcb);
	if (asoc->last_data_chunk_from == NULL) {
		/* Hmm we never received anything */
		return;
	}
	sctp_set_rwnd(stcb, asoc);
	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if (chk->rec.chunk_id == SCTP_SELECTIVE_ACK) {
			/* Hmm, found a sack already on queue, remove it */
			TAILQ_REMOVE(&asoc->control_send_queue, chk, sctp_next);
			asoc->ctrl_queue_cnt++;
			a_chk = chk;
			if (a_chk->data)
				sctp_m_freem(a_chk->data);
			a_chk->data = NULL;
			sctp_free_remote_addr(a_chk->whoTo);
			a_chk->whoTo = NULL;
			break;
		}
	}
	if (a_chk == NULL) {
		a_chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
		if (a_chk == NULL) {
			/* No memory so we drop the idea, and set a timer */
			sctp_timer_stop(SCTP_TIMER_TYPE_RECV,
			    stcb->sctp_ep, stcb, NULL);
			sctp_timer_start(SCTP_TIMER_TYPE_RECV,
			    stcb->sctp_ep, stcb, NULL);
			return;
		}
		SCTP_INCR_CHK_COUNT();
		a_chk->rec.chunk_id = SCTP_SELECTIVE_ACK;
	}
	a_chk->asoc = asoc;
	a_chk->snd_count = 0;
	a_chk->send_size = 0;	/* fill in later */
	a_chk->sent = SCTP_DATAGRAM_UNSENT;

	if ((asoc->numduptsns) ||
	    (asoc->last_data_chunk_from->dest_state & SCTP_ADDR_NOT_REACHABLE)
	    ) {
		/*
		 * Ok, we have some duplicates or the destination for the
		 * sack is unreachable, lets see if we can select an
		 * alternate than asoc->last_data_chunk_from
		 */
		if ((!(asoc->last_data_chunk_from->dest_state &
		    SCTP_ADDR_NOT_REACHABLE)) &&
		    (asoc->used_alt_onsack > asoc->numnets)) {
			/* We used an alt last time, don't this time */
			a_chk->whoTo = NULL;
		} else {
			asoc->used_alt_onsack++;
			a_chk->whoTo = sctp_find_alternate_net(stcb, asoc->last_data_chunk_from, 0);
		}
		if (a_chk->whoTo == NULL) {
			/* Nope, no alternate */
			a_chk->whoTo = asoc->last_data_chunk_from;
			asoc->used_alt_onsack = 0;
		}
	} else {
		/*
		 * No duplicates so we use the last place we received data
		 * from.
		 */
#ifdef SCTP_DEBUG
		if (asoc->last_data_chunk_from == NULL) {
			printf("Huh, last_data_chunk_from is null when we want to sack??\n");
		}
#endif
		asoc->used_alt_onsack = 0;
		a_chk->whoTo = asoc->last_data_chunk_from;
	}
	if (a_chk->whoTo) {
		atomic_add_int(&a_chk->whoTo->ref_count, 1);
	}
	/* Ok now lets formulate a MBUF with our sack */
	a_chk->data = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if ((a_chk->data == NULL) ||
	    (a_chk->whoTo == NULL)) {
		/* rats, no mbuf memory */
		if (a_chk->data) {
			/* was a problem with the destination */
			sctp_m_freem(a_chk->data);
			a_chk->data = NULL;
		}
		atomic_subtract_int(&a_chk->whoTo->ref_count, 1);
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, a_chk);
		SCTP_DECR_CHK_COUNT();
		sctp_timer_stop(SCTP_TIMER_TYPE_RECV,
		    stcb->sctp_ep, stcb, NULL);
		sctp_timer_start(SCTP_TIMER_TYPE_RECV,
		    stcb->sctp_ep, stcb, NULL);
		return;
	}
	/* ok, lets go through and fill it in */
	a_chk->data->m_data += SCTP_MIN_OVERHEAD;
	space = M_TRAILINGSPACE(a_chk->data);
	if (space > (a_chk->whoTo->mtu - SCTP_MIN_OVERHEAD)) {
		space = (a_chk->whoTo->mtu - SCTP_MIN_OVERHEAD);
	}
	limit = mtod(a_chk->data, caddr_t);
	limit += space;

	sack = mtod(a_chk->data, struct sctp_sack_chunk *);
	sack->ch.chunk_type = SCTP_SELECTIVE_ACK;
	/* 0x01 is used by nonce for ecn */
	sack->ch.chunk_flags = (asoc->receiver_nonce_sum & SCTP_SACK_NONCE_SUM);
	if (sctp_cmt_on_off && sctp_cmt_use_dac) {
		/*
		 * CMT DAC algorithm: If 2 (i.e., 0x10) packets have been
		 * received, then set high bit to 1, else 0. Reset
		 * pkts_rcvd.
		 */
		sack->ch.chunk_flags |= (asoc->cmt_dac_pkts_rcvd << 6);
		asoc->cmt_dac_pkts_rcvd = 0;
	}
	sack->sack.cum_tsn_ack = htonl(asoc->cumulative_tsn);
	sack->sack.a_rwnd = htonl(asoc->my_rwnd);
	asoc->my_last_reported_rwnd = asoc->my_rwnd;
	gap_descriptor = (struct sctp_gap_ack_block *)((caddr_t)sack + sizeof(struct sctp_sack_chunk));


	siz = (((asoc->highest_tsn_inside_map - asoc->mapping_array_base_tsn) + 1) + 7) / 8;
	if (asoc->cumulative_tsn < asoc->mapping_array_base_tsn) {
		offset = 1;
		/*
		 * cum-ack behind the mapping array, so we start and use all
		 * entries.
		 */
		jstart = 0;
	} else {
		offset = asoc->mapping_array_base_tsn - asoc->cumulative_tsn;
		/*
		 * we skip the first one when the cum-ack is at or above the
		 * mapping array base.
		 */
		jstart = 1;
	}
	if (compare_with_wrap(asoc->highest_tsn_inside_map, asoc->cumulative_tsn, MAX_TSN)) {
		/* we have a gap .. maybe */
		for (i = 0; i < siz; i++) {
			selector = &sack_array[asoc->mapping_array[i]];
			if (mergeable && selector->right_edge) {
				/*
				 * Backup, left and right edges were ok to
				 * merge.
				 */
				num_gap_blocks--;
				gap_descriptor--;
			}
			if (selector->num_entries == 0)
				mergeable = 0;
			else {
				for (j = jstart; j < selector->num_entries; j++) {
					if (mergeable && selector->right_edge) {
						/*
						 * do a merge by NOT setting
						 * the left side
						 */
						mergeable = 0;
					} else {
						/*
						 * no merge, set the left
						 * side
						 */
						mergeable = 0;
						gap_descriptor->start = htons((selector->gaps[j].start + offset));
					}
					gap_descriptor->end = htons((selector->gaps[j].end + offset));
					num_gap_blocks++;
					gap_descriptor++;
					if (((caddr_t)gap_descriptor + sizeof(struct sctp_gap_ack_block)) > limit) {
						/* no more room */
						limit_reached = 1;
						break;
					}
				}
				if (selector->left_edge) {
					mergeable = 1;
				}
			}
			jstart = 0;
			offset += 8;
		}
		if (num_gap_blocks == 0) {
			/* reneged all chunks */
			asoc->highest_tsn_inside_map = asoc->cumulative_tsn;
		}
	}
	/* now we must add any dups we are going to report. */
	if ((limit_reached == 0) && (asoc->numduptsns)) {
		dup = (uint32_t *) gap_descriptor;
		for (i = 0; i < asoc->numduptsns; i++) {
			*dup = htonl(asoc->dup_tsns[i]);
			dup++;
			num_dups++;
			if (((caddr_t)dup + sizeof(uint32_t)) > limit) {
				/* no more room */
				break;
			}
		}
		asoc->numduptsns = 0;
	}
	/*
	 * now that the chunk is prepared queue it to the control chunk
	 * queue.
	 */
	a_chk->send_size = (sizeof(struct sctp_sack_chunk) +
	    (num_gap_blocks * sizeof(struct sctp_gap_ack_block)) +
	    (num_dups * sizeof(int32_t)));
	a_chk->data->m_pkthdr.len = a_chk->data->m_len = a_chk->send_size;
	sack->sack.num_gap_ack_blks = htons(num_gap_blocks);
	sack->sack.num_dup_tsns = htons(num_dups);
	sack->ch.chunk_length = htons(a_chk->send_size);
	TAILQ_INSERT_TAIL(&asoc->control_send_queue, a_chk, sctp_next);
	asoc->ctrl_queue_cnt++;
	SCTP_STAT_INCR(sctps_sendsacks);
	return;
}


void
sctp_send_abort_tcb(struct sctp_tcb *stcb, struct mbuf *operr)
{
	struct mbuf *m_abort;
	struct mbuf *m_out = NULL, *m_end = NULL;
	struct sctp_abort_chunk *abort = NULL;
	int sz;
	uint32_t auth_offset = 0;
	struct sctp_auth_chunk *auth = NULL;
	struct sctphdr *shdr;

	/*
	 * Add an AUTH chunk, if chunk requires it and save the offset into
	 * the chain for AUTH
	 */
	m_out = sctp_add_auth_chunk(m_out, &m_end, &auth, &auth_offset,
	    stcb, SCTP_ABORT_ASSOCIATION);

	SCTP_TCB_LOCK_ASSERT(stcb);
	m_abort = sctp_get_mbuf_for_msg(sizeof(struct sctp_abort_chunk), 1, M_DONTWAIT, 1, MT_HEADER);
	if (m_abort == NULL) {
		/* no mbuf's */
		if (m_out)
			sctp_m_freem(m_out);
		return;
	}
	/* link in any error */
	m_abort->m_next = operr;
	sz = 0;
	if (operr) {
		struct mbuf *n;

		n = operr;
		while (n) {
			sz += n->m_len;
			n = n->m_next;
		}
	}
	m_abort->m_len = sizeof(*abort);
	m_abort->m_pkthdr.len = m_abort->m_len + sz;
	m_abort->m_pkthdr.rcvif = 0;
	if (m_out == NULL) {
		/* NO Auth chunk prepended, so reserve space in front */
		m_abort->m_data += SCTP_MIN_OVERHEAD;
		m_out = m_abort;
	} else {
		/* Put AUTH chunk at the front of the chain */
		m_out->m_pkthdr.len += m_abort->m_pkthdr.len;
		m_end->m_next = m_abort;
	}

	/* fill in the ABORT chunk */
	abort = mtod(m_abort, struct sctp_abort_chunk *);
	abort->ch.chunk_type = SCTP_ABORT_ASSOCIATION;
	abort->ch.chunk_flags = 0;
	abort->ch.chunk_length = htons(sizeof(*abort) + sz);

	/* prepend and fill in the SCTP header */
	M_PREPEND(m_out, sizeof(struct sctphdr), M_DONTWAIT);
	if (m_out == NULL) {
		/* TSNH: no memory */
		return;
	}
	shdr = mtod(m_out, struct sctphdr *);
	shdr->src_port = stcb->sctp_ep->sctp_lport;
	shdr->dest_port = stcb->rport;
	shdr->v_tag = htonl(stcb->asoc.peer_vtag);
	shdr->checksum = 0;
	auth_offset += sizeof(struct sctphdr);

	sctp_lowlevel_chunk_output(stcb->sctp_ep, stcb,
	    stcb->asoc.primary_destination,
	    (struct sockaddr *)&stcb->asoc.primary_destination->ro._l_addr,
	    m_out, auth_offset, auth, 1, 0, NULL, 0);
}

int
sctp_send_shutdown_complete(struct sctp_tcb *stcb,
    struct sctp_nets *net)
{
	/* formulate and SEND a SHUTDOWN-COMPLETE */
	struct mbuf *m_shutdown_comp;
	struct sctp_shutdown_complete_msg *comp_cp;

	m_shutdown_comp = sctp_get_mbuf_for_msg(sizeof(struct sctp_shutdown_complete_msg), 1, M_DONTWAIT, 1, MT_HEADER);
	if (m_shutdown_comp == NULL) {
		/* no mbuf's */
		return (-1);
	}
	m_shutdown_comp->m_data += sizeof(struct ip6_hdr);
	comp_cp = mtod(m_shutdown_comp, struct sctp_shutdown_complete_msg *);
	comp_cp->shut_cmp.ch.chunk_type = SCTP_SHUTDOWN_COMPLETE;
	comp_cp->shut_cmp.ch.chunk_flags = 0;
	comp_cp->shut_cmp.ch.chunk_length = htons(sizeof(struct sctp_shutdown_complete_chunk));
	comp_cp->sh.src_port = stcb->sctp_ep->sctp_lport;
	comp_cp->sh.dest_port = stcb->rport;
	comp_cp->sh.v_tag = htonl(stcb->asoc.peer_vtag);
	comp_cp->sh.checksum = 0;

	m_shutdown_comp->m_pkthdr.len = m_shutdown_comp->m_len = sizeof(struct sctp_shutdown_complete_msg);
	m_shutdown_comp->m_pkthdr.rcvif = 0;
	sctp_lowlevel_chunk_output(stcb->sctp_ep, stcb, net,
	    (struct sockaddr *)&net->ro._l_addr,
	    m_shutdown_comp, 0, NULL, 1, 0, NULL, 0);
	if ((stcb->sctp_socket) && 
	    (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_TCPTYPE) &&
	    (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_CONNECTED)) {
		stcb->sctp_ep->sctp_flags &= ~SCTP_PCB_FLAGS_CONNECTED;
		stcb->sctp_ep->sctp_socket->so_snd.sb_cc = 0;
		if (((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_SOCKET_ALLGONE) == 0) &&
		    ((stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_SOCKET_GONE) == 0))
			soisdisconnected(stcb->sctp_ep->sctp_socket);
		/*
		 * else printf("SCTP(c):disconnect of unref socket\n");
		 */
	}
	return (0);
}

int
sctp_send_shutdown_complete2(struct mbuf *m, int iphlen, struct sctphdr *sh)
{
	/* formulate and SEND a SHUTDOWN-COMPLETE */
	struct mbuf *mout;
	struct ip *iph, *iph_out;
	struct ip6_hdr *ip6, *ip6_out;
	int offset_out;
	struct sctp_shutdown_complete_msg *comp_cp;

	mout = sctp_get_mbuf_for_msg(sizeof(struct sctp_shutdown_complete_msg), 1, M_DONTWAIT, 1, MT_HEADER);
	if (mout == NULL) {
		/* no mbuf's */
		return (-1);
	}
	iph = mtod(m, struct ip *);
	iph_out = NULL;
	ip6_out = NULL;
	offset_out = 0;
	if (iph->ip_v == IPVERSION) {
		mout->m_len = sizeof(struct ip) +
		    sizeof(struct sctp_shutdown_complete_msg);
		mout->m_next = NULL;
		iph_out = mtod(mout, struct ip *);

		/* Fill in the IP header for the ABORT */
		iph_out->ip_v = IPVERSION;
		iph_out->ip_hl = (sizeof(struct ip) / 4);
		iph_out->ip_tos = (u_char)0;
		iph_out->ip_id = 0;
		iph_out->ip_off = 0;
		iph_out->ip_ttl = MAXTTL;
		iph_out->ip_p = IPPROTO_SCTP;
		iph_out->ip_src.s_addr = iph->ip_dst.s_addr;
		iph_out->ip_dst.s_addr = iph->ip_src.s_addr;

		/* let IP layer calculate this */
		iph_out->ip_sum = 0;
		offset_out += sizeof(*iph_out);
		comp_cp = (struct sctp_shutdown_complete_msg *)(
		    (caddr_t)iph_out + offset_out);
	} else if (iph->ip_v == (IPV6_VERSION >> 4)) {
		ip6 = (struct ip6_hdr *)iph;
		mout->m_len = sizeof(struct ip6_hdr) +
		    sizeof(struct sctp_shutdown_complete_msg);
		mout->m_next = NULL;
		ip6_out = mtod(mout, struct ip6_hdr *);

		/* Fill in the IPv6 header for the ABORT */
		ip6_out->ip6_flow = ip6->ip6_flow;
		ip6_out->ip6_hlim = ip6_defhlim;
		ip6_out->ip6_nxt = IPPROTO_SCTP;
		ip6_out->ip6_src = ip6->ip6_dst;
		ip6_out->ip6_dst = ip6->ip6_src;
		ip6_out->ip6_plen = mout->m_len;
		offset_out += sizeof(*ip6_out);
		comp_cp = (struct sctp_shutdown_complete_msg *)(
		    (caddr_t)ip6_out + offset_out);
	} else {
		/* Currently not supported. */
		return (-1);
	}

	/* Now copy in and fill in the ABORT tags etc. */
	comp_cp->sh.src_port = sh->dest_port;
	comp_cp->sh.dest_port = sh->src_port;
	comp_cp->sh.checksum = 0;
	comp_cp->sh.v_tag = sh->v_tag;
	comp_cp->shut_cmp.ch.chunk_flags = SCTP_HAD_NO_TCB;
	comp_cp->shut_cmp.ch.chunk_type = SCTP_SHUTDOWN_COMPLETE;
	comp_cp->shut_cmp.ch.chunk_length = htons(sizeof(struct sctp_shutdown_complete_chunk));

	mout->m_pkthdr.len = mout->m_len;
	/* add checksum */
	if ((sctp_no_csum_on_loopback) &&
	    (m->m_pkthdr.rcvif) &&
	    (m->m_pkthdr.rcvif->if_type == IFT_LOOP)) {
		comp_cp->sh.checksum = 0;
	} else {
		comp_cp->sh.checksum = sctp_calculate_sum(mout, NULL, offset_out);
	}

	/* zap the rcvif, it should be null */
	mout->m_pkthdr.rcvif = 0;
	/* zap the stack pointer to the route */
	if (iph_out != NULL) {
		struct route ro;

		bzero(&ro, sizeof ro);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
			printf("sctp_shutdown_complete2 calling ip_output:\n");
			sctp_print_address_pkt(iph_out, &comp_cp->sh);
		}
#endif
		/* set IPv4 length */
#if defined(__FreeBSD__) || defined (__APPLE__)
		iph_out->ip_len = mout->m_pkthdr.len;
#else
		iph_out->ip_len = htons(mout->m_pkthdr.len);
#endif
		/* out it goes */
		ip_output(mout, 0, &ro, IP_RAWOUTPUT, NULL
#if defined(__OpenBSD__) || (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
		    );
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	} else if (ip6_out != NULL) {
#ifdef NEW_STRUCT_ROUTE
		struct route ro;

#else
		struct route_in6 ro;

#endif

		bzero(&ro, sizeof(ro));
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
			printf("sctp_shutdown_complete2 calling ip6_output:\n");
			sctp_print_address_pkt((struct ip *)ip6_out,
			    &comp_cp->sh);
		}
#endif
		ip6_output(mout, NULL, &ro, 0, NULL, NULL
#if defined(__NetBSD__)
		    ,NULL
#endif
#if (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
		    ,0
#endif
		    );
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	}
	SCTP_STAT_INCR(sctps_sendpackets);
	return (0);
}

static struct sctp_nets *
sctp_select_hb_destination(struct sctp_tcb *stcb, struct timeval *now)
{
	struct sctp_nets *net, *hnet;
	int ms_goneby, highest_ms, state_overide = 0;

	SCTP_GETTIME_TIMEVAL(now);
	highest_ms = 0;
	hnet = NULL;
	SCTP_TCB_LOCK_ASSERT(stcb);
	TAILQ_FOREACH(net, &stcb->asoc.nets, sctp_next) {
		if (
		    ((net->dest_state & SCTP_ADDR_NOHB) && ((net->dest_state & SCTP_ADDR_UNCONFIRMED) == 0)) ||
		    (net->dest_state & SCTP_ADDR_OUT_OF_SCOPE)
		    ) {
			/*
			 * Skip this guy from consideration if HB is off AND
			 * its confirmed
			 */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("Skipping net:%p state:%d nohb/out-of-scope\n",
				    net, net->dest_state);
			}
#endif
			continue;
		}
		if (sctp_destination_is_reachable(stcb, (struct sockaddr *)&net->ro._l_addr) == 0) {
			/* skip this dest net from consideration */
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("Skipping net:%p reachable NOT\n",
				    net);
			}
#endif
			continue;
		}
		if (net->last_sent_time.tv_sec) {
			/* Sent to so we subtract */
			ms_goneby = (now->tv_sec - net->last_sent_time.tv_sec) * 1000;
		} else
			/* Never been sent to */
			ms_goneby = 0x7fffffff;
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
			printf("net:%p ms_goneby:%d\n",
			    net, ms_goneby);
		}
#endif
		/*
		 * When the address state is unconfirmed but still
		 * considered reachable, we HB at a higher rate. Once it
		 * goes confirmed OR reaches the "unreachable" state, thenw
		 * we cut it back to HB at a more normal pace.
		 */
		if ((net->dest_state & (SCTP_ADDR_UNCONFIRMED | SCTP_ADDR_NOT_REACHABLE)) == SCTP_ADDR_UNCONFIRMED) {
			state_overide = 1;
		} else {
			state_overide = 0;
		}

		if ((((unsigned int)ms_goneby >= net->RTO) || (state_overide)) &&
		    (ms_goneby > highest_ms)) {
			highest_ms = ms_goneby;
			hnet = net;
#ifdef SCTP_DEBUG
			if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
				printf("net:%p is the new high\n",
				    net);
			}
#endif
		}
	}
	if (hnet &&
	    ((hnet->dest_state & (SCTP_ADDR_UNCONFIRMED | SCTP_ADDR_NOT_REACHABLE)) == SCTP_ADDR_UNCONFIRMED)) {
		state_overide = 1;
	} else {
		state_overide = 0;
	}

	if (highest_ms && (((unsigned int)highest_ms >= hnet->RTO) || state_overide)) {
		/*
		 * Found the one with longest delay bounds OR it is
		 * unconfirmed and still not marked unreachable.
		 */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
			printf("net:%p is the hb winner -",
			    hnet);
			if (hnet)
				sctp_print_address((struct sockaddr *)&hnet->ro._l_addr);
			else
				printf(" none\n");
		}
#endif
		/* update the timer now */
		hnet->last_sent_time = *now;
		return (hnet);
	}
	/* Nothing to HB */
	return (NULL);
}

int
sctp_send_hb(struct sctp_tcb *stcb, int user_req, struct sctp_nets *u_net)
{
	struct sctp_tmit_chunk *chk;
	struct sctp_nets *net;
	struct sctp_heartbeat_chunk *hb;
	struct timeval now;
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;

	SCTP_TCB_LOCK_ASSERT(stcb);
	if (user_req == 0) {
		net = sctp_select_hb_destination(stcb, &now);
		if (net == NULL) {
			/*
			 * All our busy none to send to, just start the
			 * timer again.
			 */
			if (stcb->asoc.state == 0) {
				return (0);
			}
			sctp_timer_start(SCTP_TIMER_TYPE_HEARTBEAT,
			    stcb->sctp_ep,
			    stcb,
			    net);
			return (0);
		}
	} else {
		net = u_net;
		if (net == NULL) {
			return (0);
		}
		SCTP_GETTIME_TIMEVAL(&now);
	}
	sin = (struct sockaddr_in *)&net->ro._l_addr;
	if (sin->sin_family != AF_INET) {
		if (sin->sin_family != AF_INET6) {
			/* huh */
			return (0);
		}
	}
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
			printf("Gak, can't get a chunk for hb\n");
		}
#endif
		return (0);
	}
	SCTP_INCR_CHK_COUNT();
	chk->rec.chunk_id = SCTP_HEARTBEAT_REQUEST;
	chk->asoc = &stcb->asoc;
	chk->send_size = sizeof(struct sctp_heartbeat_chunk);

	chk->data = sctp_get_mbuf_for_msg(chk->send_size, 1, M_DONTWAIT, 1, MT_HEADER);
	if (chk->data == NULL) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return (0);
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;
	chk->data->m_pkthdr.len = chk->data->m_len = chk->send_size;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->whoTo = net;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	/* Now we have a mbuf that we can fill in with the details */
	hb = mtod(chk->data, struct sctp_heartbeat_chunk *);

	/* fill out chunk header */
	hb->ch.chunk_type = SCTP_HEARTBEAT_REQUEST;
	hb->ch.chunk_flags = 0;
	hb->ch.chunk_length = htons(chk->send_size);
	/* Fill out hb parameter */
	hb->heartbeat.hb_info.ph.param_type = htons(SCTP_HEARTBEAT_INFO);
	hb->heartbeat.hb_info.ph.param_length = htons(sizeof(struct sctp_heartbeat_info_param));
	hb->heartbeat.hb_info.time_value_1 = now.tv_sec;
	hb->heartbeat.hb_info.time_value_2 = now.tv_usec;
	/* Did our user request this one, put it in */
	hb->heartbeat.hb_info.user_req = user_req;
	hb->heartbeat.hb_info.addr_family = sin->sin_family;
	hb->heartbeat.hb_info.addr_len = sin->sin_len;
	if (net->dest_state & SCTP_ADDR_UNCONFIRMED) {
		/*
		 * we only take from the entropy pool if the address is not
		 * confirmed.
		 */
		net->heartbeat_random1 = hb->heartbeat.hb_info.random_value1 = sctp_select_initial_TSN(&stcb->sctp_ep->sctp_ep);
		net->heartbeat_random2 = hb->heartbeat.hb_info.random_value2 = sctp_select_initial_TSN(&stcb->sctp_ep->sctp_ep);
	} else {
		net->heartbeat_random1 = hb->heartbeat.hb_info.random_value1 = 0;
		net->heartbeat_random2 = hb->heartbeat.hb_info.random_value2 = 0;
	}
	if (sin->sin_family == AF_INET) {
		memcpy(hb->heartbeat.hb_info.address, &sin->sin_addr, sizeof(sin->sin_addr));
	} else if (sin->sin_family == AF_INET6) {
		/* We leave the scope the way it is in our lookup table. */
		sin6 = (struct sockaddr_in6 *)&net->ro._l_addr;
		memcpy(hb->heartbeat.hb_info.address, &sin6->sin6_addr, sizeof(sin6->sin6_addr));
	} else {
		/* huh compiler bug */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("Compiler bug bleeds a mbuf and a chunk\n");
		}
#endif
		return (0);
	}
	/* ok we have a destination that needs a beat */
	/* lets do the theshold management Qiaobing style */
	if (user_req == 0) {
		if (sctp_threshold_management(stcb->sctp_ep, stcb, net,
		    stcb->asoc.max_send_times)) {
			/*
			 * we have lost the association, in a way this is
			 * quite bad since we really are one less time since
			 * we really did not send yet. This is the down side
			 * to the Q's style as defined in the RFC and not my
			 * alternate style defined in the RFC.
			 */
			if (chk->data != NULL) {
				sctp_m_freem(chk->data);
				chk->data = NULL;
			}
			SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
			SCTP_DECR_CHK_COUNT();
			return (-1);
		}
	}
	net->hb_responded = 0;
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
		printf("Inserting chunk for HB\n");
	}
#endif
	TAILQ_INSERT_TAIL(&stcb->asoc.control_send_queue, chk, sctp_next);
	stcb->asoc.ctrl_queue_cnt++;
	SCTP_STAT_INCR(sctps_sendheartbeat);
	/*
	 * Call directly med level routine to put out the chunk. It will
	 * always tumble out control chunks aka HB but it may even tumble
	 * out data too.
	 */
	return (1);
}

void
sctp_send_ecn_echo(struct sctp_tcb *stcb, struct sctp_nets *net,
    uint32_t high_tsn)
{
	struct sctp_association *asoc;
	struct sctp_ecne_chunk *ecne;
	struct sctp_tmit_chunk *chk;

	asoc = &stcb->asoc;
	SCTP_TCB_LOCK_ASSERT(stcb);
	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if (chk->rec.chunk_id == SCTP_ECN_ECHO) {
			/* found a previous ECN_ECHO update it if needed */
			ecne = mtod(chk->data, struct sctp_ecne_chunk *);
			ecne->tsn = htonl(high_tsn);
			return;
		}
	}
	/* nope could not find one to update so we must build one */
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		return;
	}
	SCTP_STAT_INCR(sctps_sendecne);
	SCTP_INCR_CHK_COUNT();
	chk->rec.chunk_id = SCTP_ECN_ECHO;
	chk->asoc = &stcb->asoc;
	chk->send_size = sizeof(struct sctp_ecne_chunk);
	chk->data = sctp_get_mbuf_for_msg(chk->send_size, 1, M_DONTWAIT, 1, MT_HEADER);
	if (chk->data == NULL) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return;
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;
	chk->data->m_pkthdr.len = chk->data->m_len = chk->send_size;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->whoTo = net;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	ecne = mtod(chk->data, struct sctp_ecne_chunk *);
	ecne->ch.chunk_type = SCTP_ECN_ECHO;
	ecne->ch.chunk_flags = 0;
	ecne->ch.chunk_length = htons(sizeof(struct sctp_ecne_chunk));
	ecne->tsn = htonl(high_tsn);
	TAILQ_INSERT_TAIL(&stcb->asoc.control_send_queue, chk, sctp_next);
	asoc->ctrl_queue_cnt++;
}

void
sctp_send_packet_dropped(struct sctp_tcb *stcb, struct sctp_nets *net,
    struct mbuf *m, int iphlen, int bad_crc)
{
	struct sctp_association *asoc;
	struct sctp_pktdrop_chunk *drp;
	struct sctp_tmit_chunk *chk;
	uint8_t *datap;
	int len;
	unsigned int small_one;
	struct ip *iph;

	long spc;

	asoc = &stcb->asoc;
	SCTP_TCB_LOCK_ASSERT(stcb);
	if (asoc->peer_supports_pktdrop == 0) {
		/*
		 * peer must declare support before I send one.
		 */
		return;
	}
	if(stcb->sctp_socket == NULL) {
		return;
	}
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		return;
	}
	SCTP_INCR_CHK_COUNT();
	iph = mtod(m, struct ip *);
	if (iph == NULL) {
		return;
	}
	if (iph->ip_v == IPVERSION) {
		/* IPv4 */
#if defined(__FreeBSD__) || defined(__APPLE__)
		len = chk->send_size = iph->ip_len;
#else
		len = chk->send_size = (iph->ip_len - iphlen);
#endif
	} else {
		struct ip6_hdr *ip6h;

		/* IPv6 */
		ip6h = mtod(m, struct ip6_hdr *);
		len = chk->send_size = htons(ip6h->ip6_plen);
	}
	if ((len + iphlen) > m->m_pkthdr.len) {
		/* huh */
		chk->send_size = len = m->m_pkthdr.len - iphlen;
	}
	chk->asoc = &stcb->asoc;
	chk->data = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if (chk->data == NULL) {
jump_out:
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return;
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;
	drp = mtod(chk->data, struct sctp_pktdrop_chunk *);
	if (drp == NULL) {
		sctp_m_freem(chk->data);
		chk->data = NULL;
		goto jump_out;
	}
	small_one = asoc->smallest_mtu;
	if (small_one > MCLBYTES) {
		/* Only one cluster worth of data MAX */
		small_one = MCLBYTES;
	}
	chk->book_size = SCTP_SIZE32((chk->send_size + sizeof(struct sctp_pktdrop_chunk) +
	    sizeof(struct sctphdr) + SCTP_MED_OVERHEAD));
	if (chk->book_size > small_one) {
		drp->ch.chunk_flags = SCTP_PACKET_TRUNCATED;
		drp->trunc_len = htons(chk->send_size);
		chk->send_size = small_one - (SCTP_MED_OVERHEAD +
		    sizeof(struct sctp_pktdrop_chunk) +
		    sizeof(struct sctphdr));
		len = chk->send_size;
	} else {
		/* no truncation needed */
		drp->ch.chunk_flags = 0;
		drp->trunc_len = htons(0);
	}
	if (bad_crc) {
		drp->ch.chunk_flags |= SCTP_BADCRC;
	}
	chk->send_size += sizeof(struct sctp_pktdrop_chunk);
	chk->data->m_pkthdr.len = chk->data->m_len = chk->send_size;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	if (net) {
		/* we should hit here */
		chk->whoTo = net;
	} else {
		chk->whoTo = asoc->primary_destination;
	}
	atomic_add_int(&chk->whoTo->ref_count, 1);
	chk->rec.chunk_id = SCTP_PACKET_DROPPED;
	drp->ch.chunk_type = SCTP_PACKET_DROPPED;
	drp->ch.chunk_length = htons(chk->send_size);
	spc = stcb->sctp_socket->so_rcv.sb_hiwat;
	if (spc < 0) {
		spc = 0;
	}
	drp->bottle_bw = htonl(spc);
	if (asoc->my_rwnd) {
		drp->current_onq = htonl(asoc->size_on_reasm_queue +
		    asoc->size_on_all_streams +
		    asoc->my_rwnd_control_len +
		    stcb->sctp_socket->so_rcv.sb_cc);
	} else {
		/*
		 * If my rwnd is 0, possibly from mbuf depletion as well as
		 * space used, tell the peer there is NO space aka onq == bw
		 */
		drp->current_onq = htonl(spc);
	}
	drp->reserved = 0;
	datap = drp->data;
	m_copydata(m, iphlen, len, (caddr_t)datap);
	TAILQ_INSERT_TAIL(&stcb->asoc.control_send_queue, chk, sctp_next);
	asoc->ctrl_queue_cnt++;
}

void
sctp_send_cwr(struct sctp_tcb *stcb, struct sctp_nets *net, uint32_t high_tsn)
{
	struct sctp_association *asoc;
	struct sctp_cwr_chunk *cwr;
	struct sctp_tmit_chunk *chk;

	asoc = &stcb->asoc;
	SCTP_TCB_LOCK_ASSERT(stcb);
	TAILQ_FOREACH(chk, &asoc->control_send_queue, sctp_next) {
		if (chk->rec.chunk_id == SCTP_ECN_CWR) {
			/* found a previous ECN_CWR update it if needed */
			cwr = mtod(chk->data, struct sctp_cwr_chunk *);
			if (compare_with_wrap(high_tsn, ntohl(cwr->tsn),
			    MAX_TSN)) {
				cwr->tsn = htonl(high_tsn);
			}
			return;
		}
	}
	/* nope could not find one to update so we must build one */
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		return;
	}
	SCTP_INCR_CHK_COUNT();
	chk->rec.chunk_id = SCTP_ECN_CWR;
	chk->asoc = &stcb->asoc;
	chk->send_size = sizeof(struct sctp_cwr_chunk);
	chk->data = sctp_get_mbuf_for_msg(chk->send_size, 1, M_DONTWAIT, 1, MT_HEADER);
	if (chk->data == NULL) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return;
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;
	chk->data->m_pkthdr.len = chk->data->m_len = chk->send_size;
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->whoTo = net;
	atomic_add_int(&chk->whoTo->ref_count, 1);
	cwr = mtod(chk->data, struct sctp_cwr_chunk *);
	cwr->ch.chunk_type = SCTP_ECN_CWR;
	cwr->ch.chunk_flags = 0;
	cwr->ch.chunk_length = htons(sizeof(struct sctp_cwr_chunk));
	cwr->tsn = htonl(high_tsn);
	TAILQ_INSERT_TAIL(&stcb->asoc.control_send_queue, chk, sctp_next);
	asoc->ctrl_queue_cnt++;
}

void
sctp_add_stream_reset_out(struct sctp_tmit_chunk *chk,
    int number_entries, uint16_t * list,
    uint32_t seq, uint32_t resp_seq, uint32_t last_sent)
{
	int len, old_len, i;
	struct sctp_stream_reset_out_request *req_out;
	struct sctp_chunkhdr *ch;

	ch = mtod(chk->data, struct sctp_chunkhdr *);


	old_len = len = SCTP_SIZE32(ntohs(ch->chunk_length));

	/* get to new offset for the param. */
	req_out = (struct sctp_stream_reset_out_request *)((caddr_t)ch + len);
	/* now how long will this param be? */
	len = (sizeof(struct sctp_stream_reset_out_request) + (sizeof(uint16_t) * number_entries));
	req_out->ph.param_type = htons(SCTP_STR_RESET_OUT_REQUEST);
	req_out->ph.param_length = htons(len);
	req_out->request_seq = htonl(seq);
	req_out->response_seq = htonl(resp_seq);
	req_out->send_reset_at_tsn = htonl(last_sent);
	if (number_entries) {
		for (i = 0; i < number_entries; i++) {
			req_out->list_of_streams[i] = htons(list[i]);
		}
	}
	if (SCTP_SIZE32(len) > len) {
		/*
		 * Need to worry about the pad we may end up adding to the
		 * end. This is easy since the struct is either aligned to 4
		 * bytes or 2 bytes off.
		 */
		req_out->list_of_streams[number_entries] = 0;
	}
	/* now fix the chunk length */
	ch->chunk_length = htons(len + old_len);
	chk->send_size = len + old_len;
	chk->book_size = SCTP_SIZE32(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);
	return;
}


void
sctp_add_stream_reset_in(struct sctp_tmit_chunk *chk,
    int number_entries, uint16_t * list,
    uint32_t seq)
{
	int len, old_len, i;
	struct sctp_stream_reset_in_request *req_in;
	struct sctp_chunkhdr *ch;

	ch = mtod(chk->data, struct sctp_chunkhdr *);


	old_len = len = SCTP_SIZE32(ntohs(ch->chunk_length));

	/* get to new offset for the param. */
	req_in = (struct sctp_stream_reset_in_request *)((caddr_t)ch + len);
	/* now how long will this param be? */
	len = (sizeof(struct sctp_stream_reset_in_request) + (sizeof(uint16_t) * number_entries));
	req_in->ph.param_type = htons(SCTP_STR_RESET_IN_REQUEST);
	req_in->ph.param_length = htons(len);
	req_in->request_seq = htonl(seq);
	if (number_entries) {
		for (i = 0; i < number_entries; i++) {
			req_in->list_of_streams[i] = htons(list[i]);
		}
	}
	if (SCTP_SIZE32(len) > len) {
		/*
		 * Need to worry about the pad we may end up adding to the
		 * end. This is easy since the struct is either aligned to 4
		 * bytes or 2 bytes off.
		 */
		req_in->list_of_streams[number_entries] = 0;
	}
	/* now fix the chunk length */
	ch->chunk_length = htons(len + old_len);
	chk->send_size = len + old_len;
	chk->book_size = SCTP_SIZE32(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);
	return;
}


void
sctp_add_stream_reset_tsn(struct sctp_tmit_chunk *chk,
    uint32_t seq)
{
	int len, old_len;
	struct sctp_stream_reset_tsn_request *req_tsn;
	struct sctp_chunkhdr *ch;

	ch = mtod(chk->data, struct sctp_chunkhdr *);


	old_len = len = SCTP_SIZE32(ntohs(ch->chunk_length));

	/* get to new offset for the param. */
	req_tsn = (struct sctp_stream_reset_tsn_request *)((caddr_t)ch + len);
	/* now how long will this param be? */
	len = sizeof(struct sctp_stream_reset_tsn_request);
	req_tsn->ph.param_type = htons(SCTP_STR_RESET_TSN_REQUEST);
	req_tsn->ph.param_length = htons(len);
	req_tsn->request_seq = htonl(seq);

	/* now fix the chunk length */
	ch->chunk_length = htons(len + old_len);
	chk->send_size = len + old_len;
	chk->book_size = SCTP_SIZE32(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);
	return;
}

void
sctp_add_stream_reset_result(struct sctp_tmit_chunk *chk,
    uint32_t resp_seq, uint32_t result)
{
	int len, old_len;
	struct sctp_stream_reset_response *resp;
	struct sctp_chunkhdr *ch;

	ch = mtod(chk->data, struct sctp_chunkhdr *);


	old_len = len = SCTP_SIZE32(ntohs(ch->chunk_length));

	/* get to new offset for the param. */
	resp = (struct sctp_stream_reset_response *)((caddr_t)ch + len);
	/* now how long will this param be? */
	len = sizeof(struct sctp_stream_reset_response);
	resp->ph.param_type = htons(SCTP_STR_RESET_RESPONSE);
	resp->ph.param_length = htons(len);
	resp->response_seq = htonl(resp_seq);
	resp->result = ntohl(result);

	/* now fix the chunk length */
	ch->chunk_length = htons(len + old_len);
	chk->send_size = len + old_len;
	chk->book_size = SCTP_SIZE32(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);
	return;

}


void
sctp_add_stream_reset_result_tsn(struct sctp_tmit_chunk *chk,
    uint32_t resp_seq, uint32_t result,
    uint32_t send_una, uint32_t recv_next)
{
	int len, old_len;
	struct sctp_stream_reset_response_tsn *resp;
	struct sctp_chunkhdr *ch;

	ch = mtod(chk->data, struct sctp_chunkhdr *);


	old_len = len = SCTP_SIZE32(ntohs(ch->chunk_length));

	/* get to new offset for the param. */
	resp = (struct sctp_stream_reset_response_tsn *)((caddr_t)ch + len);
	/* now how long will this param be? */
	len = sizeof(struct sctp_stream_reset_response_tsn);
	resp->ph.param_type = htons(SCTP_STR_RESET_RESPONSE);
	resp->ph.param_length = htons(len);
	resp->response_seq = htonl(resp_seq);
	resp->result = htonl(result);
	resp->senders_next_tsn = htonl(send_una);
	resp->receivers_next_tsn = htonl(recv_next);

	/* now fix the chunk length */
	ch->chunk_length = htons(len + old_len);
	chk->send_size = len + old_len;
	chk->book_size = SCTP_SIZE32(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);
	return;
}


int
sctp_send_str_reset_req(struct sctp_tcb *stcb,
    int number_entries, uint16_t * list,
    uint8_t send_out_req, uint32_t resp_seq,
    uint8_t send_in_req,
    uint8_t send_tsn_req)
{

	struct sctp_association *asoc;
	struct sctp_tmit_chunk *chk;
	struct sctp_chunkhdr *ch;
	uint32_t seq;

	asoc = &stcb->asoc;
	if (asoc->stream_reset_outstanding) {
		/*
		 * Already one pending, must get ACK back to clear the flag.
		 */
		return (EBUSY);
	}
	if ((send_out_req == 0) && (send_in_req == 0) && (send_tsn_req == 0)) {
		/* nothing to do */
		return (EINVAL);
	}
	if (send_tsn_req && (send_out_req || send_in_req)) {
		/* error, can't do that */
		return (EINVAL);
	}
	chk = (struct sctp_tmit_chunk *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_chunk);
	if (chk == NULL) {
		return (ENOMEM);
	}
	SCTP_INCR_CHK_COUNT();
	chk->rec.chunk_id = SCTP_STREAM_RESET;
	chk->asoc = &stcb->asoc;
	chk->book_size = SCTP_SIZE32(chk->send_size = sizeof(struct sctp_chunkhdr));

	chk->data = sctp_get_mbuf_for_msg(MCLBYTES, 1, M_DONTWAIT, 1, MT_DATA);
	if (chk->data == NULL) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_chunk, chk);
		SCTP_DECR_CHK_COUNT();
		return (ENOMEM);
	}
	chk->data->m_data += SCTP_MIN_OVERHEAD;

	/* setup chunk parameters */
	chk->sent = SCTP_DATAGRAM_UNSENT;
	chk->snd_count = 0;
	chk->whoTo = asoc->primary_destination;
	atomic_add_int(&chk->whoTo->ref_count, 1);

	ch = mtod(chk->data, struct sctp_chunkhdr *);
	ch->chunk_type = SCTP_STREAM_RESET;
	ch->chunk_flags = 0;
	ch->chunk_length = htons(chk->send_size);
	chk->data->m_pkthdr.len = chk->data->m_len = SCTP_SIZE32(chk->send_size);

	seq = stcb->asoc.str_reset_seq_out;
	if (send_out_req) {
		sctp_add_stream_reset_out(chk, number_entries, list,
		    seq, resp_seq, (stcb->asoc.sending_seq - 1));
		asoc->stream_reset_out_is_outstanding = 1;
		seq++;
		asoc->stream_reset_outstanding++;
	}
	if (send_in_req) {
		sctp_add_stream_reset_in(chk, number_entries, list, seq);
		asoc->stream_reset_outstanding++;
	}
	if (send_tsn_req) {
		sctp_add_stream_reset_tsn(chk, seq);
		asoc->stream_reset_outstanding++;
	}
	asoc->str_reset = chk;

	/* insert the chunk for sending */
	TAILQ_INSERT_TAIL(&asoc->control_send_queue,
	    chk,
	    sctp_next);
	asoc->ctrl_queue_cnt++;
	sctp_timer_start(SCTP_TIMER_TYPE_STRRESET, stcb->sctp_ep, stcb, chk->whoTo);
	return (0);
}

void
sctp_send_abort(struct mbuf *m, int iphlen, struct sctphdr *sh, uint32_t vtag,
    struct mbuf *err_cause)
{
	/*
	 * Formulate the abort message, and send it back down.
	 */
	struct mbuf *mout;
	struct sctp_abort_msg *abm;
	struct ip *iph, *iph_out;
	struct ip6_hdr *ip6, *ip6_out;
	int iphlen_out;

	/* don't respond to ABORT with ABORT */
	if (sctp_is_there_an_abort_here(m, iphlen, &vtag)) {
		if (err_cause)
			sctp_m_freem(err_cause);
		return;
	}
	mout = sctp_get_mbuf_for_msg((sizeof(struct ip6_hdr) + sizeof(struct sctp_abort_msg)), 
				     1, M_DONTWAIT, 1, MT_HEADER);
	if (mout == NULL) {
		if (err_cause)
			sctp_m_freem(err_cause);
		return;
	}
	iph = mtod(m, struct ip *);
	iph_out = NULL;
	ip6_out = NULL;
	if (iph->ip_v == IPVERSION) {
		iph_out = mtod(mout, struct ip *);
		mout->m_len = sizeof(*iph_out) + sizeof(*abm);
		mout->m_next = err_cause;

		/* Fill in the IP header for the ABORT */
		iph_out->ip_v = IPVERSION;
		iph_out->ip_hl = (sizeof(struct ip) / 4);
		iph_out->ip_tos = (u_char)0;
		iph_out->ip_id = 0;
		iph_out->ip_off = 0;
		iph_out->ip_ttl = MAXTTL;
		iph_out->ip_p = IPPROTO_SCTP;
		iph_out->ip_src.s_addr = iph->ip_dst.s_addr;
		iph_out->ip_dst.s_addr = iph->ip_src.s_addr;
		/* let IP layer calculate this */
		iph_out->ip_sum = 0;

		iphlen_out = sizeof(*iph_out);
		abm = (struct sctp_abort_msg *)((caddr_t)iph_out + iphlen_out);
	} else if (iph->ip_v == (IPV6_VERSION >> 4)) {
		ip6 = (struct ip6_hdr *)iph;
		ip6_out = mtod(mout, struct ip6_hdr *);
		mout->m_len = sizeof(*ip6_out) + sizeof(*abm);
		mout->m_next = err_cause;

		/* Fill in the IP6 header for the ABORT */
		ip6_out->ip6_flow = ip6->ip6_flow;
		ip6_out->ip6_hlim = ip6_defhlim;
		ip6_out->ip6_nxt = IPPROTO_SCTP;
		ip6_out->ip6_src = ip6->ip6_dst;
		ip6_out->ip6_dst = ip6->ip6_src;

		iphlen_out = sizeof(*ip6_out);
		abm = (struct sctp_abort_msg *)((caddr_t)ip6_out + iphlen_out);
	} else {
		/* Currently not supported */
		return;
	}

	abm->sh.src_port = sh->dest_port;
	abm->sh.dest_port = sh->src_port;
	abm->sh.checksum = 0;
	if (vtag == 0) {
		abm->sh.v_tag = sh->v_tag;
		abm->msg.ch.chunk_flags = SCTP_HAD_NO_TCB;
	} else {
		abm->sh.v_tag = htonl(vtag);
		abm->msg.ch.chunk_flags = 0;
	}
	abm->msg.ch.chunk_type = SCTP_ABORT_ASSOCIATION;

	if (err_cause) {
		struct mbuf *m_tmp = err_cause;
		int err_len = 0;

		/* get length of the err_cause chain */
		while (m_tmp != NULL) {
			err_len += m_tmp->m_len;
			m_tmp = m_tmp->m_next;
		}
		mout->m_pkthdr.len = mout->m_len + err_len;
		if (err_len % 4) {
			/* need pad at end of chunk */
			uint32_t cpthis = 0;
			int padlen;

			padlen = 4 - (mout->m_pkthdr.len % 4);
			m_copyback(mout, mout->m_pkthdr.len, padlen, (caddr_t)&cpthis);
		}
		abm->msg.ch.chunk_length = htons(sizeof(abm->msg.ch) + err_len);
	} else {
		mout->m_pkthdr.len = mout->m_len;
		abm->msg.ch.chunk_length = htons(sizeof(abm->msg.ch));
	}

	/* add checksum */
	if ((sctp_no_csum_on_loopback) &&
	    (m->m_pkthdr.rcvif) &&
	    (m->m_pkthdr.rcvif->if_type == IFT_LOOP)) {
		abm->sh.checksum = 0;
	} else {
		abm->sh.checksum = sctp_calculate_sum(mout, NULL, iphlen_out);
	}

	/* zap the rcvif, it should be null */
	mout->m_pkthdr.rcvif = 0;
	if (iph_out != NULL) {
		struct route ro;

		/* zap the stack pointer to the route */
		bzero(&ro, sizeof ro);
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
			printf("sctp_send_abort calling ip_output:\n");
			sctp_print_address_pkt(iph_out, &abm->sh);
		}
#endif
		/* set IPv4 length */
#if defined(__FreeBSD__) || defined (__APPLE__)
		iph_out->ip_len = mout->m_pkthdr.len;
#else
		iph_out->ip_len = htons(mout->m_pkthdr.len);
#endif
		/* out it goes */
		(void)ip_output(mout, 0, &ro, IP_RAWOUTPUT, NULL
#if defined(__OpenBSD__) || (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
		    );
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	} else if (ip6_out != NULL) {
#ifdef NEW_STRUCT_ROUTE
		struct route ro;

#else
		struct route_in6 ro;

#endif

		/* zap the stack pointer to the route */
		bzero(&ro, sizeof(ro));
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
			printf("sctp_send_abort calling ip6_output:\n");
			sctp_print_address_pkt((struct ip *)ip6_out, &abm->sh);
		}
#endif
		ip6_output(mout, NULL, &ro, 0, NULL, NULL
#if defined(__NetBSD__)
		    ,NULL
#endif
#if (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
		    ,0
#endif
		    );
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	}
	SCTP_STAT_INCR(sctps_sendpackets);
}

void
sctp_send_operr_to(struct mbuf *m, int iphlen,
    struct mbuf *scm,
    uint32_t vtag)
{
	struct sctphdr *ihdr;
	int retcode;
	struct sctphdr *ohdr;
	struct sctp_chunkhdr *ophdr;

	struct ip *iph;

#ifdef SCTP_DEBUG
	struct sockaddr_in6 lsa6, fsa6;

#endif
	uint32_t val;

	iph = mtod(m, struct ip *);
	ihdr = (struct sctphdr *)((caddr_t)iph + iphlen);
	if (!(scm->m_flags & M_PKTHDR)) {
		/* must be a pkthdr */
		printf("Huh, not a packet header in send_operr\n");
		sctp_m_freem(scm);
		return;
	}
	M_PREPEND(scm, (sizeof(struct sctphdr) + sizeof(struct sctp_chunkhdr)), M_DONTWAIT);
	if (scm == NULL) {
		/* can't send because we can't add a mbuf */
		return;
	}
	ohdr = mtod(scm, struct sctphdr *);
	ohdr->src_port = ihdr->dest_port;
	ohdr->dest_port = ihdr->src_port;
	ohdr->v_tag = vtag;
	ohdr->checksum = 0;
	ophdr = (struct sctp_chunkhdr *)(ohdr + 1);
	ophdr->chunk_type = SCTP_OPERATION_ERROR;
	ophdr->chunk_flags = 0;
	ophdr->chunk_length = htons(scm->m_pkthdr.len - sizeof(struct sctphdr));
	if (scm->m_pkthdr.len % 4) {
		/* need padding */
		uint32_t cpthis = 0;
		int padlen;

		padlen = 4 - (scm->m_pkthdr.len % 4);
		m_copyback(scm, scm->m_pkthdr.len, padlen, (caddr_t)&cpthis);
	}
	if ((sctp_no_csum_on_loopback) &&
	    (m->m_pkthdr.rcvif) &&
	    (m->m_pkthdr.rcvif->if_type == IFT_LOOP)) {
		val = 0;
	} else {
		val = sctp_calculate_sum(scm, NULL, 0);
	}
	ohdr->checksum = val;
	if (iph->ip_v == IPVERSION) {
		/* V4 */
		struct ip *out;
		struct route ro;

		M_PREPEND(scm, sizeof(struct ip), M_DONTWAIT);
		if (scm == NULL)
			return;
		bzero(&ro, sizeof ro);
		out = mtod(scm, struct ip *);
		out->ip_v = iph->ip_v;
		out->ip_hl = (sizeof(struct ip) / 4);
		out->ip_tos = iph->ip_tos;
		out->ip_id = iph->ip_id;
		out->ip_off = 0;
		out->ip_ttl = MAXTTL;
		out->ip_p = IPPROTO_SCTP;
		out->ip_sum = 0;
		out->ip_src = iph->ip_dst;
		out->ip_dst = iph->ip_src;
#if defined(__FreeBSD__) || defined(__APPLE__)
		out->ip_len = scm->m_pkthdr.len;
#else
		out->ip_len = htons(scm->m_pkthdr.len);
#endif
		retcode = ip_output(scm, 0, &ro, IP_RAWOUTPUT, NULL
#if defined(__OpenBSD__) || (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
		    );
		SCTP_STAT_INCR(sctps_sendpackets);
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	} else {
		/* V6 */
#ifdef NEW_STRUCT_ROUTE
		struct route ro;

#else
		struct route_in6 ro;

#endif
		struct ip6_hdr *out6, *in6;

		M_PREPEND(scm, sizeof(struct ip6_hdr), M_DONTWAIT);
		if (scm == NULL)
			return;
		bzero(&ro, sizeof ro);
		in6 = mtod(m, struct ip6_hdr *);
		out6 = mtod(scm, struct ip6_hdr *);
		out6->ip6_flow = in6->ip6_flow;
		out6->ip6_hlim = ip6_defhlim;
		out6->ip6_nxt = IPPROTO_SCTP;
		out6->ip6_src = in6->ip6_dst;
		out6->ip6_dst = in6->ip6_src;

#ifdef SCTP_DEBUG
		bzero(&lsa6, sizeof(lsa6));
		lsa6.sin6_len = sizeof(lsa6);
		lsa6.sin6_family = AF_INET6;
		lsa6.sin6_addr = out6->ip6_src;
		bzero(&fsa6, sizeof(fsa6));
		fsa6.sin6_len = sizeof(fsa6);
		fsa6.sin6_family = AF_INET6;
		fsa6.sin6_addr = out6->ip6_dst;
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT2) {
			printf("sctp_operr_to calling ipv6 output:\n");
			printf("src: ");
			sctp_print_address((struct sockaddr *)&lsa6);
			printf("dst ");
			sctp_print_address((struct sockaddr *)&fsa6);
		}
#endif				/* SCTP_DEBUG */
		ip6_output(scm, NULL, &ro, 0, NULL, NULL
#if defined(__NetBSD__)
		    ,NULL
#endif
#if (defined(__FreeBSD__) && __FreeBSD_version >= 480000)
		    ,NULL
#endif
#if defined(__APPLE__) && !defined(SCTP_APPLE_PANTHER)
		    ,0
#endif
		    );
		SCTP_STAT_INCR(sctps_sendpackets);
		/* Free the route if we got one back */
		if (ro.ro_rt)
			RTFREE(ro.ro_rt);
	}
}



static struct mbuf *
sctp_copy_resume(struct sctp_stream_queue_pending *sp, 
		 struct uio *uio,
		 struct sctp_sndrcvinfo *srcv,
		 int max_send_len, 
		 int user_marks_eor,
		 int *error,
		 uint32_t *sndout,
		 struct mbuf **new_tail)
{
	int left, cancpy, willcpy, need_hdr=0;
	struct mbuf *m,*prev, *head;

        left = min(uio->uio_resid, max_send_len);
	/* Always get a header just in case */
	need_hdr = 1;

	head = sctp_get_mbuf_for_msg(left, need_hdr, M_WAIT, 0, MT_DATA);
	cancpy = M_TRAILINGSPACE(head);
	willcpy = min(cancpy, left);
	*error = uiomove(mtod(head, caddr_t), willcpy, uio);
	if (*error) {
		sctp_m_freem(head);
		return(NULL);
	}
	*sndout += willcpy;
	left -= willcpy;
	head->m_len = willcpy;
	m = head;
	*new_tail = head;
	while (left > 0) {
		/* move in user data */
		m->m_next = sctp_get_mbuf_for_msg(left, 0, M_WAIT, 0, MT_DATA);
		if (m->m_next == NULL) {
			sctp_m_freem(head);
			*error = ENOMEM;
			return(NULL);
		}
		prev = m;
		m = m->m_next;
		cancpy = M_TRAILINGSPACE(m);
		willcpy = min(cancpy, left);
		*error = uiomove(mtod(m, caddr_t), willcpy, uio);
		if (*error) {
			sctp_m_freem(head);
			return (NULL);
		}
		m->m_len = willcpy;
		left -= willcpy;
		*sndout += willcpy;
		if (left == 0) {
			*new_tail = m;
			m->m_next = NULL;
		}
	}
	return(head);
}

static int
sctp_copy_one(struct sctp_stream_queue_pending *ret, 
	      struct uio *uio, 
	      int resv_upfront)
{
	int left, cancpy, willcpy, error;
	struct mbuf *m, *head;
	int cpsz=0;

	/* First one gets a header */
	left = ret->length;
	head = m = sctp_get_mbuf_for_msg((left + resv_upfront), 1, M_WAIT, 0, MT_DATA);
	if (m == NULL) {
		return (ENOMEM);
	}
	/*
	 * Add this one for m in now, that way if the alloc fails we won't
	 * have a bad cnt.
	 */
	m->m_data += resv_upfront;
	cancpy = M_TRAILINGSPACE(m);
	willcpy = min(cancpy, left);

	while (left > 0) {
		/* move in user data */
		error = uiomove(mtod(m, caddr_t), willcpy, uio);
		if (error) {
			sctp_m_freem(head);
			return (error);
		}
		m->m_len = willcpy;
		left -= willcpy;
		cpsz += willcpy;
		if (left > 0) {
			m->m_next = sctp_get_mbuf_for_msg(left, 0, M_WAIT, 0, MT_DATA);
			if (m->m_next == NULL) {
				/*
				 * the head goes back to caller, he can free
				 * the rest
				 */
				sctp_m_freem(head);
				return (ENOMEM);
			}
			m = m->m_next;
			cancpy = M_TRAILINGSPACE(m);
			willcpy = min(cancpy, left);
		} else {
			ret->tail_mbuf = m;
			m->m_next = NULL;
		}
	}
	ret->data = head;
	ret->length = cpsz;
	return (0);
}



static struct sctp_stream_queue_pending *
sctp_copy_it_in(struct sctp_tcb *stcb,
    struct sctp_association *asoc,
    struct sctp_sndrcvinfo *srcv,
    struct uio *uio,
    struct sctp_nets *net,
    int max_send_len,
    int user_marks_eor,
    int *errno,
    int non_blocking)

{
	/*
	 * This routine must be very careful in its work. Protocol
	 * processing is up and running so care must be taken to spl...()
	 * when you need to do something that may effect the stcb/asoc. The
	 * sb is locked however. When data is copied the protocol processing
	 * should be enabled since this is a slower operation...
	 */
	struct sctp_stream_queue_pending *sp=NULL;
	int resv_in_first;
#if defined(__NetBSD__) || defined(__OpenBSD__)
	int s;
	s = splsoftnet();
#endif
	*errno = 0;
        /* Unless E_EOR mode is on, we must make
	 * a send FIT in one call.
	 */
	if (((user_marks_eor == 0) && non_blocking) && 
	    (uio->uio_resid > stcb->sctp_socket->so_snd.sb_hiwat)) {
		/* It will NEVER fit */
		*errno = EMSGSIZE;
		goto out_now;
	}

	/* Now can we send this? */
	if ((SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_SENT) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_ACK_SENT) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_RECEIVED) ||
	    (asoc->state & SCTP_STATE_SHUTDOWN_PENDING)) {
		/* got data while shutting down */
		*errno = ECONNRESET;
		goto out_now;
	}
	sp = (struct sctp_stream_queue_pending *)SCTP_ZONE_GET(sctppcbinfo.ipi_zone_strmoq);
	if(sp == NULL) {
		*errno = ENOMEM;
		goto out_now;
	}
	SCTP_INCR_STRMOQ_COUNT();
	sp->act_flags = 0;
	sp->sinfo_flags = srcv->sinfo_flags;
	sp->timetolive = srcv->sinfo_timetolive;
	sp->ppid = srcv->sinfo_ppid;
	sp->context = srcv->sinfo_context;
	sp->strseq = 0;
	SCTP_GETTIME_TIMEVAL(&sp->ts);

	sp->stream = srcv->sinfo_stream;

	sp->length = min(uio->uio_resid, max_send_len);
	if ((sp->length == uio->uio_resid) &&
	    ((user_marks_eor == 0) || 
	     (srcv->sinfo_flags & SCTP_EOF) ||
	     (user_marks_eor && (srcv->sinfo_flags & SCTP_EOR)))
	   ){
		sp->msg_is_complete = 1;
	} else {
		sp->msg_is_complete = 0;
	}
	sp->some_taken = 0;
	resv_in_first = sizeof(struct sctp_data_chunk);
	sp->data = sp->tail_mbuf = NULL;
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	socket_unlock(stcb->sctp_socket, 0);
#endif
	*errno = sctp_copy_one(sp, uio, resv_in_first);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	socket_lock(stcb->sctp_socket, 0);
#endif
	if(*errno) {
		SCTP_ZONE_FREE(sctppcbinfo.ipi_zone_strmoq, sp);
		SCTP_DECR_STRMOQ_COUNT();
		sp = NULL;
	} else {
		if(sp->sinfo_flags & SCTP_ADDR_OVER) {
			sp->net = net;
			sp->addr_over = 1;
		} else {
			sp->net = asoc->primary_destination;
			sp->addr_over = 0;
		}
		atomic_add_int(&sp->net->ref_count, 1);
		sp->data->m_pkthdr.len = sp->length;
		sctp_set_prsctp_policy(stcb, sp);
	}
out_now:
#if defined(__NetBSD__) || defined(__OpenBSD__)
	splx(s);
#endif
	return (sp);
}


int
sctp_sosend(struct socket *so,
#ifdef __NetBSD__
    struct mbuf *addr_mbuf,
#else
    struct sockaddr *addr,
#endif
    struct uio *uio,
    struct mbuf *top,
    struct mbuf *control,
    int flags
#ifdef __FreeBSD__
    ,
#if defined(__FreeBSD__) && __FreeBSD_version >= 500000
    struct thread *p
#else
    struct proc *p
#endif
#endif
)
{
#if defined(__APPLE__)
	struct proc *p = current_proc();

#elif defined(__NetBSD__)
	struct proc *p = curproc;	/* XXX */
	struct sockaddr *addr = NULL;

	if (addr_mbuf)
		addr = mtod(addr_mbuf, struct sockaddr *);
#endif
	struct sctp_inpcb *inp;
	int s, error, use_rcvinfo = 0;
	struct sctp_sndrcvinfo srcv;

	inp = (struct sctp_inpcb *)so->so_pcb;
#if defined(__NetBSD__) || defined(__OpenBSD__)
	s = splsoftnet();
#else
	s = splnet();
#endif
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	socket_lock(so, 1);
#endif
	if (control) {
		/* process cmsg snd/rcv info (maybe a assoc-id) */
		if (sctp_find_cmsg(SCTP_SNDRCV, (void *)&srcv, control,
		    sizeof(srcv))) {
			/* got one */
			use_rcvinfo = 1;
		}
	}
	error = sctp_lower_sosend(so, addr, uio, top, control, flags,
				  use_rcvinfo, &srcv, p);
	splx(s);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	socket_unlock(so, 1);
#endif
	return (error);
}

int
sctp_lower_sosend(struct socket *so,
    struct sockaddr *addr,
    struct uio *uio,
    struct mbuf *top,
    struct mbuf *control,
    int flags,
    int use_rcvinfo,
    struct sctp_sndrcvinfo *srcv,
#if defined(__FreeBSD__) && __FreeBSD_version >= 500000
    struct thread *p
#else
    struct proc *p
#endif
)
{
	unsigned int sndlen, max_len;
	int error, len;
	int s, queue_only = 0, queue_only_for_init = 0;
	int free_cnt_applied = 0;
	int un_sent = 0;
	int now_filled = 0;
	struct sctp_block_entry be;
	struct sctp_inpcb *inp;
	struct sctp_tcb *stcb = NULL;
	struct timeval now;
	struct sctp_nets *net;
	struct sctp_association *asoc;
	struct sctp_inpcb *t_inp;
	int create_lock_applied = 0;
	int nagle_applies = 0;
	int some_on_control = 0;
	int got_all_of_the_send = 0;
	int hold_tcblock = 0;
	int non_blocking = 0;

	error = 0;
	net = NULL;
	stcb = NULL;
	asoc = NULL;
	t_inp = inp = (struct sctp_inpcb *)so->so_pcb;
	if (uio)
		sndlen = uio->uio_resid;
	else
		sndlen = top->m_pkthdr.len;

#if defined(__NetBSD__) || defined(__OpenBSD__)
	s = splsoftnet();
#else
	s = splnet();
#endif
	if ((stcb) && ((so->so_state & SS_NBIO)
#if defined(__FreeBSD__) && __FreeBSD_version >= 500000
		       || (flags & MSG_NBIO)
#endif
		    )) {
		non_blocking = 1;
	}

	if ((inp->sctp_flags & SCTP_PCB_FLAGS_TCPTYPE) &&
	    (inp->sctp_socket->so_qlimit)) {
		/* The listener can NOT send */
		error = EFAULT;
		splx(s);
		goto out_unlocked;
	}
	if (use_rcvinfo) {
		if (srcv->sinfo_flags & SCTP_SENDALL) {
			/* its a sendall */
			sctppcbinfo.mbuf_track--;
			error = sctp_sendall(inp, uio, top, srcv);
			top = NULL;
			splx(s);
			goto out_unlocked;
		}
	}
	if (addr) {
		SCTP_ASOC_CREATE_LOCK(inp);
		create_lock_applied = 1;
		if ((inp->sctp_flags & SCTP_PCB_FLAGS_SOCKET_GONE) ||
		    (inp->sctp_flags & SCTP_PCB_FLAGS_SOCKET_ALLGONE)) {
			/* Should I really unlock ? */
			error = EFAULT;
			splx(s);
			goto out_unlocked;

		}
		if (((inp->sctp_flags & SCTP_PCB_FLAGS_BOUND_V6) == 0) &&
		    (addr->sa_family == AF_INET6)) {
			error = EINVAL;
			splx(s);
			goto out_unlocked;
		}
	}
	/* now we must find the assoc */
	if (inp->sctp_flags & SCTP_PCB_FLAGS_CONNECTED) {
		SCTP_INP_RLOCK(inp);
		stcb = LIST_FIRST(&inp->sctp_asoc_list);
		if (stcb == NULL) {
			SCTP_INP_RUNLOCK(inp);
			error = ENOTCONN;
			splx(s);
			goto out_unlocked;
		}
		SCTP_TCB_LOCK(stcb);
		hold_tcblock = 1;
		SCTP_INP_RUNLOCK(inp);
		net = stcb->asoc.primary_destination;
	}
	/* get control */
	if (stcb == NULL) {
		/* Need to do a lookup */
		if (use_rcvinfo && srcv->sinfo_assoc_id) {
			stcb = sctp_findassociation_ep_asocid(inp, srcv->sinfo_assoc_id);
			/*
			 * Question: Should I error here if the assoc_id is
			 * no longer valid? i.e. I can't find it?
			 */
			if ((stcb) &&
			    (addr != NULL)) {
				/* Must locate the net structure */
				net = sctp_findnet(stcb, addr);
			}
			if (stcb) {
				hold_tcblock = 1;
			}
		}
		if (non_blocking) {
			if ((so->so_snd.sb_hiwat <
			     (sndlen + stcb->asoc.total_output_queue_size)) ||
			    (stcb->asoc.chunks_on_out_queue >
			     sctp_max_chunks_on_queue)) {
				error = EWOULDBLOCK;
				splx(s);
				goto out_unlocked;
			}
		}
		if (stcb == NULL) {
			if (addr != NULL) {
				/*
				 * Since we did not use findep we must
				 * increment it, and if we don't find a tcb
				 * decrement it.
				 */
				SCTP_INP_WLOCK(inp);
				SCTP_INP_INCR_REF(inp);
				SCTP_INP_WUNLOCK(inp);
				stcb = sctp_findassociation_ep_addr(&t_inp, addr, &net, NULL, NULL);
				if (stcb == NULL) {
					SCTP_INP_WLOCK(inp);
					SCTP_INP_DECR_REF(inp);
					SCTP_INP_WUNLOCK(inp);
				} else {
					hold_tcblock = 1;
				}
			}
		}
	}
	if ((stcb == NULL) &&
	    (inp->sctp_flags & SCTP_PCB_FLAGS_TCPTYPE)) {
		error = ENOTCONN;
		splx(s);
		goto out_unlocked;
	} else if ((stcb == NULL) && (addr == NULL)) {
		error = ENOENT;
		splx(s);
		goto out_unlocked;
	} else if (stcb == NULL) {
		/* UDP style, we must go ahead and start the INIT process */
		if ((use_rcvinfo) &&
		    (srcv->sinfo_flags & SCTP_ABORT)) {
			/* User asks to abort a non-existant asoc */
			error = ENOENT;
			splx(s);
			goto out_unlocked;
		}
		/* get an asoc/stcb struct */
		stcb = sctp_aloc_assoc(inp, addr, 1, &error, 0);
		if (stcb == NULL) {
			/* Error is setup for us in the call */
			splx(s);
			goto out_unlocked;
		}
		if (create_lock_applied) {
			SCTP_ASOC_CREATE_UNLOCK(inp);
			create_lock_applied = 0;
		} else {
			printf("Huh-3? create lock should have been on??\n");
		}
		/* Turn on queue only flag to prevent data from being sent */
		queue_only = 1;
		asoc = &stcb->asoc;
		asoc->state = SCTP_STATE_COOKIE_WAIT;
		SCTP_GETTIME_TIMEVAL(&asoc->time_entered);

		/*
		 * initialize authentication parameters for the assoc
		 */
		/* generate a RANDOM for this assoc */
		asoc->authinfo.random =
			sctp_generate_random_key(sctp_auth_random_len);
		/* initialize hmac list from endpoint */
		asoc->local_hmacs =
			sctp_copy_hmaclist(inp->sctp_ep.local_hmacs);
		/* initialize auth chunks list from endpoint */
		asoc->local_auth_chunks =
			sctp_copy_chunklist(inp->sctp_ep.local_auth_chunks);
		/* copy defaults from the endpoint */
		asoc->authinfo.assoc_keyid = inp->sctp_ep.default_keyid;

		if (control) {
			/* see if a init structure exists in cmsg headers */
			struct sctp_initmsg initm;
			int i;

			if (sctp_find_cmsg(SCTP_INIT, (void *)&initm, control,
					   sizeof(initm))) {
				/* we have an INIT override of the default */
				if (initm.sinit_max_attempts)
					asoc->max_init_times = initm.sinit_max_attempts;
				if (initm.sinit_num_ostreams)
					asoc->pre_open_streams = initm.sinit_num_ostreams;
				if (initm.sinit_max_instreams)
					asoc->max_inbound_streams = initm.sinit_max_instreams;
				if (initm.sinit_max_init_timeo)
					asoc->initial_init_rto_max = initm.sinit_max_init_timeo;
				if (asoc->streamoutcnt < asoc->pre_open_streams) {
					/* Default is NOT correct */
#ifdef SCTP_DEBUG
					if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
						printf("Ok, defout:%d pre_open:%d\n",
						       asoc->streamoutcnt, asoc->pre_open_streams);
					}
#endif
					FREE(asoc->strmout, M_PCB);
					asoc->strmout = NULL;
					asoc->streamoutcnt = asoc->pre_open_streams;

					/*
					 * What happens if this fails? .. we
					 * panic ...
					 */
					{
						struct sctp_stream_out *tmp_str;

						SCTP_TCB_UNLOCK(stcb);
						MALLOC(tmp_str,
						       struct sctp_stream_out *,
						       asoc->streamoutcnt *
						       sizeof(struct sctp_stream_out),
						       M_PCB, M_WAIT);
						SCTP_TCB_LOCK(stcb);
						hold_tcblock = 1;
						asoc->strmout = tmp_str;
					}
					for (i = 0; i < asoc->streamoutcnt; i++) {
						/*
						 * inbound side must be set
						 * to 0xffff, also NOTE when
						 * we get the INIT-ACK back
						 * (for INIT sender) we MUST
						 * reduce the count
						 * (streamoutcnt) but first
						 * check if we sent to any
						 * of the upper streams that
						 * were dropped (if some
						 * were). Those that were
						 * dropped must be notified
						 * to the upper layer as
						 * failed to send.
						 */
						asoc->strmout[i].next_sequence_sent = 0x0;
						TAILQ_INIT(&asoc->strmout[i].outqueue);
						asoc->strmout[i].stream_no = i;
						asoc->strmout[i].last_msg_incomplete = 0;
						asoc->strmout[i].next_spoke.tqe_next = 0;
						asoc->strmout[i].next_spoke.tqe_prev = 0;
					}
				}
			}
		}
		/* out with the INIT */
		queue_only_for_init = 1;
		/*
		 * we may want to dig in after this call and adjust the MTU
		 * value. It defaulted to 1500 (constant) but the ro
		 * structure may now have an update and thus we may need to
		 * change it BEFORE we append the message.
		 */
		net = stcb->asoc.primary_destination;
		asoc = &stcb->asoc;
	} else {
		asoc = &stcb->asoc;
	}
	/* Keep the stcb from being freed under our feet */
	atomic_add_16(&stcb->asoc.refcnt, 1);
	free_cnt_applied = 1;
	hold_tcblock = 1;
	if (stcb->asoc.state & SCTP_STATE_ABOUT_TO_BE_FREED) {
		error = ECONNRESET;
		goto out_unlocked;
	}
	if (create_lock_applied) {
		SCTP_ASOC_CREATE_UNLOCK(inp);
		create_lock_applied = 0;
	}
	if (asoc->stream_reset_outstanding) {
		/*
		 * Can't queue any data while stream reset is underway.
		 */
		error = EAGAIN;
		goto out_unlocked;
	}
	if ((SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_WAIT) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_ECHOED)) {
		queue_only = 1;
	}
	if (use_rcvinfo == 0) {
		/* Grab the default stuff from the asoc */
		srcv = &stcb->asoc.def_send;
	}
	/* we are now done with all control */
	if (control) {
		sctp_m_freem(control);
		control = NULL;
	}
	if ((SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_SENT) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_RECEIVED) ||
	    (SCTP_GET_STATE(asoc) == SCTP_STATE_SHUTDOWN_ACK_SENT) ||
	    (asoc->state & SCTP_STATE_SHUTDOWN_PENDING)) {
		if ((use_rcvinfo) &&
		    (srcv->sinfo_flags & SCTP_ABORT)) {
			;
		} else {
			error = ECONNRESET;
			splx(s);
			goto out_unlocked;
		}
	}
	/* Ok, we will attempt a msgsnd :> */
	if (p) {
#if defined(__FreeBSD__) && __FreeBSD_version >= 500000
		p->td_proc->p_stats->p_ru.ru_msgsnd++;
#else
		p->p_stats->p_ru.ru_msgsnd++;
#endif
	}

	if (stcb) {
		if (net && ((srcv->sinfo_flags & SCTP_ADDR_OVER))) {
			/* we take the override or the unconfirmed */
			;
		} else {
			net = stcb->asoc.primary_destination;
		}
	}
	if ((net->flight_size > net->cwnd) && (sctp_cmt_on_off == 0)) {
		/*
		 * CMT: Added check for CMT above. net above is the primary
		 * dest. If CMT is ON, sender should always attempt to send
		 * with the output routine sctp_fill_outqueue() that loops
		 * through all destination addresses. Therefore, if CMT is
		 * ON, queue_only is NOT set to 1 here, so that
		 * sctp_chunk_output() can be called below.
		 */
		queue_only = 1;

	} else if (asoc->ifp_had_enobuf) {
		SCTP_STAT_INCR(sctps_ifnomemqueued);
		queue_only = 1;
	} else {
		un_sent = ((stcb->asoc.total_output_queue_size - stcb->asoc.total_flight) +
			   ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) * sizeof(struct sctp_data_chunk)));
	}
	/* Calculate the maximum we can send */
	max_len = so->so_snd.sb_hiwat -  stcb->asoc.total_output_queue_size;

	/* Are we aborting? */
	if (srcv->sinfo_flags & SCTP_ABORT) {
		struct mbuf *mm;
		int tot_demand, tot_out, max;

		if ((SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_WAIT) ||
		    (SCTP_GET_STATE(asoc) == SCTP_STATE_COOKIE_ECHOED)) {
			/* It has to be up before we abort */
			/* how big is the user initiated abort? */
			error = EINVAL;
			goto out;
		}
		SCTP_TCB_UNLOCK(stcb);
		hold_tcblock = 0;
		if(top) {
			mm = sctp_get_mbuf_for_msg(1, 1, M_WAIT, 1, MT_DATA);
			if(top->m_flags & M_PKTHDR)
				tot_out = top->m_pkthdr.len;
			else {
				struct mbuf *cntm;
				tot_out = 0;
				cntm = top;
				while(cntm) {
					tot_out += cntm->m_len;
					cntm = cntm->m_next;
				}
			}
			tot_demand = (tot_out + sizeof(struct sctp_paramhdr));
		} else {
			/* Must fit in a MTU */
			tot_out = uio->uio_resid;
			tot_demand = (tot_out + sizeof(struct sctp_paramhdr));
			mm = sctp_get_mbuf_for_msg(tot_demand, 1, M_WAIT, 1, MT_DATA);
		}
		if(mm == NULL) {
			error = ENOMEM;
			goto out;
		}
		max = asoc->smallest_mtu - sizeof(struct sctp_paramhdr);
		max -= sizeof(struct sctp_abort_msg);
		if (tot_out > max) {
			tot_out = max;
		}
		if (mm) {
			struct sctp_paramhdr *ph;
			/* now move forward the data pointer */
			ph = mtod(mm, struct sctp_paramhdr *);
			ph->param_type = htons(SCTP_CAUSE_USER_INITIATED_ABT);
			ph->param_length = htons((sizeof(struct sctp_paramhdr) + tot_out));
			ph++;
			mm->m_pkthdr.len = tot_out + sizeof(struct sctp_paramhdr);
			mm->m_len = mm->m_pkthdr.len;
			if(top == NULL) {
				error = uiomove((caddr_t)ph, (int)tot_out, uio);
				if (error) {
					/*
					 * Here if we can't get his data we
					 * still abort we just don't get to
					 * send the users note :-0
					 */
					sctp_m_freem(mm);
					mm = NULL;
				}
			} else {
				mm->m_next = top;
			}
		}
		SCTP_TCB_LOCK(stcb);
		hold_tcblock = 1;
		atomic_add_16(&stcb->asoc.refcnt, -1);
		free_cnt_applied = 0;
		/* release this lock, otherwise we hang on ourselves */
		sctp_abort_an_association(stcb->sctp_ep, stcb,
					  SCTP_RESPONSE_TO_USER_REQ,
					  mm);
		/* now relock the stcb so everything is sane */
		hold_tcblock = 0;
		stcb = NULL;
		goto out_unlocked;
	}
	SCTP_TCB_UNLOCK(stcb);
	hold_tcblock = 0;
	splx(s);
	/* Is the stream no. valid? */
	if (srcv->sinfo_stream >= asoc->streamoutcnt) {
		/* Invalid stream number */
		error = EINVAL;
		goto out_unlocked;
	}
	if (asoc->strmout == NULL) {
		/* huh? software error */
		error = EFAULT;
		goto out_unlocked;
	}
	len = 0;
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	error = sblock(&so->so_rcv, SBLOCKWAIT(in_flags));
	SAVE_I_AM_HERE(inp);
#endif
#if defined(__NetBSD__)
	error = sblock(&so->so_rcv, SBLOCKWAIT(in_flags));
#endif
	if (top == NULL) {
		struct sctp_stream_queue_pending *sp;
		struct sctp_stream_out *strm;
		uint32_t sndout, initial_out;
		int user_marks_eor;

		if(uio->uio_resid == 0) {
			if(srcv->sinfo_flags & SCTP_EOF) {
				got_all_of_the_send = 1;
				goto dataless_eof;
			} else {
				error = EINVAL;
				goto out;
			}
		}
		initial_out = uio->uio_resid;

		if ((asoc->stream_locked) &&  
		    (asoc->stream_locked_on  != srcv->sinfo_stream)) {
			error = EAGAIN;
			goto out;
		}
		strm = &stcb->asoc.strmout[srcv->sinfo_stream];
		user_marks_eor = sctp_is_feature_on(inp, SCTP_PCB_FLAGS_EXPLICIT_EOR);
		if(strm->last_msg_incomplete == 0) {
			sp = sctp_copy_it_in(stcb, asoc, srcv, uio, net, max_len, user_marks_eor, &error, non_blocking);
			if ((sp == NULL) || (error)) {
				goto out;
			}
			SCTP_TCB_LOCK(stcb);
			if(sp->msg_is_complete) {
				strm->last_msg_incomplete = 0;
			} else {
				strm->last_msg_incomplete = 1;
			}
			sctp_snd_sb_alloc(stcb, sp->length);

			asoc->stream_queue_cnt++;
			TAILQ_INSERT_TAIL(&strm->outqueue, sp, next);
			if ((srcv->sinfo_flags & SCTP_UNORDERED) == 0) {
				sp->strseq = strm->next_sequence_sent;
				strm->next_sequence_sent++;
			}

			if ((strm->next_spoke.tqe_next == NULL) &&
			    (strm->next_spoke.tqe_prev == NULL)) {
				/* Not on wheel, insert */
				sctp_insert_on_wheel(asoc, strm);
			}
			SCTP_TCB_UNLOCK(stcb);
		} else {
			sp = TAILQ_LAST(&strm->outqueue, sctp_streamhead);
		}
		while (uio->uio_resid > 0) {
			/* How much room do we have? */
			struct mbuf *new_tail, *mm;

			if(so->so_snd.sb_hiwat > stcb->asoc.total_output_queue_size)
				max_len = so->so_snd.sb_hiwat - stcb->asoc.total_output_queue_size;
			else
				max_len = 0;
			if(max_len > 0) {
				sndout = 0;
				new_tail = NULL;
				if (hold_tcblock) {
					SCTP_TCB_UNLOCK(stcb);
					hold_tcblock = 0;
				}
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
				socket_unlock(stcb->sctp_socket, 0);
#endif
				mm = sctp_copy_resume(sp, uio, srcv, max_len, user_marks_eor, &error, &sndout, &new_tail);
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
				socket_lock(stcb->sctp_socket, 0);
#endif
				if ((mm == NULL) || error) {
					goto out;
				}
				/* Update the mbuf and count */
				SCTP_TCB_LOCK(stcb);
				hold_tcblock = 1;
				/* Did we reach EOR? */
				if ((uio->uio_resid == 0) &&
				    ((user_marks_eor == 0) || 
				     (user_marks_eor && (srcv->sinfo_flags & SCTP_EOR)))
					){
					sp->msg_is_complete = 1;
				} else {
					sp->msg_is_complete = 0;
				}
				sctp_snd_sb_alloc(stcb, sndout);

				if(sp->tail_mbuf) {
					/* tack it to the end */
					sp->tail_mbuf->m_next = mm;
					sp->tail_mbuf = new_tail;
				} else {
					/* A stolen mbuf */
					sp->data = mm;
					sp->tail_mbuf = new_tail;
					mm->m_pkthdr.len = sndout;
				}
				sp->length += sndout;
				len += sndout;
				if(sp->data->m_flags & M_PKTHDR) {
					/* update length */
					sp->data->m_pkthdr.len = sp->length;
				}
			}
			if(uio->uio_resid == 0) {
				/* got it all? */
				continue;
			}
			/* PR-SCTP? */
			if ((asoc->peer_supports_prsctp) && (asoc->sent_queue_cnt_removeable > 0)) {
				/* This is ugly but we must assure locking order */
				if (hold_tcblock == 0) {
					SCTP_TCB_LOCK(stcb);
					hold_tcblock = 1;
				}
				sctp_prune_prsctp(stcb, asoc, srcv, sndlen);
				if(so->so_snd.sb_hiwat > stcb->asoc.total_output_queue_size)
					max_len = so->so_snd.sb_hiwat - stcb->asoc.total_output_queue_size;
				else
					max_len = 0;
				if(max_len > 0) {
					continue;
				}
			}
			/* wait for space now */
			if (non_blocking) {
				/* Non-blocking io in place out */
				goto skip_out_eof;
			}
			un_sent = ((stcb->asoc.total_output_queue_size - stcb->asoc.total_flight) +
				   ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) * sizeof(struct sctp_data_chunk)));

			if ((sctp_is_feature_off(inp, SCTP_PCB_FLAGS_NODELAY)) &&
			    (stcb->asoc.total_flight > 0) &&
			    (un_sent < (int)(stcb->asoc.smallest_mtu - SCTP_MIN_OVERHEAD)) &&
			    ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) < SCTP_MAX_DATA_BUNDLING)
				) {

				/*
				 * Ok, Nagle is set on and we have data outstanding.
				 * Don't send anything and let SACKs drive out the
				 * data unless wen have a "full" segment to send.
				 */
#ifdef SCTP_NAGLE_LOGGING
				sctp_log_nagle_event(stcb, SCTP_NAGLE_APPLIED);
#endif
				SCTP_STAT_INCR(sctps_naglequeued);
				nagle_applies = 1;
			} else {
#ifdef SCTP_NAGLE_LOGGING
				if (sctp_is_feature_off(inp, SCTP_PCB_FLAGS_NODELAY))
					sctp_log_nagle_event(stcb, SCTP_NAGLE_SKIPPED);
#endif
				SCTP_STAT_INCR(sctps_naglesent);
				nagle_applies = 0;
			}
			/* What about the INIT, send it maybe */
#ifdef SCTP_BLK_LOGGING
			sctp_misc_ints(SCTP_CWNDLOG_PRESEND, queue_only_for_init, queue_only, nagle_applies, un_sent);
			sctp_misc_ints(SCTP_CWNDLOG_PRESEND, stcb->asoc.total_output_queue_size, stcb->asoc.total_flight,
				       stcb->asoc.chunks_on_out_queue, stcb->asoc.total_flight_count);
#endif
			if (queue_only_for_init) {
				if (hold_tcblock == 0) {
					SCTP_TCB_LOCK(stcb);
					hold_tcblock = 1;
				}
				sctp_send_initiate(inp, stcb);
				queue_only_for_init = 0;
				queue_only = 1;
			}
			if((queue_only == 0) && (nagle_applies == 0)
				) {
				/* need to start chunk output
				 * before blocking.
				 */
				if(hold_tcblock == 0) {
					SCTP_TCB_LOCK(stcb);
					hold_tcblock = 1;
				}
				sctp_chunk_output(inp, 
						  stcb, 
						  SCTP_OUTPUT_FROM_USR_SEND);
			}
			if(hold_tcblock == 1) {
				SCTP_TCB_UNLOCK(stcb);
				hold_tcblock = 0;
			}
			SOCKBUF_LOCK(&so->so_snd);
			/* This is a bit strange, but I think it will
			 * work. The total_output_queue_size is locked and
			 * protected by the TCB_LOCK, which we just released.
			 * There is a race that can occur between releasing it
			 * above, and me getting the socket lock, where sacks
			 * come in but we have not put the SB_WAIT on the
			 * so_snd buffer to get the wakeup. After the LOCK
			 * is applied the sack_processing will also need to
			 * LOCK the so->so_snd to do the actual sowwakeup(). So
			 * once we have the socket buffer lock if we recheck the
			 * size we KNOW we will get to sleep safely with the
			 * wakeup flag in place.
			 */
			if(so->so_snd.sb_hiwat <= stcb->asoc.total_output_queue_size) {
#ifdef SCTP_BLK_LOGGING
				sctp_log_block(SCTP_BLOCK_LOG_INTO_BLK,
					       so, asoc, uio->uio_resid);
#endif
				be.error = 0;
				stcb->block_entry = &be;
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
				sbunlock(&so->so_snd, 1);
#endif
#if defined (__NetBSD__)
				sbunlock(&so->so_snd);
#endif
				error = sbwait(&so->so_snd);
				stcb->block_entry = NULL;

				if (error || so->so_error || be.error) {
					if (error == 0) {
						if (so->so_error)
							error = so->so_error;
						if (be.error) {
							error = be.error;
						}
					}
					SOCKBUF_UNLOCK(&so->so_snd);
					goto out_unlocked;
				}
#ifdef SCTP_BLK_LOGGING
				sctp_log_block(SCTP_BLOCK_LOG_OUTOF_BLK,
					       so, asoc, stcb->asoc.total_output_queue_size);
#endif
			}
			SOCKBUF_UNLOCK(&so->so_snd);
		}
		if (hold_tcblock == 0) {
			SCTP_TCB_LOCK(stcb);
			hold_tcblock = 1;
		}
		if(sp->msg_is_complete == 0) {
			strm->last_msg_incomplete = 1;
			asoc->stream_locked = 1;
			asoc->stream_locked_on  = srcv->sinfo_stream;
		} else {
			strm->last_msg_incomplete = 0;
		}
		if(uio->uio_resid == 0) {
			got_all_of_the_send = 1;
		}
	} else if (top) {
		/* We send in a 0, since we do NOT have any locks */
		error = sctp_msg_append(stcb, net, top, srcv, 0);
		top = NULL;
	}
	if (error) {
		goto out;
	}

 dataless_eof:
	/* EOF thing ? */
	if ((srcv->sinfo_flags & SCTP_EOF) &&
	    (got_all_of_the_send == 1) &&
	    (stcb->sctp_ep->sctp_flags & SCTP_PCB_FLAGS_UDPTYPE)
		) {
		error = 0;
		if(hold_tcblock == 0) {
			SCTP_TCB_LOCK(stcb);
			hold_tcblock = 1;
		}
		if (TAILQ_EMPTY(&asoc->send_queue) &&
		    TAILQ_EMPTY(&asoc->sent_queue) &&
		    (asoc->stream_queue_cnt == 0)) {
			if(asoc->locked_on_sending) {
				goto abort_anyway;
			}
			/* there is nothing queued to send, so I'm done... */
			if ((SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_SENT) &&
			    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_RECEIVED) &&
			    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_ACK_SENT)) {
				/* only send SHUTDOWN the first time through */
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_OUTPUT4) {
					printf("%s:%d sends a shutdown\n",
					       __FILE__,
					       __LINE__
						);
				}
#endif
				sctp_send_shutdown(stcb, stcb->asoc.primary_destination);
				asoc->state = SCTP_STATE_SHUTDOWN_SENT;
				sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWN, stcb->sctp_ep, stcb,
						 asoc->primary_destination);
				sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWNGUARD, stcb->sctp_ep, stcb,
						 asoc->primary_destination);
			}
		} else {
			/*
			 * we still got (or just got) data to send, so set
			 * SHUTDOWN_PENDING
			 */
			/*
			 * XXX sockets draft says that SCTP_EOF should be
			 * sent with no data.  currently, we will allow user
			 * data to be sent first and move to
			 * SHUTDOWN-PENDING
			 */
			if ((SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_SENT) &&
			    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_RECEIVED) &&
			    (SCTP_GET_STATE(asoc) != SCTP_STATE_SHUTDOWN_ACK_SENT)) {
				if (asoc->locked_on_sending) {
					/* Locked to send out the data */
					struct sctp_stream_queue_pending *sp;
					sp = TAILQ_LAST(&asoc->locked_on_sending->outqueue, sctp_streamhead);
					if(sp) {
						if ((sp->length == 0) && (sp->msg_is_complete == 0))
							asoc->state |= SCTP_STATE_PARTIAL_MSG_LEFT;
					}
				}
				asoc->state |= SCTP_STATE_SHUTDOWN_PENDING;
				if (TAILQ_EMPTY(&asoc->send_queue) &&
				    TAILQ_EMPTY(&asoc->sent_queue) &&
				    (asoc->state & SCTP_STATE_PARTIAL_MSG_LEFT)) {
				abort_anyway:
					sctp_abort_an_association(stcb->sctp_ep, stcb,
								  SCTP_RESPONSE_TO_USER_REQ,
								  NULL);
					/* now relock the stcb so everything is sane */
					hold_tcblock = 0;
					if (free_cnt_applied) {
						atomic_add_16(&stcb->asoc.refcnt, -1);
						free_cnt_applied = 0;
					}
					stcb = NULL;
					goto out;
				}
				sctp_timer_start(SCTP_TIMER_TYPE_SHUTDOWNGUARD, stcb->sctp_ep, stcb,
						 asoc->primary_destination);
			}
		}
	}	
 skip_out_eof:
	if (!TAILQ_EMPTY(&stcb->asoc.control_send_queue)) {
		some_on_control = 1;
	}
	/* re-calc unsent */
	un_sent = ((stcb->asoc.total_output_queue_size - stcb->asoc.total_flight) +
		   ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) * sizeof(struct sctp_data_chunk)));

	if ((sctp_is_feature_off(inp, SCTP_PCB_FLAGS_NODELAY)) &&
	    (stcb->asoc.total_flight > 0) &&
	    (un_sent < (int)(stcb->asoc.smallest_mtu - SCTP_MIN_OVERHEAD)) &&
	    ((stcb->asoc.chunks_on_out_queue - stcb->asoc.total_flight_count) < SCTP_MAX_DATA_BUNDLING)
		) {

		/*
		 * Ok, Nagle is set on and we have data outstanding.
		 * Don't send anything and let SACKs drive out the
		 * data unless wen have a "full" segment to send.
		 */
#ifdef SCTP_NAGLE_LOGGING
		sctp_log_nagle_event(stcb, SCTP_NAGLE_APPLIED);
#endif
		SCTP_STAT_INCR(sctps_naglequeued);
		nagle_applies = 1;
	} else {
#ifdef SCTP_NAGLE_LOGGING
		if (sctp_is_feature_off(inp, SCTP_PCB_FLAGS_NODELAY))
			sctp_log_nagle_event(stcb, SCTP_NAGLE_SKIPPED);
#endif
		SCTP_STAT_INCR(sctps_naglesent);
		nagle_applies = 0;
	}

	if (queue_only_for_init) {
		if (hold_tcblock == 0) {
			SCTP_TCB_LOCK(stcb);
			hold_tcblock = 1;
		}
		sctp_send_initiate(inp, stcb);
		queue_only_for_init = 0;
		queue_only = 1;
	}
	if ((queue_only == 0) && (nagle_applies == 0) && (stcb->asoc.peers_rwnd && un_sent)) {
		/* we can attempt to send too. */
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
			printf("USR Send calls sctp_chunk_output\n");
		}
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
		s = splsoftnet();
#else
		s = splnet();
#endif
		if (hold_tcblock == 0) {
			hold_tcblock = 1;
			SCTP_TCB_LOCK(stcb);
		}
		sctp_chunk_output(inp, stcb, SCTP_OUTPUT_FROM_USR_SEND);
		splx(s);
	} else if ((queue_only == 0) &&
		   (stcb->asoc.peers_rwnd == 0) &&
		   (stcb->asoc.total_flight == 0)) {
		/* We get to have a probe outstanding */
#if defined(__NetBSD__) || defined(__OpenBSD__)
		s = splsoftnet();
#endif
		if (hold_tcblock == 0) {
			hold_tcblock = 1;
			SCTP_TCB_LOCK(stcb);
		}
		sctp_chunk_output(inp, stcb, SCTP_OUTPUT_FROM_USR_SEND);
#if defined(__NetBSD__) || defined(__OpenBSD__)
		splx(s);
#endif
	} else if (some_on_control) {
		int num_out, reason, cwnd_full;

		/* Here we do control only */
#if defined(__NetBSD__) || defined(__OpenBSD__)
		s = splsoftnet();
#endif
		if (hold_tcblock == 0) {
			hold_tcblock = 1;
			SCTP_TCB_LOCK(stcb);
		}
		sctp_med_chunk_output(inp, stcb, &stcb->asoc, &num_out,
				      &reason, 1, &cwnd_full, 1, &now, &now_filled);
#if defined(__NetBSD__) || defined(__OpenBSD__)
		splx(s);
#endif
	}
#ifdef SCTP_DEBUG
	if (sctp_debug_on & SCTP_DEBUG_OUTPUT1) {
		printf("USR Send complete qo:%d prw:%d unsent:%d tf:%d cooq:%d toqs:%d \n",
		       queue_only, stcb->asoc.peers_rwnd, un_sent,
		       stcb->asoc.total_flight, stcb->asoc.chunks_on_out_queue,
		       stcb->asoc.total_output_queue_size);
	}
#endif
 out:
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	SAVE_I_AM_HERE(inp);
#endif
#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
	sbunlock(&so->so_rcv, 1);
#endif
#if defined(__NetBSD__)
	sbunlock(&so->so_rcv);
#endif
 out_unlocked:

	if (create_lock_applied) {
		SCTP_ASOC_CREATE_UNLOCK(inp);
		create_lock_applied = 0;
	}
	if ((stcb) && hold_tcblock) {
		SCTP_TCB_UNLOCK(stcb);
	}
	if ((stcb) && (free_cnt_applied)) {
		atomic_add_16(&stcb->asoc.refcnt, -1);
	}

	if (top){ 
		printf("Free top:%x\n", (u_int)top);
		sctp_m_freem(top);
	}
	if (control) {
		printf("Free control:%x\n", (u_int)control);
		sctp_m_freem(control);
	}

	return (error);
}


/*
 * generate an AUTHentication chunk, if required
 */
struct mbuf *
sctp_add_auth_chunk(struct mbuf *m, struct mbuf **m_end,
    struct sctp_auth_chunk **auth_ret, uint32_t * offset,
    struct sctp_tcb *stcb, uint8_t chunk)
{
	struct mbuf *m_auth;
	struct sctp_auth_chunk *auth;
	int chunk_len;

	if ((m_end == NULL) || (auth_ret == NULL) || (offset == NULL) ||
	    (stcb == NULL))
		return (m);

	/* sysctl disabled auth? */
	if (sctp_auth_disable)
		return (m);

	/* peer doesn't do auth... */
	if (!stcb->asoc.peer_supports_auth) {
		return (m);
	}
	/* does the requested chunk require auth? */
	if (!sctp_auth_is_required_chunk(chunk, stcb->asoc.peer_auth_chunks)) {
		return (m);
	}
	m_auth = sctp_get_mbuf_for_msg(sizeof(*auth), 1, M_DONTWAIT, 1, MT_HEADER);
	if (m_auth == NULL) {
		/* no mbuf's */
		return (m);
	}
	/* reserve some space if this will be the first mbuf */
	if (m == NULL)
		m_auth->m_data += SCTP_MIN_OVERHEAD;
	/* fill in the AUTH chunk details */
	auth = mtod(m_auth, struct sctp_auth_chunk *);
	bzero(auth, sizeof(*auth));
	auth->ch.chunk_type = SCTP_AUTHENTICATION;
	auth->ch.chunk_flags = 0;
	chunk_len = sizeof(*auth) +
	    sctp_get_hmac_digest_len(stcb->asoc.peer_hmac_id);
	auth->ch.chunk_length = htons(chunk_len);
	auth->hmac_id = htons(stcb->asoc.peer_hmac_id);
	/* key id and hmac digest will be computed and filled in upon send */

	/* save the offset where the auth was inserted into the chain */
	if (m != NULL)
		*offset = m->m_pkthdr.len;
	else
		*offset = 0;

	/* update length and return pointer to the auth chunk */
	m_auth->m_pkthdr.len = m_auth->m_len = chunk_len;
	m = sctp_copy_mbufchain(m_auth, m, m_end, 1, 1);
	if (auth_ret != NULL)
		*auth_ret = auth;

	return (m);
}
