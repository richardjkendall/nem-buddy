#include "captive_dns.h"
#include "nem/provision.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns";

static void dns_task(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS on :53");

    const unsigned char ip[4] = { 192, 168, 4, 1 };
    unsigned char q[512], r[600];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        int rl = nem_provision_build_dns_reply(q, n, ip, r, sizeof r);
        if (rl > 0) sendto(s, r, rl, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_dns_start(void) {
    xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL);
}
