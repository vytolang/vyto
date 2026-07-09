#ifndef NET_SHIM_H
#define NET_SHIM_H

typedef struct NetFetch NetFetch;

/* Synchronous HTTP(S) GET via libcurl. NULL on any transport error, timeout,
   or non-2xx HTTP status — same "null is a valid failure signal" contract as
   gfx_image_load_file. Blocking, bounded by a fixed connect+total timeout
   (see net_shim.c) so a dead/slow host can't hang the caller forever —
   matches the already-accepted synchronous local-file-decode tradeoff, with
   network latency substituted for disk latency. No async/threaded variant;
   out of scope for this iteration. */
NetFetch *net_fetch_url(const char *url);
void net_fetch_free(NetFetch *f);
long net_fetch_len(NetFetch *f);   /* bytes fetched */
void *net_fetch_data(NetFetch *f); /* borrowed pointer, valid until net_fetch_free */

#endif
