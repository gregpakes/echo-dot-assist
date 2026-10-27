/* Wake word clips: see wakeclip.h.  One request at a time is collected here (the capture thread's), then handed to a
 * detached thread that POSTs it, so a slow or absent collector never holds up the microphone. */
#include "wakeclip.h"
#include "core.h"
#include "net.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RATE 16000
#define MAX_SAMPLES (RATE * 20)                 /* Home Assistant's VAD ends a request after 15 s, plus the wake word */

static atomic_int enabled;
static int16_t *buf;                            /* malloc'd at begin, handed to the sender at end */
static size_t len, word_begin, word_end, stream_from, dropped;
static float level;
static char keyword[32];
static long long started_ms;                    /* wall clock of Home Assistant's first sample */
static int active;

int wakeclip_enable(int on) { if (on >= 0) atomic_store(&enabled, !!on); return atomic_load(&enabled); }
int wakeclip_active(void) { return active; }

static long long wall_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

void wakeclip_begin(const int16_t *pre, size_t n, size_t wb, size_t we, size_t from, float keyword_db, const char *kw)
{
    if (active) wakeclip_end();
    if (!atomic_load(&enabled) || !(buf = malloc(MAX_SAMPLES * sizeof *buf))) return;
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    memcpy(buf, pre, n * sizeof *buf);
    len = n; word_begin = wb; word_end = we; stream_from = from; dropped = 0; level = keyword_db;
    snprintf(keyword, sizeof keyword, "%s", kw ? kw : "");
    started_ms = wall_ms() - (long long)(n - from) * 1000 / RATE;
    active = 1;
}

void wakeclip_add(const int16_t *pcm, size_t n)
{
    if (!active) return;
    size_t k = n < MAX_SAMPLES - len ? n : MAX_SAMPLES - len;
    memcpy(buf + len, pcm, k * sizeof *buf); len += k; dropped += n - k;
}

struct clip { int16_t *pcm; size_t len, word_begin, word_end, stream_from; float level; long long started_ms; char keyword[32]; };

static void put_le(unsigned char *p, unsigned v, int bytes) { for (int i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static int write_all(int fd, const void *p, size_t n)
{
    for (const char *c = p; n; ) { ssize_t w = send(fd, c, n, MSG_NOSIGNAL); if (w <= 0) return -1; c += w; n -= (size_t)w; }
    return 0;
}

static void *send_clip(void *arg)
{
    struct clip *c = arg;
    const char *e = getenv("HASSMIC_WAKE_CLIP_TARGET");
    char host[128], *port; snprintf(host, sizeof host, "%s", e && *e ? e : WAKECLIP_TARGET);
    if (!(port = strrchr(host, ':'))) { fprintf(stderr, "wake clip: target %s has no port\n", host); goto done; }
    *port++ = 0;
    unsigned data = (unsigned)(c->len * 2);
    unsigned char wav[44];
    memcpy(wav, "RIFF", 4); put_le(wav + 4, 36 + data, 4); memcpy(wav + 8, "WAVEfmt ", 8);
    put_le(wav + 16, 16, 4); put_le(wav + 20, 1, 2); put_le(wav + 22, 1, 2); put_le(wav + 24, RATE, 4);
    put_le(wav + 28, RATE * 2, 4); put_le(wav + 32, 2, 2); put_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4); put_le(wav + 40, data, 4);
    char head[768];
    int h = snprintf(head, sizeof head,
        "POST /clip HTTP/1.0\r\nHost: %s:%s\r\nContent-Type: audio/wav\r\nContent-Length: %u\r\n"
        "X-Echo: %s\r\nX-Keyword: %s\r\nX-Keyword-Db: %.1f\r\nX-Stream-Start-Ms: %lld\r\n"
        "X-Wake-Begin-Ms: %zu\r\nX-Wake-End-Ms: %zu\r\nX-Stream-From-Ms: %zu\r\n\r\n",
        host, port, 44 + data, core_node_name(), c->keyword, c->level, c->started_ms,
        c->word_begin * 1000 / RATE, c->word_end * 1000 / RATE, c->stream_from * 1000 / RATE);
    int fd = net_connect(host, port, 3);
    if (fd < 0) goto done;                      /* net_connect logged it */
    char reply[64] = "";
    if (write_all(fd, head, (size_t)h) || write_all(fd, wav, sizeof wav) || write_all(fd, c->pcm, data))
        fprintf(stderr, "wake clip: sending to %s:%s failed\n", host, port);
    else {
        ssize_t r = recv(fd, reply, sizeof reply - 1, 0);
        reply[r > 0 ? r : 0] = 0;
        char *nl = strpbrk(reply, "\r\n"); if (nl) *nl = 0;
        fprintf(stderr, "wake clip: %zu ms (wake word %zu-%zu ms, Home Assistant's stream from %zu ms) to %s:%s: %s\n",
                c->len * 1000 / RATE, c->word_begin * 1000 / RATE, c->word_end * 1000 / RATE, c->stream_from * 1000 / RATE,
                host, port, *reply ? reply : "no answer");
    }
    close(fd);
done:
    free(c->pcm); free(c);
    return NULL;
}

void wakeclip_end(void)
{
    if (!active) return;
    active = 0;
    if (dropped) fprintf(stderr, "wake clip: %zu ms over the %d s limit left out\n", dropped * 1000 / RATE, MAX_SAMPLES / RATE);
    struct clip *c = malloc(sizeof *c);
    if (!c) { free(buf); buf = NULL; return; }
    *c = (struct clip){ buf, len, word_begin, word_end, stream_from, level, started_ms, "" };
    memcpy(c->keyword, keyword, sizeof c->keyword);
    buf = NULL;
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, send_clip, c)) { free(c->pcm); free(c); }
    pthread_attr_destroy(&a);
}
