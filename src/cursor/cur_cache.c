/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * The cache cursor has two modes:
 *   - "cache:" iterates over files in the database cache, returning one row per file with a value
 *     tuple of <dirty, bytes in cache, dirty bytes, update bytes>.
 *   - "cache:file:<filename>" iterates over pages in cache for a specific file, returning one row
 *     per page with a value tuple of <dirty, bytes in memory, dirty bytes, update bytes, read
 *     generation, page type, pin reason>.
 *
 *
 * A page-by-page cursor also configured with next_random_sample_size walks the file's cached pages
 * and returns approximately that many of them, spread across the tree, instead of every page. The
 * sampled walk is cache-only: it reads nothing from disk and legitimately returns no rows for a
 * cold file.
 */

/*
 * __cache_file_next --
 *     Move to the next file in cache summary mode.
 */
static void
__cache_file_next(WT_SESSION_IMPL *session, WT_CURSOR_CACHE *ccache)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;

    conn = S2C(session);

    /*
     * Walk the data handle list under the handle-list read lock. The list is protected by a
     * read-write lock: readers take shared mode, writers take exclusive mode. We only need to
     * iterate (find the next live btree), so shared access is sufficient.
     */
    WT_WITH_HANDLE_LIST_READ_LOCK(session, {
        /* If we haven't started yet, take the first btree. */
        if (ccache->dh_current == NULL)
            dhandle = TAILQ_FIRST(&conn->dhqh);
        else
            dhandle = TAILQ_NEXT(ccache->dh_current, q);

        /* Walk forward until we find a live btree handle. */
        while (dhandle != NULL) {
            if (WT_DHANDLE_BTREE(dhandle) && F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
              !F_ISSET(dhandle, WT_DHANDLE_DEAD)) {
                ccache->dh_current = dhandle;
                break;
            }
            dhandle = TAILQ_NEXT(dhandle, q);
        }

        if (dhandle == NULL)
            ccache->done = true;
    });
}

/*
 * __curcache_get_key --
 *     WT_CURSOR->get_key for cache cursors.
 */
