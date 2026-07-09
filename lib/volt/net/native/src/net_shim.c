#include "net_shim.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

struct NetFetch {
    char *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    struct NetFetch *f = (struct NetFetch *)ud;
    size_t n = size * nmemb;
    if (f->len + n > f->cap) {
        size_t ncap = f->cap ? f->cap * 2 : 16384;
        while (ncap < f->len + n) ncap *= 2;
        char *nd = (char *)realloc(f->data, ncap);
        if (!nd) return 0; /* signals a write error to curl -> perform() fails */
        f->data = nd;
        f->cap = ncap;
    }
    memcpy(f->data + f->len, ptr, n);
    f->len += n;
    return n;
}

NetFetch *net_fetch_url(const char *url) {
    static int inited = 0;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct NetFetch *f = (struct NetFetch *)calloc(1, sizeof(*f));
    if (!f) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); /* total-time cap */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "volt-net/0.1");

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        free(f->data);
        free(f);
        return NULL;
    }
    return (NetFetch *)f;
}

void net_fetch_free(NetFetch *f) {
    if (!f) return;
    free(((struct NetFetch *)f)->data);
    free(f);
}

long net_fetch_len(NetFetch *f) {
    return f ? (long)((struct NetFetch *)f)->len : 0;
}

void *net_fetch_data(NetFetch *f) {
    return f ? ((struct NetFetch *)f)->data : NULL;
}
