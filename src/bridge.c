/*
 * xqc-h3-bridge.c — 本地 HTTP/1.1(绝对形式) -> 上游 HTTP/3(XQUIC v1.9.4) 桥接进程
 *
 * 用法:
 *   h3_bridge [-l ip:port] [-L logfile] [-t timeout_sec] [-d]
 *   默认: -l 0.0.0.0:18080 -L /tmp/xqc_h3_bridge.log -t 15
 *
 * 入口协议 (HTTP/1.1 简单子集, 支持 curl -x / HttpClient Proxy 绝对形式):
 *   GET http://acs.m.taobao.com/gw/mtop.common.getTimestamp/1.0/ HTTP/1.1
 *   POST /gw/xxx/1.0/ HTTP/1.1 + Host: acs.m.taobao.com     (原始形式亦可)
 *   请求体仅支持 Content-Length; 不支持 CONNECT / chunked / keep-alive。
 *   上游一律以 https + HTTP/3 处理: URL/Host 中显式端口生效, 否则 443。
 *   响应: HTTP/1.1 + Content-Length + Connection: close。
 *   本地健康检查: GET /__health -> 200 "xqc-h3-bridge ok" (不进上游)。
 *   回环上游地址(localhost, 127.x, ::1, 0.0.0.0)一律 400, 防止把桥自身当上游自环。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include <xquic/xqc_http3.h>
#include <xquic/xqc_errno.h>

extern xqc_usec_t xqc_now(void);

#define BR_MAX_HDRS      100
#define BR_HDRS_MAX_LEN  (256 * 1024)
#define BR_BODY_MAX      (64UL * 1024 * 1024)
#define BR_PKT_BUF       1600
#define BR_TLS_GROUPS    "P-256:X25519:P-384:P-521"

typedef struct br_ctx_s   br_ctx_t;
typedef struct br_qconn_s br_qconn_t;
typedef struct br_req_s   br_req_t;

typedef struct br_hdr_s { char *name; char *value; } br_hdr_t;

struct br_req_s {
    br_ctx_t           *ctx;
    struct bufferevent *bev;
    struct event       *ev_timeout;

    int                 parsed;
    int                 bev_freed;
    int                 h3_closed;
    int                 responded;
    int                 finished;
    int                 close_sched;
    int                 close_done;

    char                method[16];
    char                host[256];
    int                 port;
    char                path[2048];
    char                authority[320];
    br_hdr_t            hdrs[BR_MAX_HDRS];
    int                 nhdrs;
    char               *body;
    size_t              body_len;

    xqc_h3_request_t   *h3;
    xqc_http_header_t  *h3_hdrs;
    int                 h3_nhdrs;
    int                 hdr_sent;
    size_t              body_sent_off;

    int                 status;
    br_hdr_t            rhdrs[BR_MAX_HDRS];
    int                 nrhdrs;
    char               *rbuf;
    size_t              rlen;
    size_t              rcap;
    int                 rf_fin;

    br_qconn_t         *qconn;
    xqc_cid_t           cid;
};

struct br_qconn_s {
    br_ctx_t           *ctx;
    br_req_t           *req;
    int                 fd;
    struct event       *ev;
    xqc_cid_t           cid;
    int                 connected;
};

struct br_ctx_s {
    xqc_engine_t       *engine;
    struct event_base  *eb;
    struct event       *ev_listen;
    struct event       *ev_engine;
    int                 listen_fd;
    FILE               *logfp;
    int                 debug;
    int                 timeout_sec;
};

/* ---------- helpers ---------- */
static void br_maybe_free(br_req_t *r);
static void br_udp_read_cb(evutil_socket_t fd, short what, void *arg);

static const char *br_reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Unknown";
    }
}

static int br_skip_fwd_hdr(const char *n)
{
    static const char *skip[] = {
        "host", "connection", "proxy-connection", "keep-alive",
        "transfer-encoding", "upgrade", "te", "content-length", "expect", NULL
    };
    for (int i = 0; skip[i]; i++) {
        if (!strcmp(n, skip[i])) return 1;
    }
    return 0;
}

static int br_skip_rsp_hdr(const char *n)
{
    return !strcasecmp(n, "content-length") || !strcasecmp(n, "transfer-encoding")
        || !strcasecmp(n, "connection") || !strcasecmp(n, "keep-alive")
        || !strcasecmp(n, "proxy-connection");
}

