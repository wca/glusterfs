/*
  Copyright (c) 2007-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#ifndef _NLM_H_RPCGEN
#define _NLM_H_RPCGEN

#include <rpc/rpc.h>

// #if defined(__NetBSD__) || defined(__FreeBSD__)
#define xdr_u_quad_t xdr_u_int64_t
#define xdr_quad_t   xdr_int64_t
#define xdr_uint32_t xdr_u_int32_t
#define xdr_uint64_t xdr_u_int64_t
// #endif

#ifdef __cplusplus
extern "C" {
#endif

#define MAXNETOBJ_SZ 1024
#define LM_MAXSTRLEN 1024
#define MAXNAMELEN 1025

// #if defined(__NetBSD__) || defined(__FreeBSD__)
#define xdr_u_quad_t xdr_u_int64_t
#define xdr_quad_t   xdr_int64_t
#define xdr_uint32_t xdr_u_int32_t
#define xdr_uint64_t xdr_u_int64_t
// #endif

/*
 * The following enums are actually bit encoded for efficient
 * boolean algebra.... DON'T change them.....
 */

enum fsh_mode {
	fsm_DN = 0,
	fsm_DR = 1,
	fsm_DW = 2,
	fsm_DRW = 3,
};
typedef enum fsh_mode fsh_mode;

enum fsh_access {
	fsa_NONE = 0,
	fsa_R = 1,
	fsa_W = 2,
	fsa_RW = 3,
};
typedef enum fsh_access fsh_access;
/* definitions for NLM version 4 */

enum nlm4_stats {
	nlm4_granted = 0,
	nlm4_denied = 1,
	nlm4_denied_nolock = 2,
	nlm4_blocked = 3,
	nlm4_denied_grace_period = 4,
	nlm4_deadlck = 5,
	nlm4_rofs = 6,
	nlm4_stale_fh = 7,
	nlm4_fbig = 8,
	nlm4_failed = 9,
};
typedef enum nlm4_stats nlm4_stats;

struct nlm4_stat {
	nlm4_stats stat;
};
typedef struct nlm4_stat nlm4_stat;

struct nlm4_holder {
	bool_t exclusive;
	u_int32_t svid;
	netobj oh;
	u_int64_t l_offset;
	u_int64_t l_len;
};
typedef struct nlm4_holder nlm4_holder;

struct nlm4_lock {
	char *caller_name;
	netobj fh;
	netobj oh;
	u_int32_t svid;
	u_int64_t l_offset;
	u_int64_t l_len;
};
typedef struct nlm4_lock nlm4_lock;

struct nlm4_share {
	char *caller_name;
	netobj fh;
	netobj oh;
	fsh_mode mode;
	fsh_access access;
};
typedef struct nlm4_share nlm4_share;

struct nlm4_testrply {
	nlm4_stats stat;
	union {
		struct nlm4_holder holder;
	} nlm4_testrply_u;
};
typedef struct nlm4_testrply nlm4_testrply;

struct nlm4_testres {
	netobj cookie;
	nlm4_testrply stat;
};
typedef struct nlm4_testres nlm4_testres;

struct nlm4_testargs {
	netobj cookie;
	bool_t exclusive;
	struct nlm4_lock alock;
};
typedef struct nlm4_testargs nlm4_testargs;

struct nlm4_res {
	netobj cookie;
	nlm4_stat stat;
};
typedef struct nlm4_res nlm4_res;

struct nlm4_lockargs {
	netobj cookie;
	bool_t block;
	bool_t exclusive;
	struct nlm4_lock alock;
	bool_t reclaim;
	int state;
};
typedef struct nlm4_lockargs nlm4_lockargs;

struct nlm4_cancargs {
	netobj cookie;
	bool_t block;
	bool_t exclusive;
	struct nlm4_lock alock;
};
typedef struct nlm4_cancargs nlm4_cancargs;

struct nlm4_unlockargs {
	netobj cookie;
	struct nlm4_lock alock;
};
typedef struct nlm4_unlockargs nlm4_unlockargs;

struct nlm4_shareargs {
	netobj cookie;
	nlm4_share share;
	bool_t reclaim;
};
typedef struct nlm4_shareargs nlm4_shareargs;

