"""Validate a GitHub issue attachment and prepare files for offline review."""

import argparse
import hashlib
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import URLError
from urllib.parse import unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from validate_compatibility import MAX_PROFILE_BYTES, validate_profile


PROFILE_FIELD = "Downloaded compatibility profile"
ATTACHMENT = re.compile(
    r"\[[^\]\r\n]{1,200}\]\((https://github\.com/user-attachments/files/"
    r"[1-9][0-9]{0,19}/([A-Za-z0-9._%()+-]{1,190}\.json))\)",
    re.IGNORECASE | re.ASCII,
)


def issue_number(value):
    if type(value) not in (int, str) or not re.fullmatch(r"[1-9][0-9]{0,9}", str(value)):
        raise ValueError("Enter a valid issue number.")
    return int(value)


class AttachmentRedirect(HTTPRedirectHandler):
    """GitHub redirects issue attachments to its signed object storage URLs."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urlsplit(newurl)
        if target.scheme != "https" or target.netloc != "objects.githubusercontent.com":
            raise ValueError("The attachment redirected outside GitHub object storage.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def download_profile(url):
    # Only the form's validated GitHub attachment URL reaches this function.
    # The request is anonymous, including after redirects.
    request = Request(url, headers={"Accept": "application/json, application/octet-stream",
                                    "User-Agent": "brain-zero-compatibility-intake"})
    try:
        with build_opener(AttachmentRedirect()).open(request, timeout=15) as response:
            raw = response.read(MAX_PROFILE_BYTES + 1)
    except (URLError, OSError, HTTPException) as error:
        raise ValueError("Could not download the public attachment from GitHub.") from error
    if len(raw) > MAX_PROFILE_BYTES:
        raise ValueError("The attachment exceeds the 256 KiB profile size limit.")
    return raw


def load_issue(repository, number):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "brain-zero-compatibility-intake"}
    if token := os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = "Bearer " + token
    request = Request(f"https://api.github.com/repos/{repository}/issues/{number}", headers=headers)
    try:
        with build_opener(NoRedirect()).open(request, timeout=15) as response:
            issue = json.loads(response.read(1024 * 1024))
    except (URLError, OSError, HTTPException, ValueError) as error:
        raise ValueError("Could not read the issue from GitHub.") from error
    if not isinstance(issue, dict) or issue.get("number") != number or "pull_request" in issue:
        raise ValueError("The selected number must be a repository issue.")
    return issue


def submission(body):
    parts = re.split(r"^###[ \t]+([^\r\n]+)\r?$", body, flags=re.MULTILINE)
    sections = {}
    for heading, value in zip(parts[1::2], parts[2::2]):
        heading = heading.strip()
        if heading in sections:
            raise ValueError("The issue contains duplicate form fields.")
        sections[heading] = value.strip()

    if sections.get("Inverter model", "") in ("", "_No response_"):
        raise ValueError("Enter the inverter model using the compatibility issue form.")
    match = ATTACHMENT.fullmatch(sections.get(PROFILE_FIELD, ""))
    if not match:
        raise ValueError("Attach exactly one JSON file in the compatibility profile field.")
    if not re.fullmatch(r"[^/\\:\x00-\x1f\x7f]+\.json", unquote(match[2]), re.IGNORECASE):
        raise ValueError("The attachment filename is invalid.")

    declared_firmware = sections.get("Firmware shown by Victron", "")
    if declared_firmware in ("", "_No response_"):
        return match[1], None
    if not re.fullmatch(r"[0-9]{7}", declared_firmware):
        raise ValueError("Enter the seven-digit VE.Bus firmware number or leave the field blank.")
    return match[1], int(declared_firmware)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def process(issue, repository, output_dir):
    number = issue_number(issue["number"])
    issue_url = f"https://github.com/{repository}/issues/{number}"
    report = {"issue": number, "issue_url": issue_url}
    heading = f"## Compatibility profile intake for [issue #{number}]({issue_url})\n\n"
    output_dir.mkdir(parents=True, exist_ok=False)
    try:
        url, declared_firmware = submission(issue.get("body") or "")
        raw = download_profile(url)
        report.update(attachment_url=url, attachment_bytes=len(raw),
                      attachment_sha256=hashlib.sha256(raw).hexdigest())
        fixture, details = validate_profile(raw)
        if declared_firmware is not None and declared_firmware != details["firmware"]:
            raise ValueError("The firmware entered on the issue differs from the attachment.")
    except ValueError as error:
        report.update(status="rejected", reason=str(error))
        summary = heading + f"**Profile needs changes.** {error}\n"
    else:
        write_json(output_dir / "profile.json", fixture)
        write_json(output_dir / "simulator-profile.json", fixture["profile"])
        report.update(details, status="validated", qualification="pending_review")
        summary = heading + (
            f"**Validated for review.** VE.Bus firmware `{details['firmware']}` "
            f"· {details['nominal_voltage']} V nominal.\n\n"
            f"- Attachment SHA-256: `{report['attachment_sha256']}`\n"
            f"- Descriptor SHA-256: `{details['descriptor_sha256']}`\n"
            f"- Grid SHA-256: `{details['grid_sha256']}`\n\n"
            "The artifact includes the review report and simulator inputs. "
            "ESS and settings-write qualification still needs review.\n"
        )
    write_json(output_dir / "report.json", report)
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary_path).open("a", encoding="utf-8") as handle:
            handle.write(summary)
    if report["status"] == "rejected":
        print(report["reason"], file=sys.stderr)
        return 1
    print(f"Validated issue #{number} for review.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    repository = os.environ["GITHUB_REPOSITORY"]
    if not re.fullmatch(r"[A-Za-z0-9-]+/[A-Za-z0-9_.-]+", repository):
        parser.error("Invalid GitHub repository name.")
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_bytes())
    event_name = os.environ["GITHUB_EVENT_NAME"]
    if event_name == "workflow_dispatch":
        try:
            issue = load_issue(repository, issue_number(event["inputs"]["issue_number"]))
        except ValueError as error:
            parser.error(str(error))
    elif event_name == "issues":
        issue = event["issue"]
        if event.get("action") == "edited" and "body" not in event.get("changes", {}):
            return 0
        if not ((issue.get("title") or "").casefold().startswith("[compatibility]") or
                f"### {PROFILE_FIELD}" in (issue.get("body") or "")):
            return 0
    else:
        parser.error("Unsupported intake trigger.")
    return process(issue, repository, args.output_dir)


if __name__ == "__main__":
    sys.exit(main())