static void br_lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* ---------- response write ---------- */
static void br_build_and_write(br_req_t *r, int code, const br_hdr_t *hdrs, int nhdrs,
                               const char *body, size_t blen, const char *ctype)
{
    if (!r->bev) return;

    size_t cap = 1024 + blen;
    for (int i = 0; i < nhdrs; i++) {
        cap += strlen(hdrs[i].name) + strlen(hdrs[i].value) + 8;
    }
    char *out = malloc(cap);
    if (!out) return;

    size_t off = 0;
    off += (size_t)snprintf(out + off, cap - off, "HTTP/1.1 %d %s\r\n", code, br_reason(code));
    if (ctype) {
        off += (size_t)snprintf(out + off, cap - off, "Content-Type: %s\r\n", ctype);
    }
    for (int i = 0; i < nhdrs; i++) {
        if (br_skip_rsp_hdr(hdrs[i].name)) continue;
        off += (size_t)snprintf(out + off, cap - off, "%s: %s\r\n", hdrs[i].name, hdrs[i].value);
    }
    off += (size_t)snprintf(out + off, cap - off,
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    if (blen) {
        memcpy(out + off, body, blen);
        off += blen;
    }
    bufferevent_write(r->bev, out, off);
    free(out);
    r->responded = 1;
}

static void br_close_upstream_cb(evutil_socket_t fd, short what, void *arg)
{
    br_req_t *r = arg;
    (void)fd; (void)what;

    if (r->h3) {
        xqc_h3_request_close(r->h3);
        r->h3 = NULL;               /* close_notify 触发时置 h3_closed */
    } else {
        r->h3_closed = 1;           /* 没有 request 对象, 不会有 notify */
    }
    if (r->qconn && r->qconn->connected) {
        xqc_conn_close(r->ctx->engine, &r->qconn->cid);
    }
    r->close_done = 1;
    br_maybe_free(r);
}

static void br_schedule_close(br_req_t *r)
{
    if (r->close_sched) return;
    r->close_sched = 1;
    struct timeval tv = { 0, 0 };
    event_base_once(r->ctx->eb, -1, EV_TIMEOUT, br_close_upstream_cb, r, &tv);
}

static void br_fail(br_req_t *r, int code, const char *msg)
{
    if (r->responded || r->finished) return;
    r->finished = 1;
    br_build_and_write(r, code, NULL, 0, msg, strlen(msg), "text/plain; charset=utf-8");
    br_schedule_close(r);
    br_maybe_free(r);
}

static void br_finish_ok(br_req_t *r)
{
    if (r->responded || r->finished) return;
    r->finished = 1;
    int code = r->status ? r->status : 502;
    br_build_and_write(r, code, r->rhdrs, r->nrhdrs, r->rbuf, r->rlen, NULL);
    br_schedule_close(r);
    br_maybe_free(r);
}

static void br_maybe_free(br_req_t *r)
{
    if (!r->bev_freed || !r->h3_closed) return;
    if (r->close_sched && !r->close_done) return;

    if (r->ev_timeout) {
        event_del(r->ev_timeout);
        event_free(r->ev_timeout);
        r->ev_timeout = NULL;
    }
    for (int i = 0; i < r->nhdrs; i++) { free(r->hdrs[i].name); free(r->hdrs[i].value); }
    for (int i = 0; i < r->nrhdrs; i++) { free(r->rhdrs[i].name); free(r->rhdrs[i].value); }
    free(r->body);
    free(r->h3_hdrs);
    free(r->rbuf);
    free(r);
}

/* ---------- http parse ---------- */
static int br_parse_http(br_req_t *r, char *data, size_t len, size_t *consumed)
{
    *consumed = 0;

    char *p = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '\r' && data[i+3] == '\n') {
            p = data + i;
            break;
        }
    }
    if (!p) {
        return (len > BR_HDRS_MAX_LEN) ? -1 : 0;
    }
    size_t hdr_len = (size_t)(p - data) + 4;

    char *hb = malloc(hdr_len + 1);
    if (!hb) return -1;
    memcpy(hb, data, hdr_len);
    hb[hdr_len] = 0;

    char *cur = hb;
    char *nl = strstr(cur, "\r\n");
    if (!nl) { free(hb); return -1; }
    *nl = 0;

    char method[16], url[2048], ver[16];
    if (sscanf(cur, "%15s %2047s %15s", method, url, ver) < 2) { free(hb); return -1; }
    cur = nl + 2;

    char host_hdr[256] = {0};
    int host_port = 0;
    long content_len = -1;

    while (*cur) {
        nl = strstr(cur, "\r\n");
        if (!nl) { free(hb); return -1; }
        *nl = 0;
        if (*cur == 0) break;

        char *colon = strchr(cur, ':');
        if (!colon) { free(hb); return -1; }
        *colon = 0;
        char *nm = cur;
        char *vl = colon + 1;
        while (*vl == ' ' || *vl == '\t') vl++;
        char *ve = vl + strlen(vl);
        while (ve > vl && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        br_lower(nm);

        if (!strcmp(nm, "host")) {
            char *c2 = strrchr(vl, ':');
            if (c2 && c2[1]) { host_port = atoi(c2 + 1); *c2 = 0; }
            snprintf(host_hdr, sizeof(host_hdr), "%s", vl);
        } else if (!strcmp(nm, "content-length")) {
            content_len = strtol(vl, NULL, 10);
        } else if (!strcmp(nm, "transfer-encoding")) {
            free(hb); return -1;    /* chunked 不支持 */
        }

        if (r->nhdrs < BR_MAX_HDRS) {
            r->hdrs[r->nhdrs].name = strdup(nm);
            r->hdrs[r->nhdrs].value = strdup(vl);
            r->nhdrs++;
        }
        cur = nl + 2;
    }

    if (!strcasecmp(method, "CONNECT")) { free(hb); return -1; }
    snprintf(r->method, sizeof(r->method), "%s", method);

    /* URL -> host/port/path (上游一律 https; 端口显式给定时生效, 否则 443) */
    const char *u = url;
    if (!strncasecmp(u, "https://", 8)) { u += 8; }
    else if (!strncasecmp(u, "http://", 7)) { u += 7; }

    if (*u == '/') {
        snprintf(r->path, sizeof(r->path), "%s", u);
        if (!host_hdr[0]) { free(hb); return -1; }
        snprintf(r->host, sizeof(r->host), "%s", host_hdr);
        r->port = 443;   /* origin-form: Host 端口是桥自身的, 上游固定 443 */
    } else {
        const char *slash = strchr(u, '/');
        const char *hostend = slash ? slash : u + strlen(u);
        const char *c2 = memchr(u, ':', (size_t)(hostend - u));
        size_t hl = c2 ? (size_t)(c2 - u) : (size_t)(hostend - u);
        if (hl == 0 || hl >= sizeof(r->host)) { free(hb); return -1; }
        memcpy(r->host, u, hl);
        r->host[hl] = 0;
        r->port = c2 ? atoi(c2 + 1) : 443;
        snprintf(r->path, sizeof(r->path), "%s", slash ? slash : "/");
    }
    if (r->port <= 0 || r->port > 65535) r->port = 443;

    /* 回环地址一律拒绝(健康检查除外): 防止把桥自身当上游导致自环挂起 */
    if (strcmp(r->path, "/__health") != 0) {
        if (!strcmp(r->host, "localhost") || !strncmp(r->host, "127.", 4)
            || !strcmp(r->host, "::1") || !strcmp(r->host, "[::1]")
            || !strcmp(r->host, "0.0.0.0")) {
            free(hb);
            return -1;
        }
    }

    size_t blen = content_len > 0 ? (size_t)content_len : 0;
    if (content_len > (long)BR_BODY_MAX) { free(hb); return -2; }

    size_t total = hdr_len + blen;
    if (len < total) { free(hb); return 0; }

    if (blen) {
        r->body = malloc(blen);
        if (!r->body) { free(hb); return -1; }
        memcpy(r->body, data + hdr_len, blen);
        r->body_len = blen;
    }
    *consumed = total;
    free(hb);
    return 1;
}

