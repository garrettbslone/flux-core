/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/time.h>
#include <czmq.h>
#include <flux/core.h>

#include "src/common/libutil/blobref.h"
#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"
#include "src/common/libutil/monotime.h"
#include "src/common/libutil/tstat.h"
#include "src/common/libutil/log.h"
#include "src/common/libkvs/proto.h"
#include "src/common/libkvs/json_dirent.h"

#include "waitqueue.h"
#include "cache.h"
#include "json_util.h"

#include "lookup.h"
#include "fence.h"
#include "types.h"
#include "commit.h"

#define KVS_MAGIC 0xdeadbeef

/* Expire cache_entry after 'max_lastuse_age' heartbeats.
 */
const int max_lastuse_age = 5;

/* Include root directory in kvs.setroot event.
 */
const bool event_includes_rootdir = true;

typedef struct {
    int magic;
    struct cache *cache;    /* blobref => cache_entry */
    href_t rootdir;         /* current root blobref */
    int rootseq;            /* current root version (for ordering) */
    commit_mgr_t *cm;
    waitqueue_t *watchlist;
    int watchlist_lastrun_epoch;
    int faults;                 /* for kvs.stats.get, etc. */
    flux_t *h;
    uint32_t rank;
    int epoch;              /* tracks current heartbeat epoch */
    struct json_tokener *tok;
    flux_watcher_t *prep_w;
    flux_watcher_t *idle_w;
    flux_watcher_t *check_w;
    int commit_merge;
    const char *hash_name;
} kvs_ctx_t;

static int setroot_event_send (kvs_ctx_t *ctx, json_object *names);
static int error_event_send (kvs_ctx_t *ctx, json_object *names, int errnum);
static void commit_prep_cb (flux_reactor_t *r, flux_watcher_t *w,
                            int revents, void *arg);
static void commit_check_cb (flux_reactor_t *r, flux_watcher_t *w,
                             int revents, void *arg);

static void freectx (void *arg)
{
    kvs_ctx_t *ctx = arg;
    if (ctx) {
        cache_destroy (ctx->cache);
        commit_mgr_destroy (ctx->cm);
        if (ctx->watchlist)
            wait_queue_destroy (ctx->watchlist);
        if (ctx->tok)
            json_tokener_free (ctx->tok);
        flux_watcher_destroy (ctx->prep_w);
        flux_watcher_destroy (ctx->check_w);
        flux_watcher_destroy (ctx->idle_w);
        free (ctx);
    }
}

static kvs_ctx_t *getctx (flux_t *h)
{
    kvs_ctx_t *ctx = (kvs_ctx_t *)flux_aux_get (h, "kvssrv");
    flux_reactor_t *r;

    if (!ctx) {
        if (!(ctx = calloc (1, sizeof (*ctx)))) {
            errno = ENOMEM;
            goto error;
        }
        ctx->magic = KVS_MAGIC;
        if (!(r = flux_get_reactor (h)))
            goto error;
        if (!(ctx->hash_name = flux_attr_get (h, "content.hash", NULL))) {
            flux_log_error (h, "content.hash");
            goto error;
        }
        ctx->cache = cache_create ();
        ctx->watchlist = wait_queue_create ();
        ctx->cm = commit_mgr_create (ctx->cache, ctx->hash_name, ctx);
        if (!ctx->cache || !ctx->watchlist || !ctx->cm) {
            errno = ENOMEM;
            goto error;
        }
        ctx->h = h;
        if (flux_get_rank (h, &ctx->rank) < 0)
            goto error;
        if (!(ctx->tok = json_tokener_new ())) {
            errno = ENOMEM;
            goto error;
        }
        if (ctx->rank == 0) {
            ctx->prep_w = flux_prepare_watcher_create (r, commit_prep_cb, ctx);
            ctx->check_w = flux_check_watcher_create (r, commit_check_cb, ctx);
            ctx->idle_w = flux_idle_watcher_create (r, NULL, NULL);
            if (!ctx->prep_w || !ctx->check_w || !ctx->idle_w)
                goto error;
            flux_watcher_start (ctx->prep_w);
            flux_watcher_start (ctx->check_w);
        }
        ctx->commit_merge = 1;
        flux_aux_set (h, "kvssrv", ctx, freectx);
    }
    return ctx;
error:
    freectx (ctx);
    return NULL;
}

static void content_load_completion (flux_future_t *f, void *arg)
{
    kvs_ctx_t *ctx = arg;
    json_object *o;
    const void *data;
    int size;
    const char *blobref;
    struct cache_entry *hp;

    if (flux_rpc_get_raw (f, &data, &size) < 0) {
        flux_log_error (ctx->h, "%s", __FUNCTION__);
        goto done;
    }
    blobref = flux_future_aux_get (f, "ref");
    if (!(o = json_tokener_parse_ex (ctx->tok, (char *)data, size))) {
        errno = EPROTO;
        flux_log_error (ctx->h, "%s", __FUNCTION__);
        json_tokener_reset (ctx->tok);
        goto done;
    }
    /* cache entry must have be created earlier.
     * cache_expire_entries() could not have removed it b/c it is not
     * yet valid.
     */
    assert ((hp = cache_lookup (ctx->cache, blobref, ctx->epoch)));
    cache_entry_set_json (hp, o);
done:
    flux_future_destroy (f);
}

