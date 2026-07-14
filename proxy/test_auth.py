import base64, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import nem_proxy as p

# Known-answer vector (see plan Global Constraints)
MK = p.derive_master_key("testmaster")
assert MK.hex() == "d9095f2c47b0a651748521077659d03539f4376f17ed79ef070c32d2f6198fb1"
DK = p.derive_device_key(MK, "dev01")
assert DK.hex() == "8f87ad65dbbf2716af81b89332efbd009d67a64a72499ce5b204df787c41484c"
DK2 = p.derive_device_key(MK, "dev02")

def test_sign_body():
    body = b'{"t":"2026-07-14T00:00:00","regions":[]}'
    assert p.sign_body(DK, body) == "uPGokXyiUFAQFM1FjIK8dW3EGa3Rgm23GDpxFIc+VGk="

def test_verify_request_ok():
    assert p.verify_request(DK, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=") is True

def test_verify_request_bad_mac():
    assert p.verify_request(DK, "AAAA") is False

def test_verify_request_wrong_device():
    # dev01's auth presented against dev02's key -> mismatch
    assert p.verify_request(DK2, "AgulWuvI5tPLH16AFjhdorHUgz73oTeShS+VOdQ1vdU=") is False

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn(); print("ok", name)
    print("ALL PASS")
