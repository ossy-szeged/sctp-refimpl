/*
 * Copyright (C) 2005 -2009 Michael Tuexen, tuexen@fh-muenster.de,
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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#ifdef LINUX
#include <getopt.h>
#endif
#include <netinet/sctp_os.h>
#include <pthread.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/sctp_pcb.h>
#include <usrsctp.h>

/* global for the send callback, but used in kernel version as well */
static unsigned long number_of_messages;
static char *buffer;
static int length;
static struct sockaddr_in remote_addr;
static int unordered;
uint32_t optval=1;
struct socket *psock = NULL;
#include <errno.h>

#if defined(CALLBACK_API)
static struct timeval start_time;
static unsigned long messages = 0;
static unsigned int first_length = 0;
static unsigned long long sum = 0;
#endif

#ifndef timersub
#define timersub(tvp, uvp, vvp)                                   \
	do {                                                      \
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;    \
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec; \
		if ((vvp)->tv_usec < 0) {                         \
			(vvp)->tv_sec--;                          \
			(vvp)->tv_usec += 1000000;                \
		}                                                 \
	} while (0)
#endif


char Usage[] =
"Usage: tsctp [options] [address]\n"
"Options:\n"
"        -a             set adaptation layer indication\n"
"        -f             fragmentation point\n"
"        -l             size of send/receive buffer\n"
"        -n             number of messages sent (0 means infinite)/received\n"
"        -D             turns Nagle off\n"
"        -T             time to send messages\n"
"        -u             use unordered user messages\n"
"        -v             verbose\n"
"        -V             very verbose\n"
;

#define DEFAULT_LENGTH             1024
#define DEFAULT_NUMBER_OF_MESSAGES 1024
#define DEFAULT_PORT               5001
#define BUFFERSIZE                 (1<<16)
#define LINGERTIME                 1

static int verbose, very_verbose;
static unsigned int done;

void 
handle_notification(int fd, void *buf)
{
	union sctp_notification *snp;
	struct sctp_paddr_change *spc;
	struct timeval note_time;

	if (verbose) {
		gettimeofday(&note_time, NULL);
		snp = (union sctp_notification*) buf;
		printf("%04x notification arrived at %f\n", snp->sn_header.sn_type, note_time.tv_sec+(double)note_time.tv_usec/1000000.0);
		if (snp->sn_header.sn_type==SCTP_PEER_ADDR_CHANGE) {
			spc = &snp->sn_paddr_change;
			printf("SCTP_PEER_ADDR_CHANGE: state=%d, error=%d\n",spc->spc_state, spc->spc_error);
		}
	}
}

void stop_sender(int sig)
{
	done = 1; 
}

