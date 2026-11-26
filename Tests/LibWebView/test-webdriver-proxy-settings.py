#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import os
import runpy
import subprocess
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PROXY_ENVIRONMENT_VARIABLES = (
    "all_proxy",
    "ALL_PROXY",
    "http_proxy",
    "https_proxy",
    "HTTPS_PROXY",
    "no_proxy",
    "NO_PROXY",
)


class Server:
    """An HTTP server that answers every request with a page whose title says which server it is."""

    def __init__(self, title):
        # The pages that were asked for, and everything that was asked for, which includes their icons.
        requests = self.requests = []
        all_requests = self.all_requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                all_requests.append(self.path)
                is_page = not self.path.endswith("/favicon.ico")
                if is_page:
                    requests.append(self.path)
                body = f"<!DOCTYPE html><title>{title}</title>".encode() if is_page else b""
                self.send_response(200 if is_page else 404)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Browser:
    def __init__(self, webdriver_binary, profile, graphical, environment):
        arguments = ["--profile-path", profile]
        if not graphical:
            arguments.append("--headless")
        self.graphical = graphical
        self.process, self.port = helpers["launch_webdriver"](webdriver_binary, *arguments, env=environment)
        self.session = None

    def __enter__(self):
        status, payload, raw = helpers["request"](
            self.port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": not self.graphical}}}
        )
        assert status == 200, raw
        self.session = payload["value"]["sessionId"]
        return self

    def __exit__(self, *exception):
        if self.session is not None:
            helpers["request"](self.port, "DELETE", f"/session/{self.session}")
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

    def command(self, path, body=None, method="POST"):
        status, payload, raw = helpers["request"](self.port, method, f"/session/{self.session}/{path}", body)
        assert status == 200, raw
        return payload["value"]

    def script(self, source, *arguments):
        return self.command("execute/sync", {"script": source, "args": list(arguments)})

    def title_after_navigating_to(self, url):
        # A navigation that fails either reports an error or leaves an error page, and neither has a server's title.
        status, _, _ = helpers["request"](self.port, "POST", f"/session/{self.session}/url", {"url": url})
        if status != 200:
            return None
        return self.command("title", method="GET")

    def open_proxy_settings(self):
        self.command("url", {"url": "about:settings#advanced"})
        self.command(
            "execute/async",
            {
                "script": """
                const done = arguments[0];
                const mode = document.querySelector('#proxy-mode');
                // The page has its settings once it has heard which proxy mode is saved.
                (function poll() {
                    if (mode.dataset.loaded) return done();
                    setTimeout(poll, 10);
                })();
                document.addEventListener('WebUIMessage', event => {
                    if (event.detail.name === 'loadSettings' && Object.hasOwn(event.detail.data, 'proxySettings'))
                        mode.dataset.loaded = 'true';
                });
                ladybird.sendMessage('loadCurrentSettings');
            """,
                "args": [],
            },
        )

    def change_proxy_settings(self, action, *arguments):
        # Resolves once the browser has answered with the proxy settings it saved.
        return self.command(
            "execute/async",
            {
                "script": f"""
                const parameters = Array.from(arguments);
                const done = parameters.pop();
                document.addEventListener('WebUIMessage', function listener(event) {{
                    if (event.detail.name !== 'loadSettings') return;
                    if (!Object.hasOwn(event.detail.data, 'proxySettings')) return;
                    document.removeEventListener('WebUIMessage', listener);
                    done(event.detail.data.proxySettings);
                }});
                {action}
            """,
                "args": list(arguments),
            },
        )

    def proxy_section(self):
        return self.script("""
            const visible = id => !document.querySelector(id).classList.contains('hidden');
            return {
                mode: document.querySelector('#proxy-mode').value,
                type: document.querySelector('#proxy-type').value,
                remoteDns: document.querySelector('#proxy-remote-dns').checked,
                host: document.querySelector('#proxy-host').value,
                port: document.querySelector('#proxy-port').value,
                bypass: document.querySelector('#proxy-bypass').value,
                manualVisible: visible('#manual-proxy-settings'),
                remoteDnsVisible: visible('#proxy-remote-dns-container'),
                forced: visible('#proxy-forcibly-enabled') ? document.querySelector('#proxy-forcibly-enabled').textContent : null,
                error: visible('#proxy-error') ? document.querySelector('#proxy-error').textContent : null,
                disabled: [...document.querySelectorAll('#proxy-mode, #proxy-type, #proxy-host, #proxy-port, #proxy-bypass')]
                    .every(control => control.disabled),
            };
        """)

    def set_field(self, selector, value):
        return self.change_proxy_settings(
            """
            const field = document.querySelector(parameters[0]);
            field.value = parameters[1];
            field.dispatchEvent(new Event('change'));
        """,
            selector,
            value,
        )


def environment_with(**variables):
    environment = {name: value for name, value in os.environ.items() if name not in PROXY_ENVIRONMENT_VARIABLES}
    environment.update(variables)
    return environment


def write_settings(profile, proxy_settings):
    config = Path(profile) / "config"
    config.mkdir(exist_ok=True)
    settings_file = config / "Settings.json"
    settings_file.write_text(json.dumps({"proxySettings": proxy_settings}))
    return settings_file