struct nlm4_shareres {
	netobj cookie;
	nlm4_stats stat;
	int sequence;
};
typedef struct nlm4_shareres nlm4_shareres;

struct nlm4_freeallargs {
        char *name;
        u_int32_t state;
};
typedef struct nlm4_freeallargs nlm4_freeallargs;


#define NLM4_NULL          0
#define NLM4_TEST          1
#define NLM4_LOCK          2
#define NLM4_CANCEL        3
#define NLM4_UNLOCK        4
#define NLM4_GRANTED       5
#define NLM4_TEST_MSG      6
#define NLM4_LOCK_MSG      7
#define NLM4_CANCEL_MSG    8
#define NLM4_UNLOCK_MSG    9
#define NLM4_GRANTED_MSG   10
#define NLM4_TEST_RES      11
#define NLM4_LOCK_RES      12
#define NLM4_CANCEL_RES    13
#define NLM4_UNLOCK_RES    14
#define NLM4_GRANTED_RES   15
#define NLM4_SM_NOTIFY     16
#define NLM4_SEVENTEEN     17
#define NLM4_EIGHTEEN      18
#define NLM4_NINETEEN      19
#define NLM4_SHARE         20
#define NLM4_UNSHARE       21
#define NLM4_NM_LOCK       22
#define NLM4_FREE_ALL      23
#define NLM4_PROC_COUNT    24

/* the xdr functions */

#if defined(__STDC__) || defined(__cplusplus)
extern  bool_t xdr_netobj (XDR *, netobj*);
extern  bool_t xdr_fsh_mode (XDR *, fsh_mode*);
extern  bool_t xdr_fsh_access (XDR *, fsh_access*);
extern  bool_t xdr_nlm4_stats (XDR *, nlm4_stats*);
extern  bool_t xdr_nlm4_stat (XDR *, nlm4_stat*);
extern  bool_t xdr_nlm4_holder (XDR *, nlm4_holder*);
extern  bool_t xdr_nlm4_lock (XDR *, nlm4_lock*);
extern  bool_t xdr_nlm4_share (XDR *, nlm4_share*);
extern  bool_t xdr_nlm4_testrply (XDR *, nlm4_testrply*);
extern  bool_t xdr_nlm4_testres (XDR *, nlm4_testres*);
extern  bool_t xdr_nlm4_testargs (XDR *, nlm4_testargs*);
extern  bool_t xdr_nlm4_res (XDR *, nlm4_res*);
extern  bool_t xdr_nlm4_lockargs (XDR *, nlm4_lockargs*);
extern  bool_t xdr_nlm4_cancargs (XDR *, nlm4_cancargs*);
extern  bool_t xdr_nlm4_unlockargs (XDR *, nlm4_unlockargs*);
extern  bool_t xdr_nlm4_shareargs (XDR *, nlm4_shareargs*);
extern  bool_t xdr_nlm4_shareres (XDR *, nlm4_shareres*);
extern  bool_t xdr_nlm4_freeallargs (XDR *, nlm4_freeallargs*);

#else /* K&R C */
extern bool_t xdr_netobj ();
extern bool_t xdr_fsh_mode ();
extern bool_t xdr_fsh_access ();
extern bool_t xdr_nlm4_stats ();
extern bool_t xdr_nlm4_stat ();
extern bool_t xdr_nlm4_holder ();
extern bool_t xdr_nlm4_lock ();
extern bool_t xdr_nlm4_share ();
extern bool_t xdr_nlm4_testrply ();
extern bool_t xdr_nlm4_testres ();
extern bool_t xdr_nlm4_testargs ();
extern bool_t xdr_nlm4_res ();
extern bool_t xdr_nlm4_lockargs ();
extern bool_t xdr_nlm4_cancargs ();
extern bool_t xdr_nlm4_unlockargs ();
extern bool_t xdr_nlm4_shareargs ();
extern bool_t xdr_nlm4_shareres ();
extern bool_t xdr_nlm4_freeallargs ();

#endif /* K&R C */

#ifdef __cplusplus
}
#endif

#endif /* !_NLM_H_RPCGEN */