/* ---------- h3 send ---------- */
static void br_send_req(br_req_t *r)
{
    if (!r->h3 || r->finished) return;

    if (!r->hdr_sent) {
        xqc_http_headers_t hh;
        memset(&hh, 0, sizeof(hh));
        hh.headers = r->h3_hdrs;
        hh.count = (size_t)r->h3_nhdrs;
        ssize_t n = xqc_h3_request_send_headers(r->h3, &hh, r->body_len == 0 ? 1 : 0);
        if (n == -XQC_EAGAIN) return;
        if (n < 0) { br_fail(r, 502, "h3 send headers error"); return; }
        r->hdr_sent = 1;
    }

    if (r->body_len > 0 && r->body_sent_off < r->body_len) {
        while (r->body_sent_off < r->body_len) {
            ssize_t n = xqc_h3_request_send_body(r->h3,
                (unsigned char *)r->body + r->body_sent_off,
                r->body_len - r->body_sent_off, 1);
            if (n == -XQC_EAGAIN) return;
            if (n < 0) { br_fail(r, 502, "h3 send body error"); return; }
            r->body_sent_off += (size_t)n;
        }
    }
}

static void br_start_h3(br_req_t *r)
{
    br_ctx_t *c = r->ctx;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", r->port);
    struct addrinfo hints, *res = NULL, *it, *pick = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(r->host, portstr, &hints, &res) != 0 || res == NULL) {
        br_fail(r, 502, "dns resolve failed");
        return;
    }
    for (it = res; it; it = it->ai_next) {
        if (it->ai_family == AF_INET) { pick = it; break; }
    }
    if (!pick) pick = res;

    int fd = socket(pick->ai_family, SOCK_DGRAM, 0);
    if (fd < 0) { freeaddrinfo(res); br_fail(r, 502, "udp socket failed"); return; }
    evutil_make_socket_nonblocking(fd);
    int sz = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));

    br_qconn_t *q = calloc(1, sizeof(*q));
    if (!q) { close(fd); freeaddrinfo(res); br_fail(r, 502, "oom"); return; }
    q->ctx = c;
    q->req = r;
    q->fd = fd;
    q->ev = event_new(c->eb, fd, EV_READ | EV_PERSIST, br_udp_read_cb, q);
    event_add(q->ev, NULL);
    r->qconn = q;

    xqc_conn_settings_t st;
    memset(&st, 0, sizeof(st));
    st.pacing_on = 0;
    st.cong_ctrl_callback = xqc_bbr_cb;
    st.cc_params.customize_on = 1;
    st.cc_params.init_cwnd = 96;
    st.so_sndbuf = 1024 * 1024;
    st.proto_version = XQC_VERSION_V1;
    st.spurious_loss_detect_on = 1;
    st.keyupdate_pkt_threshold = UINT64_MAX;
    st.max_pkt_out_size = 1200;
    st.max_udp_payload_size = 1200;
    st.adaptive_ack_frequency = 1;

    xqc_conn_ssl_config_t sslcfg;
    memset(&sslcfg, 0, sizeof(sslcfg));

    const xqc_cid_t *cid = xqc_h3_connect(c->engine, &st, NULL, 0, r->host, 0, &sslcfg,
                                          pick->ai_addr, pick->ai_addrlen, q);
    freeaddrinfo(res);
    if (cid == NULL) {
        if (q->ev) { event_del(q->ev); event_free(q->ev); }
        close(fd);
        r->qconn = NULL;
        free(q);
        br_fail(r, 502, "h3 connect failed");
        return;
    }
    q->connected = 1;
    memcpy(&q->cid, cid, sizeof(xqc_cid_t));
    memcpy(&r->cid, cid, sizeof(xqc_cid_t));

    /* :authority */
    if (r->port == 443) {
        snprintf(r->authority, sizeof(r->authority), "%s", r->host);
    } else {
        snprintf(r->authority, sizeof(r->authority), "%s:%d", r->host, r->port);
    }

    /* 组装 h3 头 */
    int cap = 4 + r->nhdrs;
    r->h3_hdrs = calloc((size_t)cap, sizeof(xqc_http_header_t));
    if (!r->h3_hdrs) { br_fail(r, 502, "oom"); return; }
    int n = 0;
