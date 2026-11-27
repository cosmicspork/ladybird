# Per-site JavaScript control

Status: implemented with this note, awaiting review. Tracked in the fork's issue #4.

## Problem

The only scripting control is the "Enable Scripting" switch in the Debug menu, which turns scripting off for one tab
until it is closed. A user cannot say that a site may not run scripts, cannot allow scripts on a few trusted sites
only, and cannot block third-party scripts while leaving a site's own scripts alone.

## What upstream already does

Checked against upstream `b1e0374` (2026-11-25).

- `HTML::is_scripting_enabled()` (`Libraries/LibWeb/HTML/Scripting/Environments.cpp`) is the single place that says
  whether scripts run for a document. The parser, `<script>` preparation, event handler attributes, timers, module
  loading and `<noscript>` rendering all ask it. It already answers no when the page's scripting switch is off, and it
  already exempts the browser's own WebUI pages.
- The autoplay setting is the closest feature: a default plus a list of per-site exceptions in `WebView::Settings`
  (`SiteSetting<Policy>`), sent to every WebContent process with `set_autoplay_settings`, held there by the
  `HTML::AutoplaySettings` singleton, edited in `about:settings` through the shared site-settings dialog, and
  overridden per site from the site-settings popover of the location bar.
- `PermissionManager` stores per-origin decisions for the permissions a page asks for (notifications, storage
  access). It has no default policy, no host patterns and no settings page of its own.

## Prior art

- **Chromium and Brave**: a JavaScript content setting with a default and exceptions written as host patterns such as
  `[*.]example.com`. The most specific pattern wins, and the pattern is matched against the top-level site. Brave
  Shields adds a per-site "block scripts" switch in the toolbar that reloads the page.
- **Firefox** has only the global `javascript.enabled` preference. Per-site control comes from extensions.
- **NoScript and uBlock Origin**: per-site rules decided by the top-level site, with a separate rule for third-party
  scripts. uBlock Origin's "block 3rd-party scripts" compares registrable domains.

This design follows the content-setting model and adds the third-party rule as a third policy value.

## Design

### Policies

There is one three-valued policy, used both as the default for every site and as the value of an exception:

- `allow`: every script runs.
- `block-third-party`: scripts from the site itself run. Scripts fetched from another site do not, and neither do
  scripts in frames whose document is from another site.
- `block`: no script runs.

The default is `allow`, which is upstream's behavior. The fork's own default belongs to the `Fork defaults` series.

### Sites

An exception is for a host pattern: a host name, which also covers every host under it, or an IP address, which only
covers itself. `example.com` covers `www.example.com` but not `badexample.com`. A pattern has no scheme and no port,
so an exception holds for `http:` and `https:` alike. Text typed by the user may carry a scheme, a port, a path, a
leading `*.` or a trailing dot. All of them are dropped. Host names must be ASCII (IDNA names are entered in their
`xn--` form by the settings page, which takes the host from a parsed URL).

The pattern is matched against the host of the **top-level document**. A frame runs scripts by the rules of the page
it is embedded in, not by the rules of its own site, so allowing a site never hands that permission to a page that
embeds it. When several patterns match, the longest one wins, so `mail.example.com` can be blocked while
`example.com` is allowed. A document with an opaque origin and no host (a top-level `data:` or sandboxed document)
has no exception of its own and gets the default.

### First party and third party

Under `block-third-party`, a script is first-party when the origin it comes from is same origin with the top-level
origin, or schemelessly same site with it (the same registrable domain). All `file:` origins count as one site. The
origin a script comes from is:

- for an inline script or an event handler, the origin of its document;
- for a fetched script, the origin of the URL being fetched, checked again after every redirect;
- for a `blob:`, `data:` or `about:` script URL, the origin of the document that asked for it, since that document
  made the script.

A sandboxed or `data:` frame has an opaque origin, which is same site with nothing, so it counts as third-party.

### Hooks in LibWeb

Two hooks, both in existing upstream functions:

1. `is_scripting_enabled()` asks `ScriptingSettings::the().allows_scripting_in(document, settings)`. This decides
   everything in a document at once, and it is what makes `<noscript>` content render.
2. `main_fetch()` answers a request with a script-like destination with a network error when
   `ScriptingSettings::the().allows_request(request)` says no. This is what blocks third-party scripts in a
   first-party document, and it also means a blocked script is never requested from the network, including by the
   preload scanner.

The Debug menu switch is checked first and still turns everything off. WebUI pages (`about:settings` and the rest) are
exempt from the rules, because the browser's own interface needs scripts to work.

`ScriptingSettings` keeps the last eight decisions, because a document asks before every script and event handler
it runs and telling the parties apart needs a public suffix lookup.

### Storage

