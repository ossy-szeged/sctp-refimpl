/*-
 * Copyright (C) 2006 Cisco Systems Inc,
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

#ifdef __FreeBSD__
#include <sys/cdefs.h>
__FBSDID("$FreeBSD:$");
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
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/random.h>
#endif
#if defined(__NetBSD__)
#include "rnd.h"
#include <sys/rnd.h>
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

#include <netinet/sctp.h>
#include <netinet/sctp_header.h>
#include <netinet/sctp_pcb.h>
#include <netinet/sctp_var.h>
#include <netinet/sctputil.h>
#include <netinet/sctp_indata.h>
#include <netinet/sctp_output.h>
#include <netinet/sctp_auth.h>

#ifdef SCTP_DEBUG
extern uint32_t sctp_debug_on;

#define SCTP_AUTH_DEBUG		(sctp_debug_on & SCTP_DEBUG_AUTH1)
#define SCTP_AUTH_DEBUG2	(sctp_debug_on & SCTP_DEBUG_AUTH2)
#endif				/* SCTP_DEBUG */

#if defined(SCTP_APPLE_FINE_GRAINED_LOCKING)
#define SCTP_MALLOC_NAMED(var, type, size, name) \
    do{ \
	MALLOC(var, type, size, M_PCB, M_WAITOK); \
    } while (0)
#else
#define SCTP_MALLOC_NAMED(var, type, size, name) \
    do{ \
	MALLOC(var, type, size, M_PCB, M_NOWAIT); \
    } while (0)
#endif
#define SCTP_FREE(var)	FREE(var, M_PCB)


/*
 * fill a buffer with random data
 */
void
sctp_read_random(void *buffer, uint32_t bytes)
{
#if defined(__FreeBSD__) && (__FreeBSD_version < 500000)
	read_random_unlimited(buffer, bytes);
#elif defined(__APPLE__) || (__FreeBSD_version > 500000)
	read_random(buffer, bytes);
#elif defined(__OpenBSD__)
	get_random_bytes(buffer, bytes);
#elif defined(__NetBSD__) && NRND > 0
	rnd_extract_data(buffer, bytes, RND_EXTRACT_ANY);
#else
	{
		uint8_t *rand_p;
		uint32_t rand;

		rand_p = (uint8_t *) buffer;
		while (bytes > 0) {
			rand = SCTP_RANDOM();
			if (bytes < sizeof(rand)) {
				bcopy(&rand, rand_p, bytes);
				return;
			}
			bcopy(&rand, rand_p, sizeof(rand));
			bytes -= sizeof(rand);
			rand_p += sizeof(rand);
		}
	}
#endif
}


inline void
sctp_clear_chunklist(sctp_auth_chklist_t * chklist)
{
	bzero(chklist, sizeof(*chklist));
	/* chklist->num_chunks = 0; */
}

sctp_auth_chklist_t *
sctp_alloc_chunklist(void)
{
	sctp_auth_chklist_t *chklist;

	SCTP_MALLOC_NAMED(chklist, sctp_auth_chklist_t *, sizeof(*chklist), "AUTH chklist");
	if (chklist == NULL) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_AUTH_DEBUG) {
			printf("sctp_alloc_chunklist: failed to get memory!\n");
		}
#endif				/* SCTP_DEBUG */
	} else {
		sctp_clear_chunklist(chklist);
	}
	return (chklist);
}

void
sctp_free_chunklist(sctp_auth_chklist_t * list)
{
	if (list != NULL)
		SCTP_FREE(list);
}

sctp_auth_chklist_t *
sctp_copy_chunklist(sctp_auth_chklist_t * list)
{
	sctp_auth_chklist_t *new_list;

	if (list == NULL)
		return (NULL);

	/* get a new list */
	new_list = sctp_alloc_chunklist();
	if (new_list == NULL)
		return (NULL);
	/* copy it */
	bcopy(list, new_list, sizeof(*new_list));

	return (new_list);
}

/*
 * is the chunk required to be authenticated?
 */
int
sctp_auth_is_required_chunk(uint8_t chunk, sctp_auth_chklist_t * list)
{
	if (list == NULL)
		return (0);
	return (list->chunks[chunk] != 0);
}

/*
 * add a chunk to the required chunks list
 */
int
sctp_auth_add_chunk(uint8_t chunk, sctp_auth_chklist_t * list)
{
	if (list == NULL)
		return (-1);

	/* is chunk restricted? */
	if ((chunk == SCTP_INITIATION) ||
	    (chunk == SCTP_INITIATION_ACK) ||
	    (chunk == SCTP_SHUTDOWN_COMPLETE) ||
	    (chunk == SCTP_AUTHENTICATION)) {
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: cannot add chunk %u (0x%02x) to Auth list\n",
			    chunk, chunk);
#endif
		return (-1);
	}
	if (list->chunks[chunk] == 0) {
		list->chunks[chunk] = 1;
		list->num_chunks++;
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: added chunk %u (0x%02x) to Auth list\n",
			    chunk, chunk);
#endif
	} else {
		/* chunk already set... ignore */
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: chunk %u (0x%02x) already in Auth list\n",
			    chunk, chunk);
#endif
	}
	return (0);
}

/*
 * delete a chunk from the required chunks list
 */
int
sctp_auth_delete_chunk(uint8_t chunk, sctp_auth_chklist_t * list)
{
	if (list == NULL)
		return (-1);

	/* is chunk restricted? */
	if ((chunk == SCTP_ASCONF) ||
	    (chunk == SCTP_ASCONF_ACK)) {
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: cannot delete chunk %u (0x%02x) from Auth list\n",
			    chunk, chunk);
#endif
		return (-1);
	}
	if (list->chunks[chunk] == 1) {
		list->chunks[chunk] = 0;
		list->num_chunks--;
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: deleted chunk %u (0x%02x) from Auth list\n",
			    chunk, chunk);
#endif
	} else {
		/* chunk already set... ignore */
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: chunk %u (0x%02x) not in Auth list\n",
			    chunk, chunk);
#endif
	}
	return (0);
}

inline int
sctp_auth_get_chklist_size(const sctp_auth_chklist_t * list)
{
	if (list == NULL)
		return (0);
	else
		return (list->num_chunks);
}

/*
 * set the default list of chunks requiring AUTH
 */
void
sctp_auth_set_default_chunks(sctp_auth_chklist_t * list)
{
	sctp_auth_add_chunk(SCTP_ASCONF, list);
	sctp_auth_add_chunk(SCTP_ASCONF_ACK, list);
}

/*
 * return the current number and list of required chunks caller must
 * guarantee ptr has space for up to 256 bytes
 */
int
sctp_serialize_auth_chunks(const sctp_auth_chklist_t * list, uint8_t * ptr)
{
	int i, count = 0;

	if (list == NULL)
		return (0);

	for (i = 0; i < 256; i++) {
		if (list->chunks[i] != 0) {
			*ptr++ = i;
			count++;
		}
	}
	return (count);
}

int
sctp_pack_auth_chunks(const sctp_auth_chklist_t * list, uint8_t * ptr)
{
	int i, size = 0;

	if (list == NULL)
		return (0);

	if (list->num_chunks <= 32) {
		/* just list them, one byte each */
		for (i = 0; i < 256; i++) {
			if (list->chunks[i] != 0) {
				*ptr++ = i;
				size++;
			}
		}
	} else {
		int index, offset;

		/* pack into a 32 byte bitfield */
		for (i = 0; i < 256; i++) {
			if (list->chunks[i] != 0) {
				index = i / 8;
				offset = i % 8;
				ptr[index] |= (1 << offset);
			}
		}
		size = 32;
	}
	return (size);
}

int
sctp_unpack_auth_chunks(const uint8_t * ptr, uint8_t num_chunks,
    sctp_auth_chklist_t * list)
{
	int i;
	int size;

	if (list == NULL)
		return (0);

	if (num_chunks <= 32) {
		/* just pull them, one byte each */
		for (i = 0; i < num_chunks; i++) {
			sctp_auth_add_chunk(*ptr++, list);
		}
		size = num_chunks;
	} else {
		int index, offset;

		/* unpack from a 32 byte bitfield */
		for (index = 0; index < 32; index++) {
			for (offset = 0; offset < 8; offset++) {
				if (ptr[index] & (1 << offset)) {
					sctp_auth_add_chunk((index * 8) + offset, list);
				}
			}
		}
		size = 32;
	}
	return (size);
}


/*
 * allocate structure space for a key of length keylen
 */
sctp_key_t *
sctp_alloc_key(uint32_t keylen)
{
	sctp_key_t *new_key;

	SCTP_MALLOC_NAMED(new_key, sctp_key_t *, sizeof(*new_key) + keylen,
	    "AUTH key");
	if (new_key == NULL) {
		/* out of memory */
		return (NULL);
	}
	new_key->keylen = keylen;
	return (new_key);
}

void
sctp_free_key(sctp_key_t * key)
{
	if (key != NULL)
		SCTP_FREE(key);
}

void
sctp_print_key(sctp_key_t * key, const char *str)
{
	uint32_t i;

	if (key == NULL) {
		printf("%s: [Null key]\n", str);
		return;
	}
	printf("%s: len %u, ", str, key->keylen);
	if (key->keylen) {
		for (i = 0; i < key->keylen; i++)
			printf("%02x", key->key[i]);
		printf("\n");
	} else {
		printf("[Null key]\n");
	}
}

