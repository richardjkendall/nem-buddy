#!/usr/bin/env python3
"""NEM Buddy proxy.

Fetches AEMO (price/demand) + OpenElectricity (generation mix) over HTTPS,
merges them into one compact JSON, and serves it over plain HTTP so the
ESP32 (which is internal-RAM constrained for TLS) can read it cheaply.

Run:
  NEM_OE_API_KEY=oe_xxx python3 proxy/nem_proxy.py [--port 8080]

Endpoint:  GET /nem  -> compact JSON (see build_payload)
           GET /     -> same
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import ssl
import sys
import threading
import time
import urllib.request
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

AEMO_URL = "https://visualisations.aemo.com.au/aemo/apps/api/report/ELEC_NEM_SUMMARY"
OE_BASE = ("https://api.openelectricity.org.au/v4/data/network/NEM"
           "?metrics=power&primary_grouping=network_region&secondary_grouping=fueltech")
# Market endpoint carries 5-min price + demand per region (two data blocks, one
# per metric) — used once/day to backfill today's curve midnight->now.
OE_MARKET = ("https://api.openelectricity.org.au/v4/market/network/NEM"
             "?metrics=price&metrics=demand&interval=5m"
             "&primary_grouping=network_region")
REGIONS = ["NSW1", "QLD1", "SA1", "TAS1", "VIC1"]
FUELS = ["coal", "gas", "hydro", "wind", "solar", "battery", "other"]

# fueltech -> (bucket, renewable, is_load)
FUEL_MAP = {
    "coal_black": ("coal", False, False), "coal_brown": ("coal", False, False),
    "gas_ccgt": ("gas", False, False), "gas_ocgt": ("gas", False, False),
    "gas_recip": ("gas", False, False), "gas_steam": ("gas", False, False),
    "gas_wcmg": ("gas", False, False),
    "distillate": ("other", False, False),
    "bioenergy_biomass": ("other", True, False), "bioenergy_biogas": ("other", True, False),
    "hydro": ("hydro", True, False), "wind": ("wind", True, False),
    "solar_utility": ("solar", True, False), "solar_rooftop": ("solar", True, False),
    "battery_discharging": ("battery", True, False),
    "battery_charging": ("battery", False, True), "pumps": ("hydro", False, True),
}

_ctx = ssl.create_default_context()
_cache = {"payload": None, "aemo_err": None, "oe_err": None}
_lock = threading.Lock()

_secret = os.environ.get("NEM_PROXY_SECRET", "")
_master_key = None          # bytes, or None in LAN mode
_deny = {d.strip() for d in os.environ.get("NEM_PROXY_DENY", "").split(",") if d.strip()}


def derive_master_key(secret):
    return hashlib.sha256(secret.encode()).digest()


def derive_device_key(master_key, device_id):
    return hmac.new(master_key, device_id.encode(), hashlib.sha256).digest()


def sign_body(key, body):
    return base64.b64encode(hmac.new(key, body, hashlib.sha256).digest()).decode()


def verify_request(device_key, auth_b64):
    expect = base64.b64encode(hmac.new(device_key, b"GET /nem", hashlib.sha256).digest()).decode()
    return hmac.compare_digest(expect, auth_b64 or "")


def _get(url, headers=None, timeout=20):
    h = {"User-Agent": "nem-buddy-proxy/1"}  # OE/AEMO 403 the default urllib UA
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=timeout, context=_ctx) as r:
        return r.read().decode("utf-8")


def fetch_aemo():
    """-> (settlement_str, {region: {price, demand, ni, ic}})"""
    doc = json.loads(_get(AEMO_URL, {"User-Agent": "nem-buddy-proxy/1"}))
    out, settle = {}, None
    for row in doc.get("ELEC_NEM_SUMMARY", []):
        rid = row.get("REGIONID")
        if rid not in REGIONS:
            continue
        flows = []
        raw = row.get("INTERCONNECTORFLOWS")
        try:
            for f in (json.loads(raw) if isinstance(raw, str) else (raw or [])):
                name = f.get("name")
                if name is not None and f.get("value") is not None:
                    flows.append([str(name), round(float(f["value"]), 1)])
        except (ValueError, TypeError, AttributeError):
            flows = []
        out[rid] = {"price": float(row.get("PRICE", 0.0)),
                    "demand": float(row.get("TOTALDEMAND", 0.0)),
                    "ni": round(float(row.get("NETINTERCHANGE", 0.0)), 1),
                    "ic": flows}
        settle = settle or row.get("SETTLEMENTDATE")
    return settle, out


def fetch_oe(api_key, settlement_str):
    """-> {region: {ren: float, fuel: {bucket: mw}}} using a small recent window."""
    # Anchor the window on AEMO's NEM (AEST) settlement time; fall back to now.
    try:
        end = datetime.strptime(settlement_str, "%Y-%m-%dT%H:%M:%S")
    except (TypeError, ValueError):
        end = datetime.now()
    start = end - timedelta(minutes=30)
    url = (OE_BASE
           + "&date_start=" + start.strftime("%Y-%m-%dT%H:%M:%S")
           + "&date_end=" + end.strftime("%Y-%m-%dT%H:%M:%S"))
    doc = json.loads(_get(url, {"Authorization": "Bearer " + api_key}))
    data = doc.get("data") or []
    results = data[0].get("results", []) if data else []

    mix = {r: {f: 0.0 for f in FUELS} for r in REGIONS}
    renew_mw = {r: 0.0 for r in REGIONS}
    total_mw = {r: 0.0 for r in REGIONS}
    for series in results:
        cols = series.get("columns", {})
        region = cols.get("region")
        ft = cols.get("fueltech")
        if region not in REGIONS or ft not in FUEL_MAP:
            continue
        bucket, renewable, is_load = FUEL_MAP[ft]
        if is_load:
            continue
        pts = series.get("data") or []
        if not pts:
            continue
        val = pts[-1][1]
        if val is None:
            continue
        val = float(val)
        mix[region][bucket] += val
        total_mw[region] += val
        if renewable:
            renew_mw[region] += val
    out = {}
    for r in REGIONS:
        ren = renew_mw[r] / total_mw[r] if total_mw[r] > 0 else 0.0
        out[r] = {"ren": round(ren, 3),
                  "fuel": {f: round(mix[r][f]) for f in FUELS}}
    return out


HIST_SLOTS = 288   # 5-min slots per day

# Today's intraday curve, accumulated from the live poll. Indexed by 5-min slot
# of day (0..287). Persists across device reflashes because it lives here.
_hist = {rid: {"ph": [None] * HIST_SLOTS, "dh": [None] * HIST_SLOTS} for rid in REGIONS}
_hist_day = [None]
_backfilled_day = [None]   # last AEST date we backfilled from OE (once/day)


def record_history(settle, aemo):
    """Fold this poll's price/demand into today's per-region curve, keyed on the
    AEMO (AEST) settlement time. Resets on day rollover."""
    if not settle:
        return
    try:
        dt = datetime.strptime(settle, "%Y-%m-%dT%H:%M:%S")
    except (TypeError, ValueError):
        return
    if _hist_day[0] != dt.date():
        _hist_day[0] = dt.date()
        for rid in REGIONS:
            _hist[rid]["ph"] = [None] * HIST_SLOTS
            _hist[rid]["dh"] = [None] * HIST_SLOTS
    slot = (dt.hour * 60 + dt.minute) // 5
    if not (0 <= slot < HIST_SLOTS):
        return
    for rid in REGIONS:
        a = aemo.get(rid)
        if not a:
            continue
        _hist[rid]["ph"][slot] = round(a.get("price", 0.0), 1)
        _hist[rid]["dh"][slot] = round(a.get("demand", 0.0))


def backfill_history(api_key, day):
    """One-shot: fill `day`'s per-region 5-min price/demand curve from OE's market
    endpoint, midnight->now. Fill-only — never overwrites a slot the live poll has
    already recorded, so live values always win. `day` is an AEST date (from the
    AEMO settlement clock) so slot mapping matches record_history exactly."""
    ds = day.strftime("%Y-%m-%dT00:00:00")
    de = day.strftime("%Y-%m-%dT23:55:00")   # OE clamps to latest available bucket
    url = OE_MARKET + "&date_start=" + ds + "&date_end=" + de
    doc = json.loads(_get(url, {"Authorization": "Bearer " + api_key}))
    field_of = {"price": "ph", "demand": "dh"}
    filled = 0
    for blk in doc.get("data", []):
        field = field_of.get(blk.get("metric"))
        if not field:
            continue
        for series in blk.get("results", []):
            rid = series.get("columns", {}).get("region")   # key is 'region'
            if rid not in REGIONS:
                continue
            for ts, val in (series.get("data") or []):
                if val is None:
                    continue
                try:
                    dt = datetime.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S")  # drop +10:00
                except (TypeError, ValueError):
                    continue
                if dt.date() != day:
                    continue
                slot = (dt.hour * 60 + dt.minute) // 5
                if 0 <= slot < HIST_SLOTS and _hist[rid][field][slot] is None:
                    _hist[rid][field][slot] = (round(val, 1) if field == "ph"
                                               else round(val))
                    filled += 1
    return filled


def _csv(vals):
    return ",".join("" if v is None else str(v) for v in vals)


def build_payload(settle, aemo, oe, hist=None):
    hist = hist or {}
    # Trim curves to the last filled 5-min slot (index == slot of day), so the
    # payload stays small early in the day and grows toward ~288 by day's end.
    cut = 0
    for rid in REGIONS:
        ph = hist.get(rid, {}).get("ph", [])
        for i in range(len(ph) - 1, -1, -1):
            if ph[i] is not None:
                cut = max(cut, i + 1)
                break
    regions = []
    for rid in REGIONS:
        a = aemo.get(rid, {})
        m = oe.get(rid, {}) if oe else {}
        hd = hist.get(rid, {})
        regions.append({
            "id": rid,
            "price": round(a.get("price", 0.0), 2),
            "demand": round(a.get("demand", 0.0)),
            "ni": a.get("ni", 0.0),
            "ic": a.get("ic", []),
            "ren": m.get("ren", 0.0),
            "fuel": m.get("fuel", {f: 0 for f in FUELS}),
            "ph": _csv(hd.get("ph", [])[:cut]),
            "dh": _csv(hd.get("dh", [])[:cut]),
        })
    return {"t": settle or "", "regions": regions}


def refresh_loop(api_key):
    oe_cache, last_oe = None, 0.0
    while True:
        try:
            settle, aemo = fetch_aemo()
            _cache["aemo_err"] = None
            # OE every ~5 min
            if time.time() - last_oe > 290 or oe_cache is None:
                try:
                    oe_cache = fetch_oe(api_key, settle)
                    _cache["oe_err"] = None
                    last_oe = time.time()
                except Exception as e:  # keep last-good OE
                    _cache["oe_err"] = str(e)
                    print("[proxy] OE fetch error:", e, file=sys.stderr)
            record_history(settle, aemo)   # fold this poll into today's curve
            # Backfill the rest of today's curve from OE once per day (after
            # record_history has set _hist_day + reset on rollover). Fill-only.
            if api_key and _hist_day[0] and _backfilled_day[0] != _hist_day[0]:
                try:
                    n = backfill_history(api_key, _hist_day[0])
                    _backfilled_day[0] = _hist_day[0]
                    print("[proxy] backfilled %d slots for %s" % (n, _hist_day[0]),
                          file=sys.stderr)
                except Exception as e:
                    print("[proxy] backfill error:", e, file=sys.stderr)
            payload = build_payload(settle, aemo, oe_cache, _hist)
            with _lock:
                _cache["payload"] = payload
            print("[proxy] refreshed @", settle, "VIC $%.1f" %
                  next((r["price"] for r in payload["regions"] if r["id"] == "VIC1"), 0),
                  file=sys.stderr)
        except Exception as e:
            _cache["aemo_err"] = str(e)
            print("[proxy] AEMO fetch error:", e, file=sys.stderr)
        time.sleep(60)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        device_key = None
        if _master_key is not None:
            did = self.headers.get("X-NEM-Id", "").strip()
            if not did or did in _deny:
                self.send_response(401); self.end_headers(); return
            device_key = derive_device_key(_master_key, did)
            auth = self.headers.get("X-NEM-Auth", "")
            if not verify_request(device_key, auth):
                self.send_response(401); self.end_headers(); return
        with _lock:
            payload = _cache["payload"]
        if payload is None:
            self.send_response(503)
            self.end_headers()
            self.wfile.write(b'{"error":"warming up"}')
            return
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if _master_key is not None:
            self.send_header("X-NEM-Sig", sign_body(device_key, body))
            self.send_header("Cache-Control", "no-store, no-transform")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass  # quiet


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()
    api_key = os.environ.get("NEM_OE_API_KEY", "")
    if not api_key:
        print("WARNING: NEM_OE_API_KEY not set; mix will be empty", file=sys.stderr)

    global _master_key
    if _secret:
        _master_key = derive_master_key(_secret)
        print("[proxy] app-layer auth ENABLED (%d device(s) denied)" % len(_deny), file=sys.stderr)
    else:
        print("[proxy] app-layer auth disabled (LAN mode)", file=sys.stderr)

    threading.Thread(target=refresh_loop, args=(api_key,), daemon=True).start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("[proxy] serving on http://%s:%d/nem" % (args.host, args.port), file=sys.stderr)
    srv.serve_forever()


if __name__ == "__main__":
    main()