#if !defined (CALLBACK_API)
static void* 
handle_connection(void *arg)
{
	ssize_t n;
	unsigned long long sum = 0;
	char *buf;
	pthread_t tid;
	struct socket *conn_sock;
	struct timeval start_time, now, diff_time;
	double seconds;
	unsigned long messages = 0;
	unsigned long recv_calls = 0;
	unsigned long notifications = 0;
	struct sctp_sndrcvinfo sinfo;
	unsigned int first_length;
	int flags;
	struct sockaddr_in addr;
	socklen_t len;
	union sctp_notification *snp;
	struct sctp_paddr_change *spc;
	struct timeval note_time;
	
	conn_sock = *(struct socket **)arg;
	tid = pthread_self();
	pthread_detach(tid);

	buf = malloc(BUFFERSIZE);
	flags = 0;
	len = (socklen_t)sizeof(struct sockaddr_in);
	n = userspace_sctp_recvmsg(conn_sock, buf, BUFFERSIZE, (struct sockaddr *) &addr, &len, &sinfo, &flags);
	gettimeofday(&start_time, NULL);
	first_length = 0;
	while (n > 0) {
		recv_calls++;
		if (flags & MSG_NOTIFICATION) {
			notifications++;
			gettimeofday(&note_time, NULL);
			printf("notification arrived at %f\n", note_time.tv_sec+(double)note_time.tv_usec/1000000.0);
			snp = (union sctp_notification*)&buf;
			if (snp->sn_header.sn_type==SCTP_PEER_ADDR_CHANGE)
			{
				spc = &snp->sn_paddr_change;
				printf("SCTP_PEER_ADDR_CHANGE: state=%d, error=%d\n",spc->spc_state, spc->spc_error);
			}
		} else {
			sum += n;
			if (flags & MSG_EOR) {
				messages++;
				if (first_length == 0)
					first_length = sum;
			}
		}
		flags = 0;
		len = (socklen_t)sizeof(struct sockaddr_in);
		n = userspace_sctp_recvmsg(conn_sock, (void *) buf, BUFFERSIZE, (struct sockaddr *) &addr, &len, &sinfo, &flags);
	}
	if (n < 0)
		perror("sctp_recvmsg");
	gettimeofday(&now, NULL);
	timersub(&now, &start_time, &diff_time);
	seconds = diff_time.tv_sec + (double)diff_time.tv_usec/1000000.0;
	fprintf(stdout, "%u, %lu, %lu, %lu, %llu, %f, %f\n",
	        first_length, messages, recv_calls, notifications, sum, seconds, (double)first_length * (double)messages / seconds);
	fflush(stdout);
	userspace_close(conn_sock);
	free(buf);
	return NULL;
}
#else
#if defined(SCTP_USERSPACE_SEND_CALLBACK_USE_THRESHOLD)
static int 
send_cb(struct socket *sock, uint32_t sb_free) {

	while (!done && ((number_of_messages == 0) || (messages < (number_of_messages - 1)))) {
		if (very_verbose)
			printf("Sending message number %lu.\n", messages);

		if (userspace_sctp_sendmsg(psock /* struct socket *so */,
		                           buffer /* const void *data */,
		                           length /* size_t len */,
		                           (struct sockaddr *) &remote_addr /* const struct sockaddr *to */,
		                           sizeof(struct sockaddr_in) /* socklen_t tolen */,
		                           0 /* u_int32_t ppid */,
		                           unordered?SCTP_UNORDERED:0 /* u_int32_t flags */,
		                           0 /* u_int16_t stream_no */,
		                           0 /* u_int32_t timetolive */,
		                           0 /* u_int32_t context */) < 0) {
		if (errno != EWOULDBLOCK && errno != EAGAIN) {
			perror("userspace_sctp_sendmsg (cb) returned < 0");
			exit(1);
		} else {
			/* send until EWOULDBLOCK then exit callback. */
			return 1;
		}
	}
	messages++;
	}

	return 1;
}
#endif

static int
receive_cb(struct socket* sock, struct sctp_queued_to_read *control)
{
	struct timeval now, diff_time;
	double seconds;
	
	if (control==NULL)
	{
		gettimeofday(&now, NULL);
		timersub(&now, &start_time, &diff_time);
		seconds = diff_time.tv_sec + (double)diff_time.tv_usec/1000000.0;
		fprintf(stdout, "%u, %lu, %llu, %f, %f\n",
			first_length, messages, sum, seconds, (double)first_length * (double)messages / seconds);
		userspace_close(sock);
		first_length = 0;
		sum = 0;
		messages = 0;
		return 1;
	}
	if (first_length==0)
	{
		first_length = control->length;
		gettimeofday(&start_time, NULL);
	}
	sum += control->length;
	messages++;
	m_freem(control->data);
	return 1;
}
#endif