#define H3_SET(i, nm, nml, vv, vvl) do { \
        r->h3_hdrs[i].name.iov_base = (void *)(nm);  r->h3_hdrs[i].name.iov_len = (nml); \
        r->h3_hdrs[i].value.iov_base = (void *)(vv); r->h3_hdrs[i].value.iov_len = (vvl); \
        r->h3_hdrs[i].flags = 0; \
    } while (0)

    H3_SET(n, ":method", 7, r->method, strlen(r->method)); n++;
    H3_SET(n, ":scheme", 7, "https", 5); n++;
    H3_SET(n, ":path", 5, r->path, strlen(r->path)); n++;
    H3_SET(n, ":authority", 10, r->authority, strlen(r->authority)); n++;
    for (int i = 0; i < r->nhdrs && n < cap; i++) {
        if (br_skip_fwd_hdr(r->hdrs[i].name)) continue;
        H3_SET(n, r->hdrs[i].name, strlen(r->hdrs[i].name),
               r->hdrs[i].value, strlen(r->hdrs[i].value));
        n++;
    }
#undef H3_SET
    r->h3_nhdrs = n;

    xqc_stream_settings_t ss;
    memset(&ss, 0, sizeof(ss));
    r->h3 = xqc_h3_request_create(c->engine, &r->cid, &ss, r);
    if (!r->h3) { br_fail(r, 502, "h3 request create failed"); return; }

    br_send_req(r);
}