void
sctp_show_key(sctp_key_t * key, const char *str)
{
	uint32_t i;

	if (key == NULL) {
		printf("%s: [Null key]\n", str);
		return;
	}
	printf("%s: len %u, ", str, key->keylen);
	if (key->keylen) {
		for (i = 0; i < key->keylen; i++)
			printf("%02x", key->key[i]);
		printf("\n");
	} else {
		printf("[Null key]\n");
	}
}

static inline uint32_t
sctp_get_keylen(sctp_key_t * key)
{
	if (key != NULL)
		return (key->keylen);
	else
		return (0);
}

/*
 * generate a new random key of length 'keylen'
 */
sctp_key_t *
sctp_generate_random_key(uint32_t keylen)
{
	sctp_key_t *new_key;

	/* validate keylen */
	if (keylen > SCTP_AUTH_RANDOM_SIZE_MAX)
		keylen = SCTP_AUTH_RANDOM_SIZE_MAX;

	new_key = sctp_alloc_key(keylen);
	if (new_key == NULL) {
		/* out of memory */
		return (NULL);
	}
	sctp_read_random(new_key->key, keylen);
	new_key->keylen = keylen;
	return (new_key);
}

sctp_key_t *
sctp_set_key(uint8_t * key, uint32_t keylen)
{
	sctp_key_t *new_key;

	new_key = sctp_alloc_key(keylen);
	if (new_key == NULL) {
		/* out of memory */
		return (NULL);
	}
	bcopy(key, new_key->key, keylen);
	return (new_key);
}


/*
 * given two keys of variable size, compute which key is "larger/smaller"
 * returns: 1 if key1 > key2 -1 if key1 < key2 0 if key1 = key2
 */
static int
sctp_compare_key(sctp_key_t * key1, sctp_key_t * key2)
{
	uint32_t maxlen;
	uint32_t i;
	uint32_t key1len, key2len;
	uint8_t *key_1, *key_2;
	uint8_t temp[SCTP_AUTH_RANDOM_SIZE_MAX];

	/* sanity/length check */
	key1len = sctp_get_keylen(key1);
	key2len = sctp_get_keylen(key2);
	if ((key1len == 0) && (key2len == 0))
		return (0);
	else if (key1len == 0)
		return (-1);
	else if (key2len == 0)
		return (1);

	if (key1len != key2len) {
		if (key1len >= key2len)
			maxlen = key1len;
		else
			maxlen = key2len;
		bzero(temp, maxlen);
		if (key1len < maxlen) {
			/* prepend zeroes to key1 */
			bcopy(key1->key, temp + (maxlen - key1len), key1len);
			key_1 = temp;
			key_2 = key2->key;
		} else {
			/* prepend zeroes to key2 */
			bcopy(key2->key, temp + (maxlen - key2len), key2len);
			key_1 = key1->key;
			key_2 = temp;
		}
	} else {
		maxlen = key1len;
		key_1 = key1->key;
		key_2 = key2->key;
	}

	for (i = 0; i < maxlen; i++) {
		if (*key_1 > *key_2)
			return (1);
		else if (*key_1 < *key_2)
			return (-1);
		key_1++;
		key_2++;
	}

	/* keys are equal value, so check lengths */
	if (key1len == key2len)
		return (0);
	else if (key1len < key2len)
		return (-1);
	else
		return (1);
}

/*
 * generate the concatenated keying material based on the two keys and the
 * shared key (if available). draft-ietf-tsvwg-auth specifies the specific
 * order for concatenation
 */
sctp_key_t *
sctp_compute_hashkey(sctp_key_t * key1, sctp_key_t * key2, sctp_key_t * shared)
{
	uint32_t keylen;
	sctp_key_t *new_key;
	uint8_t *key_ptr;

	keylen = sctp_get_keylen(key1) + sctp_get_keylen(key2) +
	    sctp_get_keylen(shared);

	if (keylen > 0) {
		/* get space for the new key */
		new_key = sctp_alloc_key(keylen);
		if (new_key == NULL) {
			/* out of memory */
			return (NULL);
		}
		new_key->keylen = keylen;
		key_ptr = new_key->key;
	} else {
		/* all keys empty/null?! */
		return (NULL);
	}

	/* concatenate the keys */
	if (sctp_compare_key(key1, key2) <= 0) {
		/* key is key1 + shared + key2 */
		if (sctp_get_keylen(key1)) {
			bcopy(key1->key, key_ptr, key1->keylen);
			key_ptr += key1->keylen;
		}
		if (sctp_get_keylen(shared)) {
			bcopy(shared->key, key_ptr, shared->keylen);
			key_ptr += shared->keylen;
		}
		if (sctp_get_keylen(key2)) {
			bcopy(key2->key, key_ptr, key2->keylen);
			key_ptr += key2->keylen;
		}
	} else {
		/* key is key2 + shared + key1 */
		if (sctp_get_keylen(key2)) {
			bcopy(key2->key, key_ptr, key2->keylen);
			key_ptr += key2->keylen;
		}
		if (sctp_get_keylen(shared)) {
			bcopy(shared->key, key_ptr, shared->keylen);
			key_ptr += shared->keylen;
		}
		if (sctp_get_keylen(key1)) {
			bcopy(key1->key, key_ptr, key1->keylen);
			key_ptr += key1->keylen;
		}
	}
	return (new_key);
}


sctp_sharedkey_t *
sctp_alloc_sharedkey(void)
{
	sctp_sharedkey_t *new_key;

	SCTP_MALLOC_NAMED(new_key, sctp_sharedkey_t *, sizeof(*new_key),
	    "AUTH skey");
	if (new_key == NULL) {
		/* out of memory */
		return (NULL);
	}
	new_key->keyid = 0;
	new_key->key = NULL;
	return (new_key);
}

void
sctp_free_sharedkey(sctp_sharedkey_t * skey)
{
	if (skey != NULL) {
		if (skey->key != NULL)
			sctp_free_key(skey->key);
		SCTP_FREE(skey);
	}
}

sctp_sharedkey_t *
sctp_find_sharedkey(struct sctp_keyhead *shared_keys, uint16_t key_id)
{
	sctp_sharedkey_t *skey;

	LIST_FOREACH(skey, shared_keys, next) {
		if (skey->keyid == key_id)
			return (skey);
	}
	return (NULL);
}

void
sctp_insert_sharedkey(struct sctp_keyhead *shared_keys,
    sctp_sharedkey_t * new_skey)
{
	sctp_sharedkey_t *skey;

	if ((shared_keys == NULL) || (new_skey == NULL))
		return;

	/* insert into an empty list? */
	if (LIST_EMPTY(shared_keys)) {
		LIST_INSERT_HEAD(shared_keys, new_skey, next);
		return;
	}
	/* insert into the existing list, ordered by key id */
	LIST_FOREACH(skey, shared_keys, next) {
		if (new_skey->keyid < skey->keyid) {
			/* insert it before here */
			LIST_INSERT_BEFORE(skey, new_skey, next);
			return;
		} else if (new_skey->keyid == skey->keyid) {
			/* replace the existing key */
#ifdef SCTP_DEBUG
			if (SCTP_AUTH_DEBUG)
				printf("replacing shared key id %u\n", new_skey->keyid);
#endif
			LIST_INSERT_BEFORE(skey, new_skey, next);
			LIST_REMOVE(skey, next);
			sctp_free_sharedkey(skey);
			return;
		}
		if (LIST_NEXT(skey, next) == NULL) {
			/* belongs at the end of the list */
			LIST_INSERT_AFTER(skey, new_skey, next);
			return;
		}
	}
}

static sctp_sharedkey_t *
sctp_copy_sharedkey(const sctp_sharedkey_t * skey)
{
	sctp_sharedkey_t *new_skey;

	if (skey == NULL)
		return (NULL);
	new_skey = sctp_alloc_sharedkey();
	if (new_skey == NULL)
		return (NULL);
	if (skey->key != NULL)
		new_skey->key = sctp_set_key(skey->key->key, skey->key->keylen);
	else
		new_skey->key = NULL;
	new_skey->keyid = skey->keyid;
	return (new_skey);
}

int
sctp_copy_skeylist(const struct sctp_keyhead *src, struct sctp_keyhead *dest)
{
	sctp_sharedkey_t *skey, *new_skey;
	int count = 0;

	if ((src == NULL) || (dest == NULL))
		return (0);
	LIST_FOREACH(skey, src, next) {
		new_skey = sctp_copy_sharedkey(skey);
		if (new_skey != NULL) {
			sctp_insert_sharedkey(dest, new_skey);
			count++;
		}
	}
	return (count);
}


sctp_hmaclist_t *
sctp_alloc_hmaclist(uint8_t num_hmacs)
{
	sctp_hmaclist_t *new_list;
	int alloc_size;

	alloc_size = sizeof(*new_list) + num_hmacs * sizeof(new_list->hmac[0]);
	SCTP_MALLOC_NAMED(new_list, sctp_hmaclist_t *, alloc_size,
	    "AUTH HMAC list");
	if (new_list == NULL) {
		/* out of memory */
		return (NULL);
	}
	new_list->max_algo = num_hmacs;
	new_list->num_algo = 0;
	return (new_list);
}

void
sctp_free_hmaclist(sctp_hmaclist_t * list)
{
	if (list != NULL) {
		SCTP_FREE(list);
		list = NULL;
	}
}

int
sctp_auth_add_hmacid(sctp_hmaclist_t * list, uint16_t hmac_id)
{
	if (list == NULL)
		return (-1);
	if (list->num_algo == list->max_algo) {
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP: HMAC id list full, ignoring add %u\n", hmac_id);
#endif
		return (-1);
	}
	if ((hmac_id != SCTP_AUTH_HMAC_ID_SHA1) &&
	    (hmac_id != SCTP_AUTH_HMAC_ID_MD5)) {
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("\nSCTP: cannot add unsupported HMAC id %u", hmac_id);
#endif
		return (-1);
    }
#ifdef SCTP_DEBUG
	if (SCTP_AUTH_DEBUG)
		printf("SCTP: add HMAC id %u to list\n", hmac_id);
#endif
	list->hmac[list->num_algo++] = hmac_id;
	return (0);
}

