/* test_device_url.c — host unit test for device_url() (device_id.c).
 *
 * device_url() builds the board's own HTTPS web-UI URL, used by the OLED
 * web-QR page + the /oled/qr default. The MAC (and thus the id) is pinned by
 * the esp_mac.h stub to ab:cd:ef → "cb-abcdef" (neutral, scrub-inert). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_id.h"

int main(void) {
    char buf[96];

    /* id + suffix are deterministic from the stubbed MAC */
    assert(strcmp(device_id(), "cb-abcdef") == 0);
    assert(strcmp(device_id_suffix(), "abcdef") == 0);

    /* with a domain: https://<id>.<domain>/  (the user's example host) */
    device_url(buf, sizeof(buf), "home.lan");
    printf("url(home.lan)  = %s\n", buf);
    assert(strcmp(buf, "https://cb-abcdef.home.lan/") == 0);

    /* empty / NULL domain: fall back to the bare hostname */
    device_url(buf, sizeof(buf), "");
    printf("url(\"\")    = %s\n", buf);
    assert(strcmp(buf, "https://cb-abcdef/") == 0);

    device_url(buf, sizeof(buf), NULL);
    assert(strcmp(buf, "https://cb-abcdef/") == 0);

    /* a longer domain still composes correctly */
    device_url(buf, sizeof(buf), "example.com");
    assert(strcmp(buf, "https://cb-abcdef.example.com/") == 0);

    /* tiny buffer must truncate, never overflow */
    char tiny[10];
    device_url(tiny, sizeof(tiny), "home.lan");
    assert(strlen(tiny) < sizeof(tiny));

    /* zero cap is a safe no-op (must not write) */
    char canary[4] = {'x', 'x', 'x', 'x'};
    device_url(canary, 0, "home.lan");
    assert(canary[0] == 'x');

    printf("test_device_url: OK\n");
    return 0;
}
