import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

import app
from test_prepare_veconfigure import profile_bytes


def submission(**updates):
    body = {"model": "MultiPlus-II 48/5000/70-50", "email": "owner@example.com",
            "consent": True, "profile": json.loads(profile_bytes())}
    body.update(updates)
    return body


class IntakeTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

        def verify(token):
            if token not in ("worker", "other"):
                raise ValueError("Bad signature.")
            return {"run_id": "17" if token == "worker" else "18", "run_attempt": "1"}
        self.server = app.create_server(("127.0.0.1", 0), self.directory.name, verify_oidc=verify)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def request(self, path, body, *, token=None, origin=None, content_type="application/json",
                method="POST", request_headers=None, include_headers=False):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=3)
        headers = {"Content-Type": content_type}
        if token:
            headers["Authorization"] = "Bearer " + token
        if origin:
            headers["Origin"] = origin
        headers.update(request_headers or {})
        raw = app.canonical_json(body) if not isinstance(body, bytes) else body
        connection.request(method, path, raw, headers)
        response = connection.getresponse()
        value = response.read()
        result = json.loads(value) if value else None
        response_headers = dict(response.getheaders())
        connection.close()
        return (response.status, result, response_headers) if include_headers else (response.status, result)

    def accept(self):
        status, result = self.request(app.PATH, submission(), origin="http://brain-zero.local")
        self.assertEqual(status, 202)
        return result

    def test_accepts_and_prepares_private_atomic_job(self):
        receipt = self.accept()["receipt"]
        folder = Path(self.directory.name) / receipt
        self.assertRegex(receipt, r"^[0-9a-f]{32}$")
        self.assertEqual(sorted(path.name for path in folder.iterdir()),
                         ["metadata.json", "profile.json", "settings-map.json", "synthetic.rvsc"])
        metadata = json.loads((folder / "metadata.json").read_bytes())
        self.assertEqual(metadata["state"], "queued")
        self.assertEqual(metadata["email"], "owner@example.com")
        self.assertTrue(metadata["consent"])
        normalized = json.loads((folder / "profile.json").read_bytes())
        self.assertEqual(set(normalized), {"profile", "grid"})
        self.assertNotIn("email", normalized)

    def test_rejects_bad_profile_consent_duplicate_and_oversized_input(self):
        invalid = submission()
        invalid["profile"]["profile"]["settings"][2][2] += 1
        for body in (invalid, submission(consent=False),
                     b'{"model":"one","model":"two","consent":true,"profile":{}}'):
            status, result = self.request(app.PATH, body)
            self.assertEqual(status, 400)
            self.assertFalse(result["ok"])
        status, _ = self.request(app.PATH, b"x" * (app.MAX_REQUEST_BYTES + 1))
        self.assertEqual(status, 413)
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])

    def test_rejects_non_json(self):
        self.assertEqual(self.request(app.PATH, submission(), content_type="text/plain")[0], 415)
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])

    def test_local_web_app_can_preflight_and_submit_without_credentials(self):
        for origin in ("http://192.168.68.94", "http://vebus-listener.local", "capacitor://localhost"):
            with self.subTest(origin=origin):
                status, body, headers = self.request(
                    app.PATH, b"", origin=origin, method="OPTIONS", include_headers=True,
                    request_headers={"Access-Control-Request-Method": "POST",
                                     "Access-Control-Request-Headers": "content-type"})
                self.assertEqual((status, body), (204, None))
                self.assertEqual(headers["Access-Control-Allow-Origin"], "*")
                self.assertEqual(headers["Access-Control-Allow-Methods"], "POST")
                self.assertEqual(headers["Access-Control-Allow-Headers"], "Content-Type")
                self.assertNotIn("Access-Control-Allow-Credentials", headers)
                status, result, headers = self.request(
                    app.PATH, submission(email=""), origin=origin, include_headers=True)
                self.assertEqual(status, 202)
                self.assertTrue(result["ok"])
                self.assertEqual(headers["Access-Control-Allow-Origin"], "*")
                self.assertNotIn("Access-Control-Allow-Credentials", headers)

    def test_invalid_profile_remains_readable_to_local_app(self):
        status, result, headers = self.request(
            app.PATH, submission(consent=False), origin="http://192.168.68.94", include_headers=True)
        self.assertEqual(status, 400)
        self.assertFalse(result["ok"])
        self.assertEqual(headers["Access-Control-Allow-Origin"], "*")
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])

    def test_preflight_cannot_enable_worker_or_credential_requests(self):
        for headers in ({"Access-Control-Request-Method": "DELETE"},
                        {"Access-Control-Request-Method": "POST",
                         "Access-Control-Request-Headers": "Content-Type, Authorization"}):
            status, _, response = self.request(app.PATH, b"", method="OPTIONS", include_headers=True,
                                               request_headers=headers)
            self.assertEqual(status, 403)
            self.assertNotIn("Access-Control-Allow-Methods", response)
        for route in (app.CLAIM_PATH, app.COMPLETE_PATH):
            status, _, headers = self.request(route, b"", method="OPTIONS", include_headers=True,
                                               request_headers={"Access-Control-Request-Method": "POST"})
            self.assertEqual(status, 404)
            self.assertNotIn("Access-Control-Allow-Origin", headers)
            status, _, headers = self.request(route, {}, origin="http://brain-zero.local", include_headers=True)
            self.assertEqual(status, 401)
            self.assertNotIn("Access-Control-Allow-Origin", headers)

    def test_worker_identity_claim_retry_and_completion(self):
        receipt = self.accept()["receipt"]
        claim_request = {"run_id": "17", "run_attempt": "1"}
        self.assertEqual(self.request(app.CLAIM_PATH, claim_request)[0], 401)
        self.assertEqual(self.request(app.CLAIM_PATH, claim_request, token="bad")[0], 401)
        self.assertEqual(self.request(app.CLAIM_PATH, claim_request, token="other")[0], 403)
        status, claimed = self.request(app.CLAIM_PATH, claim_request, token="worker")
        self.assertEqual(status, 200)
        self.assertEqual(claimed["receipt"], receipt)
        self.assertEqual(set(claimed), {"receipt", "lease", "profile"})
        self.assertEqual(self.request(app.CLAIM_PATH, claim_request, token="worker")[1], claimed)
        body = {"receipt": receipt, "lease": claimed["lease"], "status": "completed",
                "report": {"native": {"status": "completed"}}}
        self.assertEqual(self.request(app.COMPLETE_PATH, body, token="other")[0], 409)
        self.assertEqual(self.request(app.COMPLETE_PATH, body, token="worker")[0], 200)
        self.assertEqual(self.request(app.COMPLETE_PATH, body, token="worker")[0], 200)
        self.assertEqual(self.request(app.CLAIM_PATH, claim_request, token="worker"), (204, None))
        folder = Path(self.directory.name) / receipt
        self.assertEqual(json.loads((folder / "metadata.json").read_bytes())["state"], "completed")
        self.assertEqual(json.loads((folder / "worker-report.json").read_bytes()), body["report"])

    def test_expired_worker_is_reclaimed_and_old_lease_cannot_complete(self):
        self.accept()
        first = self.server.queue.claim("17", "1")
        with patch.object(app.time, "time", return_value=app.time.time() + app.LEASE_SECONDS + 1):
            second = self.server.queue.claim("18", "1")
        self.assertEqual(first["receipt"], second["receipt"])
        self.assertNotEqual(first["lease"], second["lease"])
        with self.assertRaisesRegex(app.SubmissionError, "invalid"):
            self.server.queue.complete({"receipt": first["receipt"], "lease": first["lease"],
                                       "status": "completed", "report": {}}, "17", "1")

    def test_preparation_failure_leaves_no_partially_visible_job(self):
        with patch.object(app, "prepare_veconfigure", side_effect=ValueError("Unexpected grid descriptor.")):
            self.assertEqual(self.request(app.PATH, submission())[0], 503)
        self.assertEqual(list(Path(self.directory.name).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
