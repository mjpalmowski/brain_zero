# Website compatibility intake

Build from the repository root with `docker build -f website/intake/Dockerfile .`.
The service listens on port 8080. Keep the container port private behind nginx.

Mount persistent storage at `/var/lib/brain-zero-intake`, owned by UID/GID
10001 with mode 0700. `INTAKE_SPOOL` changes the path. `INTAKE_ORIGINS` is a
comma-separated list of exact allowed origins. Defaults are
`https://brain-zero.com` and `https://www.brain-zero.com`.

`POST /api/compatibility` accepts JSON fields `model`, optional `email`,
`consent: true`, and the downloaded compatibility bundle as `profile`.
The total request limit is 280 KiB. The profile validator separately enforces
256 KiB for the inner bundle. A successful request returns HTTP 202 with a
`receipt` and `status: "queued"`. The normalized profile, private metadata,
synthetic RVSC and settings map are saved atomically.

`POST /api/internal/compatibility/claim` and
`POST /api/internal/compatibility/complete` require the GitHub OIDC identity
verified by `oidc.py`. Claim takes string `run_id` and `run_attempt` fields.
It returns HTTP 204 when idle or `{receipt, lease, profile}`, without contact
information. Leases expire after 15 minutes. Completion takes
`{receipt, lease, status, report}`. Status must be `completed`, `failed` or
`blocked`. Allow the completion body up to 2 MiB plus 2048 bytes.

Receipt directories hold `metadata.json` with queue state. Expired processing
leases can be reclaimed. Reports remain private as `worker-report.json`.
Repeated claims from the same active run return its existing lease, and an
identical repeated completion succeeds. Every completion checks the lease
against the authenticated run.

`GET /health` reports service availability. Logs omit submissions and contact
information. No public file-download route is exposed.

Run tests with `python -B -m unittest discover -s website/intake -p test_app.py -v`.
