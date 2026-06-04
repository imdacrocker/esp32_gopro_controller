/*
 * diagnostic.c — Temporary network probe orchestrator for the gopro_wifi_rc
 * component.  While wired up, /api/rc/add dispatches into gopro_wifi_rc_diagnose()
 * instead of the normal pair flow, so clicking "Add" on a discovered camera
 * runs:
 *
 *   1. ICMP ping (5 packets, 1 s timeout each)
 *   2. TCP port sweep across common GoPro / general-purpose ports
 *   3. HTTP/1.1 GET against several known GoPro endpoint paths on any HTTP-ish
 *      TCP port that opened in step 2
 *   4. Extra UDP opcode probes (`cv` in both selector forms, `wt`); replies
 *      get logged by the existing rc_udp_rx_task
 *
 * Periodic UDP keepalives are sent between phases so the camera continues to
 * regard us as a valid Smart Remote during the probe.
 *
 * Revert: in components/http_server/api_rc.c, swap gopro_wifi_rc_diagnose()
 * back to gopro_wifi_rc_add_camera().  This file can stay — it's harmless
 * when nothing calls it.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "gopro_wifi_rc.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/diag";

/* ---- ICMP ping ----------------------------------------------------------- */

static void diag_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint8_t  ttl;
    uint16_t seqno;
    uint32_t elapsed_ms, recv_len;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,      &seqno,      sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,        &ttl,        sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE,       &recv_len,   sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP,    &elapsed_ms, sizeof(elapsed_ms));
    ESP_LOGI(TAG, "  ping seq=%u ttl=%u time=%lu ms (%lu bytes)",
             (unsigned)seqno, (unsigned)ttl,
             (unsigned long)elapsed_ms, (unsigned long)recv_len);
}

static void diag_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGI(TAG, "  ping seq=%u TIMEOUT", (unsigned)seqno);
}

static void diag_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    SemaphoreHandle_t done = (SemaphoreHandle_t)args;
    uint32_t tx, rx, dur;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,  &tx,  sizeof(tx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,    &rx,  sizeof(rx));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &dur, sizeof(dur));
    ESP_LOGI(TAG, "ping summary: %lu sent / %lu received in %lu ms",
             (unsigned long)tx, (unsigned long)rx, (unsigned long)dur);
    xSemaphoreGive(done);
}

static void diag_ping(uint32_t ip)
{
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "[ping] %s — 5 packets, 1 s timeout each", ip_str);

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type           = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip;
    cfg.count        = 5;
    cfg.interval_ms  = 500;
    cfg.timeout_ms   = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = diag_on_ping_success,
        .on_ping_timeout = diag_on_ping_timeout,
        .on_ping_end     = diag_on_ping_end,
        .cb_args         = done,
    };

    esp_ping_handle_t handle;
    if (esp_ping_new_session(&cfg, &cbs, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "  ping session create failed");
        vSemaphoreDelete(done);
        return;
    }
    esp_ping_start(handle);
    xSemaphoreTake(done, portMAX_DELAY);
    esp_ping_delete_session(handle);
    vSemaphoreDelete(done);
}

/* ---- TCP port scan ------------------------------------------------------- */

/*
 * Returns:
 *   1 = SYN-ACK received within timeout (port open)
 *   0 = no SYN-ACK before timeout (filtered/closed) or active reject
 *  -1 = local error (couldn't even create socket)
 */
static int diag_port_scan_one(uint32_t ip, int port, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = ip,
    };

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        close(sock);
        return 1;  /* immediate connect success */
    }
    if (errno != EINPROGRESS) {
        close(sock);
        return 0;  /* immediate refusal — closed */
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (sel <= 0) {
        close(sock);
        return 0;  /* timeout / no SYN-ACK */
    }

    int err = 0;
    socklen_t err_len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
    close(sock);
    return (err == 0) ? 1 : 0;
}

/* Common GoPro / general TCP ports of interest. */
static const int k_ports[] = {
    21,   22,   23,    /* legacy */
    80,                 /* HTTP */
    443,                /* HTTPS */
    554,                /* RTSP */
    1935,               /* RTMP */
    8000, 8080, 8081, 8082, 8090, /* HTTP alt */
    8443,               /* HTTPS alt */
    8484,               /* GoPro UDP RC port (TCP attempt for symmetry) */
    8554,               /* RTSP alt; python lib uses for streaming keepalive */
    8888,               /* HTTP alt */
    50080,              /* GoPro Hero5+ alt observed in some docs */
};

