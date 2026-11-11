# Website compatibility intake

The main website serves the upload form at `/compatibility/`. Older firmware can
download `compatibility.json` for file selection. The local web app can also send
a freshly read profile to the form through a nonce-bound opener handshake. The
visitor reviews the model, optional email and consent before submitting.

The Python service accepts only validated compatibility profiles, writes a
private atomic queue entry and prepares the synthetic VEConfigure fixture. It
stores no raw owner RVSC backup. `intake-compose.yml` builds from the repository
root and joins the existing website's external Docker network.

Create `/opt/brain-zero/intake-data` with owner UID/GID 10001 and mode 0700, then
run the compose service. Keep that directory outside the static website root.
Merge the two API locations and upload limits from `nginx.conf` into the site's
nginx configuration. The backend port is available only on the Docker network.

The `server-compatibility.yml` GitHub Actions workflow checks for one queued
submission every five minutes. GitHub OIDC authenticates its exact hosted
workflow on the main branch. The service verifies the signature, audience,
repository IDs, branch, event and run identity, then grants a 15-minute lease.
The workflow returns settings maps and native reports to the private server
queue. It creates no public Actions artifacts and receives no contact email.

The OIDC policy in `intake/oidc.py` is pinned to this repository and workflow.
Review those identities if deploying a fork. The VPS needs no GitHub access token.

Run backend checks from the repository root with
`python -B -m unittest discover -s website/intake -p test_app.py`.
