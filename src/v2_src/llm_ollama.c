/* llm_ollama.c - the Ollama backend (see llm.h): POST /api/chat over plain HTTP/1.1. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "llm.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCK (-1)
#define close_sock close
#endif

/* ------------------------------------------------------------------------------------------------ pure parts */

LlmStatus llm_parse_url(Arena *a, const char *url, LlmUrl *out)
{
    if (!a || !url || !out || strncmp(url, "http://", 7)) return LLM_INVALID_ARGUMENT;   /* no TLS: http only */
    const char *h = url + 7, *e = h;
    while (*e && *e != ':' && *e != '/') e++;
    if (e == h) return LLM_INVALID_ARGUMENT;
    char *host = arena_alloc(a, (size_t)(e - h) + 1);
    if (!host) return LLM_OUT_OF_MEMORY;
    memcpy(host, h, (size_t)(e - h));
    host[e - h] = 0;
    int port = 80;
    if (*e == ':') {
        port = 0;
        e++;
        const char *d = e;
        while (*e >= '0' && *e <= '9') { port = port * 10 + (*e - '0'); if (port > 65535) return LLM_INVALID_ARGUMENT; e++; }
        if (e == d || !port) return LLM_INVALID_ARGUMENT;
    }
    if (*e && *e != '/') return LLM_INVALID_ARGUMENT;
    out->host = host;
    out->port = port;
    out->path = *e ? e : "/";
    return LLM_OK;
}

static bool header_is(const char *line, size_t n, const char *name)
{
    size_t k = strlen(name);
    if (n < k + 1 || line[k] != ':') return false;
    for (size_t i = 0; i < k; i++) if ((line[i] | 32) != (name[i] | 32)) return false;
    return true;
}

LlmStatus llm_http_parse(Arena *a, const char *raw, size_t len, int *status, const char **body, size_t *body_len)
{
    if (!a || !raw || !status || !body || !body_len) return LLM_INVALID_ARGUMENT;
    if (len < 12 || strncmp(raw, "HTTP/1.", 7)) return LLM_BAD_RESPONSE;
    const char *sp = memchr(raw, ' ', len);
    if (!sp || sp + 4 > raw + len) return LLM_BAD_RESPONSE;
    *status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    if (*status < 100 || *status > 599) return LLM_BAD_RESPONSE;
    const char *p = raw, *end = raw + len;
    long content_length = -1;
    bool chunked = false;
    p = memchr(p, '\n', len);
    if (!p) return LLM_BAD_RESPONSE;
    p++;
    for (;;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return LLM_BAD_RESPONSE;
        size_t n = (size_t)(nl - p);
        if (n && p[n - 1] == '\r') n--;
        if (!n) { p = nl + 1; break; }
        if (header_is(p, n, "content-length")) content_length = strtol(p + 15, NULL, 10);
        if (header_is(p, n, "transfer-encoding")) { for (size_t i = 18; i + 7 <= n; i++) if (!strncmp(p + i, "chunked", 7)) chunked = true; }
        p = nl + 1;
    }
    if (!chunked) {
        size_t avail = (size_t)(end - p);
        if (content_length >= 0 && (size_t)content_length < avail) avail = (size_t)content_length;
        if (content_length >= 0 && (size_t)content_length > avail) return LLM_BAD_RESPONSE;   /* cut short */
        char *b = arena_alloc(a, avail + 1);
        if (!b) return LLM_OUT_OF_MEMORY;
        memcpy(b, p, avail);
        b[avail] = 0;
        *body = b;
        *body_len = avail;
        return LLM_OK;
    }
    char *b = arena_alloc(a, (size_t)(end - p) + 1);
    if (!b) return LLM_OUT_OF_MEMORY;
    size_t n = 0;
    for (;;) {
        char *after;
        unsigned long size = strtoul(p, &after, 16);
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl || after == p) return LLM_BAD_RESPONSE;
        p = nl + 1;
        if (!size) break;
        if ((size_t)(end - p) < size) return LLM_BAD_RESPONSE;
        memcpy(b + n, p, size);
        n += size;
        p += size;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    b[n] = 0;
    *body = b;
    *body_len = n;
    return LLM_OK;
}

