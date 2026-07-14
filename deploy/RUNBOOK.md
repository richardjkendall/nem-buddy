# NEM Proxy — deploy to k8s behind Cloudflare tunnel

## 1. Secrets
    kubectl create secret generic nem-proxy-secrets \
      --from-literal=oe-api-key='oe_xxx' \
      --from-literal=proxy-secret='<long-random-passphrase>'
The same `proxy-secret` value is what you enter in the device's captive-portal
"Proxy token" field.

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
    curl -s -o /dev/null -w "%{http_code}\n" http://nembuddy.<domain>/nem   # 401 (no auth)
    # 200 with valid headers (compute X-NEM-Auth with the proxy-secret + a counter)
Confirm no 301→https redirect and that `X-NEM-Sig` is present on the 200.

## 7. Device
Re-provision via the captive portal: proxy URL = `http://nembuddy.<domain>/nem`,
proxy token = the `proxy-secret`.
