/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <assert.h>
#include <stddef.h>

#include <apr_atomic.h>
#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_connection.h>
#include <http_protocol.h>
#include <http_request.h>
#include <http_log.h>
#include <http_vhost.h>
#include <util_filter.h>
#include <ap_mmn.h>
#include <ap_mpm.h>
#include <mpm_common.h>
#include <mod_core.h>
#include <scoreboard.h>

#include "h2_private.h"
#include "h2.h"
#include "h2_bucket_beam.h"
#include "h2_c1.h"
#include "h2_config.h"
#include "h2_conn_ctx.h"
#include "h2_from_h1.h"
#include "h2_h2.h"
#include "h2_mplx.h"
#include "h2_request.h"
#include "h2_headers.h"
#include "h2_session.h"
#include "h2_stream.h"
#include "h2_c2.h"
#include "h2_util.h"


static h2_mpm_type_t mpm_type = H2_MPM_UNKNOWN;
static module *mpm_module;
static int mpm_supported = 1;
static apr_socket_t *dummy_socket;

static void check_modules(int force)
{
    static int checked = 0;
    int i;

    if (force || !checked) {
        for (i = 0; ap_loaded_modules[i]; ++i) {
            module *m = ap_loaded_modules[i];

            if (!strcmp("event.c", m->name)) {
                mpm_type = H2_MPM_EVENT;
                mpm_module = m;
                break;
            }
            else if (!strcmp("motorz.c", m->name)) {
                mpm_type = H2_MPM_MOTORZ;
                mpm_module = m;
                break;
            }
            else if (!strcmp("mpm_netware.c", m->name)) {
                mpm_type = H2_MPM_NETWARE;
                mpm_module = m;
                break;
            }
            else if (!strcmp("prefork.c", m->name)) {
                mpm_type = H2_MPM_PREFORK;
                mpm_module = m;
                /* While http2 can work really well on prefork, it collides
                 * today's use case for prefork: running single-thread app engines
                 * like php. If we restrict h2_workers to 1 per process, php will
                 * work fine, but browser will be limited to 1 active request at a
                 * time. */
                mpm_supported = 0;
                break;
            }
            else if (!strcmp("simple_api.c", m->name)) {
                mpm_type = H2_MPM_SIMPLE;
                mpm_module = m;
                mpm_supported = 0;
                break;
            }
            else if (!strcmp("mpm_winnt.c", m->name)) {
                mpm_type = H2_MPM_WINNT;
                mpm_module = m;
                break;
            }
            else if (!strcmp("worker.c", m->name)) {
                mpm_type = H2_MPM_WORKER;
                mpm_module = m;
                break;
            }
        }
        checked = 1;
    }
}

h2_mpm_type_t h2_conn_mpm_type(void)
{
    check_modules(0);
    return mpm_type;
}

const char *h2_conn_mpm_name(void)
{
    check_modules(0);
    return mpm_module? mpm_module->name : "unknown";
}

int h2_mpm_supported(void)
{
    check_modules(0);
    return mpm_supported;
}

static module *h2_conn_mpm_module(void)
{
    check_modules(0);
    return mpm_module;
}

apr_status_t h2_c2_child_init(apr_pool_t *pool, server_rec *s)
{
    check_modules(1);
    return apr_socket_create(&dummy_socket, APR_INET, SOCK_STREAM,
                             APR_PROTO_TCP, pool);
}

/* APR callback invoked if allocation fails. */
static int abort_on_oom(int retcode)
{
    ap_abort_on_oom();
    return retcode; /* unreachable, hopefully. */
}

