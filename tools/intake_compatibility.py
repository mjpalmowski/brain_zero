"""Validate an already-sanitized GitHub profile for offline review.

Original backups must be sanitized locally before attachment. GitHub uploads
issue files before Actions starts and does not expose a supported deletion API.
"""

import argparse
import hashlib
from http.client import HTTPException
from io import BytesIO
import json
import os
from pathlib import Path
import re
import stat
import sys
from urllib.error import URLError
from urllib.parse import unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
from zipfile import BadZipFile, LargeZipFile, ZIP_DEFLATED, ZIP_STORED, ZipFile
from zlib import error as ZlibError

from export_compatibility import MAX_BACKUP_BYTES, export_compatibility
from validate_compatibility import MAX_PROFILE_BYTES, validate_profile


PROFILE_FIELD = "Settings file or compatibility profile"
PROFILE_FIELDS = (PROFILE_FIELD, "Downloaded compatibility profile")
MAX_ZIP_BYTES = MAX_BACKUP_BYTES + 64 * 1024
ATTACHMENT_URL = (
    r"https://github\.com/user-attachments/files/"
    r"[1-9][0-9]{0,19}/([A-Za-z0-9._%()+-]{1,190}\.(?:json|rvsc|zip))"
)
ATTACHMENT = re.compile(
    r"\[[^\]\r\n]{1,200}\]\((" + ATTACHMENT_URL + r")\)",
    re.IGNORECASE | re.ASCII,
)
URL = re.compile(ATTACHMENT_URL, re.IGNORECASE | re.ASCII)


def attachment_kind(url):
    """Validate the public attachment URL, including its decoded filename."""
    match = URL.fullmatch(url)
    if not match:
        raise ValueError("Use a GitHub attachment ending in .json, .rvsc or .zip.")
    filename = unquote(match[1])
    if not re.fullmatch(r"[^/\\:\x00-\x1f\x7f]+\.(?:json|rvsc|zip)", filename, re.IGNORECASE):
        raise ValueError("The attachment filename is invalid.")
    return filename.rsplit(".", 1)[1].lower()


def attachment_limit(kind):
    return {"json": MAX_PROFILE_BYTES, "rvsc": MAX_BACKUP_BYTES, "zip": MAX_ZIP_BYTES}[kind]


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
    # The request is anonymous, including after redirects. Check the URL here
    # too so alternate callers cannot turn the downloader into an arbitrary GET.
    limit = attachment_limit(attachment_kind(url))
    request = Request(url, headers={"Accept": "application/json, application/zip, application/octet-stream",
                                    "User-Agent": "brain-zero-compatibility-intake"})
    try:
        with build_opener(AttachmentRedirect()).open(request, timeout=15) as response:
            raw = response.read(limit + 1)
    except (URLError, OSError, HTTPException) as error:
        raise ValueError("Could not download the public attachment from GitHub.") from error
    if len(raw) > limit:
        raise ValueError("The attachment exceeds its size limit.")
    return raw


def zipped_backup(raw):
    """Read one small regular RVSC member in memory; never extract the ZIP."""
    try:
        with ZipFile(BytesIO(raw)) as archive:
            members = archive.infolist()
            if len(members) != 1:
                raise ValueError("The ZIP must contain exactly one .rvsc file and no other entries.")
            member = members[0]
            name = member.filename
            mode = stat.S_IFMT(member.external_attr >> 16)
            if (member.orig_filename != name or
                    not re.fullmatch(r"[^/\\:\x00-\x1f\x7f]+\.rvsc", name, re.IGNORECASE) or
                    member.is_dir() or member.external_attr & 0x10 or
                    mode not in (0, stat.S_IFREG)):
                raise ValueError("The ZIP must contain one regular .rvsc file without folders or paths.")
            if member.flag_bits & 1:
                raise ValueError("Encrypted ZIP files are not supported.")
            if member.compress_type not in (ZIP_STORED, ZIP_DEFLATED):
                raise ValueError("Use a standard stored or deflated ZIP file.")
            if not 0 < member.file_size <= MAX_BACKUP_BYTES:
                raise ValueError("The .rvsc inside the ZIP exceeds the 128 KiB backup size limit or is empty.")
            with archive.open(member) as backup:
                data = backup.read(MAX_BACKUP_BYTES + 1)
            if len(data) != member.file_size or len(data) > MAX_BACKUP_BYTES:
                raise ValueError("The .rvsc inside the ZIP has an invalid size.")
            return data
    except (BadZipFile, LargeZipFile, OSError, EOFError, RuntimeError, NotImplementedError, ZlibError):
        # ZIP exceptions can contain member filenames. Keep those, and any
        # original backup content, out of review reports and workflow logs.
        raise ValueError("The ZIP is invalid, damaged or unsupported.") from None


def sanitize_attachment(raw, kind):
    """Return only validated public metadata, discarding private backup fields."""
    if kind not in ("json", "rvsc", "zip"):
        raise ValueError("Unsupported attachment type.")
    if len(raw) > attachment_limit(kind):
        raise ValueError("The attachment exceeds its size limit.")
    if kind == "json":
        return validate_profile(raw)
    backup = zipped_backup(raw) if kind == "zip" else raw
    bundle = export_compatibility(backup)
    return validate_profile(json.dumps(bundle, allow_nan=False).encode("utf-8"))


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
    fields = [field for field in PROFILE_FIELDS if field in sections]
    if len(fields) != 1:
        raise ValueError("Use exactly one settings file or compatibility profile field.")
    match = ATTACHMENT.fullmatch(sections[fields[0]])
    if not match:
        raise ValueError("Attach exactly one sanitized compatibility.json profile.")
    if attachment_kind(match[1]) != "json":
        raise ValueError("Original .rvsc and ZIP files are not accepted. Sanitize the backup locally before uploading compatibility.json.")
    # Original backup filenames can themselves include an inverter serial.
    # Use a fixed public filename and never copy its source URL into artifacts.
    if unquote(URL.fullmatch(match[1])[1]) != "compatibility.json":
        raise ValueError("Use the generic filename compatibility.json; do not include serial numbers in filenames.")

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
        kind = attachment_kind(url)
        raw = download_profile(url)
        report.update(attachment_type=kind, attachment_bytes=len(raw),
                      attachment_sha256=hashlib.sha256(raw).hexdigest())
        fixture, details = sanitize_attachment(raw, kind)
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
                any(f"### {field}" in (issue.get("body") or "") for field in PROFILE_FIELDS)):
            return 0
    else:
        parser.error("Unsupported intake trigger.")
    return process(issue, repository, args.output_dir)


if __name__ == "__main__":
    sys.exit(main())
