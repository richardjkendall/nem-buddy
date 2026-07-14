#!/usr/bin/env python3
"""Derive a device_id + device_key for the NEM Buddy proxy.

Usage:
  NEM_PROXY_SECRET=<master> python3 proxy/provision_device.py <device_id>

Prints the device_id and its base64 device_key to enter in the device captive
portal. The master secret never leaves this machine.
"""
import base64, hashlib, hmac, os, sys


def main():
    if len(sys.argv) != 2:
        print("usage: NEM_PROXY_SECRET=<master> python3 provision_device.py <device_id>",
              file=sys.stderr)
        sys.exit(2)
    secret = os.environ.get("NEM_PROXY_SECRET", "")
    if not secret:
        print("error: NEM_PROXY_SECRET not set", file=sys.stderr)
        sys.exit(2)
    device_id = sys.argv[1]
    master_key = hashlib.sha256(secret.encode()).digest()
    device_key = hmac.new(master_key, device_id.encode(), hashlib.sha256).digest()
    print("device_id:  " + device_id)
    print("device_key: " + base64.b64encode(device_key).decode())


if __name__ == "__main__":
    main()
