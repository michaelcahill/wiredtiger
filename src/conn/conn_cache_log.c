/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1)
/*
 * !!!
 * GCC with -Wformat-nonliteral complains about calls to strftime in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#endif

/*
 * __cachelog_config_discard --
 *     Discard all cache-log configuration.
 */
static int
__cachelog_config_discard(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /*
     * Discard all cache-log configuration information, called when reconfiguring or destroying the
     * cache logging setup.
     */
    __wt_free(session, conn->cache_log.format);
    ret = __wt_fclose(session, &conn->cache_log.fs);
    __wt_free(session, conn->cache_log.path);
    conn->cache_log.stamp = NULL;
    conn->cache_log.usecs = 0;
    return (ret);
}

/*
 * __cachelog_config --
 *     Parse and setup the cache server options.
 */
static int
__cachelog_config(WT_SESSION_IMPL *session, const char **cfg, bool *runp)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    /*
     * A note on reconfiguration: the standard "is this configuration string allowed" checks should
     * fail if reconfiguration has invalid strings, for example, "log=(enabled)", or
     * "cache_log=(path=XXX)", because the connection reconfiguration method doesn't allow those
     * strings. Additionally, the base configuration values during reconfiguration are the currently
     * configured values (so we don't revert to default values when repeatedly reconfiguring), and
     * configuration processing of a currently set value should not change the currently set value.
     *
     * In this code path, a previous cache log server reconfiguration may have stopped the server
     * (and we're about to restart it). Because stopping the server discarded the configured
     * information stored in the connection structure, we have to re-evaluate all configuration
     * values, reconfiguration can't skip any of them.
     */

    conn = S2C(session);

    /* Only start the server if wait time is non-zero */
    WT_RET(__wt_config_gets(session, cfg, "cache_log.wait", &cval));
    *runp = cval.val != 0;
    conn->cache_log.usecs = (uint64_t)cval.val * WT_MILLION;

    /*
     * Only set the JSON flag when stats are enabled, otherwise setting this flag can implicitly
     * enable statistics gathering.
     */
    WT_RET(__wt_config_gets(session, cfg, "cache_log.json", &cval));
    if (cval.val != 0 && WT_STAT_ENABLED(session))
        FLD_SET(conn->cache_log_flags, WT_CACHELOG_JSON);

    WT_RET(__wt_config_gets(session, cfg, "cache_log.on_close", &cval));
    if (cval.val != 0)
        FLD_SET(conn->cache_log_flags, WT_CACHELOG_ON_CLOSE);

    WT_RET(__wt_config_gets(session, cfg, "cache_log.sample", &cval));
    conn->cache_log.sample = (uint64_t)cval.val;

    /*
     * See above for the details: during reconfiguration we're loading the path value from the saved
     * configuration information, and it's required during reconfiguration because we potentially
     * stopped and are restarting, the server.
     */
    WT_RET(__wt_config_gets(session, cfg, "cache_log.path", &cval));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_buf_fmt(session, tmp, "%.*s/%s", (int)cval.len, cval.str, WT_CACHELOG_FILENAME));
    __wt_free(session, conn->cache_log.path);
    WT_ERR(__wt_filename(session, tmp->data, &conn->cache_log.path));

/*
 * When using JSON format, use the same timestamp format as MongoDB by
 * default. This requires caution: the user might have set the timestamp
 * in a previous reconfigure call and we don't want to override that, so
 * compare the retrieved value with the default value to decide if we
 * should use the JSON default.
 *
 * (This still implies if the user explicitly sets the timestamp to the
 * default value, then sets the JSON flag in a separate reconfigure
 * call, or vice-versa, we will incorrectly switch to the JSON default
 * timestamp. But there's no way to detect that, and this is all a low
 * probability path.)
 *
 * !!!
 * Don't rewrite in the compressed "%FT%T.000Z" form, MSVC13 segfaults.
 */
#define WT_TIMESTAMP_DEFAULT "%b %d %H:%M:%S"
#define WT_TIMESTAMP_JSON_DEFAULT "%Y-%m-%dT%H:%M:%S.000Z"
    WT_ERR(__wt_config_gets(session, cfg, "cache_log.timestamp", &cval));
    if (FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON) &&
      WT_CONFIG_LIT_MATCH(WT_TIMESTAMP_DEFAULT, cval))
        WT_ERR(__wt_strdup(session, WT_TIMESTAMP_JSON_DEFAULT, &conn->cache_log.format));
    else
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->cache_log.format));