sctp_hmaclist_t *
sctp_copy_hmaclist(sctp_hmaclist_t * list)
{
	sctp_hmaclist_t *new_list;
	int i;

	if (list == NULL)
		return (NULL);
	/* get a new list */
	new_list = sctp_alloc_hmaclist(list->max_algo);
	if (new_list == NULL)
		return (NULL);
	/* copy it */
	new_list->max_algo = list->max_algo;
	new_list->num_algo = list->num_algo;
	for (i = 0; i < list->num_algo; i++)
		new_list->hmac[i] = list->hmac[i];
	return (new_list);
}

sctp_hmaclist_t *
sctp_default_supported_hmaclist(void)
{
	sctp_hmaclist_t *new_list;

	new_list = sctp_alloc_hmaclist(2);
	if (new_list == NULL)
		return (NULL);
	sctp_auth_add_hmacid(new_list, SCTP_AUTH_HMAC_ID_SHA1);
	sctp_auth_add_hmacid(new_list, SCTP_AUTH_HMAC_ID_MD5);
	return (new_list);
}

/*
 * HMAC algos are listed in priority/preference order find the best HMAC id
 * to use for the peer based on local support
 */
uint16_t
sctp_negotiate_hmacid(sctp_hmaclist_t * peer, sctp_hmaclist_t * local)
{
	int i, j;

	if ((local == NULL) || (peer == NULL))
		return (SCTP_AUTH_HMAC_ID_RSVD);

	for (i = 0; i < peer->num_algo; i++) {
		for (j = 0; j < local->num_algo; j++) {
			if (peer->hmac[i] == local->hmac[j]) {
				/* found the "best" one */
#ifdef SCTP_DEBUG
				if (SCTP_AUTH_DEBUG)
					printf("SCTP: negotiated peer HMAC id %u\n", peer->hmac[i]);
#endif
				return (peer->hmac[i]);
			}
		}
	}
	/* didn't find one! */
	return (SCTP_AUTH_HMAC_ID_RSVD);
}

/*
 * serialize the HMAC algo list and return space used caller must guarantee
 * ptr has appropriate space
 */
int
sctp_serialize_hmaclist(sctp_hmaclist_t * list, uint8_t * ptr)
{
	int i;
	uint16_t hmac_id;

	if (list == NULL)
		return (0);

	for (i = 0; i < list->num_algo; i++) {
		hmac_id = htons(list->hmac[i]);
		bcopy(&hmac_id, ptr, sizeof(hmac_id));
		ptr += sizeof(hmac_id);
	}
	return (list->num_algo * sizeof(hmac_id));
}

int
sctp_verify_hmac_param (struct sctp_auth_hmac_algo *hmacs, uint32_t num_hmacs)
{ 
	uint32_t i;
	uint16_t hmac_id;
	uint32_t sha1_supported = 0;

	for (i = 0; i < num_hmacs; i++) {
		hmac_id = ntohs(hmacs->hmac_ids[i]);
		if (hmac_id == SCTP_AUTH_HMAC_ID_SHA1)
	 		sha1_supported = 1;
	}
	/* all HMAC id's are supported */
	if (sha1_supported == 0)
		return (-1);
	else
		return (0);
}

sctp_authinfo_t *
sctp_alloc_authinfo(void)
{
	sctp_authinfo_t *new_authinfo;

	SCTP_MALLOC_NAMED(new_authinfo, sctp_authinfo_t *, sizeof(*new_authinfo),
	    "AUTH info");
	if (new_authinfo == NULL) {
		/* out of memory */
		return (NULL);
	}
	bzero(&new_authinfo, sizeof(*new_authinfo));
	return (new_authinfo);
}

void
sctp_free_authinfo(sctp_authinfo_t * authinfo)
{
	if (authinfo == NULL)
		return;

	if (authinfo->random != NULL)
		sctp_free_key(authinfo->random);
	if (authinfo->peer_random != NULL)
		sctp_free_key(authinfo->peer_random);
	if (authinfo->assoc_key != NULL)
		sctp_free_key(authinfo->assoc_key);
	if (authinfo->recv_key != NULL)
		sctp_free_key(authinfo->recv_key);

	/* We are NOT dynamically allocating authinfo's right now... */
	/* SCTP_FREE(authinfo); */
}


inline uint32_t
sctp_get_auth_chunk_len(uint16_t hmac_algo)
{
	int size;

	size = sizeof(struct sctp_auth_chunk) + sctp_get_hmac_digest_len(hmac_algo);
	return (SCTP_SIZE32(size));
}

uint32_t
sctp_get_hmac_digest_len(uint16_t hmac_algo)
{
	switch (hmac_algo) {
	case SCTP_AUTH_HMAC_ID_SHA1:
		return (SCTP_AUTH_DIGEST_LEN_SHA1);
	case SCTP_AUTH_HMAC_ID_MD5:
		return (SCTP_AUTH_DIGEST_LEN_MD5);
#ifdef HAVE_SHA2
	case SCTP_AUTH_HMAC_ID_SHA224:
		return (SCTP_AUTH_DIGEST_LEN_SHA224);
	case SCTP_AUTH_HMAC_ID_SHA256:
		return (SCTP_AUTH_DIGEST_LEN_SHA256);
	case SCTP_AUTH_HMAC_ID_SHA384:
		return (SCTP_AUTH_DIGEST_LEN_SHA384);
	case SCTP_AUTH_HMAC_ID_SHA512:
		return (SCTP_AUTH_DIGEST_LEN_SHA512);
#endif				/* HAVE_SHA2 */
	default:
		/* unknown HMAC algorithm: can't do anything */
		return (0);
	}			/* end switch */
}

static inline int
sctp_get_hmac_block_len(uint16_t hmac_algo)
{
	switch (hmac_algo) {
		case SCTP_AUTH_HMAC_ID_SHA1:
		case SCTP_AUTH_HMAC_ID_MD5:
#ifdef HAVE_SHA2
		case SCTP_AUTH_HMAC_ID_SHA224:
		case SCTP_AUTH_HMAC_ID_SHA256:
#endif				/* HAVE_SHA2 */
		return (64);
#ifdef HAVE_SHA2
	case SCTP_AUTH_HMAC_ID_SHA384:
	case SCTP_AUTH_HMAC_ID_SHA512:
		return (128);
#endif				/* HAVE_SHA2 */
	case SCTP_AUTH_HMAC_ID_RSVD:
	default:
		/* unknown HMAC algorithm: can't do anything */
		return (0);
	}			/* end switch */
}

static void
sctp_hmac_init(uint16_t hmac_algo, sctp_hash_context_t * ctx)
{
	switch (hmac_algo) {
		case SCTP_AUTH_HMAC_ID_SHA1:
		SHA1_Init(&ctx->sha1);
		break;
	case SCTP_AUTH_HMAC_ID_MD5:
		MD5_Init(&ctx->md5);
		break;
	case SCTP_AUTH_HMAC_ID_SHA224:
		break;
	case SCTP_AUTH_HMAC_ID_SHA256:
		break;
	case SCTP_AUTH_HMAC_ID_SHA384:
		break;
	case SCTP_AUTH_HMAC_ID_SHA512:
		break;
	case SCTP_AUTH_HMAC_ID_RSVD:
	default:
		/* unknown HMAC algorithm: can't do anything */
		return;
	}			/* end switch */
}

static void
sctp_hmac_update(uint16_t hmac_algo, sctp_hash_context_t * ctx,
    const uint8_t * text, uint32_t textlen)
{
	switch (hmac_algo) {
		case SCTP_AUTH_HMAC_ID_SHA1:
		SHA1_Update(&ctx->sha1, text, textlen);
		break;
	case SCTP_AUTH_HMAC_ID_MD5:
		MD5_Update(&ctx->md5, text, textlen);
		break;
	case SCTP_AUTH_HMAC_ID_SHA224:
		break;
	case SCTP_AUTH_HMAC_ID_SHA256:
		break;
	case SCTP_AUTH_HMAC_ID_SHA384:
		break;
	case SCTP_AUTH_HMAC_ID_SHA512:
		break;
	case SCTP_AUTH_HMAC_ID_RSVD:
	default:
		/* unknown HMAC algorithm: can't do anything */
		return;
	}			/* end switch */
}

static void
sctp_hmac_final(uint16_t hmac_algo, sctp_hash_context_t * ctx,
    uint8_t * digest)
{
	switch (hmac_algo) {
		case SCTP_AUTH_HMAC_ID_SHA1:
		SHA1_Final(digest, &ctx->sha1);
		break;
	case SCTP_AUTH_HMAC_ID_MD5:
		MD5_Final(digest, &ctx->md5);
		break;
	case SCTP_AUTH_HMAC_ID_SHA224:
		break;
	case SCTP_AUTH_HMAC_ID_SHA256:
		break;
	case SCTP_AUTH_HMAC_ID_SHA384:
		/* SHA384 is truncated SHA512 */
		break;
	case SCTP_AUTH_HMAC_ID_SHA512:
		break;
	case SCTP_AUTH_HMAC_ID_RSVD:
	default:
		/* unknown HMAC algorithm: can't do anything */
		return;
	}			/* end switch */
}