/* ---------- h3 callbacks ---------- */
static int br_h3_conn_create(xqc_h3_conn_t *h3c, const xqc_cid_t *cid, void *ud)
{
    (void)h3c; (void)cid; (void)ud;
    return 0;
}

static int br_h3_conn_close(xqc_h3_conn_t *h3c, const xqc_cid_t *cid, void *ud)
{
    (void)h3c; (void)cid;
    br_qconn_t *q = ud;
    if (q) {
        if (q->ev) { event_del(q->ev); event_free(q->ev); }
        if (q->fd >= 0) close(q->fd);
        if (q->req && q->req->qconn == q) q->req->qconn = NULL;
        free(q);
    }
    return 0;
}

static int br_h3_req_create(xqc_h3_request_t *h3, void *ud)
{
    (void)h3; (void)ud;
    return 0;
}

static int br_h3_req_close(xqc_h3_request_t *h3, void *ud)
{
    (void)h3;
    br_req_t *r = ud;
    r->h3 = NULL;
    r->h3_closed = 1;
    br_maybe_free(r);
    return 0;
}

static int br_append_body(br_req_t *r, const unsigned char *data, size_t n)
{
    if (r->rlen + n > BR_BODY_MAX) return -1;
    if (r->rlen + n > r->rcap) {
        size_t ncap = r->rcap ? r->rcap : 8192;
        while (ncap < r->rlen + n) ncap *= 2;
        char *nb = realloc(r->rbuf, ncap);
        if (!nb) return -1;
        r->rbuf = nb;
        r->rcap = ncap;
    }
    memcpy(r->rbuf + r->rlen, data, n);
    r->rlen += n;
    return 0;
}

static int br_h3_read(xqc_h3_request_t *h3, xqc_request_notify_flag_t flag, void *ud)
{
    br_req_t *r = ud;
    uint8_t fin = 0;

    if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
        xqc_http_headers_t *hs = xqc_h3_request_recv_headers(h3, &fin);
        if (hs == NULL) { br_fail(r, 502, "h3 recv headers error"); return 0; }
        for (int i = 0; i < (int)hs->count; i++) {
            const char *nm = (const char *)hs->headers[i].name.iov_base;
            size_t nml = hs->headers[i].name.iov_len;
            const char *vl = (const char *)hs->headers[i].value.iov_base;
            size_t vvl = hs->headers[i].value.iov_len;

            if (nml == 7 && !strncmp(nm, ":status", 7)) {
                char sb[8];
                size_t cl = vvl < sizeof(sb) - 1 ? vvl : sizeof(sb) - 1;
                memcpy(sb, vl, cl);
                sb[cl] = 0;
                r->status = atoi(sb);
            } else if (nm[0] != ':' && r->nrhdrs < BR_MAX_HDRS) {
                r->rhdrs[r->nrhdrs].name = strndup(nm, nml);
                r->rhdrs[r->nrhdrs].value = strndup(vl, vvl);
                r->nrhdrs++;
            }
        }
        if (fin) r->rf_fin = 1;
    }

    if (!r->rf_fin && (flag & XQC_REQ_NOTIFY_READ_BODY)) {
        unsigned char buf[16384];
        ssize_t n;
        do {
            n = xqc_h3_request_recv_body(h3, buf, sizeof(buf), &fin);
            if (n == -XQC_EAGAIN) break;
            if (n < 0) { br_fail(r, 502, "h3 recv body error"); return 0; }
            if (n > 0) {
                if (br_append_body(r, buf, (size_t)n) < 0) {
                    br_fail(r, 502, "response too large");
                    return 0;
                }
            }
        } while (n > 0 && !fin);
        if (fin) r->rf_fin = 1;
    }

    if (flag & XQC_REQ_NOTIFY_READ_EMPTY_FIN) r->rf_fin = 1;

    if (r->rf_fin) br_finish_ok(r);
    return 0;
}