conn_rec *h2_c2_create(conn_rec *c1, int sec_id, apr_pool_t *parent)
{
    apr_allocator_t *allocator;
    apr_status_t status;
    apr_pool_t *pool;
    conn_rec *c2;
    void *cfg;
    module *mpm;

    ap_assert(c1);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE3, 0, c1,
                  "h2_stream(%ld-%d): create secondary", c1->id, sec_id);

    /* We create a pool with its own allocator to be used for
     * processing a request. This is the only way to have the processing
     * independent of its parent pool in the sense that it can work in
     * another thread. Also, the new allocator needs its own mutex to
     * synchronize sub-pools.
     */
    apr_allocator_create(&allocator);
    apr_allocator_max_free_set(allocator, ap_max_mem_free);
    status = apr_pool_create_ex(&pool, parent, NULL, allocator);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, c1,
                      APLOGNO(10004) "h2_session(%ld-%d): create secondary pool",
                      c1->id, sec_id);
        return NULL;
    }
    apr_allocator_owner_set(allocator, pool);
    apr_pool_abort_set(abort_on_oom, pool);
    apr_pool_tag(pool, "h2_secondary_conn");

    c2 = (conn_rec *) apr_palloc(pool, sizeof(conn_rec));
    memcpy(c2, c1, sizeof(conn_rec));

    c2->master                 = c1;
    c2->pool                   = pool;
    c2->conn_config            = ap_create_conn_config(pool);
    c2->notes                  = apr_table_make(pool, 5);
    c2->input_filters          = NULL;
    c2->output_filters         = NULL;
    c2->keepalives             = 0;
#if AP_MODULE_MAGIC_AT_LEAST(20180903, 1)
    c2->filter_conn_ctx        = NULL;
#endif
    c2->bucket_alloc           = apr_bucket_alloc_create(pool);
#if !AP_MODULE_MAGIC_AT_LEAST(20180720, 1)
    c2->data_in_input_filters  = 0;
    c2->data_in_output_filters = 0;
#endif
    /* prevent mpm_event from making wrong assumptions about this connection,
     * like e.g. using its socket for an async read check. */
    c2->clogging_input_filters = 1;
    c2->log                    = NULL;
    c2->log_id                 = apr_psprintf(pool, "%ld-%d",
                                             c1->id, sec_id);
    c2->aborted                = 0;
    /* We cannot install the master connection socket on the secondary, as
     * modules mess with timeouts/blocking of the socket, with
     * unwanted side effects to the master connection processing.
     * Fortunately, since we never use the secondary socket, we can just install
     * a single, process-wide dummy and everyone is happy.
     */
    ap_set_module_config(c2->conn_config, &core_module, dummy_socket);
    /* TODO: these should be unique to this thread */
    c2->sbh = c1->sbh;
    /* TODO: not all mpm modules have learned about secondary connections yet.
     * copy their config from master to secondary.
     */
    if ((mpm = h2_conn_mpm_module()) != NULL) {
        cfg = ap_get_module_config(c1->conn_config, mpm);
        ap_set_module_config(c2->conn_config, mpm, cfg);
    }

    ap_log_cerror(APLOG_MARK, APLOG_TRACE3, 0, c2,
                  "h2_secondary(%s): created", c2->log_id);
    return c2;
}

void h2_c2_destroy(conn_rec *c2)
{
    ap_log_cerror(APLOG_MARK, APLOG_TRACE3, 0, c2,
                  "h2_secondary(%s): destroy", c2->log_id);
    c2->sbh = NULL;
    apr_pool_destroy(c2->pool);
}

static void H2_TASK_OUT_LOG(int lvl, conn_rec *c, apr_bucket_brigade *bb,
                            const char *tag)
{
    if (APLOG_C_IS_LEVEL(c, lvl)) {
        h2_conn_ctx_t *ctx = h2_conn_ctx_get(c);
        char buffer[4 * 1024];
        const char *line = "(null)";
        apr_size_t len, bmax = sizeof(buffer)/sizeof(buffer[0]);
        
        len = h2_util_bb_print(buffer, bmax, tag, "", bb);
        ap_log_cerror(APLOG_MARK, lvl, 0, c, "bb_dump(%s): %s", 
                      ctx->id, len? buffer : line);
    }
}

static void output_consumed(void *ctx, h2_bucket_beam *beam, apr_off_t length)
{
    conn_rec *c = ctx;
    if (c && h2_c2_logio_add_bytes_out) {
        h2_c2_logio_add_bytes_out(c, length);
    }
}

static apr_status_t beam_out(conn_rec *c, h2_conn_ctx_t *conn_ctx, apr_bucket_brigade* bb, int block)
{
    apr_off_t written, left;
    apr_status_t rv;

    apr_brigade_length(bb, 0, &written);
    H2_TASK_OUT_LOG(APLOG_TRACE2, c, bb, "h2_c2 beam_out");
    h2_beam_log(conn_ctx->beam_out, c, APLOG_TRACE2, "beam_out(before)");

    rv = h2_beam_send(conn_ctx->beam_out, bb,
                      block? APR_BLOCK_READ : APR_NONBLOCK_READ);
    h2_beam_log(conn_ctx->beam_out, c, APLOG_TRACE2, "beam_out(after)");
    
    if (APR_STATUS_IS_EAGAIN(rv)) {
        apr_brigade_length(bb, 0, &left);
        written -= left;
        rv = APR_SUCCESS;
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, rv, c,
                  "h2_c2(%s): beam_out, added %ld bytes",
                  conn_ctx->id, (long)written);
    return rv;
}

