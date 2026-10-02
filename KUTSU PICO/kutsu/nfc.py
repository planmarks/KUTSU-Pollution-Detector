"""
NFC NTAG (NT3H2111) — write a fixed health record as an NDEF message over I2C.

There is no off-the-shelf CircuitPython driver for the NT3H2x11, so we build the
NDEF bytes by hand and write them into the tag's user memory. External phones /
NFC readers then harvest the record over RF (no power needed from us for reads).

We write the record once at boot. The payload is plain text by default; flip
config.NFC_RECORD_TYPE to "uri" later to emit a URI record instead.

!! HARDWARE TODO !!  The NT3H2x11 memory map (capability container in block 0,
user memory starting at block 1, 16-byte block writes) must be verified against
the datasheet before trusting this on real silicon. The NDEF *encoding* below is
standard; the *block placement* is the part to confirm when the module is wired.
"""

try:
    import config
except ImportError:
    from .. import config  # type: ignore

NTAG_USER_START_BLOCK = 0x01   # TODO: confirm for NT3H2111 (user mem after block 0)
BLOCK_SIZE = 16


# --- NDEF encoding ------------------------------------------------------------
def _ndef_text_record(text, lang="en"):
    payload = bytes([len(lang)]) + lang.encode("utf-8") + text.encode("utf-8")
    tnf_flags = 0xD1            # MB=1, ME=1, SR=1, TNF=0x01 (well-known)
    type_field = b"T"
    return bytes([tnf_flags, len(type_field), len(payload)]) + type_field + payload


def _ndef_uri_record(uri):
    # URI identifier code 0x00 = no abbreviation (full URI in payload).
    payload = bytes([0x00]) + uri.encode("utf-8")
    return bytes([0xD1, 0x01, len(payload)]) + b"U" + payload


def _wrap_tlv(ndef_message):
    """Wrap an NDEF message in a Type 2 Tag TLV block (0x03 len ... 0xFE)."""
    length = len(ndef_message)
    if length < 255:
        tlv = bytes([0x03, length]) + ndef_message
    else:  # 3-byte length form
        tlv = bytes([0x03, 0xFF, (length >> 8) & 0xFF, length & 0xFF]) + ndef_message
    return tlv + bytes([0xFE])  # terminator TLV


def build_health_message():
    if config.NFC_RECORD_TYPE == "uri":
        # e.g. a link to an emergency profile page
        rec = _ndef_uri_record(config.NFC_HEALTH.get("uri", "https://example.com"))
    else:
        h = config.NFC_HEALTH
        text = "Name: {}\nDOB: {}\nBlood: {}".format(
            h["full_name"], h["date_of_birth"], h["blood_type"]
        )
        rec = _ndef_text_record(text)
    return _wrap_tlv(rec)


# --- Tag writer ---------------------------------------------------------------
class NtagWriter:
    def __init__(self, i2c, address=None):
        self.i2c = i2c
        self.address = address or config.ADDR_NTAG

    def _write_block(self, block_num, data16):
        assert len(data16) == BLOCK_SIZE
        buf = bytes([block_num]) + data16
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.writeto(self.address, buf)
        finally:
            self.i2c.unlock()

    def write_message(self, message):
        """Pad to 16-byte blocks and write sequentially from the user area."""
        padded = message + bytes((-len(message)) % BLOCK_SIZE)
        for i in range(0, len(padded), BLOCK_SIZE):
            block = NTAG_USER_START_BLOCK + (i // BLOCK_SIZE)
            self._write_block(block, padded[i:i + BLOCK_SIZE])
        return len(padded) // BLOCK_SIZE

    def provision_health_record(self):
        try:
            msg = build_health_message()
            n = self.write_message(msg)
            print("[nfc] wrote health record:", len(msg), "bytes /", n, "blocks")
            return True
        except Exception as e:  # noqa: BLE001
            print("[nfc] write failed:", e)
            return False
