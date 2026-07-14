# NEM Proxy — deploy to k8s behind Cloudflare tunnel

## 1. Secrets
    kubectl create secret generic nem-proxy-secrets \
      --from-literal=oe-api-key='oe_xxx' \
      --from-literal=proxy-secret='<long-random-master-passphrase>'
`proxy-secret` is the **master** secret. Devices never hold it — each device gets its
own derived key (see §7). To revoke devices, set `NEM_PROXY_DENY` (comma-separated
device IDs) on the deployment:
    kubectl set env deploy/nem-proxy NEM_PROXY_DENY=alice-01,bob-03

## 2. Image
Build/push to a registry your cluster can pull, and set `image:` in
`nem-proxy.yaml` to that ref:
    docker build -t <registry>/nem-proxy:1 proxy && docker push <registry>/nem-proxy:1

## 3. Deploy
    kubectl apply -f deploy/k8s/nem-proxy.yaml

## 4. cloudflared tunnel ingress
Add to the tunnel config (or a `cloudflare.com` Tunnel → Public Hostname):
    - hostname: nembuddy.<your-domain>
      service: http://nem-proxy.<namespace>.svc.cluster.local:8080
    - service: http_status:404

## 5. Cloudflare zone settings for nembuddy.<your-domain>  (CRITICAL — device is HTTP-only)
- SSL/TLS → Edge Certificates → **Always Use HTTPS: OFF**.
- No HSTS header; no Page/Configuration Rule forcing HTTPS on this hostname.
- Speed → Optimization: minify OFF; Rocket Loader OFF (must not alter the body).
- Cache: the proxy already sends `Cache-Control: no-store, no-transform`.
- (Optional) Security → add a rate-limiting rule on the hostname.

## 6. Verify through Cloudflare
    curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem   # 401 (no X-NEM-Id)
    # 200 with valid headers for a provisioned device:
    AUTH=$(python3 - <<'PY'
    import hashlib,hmac,base64
    mk=hashlib.sha256(b"<master-passphrase>").digest()
    dk=hmac.new(mk,b"<device_id>",hashlib.sha256).digest()
    print(base64.b64encode(hmac.new(dk,b"GET /nem",hashlib.sha256).digest()).decode())
    PY
    )
    curl -s -D - -o /dev/null "http://nembuddy.<domain>/nem" \
      -H "X-NEM-Id: <device_id>" -H "X-NEM-Auth: $AUTH" | grep -iE "HTTP/|X-NEM-Sig|location"
Confirm no 301→https redirect and that `X-NEM-Sig` is present on the 200.

## 7. Device provisioning (per device)
Derive each device's credential from the master secret:
    NEM_PROXY_SECRET='<master-passphrase>' python3 proxy/provision_device.py rjk-kitchen
This prints a `device_id` and a base64 `device_key`. In the device captive portal set:
proxy URL = `http://nembuddy.<domain>/nem`, Device ID = `rjk-kitchen`, Device key = the
printed base64 value. Use a unique ID per device.