err:
    __wt_scr_free(session, &tmp);

    return (ret);
}

/*
 * __cachelog_print_header --
 *     Write the header for cache log when running in JSON mode.
 */
static int
__cachelog_print_header(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON))
        return (0);

    /*
     * This flag is required in order to generate correct JSON when printing out cache state for
     * individual files. When we are about to print the first file's state we must print out the
     * wiredTigerCache header once only and add a correct closing brace when we finish the files
     * section. To do this we maintain a flag variable to note when we have printed the first file.
     * Unfortunately, the mechanism which we use to print state for each file does not allow passing
     * of variables by reference, this necessitates the use of a variable on the connection. The
     * variable is safe as the cache log logic is only performed by the single threaded cache log
     * server.
     */
    conn->cache_log.json_tables = false;
    WT_RET(__wt_fprintf(session, conn->cache_log.fs, "{\"version\":\"%s\",\"localTime\":\"%s\"",
      WIREDTIGER_VERSION_STRING, conn->cache_log.stamp));
    return (0);
}

/*
 * __cachelog_print_table_name --
 *     Write the header for the wiredTigerCache section of cache log if running in JSON mode and the
 *     header has not been written this round, then print the name of the file.
 */
static int
__cachelog_print_table_name(WT_SESSION_IMPL *session, const char *name)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON))
        return (0);

    /*
     * If this is the first file we are printing state for print the header for the wiredTigerCache
     * section. Otherwise print a comma as this is a subsequent file.
     */
    if (conn->cache_log.json_tables)
        WT_RET(__wt_fprintf(session, conn->cache_log.fs, ","));
    else {
        conn->cache_log.json_tables = true;
        WT_RET(__wt_fprintf(session, conn->cache_log.fs, ",\"wiredTigerCache\":{"));
    }
    /*
     * Each file's state is an array of per-page objects; pages have no unique identifier and
     * several pages share a footprint, so object member names would collide.
     */
    WT_RET(__wt_fprintf(session, conn->cache_log.fs, "\"%s\":[", name));
    return (0);
}

/*
 * __cachelog_print_footer --
 *     Write the footer for cache log when running in JSON mode.
 */
static int
__cachelog_print_footer(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON))
        return (0);

    /* If we have printed a file's state, then close that section. */
    if (conn->cache_log.json_tables) {
        WT_RET(__wt_fprintf(session, conn->cache_log.fs, "}"));
        conn->cache_log.json_tables = false;
    }
    WT_RET(__wt_fprintf(session, conn->cache_log.fs, "}\n"));
    return (0);
}

/*
 * __cachelog_dump --
 *     Dump the cache state for a file.
 */
static int
__cachelog_dump(WT_SESSION_IMPL *session, const char *name)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(cfg_str);
    WT_DECL_ITEM(uri_buf);
    WT_DECL_RET;
    uint64_t cache_bytes, dirty_bytes, dirty, page_size, page_type, pin_reason, update_bytes,
      read_gen;
    bool first;
    const char *cfg[3] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL};
    const char *uri;

    conn = S2C(session);
    cursor = NULL;

    WT_ERR(__wt_scr_alloc(session, 0, &uri_buf));

    /* Build URI and configuration string. */
    WT_ERR(__wt_buf_fmt(session, uri_buf, "cache:file:%s", name));
    uri = uri_buf->data;

    /* Limit the walk to a sample of the file's pages when configured. */
    if (conn->cache_log.sample != 0) {
        WT_ERR(__wt_scr_alloc(session, 0, &cfg_str));
        WT_ERR(__wt_buf_fmt(
          session, cfg_str, "next_random_sample_size=%" PRIu64, conn->cache_log.sample));
        cfg[1] = cfg_str->data;
    }

    /*
     * Open the cache cursor and dump the cache state.
     *
     * If we don't find an underlying object, silently ignore it, the object may exist only
     * intermittently.
     */
    if ((ret = __wt_curcache_open(session, uri, cfg, &cursor)) != 0) {
        if (ret == EBUSY || ret == ENOENT || ret == WT_NOTFOUND)
            ret = 0;
        goto err;
    }

    WT_ERR(__cachelog_print_table_name(session, name));
    first = true;
    while ((ret = cursor->next(cursor)) == 0) {
        const char *key;

        WT_ERR(cursor->get_key(cursor, &key));
        WT_ERR(cursor->get_value(cursor, &dirty, &cache_bytes, &dirty_bytes, &update_bytes,
          &read_gen, &page_type, &pin_reason));
        if (FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON)) {
            page_size = (uint64_t)strtoull(key + strlen("page:"), NULL, 10);
            WT_ERR(__wt_fprintf(session, conn->cache_log.fs,
              "%s{\"size\":%" PRIu64 ",\"dirty\":%" PRIu64 ",\"bytes_in_cache\":%" PRIu64
              ",\"dirty_bytes\":%" PRIu64 ",\"update_bytes\":%" PRIu64 ",\"read_gen\":%" PRIu64
              ",\"page_type\":%" PRIu64 ",\"pin_reason\":%" PRIu64 "}",
              first ? "" : ",", page_size, dirty, cache_bytes, dirty_bytes, update_bytes, read_gen,
              page_type, pin_reason));
            first = false;
        } else
            WT_ERR(__wt_fprintf(session, conn->cache_log.fs,
              "%s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
              " %s\n",
              conn->cache_log.stamp, dirty, cache_bytes, dirty_bytes, update_bytes, read_gen,
              page_type, pin_reason, name));
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    if (FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_JSON))
        WT_ERR(__wt_fprintf(session, conn->cache_log.fs, "]"));

