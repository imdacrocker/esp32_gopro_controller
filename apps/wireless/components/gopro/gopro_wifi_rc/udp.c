/*
 * udp.c — UDP socket init, TX wrappers (keepalive, status, shutter, WoL),
 * and the RX task that dispatches incoming datagrams to per-slot state.
 *
 * Single bound socket: local port 8383 for both TX (so the camera replies to
 * the canonical "WiFi Remote" source port) and RX (camera's reply lands here).
 *
 * RX dispatch (§17.4.1):
 *   - buf[0] == 0x5F                            → keepalive ACK
 *   - buf[11] == 's' && buf[12] == 't' && n>=16 → status response
 *   - anything else                              → log/discard
 *
 * Either path updates ctx->last_response_tick and posts CMD_PROMOTE if the
 * slot is not yet wifi_ready.  Status-response decode is delegated to
 * rc_parse_st_response() in status.c.
 *
 * §17.2, §17.4, §17.6 of docs/design/camera-manager.md.
 */

#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_manager.h"
#include "gopro_wifi_rc_internal.h"

static const char *TAG = "gopro_rc/udp";

/* ---- UDP socket init ----------------------------------------------------- */

/*
 * Open the single UDP socket used for keepalive unicasts, status requests,
 * shutter commands, WoL broadcasts, and receiving replies.
 *
 * Bound to RC_UDP_RX_PORT (8383) so the keepalive's *source* port is 8383 —
 * Hero3/4 reply to the source port (standard UDP) and won't accept a remote
 * whose keepalive originates from an arbitrary ephemeral port.  The RX task
 * (rc_udp_rx_task) reads replies from this same socket.
 *
 * SO_BROADCAST is required for WoL.  SO_REUSEADDR allows a clean re-bind
 * after firmware restart without waiting for the kernel TIME_WAIT.
 */
esp_err_t rc_udp_init(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return ESP_FAIL;
    }

    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "setsockopt SO_BROADCAST failed: %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_RX_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() port %d failed: %d", RC_UDP_RX_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }

    s_udp_sock = sock;
    ESP_LOGD(TAG, "UDP socket opened (fd=%d, src/rx port %d)", sock, RC_UDP_RX_PORT);
    return ESP_OK;
}

/* ---- TX helpers ---------------------------------------------------------- */

/* DEBUG-TRACE: temporary INFO-level logging on every send so we can trace
 * the legacy-camera protocol on hardware.  Lower to LOGD once stable. */
static void log_tx(const char *what, uint32_t ip, size_t len)
{
    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    ESP_LOGD(TAG, "TX %s → %s:%d (%u bytes)",
             what, ip_str, RC_UDP_TX_PORT, (unsigned)len);
}

static void send_to_camera(uint32_t ip, const void *payload, size_t len)
{
    if (s_udp_sock < 0 || ip == 0) return;

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_TX_PORT),
        .sin_addr.s_addr = ip,
    };
    int sent = sendto(s_udp_sock, payload, len, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) {
        ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
    }
}

void rc_send_keepalive(uint32_t ip)
{
    const char *payload = RC_UDP_KEEPALIVE_PAYLOAD;
    size_t len = strlen(payload);
    log_tx("keepalive", ip, len);
    send_to_camera(ip, payload, len);
}

void rc_send_st(uint32_t ip)
{
    log_tx("st", ip, sizeof(RC_PKT_ST));
    send_to_camera(ip, RC_PKT_ST, sizeof(RC_PKT_ST));
}

void rc_send_sh(uint32_t ip, bool start)
{
    const uint8_t *pkt = start ? RC_PKT_SH_START : RC_PKT_SH_STOP;
    size_t len = start ? sizeof(RC_PKT_SH_START) : sizeof(RC_PKT_SH_STOP);
    log_tx(start ? "SH-start" : "SH-stop", ip, len);
    send_to_camera(ip, pkt, len);
}

