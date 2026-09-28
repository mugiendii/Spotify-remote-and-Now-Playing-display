#!/usr/bin/env python3
"""
Obtain a Spotify refresh token for the ESP32 display, using the
Authorization Code + PKCE flow.

Why PKCE: the refresh leg of a PKCE grant needs only the client ID, so the
firmware never has to hold a client secret. A secret compiled into an ESP32
image is a published secret - the flash is readable over UART with a USB
cable and no special equipment.

What this does:
  1. Generates a PKCE verifier/challenge pair.
  2. Opens your browser at Spotify's authorisation page.
  3. Serves a throwaway HTTP listener on 127.0.0.1 to catch the redirect.
  4. Exchanges the authorisation code for tokens.
  5. Prints the refresh token for pasting into include/secrets.h.

Requirements: Python 3.8+. Standard library only - nothing to install.

Usage:
    python3 tools/get_refresh_token.py --client-id YOUR_CLIENT_ID

Before running, add this EXACT redirect URI to your app in the Spotify
Developer Dashboard (Settings -> Redirect URIs):

    http://127.0.0.1:8888/callback

Use 127.0.0.1, not "localhost": Spotify stopped accepting the hostname form
for new redirect URIs and will reject the authorisation request with
INVALID_CLIENT if you use it.
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import socket
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"

# Exactly the scopes the firmware needs, and no more. Read scopes cover the
# now-playing display; modify covers the transport controls.
SCOPES = [
    "user-read-currently-playing",
    "user-read-playback-state",
    "user-modify-playback-state",
]

DEFAULT_PORT = 8888
REDIRECT_PATH = "/callback"


def pkce_pair():
    """Return (verifier, challenge) per RFC 7636 using the S256 method."""
    verifier = base64.urlsafe_b64encode(os.urandom(64)).decode().rstrip("=")
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    challenge = base64.urlsafe_b64encode(digest).decode().rstrip("=")
    return verifier, challenge


class CallbackHandler(http.server.BaseHTTPRequestHandler):
    """Single-shot handler that captures ?code= (or ?error=) and stops."""

    result = {}
    done = threading.Event()

    def do_GET(self):  # noqa: N802  (name fixed by BaseHTTPRequestHandler)
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != REDIRECT_PATH:
            self.send_error(404)
            return

        params = urllib.parse.parse_qs(parsed.query)
        CallbackHandler.result = {k: v[0] for k, v in params.items()}

        ok = "code" in CallbackHandler.result
        body = (
            "<html><body style='font-family:system-ui;background:#111;"
            "color:#eee;text-align:center;padding-top:16vh'>"
            f"<h2 style='color:{'#2ed15c' if ok else '#f56'}'>"
            f"{'Authorised' if ok else 'Authorisation failed'}</h2>"
            "<p>You can close this tab and return to the terminal.</p>"
            "</body></html>"
        ).encode()

        self.send_response(200 if ok else 400)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        CallbackHandler.done.set()

    def log_message(self, *_args):
        pass  # keep the terminal clean


def post_form(url, fields):
    """POST an application/x-www-form-urlencoded body, return parsed JSON."""
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode(errors="replace")
        print(f"\nERROR: token endpoint returned HTTP {exc.code}", file=sys.stderr)
        print(detail, file=sys.stderr)
        if "invalid_client" in detail:
            print(
                "\nThe client ID is wrong, or the redirect URI registered in "
                "the dashboard does not match this one EXACTLY.",
                file=sys.stderr,
            )
        sys.exit(1)


def port_is_free(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def main():
    ap = argparse.ArgumentParser(
        description="Fetch a Spotify refresh token for the ESP32 display.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--client-id", required=True,
                    help="Client ID from the Spotify Developer Dashboard")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"local callback port (default {DEFAULT_PORT})")
    ap.add_argument("--no-browser", action="store_true",
                    help="print the URL instead of opening a browser")
    args = ap.parse_args()

    if not port_is_free(args.port):
        print(f"ERROR: port {args.port} is already in use. Pass --port N and "
              f"register http://127.0.0.1:N{REDIRECT_PATH} in the dashboard.",
              file=sys.stderr)
        sys.exit(1)

    redirect_uri = f"http://127.0.0.1:{args.port}{REDIRECT_PATH}"
    verifier, challenge = pkce_pair()
    state = secrets.token_urlsafe(16)

    auth_url = AUTH_URL + "?" + urllib.parse.urlencode({
        "client_id": args.client_id,
        "response_type": "code",
        "redirect_uri": redirect_uri,
        "state": state,
        "scope": " ".join(SCOPES),
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        # Force the consent screen so re-running this always issues a fresh
        # refresh token rather than silently reusing a cached grant.
        "show_dialog": "true",
    })

    print("Spotify refresh-token setup")
    print("=" * 60)
    print(f"Redirect URI : {redirect_uri}")
    print("              (this must be registered in your app's dashboard)")
    print(f"Scopes       : {', '.join(SCOPES)}")
    print()

    server = http.server.HTTPServer(("127.0.0.1", args.port), CallbackHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    if args.no_browser:
        print("Open this URL in your browser:\n")
        print(auth_url, "\n")
    else:
        print("Opening your browser to authorise...")
        print("(if nothing happens, open this URL manually:)\n")
        print(auth_url, "\n")
        webbrowser.open(auth_url)

    print("Waiting for the redirect (Ctrl-C to abort)...")
    try:
        if not CallbackHandler.done.wait(timeout=300):
            print("\nERROR: timed out after 5 minutes.", file=sys.stderr)
            sys.exit(1)
    except KeyboardInterrupt:
        print("\nAborted.", file=sys.stderr)
        sys.exit(1)
    finally:
        server.shutdown()

    result = CallbackHandler.result
    if "error" in result:
        print(f"\nERROR: Spotify returned '{result['error']}'.", file=sys.stderr)
        sys.exit(1)

    # The state check is what stops a malicious page in another tab from
    # feeding us its own authorisation code.
    if result.get("state") != state:
        print("\nERROR: state mismatch - discarding this response.", file=sys.stderr)
        sys.exit(1)

    print("Authorisation code received, exchanging for tokens...")
    tokens = post_form(TOKEN_URL, {
        "client_id": args.client_id,
        "grant_type": "authorization_code",
        "code": result["code"],
        "redirect_uri": redirect_uri,
        "code_verifier": verifier,
    })

    refresh = tokens.get("refresh_token")
    if not refresh:
        print("\nERROR: no refresh_token in the response:", file=sys.stderr)
        print(json.dumps(tokens, indent=2), file=sys.stderr)
        sys.exit(1)

    print()
    print("=" * 60)
    print("SUCCESS - paste these into include/secrets.h")
    print("=" * 60)
    print()
    print(f'#define SPOTIFY_CLIENT_ID      "{args.client_id}"')
    print(f'#define SPOTIFY_REFRESH_TOKEN  "{refresh}"')
    print()
    print("=" * 60)
    print("Keep this token private: it grants ongoing access to your")
    print("playback. include/secrets.h is gitignored - keep it that way.")
    print("Revoke it any time at https://www.spotify.com/account/apps/")
    print()
    print("Note: the device rotates this token as Spotify issues new ones")
    print("and stores the current value in its own NVS, so the value above")
    print("is only the first-boot seed.")


if __name__ == "__main__":
    main()
