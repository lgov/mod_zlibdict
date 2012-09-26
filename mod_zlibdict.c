//
//  mod_zlibdict.c
//  mod_zlibdict
//
//  Created by Lieven Govaerts on 24/09/12.
//  Copyright (c) 2012 Lieven Govaerts. All rights reserved.
//

#include <httpd.h>
#include <http_protocol.h>
#include <http_config.h>
#include <http_log.h>

#include <apr_lib.h>
#include <apr_general.h>
#include <apr_buckets.h>

#include <zlib.h>

/* This is an apache module.

   More specifically, an output filter that compresses responses to requests
   with zlib and optionally a preset dictionary.

 */
static const char* zlibdict_filtername = "ZLIBDICT";

/* Dictionary for deflating responses from a svn server to a PROPFIND request. */
static const char *propfind_dictionary =
"<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\" xmlns:ns0=\"DAV:\">"
"<D:response xmlns:S=\"http://subversion.tigris.org/xmlns/svn/\" xmlns:C=\"http://subversion.tigris.org/xmlns/custom/\" xmlns:V=\"http://subversion.tigris.org/xmlns/dav/\" xmlns:lp1=\"DAV:\" xmlns:lp3=\"http://subversion.tigris.org/xmlns/dav/\" xmlns:lp2=\"http://apache.org/dav/props/\"><D:href>"
"<D:propstat><D:prop><S:eol-style>native"
"</lp1:resourcetype><lp1:resourcetype/></lp1:getcontentlength><lp1:getcontenttype>"
"</lp1:getetag></lp1:creationdate><lp1:getlastmodified></lp1:version-name>"
"<lp1:creator-displayname></lp3:md5-checksum><lp3:repository-uuid>"
"<D:supportedlock><D:lockentry><D:lockscope><D:locktype>"
"</D:lockentry></D:supportedlock><D:lockdiscovery/></D:prop>"
"<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>text/plain";

#define DEFAULT_COMPRESSION Z_DEFAULT_COMPRESSION
#define DEFAULT_WINDOWSIZE 15   /* we need the zlib wrapper for dictionary
                                   support. */
#define DEFAULT_MEMLEVEL 8
#define DEFAULT_BUFFERSIZE 8096

typedef struct zlibdict_ctx_t {
    z_stream zstr;

    apr_bucket_brigade *bb;

    char *buf;
} zlibdict_ctx_t;

static int
zlibdict__header_contains(apr_pool_t *pool, const char *header)
{
    char *token;

    token = ap_get_token(pool, &header, 0);
    while (token && token[0] && strcasecmp(token, "zlibdict")) {
        /* skip parameters, XXX: ;q=foo evaluation? */
        while (*header == ';') {
            ++header;
            ap_get_token(pool, &header, 1);
        }

        /* retrieve next token */
        if (*header == ',') {
            ++header;
        }
        token = (*header) ? ap_get_token(pool, &header, 0) : NULL;
    }

    /* No acceptable token found. */
    if (token == NULL || token[0] == '\0') {
        return 1;
    }

    return OK;
}

