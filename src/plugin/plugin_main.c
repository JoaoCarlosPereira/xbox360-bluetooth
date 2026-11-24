#include <xsdk/plugin.h>
#include "../hal/xbox360_usbd.h"
#include "../transport/bt_usb_transport.h"

extern int x360_usbd_resolve_ordinals(void);

/* Minimal DPC-safe memory primitives (plugin links libxsdk_plugin.a without newlib) */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

static void sleep_ms(uint32_t ms) {
    int64_t interval = -(int64_t)ms * 10000LL;
    KeDelayExecutionThread(0, 0, &interval);
}

void plugin_main(HANDLE module) {
    (void)module;

    xsdk_plugin_log_open("\\Device\\Mass0\\bt360_diag.log");
    xsdk_plugin_log("bt360_proto: loaded (kernel %u.%u.%u.%u)\n",
                    XboxKrnlVersion->major, XboxKrnlVersion->minor,
                    XboxKrnlVersion->build, XboxKrnlVersion->qfe);

    int rc = x360_usbd_resolve_ordinals();
    xsdk_plugin_log("bt360_proto: x360_usbd_resolve_ordinals -> %d\n", rc);

    /*
     * Keep plugin resident in background.
     * Next step (requires physical dongle / 17559 USBD bind test):
     * register 0xE0/0x01/0x01 driver or hook USBD dispatch and call
     * x360_usbd_attach_bt_device(dev_handle, iface_num).
     */
    while (1) {
        sleep_ms(5000);
    }
}
