# Cookie and storage policy

Status: the cookie policy is implemented. Storage partitioning and clearing a site's data when its last tab closes are
designed here and not built yet. Tracked in the fork's issue #5.

## Problem

A site that is embedded in many other sites can follow a user across them if it can keep state in the browser. The user
has no say in which sites may keep cookies, and no way to stop the content of other sites on a page from keeping any.

## What upstream already does

Checked against upstream `b1e0374` (2026-11-25).

- Cookies are partitioned by top-level site. `HTTP::Cookie::PartitionContext` (`Libraries/LibHTTP/Cookie/Cookie.h`)
  carries the top-level site and whether the context has an ancestor of another site, and `WebView::CookieJar` keys a
  cookie set in a third-party context by that site. A third-party context never sees the cookies its site keeps as a
  first party. This is the behavior of Firefox's Total Cookie Protection, and it is always on.
- Every cookie goes through the cookie jar of its session in the browser process: the cookies of HTTP requests and
  responses through `Application::configure_request_server_control_client()`, and `document.cookie` and the Cookie Store
  API through `WebContentPage` and `WebWorkerClient`. The browser process works out the partition context itself; it
  does not take a renderer's word for it.
- The HTTP disk cache and connections are partitioned by `HTTP::NetworkIsolationKey`.
- `localStorage`, `sessionStorage`, IndexedDB, the Cache API, service workers, shared workers and `BroadcastChannel` are
  keyed by origin only. `Web::StorageAPI::StorageKey` is an origin, with a note that it is expected to gain the
  top-level site. Step 3 of "obtain a storage key" ("if the user has disabled storage, then return failure") is a
  FIXME.
- There is no user setting for any of this.

So the issue's "no storage partitioning" is half true: cookies are partitioned, other storage is not, and nothing can be
blocked.

## Design

### Policies

One policy decides for every site, and the user can make exceptions for single sites.

| Policy | A page of the site | Content of other sites on the page |
|--------|--------------------|------------------------------------|
| `allow` | keeps its cookies | keeps cookies in the partition of the page's site |
| `block-third-party` | keeps its cookies | reads and sets no cookies |
| `block` | reads and sets no cookies | reads and sets no cookies |

`allow` is what upstream does, and is the default of this series. The fork's own default belongs in the final
`Meta: Apply privacy fork defaults` series.

"Content of other sites" is what the cookie jar already calls a third-party context: its site is not the top-level
site, or one of its ancestors is of another site.

Blocking hides cookies; it does not delete them. A cookie a site kept while it was allowed to is in the jar until it
expires or the user clears it, and is used again if the site is allowed again.

### Exceptions

An exception is for the pages of a site, that is, for a top-level site. It replaces the default policy for everything
on those pages, the content of other sites included. An exception to `block-third-party` that says `allow` for
`shop.example` lets the payment frame on the shop's pages keep its (partitioned) cookies. An exception for
`tracker.example` does nothing for or against its frames on other sites.

