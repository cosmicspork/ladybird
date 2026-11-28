#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import subprocess
import tempfile
import threading
import time
import urllib.parse

from contextlib import contextmanager
from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

# A 1x1 transparent GIF.
BEACON = bytes.fromhex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b")


class Server(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.lock = threading.Lock()
        self.cookies = {}

    def record(self, run, cookies):
        with self.lock:
            self.cookies[run] = cookies

    def cookies_of(self, run):
        with self.lock:
            return self.cookies.get(run)


class Handler(http.server.BaseHTTPRequestHandler):
    def respond(self, content_type, body, headers=()):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        run = dict(urllib.parse.parse_qsl(url.query)).get("run", "")
        server = self.server
        assert isinstance(server, Server)

        # A page sets a cookie that names its run, and then asks for an image. The cookies that request carries are
        # the ones the browser lets the page's site keep.
        if url.path == "/page":
            self.respond(
                "text/html",
                f'<!doctype html><title>Cookie test</title><body><img src="/beacon?run={run}">'.encode(),
                [("Set-Cookie", f"run={run}; Path=/")],
            )
        elif url.path == "/beacon":
            cookies = {}
            for cookie in self.headers.get("Cookie", "").split("; "):
                name, separator, value = cookie.partition("=")
                if separator:
                    cookies[name] = value
            server.record(run, cookies)
            self.respond("image/gif", BEACON)
        else:
            self.send_error(404)

    def log_message(self, format, *args):
        pass


@contextmanager
def run_browser(webdriver_binary, directory):
    process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", str(directory))
    session = None
    try:
        session = helpers["create_session"](port)

        def command(path, body=None, method="POST"):
            status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
            assert status == 200, raw
            return payload["value"]

        def script(source, *args):
            return command("execute/sync", {"script": source, "args": list(args)})

        yield command, script
    finally:
        try:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def wait_for_saved(path, condition):
    deadline = time.monotonic() + 5
    saved = None
    while True:
        # The browser rewrites the file in place, so a read may overlap a write and see part of it.
        try:
            saved = json.loads(path.read_text())
        except (OSError, ValueError):
            pass
        else:
            if condition(saved):
                return saved
        if time.monotonic() >= deadline:
            raise AssertionError(f"{path.name} never matched: {saved}")
        time.sleep(0.1)


def run_test(webdriver_binary):
    server = Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    runs = iter(range(1, 1000))

    def check_page(command, script, host, keeps_cookies, description):
        run = str(next(runs))
        command("url", {"url": f"http://{host}:{server.server_port}/page?run={run}"})

        deadline = time.monotonic() + 10
        while server.cookies_of(run) is None and time.monotonic() < deadline:
            time.sleep(0.05)
        sent = server.cookies_of(run)
        assert sent is not None, f"{description}: the page never asked for its image"

        seen = script("return document.cookie;")
        if keeps_cookies:
            assert sent == {"run": run}, f"{description}: expected the cookie of run {run} to be sent, got {sent}"
            assert seen == f"run={run}", f"{description}: expected script to see the cookie of run {run}, got {seen!r}"
        else:
            # NB: Not even a cookie the site kept while it was allowed to.
            assert sent == {}, f"{description}: expected no cookie to be sent, got {sent}"
            assert seen == "", f"{description}: expected script to see no cookie, got {seen!r}"

    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(
            json.dumps(
                {
                    "firstRunComplete": True,
                    "siteData": {"policy": "block", "siteFilters": [{"filter": "localhost", "policy": "allow"}]},
                }
            )
        )

        with run_browser(webdriver_binary, directory) as (command, script):

            def wait_for_rows(expected):
                return command(
                    "execute/async",
                    {
                        "script": """
                        const [expected, done] = arguments;
                        const dialog = document.querySelector('#site-settings');
                        const read = () => Array.from(dialog.querySelectorAll('.dialog-list-item'))
                            .map(row => [row.dataset.filter, row.querySelector('select')?.value ?? null]).sort();
                        const ready = () => dialog.open
                            && document.querySelector('#site-settings-title').textContent === 'Cookies'
                            && JSON.stringify(read()) === JSON.stringify(expected);
                        if (ready()) return done(read());
                        const observer = new MutationObserver(() => {
                            if (!ready()) return;
                            observer.disconnect();
                            done(read());
                        });
                        observer.observe(dialog, { childList: true, subtree: true, attributes: true });
                    """,
                        "args": [sorted(expected)],
                    },
                )

            def open_settings(expected):
                command("url", {"url": "about:settings#cookies"})
                return wait_for_rows(expected)

            def add_site(site, policy):
                script(
                    """
                    const input = document.querySelector('#site-settings-input');
                    input.value = arguments[0];
                    input.dispatchEvent(new Event('input'));
                    document.querySelector('#site-settings-decision').value = arguments[1];
                    document.querySelector('#site-settings-add').click();
                    """,
                    site,
                    policy,
                )

            def set_default(policy):
                script(
                    """
                    const select = document.querySelector('#cookies-policy');
                    select.value = arguments[0];
                    select.dispatchEvent(new Event('change'));
                    """,
                    policy,
                )
                wait_for_saved(settings_file, lambda saved: saved["siteData"]["policy"] == policy)

            def wait_for_filters(expected):
                wait_for_saved(
                    settings_file,
                    lambda saved: (
                        sorted((site["filter"], site["policy"]) for site in saved["siteData"]["siteFilters"])
                        == sorted(expected)
                    ),
                )

            # The saved rules apply from the first page on: cookies are blocked, except on the one site allowed them.
            check_page(command, script, "127.0.0.1", False, "Blocked by the saved default")
            check_page(command, script, "localhost", True, "Allowed by the saved exception")

            open_settings([["localhost", "allow"]])
            assert script("return document.querySelector('#cookies-policy').value;") == "block"
            assert script("""
                return Array.from(document.querySelector('#site-settings-decision').options, option => option.value);
            """) == ["allow", "block-third-party", "block"]
            assert script("return document.querySelector('#site-settings-input').placeholder;") == "example.com"

            for value in ("not a URL", "https://", "about:blank", "data:text/html,test"):
                add_site(value, "allow")
                assert script("return !document.querySelector('#site-settings-input').validity.valid;"), value

            # A site is kept as its host, whatever scheme, port and path it was typed with.
            add_site("  HTTP://127.0.0.1:8443/path?query  ", "allow")
            wait_for_rows([["127.0.0.1", "allow"], ["localhost", "allow"]])
            wait_for_filters([("127.0.0.1", "allow"), ("localhost", "allow")])
            check_page(command, script, "127.0.0.1", True, "Allowed by a new exception")

            # An exception can be changed and removed, and the default decides for a site without one. A site that
            # may no longer keep cookies does not get the ones it kept before.
            open_settings([["127.0.0.1", "allow"], ["localhost", "allow"]])
            script("""
                const select = document.querySelector('[data-filter="localhost"] select');
                select.value = 'block';
                select.dispatchEvent(new Event('change'));
            """)
            wait_for_rows([["127.0.0.1", "allow"], ["localhost", "block"]])
            wait_for_filters([("127.0.0.1", "allow"), ("localhost", "block")])
            script("document.querySelector('[data-filter=\"127.0.0.1\"] button').click();")
            wait_for_rows([["localhost", "block"]])
            wait_for_filters([("localhost", "block")])
            check_page(command, script, "localhost", False, "Blocked by a changed exception")
            check_page(command, script, "127.0.0.1", False, "Blocked by the default once its exception is removed")

            open_settings([["localhost", "block"]])
            set_default("block-third-party")
            check_page(command, script, "127.0.0.1", True, "A first party under the changed default")
            check_page(command, script, "localhost", False, "Still blocked by its exception")

            open_settings([["localhost", "block"]])
            script("document.querySelector('#site-settings-remove-all').click();")
            wait_for_rows([])
            wait_for_filters([])
            check_page(command, script, "localhost", True, "A first party once every exception is removed")

            open_settings([])
            set_default("block")
            add_site("localhost", "allow")
            wait_for_rows([["localhost", "allow"]])
            wait_for_filters([("localhost", "allow")])

        # What was chosen in the settings page is what the next launch starts with.
        saved = json.loads(settings_file.read_text())
        assert saved["siteData"] == {
            "policy": "block",
            "siteFilters": [{"filter": "localhost", "policy": "allow"}],
        }, saved

        with run_browser(webdriver_binary, directory) as (command, script):
            check_page(command, script, "127.0.0.1", False, "Blocked after a restart")
            check_page(command, script, "localhost", True, "Allowed by the exception after a restart")

    server.shutdown()
    print("PASS: Cookie rules decide by site, follow the settings page, and survive a restart")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
