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

/* This is an apache module.

   More specifically, an output filter that compresses responses to requests
   with zlib and optionally a preset dictionary.

   Content Filter.
 
   AP_FTYPE_CONTENT_SET for the second stage of content filtering.
 
 
 */

static int zlibdict_handler(request_rec *r)
{
    return OK;
}

static void zlibdict_hooks(apr_pool_t *pool)
{
    ap_hook_handler(zlibdict_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA zlibdict_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                          /* per-dir config creater */
    NULL,                          /* merge per-dir config */
#if 0
    create_zlibdict_server_config, /* per-server config */
#endif
    NULL,
    NULL,                          /* merge per-server config */
    NULL,                          /* command table */
    zlibdict_hooks                 /* register hooks */
};