LlmStatus llm_ollama_request_body(Arena *a, const LlmOllamaConfig *c, const LlmMessage *msgs, size_t n,
                                  const char *schema, const char **body, size_t *len)
{
    if (!a || !c || !c->model || !msgs || !n || !body || !len) return LLM_INVALID_ARGUMENT;
    if (schema) {   /* "format" carries the schema as a JSON value: it must be one */
        JsonValue *v;
        JsonError e;
        if (json_parse(a, schema, strlen(schema), &v, &e) != JSON_OK || v->kind != JSON_OBJECT) return LLM_INVALID_ARGUMENT;
    }
    JsonWriter w;
    json_writer_init_compact(&w, a);
    json_write_begin_object(&w);
    json_write_key(&w, "model"); json_write_string(&w, c->model);
    json_write_key(&w, "messages");
    json_write_begin_array(&w);
    for (size_t i = 0; i < n; i++) {
        json_write_begin_object(&w);
        json_write_key(&w, "role"); json_write_string(&w, llm_role_name(msgs[i].role));
        json_write_key(&w, "content"); json_write_string(&w, msgs[i].content ? msgs[i].content : "");
        json_write_end_object(&w);
    }
    json_write_end_array(&w);
    json_write_key(&w, "stream"); json_write_bool(&w, false);
    json_write_key(&w, "think"); json_write_bool(&w, false);
    if (schema) { json_write_key(&w, "format"); json_write_raw(&w, schema, strlen(schema)); }
    json_write_key(&w, "options"); json_write_raw(&w, "{\"temperature\":0}", 17);
    json_write_end_object(&w);
    *body = json_writer_finish(&w, len);
    return *body ? LLM_OK : LLM_OUT_OF_MEMORY;
}

LlmStatus llm_ollama_parse_reply(Arena *a, const char *body, size_t len, LlmReply *out)
{
    JsonValue *v;
    JsonError e;
    if (!a || !body || !out) return LLM_INVALID_ARGUMENT;
    if (json_parse(a, body, len, &v, &e) != JSON_OK || v->kind != JSON_OBJECT) { out->error = "the server's reply is not a JSON object"; return LLM_BAD_RESPONSE; }
    const JsonValue *err = json_get(v, "error");
    if (err) { out->error = err->kind == JSON_STRING ? err->text : "the server reported an error"; return LLM_HTTP_ERROR; }
    const JsonValue *msg = json_get(v, "message"), *content = json_get(msg, "content"), *model = json_get(v, "model");
    if (!content || content->kind != JSON_STRING) { out->error = "the reply has no message.content"; return LLM_BAD_RESPONSE; }
    out->content = content->text;
    out->content_len = content->text_len;
    out->model = model && model->kind == JSON_STRING ? model->text : NULL;
    int64_t t;
    if (json_to_i64(json_get(v, "prompt_eval_count"), &t) == JSON_NUMBER_OK && t >= 0) out->prompt_tokens = (uint64_t)t;
    if (json_to_i64(json_get(v, "eval_count"), &t) == JSON_NUMBER_OK && t >= 0) out->completion_tokens = (uint64_t)t;
    return LLM_OK;
}

/* ------------------------------------------------------------------------------------------------ transport */

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static LlmStatus post(Arena *a, const LlmUrl *u, const char *body, size_t blen, uint32_t timeout_ms,
                      int *status, const char **resp, size_t *rlen)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return LLM_UNAVAILABLE;