static apr_status_t h2_filter_secondary_in(ap_filter_t* f,
                                           apr_bucket_brigade* bb,
                                           ap_input_mode_t mode,
                                           apr_read_type_e block,
                                           apr_off_t readbytes)
{
    h2_conn_ctx_t *conn_ctx;
    apr_status_t status = APR_SUCCESS;
    apr_bucket *b, *next;
    apr_off_t bblen;
    const int trace1 = APLOGctrace1(f->c);
    apr_size_t rmax = ((readbytes <= APR_SIZE_MAX)? 
                       (apr_size_t)readbytes : APR_SIZE_MAX);
    
    conn_ctx = h2_conn_ctx_get(f->c);
    ap_assert(conn_ctx);

    if (trace1) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, f->c,
                      "h2_secondary_in(%s): read, mode=%d, block=%d, readbytes=%ld", 
                      conn_ctx->id, mode, block, (long)readbytes);
    }
    
    if (mode == AP_MODE_INIT) {
        return ap_get_brigade(f->c->input_filters, bb, mode, block, readbytes);
    }
    
    if (f->c->aborted) {
        return APR_ECONNABORTED;
    }
    
    if (!conn_ctx->bb_in) {
        return APR_EOF;
    }
    
    /* Cleanup brigades from those nasty 0 length non-meta buckets
     * that apr_brigade_split_line() sometimes produces. */
    for (b = APR_BRIGADE_FIRST(conn_ctx->bb_in);
         b != APR_BRIGADE_SENTINEL(conn_ctx->bb_in); b = next) {
        next = APR_BUCKET_NEXT(b);
        if (b->length == 0 && !APR_BUCKET_IS_METADATA(b)) {
            apr_bucket_delete(b);
        } 
    }
    
    while (APR_BRIGADE_EMPTY(conn_ctx->bb_in)) {
        /* Get more input data for our request. */
        if (trace1) {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, f->c,
                          "h2_secondary_in(%s): get more data from mplx, block=%d, "
                          "readbytes=%ld", conn_ctx->id, block, (long)readbytes);
        }
        if (conn_ctx->beam_in) {
            status = h2_beam_receive(conn_ctx->beam_in, conn_ctx->bb_in, block,
                                     128*1024, NULL);
        }
        else {
            status = APR_EOF;
        }
        
        if (trace1) {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE2, status, f->c,
                          "h2_secondary_in(%s): read returned", conn_ctx->id);
        }
        if (APR_STATUS_IS_EAGAIN(status) 
            && (mode == AP_MODE_GETLINE || block == APR_BLOCK_READ)) {
            /* chunked input handling does not seem to like it if we
             * return with APR_EAGAIN from a GETLINE read... 
             * upload 100k test on test-ser.example.org hangs */
            status = APR_SUCCESS;
        }
        else if (APR_STATUS_IS_EOF(status)) {
            break;
        }
        else if (status != APR_SUCCESS) {
            return status;
        }

        if (trace1) {
            h2_util_bb_log(f->c, conn_ctx->stream_id, APLOG_TRACE2,
                        "input.beam recv raw", conn_ctx->bb_in);
        }
        if (h2_c2_logio_add_bytes_in) {
            apr_brigade_length(bb, 0, &bblen);
            h2_c2_logio_add_bytes_in(f->c, bblen);
        }
    }
    
    /* Nothing there, no more data to get. Return. */
    if (status == APR_EOF && APR_BRIGADE_EMPTY(conn_ctx->bb_in)) {
        return status;
    }

    if (trace1) {
        h2_util_bb_log(f->c, conn_ctx->stream_id, APLOG_TRACE2,
                    "task_input.bb", conn_ctx->bb_in);
    }
           
    if (APR_BRIGADE_EMPTY(conn_ctx->bb_in)) {
        if (trace1) {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, f->c,
                          "h2_secondary_in(%s): no data", conn_ctx->id);
        }
        return (block == APR_NONBLOCK_READ)? APR_EAGAIN : APR_EOF;
    }
    
    if (mode == AP_MODE_EXHAUSTIVE) {
        /* return all we have */
        APR_BRIGADE_CONCAT(bb, conn_ctx->bb_in);
    }
    else if (mode == AP_MODE_READBYTES) {
        status = h2_brigade_concat_length(bb, conn_ctx->bb_in, rmax);
    }
    else if (mode == AP_MODE_SPECULATIVE) {
        status = h2_brigade_copy_length(bb, conn_ctx->bb_in, rmax);
    }
    else if (mode == AP_MODE_GETLINE) {
        /* we are reading a single LF line, e.g. the HTTP headers. 
         * this has the nasty side effect to split the bucket, even
         * though it ends with CRLF and creates a 0 length bucket */
        status = apr_brigade_split_line(bb, conn_ctx->bb_in, block,
                                        HUGE_STRING_LEN);
        if (APLOGctrace1(f->c)) {
            char buffer[1024];
            apr_size_t len = sizeof(buffer)-1;
            apr_brigade_flatten(bb, buffer, &len);
            buffer[len] = 0;
            if (trace1) {
                ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, f->c,
                              "h2_secondary_in(%s): getline: %s",
                              conn_ctx->id, buffer);
            }
        }
    }
    else {
        /* Hmm, well. There is mode AP_MODE_EATCRLF, but we chose not
         * to support it. Seems to work. */
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_ENOTIMPL, f->c,
                      APLOGNO(03472) 
                      "h2_secondary_in(%s), unsupported READ mode %d", 
                      conn_ctx->id, mode);
        status = APR_ENOTIMPL;
    }
    
    if (trace1) {
        apr_brigade_length(bb, 0, &bblen);
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, f->c,
                      "h2_secondary_in(%s): %ld data bytes", conn_ctx->id, (long)bblen);
    }
    return status;
}