static int
__curcache_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, get_key, NULL);

    WT_ERR(__cursor_needkey(cursor));

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        WT_ITEM *item;

        va_start(ap, cursor);
        item = va_arg(ap, WT_ITEM *);
        item->data = ccache->current_key;
        item->size = strlen(ccache->current_key) + 1;
        va_end(ap);
    } else {
        const char **keyp;

        va_start(ap, cursor);
        keyp = va_arg(ap, const char **);
        if (keyp != NULL)
            *keyp = ccache->current_key;
        va_end(ap);
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curcache_file_value --
 *     Assemble the value tuple for the current file-summary row: <dirty, bytes in cache, dirty
 *     bytes, update bytes>.
 */
static void
__curcache_file_value(WT_SESSION_IMPL *session, WT_CURSOR_CACHE *ccache, bool *dirtyp,
  uint64_t *cache_bytesp, uint64_t *dirty_bytesp, uint64_t *update_bytesp)
{
    WT_WITH_DHANDLE(session, ccache->dh_current, {
        *dirtyp = S2BT(session)->modified ? 1 : 0;
        *dirty_bytesp = __wt_btree_dirty_inuse(session);
        *cache_bytesp = __wt_btree_bytes_inuse(session);
        *update_bytesp = __wt_btree_bytes_updates(session);
    });
}

/*
 * __curcache_file_get_value --
 *     WT_CURSOR->get_value for file-summary mode (cache:).
 */
static int
__curcache_file_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t cache_bytes, dirty_bytes, update_bytes;
    bool dirty;
    va_list ap;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, get_value, NULL);

    WT_ERR(__cursor_needvalue(cursor));

    __curcache_file_value(session, ccache, &dirty, &cache_bytes, &dirty_bytes, &update_bytes);

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        WT_ITEM *item;
        size_t size;

        /* Encode the tuple into the cursor's value buffer. */
        WT_ERR(__wt_struct_size(session, &size, cursor->value_format, (uint64_t)dirty, cache_bytes,
          dirty_bytes, update_bytes));
        WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
        WT_ERR(__wt_struct_pack(session, cursor->value.mem, size, cursor->value_format,
          (uint64_t)dirty, cache_bytes, dirty_bytes, update_bytes));

        va_start(ap, cursor);
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->value.data;
        item->size = cursor->value.size;
        va_end(ap);
    } else {
        uint64_t *dirtyp, *cache_bytesp, *dirty_bytesp, *update_bytesp;

        /* Value format: "qqqq" - <dirty, bytes in cache, dirty bytes, update bytes>. */
        va_start(ap, cursor);
        dirtyp = va_arg(ap, uint64_t *);
        cache_bytesp = va_arg(ap, uint64_t *);
        dirty_bytesp = va_arg(ap, uint64_t *);
        update_bytesp = va_arg(ap, uint64_t *);
        if (dirtyp != NULL)
            *dirtyp = dirty ? 1 : 0;
        if (cache_bytesp != NULL)
            *cache_bytesp = cache_bytes;
        if (dirty_bytesp != NULL)
            *dirty_bytesp = dirty_bytes;
        if (update_bytesp != NULL)
            *update_bytesp = update_bytes;
        va_end(ap);
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curcache_page_value --
 *     Assemble the value tuple for the current page row: <dirty, bytes in memory, dirty bytes,
 *     update bytes, read generation, page type, pin reason>.
 *
 * A page is dirty when it has a modify structure; the cache accounts its full memory footprint as
 *     dirty bytes (see the cache dirty accounting in the page-modified path), so dirty bytes equal
 *     the footprint for a dirty page and zero otherwise. The page type is one of the WT_PAGE_*
 *     values, and the pin reason is one of the WT_PAGEPIN_* values (WT_PAGEPIN_NONE when the page
 *     can be evicted).
 */
static void
__curcache_page_value(WT_SESSION_IMPL *session, WT_CURSOR_CACHE *ccache, bool *dirtyp,
  uint64_t *cache_bytesp, uint64_t *dirty_bytesp, uint64_t *update_bytesp, uint64_t *read_genp,
  uint64_t *page_typep, uint64_t *pin_reasonp)
{
    WT_PAGE *page;
    WT_PAGEPIN_REASON pin_reason;
    WT_PAGE_MODIFY *modify;
    uint64_t cache_bytes;

    page = ccache->walk_ref->page;
    WT_ASSERT(session, page != NULL);
    modify = page->modify;
    cache_bytes = __wt_atomic_load_size_relaxed(&page->memory_footprint);

    *dirtyp = __wt_page_is_modified(page);
    *cache_bytesp = cache_bytes;
    *dirty_bytesp = modify != NULL ? modify->bytes_dirty : 0;
    *update_bytesp = modify != NULL ? modify->bytes_updates : 0;
    *read_genp = page->read_gen;
    *page_typep = page->type;

    /*
     * The pin reason checks read-only and checkpointing state of the page's btree; session->dhandle
     * is not stable across calls, so use the cursor's.
     */
    WT_WITH_BTREE(session, ccache->btree,
      { pin_reason = __wt_pagepin_reason(session, ccache->walk_ref, NULL); });
    *pin_reasonp = (uint64_t)pin_reason;
}

/*
 * __curcache_page_get_value --
 *     WT_CURSOR->get_value for page-by-page mode (cache:file:<filename>).
 */
static int
__curcache_page_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t cache_bytes, dirty_bytes, update_bytes, page_type, pin_reason, read_gen;
    bool dirty;
    va_list ap;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, get_value, NULL);

    WT_ERR(__cursor_needvalue(cursor));

    __curcache_page_value(session, ccache, &dirty, &cache_bytes, &dirty_bytes, &update_bytes,
      &read_gen, &page_type, &pin_reason);

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        WT_ITEM *item;
        size_t size;

        /* Encode the tuple into the cursor's value buffer. */
        WT_ERR(__wt_struct_size(session, &size, cursor->value_format, (uint64_t)dirty, cache_bytes,
          dirty_bytes, update_bytes, read_gen, page_type, pin_reason));
        WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
        WT_ERR(
          __wt_struct_pack(session, cursor->value.mem, size, cursor->value_format, (uint64_t)dirty,
            cache_bytes, dirty_bytes, update_bytes, read_gen, page_type, pin_reason));

        va_start(ap, cursor);
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->value.data;
        item->size = cursor->value.size;
        va_end(ap);
    } else {
        uint64_t *dirtyp, *cache_bytesp, *dirty_bytesp, *update_bytesp, *page_typep, *pin_reasonp,
          *read_genp;

        /* Value format: "qqqqqqq" - <dirty, bytes in memory, dirty bytes, update bytes, read gen,
         * page type, pin reason>.
         */
        va_start(ap, cursor);
        dirtyp = va_arg(ap, uint64_t *);
        cache_bytesp = va_arg(ap, uint64_t *);
        dirty_bytesp = va_arg(ap, uint64_t *);
        update_bytesp = va_arg(ap, uint64_t *);
        read_genp = va_arg(ap, uint64_t *);
        page_typep = va_arg(ap, uint64_t *);
        pin_reasonp = va_arg(ap, uint64_t *);
        if (dirtyp != NULL)
            *dirtyp = dirty ? 1 : 0;
        if (cache_bytesp != NULL)
            *cache_bytesp = cache_bytes;
        if (dirty_bytesp != NULL)
            *dirty_bytesp = dirty_bytes;
        if (update_bytesp != NULL)
            *update_bytesp = update_bytes;
        if (read_genp != NULL)
            *read_genp = read_gen;
        if (page_typep != NULL)
            *page_typep = page_type;
        if (pin_reasonp != NULL)
            *pin_reasonp = pin_reason;
        va_end(ap);
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curcache_set_key --
 *     WT_CURSOR->set_key for cache cursors. Not supported.
 */
static void
__curcache_set_key(WT_CURSOR *cursor, ...)
{
    WT_UNUSED(cursor);
}

/*
 * __curcache_set_value --
 *     WT_CURSOR->set_value for cache cursors. Not supported.
 */
static void
__curcache_set_value(WT_CURSOR *cursor, ...)
{
    WT_UNUSED(cursor);
}

/*
 * __curcache_next_file --
 *     WT_CURSOR->next method for file-summary mode (cache:).
 */
static int
__curcache_next_file(WT_CURSOR *cursor)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, NULL);

    /* Initialize on demand. */
    if (ccache->notinitialized) {
        ccache->notinitialized = false;
        ccache->dh_current = NULL;
        ccache->done = false;
    }

    __cache_file_next(session, ccache);
    if (ccache->done)
        WT_ERR(WT_NOTFOUND);

    /* Build the key string: "cache:file:<filename>". */
    __wt_free(session, ccache->current_key);
    {
        char *key_str;
        const char *filename = ccache->dh_current->name + strlen("file:");
        WT_ITEM key_buf;
        memset(&key_buf, 0, sizeof(key_buf));
        WT_ERR(__wt_buf_fmt(session, &key_buf, "cache:file:%s", filename));
        WT_ERR(__wt_strdup(session, key_buf.data, &key_str));
        ccache->current_key = key_str;
        __wt_buf_free(session, &key_buf);
    }

    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __curcache_pages_in_cache --
 *     Estimate this tree's resident page count by scaling the cache's page count by the tree's
 *     share of cache bytes. Assumes this tree's average resident page size matches the cache-wide
 *     average; approximate by construction, and stale by the time the walk starts.
 */