static int br_h3_write(xqc_h3_request_t *h3, void *ud)
{
    (void)h3;
    br_req_t *r = ud;
    br_send_req(r);
    return 0;
}

/* ---------- transport callbacks ---------- */
static ssize_t br_write_socket(const unsigned char *buf, size_t size,
                               const struct sockaddr *peer, socklen_t plen, void *ud)
{
    br_qconn_t *q = ud;
    ssize_t res;
    do {
        res = sendto(q->fd, buf, size, 0, peer, plen);
        if (res < 0 && errno == EAGAIN) return XQC_SOCKET_EAGAIN;
    } while (res < 0 && errno == EINTR);
    return res;
}

static ssize_t br_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size,
                                  const struct sockaddr *peer, socklen_t plen, void *ud)
{
    (void)path_id;
    return br_write_socket(buf, size, peer, plen, ud);
}

static void br_save_token(const unsigned char *token, uint32_t token_len, void *ud)
{
    (void)token; (void)token_len; (void)ud;
}

static void br_save_session(const char *data, size_t data_len, void *ud)
{
    (void)data; (void)data_len; (void)ud;
}

static void br_save_tp(const char *data, size_t data_len, void *ud)
{
    (void)data; (void)data_len; (void)ud;
}

static void br_update_cid(xqc_connection_t *conn, const xqc_cid_t *retire_cid,
                          const xqc_cid_t *new_cid, void *ud)
{
    (void)conn; (void)retire_cid;
    br_qconn_t *q = ud;
    memcpy(&q->cid, new_cid, sizeof(xqc_cid_t));
}

/* ---------- engine callbacks ---------- */
static void br_set_event_timer(xqc_usec_t wake_after, void *ud)
{
    br_ctx_t *c = ud;
    struct timeval tv;
    tv.tv_sec = (time_t)(wake_after / 1000000);
    tv.tv_usec = (suseconds_t)(wake_after % 1000000);
    event_add(c->ev_engine, &tv);
}

static void br_log_write(xqc_log_level_t lvl, const void *buf, size_t size, void *ud)
{
    (void)lvl;
    br_ctx_t *c = ud;
    if (c->logfp) {
        fwrite(buf, 1, size, c->logfp);
        fflush(c->logfp);
    }
}

static void br_qlog_write(qlog_event_importance_t imp, const void *buf, size_t size, void *ud)
{
    (void)imp; (void)buf; (void)size; (void)ud;
}

static void br_keylog(const xqc_cid_t *scid, const char *line, void *ud)
{
    (void)scid; (void)line; (void)ud;
}

static void br_engine_tick(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what;
    br_ctx_t *c = arg;
    xqc_engine_main_logic(c->engine);
}

/* ---------- udp read ---------- */
static void br_udp_read_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)what;
    br_qconn_t *q = arg;
    unsigned char buf[BR_PKT_BUF];

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&ss, &sl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        struct sockaddr_storage la;
        socklen_t lal = sizeof(la);
        getsockname(fd, (struct sockaddr *)&la, &lal);

        if (xqc_engine_packet_process(q->ctx->engine, buf, (size_t)n,
                (struct sockaddr *)&la, lal, (struct sockaddr *)&ss, sl,
                (xqc_msec_t)xqc_now(), q) != XQC_OK) {
            break;
        }
    }
    xqc_engine_finish_recv(q->ctx->engine);
}

/* ---------- local server ---------- */
static void br_timeout_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what;
    br_req_t *r = arg;
    if (r->responded || r->finished) return;
    br_fail(r, 504, "upstream timeout");
}

