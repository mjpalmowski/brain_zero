"""Private compatibility-profile queue behind the brain_zero website."""

from datetime import datetime, timezone
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import socket
import sys
import tempfile
import threading
import time


REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools"))

from prepare_veconfigure import prepare_veconfigure
from validate_compatibility import validate_profile


MAX_REQUEST_BYTES = 280 * 1024
MAX_REPORT_BYTES = 2 * 1024 * 1024
LEASE_SECONDS = 15 * 60
DEFAULT_ORIGINS = ("https://brain-zero.com", "https://www.brain-zero.com")
PATH = "/api/compatibility"
CLAIM_PATH = "/api/internal/compatibility/claim"
COMPLETE_PATH = "/api/internal/compatibility/complete"
RECEIPT = re.compile(r"[0-9a-f]{32}")


class SubmissionError(ValueError):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def unique_keys(pairs):
    value = {}
    for name, item in pairs:
        if name in value:
            raise SubmissionError("The submission contains duplicate JSON fields.")
        value[name] = item
    return value


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")

def parse_json(raw):
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=unique_keys)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        if isinstance(error, SubmissionError):
            raise
        raise SubmissionError("Upload valid JSON.") from None


def parse_submission(raw):
    """Validate the complete envelope before it reaches persistent storage."""
    if len(raw) > MAX_REQUEST_BYTES:
        raise SubmissionError("The submission exceeds the 280 KiB upload limit.", 413)
    envelope = parse_json(raw)
    required = {"model", "consent", "profile"}
    if (not isinstance(envelope, dict) or not required <= envelope.keys() or
            envelope.keys() - required - {"email"}):
        raise SubmissionError("The submission has missing or unexpected fields.")
    model = envelope["model"]
    if (not isinstance(model, str) or not 1 <= len(model.strip()) <= 160 or
            any(ord(char) < 32 or ord(char) == 127 for char in model)):
        raise SubmissionError("Enter the inverter model using at most 160 characters.")
    email = envelope.get("email", "")
    if (not isinstance(email, str) or len(email) > 254 or
            any(char.isspace() or ord(char) < 32 or ord(char) == 127 for char in email) or
            (email and not re.fullmatch(r"[^@<>]+@[^@<>]+\.[^@<>]+", email))):
        raise SubmissionError("Enter a contact email address or leave it blank.")
    if envelope["consent"] is not True:
        raise SubmissionError("Confirm that you agree to submit the compatibility profile.")
    try:
        profile_raw = canonical_json(envelope["profile"])
        normalized, details = validate_profile(profile_raw)
    except (ValueError, OverflowError, TypeError, RecursionError) as error:
        raise SubmissionError(str(error) if isinstance(error, ValueError) else
                              "The compatibility profile is invalid.") from None
    return normalized, {
        "model": model.strip(),
        "email": email,
        "consent": True,
        "source_profile_sha256": hashlib.sha256(profile_raw).hexdigest(),
        "normalized_profile_sha256": hashlib.sha256(canonical_json(normalized)).hexdigest(),
        "profile": details,
    }


def write_private_json(path, value):
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(canonical_json(value) + b"\n")
        handle.flush()
        os.fsync(handle.fileno())

def replace_private_json(path, value):
    temporary = path.with_name("." + path.name + "." + secrets.token_hex(8))
    try:
        write_private_json(temporary, value)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


