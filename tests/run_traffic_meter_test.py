"""Run the real C traffic meter on Windows without Wi-Fi hardware.

Run from an x64 Visual Studio Developer Command Prompt:
    python tests/run_traffic_meter_test.py
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


STUB = """
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NO_MEM 3
#define ERR_OK 0
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_calloc(count, size, caps) calloc(count, size)
static int64_t fake_time;
static inline int64_t esp_timer_get_time(void) { return fake_time; }
typedef struct { uint32_t addr; } ip4_addr_t;
struct pbuf { uint16_t tot_len; const uint8_t *payload; };
static inline uint16_t pbuf_copy_partial(const struct pbuf *p, void *out, uint16_t size, uint16_t offset) {
    if (!p->payload || (unsigned)offset + size > p->tot_len) return 0;
    memcpy(out, p->payload + offset, size); return size;
}
struct netif;
typedef err_t (*netif_output_fn)(struct netif *, struct pbuf *, const ip4_addr_t *);
struct netif { netif_output_fn output; };
typedef struct netif esp_netif_t;
static inline void *esp_netif_get_netif_impl(esp_netif_t *netif) { return netif; }
static inline esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *), void *ctx) { return fn(ctx); }
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cl", default="cl", help="MSVC C compiler")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="traffic-meter-") as directory:
        temp = Path(directory)
        (temp / "fake_platform.h").write_text(STUB, encoding="utf-8")
        for name in ("esp_err.h", "esp_netif.h", "esp_netif_net_stack.h",
                     "esp_heap_caps.h", "esp_timer.h", "lwip/ip4.h", "lwip/netif.h"):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake_platform.h"\n', encoding="utf-8")
        obj = temp / "test.obj"
        exe = temp / "test.exe"
        subprocess.run([
            args.cl, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/wd4100", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{temp}", f"/I{root / 'main'}",
            str(root / "tests/test_traffic_meter.c"), f"/Fo{obj}", f"/Fe{exe}",
            "/link", "/INCREMENTAL:NO",
        ], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
