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

# What a test page asks the server for, by what the browser lets it run. The page is from one host and takes a script
# and a frame from the other, which is another site.
ALLOW = {
    "inline-ran",
    "own-fetched",
    "own-ran",
    "other-fetched",
    "other-ran",
    "same-site-frame-ran",
    "cross-site-frame-ran",
}
BLOCK_THIRD_PARTY = {
    "inline-ran",
    "own-fetched",
    "own-ran",
    "same-site-frame-ran",
    "cross-site-frame-noscript",
}
BLOCK = {
    "noscript",
    "same-site-frame-noscript",
    "cross-site-frame-noscript",
}


class Server(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.lock = threading.Lock()
        self.seen = {}

    def record(self, run, name):
        with self.lock:
            self.seen.setdefault(run, set()).add(name)

    def seen_in(self, run):
        with self.lock:
            return set(self.seen.get(run, ()))


class Handler(http.server.BaseHTTPRequestHandler):
    def respond(self, content_type, body):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = dict(urllib.parse.parse_qsl(url.query))
        run = query.get("run", "")
        name = query.get("name", "")
        server = self.server
        assert isinstance(server, Server)

        # Everything a page reports is a resource that its load waits for, so the server has seen all of it by the
        # time the page has loaded. A script reports that it ran by writing out an image.
        def report(what):
            return f"document.write('<img src=\"/beacon?run={run}&name={what}\">');"

        if url.path == "/beacon":
            server.record(run, name)
            self.respond("image/gif", BEACON)
        elif url.path == "/script.js":
            server.record(run, f"{name}-fetched")
            self.respond("text/javascript", report(f"{name}-ran").encode())
        elif url.path == "/frame":
            self.respond(
                "text/html",
                (
                    f"<!doctype html><body><script>{report(f'{name}-ran')}</script>"
                    f'<noscript><img src="/beacon?run={run}&name={name}-noscript"></noscript>'
                ).encode(),
            )
        elif url.path == "/page":
            host, port = self.headers["Host"].rsplit(":", 1)
            other = f"http://{'localhost' if host == '127.0.0.1' else '127.0.0.1'}:{port}"
            self.respond(
                "text/html",
                (
                    "<!doctype html><title>JavaScript test</title><body>"
                    f"<script>{report('inline-ran')}</script>"
                    f'<noscript><img src="/beacon?run={run}&name=noscript"></noscript>'
                    f'<script src="/script.js?run={run}&name=own"></script>'
                    f'<script src="{other}/script.js?run={run}&name=other"></script>'
                    f'<iframe src="/frame?run={run}&name=same-site-frame"></iframe>'
                    f'<iframe src="{other}/frame?run={run}&name=cross-site-frame"></iframe>'
                ).encode(),
            )
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

    def check_page(command, host, expected, description):
        run = str(next(runs))
        command("url", {"url": f"http://{host}:{server.server_port}/page?run={run}"})

        # A frame from another site loads in a process of its own, so give what it asks for a moment to arrive, and
        # then a moment more for anything that should not have been asked for.
        deadline = time.monotonic() + 10
        while server.seen_in(run) != expected and time.monotonic() < deadline:
            time.sleep(0.05)
        time.sleep(0.3)
        seen = server.seen_in(run)
        assert seen == expected, f"{description}: expected {sorted(expected)}, got {sorted(seen)}"

    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(
            json.dumps(
                {
                    "firstRunComplete": True,
                    "javascript": {"policy": "block", "siteFilters": [{"filter": "localhost", "policy": "allow"}]},
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
                            && document.querySelector('#site-settings-title').textContent === 'JavaScript'
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
                command("url", {"url": "about:settings#javascript"})
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
                    const select = document.querySelector('#javascript-policy');
                    select.value = arguments[0];
                    select.dispatchEvent(new Event('change'));
                    """,
                    policy,
                )
                wait_for_saved(settings_file, lambda saved: saved["javascript"]["policy"] == policy)

            def wait_for_filters(expected):
                wait_for_saved(
                    settings_file,
                    lambda saved: (
                        sorted((site["filter"], site["policy"]) for site in saved["javascript"]["siteFilters"])
                        == sorted(expected)
                    ),
                )

            # The saved rules apply from the first page on: scripts are blocked, except on the one site allowed them.
            check_page(command, "127.0.0.1", BLOCK, "Blocked by the saved default")
            check_page(command, "localhost", ALLOW, "Allowed by the saved exception")

            # The settings page is the browser's own, and works whatever the rules say.
            open_settings([["localhost", "allow"]])
            assert script("return document.querySelector('#javascript-policy').value;") == "block"
            assert script("""
                return Array.from(document.querySelector('#site-settings-decision').options, option => option.value);
            """) == ["allow", "block-third-party", "block"]
            assert script("return document.querySelector('#site-settings-input').placeholder;") == "example.com"

            for value in ("not a URL", "https://", "about:blank", "data:text/html,test"):
                add_site(value, "allow")
                assert script("return !document.querySelector('#site-settings-input').validity.valid;"), value

            # A site is kept as its host, whatever scheme, port and path it was typed with.
            add_site("  HTTP://127.0.0.1:8443/path?query  ", "block-third-party")
            wait_for_rows([["127.0.0.1", "block-third-party"], ["localhost", "allow"]])
            wait_for_filters([("127.0.0.1", "block-third-party"), ("localhost", "allow")])
            check_page(command, "127.0.0.1", BLOCK_THIRD_PARTY, "Third-party scripts blocked by a new exception")
            check_page(command, "localhost", ALLOW, "Still allowed by the saved exception")

            # An exception can be changed and removed, and the default decides for a site without one.
            open_settings([["127.0.0.1", "block-third-party"], ["localhost", "allow"]])
            script("""
                const select = document.querySelector('[data-filter="localhost"] select');
                select.value = 'block';
                select.dispatchEvent(new Event('change'));
            """)
            wait_for_rows([["127.0.0.1", "block-third-party"], ["localhost", "block"]])
            wait_for_filters([("127.0.0.1", "block-third-party"), ("localhost", "block")])
            script("document.querySelector('[data-filter=\"127.0.0.1\"] button').click();")
            wait_for_rows([["localhost", "block"]])
            wait_for_filters([("localhost", "block")])
            set_default("allow")
            check_page(command, "localhost", BLOCK, "Blocked by a changed exception")
            check_page(command, "127.0.0.1", ALLOW, "Allowed by the changed default")

            open_settings([["localhost", "block"]])
            set_default("block-third-party")
            script("document.querySelector('#site-settings-remove-all').click();")
            wait_for_rows([])
            wait_for_filters([])
            add_site("localhost", "allow")
            wait_for_rows([["localhost", "allow"]])
            wait_for_filters([("localhost", "allow")])
            check_page(command, "127.0.0.1", BLOCK_THIRD_PARTY, "Third-party scripts blocked by the changed default")

        # What was chosen in the settings page is what the next launch starts with.
        saved = json.loads(settings_file.read_text())
        assert saved["javascript"] == {
            "policy": "block-third-party",
            "siteFilters": [{"filter": "localhost", "policy": "allow"}],
        }, saved

        with run_browser(webdriver_binary, directory) as (command, script):
            check_page(command, "127.0.0.1", BLOCK_THIRD_PARTY, "Third-party scripts blocked after a restart")
            check_page(command, "localhost", ALLOW, "Allowed by the exception after a restart")

    server.shutdown()
    print("PASS: JavaScript rules block scripts by site and by party, follow the settings page, and survive a restart")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