/* TODO: cleanup ctx */
static apr_status_t
zlibdict_output_filter(ap_filter_t *f, apr_bucket_brigade *bb)
{
    apr_bucket *b;
    zlibdict_ctx_t *ctx = f->ctx;
    request_rec *r = f->r;
    const char *client_accepts;
    apr_status_t status = APR_SUCCESS;
    apr_pool_t *subpool;
    int zerr;

    ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, f->r,
                  "triggered zlibdict_output_filter");

    /* Do nothing if asked to filter nothing. */
    if (APR_BRIGADE_EMPTY(bb)) {
        return APR_SUCCESS;
    }

    /* First time we are called for this response? */
    if (!ctx) {
        client_accepts = apr_table_get(r->headers_in, "Accept-Encoding");
        if (client_accepts == NULL ||
            zlibdict__header_contains(r->pool, client_accepts)) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "Not compressing (no Accept-Encoding: zlibdict)");
            ap_remove_output_filter(f);
            return ap_pass_brigade(f->next, bb);
        }

        ctx = f->ctx = apr_pcalloc(r->pool, sizeof(*ctx));
        ctx->bb = apr_brigade_create(r->pool, f->c->bucket_alloc);
        ctx->buf = apr_palloc(r->pool, DEFAULT_BUFFERSIZE);

        /* zstream must be NULL'd out. */
        memset(&ctx->zstr, 0, sizeof(z_stream));
        zerr = deflateInit2(&ctx->zstr, DEFAULT_COMPRESSION,
                            Z_DEFLATED,
                            DEFAULT_WINDOWSIZE, DEFAULT_MEMLEVEL,
                            Z_DEFAULT_STRATEGY);

        deflateSetDictionary(&ctx->zstr, (Bytef *)propfind_dictionary,
                             strlen(propfind_dictionary));

        /* Set Content-Encoding header so our client knows how to handle 
           this data. */
        apr_table_mergen(r->headers_out, "Content-Encoding", "zlibdict");
    }

    /* Read the data from the handler and compress it with a dictionary. */
    for (b = APR_BRIGADE_FIRST(bb);
         b != APR_BRIGADE_SENTINEL(bb);
         b = APR_BUCKET_NEXT(b)) {

        const char *data;
        void *write_buf;
        size_t len;
        size_t buf_size, write_len;

        if (APR_BUCKET_IS_EOS(b)) {
            deflateEnd(&ctx->zstr);

            /* Remove EOS from the old list, and insert into the new. */
            APR_BUCKET_REMOVE(b);
            APR_BRIGADE_INSERT_TAIL(ctx->bb, b);

            return ap_pass_brigade(f->next, ctx->bb);
        }

        if (APR_BUCKET_IS_METADATA(b))
            continue;

        status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
        if (status != APR_SUCCESS)
            break;

        /* The largest buffer we should need is 0.1% larger than the
         compressed data, + 12 bytes. This info comes from zlib.h.  */
        buf_size = len + (len / 1000) + 13;
        apr_pool_create(&subpool, r->pool);
        write_buf = apr_palloc(subpool, buf_size);
        
        ctx->zstr.next_in = (Bytef *)data;  /* Casting away const! */
        ctx->zstr.avail_in = (uInt) len;

        zerr = Z_OK;
        while (ctx->zstr.avail_in > 0 && zerr != Z_STREAM_END)
        {
            ctx->zstr.next_out = write_buf;
            ctx->zstr.avail_out = (uInt) buf_size;

            zerr = deflate(&ctx->zstr, Z_FINISH);
            if (zerr < 0)
                return -1; /* TODO: fix error */
            write_len = buf_size - ctx->zstr.avail_out;
            if (write_len > 0) {
                apr_bucket *b_out;

                b_out = apr_bucket_heap_create(write_buf, len,
                                               NULL, f->c->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(ctx->bb, b_out);
                /* Send what we have right now to the next filter. */
                status = ap_pass_brigade(f->next, ctx->bb);
                if (status != APR_SUCCESS) {
                    apr_pool_destroy(subpool);
                    return status;
                }
            }

            apr_pool_destroy(subpool);
        }
    }
    
    return status;
}

static void
zlibdict_hooks(apr_pool_t *pool)
{
    ap_register_output_filter(zlibdict_filtername, zlibdict_output_filter, NULL,
                              AP_FTYPE_CONTENT_SET);
}

module AP_MODULE_DECLARE_DATA zlibdict_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                          /* per-dir config creater */
    NULL,                          /* merge per-dir config */
    NULL,                          /* per-server config */
    NULL,                          /* merge per-server config */
    NULL,                          /* command table */
    zlibdict_hooks                 /* register hooks */
};