This is the same rule as per-site JavaScript control (issue #4): the site in the address bar decides.

A site is kept as the registrable domain of what the user typed (`www.example.com` and `https://example.com:8443/x`
both become `example.com`), or as the host where there is no registrable domain (`localhost`, an IP address). It covers
every scheme. A public suffix such as `com` is refused.

### Where the decision is made

- **Rust**, `Libraries/LibWebCommon/SiteDataPolicy/Rust` (`libwebcommon_site_data_policy_rust`): the policies, the
  rules with their exceptions, and the decision. Asked with the serialized top-level site and whether the context is a
  third party, it answers `Denied`, `FirstParty` or `Partitioned`. It does no I/O and has no unsafe code. Its C
  interface is in the existing `libwebcommon_rust`, so no new static library is linked.
- **C++**, `Web::StorageAPI::SiteDataRules` (`Libraries/LibWebCommon/StorageAPI/SiteDataPolicy.h`): a value that holds
  the Rust rules. Turning what a user typed into a site is done here, because it needs the URL parser and the public
  suffix list.
- **`WebView::CookieJar`** holds the rules of its session and applies them in `get_cookie()`, `set_cookie()` and
  `get_all_cookies_cookiestore()`. These are the three functions every page-visible cookie goes through, so HTTP
  requests, WebSocket handshakes, `document.cookie`, the Cookie Store API and workers are all covered by one hook.
  WebDriver's and DevTools' views of the jar are not filtered.
- **`WebView::Settings`** saves the rules under `siteData`, in the shape of `autoplay`. `Application` gives them to the
  cookie jar of the ordinary session and of the private session, at startup and when they change. Changing them also
  drops every document's cached `document.cookie`.
- **`about:settings`**: a Cookies row in the Permissions tab, with the shared exceptions dialog.

A private window uses the same rules as an ordinary one.

### Storage partitioning (not built)

The plan is to give `Web::StorageAPI::StorageKey` the top-level site for third-party contexts, as the Storage
Partitioning proposal describes, so that every storage API that is keyed by it is partitioned at once. The decision
crate gains a `storage_access()` beside `cookie_access()` with the same three answers, and "obtain a storage key"
asks it in step 3.

This cannot be done in the browser process alone, the way cookies are. `localStorage` and `sessionStorage` do go
through the browser process (`WebContentPage::canonical_storage_key()`), but a renderer delivers `storage` events
between its own documents by comparing origins, so two partitions of one origin in one renderer would hear each other.
IndexedDB, the Cache API and `BroadcastChannel` are keyed inside the renderer. The storage key itself has to change,
which touches upstream files in LibWeb widely and is its own series.

Blocked storage has two possible shapes: the API throws a `SecurityError` (what the spec says for disabled storage, and
what Firefox and Chrome do), or the API works but keeps nothing beyond the document (what Safari and Brave do, because
throwing breaks sites that do not expect it).

### Clearing a site's data when its last tab closes (not built)

An exception can also say "keep until I close the site". When the last top-level document of the site in a session goes
away, the browser deletes the site's first-party cookies, the cookies partitioned under it, and its storage. This
needs storage keyed by top-level site to find what to delete, so it follows storage partitioning.

## Tests

- Rust unit tests in the crate: the decision table, site matching, exceptions.
- `Tests/LibWebCommon/TestSiteDataPolicy.cpp`: what a user may type as a site, and the rules through the C interface.
- `Tests/LibWebView/TestCookieJar.cpp`: each policy against first-party, embedded, nested and context-free requests,
  exceptions, and that blocking does not delete.
- `Tests/LibWebView/TestSiteDataSettings.cpp`: saving, loading, damaged settings files and change notifications.
- `Tests/LibWeb/Text/input/site-data-policy-decides-which-cookies-a-site-keeps.html`: real pages with cross-site frames
  over HTTPS, for HTTP cookies and `document.cookie`.
- `Tests/LibWebView/test-webdriver-cookie-settings.py`: a running browser, the settings page, and a restart.
- Upstream already imports the WPT `cookies/samesite` and `cookiestore` suites, which run with the default policy and
  so check that it changes nothing.

## Open questions

These are for the fork's owner. The code follows the first option of each.

1. **What an exception is for.** The top-level site (as built, and as for JavaScript), or the site the cookies belong
   to, wherever it is embedded (Firefox's cookie exceptions)? The second lets a user say "never any cookies for
   tracker.example", which content blocking already does more thoroughly.
2. **Third parties under `allow`.** Partitioned cookies (as built, and what upstream does), or a fourth policy that
   gives third parties their unpartitioned cookies back for sites that break? Upstream has no unpartitioned mode, so
   that would be new code against the grain.
3. **Blocked storage.** Throw `SecurityError`, or work without keeping anything? This decides what the storage series
   builds.
4. **Storage partitioning and upstream's default.** Always on once built, like cookie partitioning is upstream, or a
   setting that is off until the fork defaults series turns it on? `FORK.md` says a feature keeps upstream's default
   behavior, which argues for the setting.
5. **Private windows.** The same rules (as built), or always at least `block-third-party`?
