# Proxy settings and fail-closed proxying

Status: design, awaiting review. Tracked in the fork's issue #3.

## Problem

Ladybird can send all of its traffic through an HTTP, HTTPS, SOCKS4, SOCKS4a, SOCKS5 or SOCKS5-with-remote-DNS proxy,
but the proxy can only be chosen through the `all_proxy`, `http_proxy`, `https_proxy` and `no_proxy` environment
variables. A user who launches the browser from a desktop launcher has no way to set one, and has no way to check which
proxy, if any, is in effect.

A privacy proxy is only useful if nothing goes around it. The browser must never fall back to a direct connection when
the proxy is unreachable, and must never leak a connection or a DNS lookup while the configuration changes at runtime.

## What upstream already does

Checked against upstream `b1e0374` (2026-11-25).

- `HTTP::ProxyConfiguration::from_environment()` reads the environment with libcurl's precedence, and
  `proxy_for(url)` applies `no_proxy`. Both are C++ in `Libraries/LibHTTP/Proxy.cpp`, with tests in
  `Tests/LibHTTP/TestProxy.cpp`.
- RequestServer receives the configuration over the existing `set_proxy_configuration` IPC message and applies it to
  page requests (`Request.cpp`), WebSocket connections and AIA certificate fetches (`ConnectionFromClient.cpp`).
- The proxy's own host name is resolved once and pinned (`resolve_proxy_host()` in `Resolver.cpp`). A failed lookup
  fails the request with `UnableToResolveProxy`.
- With any proxy configured, RequestServer answers every `resolve_host` request with no addresses, so DNS prefetching
  and speculative connections cannot reach the network. Alternative services (HTTP/3) are not used either.
- libcurl does not fall back to a direct connection when `CURLOPT_PROXY` is set and the proxy refuses the connection,
  so an unreachable fixed proxy already fails the request. The failure is reported as the generic `UnableToConnect`.
- `--proxy-autoconfig-url` exists, but PAC files are not evaluated: every request in WebContent fails with "PAC proxy
  configurations are not supported" (`ResourceLoader.cpp`).
- WebRTCClient is launched with `--disable-network` when a proxy or PAC URL is configured (`HelperProcess.cpp`).
- The DNS section of `about:settings` already shows a note when a proxy resolves host names, and already shows DNS
  settings forced from the command line as read-only.

So the static case is already fail-closed. What is missing is a way to set the proxy, and guarantees for the dynamic
case, where the configuration changes while the browser runs.

## Gaps

1. **No settings UI or persistence.** The proxy can only come from the environment.
2. **Runtime changes.** Applying a new configuration over IPC does not touch connections that libcurl already pooled
   while there was no proxy, requests in flight, or WebRTCClient processes launched with network access. Once proxy
   settings can change at runtime, each of these is a direct connection that outlives the switch.
3. **Invalid configuration.** If saved settings name a proxy that cannot be parsed, the safe outcome is to fail every
   request, not to drop the proxy and connect directly.
4. **Error reporting.** A refused proxy connection is indistinguishable from an unreachable site on the error page.

## Prior art

- **Firefox** (and LibreWolf, which inherits it): `network.proxy.type` selects none, system, manual, PAC or WPAD.
  `network.proxy.socks_remote_dns` sends host names to a SOCKS proxy. `network.proxy.failover_direct` controls
  whether a connection falls back to direct when every proxy fails.
- **Tor Browser**: always proxies through its own tor process and resolves host names through it.
- **Mullvad Browser**: ships no proxy and relies on a system VPN; the Firefox proxy settings remain available.
- **Brave and Chromium**: a fixed proxy never falls back to direct. A PAC result can include `DIRECT`. On Linux the
  proxy comes from the desktop environment or command-line flags; there is no proxy UI of their own.

The common rule is that a manually configured proxy fails closed, which matches what upstream does today. This design
keeps that rule and does not add a failover option.

## Design

### Modes