/*
 * Keyed-Hashing for Message Authentication: FIPS 198 (RFC 2104)
 *
 * Compute the HMAC digest using the desired hash key, text, and HMAC algorithm.
 * Resulting digest is placed in 'digest' and digest length is returned, if
 * the HMAC was performed.
 *
 * WARNING: it is up to the caller to supply sufficient space to hold the
 * resultant digest.
 */
uint32_t
sctp_hmac(uint16_t hmac_algo, uint8_t * key, uint32_t keylen,
    const uint8_t * text, uint32_t textlen, uint8_t * digest)
{
	uint32_t digestlen;
	uint32_t blocklen;
	sctp_hash_context_t ctx;
	uint8_t ipad[128], opad[128];	/* keyed hash inner/outer pads */
	uint8_t temp[SCTP_AUTH_DIGEST_LEN_MAX];
	uint32_t i;

	/* sanity check the material and length */
	if ((key == NULL) || (keylen == 0) || (text == NULL) || (textlen == 0) ||
	    (digest == NULL)) {
		/* can't do HMAC with empty key or text or digest store */
		return (0);
	}
	/* validate the hmac algo and get the digest length */
	digestlen = sctp_get_hmac_digest_len(hmac_algo);
	if (digestlen == 0)
		return (0);

	/* hash the key if it is longer than the hash block size */
	blocklen = sctp_get_hmac_block_len(hmac_algo);
	if (keylen > blocklen) {
		sctp_hmac_init(hmac_algo, &ctx);
		sctp_hmac_update(hmac_algo, &ctx, key, keylen);
		sctp_hmac_final(hmac_algo, &ctx, temp);
		/* set the hashed key as the key */
		keylen = digestlen;
		key = temp;
	}
	/* initialize the inner/outer pads with the key and "append" zeroes */
	bzero(ipad, blocklen);
	bzero(opad, blocklen);
	bcopy(key, ipad, keylen);
	bcopy(key, opad, keylen);

	/* XOR the key with ipad and opad values */
	for (i = 0; i < blocklen; i++) {
		ipad[i] ^= 0x36;
		opad[i] ^= 0x5c;
	}

	/* perform inner hash */
	sctp_hmac_init(hmac_algo, &ctx);
	sctp_hmac_update(hmac_algo, &ctx, ipad, blocklen);
	sctp_hmac_update(hmac_algo, &ctx, text, textlen);
	sctp_hmac_final(hmac_algo, &ctx, temp);

	/* perform outer hash */
	sctp_hmac_init(hmac_algo, &ctx);
	sctp_hmac_update(hmac_algo, &ctx, opad, blocklen);
	sctp_hmac_update(hmac_algo, &ctx, temp, digestlen);
	sctp_hmac_final(hmac_algo, &ctx, digest);

	return (digestlen);
}

/* mbuf version */
uint32_t
sctp_hmac_m(uint16_t hmac_algo, uint8_t * key, uint32_t keylen,
    struct mbuf *m, uint32_t m_offset, uint8_t * digest)
{
	uint32_t digestlen;
	uint32_t blocklen;
	sctp_hash_context_t ctx;
	uint8_t ipad[128], opad[128];	/* keyed hash inner/outer pads */
	uint8_t temp[SCTP_AUTH_DIGEST_LEN_MAX];
	uint32_t i;
	struct mbuf *m_tmp;

	/* sanity check the material and length */
	if ((key == NULL) || (keylen == 0) || (m == NULL) || (digest == NULL)) {
		/* can't do HMAC with empty key or text or digest store */
		return (0);
	}
	/* validate the hmac algo and get the digest length */
	digestlen = sctp_get_hmac_digest_len(hmac_algo);
	if (digestlen == 0)
		return (0);

	/* hash the key if it is longer than the hash block size */
	blocklen = sctp_get_hmac_block_len(hmac_algo);
	if (keylen > blocklen) {
		sctp_hmac_init(hmac_algo, &ctx);
		sctp_hmac_update(hmac_algo, &ctx, key, keylen);
		sctp_hmac_final(hmac_algo, &ctx, temp);
		/* set the hashed key as the key */
		keylen = digestlen;
		key = temp;
	}
	/* initialize the inner/outer pads with the key and "append" zeroes */
	bzero(ipad, blocklen);
	bzero(opad, blocklen);
	bcopy(key, ipad, keylen);
	bcopy(key, opad, keylen);

	/* XOR the key with ipad and opad values */
	for (i = 0; i < blocklen; i++) {
		ipad[i] ^= 0x36;
		opad[i] ^= 0x5c;
	}

	/* perform inner hash */
	sctp_hmac_init(hmac_algo, &ctx);
	sctp_hmac_update(hmac_algo, &ctx, ipad, blocklen);
	/* find the correct starting mbuf and offset (get start of text) */
	m_tmp = m;
	while ((m_tmp != NULL) && (m_offset >= (uint32_t) m_tmp->m_len)) {
		m_offset -= m_tmp->m_len;
		m_tmp = m_tmp->m_next;
	}
	/* now use the rest of the mbuf chain for the text */
	while (m_tmp != NULL) {
		sctp_hmac_update(hmac_algo, &ctx, mtod(m_tmp, uint8_t *) + m_offset,
		    m_tmp->m_len - m_offset);
		/* clear the offset since it's only for the first mbuf */
		m_offset = 0;
		m_tmp = m_tmp->m_next;
	}
	sctp_hmac_final(hmac_algo, &ctx, temp);

	/* perform outer hash */
	sctp_hmac_init(hmac_algo, &ctx);
	sctp_hmac_update(hmac_algo, &ctx, opad, blocklen);
	sctp_hmac_update(hmac_algo, &ctx, temp, digestlen);
	sctp_hmac_final(hmac_algo, &ctx, digest);

	return (digestlen);
}

/*
 * verify the HMAC digest using the desired hash key, text, and HMAC
 * algorithm. Returns -1 on error, 0 on success.
 */
int
sctp_verify_hmac(uint16_t hmac_algo, uint8_t * key, uint32_t keylen,
    const uint8_t * text, uint32_t textlen,
    uint8_t * digest, uint32_t digestlen)
{
	uint32_t len;
	uint8_t temp[SCTP_AUTH_DIGEST_LEN_MAX];

	/* sanity check the material and length */
	if ((key == NULL) || (keylen == 0) ||
	    (text == NULL) || (textlen == 0) || (digest == NULL)) {
		/* can't do HMAC with empty key or text or digest */
		return (-1);
	}
	len = sctp_get_hmac_digest_len(hmac_algo);
	if ((len == 0) || (digestlen != len))
		return (-1);

	/* compute the expected hash */
	if (sctp_hmac(hmac_algo, key, keylen, text, textlen, temp) != len)
		return (-1);

	if (memcmp(digest, temp, digestlen) != 0)
		return (-1);
	else
		return (0);
}


/*
 * computes the requested HMAC using a key struct (which may be modified if
 * the keylen exceeds the HMAC block len).
 */
uint32_t
sctp_compute_hmac(uint16_t hmac_algo, sctp_key_t * key, const uint8_t * text,
    uint32_t textlen, uint8_t * digest)
{
	uint32_t digestlen;
	uint32_t blocklen;
	sctp_hash_context_t ctx;
	uint8_t temp[SCTP_AUTH_DIGEST_LEN_MAX];

	/* sanity check */
	if ((key == NULL) || (text == NULL) || (textlen == 0) ||
	    (digest == NULL)) {
		/* can't do HMAC with empty key or text or digest store */
		return (0);
	}
	/* validate the hmac algo and get the digest length */
	digestlen = sctp_get_hmac_digest_len(hmac_algo);
	if (digestlen == 0)
		return (0);

	/* hash the key if it is longer than the hash block size */
	blocklen = sctp_get_hmac_block_len(hmac_algo);
	if (key->keylen > blocklen) {
		sctp_hmac_init(hmac_algo, &ctx);
		sctp_hmac_update(hmac_algo, &ctx, key->key, key->keylen);
		sctp_hmac_final(hmac_algo, &ctx, temp);
		/* save the hashed key as the new key */
		key->keylen = digestlen;
		bcopy(temp, key->key, key->keylen);
	}
	return (sctp_hmac(hmac_algo, key->key, key->keylen, text, textlen,
	    digest));
}

/* mbuf version */
uint32_t
sctp_compute_hmac_m(uint16_t hmac_algo, sctp_key_t * key, struct mbuf *m,
    uint32_t m_offset, uint8_t * digest)
{
	uint32_t digestlen;
	uint32_t blocklen;
	sctp_hash_context_t ctx;
	uint8_t temp[SCTP_AUTH_DIGEST_LEN_MAX];

	/* sanity check */
	if ((key == NULL) || (m == NULL) || (digest == NULL)) {
		/* can't do HMAC with empty key or text or digest store */
		return (0);
	}
	/* validate the hmac algo and get the digest length */
	digestlen = sctp_get_hmac_digest_len(hmac_algo);
	if (digestlen == 0)
		return (0);

	/* hash the key if it is longer than the hash block size */
	blocklen = sctp_get_hmac_block_len(hmac_algo);
	if (key->keylen > blocklen) {
		sctp_hmac_init(hmac_algo, &ctx);
		sctp_hmac_update(hmac_algo, &ctx, key->key, key->keylen);
		sctp_hmac_final(hmac_algo, &ctx, temp);
		/* save the hashed key as the new key */
		key->keylen = digestlen;
		bcopy(temp, key->key, key->keylen);
	}
	return (sctp_hmac_m(hmac_algo, key->key, key->keylen, m, m_offset, digest));
}