int main(int argc, char **argv)
{
        char c;
	socklen_t addr_len;
	struct sockaddr_in local_addr;
	struct timeval start_time, now, diff_time;
	int client;
	short local_port, remote_port, port;
	double seconds;
	double throughput;
	int nodelay = 0;
	unsigned long i;
	struct sctp_assoc_value av;
#if !defined (CALLBACK_API)
	pthread_t tid;
#endif
	int fragpoint = 0;
#ifdef CALLBACK_API
	int ret;
#endif
	unsigned int runtime = 0;
	struct sctp_setadaptation ind = {0};
	
	unordered = 0;

	length = DEFAULT_LENGTH;
	number_of_messages = DEFAULT_NUMBER_OF_MESSAGES;
	port = DEFAULT_PORT;
	verbose = 0;
	very_verbose = 0;

	memset((void *) &remote_addr, 0, sizeof(struct sockaddr_in));
	memset((void *) &local_addr, 0, sizeof(struct sockaddr_in));

	while ((c = getopt(argc, argv, "a:p:l:f:n:T:uvVD")) != -1)
		switch(c) {
			case 'a':
				ind.ssb_adaptation_ind = atoi(optarg);
				break;
			case 'l':
				length = atoi(optarg);
				break;
			case 'n':
				number_of_messages = atoi(optarg);
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'f':
				fragpoint = atoi(optarg);
				break;
			case 'T':
				runtime = atoi(optarg);
				number_of_messages = 0;
				break;
			case 'u':
				unordered = 1;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'V':
				verbose = 1;
				very_verbose = 1;
				break;
			case 'D':
				nodelay = 1;
				break;
			default:
				fprintf(stderr, "%s", Usage);
				exit(1);
		}

	if (optind == argc) {
		client = 0;
		local_port = port;
		remote_port = 0;
	} else {
		client = 1;
		local_port = 0;
		remote_port = port;
	}
	local_addr.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
	local_addr.sin_len = sizeof(struct sockaddr_in);
#endif
	local_addr.sin_port = htons(local_port);
	local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	sctp_init();
	SCTP_BASE_SYSCTL(sctp_udp_tunneling_for_client_enable) = 0;
	SCTP_BASE_SYSCTL(sctp_udp_tunneling_port) = 9899;
	SCTP_BASE_SYSCTL(sctp_debug_on) = 0xffffffff;

	if (!(psock = userspace_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)) ){
		printf("user_socket() returned NULL\n");
		exit(1);
	}

	if (userspace_bind(psock, (struct sockaddr *)&local_addr, sizeof(struct sockaddr_in)) == -1) {
		printf("userspace_bind failed.\n");
		exit(1);
	}

	if (userspace_setsockopt(psock, IPPROTO_SCTP, SCTP_ADAPTATION_LAYER, (const void*)&ind, (socklen_t)sizeof(struct sctp_setadaptation)) < 0) {
		perror("setsockopt");
	}

	if (!client) {
		if (userspace_listen(psock, 1) < 0) {
			printf("userspace_listen failed.\n");
			exit(1);
		}

		while (1) {
			struct socket **conn_sock;
			memset(&remote_addr, 0, sizeof(struct sockaddr_in));
			addr_len = sizeof(struct sockaddr_in);
			conn_sock = (struct socket **)malloc(sizeof(struct socket *));
			if ((*conn_sock = userspace_accept(psock, (struct sockaddr *) &remote_addr, &addr_len))== NULL) {
				printf("userspace_accept failed.  exiting...\n");
				continue;
			}
			if (verbose)
				fprintf(stdout,"Connection accepted from %s:%d\n", inet_ntoa(remote_addr.sin_addr), ntohs(remote_addr.sin_port));
#if defined (CALLBACK_API)
			ret = register_recv_cb(conn_sock, receive_cb);
#else
			pthread_create(&tid, NULL, &handle_connection, (void *)conn_sock);
#endif
		}
		userspace_close(psock);
	} else {
		remote_addr.sin_family = AF_INET;
#ifdef HAVE_SIN_LEN
		remote_addr.sin_len = sizeof(struct sockaddr_in);
#endif
		remote_addr.sin_addr.s_addr = inet_addr(argv[optind]);
		remote_addr.sin_port = htons(remote_port);

		/* TODO fragpoint stuff */
		if (nodelay == 1) {
			optval = 1;
		} else {
			optval = 0;
		}
		userspace_setsockopt(psock, IPPROTO_SCTP, SCTP_NODELAY, &optval, sizeof(int));

		if (fragpoint) {
			av.assoc_id = 0;
			av.assoc_value = fragpoint;
			if (userspace_setsockopt(psock, IPPROTO_SCTP, SCTP_MAXSEG, &av, sizeof(struct sctp_assoc_value)) < 0)
				perror("setsockopt: SCTP_MAXSEG");
		}

		if (userspace_connect(psock, (struct sockaddr *) &remote_addr, sizeof(struct sockaddr_in)) == -1 ) {
			printf("userspace_connect failed.  exiting...\n");
			exit(1);
		}

		buffer = malloc(length);
		memset(buffer, 0, length);

		gettimeofday(&start_time, NULL);
		if (verbose) {
			printf("Start sending %ld messages...", (long)number_of_messages);
			fflush(stdout);
		}

		i = 0;
		done = 0;

		if (runtime > 0) {
			signal(SIGALRM, stop_sender);
			alarm(runtime);
		}

#if defined(CALLBACK_API)
		messages = 0;
#if defined(SCTP_USERSPACE_SEND_CALLBACK_USE_THRESHOLD)
		ret = register_send_cb(psock, length, send_cb);
#else
		ret = register_send_cb(psock, length, NULL);
#endif
#endif
		while (!done && ((number_of_messages == 0) || (i < (number_of_messages - 1)))) {
			if (very_verbose)
				printf("Sending message number %lu.\n", i);

			if (userspace_sctp_sendmsg(psock /* struct socket *so */,
			                           buffer /* const void *data */,
			                           length /* size_t len */,
			                           (struct sockaddr *) &remote_addr /* const struct sockaddr *to */,
			                           sizeof(struct sockaddr_in) /* socklen_t tolen */,
			                           0 /* u_int32_t ppid */,
			                           unordered?SCTP_UNORDERED:0 /* u_int32_t flags */,
			                           0 /* u_int16_t stream_no */,
			                           0 /* u_int32_t timetolive */,
			                           0 /* u_int32_t context */) < 0) {
#if defined (CALLBACK_API)
				if (errno != EWOULDBLOCK && errno != EAGAIN) {
					perror("userspace_sctp_sendmsg returned < 0");
					exit(1);
				} else {
					/* send until EWOULDBLOCK then sleep until runtime expires.
					   All sending after initial EWOULDBLOCK done in send callback. */
					sleep(runtime);
					i += messages;
					continue;
				}
#else
				perror("userspace_sctp_sendmsg returned < 0");
				exit(1);
#endif
			}

			i++;
		}
		while (1) {
			if ((userspace_sctp_sendmsg(psock /* struct socket *so */,
			                            buffer /* const void *data */,
			                            length /* size_t len */,
			                            (struct sockaddr *) &remote_addr /* const struct sockaddr *to */,
			                            sizeof(struct sockaddr_in) /* socklen_t tolen */,
			                            0 /* u_int32_t ppid */,
			                            unordered?SCTP_EOF|SCTP_UNORDERED:SCTP_EOF /* u_int32_t flags */,
			                            0 /* u_int16_t stream_no */,
			                            0 /* u_int32_t timetolive */,
			                            0 /* u_int32_t context */)) < 0) {
				if (errno != EWOULDBLOCK && errno != EAGAIN) {
					perror("final userspace_sctp_sendmsg returned\n");
					exit(1);
				} else
					continue;
			}
			break;
		}
		i++;
		if (verbose)
			printf("done.\n");
		/* TODO SO_LINGER stuff */
		userspace_close(psock);
		gettimeofday(&now, NULL);
		timersub(&now, &start_time, &diff_time);
		seconds = diff_time.tv_sec + (double)diff_time.tv_usec/1000000;
		fprintf(stdout, "%s of %ld messages of length %u took %f seconds.\n",
				"Sending", i, length, seconds);
		throughput = (double)i * (double)length / seconds;
		fprintf(stdout, "Throughput was %f Byte/sec.\n", throughput);
	}
	sleep(10);
	sctp_finish();
	free (buffer);
	return 0;
}