| Mode | Meaning |
|------|---------|
| `system` | Today's behavior: the environment variables decide. This is the default. |
| `none` | No proxy, even if the environment names one. |
| `manual` | One proxy for HTTP, HTTPS and WebSocket traffic, plus a bypass list. |

PAC is left out of the UI. Upstream cannot evaluate PAC files, so a PAC mode would only make every request fail. It can
be added once upstream evaluates PAC, and `--proxy-autoconfig-url` keeps working as it does today.

### Precedence

As with DNS settings, the command line and environment win over saved settings, and the UI shows the section as forced
and read-only when they apply. In `system` mode the environment is the configuration, so nothing is forced.
"Environment variables win" applies to the `manual` and `none` modes too, so that a launcher which sets `all_proxy`
cannot be silently overridden by a stale saved setting.

### Settings schema

Stored in `Settings.json` next to `dnsSettings`:

```json
"proxySettings": {
    "mode": "manual",
    "type": "socks5h",
    "host": "127.0.0.1",
    "port": 9050,
    "bypass": ["localhost", "127.0.0.1", ".internal.example"]
}
```

- `type` is one of `http`, `https`, `socks4`, `socks4a`, `socks5` and `socks5h`, mapping onto `HTTP::Proxy::Type`.
  Remote DNS is not a separate switch: it is the difference between `socks5` and `socks5h`, which is how libcurl and
  the environment variables already express it. The UI presents it as a "Resolve host names through the proxy"
  checkbox shown for SOCKS5.
- `bypass` uses the `no_proxy` syntax so that `proxy_for()` applies it unchanged.
- Credentials are not stored in this version (see open questions).
- A missing or unparseable object means `system`. A `manual` object that fails validation is kept, reported in the UI,
  and turned into a configuration that fails every request.

### Process and IPC flow

```
about:settings (JS) --WebUI--> UI process: Settings::set_proxy_settings()
                                 |  persists Settings.json
                                 |  notifies SettingsObserver::proxy_settings_changed()
                                 v
                        Application: resolve effective configuration
                                 |  (environment / flags > saved settings)
                                 +--> RequestServer: set_proxy_configuration (existing message)
                                 |        drops pooled connections and fails in-flight requests
                                 |        started under the previous configuration
                                 +--> relaunch WebRTCClient helpers with or without --disable-network
```

- No new IPC message is needed for the configuration itself. RequestServer's handler gains the step that discards
  connections opened under the previous configuration (closing the libcurl multi handle's connection cache, and
  cancelling requests whose configuration snapshot differs).
- WebContent needs no change: it never connects on its own, and the PAC URL is unaffected.
- `HelperProcess.cpp` already terminates and relaunches WebRTCClient helpers per WebContent. The change applies the
  same relaunch when the effective configuration flips between empty and non-empty.

### Error page

`Requests::NetworkError` gains `ProxyConnectionFailed`, mapped from libcurl's proxy errors (`CURLE_COULDNT_CONNECT`
when a proxy was in use, and `CURLE_PROXY` with `CURLINFO_PROXY_ERROR`). The network error page shows "The
proxy server refused the connection" with the proxy's address, and links to the proxy settings.

### Rust and C++ split

The plan for this fork puts decision logic in Rust, and also says not to rewrite upstream C++. For this feature those
two rules pull in different directions, because the parsing and matching that exist are already C++:

- **Rust, new crate `Libraries/LibWebView/ProxySettings/Rust`:** validation of the `proxySettings` JSON (type, host,
  port range, bypass entries), the precedence rule (flags, environment, saved settings), and the fail-closed decision
  that turns an invalid manual configuration into "fail every request". It exposes one function over cbindgen that
  takes the saved JSON and the environment and returns either a configuration or a validation error. This is the only
  new code that handles user input.
- **C++, unchanged:** `Proxy::parse()`, `ProxyConfiguration::from_environment()` and `proxy_for()`. Duplicating the
  bypass matcher in Rust would add a second implementation to keep in sync.