int
sctp_auth_is_supported_hmac(sctp_hmaclist_t * list, uint16_t id)
{
	int i;

	if ((list == NULL) || (id == SCTP_AUTH_HMAC_ID_RSVD))
		return (0);

	for (i = 0; i < list->num_algo; i++)
		if (list->hmac[i] == id)
			return (1);

	/* not in the list */
	return (0);
}


/*
 * clear any cached key(s) if they match the given key id on an association
 * the cached key(s) will be recomputed and re-cached at next use. ASSUMES
 * TCB_LOCK is already held
 */
void
sctp_clear_cachedkeys(struct sctp_tcb *stcb, uint16_t keyid)
{
	if (stcb == NULL)
		return;

	if (keyid == stcb->asoc.authinfo.assoc_keyid) {
		sctp_free_key(stcb->asoc.authinfo.assoc_key);
		stcb->asoc.authinfo.assoc_key = NULL;
	}
	if (keyid == stcb->asoc.authinfo.recv_keyid) {
		sctp_free_key(stcb->asoc.authinfo.recv_key);
		stcb->asoc.authinfo.recv_key = NULL;
	}
}

/*
 * clear any cached key(s) if they match the given key id for all assocs on
 * an association ASSUMES INP_WLOCK is already held
 */
void
sctp_clear_cachedkeys_ep(struct sctp_inpcb *inp, uint16_t keyid)
{
	struct sctp_tcb *stcb;

	if (inp == NULL)
		return;

	/* clear the cached keys on all assocs on this instance */
	LIST_FOREACH(stcb, &inp->sctp_asoc_list, sctp_tcblist) {
		SCTP_TCB_LOCK(stcb);
		sctp_clear_cachedkeys(stcb, keyid);
		SCTP_TCB_UNLOCK(stcb);
	}
}

/*
 * delete a shared key from an association ASSUMES TCB_LOCK is already held
 */
int
sctp_delete_sharedkey(struct sctp_tcb *stcb, uint16_t keyid)
{
	sctp_sharedkey_t *skey;

	if (stcb == NULL)
		return (-1);

	/* is the keyid the assoc active sending key */
	if (keyid == stcb->asoc.authinfo.assoc_keyid)
		return (-1);

	/* does the key exist? */
	skey = sctp_find_sharedkey(&stcb->asoc.shared_keys, keyid);
	if (skey == NULL)
		return (-1);

	/* remove it */
	LIST_REMOVE(skey, next);
	sctp_free_sharedkey(skey);	/* frees skey->key as well */

	/* clear any cached keys */
	sctp_clear_cachedkeys(stcb, keyid);
	return (0);
}

/*
 * deletes a shared key from the endpoint ASSUMES INP_WLOCK is already held
 */
int
sctp_delete_sharedkey_ep(struct sctp_inpcb *inp, uint16_t keyid)
{
	sctp_sharedkey_t *skey;
	struct sctp_tcb *stcb;

	if (inp == NULL)
		return (-1);

	/* is the keyid the active sending key on the endpoint or any assoc */
	if (keyid == inp->sctp_ep.default_keyid)
		return (-1);
	LIST_FOREACH(stcb, &inp->sctp_asoc_list, sctp_tcblist) {
		SCTP_TCB_LOCK(stcb);
		if (keyid == stcb->asoc.authinfo.assoc_keyid) {
			SCTP_TCB_UNLOCK(stcb);
			return (-1);
		}
		SCTP_TCB_UNLOCK(stcb);
	}

	/* does the key exist? */
	skey = sctp_find_sharedkey(&inp->sctp_ep.shared_keys, keyid);
	if (skey == NULL)
		return (-1);

	/* remove it */
	LIST_REMOVE(skey, next);
	sctp_free_sharedkey(skey);	/* frees skey->key as well */

	/* clear any cached keys */
	sctp_clear_cachedkeys_ep(inp, keyid);
	return (0);
}

/*
 * set the active key on an association ASSUME TCB_LOCK is already held
 */
int
sctp_auth_setactivekey(struct sctp_tcb *stcb, uint16_t keyid)
{
	sctp_sharedkey_t *skey = NULL;
	sctp_key_t *key = NULL;
	int using_ep_key = 0;

	/* find the key on the assoc */
	skey = sctp_find_sharedkey(&stcb->asoc.shared_keys, keyid);
	if (skey == NULL) {
		/* if not on the assoc, find the key on the endpoint */
		SCTP_INP_RLOCK(stcb->sctp_ep);
		skey = sctp_find_sharedkey(&stcb->sctp_ep->sctp_ep.shared_keys,
		    keyid);
		using_ep_key = 1;
	}
	if (skey == NULL) {
		/* that key doesn't exist */
		if (using_ep_key)
			SCTP_INP_RUNLOCK(stcb->sctp_ep);
		return (-1);
	}
	/* get the shared key text */
	key = skey->key;

	/* free any existing cached key */
	if (stcb->asoc.authinfo.assoc_key != NULL)
		sctp_free_key(stcb->asoc.authinfo.assoc_key);
	/* compute a new assoc key and cache it */
	stcb->asoc.authinfo.assoc_key =
	    sctp_compute_hashkey(stcb->asoc.authinfo.random,
	    stcb->asoc.authinfo.peer_random, key);
	stcb->asoc.authinfo.assoc_keyid = keyid;
#ifdef SCTP_DEBUG
	if (SCTP_AUTH_DEBUG)
		sctp_print_key(stcb->asoc.authinfo.assoc_key, "Assoc Key");
#endif

	if (using_ep_key)
		SCTP_INP_RUNLOCK(stcb->sctp_ep);
	return (0);
}

/*
 * set the active key on an endpoint ASSUMES INP_WLOCK is already held
 */
int
sctp_auth_setactivekey_ep(struct sctp_inpcb *inp, uint16_t keyid)
{
	sctp_sharedkey_t *skey;

	/* find the key */
	skey = sctp_find_sharedkey(&inp->sctp_ep.shared_keys, keyid);
	if (skey == NULL) {
		/* that key doesn't exist */
		return (-1);
	}
	inp->sctp_ep.default_keyid = keyid;
	return (0);
}

/*
 * get local authentication parameters from cookie (from INIT-ACK)
 */
void
sctp_auth_get_cookie_params(struct sctp_tcb *stcb, struct mbuf *m,
    uint32_t offset, uint32_t length)
{
	struct sctp_paramhdr *phdr, tmp_param;
	uint16_t plen, ptype;

	/* convert to upper bound */
	length += offset;

	phdr = (struct sctp_paramhdr *)sctp_m_getptr(m, offset,
	    sizeof(struct sctp_paramhdr),
	    (uint8_t *) & tmp_param);
	while (phdr != NULL) {
		ptype = ntohs(phdr->param_type);
		plen = ntohs(phdr->param_length);

		if ((plen == 0) || (offset + plen > length))
			break;

		if (ptype == SCTP_RANDOM) {
			uint8_t store[256];
			struct sctp_auth_random *random;
			uint32_t keylen;

			if (plen > sizeof(store))
				break;
			phdr = sctp_get_next_param(m, offset,
			    (struct sctp_paramhdr *)store, plen);
			if (phdr == NULL)
				return;
			random = (struct sctp_auth_random *)phdr;
			keylen = plen - sizeof(*random);
			if (stcb->asoc.authinfo.random != NULL)
				sctp_free_key(stcb->asoc.authinfo.random);
			stcb->asoc.authinfo.random =
			    sctp_set_key(random->random_data, keylen);
			sctp_clear_cachedkeys(stcb, stcb->asoc.authinfo.assoc_keyid);
			sctp_clear_cachedkeys(stcb, stcb->asoc.authinfo.recv_keyid);
		} else if (ptype == SCTP_HMAC_LIST) {
			uint8_t store[256];
			struct sctp_auth_hmac_algo *hmacs;
			int num_hmacs, i;

			if (plen > sizeof(store))
				break;
			phdr = sctp_get_next_param(m, offset,
			    (struct sctp_paramhdr *)store, plen);
			if (phdr == NULL)
				return;
			hmacs = (struct sctp_auth_hmac_algo *)phdr;
			num_hmacs = (plen - sizeof(*hmacs)) / sizeof(hmacs->hmac_ids[0]);
			if (stcb->asoc.local_hmacs != NULL)
				sctp_free_hmaclist(stcb->asoc.local_hmacs);
			stcb->asoc.local_hmacs = sctp_alloc_hmaclist(num_hmacs);
			if (stcb->asoc.local_hmacs != NULL) {
				for (i = 0; i < num_hmacs; i++) {
					sctp_auth_add_hmacid(stcb->asoc.local_hmacs,
					    ntohs(hmacs->hmac_ids[i]));
				}
			}
		} else if (ptype == SCTP_CHUNK_LIST) {
			uint8_t store[384];
			struct sctp_auth_chunk_list *chunks;
			int size, i;

			if (plen > sizeof(store))
				break;
			phdr = sctp_get_next_param(m, offset,
			    (struct sctp_paramhdr *)store, plen);
			if (phdr == NULL)
				return;
			chunks = (struct sctp_auth_chunk_list *)phdr;
			size = plen - sizeof(*chunks);
			if (stcb->asoc.local_auth_chunks != NULL)
				sctp_clear_chunklist(stcb->asoc.local_auth_chunks);
			else
				stcb->asoc.local_auth_chunks = sctp_alloc_chunklist();
			for (i = 0; i < size; i++) {
				sctp_auth_add_chunk(chunks->chunk_types[i],
				    stcb->asoc.local_auth_chunks);
			}
		}
		/* get next parameter */
		offset += SCTP_SIZE32(plen);
		if (offset + sizeof(struct sctp_paramhdr) > length)
			break;
		phdr = (struct sctp_paramhdr *)sctp_m_getptr(m, offset, sizeof(struct sctp_paramhdr),
		    (uint8_t *) & tmp_param);
	}			/* while */

	/* negotiate what HMAC to use for the peer */
	stcb->asoc.peer_hmac_id = sctp_negotiate_hmacid(stcb->asoc.peer_hmacs,
	    stcb->asoc.local_hmacs);
	/* copy defaults from the endpoint */
	/* FIX ME: put in cookie? */
	stcb->asoc.authinfo.assoc_keyid = stcb->sctp_ep->sctp_ep.default_keyid;
}