/* If now is true, perform the load rpc synchronously;
 * otherwise arrange for a continuation to handle the response.
 */
static int content_load_request_send (kvs_ctx_t *ctx, const href_t ref, bool now)
{
    flux_future_t *f = NULL;

    //flux_log (ctx->h, LOG_DEBUG, "%s: %s", __FUNCTION__, ref);
    if (!(f = flux_rpc_raw (ctx->h, "content.load",
                    ref, strlen (ref) + 1, FLUX_NODEID_ANY, 0)))
        goto error;
    if (flux_future_aux_set (f, "ref", xstrdup (ref), free) < 0)
        goto error;
    if (now) {
        content_load_completion (f, ctx);
    } else if (flux_future_then (f, -1., content_load_completion, ctx) < 0) {
        flux_future_destroy (f);
        goto error;
    }
    return 0;
error:
    return -1;
}

/* Return true if load successful, false if stalling */
static bool load (kvs_ctx_t *ctx, const href_t ref, wait_t *wait, json_object **op)
{
    struct cache_entry *hp = cache_lookup (ctx->cache, ref, ctx->epoch);

    /* Create an incomplete hash entry if none found.
     */
    if (!hp) {
        hp = cache_entry_create (NULL);
        cache_insert (ctx->cache, ref, hp);
        if (content_load_request_send (ctx, ref, wait ? false : true) < 0)
            flux_log_error (ctx->h, "content_load_request_send");
        ctx->faults++;
    }
    /* If hash entry is incomplete (either created above or earlier),
     * arrange to stall caller if wait_t was provided.
     */
    if (!cache_entry_get_valid (hp)) {
        cache_entry_wait_valid (hp, wait);
        return false;
    }

    if (op)
        *op = cache_entry_get_json (hp);
    return true;
}

static int content_store_get (flux_future_t *f, void *arg)
{
    kvs_ctx_t *ctx = arg;
    struct cache_entry *hp;
    const char *blobref;
    int blobref_size;
    int rc = -1;

    if (flux_rpc_get_raw (f, &blobref, &blobref_size) < 0) {
        flux_log_error (ctx->h, "%s", __FUNCTION__);
        goto done;
    }
    if (!blobref || blobref[blobref_size - 1] != '\0') {
        errno = EPROTO;
        flux_log_error (ctx->h, "%s", __FUNCTION__);
        goto done;
    }
    //flux_log (ctx->h, LOG_DEBUG, "%s: %s", __FUNCTION__, ref);
    hp = cache_lookup (ctx->cache, blobref, ctx->epoch);
    cache_entry_set_dirty (hp, false);
    rc = 0;
done:
    flux_future_destroy (f);
    return rc;
}

static void content_store_completion (flux_future_t *f, void *arg)
{
    (void)content_store_get (f, arg);
}

static int content_store_request_send (kvs_ctx_t *ctx, json_object *val,
                                       bool now)
{
    flux_future_t *f;
    const char *data = Jtostr (val);
    int size = strlen (data) + 1;

    if (!(f = flux_rpc_raw (ctx->h, "content.store",
                            data, size, FLUX_NODEID_ANY, 0)))
        goto error;
    if (now) {
        if (content_store_get (f, ctx) < 0)
            goto error;
    } else if (flux_future_then (f, -1., content_store_completion, ctx) < 0) {
        flux_future_destroy (f);
        goto error;
    }
    return 0;
error:
    return -1;
}

static void setroot (kvs_ctx_t *ctx, const char *rootdir, int rootseq)
{
    if (rootseq == 0 || rootseq > ctx->rootseq) {
        assert (strlen (rootdir) < sizeof (href_t));
        strcpy (ctx->rootdir, rootdir);
        ctx->rootseq = rootseq;
        wait_runqueue (ctx->watchlist);
        ctx->watchlist_lastrun_epoch = ctx->epoch;
    }
}

struct commit_cb_data {
    kvs_ctx_t *ctx;
    wait_t *wait;
};

static int commit_load_cb (commit_t *c, const char *ref, void *data)
{
    struct commit_cb_data *cbd = data;

    assert (!load (cbd->ctx, ref, cbd->wait, NULL));
    return 0;
}

/* Flush to content cache asynchronously and push wait onto cache
 * object's wait queue.  FIXME: asynchronous errors need to be
 * propagated back to caller.
 */
static int commit_cache_cb (commit_t *c, struct cache_entry *hp, void *data)
{
    struct commit_cb_data *cbd = data;

    if (cache_entry_get_content_store_flag (hp)) {
        if (content_store_request_send (cbd->ctx,
                                        cache_entry_get_json (hp),
                                        false) < 0) {
            flux_log_error (cbd->ctx->h, "content_store");
            return -1;
        }
        cache_entry_set_content_store_flag (hp, false);
    }
    cache_entry_wait_notdirty (hp, cbd->wait);
    return 0;
}

/* Commit all the ops for a particular commit/fence request (rank 0 only).
 * The setroot event will cause responses to be sent to the fence requests
 * and clean up the fence_t state.  This function is idempotent.
 */