- **C++ glue:** `Settings.{h,cpp}` persistence and observer, the `Application` hook that pushes the configuration,
  RequestServer connection draining, the error mapping and the WebRTC relaunch.
- **HTML/JS:** a Proxy section in `settings.html` and a new `settings/proxy.js`, following `network.js`.

The crate is small. If a maintainer would rather keep this in C++ next to `Proxy.cpp`, the C++ version is a direct port;
see the open questions.

## Touched upstream files

Hooks into existing files, each expected to be a few lines:

- `Libraries/LibWebView/Settings.{h,cpp}`: the setting, its JSON and the observer call.
- `Libraries/LibWebView/Application.cpp`: compute and push the effective configuration.
- `Libraries/LibWebView/HelperProcess.cpp`: relaunch WebRTCClient on a change.
- `Services/RequestServer/ControlConnectionFromClient.cpp`, `Resolver.cpp`: drain connections on a change.
- `Services/RequestServer/CURL.cpp`, `Libraries/LibRequests/NetworkError.h`: the new error.
- `Base/res/ladybird/about-pages/settings.html`: the section markup.
- `Libraries/LibWebView/CMakeLists.txt`, root `Cargo.toml`: the crate.

New files: the crate, `settings/proxy.js`, and the tests below.

## Commit series

1. `LibWebView: Add a Rust crate that validates proxy settings`
2. `LibWebView: Persist proxy settings`
3. `RequestServer: Drop connections made under a previous proxy configuration`
4. `LibWebView: Relaunch WebRTC helpers when proxy settings change`
5. `LibRequests+RequestServer: Report proxy connection failures separately`
6. `LibWebView: Apply saved proxy settings to RequestServer`
7. `Base: Add proxy settings to about:settings`

Each commit leaves the default (`system`) behavior identical to upstream.

## Test plan

- **Rust unit tests** (`cargo test`, run by the `RustWorkspace` ctest): every type, host forms (names, IPv4, bracketed
  IPv6, zone IDs), port bounds, bypass syntax, precedence of flags and environment over saved settings, and that every
  invalid manual configuration yields "fail every request".
- **LibWebView** (`Tests/LibWebView/TestProxySettings.cpp`): JSON round trip, defaults, and that an unknown `mode`
  reads as `system`.
- **RequestServer** (`Tests/RequestServer/TestProxyFailClosed.cpp`): a local origin server that counts its
  connections and a local HTTP and SOCKS5 proxy, both in-process. Each case asserts that the origin sees no connection
  that did not come through the proxy:
  - proxy refusing connections, for HTTP requests, WebSocket connections and AIA fetches
  - proxy stopped after the first request, with the connection pool warm
  - proxy enabled after a direct connection was pooled
  - `resolve_host` returning nothing while a proxy is set
  - invalid manual configuration failing every request
- **WebDriver end-to-end** (`Tests/LibWebView/test-webdriver-proxy-settings.py`, following
  `test-webdriver-settings-changes.py`): launch with a `Settings.json` that names a local proxy, load a page, and
  assert that the proxy saw the request and the origin saw only the proxy. Then change the setting through
  `about:settings` and repeat. A second case sets `all_proxy` and checks that the UI shows the section as forced.
- **WebRTC**: a LibWebView test that the helper's arguments include `--disable-network` after a runtime change.

## Open questions

1. **Rust or C++ for validation.** The crate is about 200 lines. Is a separate crate worth its build and FFI cost for
   that, given the related code is C++?
2. **Credentials.** `HTTP::Proxy` supports `user:password@`. Storing a password in plain `Settings.json` is a poor
   default. Leave credentials out, or wait for a secret store?
3. **Separate proxies per scheme.** `http_proxy` and `https_proxy` can differ. The UI offers one proxy for all
   traffic. Is that enough?
4. **Container tabs (feature 4).** A per-session proxy would make the configuration part of `BrowsingSession` rather
   than global. This design keeps it global and leaves that change to feature 4.
