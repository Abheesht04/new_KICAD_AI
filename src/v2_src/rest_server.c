/* rest_server.c - HTTP/1.1 REST front end for the tool server (see rest_server.h). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "rest_server.h"

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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int Sock;
#define BAD_SOCK (-1)
#define close_sock close
#endif

#define MAX_REQUEST (16u << 20)

static RestResponse json_response(int status, const char *body, size_t len)
{
    RestResponse r = {status, body, len, false};
    return r;
}

static RestResponse error(Arena *a, int status, const char *kind, const char *message)
{
    JsonWriter w;
    json_writer_init_compact(&w, a);
    json_write_begin_object(&w);
    json_write_key(&w, "error");
    json_write_begin_object(&w);
    json_write_key(&w, "code"); json_write_int(&w, status);
    json_write_key(&w, "message"); json_write_string(&w, message);
    json_write_key(&w, "data"); json_write_begin_object(&w); json_write_key(&w, "kind"); json_write_string(&w, kind); json_write_end_object(&w);
    json_write_end_object(&w);
    json_write_end_object(&w);
    size_t n;
    const char *t = json_writer_finish(&w, &n);
    return json_response(status, t ? t : "{}", t ? n : 2);
}

static int http_status_for(int64_t code)
{
    switch (code) {
    case -32700: case -32600: case -32602: return 400;
    case -32601: case -32001: case -32003: return 404;
    case -32012: return 409;
    case -32002: return 422;
    case -32011: return 502;
    case -32010: return 503;
    default: return 500;
    }
}

static void write_value(JsonWriter *w, const JsonValue *v)
{
    switch (v->kind) {
    case JSON_NULL: json_write_null(w); break;
    case JSON_TRUE: json_write_bool(w, true); break;
    case JSON_FALSE: json_write_bool(w, false); break;
    case JSON_NUMBER: json_write_raw(w, v->text, v->text_len); break;
    case JSON_STRING: json_write_string(w, v->text); break;
    case JSON_ARRAY:
        json_write_begin_array(w);
        for (size_t i = 0; i < v->count; i++) write_value(w, v->items[i]);
        json_write_end_array(w);
        break;
    case JSON_OBJECT:
        json_write_begin_object(w);
        for (size_t i = 0; i < v->count; i++) { json_write_key(w, v->keys[i]); write_value(w, v->items[i]); }
        json_write_end_object(w);
        break;
    }
}

/* Run one JSON-RPC request line through the server and return its parsed response. */
static const JsonValue *rpc(ToolServer *s, Arena *a, const char *line, size_t n)
{
    size_t rl;
    const char *resp = tool_server_handle(s, line, n, &rl);
    JsonValue *v;
    JsonError e;
    if (!resp) return NULL;
    char *copy = arena_alloc(a, rl + 1);
    if (!copy) return NULL;
    memcpy(copy, resp, rl);
    copy[rl] = 0;
    return json_parse(a, copy, rl, &v, &e) == JSON_OK ? v : NULL;
}

/* {"result": ...} with 200, or {"error": ...} with the status of its code. */
static RestResponse from_rpc(Arena *a, const JsonValue *resp)
{
    if (!resp) return error(a, 500, "NO_RESPONSE", "the tool server gave no answer");
    const JsonValue *result = json_get(resp, "result"), *err = json_get(resp, "error");
    JsonWriter w;
    json_writer_init_compact(&w, a);
    json_write_begin_object(&w);
    int status = 200;
    if (err) {
        int64_t code = 0;
        json_to_i64(json_get(err, "code"), &code);
        status = http_status_for(code);
        json_write_key(&w, "error"); write_value(&w, err);
    } else {
        json_write_key(&w, "result");
        if (result) write_value(&w, result); else json_write_null(&w);
    }
    json_write_end_object(&w);
    size_t n;
    const char *t = json_writer_finish(&w, &n);
    return t ? json_response(status, t, n) : error(a, 500, "OUT_OF_MEMORY", "out of memory");
}