static void commit_apply (commit_t *c)
{
    kvs_ctx_t *ctx = commit_get_aux (c);
    wait_t *wait = NULL;
    int errnum = 0;
    commit_process_t ret;

    if ((ret = commit_process (c,
                               ctx->epoch,
                               ctx->rootdir)) == COMMIT_PROCESS_ERROR) {
        errnum = commit_get_errnum (c);
        goto done;
    }

    if (ret == COMMIT_PROCESS_LOAD_MISSING_REFS) {
        struct commit_cb_data cbd;

        if (!(wait = wait_create ((wait_cb_f)commit_apply, c))) {
            errnum = errno;
            goto done;
        }

        cbd.ctx = ctx;
        cbd.wait = wait;

        if (commit_iter_missing_refs (c, commit_load_cb, &cbd) < 0) {
            errnum = errno;
            goto done;
        }

        assert (wait_get_usecount (wait) > 0);
        goto stall;
    }
    else if (ret == COMMIT_PROCESS_DIRTY_CACHE_ENTRIES) {
        struct commit_cb_data cbd;

        if (!(wait = wait_create ((wait_cb_f)commit_apply, c))) {
            errnum = errno;
            goto done;
        }

        cbd.ctx = ctx;
        cbd.wait = wait;

        if (commit_iter_dirty_cache_entries (c, commit_cache_cb, &cbd) < 0) {
            errnum = errno;
            goto done;
        }

        assert (wait_get_usecount (wait) > 0);
        goto stall;
    }
    /* else ret == COMMIT_PROCESS_FINISHED */

    /* This is the transaction that finalizes the commit by replacing
     * ctx->rootdir with newroot, incrementing the root seq,
     * and sending out the setroot event for "eventual consistency"
     * of other nodes.
     */
done:
    if (errnum == 0) {
        fence_t *f = commit_get_fence (c);
        int count;
        if (Jget_ar_len (fence_get_json_names (f), &count) && count > 1) {
            int opcount = 0;
            (void)Jget_ar_len (fence_get_json_ops (f), &opcount);
            flux_log (ctx->h, LOG_DEBUG, "aggregated %d commits (%d ops)",
                      count, opcount);
        }
        setroot (ctx, commit_get_newroot_ref (c), ctx->rootseq + 1);
        setroot_event_send (ctx, fence_get_json_names (f));
    } else {
        fence_t *f = commit_get_fence (c);
        flux_log (ctx->h, LOG_ERR, "commit failed: %s",
                  flux_strerror (errnum));
        error_event_send (ctx, fence_get_json_names (f), errnum);
    }
    wait_destroy (wait);

    /* Completed: remove from 'ready' list.
     * N.B. fence_t remains in the fences hash until event is received.
     */
    commit_mgr_remove_commit (ctx->cm, c);
    return;

stall:
    return;
}

static void commit_prep_cb (flux_reactor_t *r, flux_watcher_t *w,
                            int revents, void *arg)
{
    kvs_ctx_t *ctx = arg;

    if (commit_mgr_commits_ready (ctx->cm))
        flux_watcher_start (ctx->idle_w);
}

static void commit_check_cb (flux_reactor_t *r, flux_watcher_t *w,
                             int revents, void *arg)
{
    kvs_ctx_t *ctx = arg;
    commit_t *c;

    flux_watcher_stop (ctx->idle_w);

    if ((c = commit_mgr_get_ready_commit (ctx->cm))) {
        if (ctx->commit_merge)
            commit_mgr_merge_ready_commits (ctx->cm);
        commit_apply (c);
    }
}