/*
 * compute and fill in the HMAC digest for a packet
 */
void
sctp_fill_hmac_digest_m(struct mbuf *m, uint32_t auth_offset,
    struct sctp_auth_chunk *auth, struct sctp_tcb *stcb)
{
	uint32_t digestlen;
	sctp_sharedkey_t *skey;
	sctp_key_t *key;

	if ((stcb == NULL) || (auth == NULL))
		return;

	/* zero the digest + chunk padding */
	digestlen = sctp_get_hmac_digest_len(stcb->asoc.peer_hmac_id);
	bzero(auth->hmac, SCTP_SIZE32(digestlen));
	/* is an assoc key cached? */
	if (stcb->asoc.authinfo.assoc_key == NULL) {
		skey = sctp_find_sharedkey(&stcb->asoc.shared_keys,
		    stcb->asoc.authinfo.assoc_keyid);
		if (skey == NULL) {
			/* not in the assoc list, so check the endpoint list */
			skey = sctp_find_sharedkey(&stcb->sctp_ep->sctp_ep.shared_keys,
			    stcb->asoc.authinfo.assoc_keyid);
		}
		/* the only way skey is NULL is if null key id 0 is used */
		if (skey != NULL)
			key = skey->key;
		else
			key = NULL;
		/* compute a new assoc key and cache it */
		stcb->asoc.authinfo.assoc_key =
		    sctp_compute_hashkey(stcb->asoc.authinfo.random,
		    stcb->asoc.authinfo.peer_random, key);
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG) {
			printf("caching key id %u\n",
			    stcb->asoc.authinfo.assoc_keyid);
			sctp_print_key(stcb->asoc.authinfo.assoc_key, "Assoc Key");
		}
#endif
	}
	/* set in the active key id */
	auth->shared_key_id = htons(stcb->asoc.authinfo.assoc_keyid);

	/* compute and fill in the digest */
	(void)sctp_compute_hmac_m(stcb->asoc.peer_hmac_id,
	    stcb->asoc.authinfo.assoc_key,
	    m, auth_offset, auth->hmac);
}


static void
sctp_bzero_m(struct mbuf *m, uint32_t m_offset, uint32_t size)
{
	struct mbuf *m_tmp;
	uint8_t *data;

	/* sanity check */
	if (m == NULL)
		return;

	/* find the correct starting mbuf and offset (get start position) */
	m_tmp = m;
	while ((m_tmp != NULL) && (m_offset >= (uint32_t) m_tmp->m_len)) {
		m_offset -= m_tmp->m_len;
		m_tmp = m_tmp->m_next;
	}
	/* now use the rest of the mbuf chain */
	while ((m_tmp != NULL) && (size > 0)) {
		data = mtod(m_tmp, uint8_t *) + m_offset;
		if (size > (uint32_t) m_tmp->m_len) {
			bzero(data, m_tmp->m_len);
			size -= m_tmp->m_len;
		} else {
			bzero(data, size);
			size = 0;
		}
		/* clear the offset since it's only for the first mbuf */
		m_offset = 0;
		m_tmp = m_tmp->m_next;
	}
}

/*
 * process the incoming Authentication chunk return codes: -1 on any
 * authentication error 0 on authentication verification
 */
int
sctp_handle_auth(struct sctp_tcb *stcb, struct sctp_auth_chunk *auth,
    struct mbuf *m, uint32_t offset)
{
	uint16_t chunklen;
	uint16_t shared_key_id;
	uint16_t hmac_id;
	sctp_sharedkey_t *skey;
	uint32_t digestlen;
	uint8_t digest[SCTP_AUTH_DIGEST_LEN_MAX];
	uint8_t computed_digest[SCTP_AUTH_DIGEST_LEN_MAX];

	/* auth is checked for NULL by caller */
	chunklen = ntohs(auth->ch.chunk_length);
	if (chunklen < sizeof(*auth)) {
		SCTP_STAT_INCR(sctps_recvauthfailed);
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP AUTH Chunk: chunk too short\n");
#endif
		return (-1);
	}
	SCTP_STAT_INCR(sctps_recvauth);

	/* get the auth params */
	shared_key_id = ntohs(auth->shared_key_id);
	hmac_id = ntohs(auth->hmac_id);
#ifdef SCTP_DEBUG
	if (SCTP_AUTH_DEBUG)
		printf("SCTP AUTH Chunk: shared key %u, HMAC id %u\n",
		    shared_key_id, hmac_id);
#endif

	/* is the indicated HMAC supported? */
	if (!sctp_auth_is_supported_hmac(stcb->asoc.local_hmacs, hmac_id)) {
		struct mbuf *m_err;
		struct sctp_auth_invalid_hmac *err;

		SCTP_STAT_INCR(sctps_recvivalhmacid);
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP Auth: unsupported HMAC id %u\n", hmac_id);
#endif
		/*
		 * report this in an Error Chunk: Unsupported HMAC
		 * Identifier
		 */
		m_err = sctp_get_mbuf_for_msg(sizeof(*err), 1, M_DONTWAIT, 1, MT_HEADER);
		if (m_err != NULL) {
			/* pre-reserve some space */
			m_err->m_data += sizeof(struct sctp_chunkhdr);
			/* fill in the error */
			err = mtod(m_err, struct sctp_auth_invalid_hmac *);
			bzero(err, sizeof(*err));
			err->ph.param_type = htons(SCTP_CAUSE_UNSUPPORTED_HMACID);
			err->ph.param_length = htons(sizeof(*err));
			err->hmac_id = ntohs(hmac_id);
			m_err->m_pkthdr.len = m_err->m_len = sizeof(*err);
			/* queue it */
			sctp_queue_op_err(stcb, m_err);
		}
		return (-1);
	}
	/* get the indicated shared key, if available */
	if ((stcb->asoc.authinfo.recv_key == NULL) ||
	    (stcb->asoc.authinfo.recv_keyid != shared_key_id)) {
		/* find the shared key on the assoc first */
		skey = sctp_find_sharedkey(&stcb->asoc.shared_keys, shared_key_id);
		if (skey == NULL) {
			/* if not on the assoc, find it on the endpoint */
			skey = sctp_find_sharedkey(&stcb->sctp_ep->sctp_ep.shared_keys,
			    shared_key_id);
		}
		/* if the shared key isn't found, discard the chunk */
		if (skey == NULL) {
			SCTP_STAT_INCR(sctps_recvivalkeyid);
#ifdef SCTP_DEBUG
			if (SCTP_AUTH_DEBUG)
				printf("SCTP Auth: unknown key id %u\n", shared_key_id);
#endif
			return (-1);
		}
		/* generate a notification if this is a new key id */
		if (stcb->asoc.authinfo.recv_keyid != shared_key_id)
			/*
			 * sctp_ulp_notify(SCTP_NOTIFY_AUTH_NEW_KEY, stcb,
			 * shared_key_id, (void
			 * *)stcb->asoc.authinfo.recv_keyid);
			 */
			sctp_notify_authentication(stcb, SCTP_AUTH_NEWKEY, shared_key_id,
			    stcb->asoc.authinfo.recv_keyid);
		/* compute a new recv assoc key and cache it */
		if (stcb->asoc.authinfo.recv_key != NULL)
			sctp_free_key(stcb->asoc.authinfo.recv_key);
		stcb->asoc.authinfo.recv_key =
		    sctp_compute_hashkey(stcb->asoc.authinfo.random,
		    stcb->asoc.authinfo.peer_random, skey->key);
		stcb->asoc.authinfo.recv_keyid = shared_key_id;
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			sctp_print_key(stcb->asoc.authinfo.recv_key, "Recv Key");
#endif
	}
	/* validate the digest length */
	digestlen = sctp_get_hmac_digest_len(hmac_id);
	if (chunklen < (sizeof(*auth) + digestlen)) {
		/* invalid digest length */
		SCTP_STAT_INCR(sctps_recvauthfailed);
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP Auth: chunk too short for HMAC\n");
#endif
		return (-1);
	}
	/* save a copy of the digest, zero the pseudo header, and validate */
	bcopy(auth->hmac, digest, digestlen);
	sctp_bzero_m(m, offset + sizeof(*auth), SCTP_SIZE32(digestlen));
	(void)sctp_compute_hmac_m(hmac_id, stcb->asoc.authinfo.recv_key,
	    m, offset, computed_digest);

	/* compare the computed digest with the one in the AUTH chunk */
	if (memcmp(digest, computed_digest, digestlen) != 0) {
		SCTP_STAT_INCR(sctps_recvauthfailed);
#ifdef SCTP_DEBUG
		if (SCTP_AUTH_DEBUG)
			printf("SCTP Auth: HMAC digest check failed\n");
#endif
		return (-1);
	}
	/* everything looks good... */
#ifdef SCTP_DEBUG
	if (SCTP_AUTH_DEBUG)
		printf("SCTP Auth: Digest verified ok\n");
#endif
	return (0);
}

/*
 * Generate NOTIFICATION
 */
