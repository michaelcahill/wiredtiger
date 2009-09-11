/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2009 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

#define	WT_PSTOC_MASTER		0

/*******************************************
 * Server thread-of-control information
 *******************************************/
struct __wt_stoc {
#define	WT_SERVER_QSIZE		 40		/* Queued operations max */
	WT_TOC *ops[WT_SERVER_QSIZE];		/* Queued operations */

	u_int id;				/* Server ID (array offset) */
	pthread_t tid;				/* System thread ID */

	int running;				/* Thread active */

	/*
	 * Payload: when a WT_STOC starts performing an operation, it loads
	 * some handles used by underlying functions to figure out what to
	 * do.  This is a simply cache, just so the called functions don't
	 * have to dig through the WT_TOC.
	 */
	WT_TOC *toc;				/* API thread of control */
	ENV *env;				/* Enclosing ENV */
	DB *db;					/* Enclosing DB */

	/*
	 * Per-server thread cache of database pages.
	 */
#define	WT_CACHE_DEFAULT_SIZE		(20)		/* 20MB */
	u_int64_t cache_max;			/* Cache bytes maximum */
	u_int64_t cache_bytes;			/* Cache bytes allocated */

	/*
	 * Each in-memory page is threaded on two queues: a hash queue
	 * based on its file and page number, and an LRU list.
	 */
	u_int32_t hashsize;
#define	WT_HASH(stoc, addr)	((addr) % (stoc)->hashsize)
	TAILQ_HEAD(__wt_page_hqh, __wt_page) *hqh;
	TAILQ_HEAD(__wt_page_lqh, __wt_page) lqh;

	WT_STATS *stats;			/* Server statistics */
};

#define	WT_STOC_FOREACH(ienv, stoc, i)					\
	for ((i) = 0, (stoc) = (ienv)->sq;				\
	    (i) < (ienv)->sq_entries; ++(i), ++(stoc))

#if defined(__cplusplus)
}
#endif
