# NEM Buddy

A small desk device that shows the Australian **National Electricity Market (NEM)** live:
per-region price and demand, the generation-fuel mix, renewable share, and
interconnector flows — with tap-to-drill-in charts.

It runs on a Waveshare **ESP32-S3-Touch-AMOLED-2.16** (480×480 round AMOLED, ESP-IDF v5.5
+ LVGL v9).

## How it works

The device does **not** call the data APIs directly. A small Python proxy fetches
[AEMO](https://aemo.com.au) (price / demand / interconnector flows) and
[OpenElectricity](https://openelectricity.org.au) (generation mix) over HTTPS, merges and
trims them into one compact JSON blob, and serves it over **plain HTTP**. The ESP32 reads
that — on-device TLS plus the display's DMA buffers plus Wi-Fi don't fit in internal RAM,
so the proxy is the design's accepted trade-off.

```
AEMO + OpenElectricity  ──HTTPS──▶  proxy (nem_proxy.py)  ──plain HTTP──▶  ESP32 device
                                    fetch · merge · sign                   parse · render
```

Because the link is plain HTTP, the proxy can be exposed to the internet with an
application-layer **HMAC-SHA256** scheme (see [Authentication](#authentication)) so a MITM
can't feed the device forged data and randoms can't burn the API quota. With no secret
configured it runs in **LAN mode** (no auth) for local use.

## Repository layout

| Path | What |
|------|------|
| `core/` | Portable C library (data parsers, protocol helpers) — host-tested with Unity |
| `firmware/` | ESP-IDF application (LVGL UI, Wi-Fi provisioning, fetch/verify) |
| `proxy/` | Python-stdlib proxy, `Dockerfile`, `provision_device.py`, `test_auth.py` |
| `deploy/` | Kubernetes manifest + deployment runbook |
| `docs/superpowers/` | Design specs and implementation plans |

## Build & run

**Core library tests** (host, needs CMake + a C compiler):
```bash
cmake -S core -B core/build && cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

**Proxy** (Python 3, stdlib only):
```bash
NEM_OE_API_KEY=oe_xxx python3 proxy/nem_proxy.py --port 8080
python3 proxy/test_auth.py     # unit tests
```

**Firmware** (needs the ESP-IDF v5.5 toolchain):
```bash
idf.py -C firmware build
idf.py -C firmware -p /dev/cu.usbmodemXXXX flash
```

On first boot (or after failing to connect) the device opens a Wi-Fi setup portal:
join the `NEM-Buddy-XXXX` access point (password `nembuddy`), then enter your Wi-Fi, the
proxy URL, and — if auth is enabled — a device ID and key. Copy `firmware/main/secrets.h.example`
to `secrets.h` to pre-seed those on a dev build.

## Authentication

The proxy holds one **master secret** (`NEM_PROXY_SECRET`) and derives a per-device key on
the fly — it never stores a device list:

```
master_key  = SHA256(NEM_PROXY_SECRET)
device_key  = HMAC-SHA256(master_key, device_id)
```

Each request carries `X-NEM-Id` + `X-NEM-Auth` (an HMAC proving possession of the device
key); each response carries `X-NEM-Sig` (an HMAC over the body) which the device verifies,
failing closed. Mint a device credential with:

```bash
NEM_PROXY_SECRET=<master> python3 proxy/provision_device.py <device-id>
```

Revoke a device by adding its ID to `NEM_PROXY_DENY` (comma-separated) on the proxy.

## Deployment

The proxy is a single always-on instance (in-memory cache + background poll), so it wants
`replicas: 1`. See `deploy/k8s/nem-proxy.yaml` and `deploy/RUNBOOK.md` for running it in
Kubernetes behind a plain-HTTP ingress.

## License

No license yet — all rights reserved until one is added.