static int diag_port_scan(uint32_t ip, int *open_ports, int max_open)
{
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    int n_total = (int)(sizeof(k_ports) / sizeof(k_ports[0]));
    ESP_LOGI(TAG, "[port-scan] %s — %d ports, 250 ms each "
                  "(keepalive every 4)", ip_str, n_total);

    int n_open = 0;
    for (int i = 0; i < n_total; i++) {
        /* Inject a keepalive every 4 ports so the camera doesn't time us out
         * during the long scan (~4 s total at 250 ms/port).  The legacy
         * Smart-Remote watchdog on Hero7 is sub-9 s; without this, the
         * camera goes silent before later probes can run. */
        if (i > 0 && (i % 4) == 0) {
            rc_send_keepalive(ip);
        }

        int port = k_ports[i];
        int r = diag_port_scan_one(ip, port, 250);
        ESP_LOGI(TAG, "  TCP/%-5d %s", port, r == 1 ? "OPEN" : "closed/filtered");
        if (r == 1 && n_open < max_open) {
            open_ports[n_open++] = port;
        }
    }
    ESP_LOGI(TAG, "port scan: %d open of %d tested", n_open, n_total);
    return n_open;
}

/* ---- HTTP probes on responding ports ------------------------------------- */

static const char *k_http_paths[] = {
    "/gp/gpControl",
    "/gp/gpControl/status",
    "/camera/cv",
    "/bacpac/sd",
    "/",
};

static void diag_http_probe(uint32_t ip, int port, const char *path)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = ip,
    };

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        ESP_LOGI(TAG, "    %s -> connect refused errno=%d", path, errno);
        close(sock);
        return;
    }
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            ESP_LOGI(TAG, "    %s -> connect timeout", path);
            close(sock);
            return;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err != 0) {
            ESP_LOGI(TAG, "    %s -> connect failed errno=%d", path, err);
            close(sock);
            return;
        }
    }

    /* Restore blocking mode + per-op timeouts for send/recv. */
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));

    /* HTTP/1.1 + Connection: close — many cameras prefer 1.1 with explicit
     * close.  Worth comparing against 1.0 if every response is 5xx / empty. */
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, ip_str);
    if (send(sock, req, rl, 0) < 0) {
        ESP_LOGI(TAG, "    %s -> send failed errno=%d", path, errno);
        close(sock);
        return;
    }

    char resp[1024];
    int total = 0;
    int n;
    while ((n = recv(sock, resp + total, sizeof(resp) - 1 - total, 0)) > 0) {
        total += n;
        if (total >= (int)sizeof(resp) - 1) break;
    }
    resp[total] = '\0';
    close(sock);

    if (total == 0) {
        ESP_LOGI(TAG, "    %s -> empty response", path);
        return;
    }

    int status = -1;
    if (strncmp(resp, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) status = atoi(sp + 1);
    }

    const char *body = strstr(resp, "\r\n\r\n");
    int body_off = body ? (int)((body - resp) + 4) : total;
    int body_len = total - body_off;

    ESP_LOGI(TAG, "    %s -> HTTP %d (%d bytes total, %d body)",
             path, status, total, body_len);
    if (body && body_len > 0) {
        int show = body_len > 200 ? 200 : body_len;
        ESP_LOGI(TAG, "      body: %.*s%s", show, resp + body_off,
                 body_len > 200 ? "…" : "");
    }
}

static bool port_looks_http(int port)
{
    return port == 80    || port == 8080 || port == 8081 || port == 8082 ||
           port == 8090  || port == 8000 || port == 8888 || port == 50080;
}