err:
    __wt_scr_free(session, &cfg_str);
    __wt_scr_free(session, &uri_buf);
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __cachelog_log_one --
 *     Output a set of cache state into the current log file.
 */
static int
__cachelog_log_one(WT_SESSION_IMPL *session, WT_ITEM *path, WT_ITEM *tmp)
{
    struct timespec ts;
    struct tm localt;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};
    const char *uri;

    conn = S2C(session);

    /* Get the current local time of day. */
    __wt_epoch(session, &ts);
    WT_RET(__wt_localtime(session, &ts.tv_sec, &localt));

    /* Create the logging path name for this time of day. */
    if (strftime(tmp->mem, tmp->memsize, conn->cache_log.path, &localt) == 0)
        WT_RET_MSG(session, ENOMEM, "strftime path conversion");

    /* If the path has changed, cycle the log file. */
    if (conn->cache_log.fs == NULL || path == NULL || strcmp(tmp->mem, path->mem) != 0) {
        WT_RET(__wt_fclose(session, &conn->cache_log.fs));
        WT_RET(__wt_fopen(session, tmp->mem, WT_FS_OPEN_CREATE | WT_FS_OPEN_FIXED, WT_STREAM_APPEND,
          &conn->cache_log.fs));

        if (path != NULL)
            WT_RET(__wt_buf_setstr(session, path, tmp->mem));
    }

    /* Create the entry prefix for this time of day. */
    if (strftime(tmp->mem, tmp->memsize, conn->cache_log.format, &localt) == 0)
        WT_RET_MSG(session, ENOMEM, "strftime timestamp conversion");
    conn->cache_log.stamp = tmp->mem;
    WT_RET(__cachelog_print_header(session));

    /*
     * Open a cache cursor and iterate over all files in cache, dumping their state.
     */
    uri = "cache:";
    if ((ret = __wt_curcache_open(session, uri, cfg, &cursor)) != 0) {
        if (ret == EBUSY || ret == ENOENT || ret == WT_NOTFOUND)
            ret = 0;
        goto err;
    }

    while ((ret = cursor->next(cursor)) == 0) {
        const char *file_name;
        WT_ERR(cursor->get_key(cursor, &file_name));
        /* The key is a full "cache:file:<filename>" URI; dump takes the bare filename. */
        WT_ERR(__cachelog_dump(session, file_name + strlen("cache:file:")));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    WT_ERR(__cachelog_print_footer(session));

    /* Flush. */
    WT_ERR(__wt_fflush(session, conn->cache_log.fs));

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __cachelog_on_close --
 *     Log a set of cache state at close. Requires the server is not currently running.
 */
static int
__cachelog_on_close(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    conn = S2C(session);

    if (!FLD_ISSET(conn->cache_log_flags, WT_CACHELOG_ON_CLOSE))
        return (0);

    if (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_CACHE_LOG))
        WT_RET_MSG(session, EINVAL, "Attempt to log cache state while a server is running");

    WT_RET(__wt_scr_alloc(session, strlen(conn->cache_log.path) + 128, &tmp));
    WT_ERR(__wt_buf_setstr(session, tmp, ""));
    WT_ERR(__cachelog_log_one(session, NULL, tmp));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __cachelog_server_run_chk --
 *     Check to decide if the cache log server should continue running.
 */
static bool
__cachelog_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CACHE_LOG));
}