static void br_bev_read_cb(struct bufferevent *bev, void *arg)
{
    br_req_t *r = arg;
    if (r->parsed || r->responded || r->finished) return;

    struct evbuffer *in = bufferevent_get_input(bev);
    size_t len = evbuffer_get_length(in);
    if (len == 0) return;

    unsigned char *data = evbuffer_pullup(in, -1);
    if (!data) return;

    size_t consumed = 0;
    int ret = br_parse_http(r, (char *)data, len, &consumed);
    if (ret < 0) {
        br_fail(r, ret == -2 ? 413 : 400, ret == -2 ? "request too large" : "bad request");
        return;
    }
    if (ret == 0) return;

    evbuffer_drain(in, consumed);
    r->parsed = 1;

    /* 本地健康检查: GET /__health -> 200, 不进上游 */
    if (!strcmp(r->method, "GET") && !strcmp(r->path, "/__health")) {
        static const char health_body[] = "xqc-h3-bridge ok\n";
        r->finished = 1;
        br_build_and_write(r, 200, NULL, 0, health_body, sizeof(health_body) - 1,
                           "text/plain; charset=utf-8");
        br_schedule_close(r);
        br_maybe_free(r);
        return;
    }

    br_start_h3(r);
}

static void br_bev_write_cb(struct bufferevent *bev, void *arg)
{
    br_req_t *r = arg;
    if (r->responded && evbuffer_get_length(bufferevent_get_output(bev)) == 0) {
        r->bev_freed = 1;
        bufferevent_free(bev);
        r->bev = NULL;
        br_maybe_free(r);
    }
}

static void br_bev_event_cb(struct bufferevent *bev, short what, void *arg)
{
    br_req_t *r = arg;

    if (what & BEV_EVENT_ERROR) {
        if (!r->bev_freed) {
            r->bev_freed = 1;
            bufferevent_free(bev);
            r->bev = NULL;
        }
        if (!r->responded && !r->finished) {
            r->finished = 1;
            br_schedule_close(r);
        }
        br_maybe_free(r);
        return;
    }

    if (what & BEV_EVENT_EOF) {
        if (!r->parsed) {
            if (!r->bev_freed) {
                r->bev_freed = 1;
                bufferevent_free(bev);
                r->bev = NULL;
            }
            if (!r->responded && !r->finished) {
                r->finished = 1;
                br_schedule_close(r);
            }
            br_maybe_free(r);
        }
        /* 已解析完成: 客户端只是关了写侧, 继续等响应 */
        return;
    }

    if (what & BEV_EVENT_TIMEOUT) {
        br_timeout_cb(-1, 0, r);
    }
}

static void br_accept_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)what;
    br_ctx_t *c = arg;

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        evutil_make_socket_nonblocking(cfd);
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct bufferevent *bev = bufferevent_socket_new(c->eb, cfd, BEV_OPT_CLOSE_ON_FREE);
        if (!bev) { close(cfd); continue; }

        br_req_t *r = calloc(1, sizeof(*r));
        if (!r) { bufferevent_free(bev); continue; }
        r->ctx = c;
        r->bev = bev;

        bufferevent_setcb(bev, br_bev_read_cb, br_bev_write_cb, br_bev_event_cb, r);
        bufferevent_enable(bev, EV_READ | EV_WRITE);

        r->ev_timeout = event_new(c->eb, -1, 0, br_timeout_cb, r);
        struct timeval tv = { c->timeout_sec, 0 };
        event_add(r->ev_timeout, &tv);
    }
}