void
sctp_notify_authentication(struct sctp_tcb *stcb, uint32_t indication,
    uint16_t keyid, uint16_t alt_keyid)
{
	struct mbuf *m_notify;
	struct sctp_authkey_event *auth;
	struct sctp_queued_to_read *control;

	if (sctp_is_feature_off(stcb->sctp_ep, SCTP_PCB_FLAGS_AUTHEVNT))
		/* event not enabled */
		return;

	m_notify = sctp_get_mbuf_for_msg(sizeof(struct sctp_authkey_event), 
					  1, M_DONTWAIT, 1, MT_HEADER);
	if (m_notify == NULL)
		/* no space left */
		return;
	m_notify->m_len = 0;
	auth = mtod(m_notify, struct sctp_authkey_event *);
	auth->auth_type = SCTP_AUTHENTICATION_EVENT;
	auth->auth_flags = 0;
	auth->auth_length = sizeof(*auth);
	auth->auth_keynumber = keyid;
	auth->auth_altkeynumber = alt_keyid;
	auth->auth_indication = indication;
	auth->auth_assoc_id = sctp_get_associd(stcb);

	m_notify->m_flags |= M_EOR | M_NOTIFICATION;
	m_notify->m_pkthdr.len = sizeof(*auth);
	m_notify->m_pkthdr.rcvif = 0;
	m_notify->m_len = sizeof(*auth);
	m_notify->m_next = NULL;

	/* append to socket */
	control = sctp_build_readq_entry(stcb, stcb->asoc.primary_destination,
	    0, 0, 0, 0, 0, 0, m_notify);
	if (control == NULL) {
		/* no memory */
		sctp_m_freem(m_notify);
		return;
	}
	control->length = m_notify->m_len;
	/* not that we need this */
	control->tail_mbuf = m_notify;
	sctp_add_to_readq(stcb->sctp_ep, stcb, control,
	    &stcb->sctp_socket->so_rcv, 1);
}


/*
 * validates the AUTHentication related parameters in an INIT/INIT-ACK
 * Note: currently only used for INIT as INIT-ACK is handled inline
 * with sctp_load_addresses_from_init()
 */
int
sctp_validate_init_auth_params(struct mbuf *m, int offset, int limit)
{
	struct sctp_paramhdr *phdr, parm_buf;
	uint16_t ptype, plen;
	int peer_supports_asconf = 0;
	int peer_supports_auth = 0;
	int got_random = 0, got_hmacs = 0;

	/* go through each of the params. */
	phdr = sctp_get_next_param(m, offset, &parm_buf, sizeof(parm_buf));
	while (phdr) {
		ptype = ntohs(phdr->param_type);
		plen = ntohs(phdr->param_length);

		if (offset + plen > limit) {
			break;
		}
		if (plen == 0) {
			break;
		}
		if (ptype == SCTP_SUPPORTED_CHUNK_EXT) {
			/* A supported extension chunk */
			struct sctp_supported_chunk_types_param *pr_supported;
			uint8_t local_store[128];
			int num_ent, i;

			phdr = sctp_get_next_param(m, offset,
			    (struct sctp_paramhdr *)&local_store, plen);
			if (phdr == NULL) {
				return (-1);
			}
			pr_supported = (struct sctp_supported_chunk_types_param *)phdr;
			num_ent = plen - sizeof(struct sctp_paramhdr);
			for (i = 0; i < num_ent; i++) {
				switch (pr_supported->chunk_types[i]) {
				case SCTP_ASCONF:
				case SCTP_ASCONF_ACK:
					peer_supports_asconf = 1;
					break;
				case SCTP_AUTHENTICATION:
					peer_supports_auth = 1;
					break;
				default:
					/* one we don't care about */
					break;
				}
			}
		} else if (ptype == SCTP_RANDOM) {
			got_random = 1;
		} else if (ptype == SCTP_HMAC_LIST) {
			uint8_t store[256];
			struct sctp_auth_hmac_algo *hmacs;
			int num_hmacs;

			if (plen > sizeof(store))
				break;
			phdr = sctp_get_next_param(m, offset,
			    (struct sctp_paramhdr *)store, plen);
			if (phdr == NULL)
				return (-1);
			hmacs = (struct sctp_auth_hmac_algo *)phdr;
			num_hmacs = (plen - sizeof(*hmacs)) /
			    sizeof(hmacs->hmac_ids[0]);
			/* validate the hmac list */
			if (sctp_verify_hmac_param(hmacs, num_hmacs)) {
#ifdef SCTP_DEBUG
				if (sctp_debug_on & SCTP_DEBUG_AUTH1)
					printf("SCTP: invalid HMAC param\n");
#endif
				return (-1);
			}
			got_hmacs = 1;
		}

		offset += SCTP_SIZE32(plen);
		if (offset >= limit) {
			break;
		}
		phdr = sctp_get_next_param(m, offset, &parm_buf,
		    sizeof(parm_buf));
	}
	/* validate authentication required parameters */
	if (got_random && got_hmacs) {
		peer_supports_auth = 1;
	} else {
		peer_supports_auth = 0;
	}
	if (!sctp_asconf_auth_nochk && peer_supports_asconf &&
	    !peer_supports_auth) {
#ifdef SCTP_DEBUG
		if (sctp_debug_on & SCTP_DEBUG_AUTH1)
			printf("SCTP: peer supports ASCONF but not AUTH\n");
#endif
		return (-1);
	}
	return (0);
}

#ifdef SCTP_HMAC_TEST
/*
 * HMAC and key concatenation tests
 */
static void
sctp_print_digest(uint8_t * digest, uint32_t digestlen, const char *str)
{
	uint32_t i;

	printf("\n%s: 0x", str);
	if (digest == NULL)
		return;

	for (i = 0; i < digestlen; i++)
		printf("%02x", digest[i]);
}

static int
sctp_test_hmac(const char *str, uint16_t hmac_id, uint8_t * key,
    uint32_t keylen, uint8_t * text, uint32_t textlen,
    uint8_t * digest, uint32_t digestlen)
{
	uint8_t computed_digest[SCTP_AUTH_DIGEST_LEN_MAX];

	printf("\n%s:", str);
	sctp_hmac(hmac_id, key, keylen, text, textlen, computed_digest);
	sctp_print_digest(digest, digestlen, "Expected digest");
	sctp_print_digest(computed_digest, digestlen, "Computed digest");
	if (memcmp(digest, computed_digest, digestlen) != 0) {
		printf("\nFAILED");
		return (-1);
	} else {
		printf("\nPASSED");
		return (0);
	}
}


/*
 * RFC 2202: HMAC-SHA1 test cases
 */
