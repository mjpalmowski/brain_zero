"""Authenticate this repository's hosted compatibility worker with GitHub OIDC."""

import json
import threading
import time
from urllib.request import HTTPRedirectHandler, Request, build_opener

import jwt


ISSUER = "https://token.actions.githubusercontent.com"
AUDIENCE = "https://brain-zero.com/compatibility-intake"
JWKS_URL = ISSUER + "/.well-known/jwks"
REPOSITORY_ID = "1452774554"
OWNER_ID = "117575550"
WORKFLOW_REF = "mjpalmowski/brain_zero/.github/workflows/server-compatibility.yml@refs/heads/main"
_lock = threading.Lock()
_keys = {}
_refreshed = 0.0


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _key(kid):
    global _keys, _refreshed
    with _lock:
        now = time.monotonic()
        if not _keys or now - _refreshed >= 300 or (kid not in _keys and now - _refreshed >= 30):
            request = Request(JWKS_URL, headers={"Accept": "application/json",
                                               "User-Agent": "brain_zero worker identity verifier"})
            try:
                with build_opener(NoRedirect()).open(request, timeout=5) as response:
                    raw = response.read(64 * 1024 + 1)
                if len(raw) > 64 * 1024:
                    raise ValueError("Oversized GitHub signing-key response.")
                document = json.loads(raw)
                keys = {}
                for item in document["keys"]:
                    if (item.get("kty") == "RSA" and item.get("use", "sig") == "sig" and
                            item.get("alg", "RS256") == "RS256" and
                            isinstance(item.get("kid"), str) and len(item["kid"]) <= 128):
                        keys[item["kid"]] = jwt.PyJWK.from_dict(item, algorithm="RS256").key
                if not keys:
                    raise ValueError("No GitHub signing keys.")
            except (OSError, ValueError, KeyError, TypeError, jwt.PyJWTError) as error:
                raise ValueError("GitHub worker identity could not be verified.") from error
            _keys, _refreshed = keys, now
        if kid not in _keys:
            raise ValueError("Unrecognized GitHub signing key.")
        return _keys[kid]


def verify_token(token):
    """Return verified claims only for this workflow on main, never a pull request."""
    if not isinstance(token, str) or not 32 < len(token) <= 16 * 1024:
        raise ValueError("A GitHub worker identity is required.")
    try:
        header = jwt.get_unverified_header(token)
        kid = header.get("kid")
        if header.get("alg") != "RS256" or not isinstance(kid, str) or not 0 < len(kid) <= 128:
            raise ValueError("Unsupported GitHub signing key.")
        claims = jwt.decode(token, _key(kid), algorithms=["RS256"], issuer=ISSUER,
                            audience=AUDIENCE, leeway=15,
                            options={"require": ["iss", "aud", "exp", "iat", "nbf", "repository_id",
                                                 "repository_owner_id", "workflow_ref", "ref", "event_name",
                                                 "run_id", "run_attempt", "runner_environment"]})
        expected = {
            "repository_id": REPOSITORY_ID,
            "repository_owner_id": OWNER_ID,
            "workflow_ref": WORKFLOW_REF,
            "ref": "refs/heads/main",
            "runner_environment": "github-hosted",
        }
        if any(claims.get(name) != value for name, value in expected.items()):
            raise ValueError("The GitHub worker is outside the permitted workflow.")
        if claims["event_name"] not in ("schedule", "workflow_dispatch"):
            raise ValueError("The GitHub workflow event is not permitted.")
        if (type(claims["iat"]) is not int or type(claims["exp"]) is not int or
                not 0 < claims["exp"] - claims["iat"] <= 900):
            raise ValueError("The GitHub identity lifetime is invalid.")
        for name in ("run_id", "run_attempt"):
            if not isinstance(claims[name], str) or not claims[name].isdigit() or not 0 < len(claims[name]) <= 20:
                raise ValueError("The GitHub run identity is invalid.")
        return claims
    except (jwt.PyJWTError, KeyError, TypeError) as error:
        raise ValueError("The GitHub worker identity is invalid or expired.") from error