static void diag_http_all_paths(uint32_t ip, int port)
{
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    int n = (int)(sizeof(k_http_paths) / sizeof(k_http_paths[0]));
    ESP_LOGI(TAG, "[http] http://%s:%d — probing %d paths", ip_str, port, n);

    for (int i = 0; i < n; i++) {
        diag_http_probe(ip, port, k_http_paths[i]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- UDP opcode probes (replies appear in the rx-task INFO log) ---------- */

static void diag_udp_send(uint32_t ip, const char *what,
                           const uint8_t *pkt, size_t len)
{
    if (s_udp_sock < 0 || ip == 0) return;
    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_TX_PORT),
        .sin_addr.s_addr = ip,
    };
    ESP_LOGI(TAG, "  TX %s (%u bytes)", what, (unsigned)len);
    sendto(s_udp_sock, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void diag_udp(uint32_t ip)
{
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "[udp] %s — opcode probes (replies in rx-task log)", ip_str);

    /* `cv` (camera version) — two selector forms.  ESP8266 reference
     * comments call out "response contains MAC and SSID" for byte 8 = 0x01.
     * 1 s receive window after each so the rx task gets a chance to log
     * the reply before we move on. */
    static const uint8_t pkt_cv0[13] = {
        0,0,0,0,0,0,0,0, 0x00, 0,0, 'c','v'
    };
    static const uint8_t pkt_cv1[13] = {
        0,0,0,0,0,0,0,0, 0x01, 0,0, 'c','v'
    };
    /* `wt` (wifi keepalive variant from ESP8266 sketch heartbeat). */
    static const uint8_t pkt_wt[13] = {
        0,0,0,0,0,0,0,0, 0x01, 0,0, 'w','t'
    };

    diag_udp_send(ip, "cv (sel=0)", pkt_cv0, sizeof(pkt_cv0));
    vTaskDelay(pdMS_TO_TICKS(1000));
    diag_udp_send(ip, "cv (sel=1)", pkt_cv1, sizeof(pkt_cv1));
    vTaskDelay(pdMS_TO_TICKS(1000));
    diag_udp_send(ip, "wt (sel=1)", pkt_wt, sizeof(pkt_wt));
    vTaskDelay(pdMS_TO_TICKS(500));
}

/* ---- Orchestrator -------------------------------------------------------- */

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;
} diag_args_t;

static void diagnostic_task(void *arg)
{
    diag_args_t *args = (diag_args_t *)arg;
    uint32_t       ip  = args->ip;
    const uint8_t *mac = args->mac;

    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));

    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "DIAGNOSTIC start: %s mac=%02x:%02x:%02x:%02x:%02x:%02x",
             ip_str, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "============================================================");

    /* Prime the camera so it considers us a valid Smart Remote during tests. */
    rc_send_keepalive(ip);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Phase order is intentional: UDP opcode probes first, while the camera
     * is freshest from pairing.  ICMP/port-scan/HTTP run after — their
     * value is lower (we already know TCP is closed on Hero7) and they're
     * the long-tail tests most likely to lose camera attention. */

    /* 1. UDP opcode probes (cv response is the prize — gives model + fw). */
    diag_udp(ip);

    rc_send_keepalive(ip);

    /* 2. ICMP ping. */
    diag_ping(ip);

    rc_send_keepalive(ip);

    /* 3. TCP port sweep (with embedded keepalives every 4 ports). */
    int open_ports[8];
    int n_open = diag_port_scan(ip, open_ports, 8);

    rc_send_keepalive(ip);

    /* 4. HTTP probes against any HTTP-ish open port. */
    bool any_http_probed = false;
    for (int i = 0; i < n_open; i++) {
        if (port_looks_http(open_ports[i])) {
            diag_http_all_paths(ip, open_ports[i]);
            any_http_probed = true;
            rc_send_keepalive(ip);
        }
    }
    if (!any_http_probed) {
        ESP_LOGI(TAG, "[http] no HTTP-ish TCP ports open — skipping path probes");
    }

    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "DIAGNOSTIC done — %s, %d open TCP ports", ip_str, n_open);
    ESP_LOGI(TAG, "============================================================");

    free(args);
    vTaskDelete(NULL);
}

void gopro_wifi_rc_diagnose(const uint8_t mac[6], uint32_t ip)
{
    diag_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ESP_LOGE(TAG, "diagnose: malloc failed");
        return;
    }
    memcpy(args->mac, mac, 6);
    args->ip = ip;

    BaseType_t ok = xTaskCreate(diagnostic_task, "rc_diag",
                                 8192, args, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "diagnose: task creation failed");
        free(args);
    }
}