static apr_status_t register_output_at_mplx(h2_conn_ctx_t *conn_ctx, conn_rec *c)
{
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c, APLOGNO(03348)
                  "h2_c2(%s): open output to %s %s %s",
                  conn_ctx->id, conn_ctx->request->method,
                  conn_ctx->request->authority,
                  conn_ctx->request->path);
    conn_ctx->registered_at_mplx = 1;
    return h2_mplx_t_out_open(conn_ctx->mplx, c);
}

static apr_status_t h2_filter_secondary_output(ap_filter_t* f,
                                               apr_bucket_brigade* bb)
{
    h2_conn_ctx_t *conn_ctx = h2_conn_ctx_get(f->c);
    apr_bucket *b;
    apr_status_t rv;
    int flush = 0;

    ap_assert(conn_ctx);

    for (b = APR_BRIGADE_FIRST(bb);
         b != APR_BRIGADE_SENTINEL(bb);
         b = APR_BUCKET_NEXT(b)) {
        if (APR_BUCKET_IS_FLUSH(b) || APR_BUCKET_IS_EOS(b) || AP_BUCKET_IS_EOR(b)) {
            flush = 1;
            break;
        }
    }

send:
    rv = beam_out(f->c, conn_ctx, bb, conn_ctx->registered_at_mplx);
    if (APR_SUCCESS != rv) goto cleanup;

    if (!APR_BRIGADE_EMPTY(bb)) {
        /* We did not add all buckets to the beam. This can only happen
         * if added non-blocking, e.g. when we have not registered the
         * output to be polled at the h2_mplx. */
        ap_assert(!conn_ctx->registered_at_mplx);
        rv = register_output_at_mplx(conn_ctx, f->c);
        if (APR_SUCCESS == rv) goto send;
    }
    else if (!conn_ctx->registered_at_mplx
        && (flush || conn_ctx->out_unbuffered)) {
        /* on flush or disabled buffering, register the output
         * at the mplx for processing right away. */
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, rv, f->c,
                      "h2_c2(%s): open output, buffered=%d",
                      conn_ctx->id, !conn_ctx->out_unbuffered);
        rv = register_output_at_mplx(conn_ctx, f->c);
    }

cleanup:
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, rv, f->c,
                  "h2_c2(%s): output leave", conn_ctx->id);
    if (APR_SUCCESS != rv) {
        if (conn_ctx->beam_in) {
            h2_beam_leave(conn_ctx->beam_in);
        }
        if (!conn_ctx->done) {
            h2_beam_abort(conn_ctx->beam_out);
        }
        f->c->aborted = 1;
    }
    return rv;
}