class Queue:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.directory.chmod(0o700)
        self.lock = threading.Lock()

    def accept(self, raw):
        profile, metadata = parse_submission(raw)
        receipt = secrets.token_hex(16)
        metadata.update(receipt=receipt, state="queued",
                        received_at=datetime.now(timezone.utc).isoformat())
        # Consumers see only the renamed, complete directory. Incomplete or
        # failed preparations remain outside the queue and are cleaned up.
        with tempfile.TemporaryDirectory(prefix=".pending-", dir=self.directory) as temporary:
            folder = Path(temporary)
            profile_raw = canonical_json(profile)
            write_private_json(folder / "profile.json", profile)
            prepare_veconfigure(profile_raw, folder)
            write_private_json(folder / "metadata.json", metadata)
            for file in folder.iterdir():
                file.chmod(0o600)
            os.replace(folder, self.directory / receipt)
        return receipt

    def claim(self, run_id, run_attempt):
        now = time.time()
        with self.lock:
            jobs = []
            for folder in self.directory.iterdir():
                if not RECEIPT.fullmatch(folder.name) or not folder.is_dir():
                    continue
                metadata = json.loads((folder / "metadata.json").read_bytes())
                jobs.append((metadata["received_at"], folder, metadata))
            jobs.sort(key=lambda item: item[0])
            # Network retries from one run receive its existing lease.
            for _, folder, metadata in jobs:
                lease = metadata.get("lease", {})
                if (metadata["state"] == "processing" and lease.get("expires_at", 0) > now and
                        (lease.get("run_id"), lease.get("run_attempt")) == (run_id, run_attempt)):
                    return self._leased_profile(folder, metadata)
            for _, folder, metadata in jobs:
                lease = metadata.get("lease", {})
                if metadata["state"] != "queued" and not (
                        metadata["state"] == "processing" and lease.get("expires_at", 0) <= now):
                    continue
                metadata.update(state="processing", lease={
                    "token": secrets.token_urlsafe(32), "run_id": run_id, "run_attempt": run_attempt,
                    "expires_at": now + LEASE_SECONDS,
                })
                replace_private_json(folder / "metadata.json", metadata)
                return self._leased_profile(folder, metadata)
        return None

    @staticmethod
    def _leased_profile(folder, metadata):
        return {"receipt": folder.name, "lease": metadata["lease"]["token"],
                "profile": json.loads((folder / "profile.json").read_bytes())}

    def complete(self, body, run_id, run_attempt):
        if (not isinstance(body, dict) or set(body) != {"receipt", "lease", "status", "report"} or
                not isinstance(body["receipt"], str) or not RECEIPT.fullmatch(body["receipt"]) or
                not isinstance(body["lease"], str) or not re.fullmatch(r"[A-Za-z0-9_-]{43}", body["lease"]) or
                body["status"] not in ("completed", "failed", "blocked") or
                not isinstance(body["report"], dict)):
            raise SubmissionError("The completion report is invalid.")
        try:
            encoded = canonical_json(body["report"])
        except (ValueError, TypeError, RecursionError):
            raise SubmissionError("The completion report is invalid.") from None
        if len(encoded) > MAX_REPORT_BYTES:
            raise SubmissionError("The completion report is too large.", 413)
        report_hash = hashlib.sha256(encoded).hexdigest()
        folder = self.directory / body["receipt"]
        with self.lock:
            try:
                metadata = json.loads((folder / "metadata.json").read_bytes())
            except FileNotFoundError:
                raise SubmissionError("The processing lease is invalid.", 409) from None
            lease = metadata.get("lease", {})
            if ((lease.get("run_id"), lease.get("run_attempt")) != (run_id, run_attempt) or
                    not hmac.compare_digest(lease.get("token", ""), body["lease"])):
                raise SubmissionError("The processing lease is invalid.", 409)
            if metadata["state"] in ("completed", "failed", "blocked"):
                if metadata.get("report_sha256") == report_hash and metadata["state"] == body["status"]:
                    return
                raise SubmissionError("This processing lease has already been completed.", 409)
            if metadata["state"] != "processing" or lease.get("expires_at", 0) <= time.time():
                raise SubmissionError("The processing lease has expired.", 409)
            replace_private_json(folder / "worker-report.json", body["report"])
            metadata.update(state=body["status"], report_sha256=report_hash,
                            completed_at=datetime.now(timezone.utc).isoformat())
            replace_private_json(folder / "metadata.json", metadata)