def test_saved_proxy(webdriver_binary, graphical, proxy, origin):
    target = f"http://127.0.0.1:{origin.port}"

    with tempfile.TemporaryDirectory() as profile:
        settings_file = write_settings(
            profile, {"mode": "manual", "type": "http", "host": "127.0.0.1", "port": proxy.port, "bypass": []}
        )
        with Browser(webdriver_binary, profile, graphical, environment_with()) as browser:
            # The saved proxy is in use from the first request on.
            assert browser.title_after_navigating_to(f"{target}/saved") == "proxy"
            assert proxy.requests == [f"{target}/saved"], proxy.requests
            assert origin.all_requests == [], origin.all_requests

            browser.open_proxy_settings()
            section = browser.proxy_section()
            assert section["mode"] == "manual" and section["manualVisible"], section
            assert (section["type"], section["host"], section["port"]) == ("http", "127.0.0.1", str(proxy.port)), (
                section
            )
            assert not section["remoteDnsVisible"] and not section["disabled"], section
            assert section["forced"] is None and section["error"] is None, section

            # A proxy that nothing listens on fails the page: the request does not fall back to the origin.
            closed_port = helpers["unused_port"]()
            saved = browser.set_field("#proxy-port", str(closed_port))
            assert saved["port"] == closed_port and "error" not in saved, saved
            assert json.loads(settings_file.read_text())["proxySettings"]["port"] == closed_port
            assert browser.title_after_navigating_to(f"{target}/unreachable") not in ("proxy", "origin")
            assert origin.all_requests == [], origin.all_requests

            # A proxy that cannot be used is kept, reported, and fails every page.
            browser.open_proxy_settings()
            saved = browser.set_field("#proxy-port", "0")
            assert saved["mode"] == "manual" and saved["port"] == 0 and "error" in saved, saved
            section = browser.proxy_section()
            assert section["error"] is not None and "between 1 and 65535" in section["error"], section
            assert "Nothing is loaded" in section["error"], section
            assert browser.title_after_navigating_to(f"{target}/invalid") not in ("proxy", "origin")
            assert origin.all_requests == [], origin.all_requests

            # Correcting it puts requests back on the proxy, with the bypass list in effect.
            browser.open_proxy_settings()
            browser.set_field("#proxy-port", str(proxy.port))
            saved = browser.set_field("#proxy-bypass", "localhost, .internal.example")
            assert saved["bypass"] == ["localhost", ".internal.example"] and "error" not in saved, saved
            assert browser.proxy_section()["error"] is None
            assert browser.title_after_navigating_to(f"{target}/corrected") == "proxy"
            assert browser.title_after_navigating_to(f"http://localhost:{origin.port}/bypassed") == "origin"
            assert proxy.requests == [f"{target}/saved", f"{target}/corrected"], proxy.requests
            assert origin.requests == ["/bypassed"], origin.requests

            # SOCKS5 offers to resolve host names through the proxy, which is the type socks5h.
            browser.open_proxy_settings()
            saved = browser.set_field("#proxy-type", "socks5")
            assert saved["type"] == "socks5h", saved
            section = browser.proxy_section()
            assert section["remoteDnsVisible"] and section["remoteDns"], section
            saved = browser.change_proxy_settings("document.querySelector('#proxy-remote-dns').click();")
            assert saved["type"] == "socks5", saved

            # Without a proxy, requests reach the origin again.
            saved = browser.set_field("#proxy-mode", "none")
            assert saved["mode"] == "none", saved
            assert not browser.proxy_section()["manualVisible"]
            assert browser.title_after_navigating_to(f"{target}/direct") == "origin"
            assert origin.requests == ["/bypassed", "/direct"], origin.requests

    print("PASS: a saved proxy is used, fails closed, and follows changes made in about:settings")


def test_environment_wins(webdriver_binary, graphical, proxy, origin):
    target = f"http://127.0.0.1:{origin.port}"
    for server in (proxy, origin):
        server.requests.clear()
        server.all_requests.clear()

    with tempfile.TemporaryDirectory() as profile:
        write_settings(profile, {"mode": "none", "type": "", "host": "", "port": 0, "bypass": []})
        environment = environment_with(all_proxy=f"http://127.0.0.1:{proxy.port}")
        with Browser(webdriver_binary, profile, graphical, environment) as browser:
            assert browser.title_after_navigating_to(f"{target}/environment") == "proxy"
            assert origin.all_requests == [], origin.all_requests

            browser.open_proxy_settings()
            section = browser.proxy_section()
            assert section["mode"] == "none" and section["disabled"], section
            assert section["forced"] == "This setting is controlled via the proxy environment variables", section

            # Even a page that sends the message itself cannot turn the environment's proxy off.
            browser.change_proxy_settings(
                "ladybird.sendMessage('setProxySettings', { mode: 'manual', type: 'http', host: '', port: 0 });"
            )
            assert browser.title_after_navigating_to(f"{target}/still-environment") == "proxy"
            assert origin.all_requests == [], origin.all_requests
            assert proxy.requests == [f"{target}/environment", f"{target}/still-environment"], proxy.requests

    print("PASS: a proxy from the environment wins over saved proxy settings")


def run_test(webdriver_binary, graphical):
    proxy = Server("proxy")
    origin = Server("origin")
    try:
        test_saved_proxy(webdriver_binary, graphical, proxy, origin)
        test_environment_wins(webdriver_binary, graphical, proxy, origin)
    finally:
        proxy.close()
        origin.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    run_test(args.webdriver_binary, args.graphical)
