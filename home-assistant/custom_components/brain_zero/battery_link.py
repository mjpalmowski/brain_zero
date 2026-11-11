"""Read-only battery challenge protocol; source ages survive MQTT forwarding."""
import asyncio
from dataclasses import dataclass
import hmac
import math
import secrets
import socket
import struct
from time import monotonic, time

PORT = 41903
MAGIC = 0x50414332
REQUEST = struct.Struct("<III")
REPLY = struct.Struct("<IIIffIII")


@dataclass(frozen=True)
class BatterySample:
    soc: float
    voltage: float
    timestamp: float
    sequence: int

    def payload(self, now):
        if not 0 <= now - self.timestamp <= 10:
            return {}
        return {"soc": self.soc, "voltage": self.voltage, "soc_ts": self.timestamp}


def challenge(key, nonce=None):
    nonce = nonce or secrets.token_bytes(8)
    if len(nonce) != 8:
        raise ValueError("Invalid battery challenge")
    prefix = struct.pack("<I", MAGIC) + nonce
    return prefix + hmac.digest(key.encode(), prefix, "sha256")


def decode_reply(raw, request, key, sent_at, received_at, elapsed):
    """Use the older CAN age plus whole RTT: never make an old sample fresh."""
    if len(raw) != 64 or len(request) != 44 or not 0 <= elapsed <= 2:
        raise ValueError("Invalid battery response size or timing")
    prefix = raw[:32]
    if not hmac.compare_digest(raw[32:], hmac.digest(key.encode(), prefix, "sha256")):
        raise ValueError("Battery authentication failed")
    magic, hi, lo, soc, voltage, soc_age, voltage_age, sequence = REPLY.unpack(prefix)
    if prefix[:12] != request[:12] or magic != MAGIC:
        raise ValueError("Battery challenge mismatch")
    age = max(soc_age, voltage_age) / 1000 + elapsed
    if (not math.isfinite(soc) or not 0 <= soc <= 100 or
            not math.isfinite(voltage) or not 3 <= voltage <= 80 or age > 10 or
            abs((received_at - sent_at) - elapsed) > 1):
        raise ValueError("Battery CAN sample is stale or invalid")
    return BatterySample(soc, voltage, received_at - age, sequence)


async def read_battery(host, key, timeout=1):
    loop = asyncio.get_running_loop()
    address = (await loop.getaddrinfo(host, PORT, family=socket.AF_INET,
                                     type=socket.SOCK_DGRAM))[0][4]
    request = challenge(key)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    sent_at, started = time(), monotonic()
    try:
        async with asyncio.timeout(timeout):
            await loop.sock_connect(sock, address)
            await loop.sock_sendall(sock, request)
            # Connected UDP filters both source address and source port.
            raw = await loop.sock_recv(sock, 65)
        return decode_reply(raw, request, key, sent_at, time(), monotonic() - started)
    finally:
        sock.close()