The rules are stored in `WebView::Settings` (the profile's `Settings.json`) under a `javascript` key, in the same
shape as `autoplay`:

```json
"javascript": {
    "policy": "block",
    "siteFilters": [{ "filter": "example.com", "policy": "allow" }]
}
```

The issue asks for `PermissionManager` or an equivalent store. `Settings` fits better for three reasons. The rules
need a default and pattern matching, which `PermissionManager` does not have. `Settings` is already mirrored between
the browser process and the UI process, which the popover needs. And the shared site-settings dialog in
`about:settings` works on a `SiteSetting`, so the exception list needed no new UI code. Filters are normalized when
they are loaded, and one that names no site is dropped.

For container tabs (issue #6), the plan is that a container can carry its own `javascript` object of the same shape
and the view sends that instead of the profile's. The IPC message and everything in WebContent already take the rules
as a value and would not change.

### Process and IPC flow

`ViewImplementation` sends `set_scripting_settings(page_id, policy, site_policies)` to a WebContent process when a
page is created in it, and again to every process hosting part of the view when the settings change. That includes
the processes of cross-site frames, which need the rules to know that they are third-party. A WebContent process
holds one set of rules for all of its pages, as it does for autoplay.

### User interface

- **Settings page** (`about:settings`, Permissions tab): a JavaScript row with the default policy and a button that
  opens the exception list in the shared site-settings dialog. A site is typed as a host or pasted as a URL.
- **Site-settings popover** (location bar): a JavaScript row with Allow, Block from other sites, Block, and Default.
  Choosing one writes or removes the exception for the current site (its registrable domain, or its host where it has
  none) and reloads the tab. The popover's reset link removes the exception too. In a private window the row shows
  the rule in effect but cannot change it, like the force-dark row, so that a private window never writes a visited
  site into the profile.

### Rust and C++ split

- **Rust**, new crate `libwebcommon_script_policy_rust` in `Libraries/LibWebCommon/ScriptPolicy/Rust`: site pattern
  parsing and matching, the policy type, and rule evaluation (`Rules::allows(host, party)`). It has no dependencies,
  no FFI and no unsafe code, and holds all of the unit tests.
- **Rust FFI**, `Libraries/LibWebCommon/Rust/src/script_policy.rs`: the C interface over the crate. It lives in the
  existing `libwebcommon_rust` static library so that LibWebCommon still links a single Rust library.
- **C++**: `Web::HTML::ScriptingRules` (LibWebCommon) wraps the FFI. `Web::HTML::ScriptingSettings` (LibWeb) finds the
  origins and decides the party, which needs the public suffix list that LibURL already has. `WebView::Settings`
  stores the rules.

## Touched upstream files

Hooks of a few lines each:

- `Libraries/LibWeb/HTML/Scripting/Environments.cpp`, `Libraries/LibWeb/Fetch/Fetching/Fetching.cpp`
- `Libraries/LibWeb/Internals/Internals.{idl,h,cpp}` (test hooks)
- `Services/WebContent/WebContentServer.ipc`, `Services/WebContent/ConnectionFromClient.{h,cpp}`
- `Libraries/LibWebView/ViewImplementation.{h,cpp}`, `Libraries/LibWebView/WebUI/SettingsUI.cpp`
- `UI/Qt/Tab.cpp`
- `Cargo.toml`, `Cargo.lock`, `Libraries/LibWebCommon/Rust/Cargo.toml`, `Libraries/LibWebCommon/Rust/src/lib.rs`,
  and the `CMakeLists.txt` of LibWeb, LibWebCommon and `Tests/LibWebView`

Larger additions, next to the autoplay code they mirror:

- `Libraries/LibWebView/Settings.{h,cpp}`
- `Base/res/ladybird/about-pages/settings.html`, `Base/res/ladybird/about-pages/settings/permissions.js`
- `UI/Qt/SiteSettingsPopover.{h,cpp}`

## Test plan

- Rust unit tests in the crate: pattern parsing and normalization, subdomain and IP matching, most specific pattern
  wins, exceptions over the default, first party versus third party.
- `Tests/LibWeb/Text/input/HTML/scripting-policy-*.html`: scripts, event handlers and `<noscript>` under each policy,
  decided by the top-level site, and third-party script fetches blocked while first-party ones run. They set the
  rules with `internals.setScriptingPolicy()` and `internals.setScriptingPolicyForSite()`.
- `Tests/LibWebView/TestScriptingSettings.cpp`: the settings are saved, loaded, normalized and matched.
- `Tests/LibWebView/test-webdriver-javascript-settings.py`: the whole path in a running browser with two sites.
  Saved rules apply from the first page, including in a cross-site frame's process; the settings page changes them;
  and they survive a restart.

## Known limits

- Dedicated and shared workers run in WebWorker processes, which do not receive the rules. A worker can only be
  started by a script, so `block` is unaffected, but under `block-third-party` a first-party worker can still
  `importScripts()` from another site.
- A change of rules takes effect for documents loaded afterwards. A document that is already open keeps its parsed
  `<noscript>` state, and its scripts stop or start with the next thing it tries to run. The popover reloads the tab
  for this reason.
- The popover edits the exception for the site's registrable domain. If a more specific exception from the settings
  page also matches the current host, that one keeps winning.
- There is no way to allow a single third-party site on a page (a matrix in the style of uMatrix). That is left to a
  later feature.

## Open questions

Asked on issue #4. The implementation follows the recommended answer to each.

1. Storage in `Settings` instead of `PermissionManager` (recommended), as argued above.
2. Exceptions cover subdomains and ignore the scheme (recommended), or are exact origins as for autoplay.
3. A private window cannot change the rules (recommended), or may write exceptions like a normal window.
4. Sandboxed and `data:` frames count as third-party (recommended), or inherit the party of their embedder.
