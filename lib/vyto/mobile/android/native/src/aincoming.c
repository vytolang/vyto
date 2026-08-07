/* aincoming.c — intents arriving *at* the app: deep links, shares, and a tap
 * on one of our own notifications.
 *
 * A queue, not a slot (contrast astream.c). Two shared links, or a link
 * arriving while a share is unread, must not overwrite each other: each one is
 * a user action, and dropping one drops something they did. There are never
 * many, so the cost of a queue is nothing.
 *
 * The queue exists from process start, before the Vyto thread does. That is
 * the point: the intent that *launched* the app is delivered in onCreate,
 * which happens before Native.start, so a design that needed the Vyto side to
 * be ready first would lose exactly the most common case — the app being
 * opened by a link.
 *
 * Ownership: strdup on push, freed when the entry is dropped after being read.
 * The strings a caller reads stay valid until the next incoming_poll, which is
 * the same contract vta_result_uri has in aintent_shim.c.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__

#include "vyto_android.h"
#include <pthread.h>

typedef struct Incoming {
    char *action;
    char *uri;
    char *mime;
    char *text;
    int32_t notification;   /* id of the notification tapped, or 0 */
    struct Incoming *next;
} Incoming;

static Incoming *g_head = NULL, *g_tail = NULL;
static Incoming *g_current = NULL;      /* the one the reader is looking at */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static char *dup_or_empty(const char *s) {
    char *d = strdup(s ? s : "");
    return d;
}

static void incoming_free(Incoming *i) {
    if (!i) return;
    free(i->action); free(i->uri); free(i->mime); free(i->text);
    free(i);
}

/* From the UI thread (onCreate / onNewIntent). */
void vta_incoming_push(const char *action, const char *uri, const char *mime,
                       const char *text, int32_t notification) {
    Incoming *i = (Incoming *)calloc(1, sizeof *i);
    if (!i) return;
    i->action = dup_or_empty(action);
    i->uri = dup_or_empty(uri);
    i->mime = dup_or_empty(mime);
    i->text = dup_or_empty(text);
    i->notification = notification;

    pthread_mutex_lock(&g_lock);
    if (g_tail) { g_tail->next = i; g_tail = i; }
    else { g_head = i; g_tail = i; }
    pthread_mutex_unlock(&g_lock);
}

/* Take the next one. Returns 1 when there was one — its fields are then read
 * with the accessors below and stay valid until the next call. */
int32_t vta_incoming_next(void) {
    pthread_mutex_lock(&g_lock);
    Incoming *i = g_head;
    if (i) {
        g_head = i->next;
        if (!g_head) g_tail = NULL;
        i->next = NULL;
    }
    pthread_mutex_unlock(&g_lock);

    incoming_free(g_current);   /* the previous one's strings die here */
    g_current = i;
    return i ? 1 : 0;
}

const char *vta_incoming_action(void) { return g_current ? g_current->action : ""; }
const char *vta_incoming_uri(void)    { return g_current ? g_current->uri : ""; }
const char *vta_incoming_mime(void)   { return g_current ? g_current->mime : ""; }
const char *vta_incoming_text(void)   { return g_current ? g_current->text : ""; }
int32_t vta_incoming_notification(void) { return g_current ? g_current->notification : 0; }

#else

void vta_incoming_push(const char *action, const char *uri, const char *mime,
                       const char *text, int32_t notification) {
    (void)action; (void)uri; (void)mime; (void)text; (void)notification;
}
int32_t vta_incoming_next(void) { return 0; }
const char *vta_incoming_action(void) { return ""; }
const char *vta_incoming_uri(void)    { return ""; }
const char *vta_incoming_mime(void)   { return ""; }
const char *vta_incoming_text(void)   { return ""; }
int32_t vta_incoming_notification(void) { return 0; }

#endif
