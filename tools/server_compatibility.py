"""Process one private server submission on a hosted Windows Actions worker."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.error import HTTPError
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from prepare_veconfigure import prepare_veconfigure
from validate_compatibility import validate_profile


SERVER = "https://brain-zero.com"
AUDIENCE = SERVER + "/compatibility-intake"
MAX_RESPONSE_BYTES = 300 * 1024
MAX_REPORT_BYTES = 2 * 1024 * 1024
TOOLS = Path(__file__).resolve().parent


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def encoded(value):
    return json.dumps(value, separators=(",", ":"), allow_nan=False).encode("utf-8")


def worker_token():
    url = urlsplit(os.environ["ACTIONS_ID_TOKEN_REQUEST_URL"])
    if (url.scheme != "https" or not url.hostname or
            not url.hostname.endswith(".actions.githubusercontent.com") or url.username or url.fragment):
        raise ValueError("The GitHub identity endpoint is unavailable.")
    query = [(key, value) for key, value in parse_qsl(url.query) if key != "audience"]
    query.append(("audience", AUDIENCE))
    target = urlunsplit((url.scheme, url.netloc, url.path, urlencode(query), ""))
    request = Request(target, headers={"Authorization": "Bearer " + os.environ["ACTIONS_ID_TOKEN_REQUEST_TOKEN"],
                                      "User-Agent": "brain_zero compatibility worker"})
    with build_opener(NoRedirect()).open(request, timeout=15) as response:
        raw = response.read(32 * 1024 + 1)
    if len(raw) > 32 * 1024:
        raise ValueError("The GitHub identity response was too large.")
    token = json.loads(raw).get("value")
    if not isinstance(token, str) or not 32 < len(token) <= 16 * 1024:
        raise ValueError("The GitHub identity response was invalid.")
    return token


def post(path, body):
    request = Request(SERVER + path, data=encoded(body), method="POST",
                      headers={"Authorization": "Bearer " + worker_token(), "Content-Type": "application/json",
                               "User-Agent": "brain_zero compatibility worker"})
    try:
        with build_opener(NoRedirect()).open(request, timeout=30) as response:
            if response.status == 204:
                return None
            raw = response.read(MAX_RESPONSE_BYTES + 1)
    except HTTPError as error:
        raise ValueError(f"The private intake server refused the worker request (HTTP {error.code}).") from None
    if len(raw) > MAX_RESPONSE_BYTES:
        raise ValueError("The private intake response was too large.")
    return json.loads(raw)


def write(path, value):
    path.write_bytes(encoded(value) + b"\n")


def lease_info(folder):
    value = json.loads((folder / "lease.json").read_bytes())
    if (not isinstance(value, dict) or set(value) != {"receipt", "lease", "run_id", "run_attempt"} or
            not isinstance(value["receipt"], str) or
            not re.fullmatch(r"[a-f0-9]{32}", value["receipt"]) or
            not isinstance(value["lease"], str) or not 16 <= len(value["lease"]) <= 128):
        raise ValueError("The private submission lease is invalid.")
    return value


def claim(folder):
    run = {"run_id": os.environ["GITHUB_RUN_ID"], "run_attempt": os.environ["GITHUB_RUN_ATTEMPT"]}
    result = post("/api/internal/compatibility/claim", run)
    claimed = result is not None
    if claimed:
        if not isinstance(result, dict) or set(result) != {"receipt", "lease", "profile"}:
            raise ValueError("The private intake lease response is invalid.")
        normalized, _ = validate_profile(encoded(result["profile"]))
        folder.mkdir(parents=True, exist_ok=False)
        write(folder / "lease.json", {**run, "receipt": result["receipt"], "lease": result["lease"]})
        lease_info(folder)
        write(folder / "profile.json", normalized)
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as handle:
            handle.write(f"claimed={'true' if claimed else 'false'}\n")
    print("Private compatibility submission claimed." if claimed else "No private submissions are waiting.")


def process(folder, exe):
    lease_info(folder)
    report = {"status": "failed", "stage": "prepare"}
    try:
        raw = (folder / "profile.json").read_bytes()
        report["settings_map"] = prepare_veconfigure(raw, folder)
        report["stage"] = "reference_bindings"
        bindings = folder / "veconfigure-bindings.json"
        subprocess.run([sys.executable, "-B", str(TOOLS / "veconfigure_bindings.py"), "--exe", str(exe),
                        "--report", str(bindings)], check=True, timeout=45,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        report["bindings"] = json.loads(bindings.read_bytes())
        report["stage"] = "native_capture"
        native = folder / "native-report.json"
        result = subprocess.run([sys.executable, "-B", str(TOOLS / "native_veconfigure.py"),
                                 "--fixture", str(folder / "synthetic.rvsc"), "--report", str(native),
                                 "--exe", str(exe), "--isolated"], timeout=210,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        report["native"] = json.loads(native.read_bytes())
        if result.returncode == 0 and report["native"].get("status") == "completed":
            report.update(status="completed", stage="complete")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["error_type"] = type(error).__name__
    write(folder / "report.json", report)
    print(f"Private compatibility processing {report['status']} at {report['stage']}.")
    return int(report["status"] != "completed")


def complete(folder):
    lease = lease_info(folder)
    path = folder / "report.json"
    report = json.loads(path.read_bytes()) if path.is_file() else {
        "status": "failed", "stage": "setup", "reason": "The hosted worker could not prepare the reference application."
    }
    body = {"receipt": lease["receipt"], "lease": lease["lease"], "status": report["status"], "report": report}
    if len(encoded(body)) > MAX_REPORT_BYTES:
        body["report"] = {"status": "failed", "stage": "report", "reason": "The worker report exceeded its size limit."}
        body["status"] = "failed"
    post("/api/internal/compatibility/complete", body)
    print("Private compatibility report returned to the server.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("claim", "process", "complete"))
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--exe", type=Path)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        parser.error("Use this worker only on a disposable GitHub-hosted runner.")
    try:
        if args.action == "claim":
            claim(args.work_dir)
        elif args.action == "complete":
            complete(args.work_dir)
        else:
            if args.exe is None:
                parser.error("--exe is required for processing.")
            return process(args.work_dir, args.exe)
    except (OSError, ValueError, KeyError) as error:
        print(f"Private compatibility worker could not complete {args.action} ({type(error).__name__}).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