void
sctp_test_hmac_sha1(void)
{
	uint8_t *digest;
	uint8_t key[128];
	uint32_t keylen;
	uint8_t text[128];
	uint32_t textlen;
	uint32_t digestlen = 20;
	int failed = 0;

	/*
	 * test_case =     1 key =
	 * 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b key_len =       20
	 * data =          "Hi There" data_len =      8 digest =
	 * 0xb617318655057264e28bc0b6fb378c8ef146be00
	 */
	keylen = 20;
	memset(key, 0x0b, keylen);
	textlen = 8;
	strcpy(text, "Hi There");
	digest = "\xb6\x17\x31\x86\x55\x05\x72\x64\xe2\x8b\xc0\xb6\xfb\x37\x8c\x8e\xf1\x46\xbe\x00";
	if (sctp_test_hmac("SHA1 test case 1", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     2 key =           "Jefe" key_len =       4 data =
	 * "what do ya want for nothing?" data_len =      28 digest =
	 * 0xeffcdf6ae5eb2fa2d27416d5f184df9c259a7c79
	 */
	keylen = 4;
	strcpy(key, "Jefe");
	textlen = 28;
	strcpy(text, "what do ya want for nothing?");
	digest = "\xef\xfc\xdf\x6a\xe5\xeb\x2f\xa2\xd2\x74\x16\xd5\xf1\x84\xdf\x9c\x25\x9a\x7c\x79";
	if (sctp_test_hmac("SHA1 test case 2", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     3 key =
	 * 0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa key_len =       20
	 * data =          0xdd repeated 50 times data_len =      50 digest
	 * = 0x125d7342b9ac11cd91a39af48aa17b4f63f175d3
	 */
	keylen = 20;
	memset(key, 0xaa, keylen);
	textlen = 50;
	memset(text, 0xdd, textlen);
	digest = "\x12\x5d\x73\x42\xb9\xac\x11\xcd\x91\xa3\x9a\xf4\x8a\xa1\x7b\x4f\x63\xf1\x75\xd3";
	if (sctp_test_hmac("SHA1 test case 3", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     4 key =
	 * 0x0102030405060708090a0b0c0d0e0f10111213141516171819 key_len = 25
	 * data =          0xcd repeated 50 times data_len =      50 digest
	 * =        0x4c9007f4026250c6bc8414f9bf50c86c2d7235da
	 */
	keylen = 25;
	memcpy(key, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19", keylen);
	textlen = 50;
	memset(text, 0xcd, textlen);
	digest = "\x4c\x90\x07\xf4\x02\x62\x50\xc6\xbc\x84\x14\xf9\xbf\x50\xc8\x6c\x2d\x72\x35\xda";
	if (sctp_test_hmac("SHA1 test case 4", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     5 key =
	 * 0x0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c key_len =       20
	 * data =          "Test With Truncation" data_len =      20 digest
	 * = 0x4c1a03424b55e07fe7f27be1d58bb9324a9a5a04 digest-96 =
	 * 0x4c1a03424b55e07fe7f27be1
	 */
	keylen = 20;
	memset(key, 0x0c, keylen);
	textlen = 20;
	strcpy(text, "Test With Truncation");
	digest = "\x4c\x1a\x03\x42\x4b\x55\xe0\x7f\xe7\xf2\x7b\xe1\xd5\x8b\xb9\x32\x4a\x9a\x5a\x04";
	if (sctp_test_hmac("SHA1 test case 5", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     6 key =           0xaa repeated 80 times key_len
	 * = 80 data =          "Test Using Larger Than Block-Size Key -
	 * Hash Key First" data_len =      54 digest =
	 * 0xaa4ae5e15272d00e95705637ce8a3b55ed402112
	 */
	keylen = 80;
	memset(key, 0xaa, keylen);
	textlen = 54;
	strcpy(text, "Test Using Larger Than Block-Size Key - Hash Key First");
	digest = "\xaa\x4a\xe5\xe1\x52\x72\xd0\x0e\x95\x70\x56\x37\xce\x8a\x3b\x55\xed\x40\x21\x12";
	if (sctp_test_hmac("SHA1 test case 6", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     7 key =           0xaa repeated 80 times key_len
	 * = 80 data =          "Test Using Larger Than Block-Size Key and
	 * Larger Than One Block-Size Data" data_len =      73 digest =
	 * 0xe8e99d0f45237d786d6bbaa7965c7808bbff1a91
	 */
	keylen = 80;
	memset(key, 0xaa, keylen);
	textlen = 73;
	strcpy(text, "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data");
	digest = "\xe8\xe9\x9d\x0f\x45\x23\x7d\x78\x6d\x6b\xba\xa7\x96\x5c\x78\x08\xbb\xff\x1a\x91";
	if (sctp_test_hmac("SHA1 test case 7", SCTP_AUTH_HMAC_ID_SHA1, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/* done with all tests */
	if (failed)
		printf("\nSHA1 test results: %d cases failed", failed);
	else
		printf("\nSHA1 test results: all test cases passed");
}

/*
 * RFC 2202: HMAC-MD5 test cases
 */
void
sctp_test_hmac_md5(void)
{
	uint8_t *digest;
	uint8_t key[128];
	uint32_t keylen;
	uint8_t text[128];
	uint32_t textlen;
	uint32_t digestlen = 16;
	int failed = 0;

	/*
	 * test_case =     1 key = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b
	 * key_len =       16 data = "Hi There" data_len =      8 digest =
	 * 0x9294727a3638bb1c13f48ef8158bfc9d
	 */
	keylen = 16;
	memset(key, 0x0b, keylen);
	textlen = 8;
	strcpy(text, "Hi There");
	digest = "\x92\x94\x72\x7a\x36\x38\xbb\x1c\x13\xf4\x8e\xf8\x15\x8b\xfc\x9d";
	if (sctp_test_hmac("MD5 test case 1", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     2 key =           "Jefe" key_len =       4 data =
	 * "what do ya want for nothing?" data_len =      28 digest =
	 * 0x750c783e6ab0b503eaa86e310a5db738
	 */
	keylen = 4;
	strcpy(key, "Jefe");
	textlen = 28;
	strcpy(text, "what do ya want for nothing?");
	digest = "\x75\x0c\x78\x3e\x6a\xb0\xb5\x03\xea\xa8\x6e\x31\x0a\x5d\xb7\x38";
	if (sctp_test_hmac("MD5 test case 2", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     3 key = 0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
	 * key_len =       16 data = 0xdd repeated 50 times data_len =
	 * 50 digest = 0x56be34521d144c88dbb8c733f0e8b3f6
	 */
	keylen = 16;
	memset(key, 0xaa, keylen);
	textlen = 50;
	memset(text, 0xdd, textlen);
	digest = "\x56\xbe\x34\x52\x1d\x14\x4c\x88\xdb\xb8\xc7\x33\xf0\xe8\xb3\xf6";
	if (sctp_test_hmac("MD5 test case 3", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     4 key =
	 * 0x0102030405060708090a0b0c0d0e0f10111213141516171819 key_len = 25
	 * data =          0xcd repeated 50 times data_len =      50 digest
	 * =        0x697eaf0aca3a3aea3a75164746ffaa79
	 */
	keylen = 25;
	memcpy(key, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19", keylen);
	textlen = 50;
	memset(text, 0xcd, textlen);
	digest = "\x69\x7e\xaf\x0a\xca\x3a\x3a\xea\x3a\x75\x16\x47\x46\xff\xaa\x79";
	if (sctp_test_hmac("MD5 test case 4", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     5 key = 0x0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c
	 * key_len =       16 data = "Test With Truncation" data_len =
	 * 20 digest = 0x56461ef2342edc00f9bab995690efd4c digest-96
	 * 0x56461ef2342edc00f9bab995
	 */
	keylen = 16;
	memset(key, 0x0c, keylen);
	textlen = 20;
	strcpy(text, "Test With Truncation");
	digest = "\x56\x46\x1e\xf2\x34\x2e\xdc\x00\xf9\xba\xb9\x95\x69\x0e\xfd\x4c";
	if (sctp_test_hmac("MD5 test case 5", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     6 key =           0xaa repeated 80 times key_len
	 * = 80 data =          "Test Using Larger Than Block-Size Key -
	 * Hash Key First" data_len =      54 digest =
	 * 0x6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd
	 */
	keylen = 80;
	memset(key, 0xaa, keylen);
	textlen = 54;
	strcpy(text, "Test Using Larger Than Block-Size Key - Hash Key First");
	digest = "\x6b\x1a\xb7\xfe\x4b\xd7\xbf\x8f\x0b\x62\xe6\xce\x61\xb9\xd0\xcd";
	if (sctp_test_hmac("MD5 test case 6", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/*
	 * test_case =     7 key =           0xaa repeated 80 times key_len
	 * = 80 data =          "Test Using Larger Than Block-Size Key and
	 * Larger Than One Block-Size Data" data_len =      73 digest =
	 * 0x6f630fad67cda0ee1fb1f562db3aa53e
	 */
	keylen = 80;
	memset(key, 0xaa, keylen);
	textlen = 73;
	strcpy(text, "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data");
	digest = "\x6f\x63\x0f\xad\x67\xcd\xa0\xee\x1f\xb1\xf5\x62\xdb\x3a\xa5\x3e";
	if (sctp_test_hmac("MD5 test case 7", SCTP_AUTH_HMAC_ID_MD5, key, keylen,
	    text, textlen, digest, digestlen) < 0)
		failed++;

	/* done with all tests */
	if (failed)
		printf("\nMD5 test results: %d cases failed", failed);
	else
		printf("\nMD5 test results: all test cases passed");
}

/*
 * test assoc key concatenation
 */
static int
sctp_test_key_concatenation(sctp_key_t * key1, sctp_key_t * key2,
    sctp_key_t * expected_key)
{
	sctp_key_t *key;
	int ret_val;

	sctp_show_key(key1, "\nkey1");
	sctp_show_key(key2, "\nkey2");
	key = sctp_compute_hashkey(key1, key2, NULL);
	sctp_show_key(expected_key, "\nExpected");
	sctp_show_key(key, "\nComputed");
	if (memcmp(key, expected_key, expected_key->keylen) != 0) {
		printf("\nFAILED");
		ret_val = -1;
	} else {
		printf("\nPASSED");
		ret_val = 0;
	}
	sctp_free_key(key1);
	sctp_free_key(key2);
	sctp_free_key(expected_key);
	sctp_free_key(key);
	return (ret_val);
}


void
sctp_test_authkey(void)
{
	sctp_key_t *key1, *key2, *expected_key;
	int failed = 0;

	/* test case 1 */
	key1 = sctp_set_key("\x01\x01\x01\x01", 4);
	key2 = sctp_set_key("\x01\x02\x03\x04", 4);
	expected_key = sctp_set_key("\x01\x01\x01\x01\x01\x02\x03\x04", 8);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 2 */
	key1 = sctp_set_key("\x00\x00\x00\x01", 4);
	key2 = sctp_set_key("\x02", 1);
	expected_key = sctp_set_key("\x00\x00\x00\x01\x02", 5);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 3 */
	key1 = sctp_set_key("\x01", 1);
	key2 = sctp_set_key("\x00\x00\x00\x02", 4);
	expected_key = sctp_set_key("\x01\x00\x00\x00\x02", 5);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 4 */
	key1 = sctp_set_key("\x00\x00\x00\x01", 4);
	key2 = sctp_set_key("\x01", 1);
	expected_key = sctp_set_key("\x01\x00\x00\x00\x01", 5);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 5 */
	key1 = sctp_set_key("\x01", 1);
	key2 = sctp_set_key("\x00\x00\x00\x01", 4);
	expected_key = sctp_set_key("\x01\x00\x00\x00\x01", 5);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 6 */
	key1 = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07", 11);
	key2 = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x08", 11);
	expected_key = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x08", 22);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* test case 7 */
	key1 = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x08", 11);
	key2 = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07", 11);
	expected_key = sctp_set_key("\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x08", 22);
	if (sctp_test_key_concatenation(key1, key2, expected_key) < 0)
		failed++;

	/* done with all tests */
	if (failed)
		printf("\nKey concatenation test results: %d cases failed", failed);
	else
		printf("\nKey concatenation test results: all test cases passed");
}


#if defined(STANDALONE_HMAC_TEST)
int
main(void)
{
	sctp_test_hmac_sha1();
	sctp_test_hmac_md5();
	sctp_test_authkey();
}

#endif				/* STANDALONE_HMAC_TEST */

#endif				/* SCTP_HMAC_TEST */