class Handler(BaseHTTPRequestHandler):
    server_version = "brain_zero"
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(10)

    def log_message(self, _format, *_args):
        # Nginx keeps request-level access logs. Do not print submitted data,
        # email, model, query strings or client addresses from this service.
        pass

    def reply(self, status, body=None):
        encoded = canonical_json(body) if body is not None else b""
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(encoded)
        self.close_connection = True

    def do_GET(self):
        if self.path == "/health":
            self.reply(200, {"ok": True})
        else:
            self.reply(404, {"ok": False, "error": "Not found."})

    def do_POST(self):
        try:
            internal = self.path in (CLAIM_PATH, COMPLETE_PATH)
            if self.path != PATH and not internal:
                raise SubmissionError("Not found.", 404)
            origin = self.headers.get("Origin")
            if not internal and origin and origin not in self.server.allowed_origins:
                raise SubmissionError("Open the upload page on brain-zero.com.", 403)
            claims = self.authenticate_worker() if internal else None
            if self.headers.get_content_type() != "application/json":
                raise SubmissionError("Upload JSON using the compatibility form.", 415)
            if self.headers.get("Transfer-Encoding"):
                raise SubmissionError("The upload must include its content length.", 411)
            length = self.headers.get("Content-Length", "")
            if not re.fullmatch(r"[0-9]{1,9}", length):
                raise SubmissionError("The upload must include its content length.", 411)
            length = int(length)
            maximum = MAX_REPORT_BYTES + 2048 if self.path == COMPLETE_PATH else MAX_REQUEST_BYTES
            if not 0 < length <= maximum:
                raise SubmissionError("The upload exceeds its size limit.", 413)
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise SubmissionError("The upload was interrupted.")
            if self.path == CLAIM_PATH:
                body = parse_json(raw)
                if (not isinstance(body, dict) or set(body) != {"run_id", "run_attempt"} or
                        (body.get("run_id"), body.get("run_attempt")) !=
                        (claims["run_id"], claims["run_attempt"])):
                    raise SubmissionError("The worker run does not match its identity.", 403)
                result = self.server.queue.claim(claims["run_id"], claims["run_attempt"])
                self.reply(200 if result else 204, result)
                return
            if self.path == COMPLETE_PATH:
                self.server.queue.complete(parse_json(raw), claims["run_id"], claims["run_attempt"])
                self.reply(200, {"ok": True})
                return
            receipt = self.server.queue.accept(raw)
        except SubmissionError as error:
            self.reply(error.status, {"ok": False, "error": str(error)})
        except (TimeoutError, socket.timeout):
            self.reply(408, {"ok": False, "error": "The upload timed out. Try again."})
        except (OSError, ValueError):
            self.reply(503, {"ok": False, "error": "The upload could not be saved. Try again shortly."})
        else:
            self.reply(202, {"ok": True, "receipt": receipt, "status": "queued"})

    def authenticate_worker(self):
        header = self.headers.get("Authorization", "")
        if not header.startswith("Bearer ") or len(header) > 16384:
            raise SubmissionError("The worker identity is invalid.", 401)
        try:
            claims = self.server.verify_oidc(header[7:])
            if any(not isinstance(claims.get(key), str) or not re.fullmatch(r"[1-9][0-9]{0,19}", claims[key])
                   for key in ("run_id", "run_attempt")):
                raise ValueError("Missing worker run.")
        except (ValueError, KeyError):
            raise SubmissionError("The worker identity is invalid.", 401) from None
        return claims


def verify_worker_token(token):
    from oidc import verify_token
    return verify_token(token)


def create_server(address, directory, origins=DEFAULT_ORIGINS, verify_oidc=verify_worker_token):
    server = ThreadingHTTPServer(address, Handler)
    server.daemon_threads = True
    server.queue = Queue(directory)
    server.allowed_origins = frozenset(origins)
    server.verify_oidc = verify_oidc
    return server


def main():
    origins = tuple(filter(None, os.environ.get("INTAKE_ORIGINS", ",".join(DEFAULT_ORIGINS)).split(",")))
    if not origins:
        raise SystemExit("INTAKE_ORIGINS must include the website's HTTPS origin.")
    server = create_server(("0.0.0.0", int(os.environ.get("PORT", "8080"))),
                           os.environ.get("INTAKE_SPOOL", "/var/lib/brain-zero-intake"), origins)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