/*
 * __cachelog_server --
 *     The cache log server thread.
 */
static WT_THREAD_RET
__cachelog_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;

    session = arg;
    conn = S2C(session);

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    /*
     * We need a temporary place to build a path and an entry prefix. The length of the path plus
     * 128 should be more than enough.
     *
     * We also need a place to store the current path, because that's how we know when to
     * close/re-open the file.
     */
    WT_ERR(__wt_buf_init(session, &path, strlen(conn->cache_log.path) + 128));
    WT_ERR(__wt_buf_setstr(session, &path, ""));
    WT_ERR(__wt_buf_init(session, &tmp, strlen(conn->cache_log.path) + 128));
    WT_ERR(__wt_buf_setstr(session, &tmp, ""));

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait(
          session, conn->cache_log.cond, conn->cache_log.usecs, __cachelog_server_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__cachelog_server_run_chk(session))
            break;

        if (WT_STAT_ENABLED(session))
            WT_ERR(__cachelog_log_one(session, &path, &tmp));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "cache log server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}

/*
 * __cachelog_start --
 *     Start the cache log server thread.
 */
static int
__cachelog_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    /* Nothing to do if the server is already running. */
    if (conn->cache_log.session != NULL)
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_CACHE_LOG);

    /* The cache log server gets its own session. */
    WT_RET(
      __wt_open_internal_session(conn, "cachelog-server", true, 0, 0, &conn->cache_log.session));
    session = conn->cache_log.session;

    WT_RET(__wt_cond_alloc(session, "cache log server", &conn->cache_log.cond));

    /*
     * Start the thread.
     *
     * Cache logging creates a thread per database, rather than using a single thread to do logging
     * for all of the databases. If we ever see lots of databases at a time, doing cache logging,
     * and we want to reduce the number of threads, there's no reason we have to have more than one
     * thread, I just didn't feel like writing the code to figure out the scheduling.
     */
    WT_RET(__wt_thread_create(session, &conn->cache_log.tid, __cachelog_server, session));
    conn->cache_log.tid_set = true;

    return (0);
}

/*
 * __wti_cachelog_create --
 *     Start the cache log server thread.
 */
int
__wti_cachelog_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    bool start;

    conn = S2C(session);

    /* Readonly systems don't configure cache logging. */
    if (F_ISSET(conn, WT_CONN_READONLY))
        return (0);

    /*
     * Stop any server that is already running. This means that each time reconfigure is called
     * we'll bounce the server even if there are no configuration changes. This makes our life
     * easier as the underlying configuration routine doesn't have to worry about freeing objects in
     * the connection structure (it's guaranteed to always start with a blank slate), and we don't
     * have to worry about races where a running server is reading configuration information that
     * we're updating, and it's not expected that reconfiguration will happen a lot.
     *
     * If there's no server running, discard any configuration information so we don't leak memory
     * during reconfiguration.
     */
    if (conn->cache_log.session == NULL)
        WT_RET(__cachelog_config_discard(session));
    else
        WT_RET(__wti_cachelog_destroy(session, false));

    WT_RET(__cachelog_config(session, cfg, &start));
    if (start)
        WT_RET(__cachelog_start(conn));

    return (0);
}

/*
 * __wti_cachelog_destroy --
 *     Destroy the cache log server thread.
 */
int
__wti_cachelog_destroy(WT_SESSION_IMPL *session, bool is_close)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Readonly systems don't configure cache logging. */
    if (F_ISSET(conn, WT_CONN_READONLY))
        return (0);

    /* Stop the server thread. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CACHE_LOG);
    if (conn->cache_log.tid_set) {
        __wt_cond_signal(session, conn->cache_log.cond);
        WT_TRET(__wt_thread_join(session, &conn->cache_log.tid));
        conn->cache_log.tid_set = false;
    }
    __wt_cond_destroy(session, &conn->cache_log.cond);

    /* Log a set of cache state on shutdown if configured. */
    if (is_close)
        WT_TRET(__cachelog_on_close(session));

    /* Discard all configuration information. */
    WT_TRET(__cachelog_config_discard(session));

    /* Close the server thread's session. */
    if (conn->cache_log.session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->cache_log.session));
        conn->cache_log.session = NULL;
    }

    return (ret);
}
