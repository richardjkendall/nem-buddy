import base64, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import nem_proxy as p

# Known-answer vector (see plan Global Constraints)
KEY = p.derive_key("testsecret")
assert KEY.hex() == "59953998e54a579be74c1b7344cd55c64981451b066a35c9d7baf5497f16d865"

def test_sign_body():
    body = b'{"t":"2026-07-14T00:00:00","regions":[]}'
    assert p.sign_body(KEY, body) == "iYW1w5bFInoap2OfScY0Xt082rEedKXacCGM4EZu11g="

def test_verify_request_ok():
    assert p.verify_request(KEY, 42, "G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=") is True

def test_verify_request_bad_mac():
    assert p.verify_request(KEY, 42, "AAAA") is False

def test_verify_request_wrong_counter():
    # right MAC but for counter 42, presented as counter 43 -> mismatch
    assert p.verify_request(KEY, 43, "G69StGPYEo1A7d1r9FFqAp8aLV206F0TJN6guGhF8S4=") is False

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print("ok", name)
    print("ALL PASS")