void rc_send_cv(uint32_t ip)
{
    log_tx("cv", ip, sizeof(RC_PKT_CV));
    send_to_camera(ip, RC_PKT_CV, sizeof(RC_PKT_CV));
}

/* ---- Wake-on-LAN TX ------------------------------------------------------ */

/*
 * Send RC_WOL_BURST standard magic packets to 255.255.255.255:9 targeting mac.
 * Magic packet = 6 × 0xFF followed by the target MAC repeated 16 times (102 B).
 */
void rc_send_wol(uint32_t ip, const uint8_t mac[6])
{
    if (s_udp_sock < 0) return;

    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RC_UDP_WOL_PORT),
        .sin_addr.s_addr = IPADDR_BROADCAST,
    };

    for (int i = 0; i < RC_WOL_BURST; i++) {
        sendto(s_udp_sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dst, sizeof(dst));
    }

    char ip_str[16];
    ip4_addr_t a = { .addr = ip };
    ip4addr_ntoa_r(&a, ip_str, sizeof(ip_str));
    ESP_LOGD(TAG, "WoL burst ×%d → %s (mac %02x:%02x:%02x:%02x:%02x:%02x)",
             RC_WOL_BURST, ip_str,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- UDP RX task --------------------------------------------------------- */

/*
 * Find the slot whose ctx->last_ip matches src_ip (and last_ip != 0).
 * Returns -1 if no slot is currently associated with that source IP.
 */
static int find_slot_by_ip(uint32_t src_ip)
{
    for (int i = 0; i < CAMERA_MAX_SLOTS; i++) {
        if (s_ctx[i].last_ip == src_ip && s_ctx[i].last_ip != 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Common bookkeeping for any datagram we receive from a known slot's IP:
 *   - Refresh last_response_tick (drives the WoL silence watchdog)
 *   - If the slot is not yet wifi_ready, post CMD_PROMOTE so the work task
 *     flips the flag, applies cv data if it has already arrived (or nudges
 *     another `cv` if not), and sends date/time.
 *
 * last_response_tick is a TickType_t (32-bit on Xtensa LX7); aligned 32-bit
 * stores are atomic, so the single-writer (this task) / single-reader
 * (work task in keepalive_tick) pattern needs no mutex.
 */
static void on_response_from_slot(int slot)
{
    s_ctx[slot].last_response_tick = xTaskGetTickCount();

    if (!s_ctx[slot].wifi_ready) {
        rc_work_cmd_t cmd = { .type = RC_CMD_PROMOTE,
                              .slot_cmd = { .slot = slot } };
        xQueueSend(s_work_queue, &cmd, 0);
    }
}

/*
 * Loop forever reading from the shared s_udp_sock.  Dispatch by content:
 *   - buf[0] == 0x5F                              → keepalive ACK
 *   - buf[11..12] == "st" and n >= 16             → status response
 *                                                   (bytes 13/14/15 decoded
 *                                                    by rc_parse_st_response)
 *   - anything else                               → log at VERBOSE, discard
 */
void rc_udp_rx_task(void *arg)
{
    (void)arg;

    /* rc_udp_init() runs before this task is created, so s_udp_sock is valid. */
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "RX task: shared socket not initialised");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RX task started on shared socket (fd=%d, port %d)",
             s_udp_sock, RC_UDP_RX_PORT);

    uint8_t buf[64];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (1) {
        int n = recvfrom(s_udp_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n <= 0) continue;

        uint32_t src_ip = src.sin_addr.s_addr;

        /* DEBUG-TRACE: log every received datagram with source, length, and
         * a few key bytes (first byte + bytes 11-12 if it's a binary frame).
         * Lower to LOGD once the legacy protocol is stable on hardware. */
        char src_str[16];
        ip4_addr_t src_a = { .addr = src_ip };
        ip4addr_ntoa_r(&src_a, src_str, sizeof(src_str));

        char opcode_str[8] = "??";
        if (n >= RC_RESP_OPCODE_OFFSET + 2) {
            uint8_t o0 = buf[RC_RESP_OPCODE_OFFSET];
            uint8_t o1 = buf[RC_RESP_OPCODE_OFFSET + 1];
            if (o0 >= ' ' && o0 < 127 && o1 >= ' ' && o1 < 127) {
                snprintf(opcode_str, sizeof(opcode_str), "%c%c", o0, o1);
            } else {
                snprintf(opcode_str, sizeof(opcode_str), "%02x%02x", o0, o1);
            }
        }
        ESP_LOGD(TAG, "RX %s:%d (%d bytes) first=0x%02x op='%s'",
                 src_str, ntohs(src.sin_port), n, buf[0], opcode_str);

        /* Hex+ASCII dump at DEBUG so it's available when troubleshooting a
         * new camera but doesn't spam during normal operation. */
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, n, ESP_LOG_DEBUG);

        int slot = find_slot_by_ip(src_ip);
        if (slot < 0) {
            ESP_LOGI(TAG, "  └ unknown source — ignored");
            continue;
        }

        /* Keepalive ACK — first byte is `_` (0x5F). */
        if (buf[0] == RC_UDP_KEEPALIVE_ACK_BYTE) {
            ESP_LOGD(TAG, "  └ slot %d: keepalive ACK", slot);
            on_response_from_slot(slot);
            continue;
        }

        /* Status response — opcode "st" in bytes 11-12, full frame ≥ 16 B. */
        if (n >= RC_RESP_MIN_BYTES &&
            buf[RC_RESP_OPCODE_OFFSET]     == 's' &&
            buf[RC_RESP_OPCODE_OFFSET + 1] == 't') {
            ESP_LOGD(TAG, "  └ slot %d: st response (pwr=%u mode=%u state=%u)",
                     slot,
                     (unsigned)buf[RC_RESP_PWR_OFFSET],
                     (unsigned)buf[RC_RESP_MODE_OFFSET],
                     (unsigned)buf[RC_RESP_STATE_OFFSET]);
            rc_parse_st_response(slot, buf, n);
            on_response_from_slot(slot);
            continue;
        }

        /* Shutter ACK echo — opcode "SH" in bytes 11-12, byte 13 echoes our
         * parameter (0x02 start, 0x00 stop).  Not used to drive state — we
         * track recording_status from `st` responses — but it IS a liveness
         * signal worth refreshing last_response_tick on. */
        if (n >= RC_RESP_OPCODE_OFFSET + 2 &&
            buf[RC_RESP_OPCODE_OFFSET]     == 'S' &&
            buf[RC_RESP_OPCODE_OFFSET + 1] == 'H') {
            ESP_LOGI(TAG, "  └ slot %d: SH ACK (param=0x%02x)",
                     slot,
                     n > RC_RESP_OPCODE_OFFSET + 2 ?
                         (unsigned)buf[RC_RESP_OPCODE_OFFSET + 2] : 0);
            on_response_from_slot(slot);
            continue;
        }

        /* Camera-version response — opcode "cv" in bytes 11-12, variable
         * length, length-prefixed firmware + model_name strings.  Decoded by
         * rc_parse_cv_response() in status.c, which posts RC_CMD_APPLY_CV
         * so the work task maps the model_name to camera_model_t and
         * persists it via set_model + save_slot. */
        if (n > RC_CV_RESP_FW_LEN_OFFSET &&
            buf[RC_RESP_OPCODE_OFFSET]     == 'c' &&
            buf[RC_RESP_OPCODE_OFFSET + 1] == 'v') {
            ESP_LOGI(TAG, "  └ slot %d: cv response (%d bytes)", slot, n);
            rc_parse_cv_response(slot, buf, n);
            on_response_from_slot(slot);
            continue;
        }

        ESP_LOGI(TAG, "  └ slot %d: unhandled opcode '%s'", slot, opcode_str);
    }
}
