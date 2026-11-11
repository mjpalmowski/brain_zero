"""Broker requests with bounded replies, backoff and no write retries."""
import asyncio
import json
import re
from time import monotonic, time
from uuid import uuid4

READ_PATHS = {"state", "ess", "inverter", "charger", "assist", "system", "recharge", "free-power", "battery-input", "ess/lease"}
SITE_PATHS = {"recharge", "free-power"}
WRITE_PATHS = {"ess", "inverter", "charger", "charger/read", "assist", "assist/read", "system", "recharge", "free-power",
               "ac-input", "ac-input/settings", "pv-disable", "pv-disable/settings", "battery-input", "ess/lease"}


class ApiError(Exception):
    """Controller rejected a request or could not be reached."""

    def __init__(self, message, *, status=None):
        super().__init__(message)
        self.status = status


class SiteFeatureUnavailable(ApiError):
    """Standard firmware does not expose private charging controls."""


class BrainApi:
    def __init__(self, exchange, identity, host="brain-zero.local", port=80, *, clock=monotonic, sleep=asyncio.sleep):
        if not re.fullmatch(r"[0-9a-f]{12}", identity or ""):
            raise ValueError("Use the controller ID shown in brain_zero MQTT settings")
        if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9.-]{0,252}", host) or not 1 <= int(port) <= 65535:
            raise ValueError("Use a valid host name or IPv4 address for the controller web link")
        self.exchange = exchange
        self.prefix = f"brain_zero/{identity}"
        self.host, self.port = host, int(port)
        self.url = f"http://{host}" + (f":{port}" if int(port) != 80 else "")
        self.lock = asyncio.Lock()
        self.clock, self.sleep = clock, sleep
        self.next_request = self.retry_at = 0.0
        self.failures = self.requests = self.errors = 0

    async def request(self, endpoint, fields=None):
        write = fields is not None
        if endpoint not in (WRITE_PATHS if write else READ_PATHS):
            raise ApiError("Unsupported controller operation")
        async with self.lock:
            if self.clock() < self.retry_at:
                raise ApiError("Controller connection is backing off; wait before trying again")
            await self.sleep(max(0, self.next_request - self.clock()))
            self.next_request = self.clock() + .5
            request_id = uuid4().hex
            request = {"id": request_id, "path": endpoint, "op": "set" if write else "get", "fields": fields or {}}
            if write:
                request["expires"] = int(time()) + 5
            payload = json.dumps(request, separators=(",", ":"), ensure_ascii=False)
            if len(payload.encode()) >= 1400:
                raise ApiError("Controller request exceeded the size limit")
            self.requests += 1
            try:
                async with asyncio.timeout(5):
                    raw = await self.exchange(f"{self.prefix}/request", f"{self.prefix}/reply/{request_id}", payload)
            except (OSError, TimeoutError) as err:
                self.errors += 1
                self.failures += 1
                self.retry_at = self.clock() + min(60, 5 * 2 ** min(self.failures - 1, 4))
                message = "MQTT command was not retried. Check readback before repeating it" if write else "No MQTT response from brain_zero"
                raise ApiError(message) from err
            if len(raw) > 16384:
                raise ApiError("Controller response exceeded the size limit")
            try:
                reply = json.loads(raw)
            except (ValueError, UnicodeError) as err:
                raise ApiError("Controller returned invalid JSON") from err
            if not isinstance(reply, dict) or type(reply.get("status")) is not int or not isinstance(reply.get("data"), dict):
                raise ApiError("Controller returned an invalid response")
            self.failures = 0
            status, data = reply["status"], reply["data"]
            if not write and endpoint in SITE_PATHS and status == 404:
                raise SiteFeatureUnavailable("Private charging is not enabled on this controller")
            if not 200 <= status < 300 or data.get("ok") is False:
                raise ApiError(data.get("error", f"Controller rejected the request ({status})"), status=status)
            return data

    async def get(self, endpoint):
        return await self.request(endpoint)

    async def post(self, endpoint, fields):
        def encode(value):
            if isinstance(value, bool) or isinstance(value, (int, float)) and float(value).is_integer():
                return str(int(value))
            return str(value)
        return await self.request(endpoint, {k: encode(v) for k, v in fields.items()})