RestResponse rest_handle(ToolServer *s, Arena *a, const char *method, const char *path, const char *body, size_t len)
{
    if (!s || !a || !method || !path) return json_response(500, "{}", 2);
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (!strcmp(path, "/v1/health")) {
        if (!get) return error(a, 405, "METHOD_NOT_ALLOWED", "use GET");
        static const char ok[] = "{\"status\":\"ok\",\"server\":\"copper-core\"}";
        return json_response(200, ok, sizeof ok - 1);
    }
    if (!strcmp(path, "/v1/tools")) {
        if (!get) return error(a, 405, "METHOD_NOT_ALLOWED", "use GET");
        static const char req[] = "{\"jsonrpc\":\"2.0\",\"id\":\"rest\",\"method\":\"tools/list\"}";
        return from_rpc(a, rpc(s, a, req, sizeof req - 1));
    }
    if (!strcmp(path, "/v1/shutdown")) {
        if (!post) return error(a, 405, "METHOD_NOT_ALLOWED", "use POST");
        RestResponse r = json_response(200, "{\"status\":\"stopping\"}", 21);
        r.stop = true;
        return r;
    }
    if (!strcmp(path, "/v1/rpc")) {
        if (!post) return error(a, 405, "METHOD_NOT_ALLOWED", "use POST");
        size_t rl;
        const char *resp = tool_server_handle(s, body ? body : "", body ? len : 0, &rl);
        if (!resp) return json_response(204, "", 0);
        char *copy = arena_alloc(a, rl + 1);
        if (!copy) return error(a, 500, "OUT_OF_MEMORY", "out of memory");
        memcpy(copy, resp, rl);
        copy[rl] = 0;
        return json_response(200, copy, rl);
    }
    if (!strncmp(path, "/v1/tools/", 10) && path[10]) {
        if (!post) return error(a, 405, "METHOD_NOT_ALLOWED", "use POST with the tool's arguments as a JSON object");
        JsonValue *args = NULL;
        JsonError e;
        if (len && (json_parse(a, body, len, &args, &e) != JSON_OK || args->kind != JSON_OBJECT))
            return error(a, 400, "BAD_ARGUMENTS", "the body must be a JSON object of the tool's arguments");
        JsonWriter w;
        json_writer_init_compact(&w, a);
        json_write_begin_object(&w);
        json_write_key(&w, "jsonrpc"); json_write_string(&w, "2.0");
        json_write_key(&w, "id"); json_write_string(&w, "rest");
        json_write_key(&w, "method"); json_write_string(&w, "tools/call");
        json_write_key(&w, "params");
        json_write_begin_object(&w);
        json_write_key(&w, "name"); json_write_string(&w, path + 10);
        json_write_key(&w, "arguments");
        if (args) write_value(&w, args); else { json_write_begin_object(&w); json_write_end_object(&w); }
        json_write_end_object(&w);
        json_write_end_object(&w);
        size_t n;
        const char *req = json_writer_finish(&w, &n);
        if (!req) return error(a, 500, "OUT_OF_MEMORY", "out of memory");
        return from_rpc(a, rpc(s, a, req, n));
    }
    return error(a, 404, "NOT_FOUND", "unknown path (see /v1/health, /v1/tools, /v1/tools/{name}, /v1/rpc)");
}

/* ------------------------------------------------------------------------------------------------ sockets */