static void dropcache_request_cb (flux_t *h, flux_msg_handler_t *w,
                                  const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    int size, expcount = 0;
    int rc = -1;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        goto done;
    size = cache_count_entries (ctx->cache);
    expcount = cache_expire_entries (ctx->cache, ctx->epoch, 0);
    flux_log (h, LOG_ALERT, "dropped %d of %d cache entries", expcount, size);
    rc = 0;
done:
    if (flux_respond (h, msg, rc < 0 ? errno : 0, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static void dropcache_event_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    int size, expcount = 0;

    if (flux_event_decode (msg, NULL, NULL) < 0)
        return;
    size = cache_count_entries (ctx->cache);
    expcount = cache_expire_entries (ctx->cache, ctx->epoch, 0);
    flux_log (h, LOG_ALERT, "dropped %d of %d cache entries", expcount, size);
}

static void heartbeat_cb (flux_t *h, flux_msg_handler_t *w,
                          const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;

    if (flux_heartbeat_decode (msg, &ctx->epoch) < 0) {
        flux_log_error (ctx->h, "%s", __FUNCTION__);
        return;
    }
    /* "touch" objects involved in watched keys */
    if (ctx->epoch - ctx->watchlist_lastrun_epoch > max_lastuse_age) {
        wait_runqueue (ctx->watchlist);
        ctx->watchlist_lastrun_epoch = ctx->epoch;
    }
    /* "touch" root */
    (void)load (ctx, ctx->rootdir, NULL, NULL);

    cache_expire_entries (ctx->cache, ctx->epoch, max_lastuse_age);
}

static void get_request_cb (flux_t *h, flux_msg_handler_t *w,
                            const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = NULL;
    const char *json_str;
    json_object *in = NULL;
    json_object *out = NULL;
    int flags;
    const char *key;
    json_object *val = NULL;
    json_object *root_dirent = NULL;
    json_object *tmp_dirent = NULL;
    lookup_t *lh = NULL;
    const char *root_ref = NULL;
    wait_t *wait = NULL;
    int rc = -1;

    /* if bad lh, then first time rpc and not a replay */
    if (lookup_validate (arg) == false) {
        ctx = arg;

        if (flux_request_decode (msg, NULL, &json_str) < 0)
            goto done;
        if (!json_str || !(in = Jfromstr (json_str))) {
            errno = EPROTO;
            goto done;
        }
        if (kp_tget_dec (in, &root_dirent, &key, &flags) < 0)
            goto done;
        /* If root dirent was specified, lookup corresponding 'root' directory.
         * Otherwise, use the current root.
         */
        if (root_dirent) {
            if (dirent_validate (root_dirent) < 0
                            || !Jget_str (root_dirent, "DIRREF", &root_ref)) {
                errno = EINVAL;
                goto done;
            }
        }

        if (!(lh = lookup_create (ctx->cache,
                                  ctx->epoch,
                                  ctx->rootdir,
                                  root_ref,
                                  key,
                                  flags)))
            goto done;

        assert (lookup_set_aux_data (lh, ctx) == 0);
    }
    else {
        lh = arg;

        assert ((ctx = lookup_get_aux_data (lh)));

        assert (lookup_set_current_epoch (lh, ctx->epoch) == 0);
    }

    if (!lookup (lh)) {
        const char *missing_ref;

        missing_ref = lookup_get_missing_ref (lh);
        assert (missing_ref);

        if (!(wait = wait_create_msg_handler (h, w, msg, get_request_cb, lh)))
            goto done;
        if (load (ctx, missing_ref, wait, NULL))
            log_msg_exit ("%s: failure in load logic", __FUNCTION__);
        goto stall;
    }
    if (lookup_get_errnum (lh) != 0) {
        errno = lookup_get_errnum (lh);
        goto done;
    }
    if ((val = lookup_get_value (lh)) == NULL) {
        errno = ENOENT;
        goto done;
    }

    if (!root_dirent) {
        tmp_dirent = dirent_create ("DIRREF", (char *)lookup_get_root_ref (lh));
        root_dirent = tmp_dirent;
    }

    /* ownership of val passed to 'out' */
    if (!(out = kp_rget_enc (Jget (root_dirent), val)))
        goto done;

    rc = 0;
done:
    if (flux_respond (h, msg, rc < 0 ? errno : 0,
                              rc < 0 ? NULL : Jtostr (out)) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    wait_destroy (wait);
    lookup_destroy (lh);
stall:
    Jput (in);
    Jput (out);
    Jput (tmp_dirent);
}

static void watch_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = NULL;
    const char *json_str;
    json_object *in = NULL;
    json_object *in2 = NULL;
    json_object *out = NULL;
    json_object *oval = NULL;
    json_object *val = NULL;
    flux_msg_t *cpy = NULL;
    const char *key;
    int flags;
    lookup_t *lh = NULL;
    wait_t *wait = NULL;
    wait_t *watcher = NULL;
    bool isreplay = false;
    int rc = -1;

    /* if bad lh, then first time rpc and not a replay */
    if (lookup_validate (arg) == false) {
        ctx = arg;

        if (flux_request_decode (msg, NULL, &json_str) < 0)
            goto done;
        if (!json_str || !(in = Jfromstr (json_str))) {
            errno = EPROTO;
            goto done;
        }
        if (kp_twatch_dec (in, &key, &oval, &flags) < 0)
            goto done;

        if (!(lh = lookup_create (ctx->cache,
                                  ctx->epoch,
                                  ctx->rootdir,
                                  NULL,
                                  key,
                                  flags)))
            goto done;

        assert (lookup_set_aux_data (lh, ctx) == 0);
    }
    else {
        lh = arg;

        assert ((ctx = lookup_get_aux_data (lh)));

        assert (lookup_set_current_epoch (lh, ctx->epoch) == 0);

        isreplay = true;
    }

    if (!lookup (lh)) {
        const char *missing_ref;

        missing_ref = lookup_get_missing_ref (lh);
        assert (missing_ref);

        if (!(wait = wait_create_msg_handler (h, w, msg, watch_request_cb, lh)))
            goto done;
        if (load (ctx, missing_ref, wait, NULL))
            log_msg_exit ("%s: failure in load logic", __FUNCTION__);
        goto stall;
    }
    if (lookup_get_errnum (lh) != 0) {
        errno = lookup_get_errnum (lh);
        goto done;
    }
    val = lookup_get_value (lh);

    /* we didn't initialize these values on a replay, get them */
    if (isreplay) {
        if (flux_request_decode (msg, NULL, &json_str) < 0)
            goto done;
        if (!json_str || !(in = Jfromstr (json_str))) {
            errno = EPROTO;
            goto done;
        }
        if (kp_twatch_dec (in, &key, &oval, &flags) < 0)
            goto done;
    }

    /* Value changed or this is the initial request, so prepare a reply.
     */
    if ((flags & KVS_PROTO_FIRST) || !json_compare (val, oval)) {
        if (!(out = kp_rwatch_enc (Jget (val))))
            goto done;
    }
    /* No reply sent or this is a multi-response watch request.
     * Arrange to wait on ctx->watchlist for each new commit.
     * Reconstruct the payload with 'first' flag clear, and updated value.
     */
    if (!out || !(flags & KVS_PROTO_ONCE)) {
        if (!(cpy = flux_msg_copy (msg, false)))
            goto done;
        if (!(in2 = kp_twatch_enc (key,
                                   Jget (val),
                                   flags & ~KVS_PROTO_FIRST)))
            goto done;
        if (flux_msg_set_json (cpy, Jtostr (in2)) < 0)
            goto done;
        if (!(watcher = wait_create_msg_handler (h, w, cpy,
                                                 watch_request_cb, ctx)))
            goto done;
        wait_addqueue (ctx->watchlist, watcher);
    }
    rc = 0;
done:
    if (rc < 0 || out != NULL) {
        if (flux_respond (h, msg, rc < 0 ? errno : 0,
                                  rc < 0 ? NULL : Jtostr (out)) < 0)
            flux_log_error (h, "%s", __FUNCTION__);
    }
    wait_destroy (wait);
    lookup_destroy (lh);
stall:
    Jput (in);
    Jput (in2);
    Jput (out);
    flux_msg_destroy (cpy);
    Jput (val);
}

typedef struct {
    const char *key;
    char *sender;
} unwatch_param_t;

static bool unwatch_cmp (const flux_msg_t *msg, void *arg)
{
    unwatch_param_t *p = arg;
    char *sender = NULL;
    json_object *o = NULL;
    json_object *val;
    const char *key, *topic, *json_str;
    int flags;
    bool match = false;

    if (flux_request_decode (msg, &topic, &json_str) < 0)
        goto done;
    if (strcmp (topic, "kvs.watch") != 0)
        goto done;
    if (flux_msg_get_route_first (msg, &sender) < 0)
        goto done;
    if (strcmp (sender, p->sender) != 0)
        goto done;
    if (!json_str || !(o = Jfromstr (json_str)))
        goto done;
    if (kp_twatch_dec (o, &key, &val, &flags) <  0)
        goto done;
    if (strcmp (p->key, key) != 0)
        goto done;
    match = true;
done:
    if (sender)
        free (sender);
    Jput (o);
    return match;
}

static void unwatch_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    const char *json_str;
    json_object *in = NULL;
    unwatch_param_t p = { NULL, NULL };
    int rc = -1;

    if (flux_request_decode (msg, NULL, &json_str) < 0)
        goto done;
    if (!json_str || !(in = Jfromstr (json_str))) {
        errno = EPROTO;
        goto done;
    }
    if (kp_tunwatch_dec (in, &p.key) < 0)
        goto done;
    if (flux_msg_get_route_first (msg, &p.sender) < 0)
        goto done;
    if (wait_destroy_msg (ctx->watchlist, unwatch_cmp, &p) < 0)
        goto done;
    if (cache_wait_destroy_msg (ctx->cache, unwatch_cmp, &p) < 0)
        goto done;
    rc = 0;
done:
    if (flux_respond (h, msg, rc < 0 ? errno : 0, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    Jput (in);
    if (p.sender)
        free (p.sender);
}

struct finalize_data {
    kvs_ctx_t *ctx;
    int errnum;
};

static int finalize_fence_req (fence_t *f, const flux_msg_t *req, void *data)
{
    struct finalize_data *d = data;

    if (flux_respond (d->ctx->h, req, d->errnum, NULL) < 0)
        flux_log_error (d->ctx->h, "%s", __FUNCTION__);

    return 0;
}

static void finalize_fences_bynames (kvs_ctx_t *ctx, json_object *names, int errnum)
{
    int i, len;
    const char *name;
    fence_t *f;
    struct finalize_data d = { .ctx = ctx, .errnum = errnum };

    if (!Jget_ar_len (names, &len)) {
        flux_log_error (ctx->h, "%s: parsing array", __FUNCTION__);
        return;
    }
    for (i = 0; i < len; i++) {
        if (!Jget_ar_str (names, i, &name)) {
            flux_log_error (ctx->h, "%s: parsing array[%d]", __FUNCTION__, i);
            return;
        }
        if ((f = commit_mgr_lookup_fence (ctx->cm, name))) {
            fence_iter_request_copies (f, finalize_fence_req, &d);
            commit_mgr_remove_fence (ctx->cm, name);
        }
    }
}

/* kvs.relayfence (rank 0 only, no response).
 */
static void relayfence_request_cb (flux_t *h, flux_msg_handler_t *w,
                                   const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    const char *json_str, *name;
    int nprocs, flags;
    json_object *in = NULL;
    json_object *ops = NULL;
    fence_t *f;

    if (flux_request_decode (msg, NULL, &json_str) < 0)
        goto done;
    if (!json_str || !(in = Jfromstr (json_str))) {
        errno = EPROTO;
        goto done;
    }
    if (kp_tfence_dec (in, &name, &nprocs, &flags, &ops) < 0) {
        errno = EPROTO;
        goto done;
    }
    /* FIXME: generate a kvs.fence.abort (or similar) if an error
     * occurs after we know the fence name
     */
    if (!(f = commit_mgr_lookup_fence (ctx->cm, name))) {
        if (!(f = fence_create (name, nprocs, flags)))
            goto done;
        if (commit_mgr_add_fence (ctx->cm, f) < 0) {
            fence_destroy (f);
            goto done;
        }
    }
    else
        fence_set_flags (f, fence_get_flags (f) | flags);

    if (fence_add_request_data (f, ops) < 0)
        goto done;

    if (commit_mgr_process_fence_request (ctx->cm, f) < 0)
        goto done;

done:
    Jput (in);
}

/* kvs.fence
 * Sent from users to local kvs module.
 */
static void fence_request_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    const char *json_str, *name;
    int nprocs, flags;
    json_object *in = NULL;
    json_object *ops = NULL;
    fence_t *f;

    if (flux_request_decode (msg, NULL, &json_str) < 0)
        goto error;
    if (!json_str || !(in = Jfromstr (json_str))) {
        errno = EPROTO;
        goto error;
    }
    if (kp_tfence_dec (in, &name, &nprocs, &flags, &ops) < 0) {
        errno = EPROTO;
        goto error;
    }
    if (!(f = commit_mgr_lookup_fence (ctx->cm, name))) {
        if (!(f = fence_create (name, nprocs, flags)))
            goto error;
        if (commit_mgr_add_fence (ctx->cm, f) < 0) {
            fence_destroy (f);
            goto error;
        }
    }
    else
        fence_set_flags (f, fence_get_flags (f) | flags);

    if (fence_add_request_copy (f, msg) < 0)
        goto error;
    if (ctx->rank == 0) {
        if (fence_add_request_data (f, ops) < 0)
            goto error;

        if (commit_mgr_process_fence_request (ctx->cm, f) < 0)
            goto error;
    }
    else {
        flux_future_t *f = flux_rpc (h, "kvs.relayfence", json_str,
                                     0, FLUX_RPC_NORESPONSE);
        if (!f)
            goto error;
        flux_future_destroy (f);
    }
    Jput (in);
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    Jput (in);
}


/* For wait_version().
 */
static void sync_request_cb (flux_t *h, flux_msg_handler_t *w,
                             const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    int rootseq;
    wait_t *wait = NULL;

    if (flux_request_unpack (msg, NULL, "{ s:i }",
                             "rootseq", &rootseq) < 0)
        goto error;
    if (ctx->rootseq < rootseq) {
        if (!(wait = wait_create_msg_handler (h, w, msg, sync_request_cb, arg)))
            goto error;
        wait_addqueue (ctx->watchlist, wait);
        return; /* stall */
    }
    if (flux_respondf (h, msg, "{ s:i s:s }",
                       "rootseq", ctx->rootseq,
                       "rootdir", ctx->rootdir) < 0)
        goto error;
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static void getroot_request_cb (flux_t *h, flux_msg_handler_t *w,
                                const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        goto error;
    if (flux_respondf (h, msg, "{ s:i s:s }",
                       "rootseq", ctx->rootseq,
                       "rootdir", ctx->rootdir) < 0)
        goto error;
    return;

error:
    if (flux_respond (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s: flux_respond", __FUNCTION__);
}

static int getroot_rpc (kvs_ctx_t *ctx, int *rootseq, href_t rootdir)
{
    flux_future_t *f;
    const char *ref;
    int rc = -1;

    if (!(f = flux_rpc (ctx->h, "kvs.getroot", NULL, FLUX_NODEID_UPSTREAM, 0)))
        goto done;
    if (flux_rpc_get_unpack (f, "{ s:i s:s }",
                             "rootseq", rootseq,
                             "rootdir", &ref) < 0)
        goto done;
    if (strlen (ref) > sizeof (href_t) - 1) {
        errno = EPROTO;
        goto done;
    }
    strcpy (rootdir, ref);
    rc = 0;
done:
    flux_future_destroy (f);
    return rc;
}

static void error_event_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    const char *json_str;
    json_object *out = NULL;
    json_object *names = NULL;
    int errnum;

    if (flux_event_decode (msg, NULL, &json_str) < 0) {
        flux_log_error (ctx->h, "%s: flux_event_decode", __FUNCTION__);
        goto done;
    }
    if (!json_str || !(out = Jfromstr (json_str))) {
        errno = EPROTO;
        flux_log_error (ctx->h, "%s: json_decode", __FUNCTION__);
        goto done;
    }
    if (kp_terror_dec (out, &names, &errnum) < 0) {
        flux_log_error (ctx->h, "%s: kp_terror_dec", __FUNCTION__);
        goto done;
    }
    finalize_fences_bynames (ctx, names, errnum);
done:
    Jput (out);
}

static int error_event_send (kvs_ctx_t *ctx, json_object *names, int errnum)
{
    json_object *in = NULL;
    flux_msg_t *msg = NULL;
    int rc = -1;

    if (!(in = kp_terror_enc (names, errnum)))
        goto done;
    if (!(msg = flux_event_encode ("kvs.error", Jtostr (in))))
        goto done;
    if (flux_msg_set_private (msg) < 0)
        goto done;
    if (flux_send (ctx->h, msg, 0) < 0)
        goto done;
    rc = 0;
done:
    Jput (in);
    flux_msg_destroy (msg);
    return rc;
}

/* Alter the (rootdir, rootseq) in response to a setroot event.
 */
static void setroot_event_cb (flux_t *h, flux_msg_handler_t *w,
                              const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    json_object *out = NULL;
    int rootseq;
    const char *json_str;
    const char *rootdir;
    json_object *root = NULL;
    json_object *names = NULL;

    if (flux_event_decode (msg, NULL, &json_str) < 0) {
        flux_log_error (ctx->h, "%s: flux_event_decode", __FUNCTION__);
        goto done;
    }
    if (!json_str || !(out = Jfromstr (json_str))) {
        errno = EPROTO;
        flux_log_error (ctx->h, "%s: json decode", __FUNCTION__);
        goto done;
    }
    if (kp_tsetroot_dec (out, &rootseq, &rootdir, &root, &names) < 0) {
        flux_log_error (ctx->h, "%s: kp_tsetroot_dec", __FUNCTION__);
        goto done;
    }
    finalize_fences_bynames (ctx, names, 0);
    /* Copy of root object (corresponding to rootdir blobref) was included
     * in the setroot event as an optimization, since it would otherwise
     * be loaded from the content store on next KVS access - immediate
     * if there are watchers.  Store this object in the KVS cache
     * with clear dirty bit as it is already valid in the content store.
     */
    if (root) {
        struct cache_entry *hp;
        if ((hp = cache_lookup (ctx->cache, rootdir, ctx->epoch))) {
            if (!cache_entry_get_valid (hp))
                cache_entry_set_json (hp, Jget (root));
            if (cache_entry_get_dirty (hp))
                cache_entry_set_dirty (hp, false);
        } else {
            hp = cache_entry_create (Jget (root));
            cache_insert (ctx->cache, rootdir, hp);
        }
    }
    setroot (ctx, rootdir, rootseq);
done:
    Jput (out);
}

static int setroot_event_send (kvs_ctx_t *ctx, json_object *names)
{
    json_object *in = NULL;
    json_object *root = NULL;
    flux_msg_t *msg = NULL;
    int rc = -1;

    if (event_includes_rootdir) {
        bool stall = !load (ctx, ctx->rootdir, NULL, &root);
        FASSERT (ctx->h, stall == false);
    }
    if (!(in = kp_tsetroot_enc (ctx->rootseq, ctx->rootdir, root, names)))
        goto done;
    if (!(msg = flux_event_encode ("kvs.setroot", Jtostr (in))))
        goto done;
    if (flux_msg_set_private (msg) < 0)
        goto done;
    if (flux_send (ctx->h, msg, 0) < 0)
        goto done;
    rc = 0;
done:
    Jput (in);
    flux_msg_destroy (msg);
    return rc;
}

static bool disconnect_cmp (const flux_msg_t *msg, void *arg)
{
    char *sender = arg;
    char *s = NULL;
    bool match = false;

    if (flux_msg_get_route_first (msg, &s) == 0 && !strcmp (s, sender))
        match = true;
    if (s)
        free (s);
    return match;
}

static void disconnect_request_cb (flux_t *h, flux_msg_handler_t *w,
                                   const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    char *sender = NULL;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        return;
    if (flux_msg_get_route_first (msg, &sender) < 0)
        return;
    (void)wait_destroy_msg (ctx->watchlist, disconnect_cmp, sender);
    (void)cache_wait_destroy_msg (ctx->cache, disconnect_cmp, sender);
    free (sender);
}

static void add_tstat (json_object *o, const char *name, tstat_t *ts,
                       double scale)
{
    json_object *t = Jnew ();

    Jadd_int (t, "count", tstat_count (ts));
    Jadd_double (t, "min", tstat_min (ts)*scale);
    Jadd_double (t, "mean", tstat_mean (ts)*scale);
    Jadd_double (t, "stddev", tstat_stddev (ts)*scale);
    Jadd_double (t, "max", tstat_max (ts)*scale);

    json_object_object_add (o, name, t);
}

static void stats_get_cb (flux_t *h, flux_msg_handler_t *w,
                          const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    json_object *o = Jnew ();
    tstat_t ts;
    int size, incomplete, dirty;
    int rc = -1;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        goto done;
    memset (&ts, 0, sizeof (ts));
    cache_get_stats (ctx->cache, &ts, &size, &incomplete, &dirty);
    Jadd_double (o, "obj size total (MiB)", (double)size/1048576);
    add_tstat (o, "obj size (KiB)", &ts, 1E-3);
    Jadd_int (o, "#obj dirty", dirty);
    Jadd_int (o, "#obj incomplete", incomplete);
    Jadd_int (o, "#watchers", wait_queue_length (ctx->watchlist));
    Jadd_int (o, "#no-op stores", commit_mgr_get_noop_stores (ctx->cm));
    Jadd_int (o, "#faults", ctx->faults);
    Jadd_int (o, "store revision", ctx->rootseq);
    rc = 0;
done:
    if (flux_respond (h, msg, rc < 0 ? errno : 0,
                              rc < 0 ? NULL : Jtostr (o)) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
    Jput (o);
}

static void stats_clear (kvs_ctx_t *ctx)
{
    ctx->faults = 0;
    commit_mgr_clear_noop_stores (ctx->cm);
}

static void stats_clear_event_cb (flux_t *h, flux_msg_handler_t *w,
                                  const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;
    stats_clear (ctx);
}

static void stats_clear_request_cb (flux_t *h, flux_msg_handler_t *w,
                                    const flux_msg_t *msg, void *arg)
{
    kvs_ctx_t *ctx = arg;

    stats_clear (ctx);

    if (flux_respond (h, msg, 0, NULL) < 0)
        flux_log_error (h, "%s", __FUNCTION__);
}

static struct flux_msg_handler_spec handlers[] = {
    { FLUX_MSGTYPE_REQUEST, "kvs.stats.get",  stats_get_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.stats.clear",stats_clear_request_cb, 0, NULL },
    { FLUX_MSGTYPE_EVENT,   "kvs.stats.clear",stats_clear_event_cb, 0, NULL },
    { FLUX_MSGTYPE_EVENT,   "kvs.setroot",    setroot_event_cb, 0, NULL },
    { FLUX_MSGTYPE_EVENT,   "kvs.error",      error_event_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.getroot",    getroot_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.dropcache",  dropcache_request_cb, 0, NULL },
    { FLUX_MSGTYPE_EVENT,   "kvs.dropcache",  dropcache_event_cb, 0, NULL },
    { FLUX_MSGTYPE_EVENT,   "hb",             heartbeat_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.disconnect", disconnect_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.unwatch",    unwatch_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.sync",       sync_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.get",        get_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.watch",      watch_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.fence",      fence_request_cb, 0, NULL },
    { FLUX_MSGTYPE_REQUEST, "kvs.relayfence", relayfence_request_cb, 0, NULL },
    FLUX_MSGHANDLER_TABLE_END,
};

static void process_args (kvs_ctx_t *ctx, int ac, char **av)
{
    int i;

    for (i = 0; i < ac; i++) {
        if (strncmp (av[i], "commit-merge=", 13) == 0)
            ctx->commit_merge = strtoul (av[i]+13, NULL, 10);
        else
            flux_log (ctx->h, LOG_ERR, "Unknown option `%s'", av[i]);
    }
}

/* Store initial rootdir in local cache, and flush to content
 * cache synchronously.
 */
static int store_initial_rootdir (kvs_ctx_t *ctx, json_object *o, href_t ref)
{
    struct cache_entry *hp;
    int rc = -1;

    if (json_hash (ctx->hash_name, o, ref) < 0) {
        flux_log_error (ctx->h, "json_hash");
        goto done;
    }
    if (!(hp = cache_lookup (ctx->cache, ref, ctx->epoch))) {
        hp = cache_entry_create (NULL);
        cache_insert (ctx->cache, ref, hp);
    }
    if (!cache_entry_get_valid (hp)) {
        cache_entry_set_json (hp, o);
        cache_entry_set_dirty (hp, true);
        if (content_store_request_send (ctx, o, true) < 0) {
            flux_log_error (ctx->h, "content_store");
            goto done;
        }
    } else
        json_object_put (o);
    rc = 0;
done:
    return rc;
}

int mod_main (flux_t *h, int argc, char **argv)
{
    kvs_ctx_t *ctx = getctx (h);
    int rc = -1;

    if (!ctx) {
        flux_log_error (h, "error creating KVS context");
        goto done;
    }
    process_args (ctx, argc, argv);
    if (flux_event_subscribe (h, "hb") < 0) {
        flux_log_error (h, "flux_event_subscribe");
        goto done;
    }
    if (flux_event_subscribe (h, "kvs.") < 0) {
        flux_log_error (h, "flux_event_subscribe");
        goto done;
    }
    if (ctx->rank == 0) {
        json_object *rootdir = Jnew ();
        href_t href;

        if (store_initial_rootdir (ctx, rootdir, href) < 0) {
            flux_log_error (h, "storing root object");
            goto done;
        }
        setroot (ctx, href, 0);
    } else {
        href_t href;
        int rootseq;
        if (getroot_rpc (ctx, &rootseq, href) < 0) {
            flux_log_error (h, "getroot");
            goto done;
        }
        setroot (ctx, href, rootseq);
    }
    if (flux_msg_handler_addvec (h, handlers, ctx) < 0) {
        flux_log_error (h, "flux_msg_handler_addvec");
        goto done;
    }
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0) {
        flux_log_error (h, "flux_reactor_run");
        goto done_delvec;
    }
    rc = 0;
done_delvec:
    flux_msg_handler_delvec (handlers);
done:
    return rc;
}

MOD_NAME ("kvs");

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
