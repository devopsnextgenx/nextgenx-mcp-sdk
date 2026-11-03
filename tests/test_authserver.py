#!/usr/bin/env python3
"""Unsafe, test-only OAuth authorization server for Phase 6 acceptance tests."""

import base64
import hashlib
import hmac
import json
import os
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def b64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def read_tlv(data, offset):
    tag = data[offset]
    offset += 1
    length = data[offset]
    offset += 1
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[offset:offset + count], "big")
        offset += count
    end = offset + length
    return tag, data[offset:end], end


def rsa_numbers(public_der):
    _, outer, _ = read_tlv(public_der, 0)
    _, _, pos = read_tlv(outer, 0)
    _, bit_string, _ = read_tlv(outer, pos)
    _, sequence, _ = read_tlv(bit_string, 1)
    _, modulus, pos = read_tlv(sequence, 0)
    _, exponent, _ = read_tlv(sequence, pos)
    return int.from_bytes(modulus, "big"), int.from_bytes(exponent, "big")


def ec_coordinates(public_der):
    _, outer, _ = read_tlv(public_der, 0)
    _, _, pos = read_tlv(outer, 0)
    _, bit_string, _ = read_tlv(outer, pos)
    point = bit_string[1:]
    return point[1:33], point[33:65]


def der_ecdsa_to_raw(signature):
    _, sequence, _ = read_tlv(signature, 0)
    _, r, pos = read_tlv(sequence, 0)
    _, s, _ = read_tlv(sequence, pos)
    return r[-32:].rjust(32, b"\0") + s[-32:].rjust(32, b"\0")