/*******************************************************************************
 * Register various hooks
 */
static int h2_c2_hook_process(conn_rec* c);

APR_OPTIONAL_FN_TYPE(ap_logio_add_bytes_in) *h2_c2_logio_add_bytes_in;
APR_OPTIONAL_FN_TYPE(ap_logio_add_bytes_out) *h2_c2_logio_add_bytes_out;

void h2_c2_register_hooks(void)
{
    /* When the connection processing actually starts, we might
     * take over, if the connection is for a task.
     */
    ap_hook_process_connection(h2_c2_hook_process,
                               NULL, NULL, APR_HOOK_FIRST);

    ap_register_input_filter("H2_SECONDARY_IN", h2_filter_secondary_in,
                             NULL, AP_FTYPE_NETWORK);
    ap_register_output_filter("H2_SECONDARY_OUT", h2_filter_secondary_output,
                              NULL, AP_FTYPE_NETWORK);
    ap_register_output_filter("H2_PARSE_H1", h2_from_h1_parse_response,
                              NULL, AP_FTYPE_NETWORK);

    ap_register_input_filter("H2_REQUEST", h2_filter_request_in,
                             NULL, AP_FTYPE_PROTOCOL);
    ap_register_output_filter("H2_RESPONSE", h2_filter_headers_out,
                              NULL, AP_FTYPE_PROTOCOL);
    ap_register_output_filter("H2_TRAILERS_OUT", h2_filter_trailers_out,
                              NULL, AP_FTYPE_PROTOCOL);
}

/* post config init */
apr_status_t h2_c2_init(apr_pool_t *pool, server_rec *s)
{
    h2_c2_logio_add_bytes_in = APR_RETRIEVE_OPTIONAL_FN(ap_logio_add_bytes_in);
    h2_c2_logio_add_bytes_out = APR_RETRIEVE_OPTIONAL_FN(ap_logio_add_bytes_out);

    return APR_SUCCESS;
}

static apr_status_t c2_run_pre_connection(conn_rec *secondary, apr_socket_t *csd)
{
    if (secondary->keepalives == 0) {
        /* Simulate that we had already a request on this connection. Some
         * hooks trigger special behaviour when keepalives is 0.
         * (Not necessarily in pre_connection, but later. Set it here, so it
         * is in place.) */
        secondary->keepalives = 1;
        /* We signal that this connection will be closed after the request.
         * Which is true in that sense that we throw away all traffic data
         * on this secondary connection after each requests. Although we might
         * reuse internal structures like memory pools.
         * The wanted effect of this is that httpd does not try to clean up
         * any dangling data on this connection when a request is done. Which
         * is unnecessary on a h2 stream.
         */
        secondary->keepalive = AP_CONN_CLOSE;
        return ap_run_pre_connection(secondary, csd);
    }
    ap_assert(secondary->output_filters);
    return APR_SUCCESS;
}

