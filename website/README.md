# Website compatibility intake

The brain_zero web app submits a freshly read sanitized compatibility profile
directly to `POST https://brain-zero.com/api/compatibility`. The visitor enters
the inverter model and chooses **Submit to developer** inside the app. Progress,
errors and the submission reference stay in the app.

Visitors with a current `.rvsc` backup can also use `/compatibility/`. The browser
reads and sanitizes the file locally, displays the detected model/firmware/voltage,
then submits only `{model, consent: true, profile}` to the same endpoint. It never
sends the original bytes or filename and does not request contact details. The
model is derived from the backup's schema firmware using the firmware model
catalogue and cannot be edited. Submission derives it again from the profile,
not the displayed value. The page displays the receipt without polling or
automatically retrying ambiguous network failures. Public GitHub issues remain an optional developer path for
sanitized JSON only.

Run `python -B tools/build_hosted_sanitizer.py` after modifying the standalone
sanitizer. It builds the hosted `sanitizer.js` from the exact same parser source.
Run `python -B tools/build_hosted_models.py` after changing the firmware model
catalogue. It generates the hosted model lookup from `multiplus_ii.h`.
Deploy all files in `public/compatibility/` along with the homepage link. The
existing nginx same-origin script/style/connect policy supports this page; no
inline script or external libraries are needed.

The public intake endpoint accepts browser CORS requests without credentials so
the controller’s local web interface can use HTTPS directly. Its envelope contains
the model, explicit consent and the sanitized profile. The controller does not
perform the external TLS upload or hold a developer API credential. Internal
worker endpoints keep their separate GitHub OIDC authentication.

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