class SigningKey:
    def __init__(self, directory, kid, algorithm="RS256"):
        self.kid = kid
        self.algorithm = algorithm
        self.path = os.path.join(directory, kid + ".pem")
        if algorithm == "ES256":
            args = ["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                    "ec_paramgen_curve:P-256", "-out", self.path]
        else:
            args = ["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                    "rsa_keygen_bits:2048", "-out", self.path]
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=15)
        public_der = subprocess.run(
            ["openssl", "pkey", "-in", self.path, "-pubout", "-outform", "DER"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=5).stdout
        if algorithm == "ES256":
            x, y = ec_coordinates(public_der)
            self.jwk = {"kty": "EC", "crv": "P-256", "x": b64url(x),
                        "y": b64url(y), "use": "sig", "alg": algorithm,
                        "kid": kid}
        else:
            n, e = rsa_numbers(public_der)
            self.public_der = public_der
            self.jwk = {"kty": "RSA", "n": b64url(n.to_bytes((n.bit_length() + 7) // 8, "big")),
                        "e": b64url(e.to_bytes((e.bit_length() + 7) // 8, "big")),
                        "use": "sig", "alg": algorithm, "kid": kid}

    def sign(self, value):
        signature = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", self.path],
            input=value, check=True, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, timeout=5).stdout
        return der_ecdsa_to_raw(signature) if self.algorithm == "ES256" else signature


class State:
    DEFAULT_CONFIG = {
        "cimd": "1", "dcr": "1", "pkce": "1", "iss_param": "good",
        "iss_advertised": "1", "issuer_doc": "good", "ttl": "3600",
        "rotate": "1", "grant_limit": "", "state": "good",
        "introspect_fail": "0",
    }

    def __init__(self, issuer):
        self.issuer = issuer.rstrip("/")
        self.lock = threading.RLock()
        self.temp = tempfile.TemporaryDirectory(prefix="cppmcp-test-as-")
        self.keys = {}
        self.rotate()
        self.config = dict(self.DEFAULT_CONFIG)
        self.codes = {}
        self.refresh = {}
        self.opaque = {}
        self.revoked = set()
        self.stats = self.new_stats()

    @staticmethod
    def new_stats():
        return {"authorize": [], "token": [], "register": [], "cimd_fetches": [],
                "metadata_fetches": 0, "jwks_requests": 0,
                "introspect_requests": 0, "bearer_on_non_resource_endpoints": 0}

    def rotate(self):
        kid = "key-" + secrets.token_hex(4)
        self.keys[kid] = SigningKey(self.temp.name, kid)
        self.active_kid = kid

    def sign_jwt(self, claims, algorithm="RS256", kid=None, badsig=False,
                 public_hmac=False):
        selected = self.keys.get(kid or self.active_kid) or \
            self.keys[self.active_kid]
        if algorithm == "none":
            header = {"alg": "none", "typ": "at+jwt"}
            encoded = b64url(json.dumps(header, separators=(",", ":")).encode())
            payload = b64url(json.dumps(claims, separators=(",", ":")).encode())
            return encoded + "." + payload + "."
        signing_key = selected
        if algorithm == "ES256":
            signing_key = next((key for key in self.keys.values()
                                if key.algorithm == "ES256"), None)
            if signing_key is None:
                signing_key = SigningKey(self.temp.name, "ec-" + secrets.token_hex(3), "ES256")
                self.keys[signing_key.kid] = signing_key
        header = {"alg": algorithm, "typ": "at+jwt", "kid": kid or signing_key.kid}
        encoded = b64url(json.dumps(header, separators=(",", ":")).encode())
        payload = b64url(json.dumps(claims, separators=(",", ":")).encode())
        message = (encoded + "." + payload).encode("ascii")
        if algorithm == "HS256":
            secret = signing_key.public_der if public_hmac else b"wrong-test-secret"
            signature = hmac.new(secret, message, hashlib.sha256).digest()
        elif algorithm in ("RS256", "ES256"):
            signature = signing_key.sign(message)
        else:
            signature = b"\0" * 256
        if badsig and signature:
            signature = bytes([signature[0] ^ 1]) + signature[1:]
        return encoded + "." + payload + "." + b64url(signature)

    def token(self, client_id, resource, scope, subject="alice", ttl=None):
        now = int(time.time())
        expires = now + int(ttl if ttl is not None else self.config["ttl"])
        grant_limit = self.config["grant_limit"].split()
        if grant_limit:
            scope = " ".join(
                item for item in scope.split() if item in set(grant_limit))
        claims = {"iss": self.issuer, "sub": subject, "aud": resource,
                  "scope": scope, "client_id": client_id, "iat": now,
                  "exp": expires, "jti": secrets.token_hex(12)}
        access = self.sign_jwt(claims)
        refresh_token = secrets.token_urlsafe(32)
        self.refresh[refresh_token] = (client_id, resource, scope, subject)
        return {"access_token": access, "token_type": "Bearer",
                "expires_in": max(0, expires - now), "scope": scope,
                "refresh_token": refresh_token}

    def metadata(self):
        issuer = self.issuer if self.config["issuer_doc"] == "good" else self.issuer + "/wrong"
        result = {
            "issuer": issuer, "authorization_endpoint": self.issuer + "/authorize",
            "token_endpoint": self.issuer + "/token", "jwks_uri": self.issuer + "/jwks",
            "registration_endpoint": self.issuer + "/register",
            "introspection_endpoint": self.issuer + "/introspect",
            "authorization_response_iss_parameter_supported": self.config["iss_advertised"] == "1",
            "client_id_metadata_document_supported": self.config["cimd"] == "1",
            "scopes_supported": ["mcp:use", "projects:write", "projects:admin",
                                 "reports:read", "files:read", "files:write"],
            "token_endpoint_auth_methods_supported": ["none", "client_secret_basic",
                                                       "client_secret_post"],
        }
        if self.config["pkce"] == "1":
            result["code_challenge_methods_supported"] = ["S256"]
        if self.config["dcr"] == "0":
            result.pop("registration_endpoint")
        return result


def json_response(handler, status, value, headers=None):
    body = json.dumps(value, separators=(",", ":")).encode()
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    for key, value in (headers or {}).items():
        handler.send_header(key, value)
    handler.end_headers()
    handler.wfile.write(body)


class Handler(BaseHTTPRequestHandler):
    server_version = "cppmcp-test-authserver/1"

    @property
    def state(self):
        return self.server.state

    def log_message(self, _format, *_args):
        return

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        path, query = parsed.path, urllib.parse.parse_qs(parsed.query)
        state = self.state
        if path == "/":
            return json_response(self, 200, {"ok": True})
        if path.startswith("/.well-known/oauth-authorization-server") or \
                path.startswith("/.well-known/openid-configuration"):
            with state.lock:
                state.stats["metadata_fetches"] += 1
                metadata = state.metadata()
            return json_response(self, 200, metadata)
        if path == "/jwks":
            with state.lock:
                state.stats["jwks_requests"] += 1
                keys = [key.jwk for key in state.keys.values()]
            return json_response(self, 200, {"keys": keys},
                                 {"Cache-Control": "public, max-age=60"})
        if path == "/authorize":
            return self.authorize(query)
        if path.startswith("/__test/"):
            return self.control(path, query)
        return json_response(self, 404, {"error": "not_found"})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        form = urllib.parse.parse_qs(body.decode("utf-8"), keep_blank_values=True)
        values = {key: items[-1] for key, items in form.items()}
        if urllib.parse.urlsplit(self.path).path == "/token":
            return self.token(values)
        if urllib.parse.urlsplit(self.path).path == "/register":
            return self.register(values)
        if urllib.parse.urlsplit(self.path).path == "/introspect":
            return self.introspect(values)
        return json_response(self, 404, {"error": "not_found"})

    def authorize(self, query):
        state = self.state
        get = lambda key: query.get(key, [""])[-1]
        client_id, redirect = get("client_id"), get("redirect_uri")
        resource, scope = get("resource"), get("scope")
        challenge = get("code_challenge")
        method = get("code_challenge_method")
        state_value = get("state")
        record = {"client_id": client_id, "redirect_uri": redirect,
                  "resource": resource, "scope": scope,
                  "code_challenge_method": method, "has_state": bool(state_value)}
        with state.lock:
            state.stats["authorize"].append(record)
            if state.config["pkce"] != "1" or method != "S256" or not challenge or \
                    not redirect or not resource:
                return json_response(self, 400, {"error": "invalid_request"})
            if client_id.startswith(("http://", "https://")):
                try:
                    request = urllib.request.Request(client_id, headers={"Accept": "application/json"})
                    with urllib.request.urlopen(request, timeout=4) as response:
                        document = json.loads(response.read(65536))
                    state.stats["cimd_fetches"].append(client_id)
                    if document.get("client_id") != client_id or \
                            not document.get("client_name") or \
                            redirect not in document.get("redirect_uris", []):
                        return json_response(self, 400, {"error": "invalid_client"})
                except Exception:
                    return json_response(self, 400, {"error": "invalid_client"})
            code = secrets.token_urlsafe(24)
            state.codes[code] = (client_id, redirect, resource, scope, challenge)
            params = {"code": code, "state": state_value}
            if state.config["iss_param"] != "absent":
                params["iss"] = state.issuer if state.config["iss_param"] == "good" \
                    else state.issuer + "/wrong"
            if state.config["state"] == "bad":
                params["state"] = "wrong-state"
            location = redirect + ("&" if "?" in redirect else "?") + \
                urllib.parse.urlencode(params)
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def token(self, values):
        state = self.state
        grant = values.get("grant_type", "")
        client_id = values.get("client_id", "")
        client_auth = "none"
        authorization = self.headers.get("Authorization", "")
        if authorization.lower().startswith("basic "):
            client_auth = "client_secret_basic"
            try:
                decoded = base64.b64decode(authorization[6:]).decode()
                client_id = decoded.split(":", 1)[0]
            except Exception:
                pass
        elif values.get("client_secret"):
            client_auth = "client_secret_post"
        record = {"grant_type": grant, "client_id": client_id,
                  "resource": values.get("resource", ""),
                  "has_code_verifier": bool(values.get("code_verifier")),
                  "client_auth": client_auth, "scope": values.get("scope", "")}
        with state.lock:
            state.stats["token"].append(record)
            resource = values.get("resource", "")
            if grant == "authorization_code":
                code = state.codes.pop(values.get("code", ""), None)
                if not code:
                    return json_response(self, 400, {"error": "invalid_grant"})
                expected_client, redirect, expected_resource, scope, challenge = code
                verifier = values.get("code_verifier", "")
                digest = b64url(hashlib.sha256(verifier.encode()).digest())
                if client_id != expected_client or values.get("redirect_uri") != redirect or \
                        digest != challenge or resource != expected_resource:
                    return json_response(self, 400, {"error": "invalid_grant"})
                token = state.token(client_id, resource, scope)
            elif grant == "refresh_token":
                previous = state.refresh.pop(values.get("refresh_token", ""), None)
                if not previous:
                    return json_response(self, 400, {"error": "invalid_grant"})
                client_id, old_resource, scope, subject = previous
                token = state.token(client_id, resource or old_resource, scope, subject)
            elif grant == "client_credentials":
                token = state.token(client_id or "test-client",
                                    resource, values.get("scope", "mcp:use"), "service")
            else:
                return json_response(self, 400, {"error": "unsupported_grant_type"})
        return json_response(self, 200, token, {"Cache-Control": "no-store"})

    def register(self, values):
        client_id = "dcr-" + secrets.token_hex(8)
        with self.state.lock:
            self.state.stats["register"].append({
                "application_type": values.get("application_type", ""),
                "redirect_uris": json.loads(values.get("redirect_uris", "[]")),
            })
        return json_response(self, 201, {"client_id": client_id,
                                         "client_secret": secrets.token_urlsafe(24),
                                         "token_endpoint_auth_method": "none"})

    def introspect(self, values):
        state = self.state
        with state.lock:
            state.stats["introspect_requests"] += 1
            if state.config["introspect_fail"] == "1":
                return json_response(self, 503, {"error": "temporarily_unavailable"})
            token = values.get("token", "")
            if token in state.revoked:
                return json_response(self, 200, {"active": False})
            claims = state.opaque.get(token)
            if claims is None:
                return json_response(self, 200, {"active": False})
            response = dict(claims)
            response["active"] = int(claims["exp"]) > int(time.time())
        return json_response(self, 200, response)

    def control(self, path, query):
        state = self.state
        if path == "/__test/stats":
            with state.lock:
                return json_response(self, 200, state.stats)
        if path == "/__test/reset":
            with state.lock:
                state.stats = state.new_stats()
                state.config = dict(state.DEFAULT_CONFIG)
                state.codes.clear()
                state.refresh.clear()
                state.opaque.clear()
                state.revoked.clear()
            return json_response(self, 200, {"ok": True})
        if path == "/__test/config":
            with state.lock:
                for key, value in query.items():
                    if key in state.config:
                        state.config[key] = value[-1]
                return json_response(self, 200, {"ok": True})
        if path == "/__test/rotate":
            with state.lock:
                state.rotate()
            return json_response(self, 200, {"kid": state.active_kid})
        if path == "/__test/revoke":
            token = query.get("token", [""])[-1]
            with state.lock:
                state.revoked.add(token)
            return json_response(self, 200, {"ok": True})
        if path == "/__test/mint":
            get = lambda key, default="": query.get(key, [default])[-1]
            now = int(time.time())
            claims = {"iss": get("iss", state.issuer),
                      "sub": get("sub", "alice"),
                      "scope": get("scope", "mcp:use"),
                      "client_id": get("client_id", "test-client"),
                      "iat": now, "exp": now + int(get("ttl", "3600")),
                      "jti": secrets.token_hex(12)}
            if not query.get("noaud"):
                claims["aud"] = get("aud", "")
            if query.get("exp_offset"):
                claims["exp"] = now + int(get("exp_offset", "0"))
            if query.get("nbf_offset"):
                claims["nbf"] = now + int(get("nbf_offset", "0"))
            if query.get("scp"):
                claims.pop("scope", None)
                claims["scp"] = [s for s in get("scope", "").split() if s]
            if query.get("opaque"):
                token = secrets.token_urlsafe(24)
                state.opaque[token] = claims
                return json_response(self, 200, {"access_token": token})
            alg = get("alg", "RS256")
            token = state.sign_jwt(
                claims, alg, get("kid") or None, bool(query.get("badsig")),
                public_hmac=(alg == "HS256"))
            return json_response(self, 200, {"access_token": token})
        return json_response(self, 404, {"error": "not_found"})


def main():
    if len(sys.argv) not in (3, 5) or sys.argv[1] != "--port" or \
            (len(sys.argv) == 5 and sys.argv[3] != "--issuer"):
        print("usage: test_authserver --port N [--issuer URL]", file=sys.stderr)
        return 2
    port = int(sys.argv[2])
    issuer = sys.argv[4] if len(sys.argv) == 5 else \
        "http://127.0.0.1:" + str(port)
    state = State(issuer)
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    server.state = state
    try:
        server.serve_forever(poll_interval=0.1)
    finally:
        server.server_close()
        state.temp.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
