/* dns_hijack.c — minimal captive-portal DNS responder. See dns_hijack.h. */

#include "dns_hijack.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns_hijack";

#define DNS_ANS_LEN 16   /* name-ptr(2)+type(2)+class(2)+ttl(4)+rdlen(2)+ipv4(4) */

static TaskHandle_t   s_task;
static volatile bool  s_run;
static uint32_t       s_resolve_ip_be;   /* network-order IPv4 to answer with */

/* Parse + answer one DNS query. Returns the response length in `out` (cap
 * bytes), or <0 to drop. We only ever read the first question — phones send
 * QDCOUNT=1 — and reply with a single A record (or NODATA for non-A). */
static int build_reply(const uint8_t *q, int qn, uint8_t *out, int cap) {
    if (qn < 12 || cap < 12)
        return -1;
    /* Walk the QNAME labels (length-prefixed, terminated by a 0 byte). DNS
     * queries don't use name compression, so no 0xC0 pointer handling. */
    int p = 12;
    while (p < qn && q[p] != 0)
        p += q[p] + 1;
    if (p >= qn)
        return -1;            /* malformed / unterminated name */
    p += 1;                   /* past the 0 terminator */
    if (p + 4 > qn)
        return -1;            /* need QTYPE + QCLASS */
    int qtype = (q[p] << 8) | q[p + 1];
    int qend  = p + 4;        /* end of the question section */

    /* Answer (or NODATA) for the first question only: header + that question
     * + optionally one A record (name pointer 0xC00C → offset 12). */
    if (qend + ((qtype == 1) ? DNS_ANS_LEN : 0) > cap)
        return -1;
    memcpy(out, q, qend);
    out[2] = 0x81; out[3] = 0x80;           /* QR=1, RD copied, RA=1, rcode 0 */
    out[4] = 0x00; out[5] = 0x01;           /* QDCOUNT = 1 */
    out[8] = out[9] = out[10] = out[11] = 0;/* NSCOUNT = ARCOUNT = 0 (drop EDNS) */
    int n = qend;
    if (qtype == 1) {                        /* A → point it at us */
        out[6] = 0x00; out[7] = 0x01;        /* ANCOUNT = 1 */
        const uint8_t ans[DNS_ANS_LEN] = {
            0xC0, 0x0C,                      /* name → offset 12 (the question) */
            0x00, 0x01,                      /* TYPE  A */
            0x00, 0x01,                      /* CLASS IN */
            0x00, 0x00, 0x00, 0x3C,          /* TTL 60 s */
            0x00, 0x04,                      /* RDLENGTH 4 */
            0, 0, 0, 0,                      /* RDATA filled below */
        };
        memcpy(out + n, ans, DNS_ANS_LEN);
        memcpy(out + n + 12, &s_resolve_ip_be, 4);
        n += DNS_ANS_LEN;
    } else {
        out[6] = 0x00; out[7] = 0x00;        /* ANCOUNT = 0 → NODATA (IPv4 fallback) */
    }
    return n;
}

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :53: errno %d", errno);
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* 1 s recv timeout so the loop re-checks s_run for a clean stop. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const uint8_t *o = (const uint8_t *)&s_resolve_ip_be;  /* net-order octets */
    ESP_LOGI(TAG, "captive DNS up on :53 (all A → %u.%u.%u.%u)",
             o[0], o[1], o[2], o[3]);

    uint8_t rx[512], tx[512];
    while (s_run) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &sl);
        if (n < 0)
            continue;          /* timeout (re-check s_run) or transient error */
        int rl = build_reply(rx, n, tx, sizeof(tx));
        if (rl > 0)
            sendto(sock, tx, rl, 0, (struct sockaddr *)&src, sl);
    }
    close(sock);
    ESP_LOGI(TAG, "captive DNS stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(const char *resolve_ip) {
    if (s_task)
        return ESP_OK;
    if (!resolve_ip)
        return ESP_ERR_INVALID_ARG;
    esp_ip4_addr_t a = {0};
    esp_netif_str_to_ip4(resolve_ip, &a);
    s_resolve_ip_be = a.addr;
    s_run = true;
    if (xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void dns_hijack_stop(void) {
    s_run = false;   /* task notices within the recv timeout, then self-deletes */
}
