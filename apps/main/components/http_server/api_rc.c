/*
 * api_rc.c — RC-emulation camera API handlers (§20.4).
 *
 * Endpoints:
 *   GET  /api/rc/discovered
 *   POST /api/rc/add
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "wifi_manager.h"
#include "gopro_wifi_rc.h"
#include "camera_manager.h"
#include "http_server_internal.h"

static const char *TAG = "http_api_rc";

/* GoPro OUIs (IEEE MA-L registrations to Woodman Labs / GoPro).
 * Used to filter SoftAP stations down to GoPro cameras only.
 * Add new prefixes here as IEEE registers them or as we observe them in the wild. */
static const uint8_t GOPRO_RC_OUIS[][3] = {
    { 0x04, 0x41, 0x69 },   /* registered 2015-11-17 */
    { 0x04, 0x57, 0x47 },   /* registered 2022-05-07 */
    { 0x24, 0x74, 0xF7 },   /* registered 2019-08-07 */
    { 0xAC, 0x04, 0xAA },   /* registered 2024-09-04 */
    { 0xD4, 0x32, 0x60 },   /* registered 2018-07-28 */
    { 0xD4, 0xD9, 0x19 },   /* registered 2013-09-12 */
    { 0xD8, 0x96, 0x85 },   /* registered 2011-08-05 */
    { 0xF4, 0xDD, 0x9E },   /* registered 2014-04-22 */
};

static bool mac_has_gopro_oui(const uint8_t mac[6])
{
    for (size_t i = 0; i < sizeof(GOPRO_RC_OUIS) / sizeof(GOPRO_RC_OUIS[0]); i++) {
        if (memcmp(mac, GOPRO_RC_OUIS[i], 3) == 0) return true;
    }
    return false;
}

/*
 * GET /api/rc/discovered
 *
 * Returns SoftAP stations that are NOT yet in a managed RC-emulation slot —
 * i.e. cameras connected to the AP that the user can add via /api/rc/add.
 */
static esp_err_t handler_rc_discovered(httpd_req_t *req)
{
    wifi_mgr_sta_info_t stations[AP_MAX_CONN];
    int n = wifi_manager_get_connected_stations(stations, AP_MAX_CONN);

    /* Max entry ~60 bytes; AP_MAX_CONN entries + brackets = ~400. */
    const size_t BUF_SIZE = 512;
    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t pos = 0;
    buf[pos++] = '[';
    int first = 1;

    for (int i = 0; i < n; i++) {
        /* Only show GoPro RC-capable cameras — filter by OUI. */
        if (!mac_has_gopro_oui(stations[i].mac)) continue;
        /* And only those not already managed by the RC driver. */
        if (gopro_wifi_rc_is_managed_mac(stations[i].mac)) continue;

        char mac_str[18];
        format_mac(mac_str, stations[i].mac);

        char ip_str[16];
        format_ip(ip_str, sizeof(ip_str), stations[i].ip_addr);

        int written = snprintf(buf + pos, BUF_SIZE - pos,
            "%s{\"addr\":\"%s\",\"ip\":\"%s\"}",
            first ? "" : ",",
            mac_str, ip_str);

        if (written < 0 || (size_t)written >= BUF_SIZE - pos) break;
        pos += (size_t)written;
        first = 0;
    }

    if (pos < BUF_SIZE - 1) buf[pos++] = ']';
    buf[pos] = '\0';

    send_json(req, buf);
    free(buf);
    return ESP_OK;
}

/*
 * POST /api/rc/add
 * Body: { "addr": "XX:XX:XX:XX:XX:XX", "ip": "10.71.79.X" }
 *
 * Registers the station as an RC-emulation camera (initial model =
 * HERO_LEGACY_RC) and primes it with a UDP keepalive + `st` + `cv` burst.
 * The camera's `cv` reply (when it arrives) settles the actual model and
 * display name.  The MAC must already be associated with the SoftAP (have
 * a DHCP lease).
 */
static esp_err_t handler_rc_add(httpd_req_t *req)
{
    if (reject_if_shutting_down(req) != ESP_OK) return ESP_FAIL;

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *addr_item = cJSON_GetObjectItem(root, "addr");
    cJSON *ip_item   = cJSON_GetObjectItem(root, "ip");

    if (!cJSON_IsString(addr_item) || !cJSON_IsString(ip_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr or ip");
        return ESP_FAIL;
    }

    uint8_t mac[6];
    if (!parse_mac(cJSON_GetStringValue(addr_item), mac)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid addr");
        return ESP_FAIL;
    }

    uint32_t ip = parse_ip(cJSON_GetStringValue(ip_item));
    cJSON_Delete(root);

    if (ip == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ip");
        return ESP_FAIL;
    }

    /* Reserve the pair-attempt state machine (shared with the BLE pair flow)
     * so the web UI can poll /api/pair/status for "Connecting / Success /
     * Failed" updates and so the watchdog removes the slot if the camera
     * never responds to UDP.  If a previous attempt is still in flight,
     * refuse with 409. */
    esp_err_t err = pair_attempt_begin(mac, 0, PAIR_TRANSPORT_WIFI_RC);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"pair already in flight\"}");
        return ESP_FAIL;
    }

    gopro_wifi_rc_add_camera(mac, ip);
    ESP_LOGI(TAG, "rc/add: registering camera (ip 0x%08lx)", (unsigned long)ip);
    send_json(req, "{}");
    return ESP_OK;
}

/* ---- Registration -------------------------------------------------------- */

void api_rc_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/rc/discovered", .method = HTTP_GET,  .handler = handler_rc_discovered },
        { .uri = "/api/rc/add",        .method = HTTP_POST, .handler = handler_rc_add        },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