static bool send_all(Sock c, const char *p, size_t n)
{
    while (n) {
        int k = send(c, p, (int)(n > 65536 ? 65536 : n), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 409: return "Conflict"; case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity"; case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static void respond(Sock c, const RestResponse *r)
{
    char head[256];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     r->status, reason(r->status), r->body_len);
    if (send_all(c, head, (size_t)n) && r->body_len) send_all(c, r->body, r->body_len);
}

/* Read one request: request line, headers, Content-Length body. False on a malformed or oversized request. */
static bool read_request(Sock c, Arena *a, char **method, char **path, char **body, size_t *blen, int *bad_status)
{
    size_t cap = 1 << 14, n = 0, header_end = 0, need = 0;
    char *buf = arena_alloc(a, cap);
    *bad_status = 400;
    while (buf) {
        if (n == cap) {
            if (cap >= MAX_REQUEST + 65536) { *bad_status = 413; return false; }
            char *more = arena_alloc(a, cap * 2);
            if (!more) return false;
            memcpy(more, buf, n);
            buf = more;
            cap *= 2;
        }
        int k = recv(c, buf + n, (int)(cap - n), 0);
        if (k <= 0) return false;
        n += (size_t)k;
        if (!header_end) {
            for (size_t i = 3; i < n; i++) if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') { header_end = i + 1; break; }
            if (!header_end) continue;
            size_t content = 0;
            for (size_t i = 0; i + 15 < header_end; i++) {
                if ((i == 0 || buf[i - 1] == '\n') && (buf[i] | 32) == 'c' && !strncmp(buf + i + 1, "ontent-", 7) && ((buf[i + 8] | 32) == 'l') && !strncmp(buf + i + 9, "ength:", 6))
                    content = strtoul(buf + i + 15, NULL, 10);
            }
            if (content > MAX_REQUEST) { *bad_status = 413; return false; }
            need = header_end + content;
        }
        if (header_end && n >= need) break;
    }
    if (!buf || !header_end) return false;
    /* request line: METHOD SP PATH SP HTTP/1.x */
    char *sp1 = memchr(buf, ' ', header_end), *sp2 = sp1 ? memchr(sp1 + 1, ' ', header_end - (size_t)(sp1 + 1 - buf)) : NULL;
    if (!sp1 || !sp2) return false;
    *sp1 = 0;
    *sp2 = 0;
    char *q = strchr(sp1 + 1, '?');
    if (q) *q = 0;
    *method = buf;
    *path = sp1 + 1;
    *blen = need - header_end;
    *body = arena_alloc(a, *blen + 1);   /* its own buffer, so the terminating NUL always fits */
    if (!*body) return false;
    memcpy(*body, buf + header_end, *blen);
    (*body)[*blen] = 0;
    return true;
}

RestStatus rest_serve(ToolServer *s, const char *host, int port, void (*ready)(void *, int), void *ctx)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return REST_SOCKET_ERROR;
#endif
    RestStatus st = REST_BIND_FAILED;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    Sock l = BAD_SOCK;
    if (getaddrinfo(host ? host : "127.0.0.1", ps, &hints, &res) || !res) goto out;
    l = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (l == BAD_SOCK) goto out;
    if (bind(l, res->ai_addr, (int)res->ai_addrlen) || listen(l, 16)) goto out;
    {
        struct sockaddr_in bound;
        socklen_t bl = sizeof bound;
        int actual = port;
        if (!getsockname(l, (struct sockaddr *)&bound, &bl)) actual = ntohs(bound.sin_port);
        if (ready) ready(ctx, actual);
    }
    st = REST_OK;
    for (bool stop = false; !stop;) {
        Sock c = accept(l, NULL, NULL);
        if (c == BAD_SOCK) { st = REST_SOCKET_ERROR; break; }
        Arena a;
        arena_init(&a, 1 << 16);
        char *method, *path, *body;
        size_t blen;
        int bad;
        RestResponse r;
        if (read_request(c, &a, &method, &path, &body, &blen, &bad)) r = rest_handle(s, &a, method, path, body, blen);
        else r = error(&a, bad, bad == 413 ? "TOO_LARGE" : "BAD_HTTP", bad == 413 ? "the request is larger than 16 MiB" : "not an HTTP/1.1 request");
        respond(c, &r);
        stop = r.stop;
        close_sock(c);
        arena_destroy(&a);
    }
out:
    if (l != BAD_SOCK) close_sock(l);
    if (res) freeaddrinfo(res);
#ifdef _WIN32
    WSACleanup();
#endif
    return st;
}