static uint64_t
__curcache_pages_in_cache(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    uint64_t bytes, pages, total;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    bytes = __wt_atomic_load_uint64_relaxed(&btree->bytes_inmem);
    total = __wt_atomic_load_uint64_relaxed(&cache->bytes_inmem);
    if (bytes == 0 || total == 0)
        return (0);
    /* Scale in floating point: bytes * pages overflows 64 bits on a large cache. */
    pages = __wt_cache_pages_inuse(cache);
    return ((uint64_t)(((double)bytes / (double)total) * (double)pages));
}

/*
 * __curcache_next_page --
 *     WT_CURSOR->next method for page-by-page mode (cache:file:<filename>).
 */
static int
__curcache_next_page(WT_CURSOR *cursor)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t est;
    uint32_t flags;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, NULL);

    /* An exhausted cursor stays exhausted until it is reset. */
    if (ccache->done)
        WT_ERR(WT_NOTFOUND);

    /* Initialize on demand: open the btree. */
    if (ccache->notinitialized) {
        ccache->notinitialized = false;
        ccache->done = false;
        ccache->pages_seen = 0;
        ccache->pages_returned = 0;

        WT_ERR(__wt_session_get_btree_ckpt(session, ccache->file_uri, NULL, 0, NULL, NULL));
        /* Keep the btree on the cursor; session->dhandle is not stable across calls. */
        ccache->btree = S2BT(session);

        if (ccache->sample_size != 0) {
            /* Residency moved since the last pass, so take a fresh estimate of it. */
            est = __curcache_pages_in_cache(session);
            ccache->stride = est == 0 ? 1 : WT_MAX(1, est / ccache->sample_size);
            ccache->offset = __wt_random(&ccache->rnd) % ccache->stride;
            __wt_verbose_debug1(session, WT_VERB_EVICTION,
              "cache cursor sampling: estimate %" PRIu64 " resident pages, stride %" PRIu64
              ", sample %" PRIu64,
              est, ccache->stride, ccache->sample_size);
        }
    }

    {
        WT_REF *next_walk;

        /*
         * Walk to the next page using tree walk. A NULL ref starts the walk at the beginning of the
         * tree, a NULL result means the walk is finished, and every page is returned exactly once
         * (the root last). Sampling walks the same pages and hands back one per stride, which
         * spreads the rows over the tree instead of truncating it at its left edge.
         */
        flags = WT_READ_INTERNAL_OP | WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED | WT_READ_NO_EVICT;
        if (ccache->sample_size != 0)
            /* Resident pages only, so the walk reads nothing from disk. */
            FLD_SET(flags, WT_READ_CACHE | WT_READ_NO_WAIT);

        for (;;) {
            next_walk = ccache->walk_ref;
            WT_WITH_BTREE(session, ccache->btree, ret = __wt_tree_walk(session, &next_walk, flags));

            /*
             * A tree walk releases the reference it was handed on every return, so the cursor's
             * hazard pointer is whatever the walk left behind: the page it returned, or nothing.
             */
            ccache->walk_ref = next_walk;
            if (ret != 0)
                goto err;
            if (next_walk == NULL) {
                ccache->done = true;
                WT_ERR(WT_NOTFOUND);
            }

            if (ccache->sample_size == 0)
                break;
            ++ccache->pages_seen;
            if (((ccache->pages_seen + ccache->offset) % ccache->stride) == 0)
                break;
        }
    }

    /* The cap, not the estimate, decides how many rows the cursor can return. */
    if (ccache->sample_size != 0 && ++ccache->pages_returned > ccache->sample_size) {
        ccache->done = true;
        WT_ERR(WT_NOTFOUND);
    }

    {
        WT_PAGE *page;
        size_t page_size;

        page = ccache->walk_ref->page;
        page_size =
          WT_BLOCK_HEADER_BYTE_SIZE + __wt_atomic_load_size_relaxed(&page->memory_footprint);

        __wt_free(session, ccache->current_key);
        {
            char *key_str;
            WT_ITEM key_buf;
            memset(&key_buf, 0, sizeof(key_buf));
            WT_ERR(__wt_buf_fmt(session, &key_buf, "page:%" WT_SIZET_FMT, page_size));
            WT_ERR(__wt_strdup(session, key_buf.data, &key_str));
            ccache->current_key = key_str;
            __wt_buf_free(session, &key_buf);
        }

        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }

err:
    if (ret != 0) {
        ccache->done = true;
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    }
    API_END_RET(session, ret);
}

/*
 * __curcache_reset --
 *     WT_CURSOR->reset method for cache cursors.
 */
static int
__curcache_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    ccache->notinitialized = true;
    ccache->dh_current = NULL;
    ccache->done = false;
    ccache->pages_seen = 0;
    ccache->pages_returned = 0;
    if (ccache->btree != NULL) {
        WT_WITH_BTREE(session, ccache->btree, {
            WT_TRET(__wt_page_release(session, ccache->walk_ref, 0));
            WT_TRET(__wt_session_release_dhandle(session));
        });
        ccache->walk_ref = NULL;
        ccache->btree = NULL;
    }
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __curcache_search --
 *     WT_CURSOR->search method for cache cursors. Not supported.
 */
static int
__curcache_search(WT_CURSOR *cursor)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);
    WT_RET_MSG(session, ENOTSUP, "Cache cursor does not support search");
}

/*
 * __curcache_close --
 *     WT_CURSOR->close method for cache cursors.
 */
static int
__curcache_close(WT_CURSOR *cursor)
{
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ccache = (WT_CURSOR_CACHE *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);

    /* Release the page hazard pointer and btree dhandle opened for page-by-page mode (if any). */
    if (ccache->btree != NULL) {
        WT_WITH_BTREE(session, ccache->btree, {
            WT_TRET(__wt_page_release(session, ccache->walk_ref, 0));
            WT_TRET(__wt_session_release_dhandle(session));
        });
        ccache->walk_ref = NULL;
        ccache->btree = NULL;
    }

    __wt_free(session, ccache->current_key);
    __wt_free(session, ccache->file_uri);

err:
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_curcache_open --
 *     WT_SESSION->open_cursor method for the cache cursor type.
 */
int
__wt_curcache_open(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor;
    WT_CURSOR_CACHE *ccache;
    WT_DECL_RET;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_CACHE);

    /* Random-record semantics don't apply to a cache cursor, only the sample size does. */
    WT_RET(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0)
        WT_RET_MSG(session, ENOTSUP, "next_random configuration not supported by cache cursors");

    WT_RET(__wt_calloc_one(session, &ccache));
    cursor = (WT_CURSOR *)ccache;

    /* Choose the appropriate next method based on URI. */
    if (strcmp(uri, "cache:") == 0) {
        /* File-summary mode: iterate over all files in cache. */
        WT_CURSOR_STATIC_INIT(iface, __curcache_get_key, /* get-key */
          __curcache_file_get_value,                     /* get-value */
          __wti_cursor_get_raw_key_value_notsup,         /* get-raw-key-value */
          __curcache_set_key,                            /* set-key */
          __curcache_set_value,                          /* set-value */
          __wti_cursor_compare_notsup,                   /* compare */
          __wti_cursor_equals_notsup,                    /* equals */
          __curcache_next_file,                          /* next */
          __wt_cursor_notsup,                            /* prev */
          __curcache_reset,                              /* reset */
          __curcache_search,                             /* search */
          __wt_cursor_search_near_notsup,                /* search-near */
          __wt_cursor_notsup,                            /* insert */
          __wt_cursor_modify_notsup,                     /* modify */
          __wt_cursor_notsup,                            /* update */
          __wt_cursor_notsup,                            /* remove */
          __wt_cursor_notsup,                            /* reserve */
          __wt_cursor_config_notsup,                     /* reconfigure */
          __wt_cursor_notsup,                            /* largest_key */
          __wt_cursor_config_notsup,                     /* bound */
          __wt_cursor_notsup,                            /* cache */
          __wt_cursor_reopen_notsup,                     /* reopen */
          __wt_cursor_checkpoint_id,                     /* checkpoint ID */
          __curcache_close);                             /* close */

        *cursor = iface;
        cursor->session = (WT_SESSION *)session;
        cursor->key_format = "S";
        cursor->value_format = "qqqq";

        ccache->notinitialized = true;
        ccache->dh_current = NULL;
        ccache->done = false;
    } else if (WT_PREFIX_MATCH(uri, "cache:file:")) {
        /* Page-by-page mode: iterate over pages for a specific file. */
        WT_CURSOR_STATIC_INIT(iface, __curcache_get_key, /* get-key */
          __curcache_page_get_value,                     /* get-value */
          __wti_cursor_get_raw_key_value_notsup,         /* get-raw-key-value */
          __curcache_set_key,                            /* set-key */
          __curcache_set_value,                          /* set-value */
          __wti_cursor_compare_notsup,                   /* compare */
          __wti_cursor_equals_notsup,                    /* equals */
          __curcache_next_page,                          /* next */
          __wt_cursor_notsup,                            /* prev */
          __curcache_reset,                              /* reset */
          __curcache_search,                             /* search */
          __wt_cursor_search_near_notsup,                /* search-near */
          __wt_cursor_notsup,                            /* insert */
          __wt_cursor_modify_notsup,                     /* modify */
          __wt_cursor_notsup,                            /* update */
          __wt_cursor_notsup,                            /* remove */
          __wt_cursor_notsup,                            /* reserve */
          __wt_cursor_config_notsup,                     /* reconfigure */
          __wt_cursor_notsup,                            /* largest_key */
          __wt_cursor_config_notsup,                     /* bound */
          __wt_cursor_notsup,                            /* cache */
          __wt_cursor_reopen_notsup,                     /* reopen */
          __wt_cursor_checkpoint_id,                     /* checkpoint ID */
          __curcache_close);                             /* close */

        *cursor = iface;
        cursor->session = (WT_SESSION *)session;
        cursor->key_format = "S";
        cursor->value_format = "qqqqqqq";

        /*
         * Strip the prefix to get the file name, then restore the "file:" scheme so the name
         * matches the dhandle name produced by file-summary mode.
         */
        {
            WT_ITEM uri_buf;
            memset(&uri_buf, 0, sizeof(uri_buf));
            WT_ERR(__wt_buf_fmt(session, &uri_buf, "file:%s", uri + strlen("cache:file:")));
            WT_ERR(__wt_strdup(session, uri_buf.data, &ccache->file_uri));
            __wt_buf_free(session, &uri_buf);
        }
        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_sample_size", 0, &cval));
        ccache->sample_size = (uint64_t)cval.val;
        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_seed", 0, &cval));
        if (cval.val != 0)
            __wt_random_init_seed(&ccache->rnd, (uint64_t)cval.val);
        else
            __wt_random_init(session, &ccache->rnd);

        ccache->notinitialized = true;
        ccache->btree = NULL;
        ccache->walk_ref = NULL;
        ccache->done = false;
    } else
        WT_ERR_MSG(session, EINVAL, "Invalid cache cursor URI: %s", uri);

    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
err:
        __curcache_close(cursor);
        *cursorp = NULL;
    }

    return (ret);
}