apr_status_t h2_c2_process(conn_rec *c, apr_thread_t *thread, int worker_id)
{
    h2_conn_ctx_t *conn_ctx = h2_conn_ctx_get(c);

    ap_assert(conn_ctx);
    ap_assert(conn_ctx->mplx);

    /* See the discussion at <https://github.com/icing/mod_h2/issues/195>
     *
     * Each conn_rec->id is supposed to be unique at a point in time. Since
     * some modules (and maybe external code) uses this id as an identifier
     * for the request_rec they handle, it needs to be unique for secondary
     * connections also.
     *
     * The MPM module assigns the connection ids and mod_unique_id is using
     * that one to generate identifier for requests. While the implementation
     * works for HTTP/1.x, the parallel execution of several requests per
     * connection will generate duplicate identifiers on load.
     *
     * The original implementation for secondary connection identifiers used
     * to shift the master connection id up and assign the stream id to the
     * lower bits. This was cramped on 32 bit systems, but on 64bit there was
     * enough space.
     *
     * As issue 195 showed, mod_unique_id only uses the lower 32 bit of the
     * connection id, even on 64bit systems. Therefore collisions in request ids.
     *
     * The way master connection ids are generated, there is some space "at the
     * top" of the lower 32 bits on allmost all systems. If you have a setup
     * with 64k threads per child and 255 child processes, you live on the edge.
     *
     * The new implementation shifts 8 bits and XORs in the worker
     * id. This will experience collisions with > 256 h2 workers and heavy
     * load still. There seems to be no way to solve this in all possible
     * configurations by mod_h2 alone.
     */
    c->id = (c->master->id << 8)^worker_id;

    h2_beam_create(&conn_ctx->beam_out, conn_ctx->pool, conn_ctx->stream_id, "output",
                   H2_BEAM_OWNER_SEND, 0, c->base_server->timeout);
    if (!conn_ctx->beam_out) {
        return APR_ENOMEM;
    }
    
    h2_beam_buffer_size_set(conn_ctx->beam_out, conn_ctx->mplx->stream_max_mem);
    h2_beam_send_from(conn_ctx->beam_out, conn_ctx->pool);
    h2_beam_on_consumed(conn_ctx->beam_out, NULL, output_consumed, c);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c,
                  "h2_secondary(%s), adding filters", conn_ctx->id);
    ap_add_input_filter("H2_SECONDARY_IN", NULL, NULL, c);
    ap_add_output_filter("H2_PARSE_H1", NULL, NULL, c);
    ap_add_output_filter("H2_SECONDARY_OUT", NULL, NULL, c);

    c2_run_pre_connection(c, ap_get_conn_socket(c));

    conn_ctx->bb_in = apr_brigade_create(conn_ctx->pool, c->bucket_alloc);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_c2(%s): process connection", conn_ctx->id);
                  
    c->current_thread = thread;
    ap_run_process_connection(c);
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_c2(%s): processing done", conn_ctx->id);
    if (!conn_ctx->registered_at_mplx) {
        return register_output_at_mplx(conn_ctx, c);
    }
    return APR_SUCCESS;
}

static apr_status_t c2_process(h2_conn_ctx_t *conn_ctx, conn_rec *c)
{
    const h2_request *req = conn_ctx->request;
    conn_state_t *cs = c->cs;
    request_rec *r;

    r = h2_create_request_rec(conn_ctx->request, c);
    if (!r) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                      "h2_c2(%s): create request_rec failed, r=NULL", conn_ctx->id);
        goto cleanup;
    }
    if (r->status != HTTP_OK) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                      "h2_c2(%s): create request_rec failed, r->status=%d",
                      conn_ctx->id, r->status);
        goto cleanup;
    }

    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_c2(%s): created request_rec", conn_ctx->id);
    conn_ctx->server = r->server;
    conn_ctx->out_unbuffered = !h2_config_rgeti(r, H2_CONF_OUTPUT_BUFFER);

    /* the request_rec->server carries the timeout value that applies */
    h2_beam_timeout_set(conn_ctx->beam_out, r->server->timeout);
    if (conn_ctx->beam_in) {
        h2_beam_timeout_set(conn_ctx->beam_in, r->server->timeout);
    }

    ap_update_child_status(c->sbh, SERVER_BUSY_WRITE, r);
    if (cs) {
        cs->state = CONN_STATE_HANDLER;
    }
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_c2(%s): start process_request", conn_ctx->id);

    /* Add the raw bytes of the request (e.g. header frame lengths to
     * the logio for this request. */
    if (req->raw_bytes && h2_c2_logio_add_bytes_in) {
        h2_c2_logio_add_bytes_in(c, req->raw_bytes);
    }

    ap_process_request(r);

    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                  "h2_c2(%s): process_request done", conn_ctx->id);

    /* After the call to ap_process_request, the
     * request pool may have been deleted.  We set
     * r=NULL here to ensure that any dereference
     * of r that might be added later in this function
     * will result in a segfault immediately instead
     * of nondeterministic failures later.
     */
    if (cs)
        cs->state = CONN_STATE_WRITE_COMPLETION;
    r = NULL;

cleanup:
    return APR_SUCCESS;
}

static int h2_c2_hook_process(conn_rec* c)
{
    h2_conn_ctx_t *ctx;
    
    if (!c->master) {
        return DECLINED;
    }
    
    ctx = h2_conn_ctx_get(c);
    if (ctx->stream_id) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                      "h2_h2, processing request directly");
        c2_process(ctx, c);
        return DONE;
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, 
                      "secondary_conn(%ld): has no task", c->id);
    }
    return DECLINED;
}

