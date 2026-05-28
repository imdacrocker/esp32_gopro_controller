/*
 * captive_dns.c — wildcard DNS responder (see captive_dns.h).
 *
 * Adapted from the ESP-IDF captive_portal example's dns_server: a single
 * UDP socket on port 53 that rewrites every inbound query into a response
 * whose A records all point at the SoftAP IP. The AP IP is read once at
 * start from the "WIFI_AP_DEF" netif, so this stays in sync with whatever
 * wifi_manager configures (currently 10.71.79.1) without a hardcoded copy.
 */

#include "captive_dns.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "captive_dns";

#define DNS_PORT        53
#define DNS_MAX_LEN     256     /* enough for one question + our appended answers */
#define DNS_TTL_SECS    300

#define QR_FLAG         (1 << 7)   /* high byte of the 16-bit flags field */
#define QD_TYPE_A       0x0001

/* DNS wire-format structs. ptr_offset uses the 0xC0xx compression pointer
 * back to the question's name, so we never re-emit the name. */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t klass;
} dns_question_t;

typedef struct __attribute__((packed)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t klass;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

static uint32_t s_ap_ip;            /* network byte order, captured at start */
static bool     s_started;

/*
 * Walk a length-prefixed DNS name (e.g. 03 'g' 'p' 'o' ... 00) and write a
 * dotted string into out. Returns a pointer just past the terminating zero
 * byte (i.e. the start of the question's type/class), or NULL on overflow.
 */
static char *parse_dns_name(char *raw, char *out, size_t out_max)
{
    char *label = raw;
    char *itr   = out;
    int   len   = 0;

    do {
        int seg = *label;
        len += seg + 1;
        if (len > (int)out_max) {
            return NULL;
        }
        memcpy(itr, label + 1, seg);
        itr += seg;
        *itr++ = '.';
        label += seg + 1;
    } while (*label != 0);

    out[len - 1] = '\0';   /* overwrite trailing dot */
    return label + 1;
}

/*
 * Build a reply in-place from the request: flip the QR bit, set an_count,
 * and append one A answer per question, each pointing at s_ap_ip.
 * Returns reply length, or <=0 if the packet should be dropped.
 */
static int build_reply(const char *req, size_t req_len, char *reply, size_t reply_max)
{
    if (req_len < sizeof(dns_header_t) || req_len > reply_max) {
        return -1;
    }
    memcpy(reply, req, req_len);

    dns_header_t *hdr = (dns_header_t *)reply;
    if (hdr->flags & QR_FLAG) {
        return 0;   /* already a response — ignore */
    }
    hdr->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(hdr->qd_count);
    if (qd_count == 0) {
        return 0;
    }
    hdr->an_count = htons(qd_count);

    int reply_len = (int)req_len + qd_count * (int)sizeof(dns_answer_t);
    if (reply_len > (int)reply_max) {
        return -1;
    }

    char  name[128];
    char *qd_ptr  = reply + sizeof(dns_header_t);
    char *ans_ptr = reply + req_len;

    for (int i = 0; i < qd_count; i++) {
        char *after_name = parse_dns_name(qd_ptr, name, sizeof(name));
        if (after_name == NULL || after_name + sizeof(dns_question_t) > reply + req_len) {
            return -1;
        }
        dns_question_t *q = (dns_question_t *)after_name;
        uint16_t qtype  = ntohs(q->type);
        uint16_t qklass = ntohs(q->klass);

        dns_answer_t *ans = (dns_answer_t *)ans_ptr;
        ans->ptr_offset = htons(0xC000 | (uint16_t)(qd_ptr - reply));
        ans->type       = htons(qtype == QD_TYPE_A ? QD_TYPE_A : qtype);
        ans->klass      = htons(qklass);
        ans->ttl        = htonl(DNS_TTL_SECS);
        ans->addr_len   = htons(sizeof(uint32_t));
        ans->ip_addr    = s_ap_ip;

        ESP_LOGD(TAG, "resolved \"%s\" -> AP", name);

        qd_ptr   = (char *)q + sizeof(dns_question_t);
        ans_ptr += sizeof(dns_answer_t);
    }

    return reply_len;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    char rx[DNS_MAX_LEN];
    char tx[DNS_MAX_LEN];

    while (1) {
        struct sockaddr_in bind_addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() failed: errno=%d — retrying in 1 s", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "bind(:53) failed: errno=%d — retrying in 1 s", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "listening on udp/53");

        while (1) {
            struct sockaddr_storage src;
            socklen_t srclen = sizeof(src);
            int len = recvfrom(sock, rx, sizeof(rx), 0,
                               (struct sockaddr *)&src, &srclen);
            if (len < 0) {
                ESP_LOGW(TAG, "recvfrom failed: errno=%d — resetting socket", errno);
                break;
            }
            int tx_len = build_reply(rx, (size_t)len, tx, sizeof(tx));
            if (tx_len > 0) {
                sendto(sock, tx, tx_len, 0, (struct sockaddr *)&src, srclen);
            }
        }
        close(sock);
    }
}

void captive_dns_start(void)
{
    if (s_started) {
        return;
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = {};
    if (ap == NULL || esp_netif_get_ip_info(ap, &ip) != ESP_OK || ip.ip.addr == 0) {
        ESP_LOGE(TAG, "AP netif/IP unavailable — DNS responder not started");
        return;
    }
    s_ap_ip = ip.ip.addr;   /* esp_ip4_addr_t is already network byte order */

    BaseType_t ok = xTaskCreate(dns_server_task, "captive_dns", 4096,
                                NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed — DNS responder not started");
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "started, answering all A queries with " IPSTR, IP2STR(&ip.ip));
}