#endif
    char port[16];
    snprintf(port, sizeof port, "%d", u->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    LlmStatus st = LLM_UNAVAILABLE;
    Sock s = BAD_SOCK;
    if (getaddrinfo(u->host, port, &hints, &res) || !res) goto out;
    for (struct addrinfo *ai = res; ai && s == BAD_SOCK; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK) continue;
#ifdef _WIN32
        DWORD tv = timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
#else
        struct timeval tv = {(time_t)(timeout_ms / 1000), (suseconds_t)((timeout_ms % 1000) * 1000)};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen)) { close_sock(s); s = BAD_SOCK; }
    }
    if (s == BAD_SOCK) goto out;
    char head[1024];
    int hn = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\nUser-Agent: copper-core\r\n\r\n", u->path, u->host, u->port, blen);
    st = LLM_TIMEOUT;
    if (send(s, head, hn, 0) != hn) goto out;
    for (size_t sent = 0; sent < blen;) {
        int k = send(s, body + sent, (int)(blen - sent > 65536 ? 65536 : blen - sent), 0);
        if (k <= 0) goto out;
        sent += (size_t)k;
    }
    size_t cap = 1 << 16, n = 0;
    char *buf = arena_alloc(a, cap);
    for (;;) {
        if (!buf) { st = LLM_OUT_OF_MEMORY; goto out; }
        if (n + 4096 > cap) {
            char *more = arena_alloc(a, cap * 2);
            if (more) memcpy(more, buf, n);
            buf = more;
            cap *= 2;
            continue;
        }
        int k = recv(s, buf + n, 4096, 0);
        if (k < 0) goto out;                 /* timeout or reset */
        if (k == 0) break;                   /* Connection: close */
        n += (size_t)k;
    }
    st = llm_http_parse(a, buf, n, status, resp, rlen);
out:
    if (s != BAD_SOCK) close_sock(s);
    if (res) freeaddrinfo(res);
#ifdef _WIN32
    WSACleanup();
#endif
    return st;
}

static LlmStatus ollama_chat(LlmBackend *b, const LlmMessage *msgs, size_t n, const char *schema, Arena *a, LlmReply *out)
{
    LlmOllamaState *s = b->state;
    memset(out, 0, sizeof *out);
    const char *base = s->config.base_url ? s->config.base_url : "http://localhost:11434";
    size_t nb = strlen(base);
    char *url = arena_alloc(a, nb + 16);
    if (!url) return LLM_OUT_OF_MEMORY;
    snprintf(url, nb + 16, "%s%s", base, base[nb ? nb - 1 : 0] == '/' ? "api/chat" : "/api/chat");
    LlmUrl u;
    LlmStatus st = llm_parse_url(a, url, &u);
    if (st) { out->error = "base_url must be http://host[:port]"; return st; }
    const char *body;
    size_t blen;
    if ((st = llm_ollama_request_body(a, &s->config, msgs, n, schema, &body, &blen))) { out->error = "the request could not be built"; return st; }
    uint64_t t0 = now_ms();
    int status = 0;
    const char *resp;
    size_t rlen;
    st = post(a, &u, body, blen, s->config.timeout_ms ? s->config.timeout_ms : 300000u, &status, &resp, &rlen);
    out->elapsed_ms = now_ms() - t0;
    out->http_status = status;
    if (st == LLM_UNAVAILABLE) { out->error = "no Ollama server answers at this URL"; return st; }
    if (st == LLM_TIMEOUT) { out->error = "the Ollama server did not answer in time"; return st; }
    if (st) { out->error = "the HTTP response could not be read"; return st; }
    st = llm_ollama_parse_reply(a, resp, rlen, out);
    if (st == LLM_OK && status != 200) { out->error = "HTTP status is not 200"; return LLM_HTTP_ERROR; }
    return st;
}

void llm_ollama_init(LlmBackend *b, LlmOllamaState *s, const LlmOllamaConfig *config)
{
    memset(s, 0, sizeof *s);
    if (config) s->config = *config;
    b->name = "ollama";
    b->chat = ollama_chat;
    b->state = s;
}