static void br_signal_cb(evutil_socket_t fd, short what, void *arg)
{
    (void)fd; (void)what;
    br_ctx_t *c = arg;
    event_base_loopbreak(c->eb);
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *listen_spec = "0.0.0.0:18080";
    const char *logpath = "/tmp/xqc_h3_bridge.log";
    int timeout_sec = 15;
    int debug = 0;

    int opt;
    while ((opt = getopt(argc, argv, "l:L:t:d")) != -1) {
        switch (opt) {
        case 'l': listen_spec = optarg; break;
        case 'L': logpath = optarg; break;
        case 't': timeout_sec = atoi(optarg); break;
        case 'd': debug = 1; break;
        default:
            fprintf(stderr, "usage: %s [-l ip:port] [-L logfile] [-t timeout_sec] [-d]\n", argv[0]);
            return 1;
        }
    }

    br_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return 1;
    c->debug = debug;
    c->timeout_sec = timeout_sec > 0 ? timeout_sec : 15;
    c->listen_fd = -1;
    c->logfp = fopen(logpath, "a");

    c->eb = event_base_new();
    if (!c->eb) { fprintf(stderr, "event_base_new failed\n"); return 1; }

    /* engine */
    xqc_engine_ssl_config_t essl;
    memset(&essl, 0, sizeof(essl));
    essl.ciphers = XQC_TLS_CIPHERS;
    essl.groups = BR_TLS_GROUPS;

    xqc_engine_callback_t ecbs;
    memset(&ecbs, 0, sizeof(ecbs));
    ecbs.set_event_timer = br_set_event_timer;
    ecbs.log_callbacks.xqc_log_write_err = br_log_write;
    ecbs.log_callbacks.xqc_log_write_stat = br_log_write;
    ecbs.log_callbacks.xqc_qlog_event_write = br_qlog_write;
    ecbs.keylog_cb = br_keylog;

    xqc_transport_callbacks_t tcbs;
    memset(&tcbs, 0, sizeof(tcbs));
    tcbs.write_socket = br_write_socket;
    tcbs.write_socket_ex = br_write_socket_ex;
    tcbs.save_token = br_save_token;
    tcbs.save_session_cb = br_save_session;
    tcbs.save_tp_cb = br_save_tp;
    tcbs.conn_update_cid_notify = br_update_cid;

    xqc_config_t xcfg;
    if (xqc_engine_get_default_config(&xcfg, XQC_ENGINE_CLIENT) < 0) {
        fprintf(stderr, "xqc_engine_get_default_config failed\n");
        return 1;
    }
    xcfg.cfg_log_level = debug ? XQC_LOG_DEBUG : XQC_LOG_WARN;

    c->engine = xqc_engine_create(XQC_ENGINE_CLIENT, &xcfg, &essl, &ecbs, &tcbs, c);
    if (!c->engine) { fprintf(stderr, "xqc_engine_create failed\n"); return 1; }

    xqc_h3_callbacks_t h3cbs;
    memset(&h3cbs, 0, sizeof(h3cbs));
    h3cbs.h3c_cbs.h3_conn_create_notify = br_h3_conn_create;
    h3cbs.h3c_cbs.h3_conn_close_notify = br_h3_conn_close;
    h3cbs.h3r_cbs.h3_request_create_notify = br_h3_req_create;
    h3cbs.h3r_cbs.h3_request_close_notify = br_h3_req_close;
    h3cbs.h3r_cbs.h3_request_read_notify = br_h3_read;
    h3cbs.h3r_cbs.h3_request_write_notify = br_h3_write;
    if (xqc_h3_ctx_init(c->engine, &h3cbs) != XQC_OK) {
        fprintf(stderr, "xqc_h3_ctx_init failed\n");
        return 1;
    }

    /* listener */
    char lspec[128];
    snprintf(lspec, sizeof(lspec), "%s", listen_spec);
    char *colon = strrchr(lspec, ':');
    if (!colon) { fprintf(stderr, "bad listen spec\n"); return 1; }
    *colon = 0;
    int lport = atoi(colon + 1);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { fprintf(stderr, "listen socket failed\n"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = htons((uint16_t)lport);
    if (inet_pton(AF_INET, lspec, &la.sin_addr) != 1) {
        fprintf(stderr, "bad listen ip: %s\n", lspec);
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) < 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        return 1;
    }
    if (listen(lfd, 128) < 0) { fprintf(stderr, "listen failed\n"); return 1; }
    evutil_make_socket_nonblocking(lfd);
    c->listen_fd = lfd;

    c->ev_listen = event_new(c->eb, lfd, EV_READ | EV_PERSIST, br_accept_cb, c);
    event_add(c->ev_listen, NULL);

    c->ev_engine = event_new(c->eb, -1, 0, br_engine_tick, c);
    struct timeval tv0 = { 0, 1000 };
    event_add(c->ev_engine, &tv0);

    struct event *ev_int = evsignal_new(c->eb, SIGINT, br_signal_cb, c);
    struct event *ev_term = evsignal_new(c->eb, SIGTERM, br_signal_cb, c);
    event_add(ev_int, NULL);
    event_add(ev_term, NULL);

    fprintf(stderr, "xqc-h3-bridge v1.9.4 listening on %s (timeout %ds)\n",
            listen_spec, c->timeout_sec);

    event_base_dispatch(c->eb);

    if (c->engine) xqc_engine_destroy(c->engine);
    if (c->ev_listen) { event_del(c->ev_listen); event_free(c->ev_listen); }
    if (lfd >= 0) close(lfd);
    if (c->ev_engine) { event_del(c->ev_engine); event_free(c->ev_engine); }
    event_base_free(c->eb);
    if (c->logfp) fclose(c->logfp);
    free(c);
    return 0;
}
