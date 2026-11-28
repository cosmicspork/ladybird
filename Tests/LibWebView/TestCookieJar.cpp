/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/QuickSort.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/CookieJar.h>

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static bool stores_cookie(URL::URL const& url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    auto jar = WebView::CookieJar::create();
    jar->set_cookie(url, cookie, source, {}, HTTP::Cookie::SameSiteContext::same_site());
    return !jar->get_all_cookies().is_empty();
}

TEST_CASE(cookies_round_trip_on_fresh_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    {
        auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

        HTTP::Cookie::ParsedCookie cookie {
            .name = "foo"_string,
            .value = "bar"_string,
            .expiry_time_from_expires_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600),
        };
        jar->set_cookie(parse_url("https://example.com/"sv), cookie, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site());

        // The jar flushes dirty cookies to the database on destruction.
    }

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
}

TEST_CASE(same_site_cookie_storage_and_retrieval)
{
    using namespace HTTP::Cookie;

    auto url = parse_url("https://example.com/"sv);
    auto jar = WebView::CookieJar::create();
    auto strict_cookie = parse_cookie(url, "strict=1; SameSite=Strict; HttpOnly"sv).release_value();
    auto lax_cookie = parse_cookie(url, "lax=1; SameSite=Lax"sv).release_value();
    auto default_cookie = parse_cookie(url, "default=1"sv).release_value();
    auto none_cookie = parse_cookie(url, "none=1; SameSite=None; Secure"sv).release_value();

    SameSiteContext cross_site_fetch { .status = SameSiteStatus::CrossSite, .is_safe_method = true };
    SameSiteContext cross_site_post { .status = SameSiteStatus::CrossSite, .is_top_level_navigation = true };
    SameSiteContext cross_site_get { .status = SameSiteStatus::CrossSite, .is_top_level_navigation = true, .is_safe_method = true };
    SameSiteContext same_site_fetch { .status = SameSiteStatus::SameSite };

    jar->set_cookie(url, strict_cookie, Source::Http, {}, cross_site_fetch);
    jar->set_cookie(url, lax_cookie, Source::Http, {}, cross_site_fetch);
    jar->set_cookie(url, default_cookie, Source::NonHttp, {}, cross_site_fetch);
    jar->set_cookie(url, none_cookie, Source::Http, {}, cross_site_fetch);
    EXPECT_EQ(jar->get_cookie(url, Source::Http, {}, same_site_fetch), "none=1"_string);

    jar->set_cookie(url, strict_cookie, Source::Http, {}, cross_site_post);
    jar->set_cookie(url, lax_cookie, Source::Http, {}, cross_site_post);
    jar->set_cookie(url, default_cookie, Source::Http, {}, cross_site_post);
    EXPECT_EQ(jar->get_all_cookies().size(), 4uz);

    EXPECT_EQ(jar->get_cookie(url, Source::Http, {}, cross_site_fetch), "none=1"_string);
    EXPECT_EQ(jar->get_cookie(url, Source::Http, {}, cross_site_post), "none=1"_string);
    auto navigation_cookies = jar->get_cookie(url, Source::Http, {}, cross_site_get);
    EXPECT(navigation_cookies.contains("none=1"sv));
    EXPECT(navigation_cookies.contains("lax=1"sv));
    EXPECT(navigation_cookies.contains("default=1"sv));
    EXPECT(!navigation_cookies.contains("strict=1"sv));
    EXPECT_EQ(jar->get_cookie(url, Source::NonHttp, {}, cross_site_get), "none=1"_string);
    EXPECT(jar->get_cookie(url, Source::Http, {}, same_site_fetch).contains("strict=1"sv));
}

TEST_CASE(http_cookie_prefixes_require_secure_http_provenance)
{
    auto http_url = parse_url("http://attacker.example.com/"sv);
    auto https_url = parse_url("https://example.com/"sv);

    EXPECT(!stores_cookie(http_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__hTtP-session"_string,
            .value = "attacker"_string,
            .domain = "example.com"_string,
            .path = "/"_string,
        },
        HTTP::Cookie::Source::NonHttp));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Host-Http-session"_string,
            .value = "attacker"_string,
            .path = "/"_string,
            .secure_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Host-Http-session"_string,
            .value = "attacker"_string,
            .secure_attribute_present = true,
            .http_only_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    EXPECT(!stores_cookie(https_url,
        HTTP::Cookie::ParsedCookie {
            .name = {},
            .value = "__Http-session"_string,
            .path = "/"_string,
            .secure_attribute_present = true,
            .http_only_attribute_present = true,
        },
        HTTP::Cookie::Source::Http));

    HTTP::Cookie::Cookie structured_cookie {
        .name = "__Http-session"_string,
        .value = "attacker"_string,
        .path = "/"_string,
        .secure = true,
        .host_only = true,
    };
    auto jar = WebView::CookieJar::create();
    EXPECT(jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());
    structured_cookie.name = "__Host-Http-session"_string;
    EXPECT(jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());
    structured_cookie.http_only = true;
    EXPECT(!jar->set_cookie_from_devtools(https_url, {}, structured_cookie).is_error());

    EXPECT(stores_cookie(http_url,
        HTTP::Cookie::ParsedCookie {
            .name = "__Httq-session"_string,
            .value = "ordinary"_string,
            .domain = "example.com"_string,
            .path = "/"_string,
        },
        HTTP::Cookie::Source::NonHttp));
}

TEST_CASE(unversioned_cookie_table_is_stamped_and_preserved)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    // The Cookies table as created before schema versioning existed.
    TRY_OR_FAIL(database->execute_raw(R"#(
        CREATE TABLE Cookies (
            name TEXT,
            value TEXT,
            same_site INTEGER CHECK (same_site >= 0 AND same_site <= 3),
            creation_time INTEGER,
            last_access_time INTEGER,
            expiry_time INTEGER,
            domain TEXT,
            path TEXT,
            secure BOOLEAN,
            http_only BOOLEAN,
            host_only BOOLEAN,
            persistent BOOLEAN,
            PRIMARY KEY(name, domain, path)
        );
    )#"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO Cookies VALUES ('foo', 'bar', 0, 0, 0, 4102444800000, 'example.com', '/', 0, 0, 0, 1);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    Optional<u32> version;
    auto statement = TRY_OR_FAIL(database->prepare_statement("SELECT version FROM SchemaVersions WHERE store = 'Cookies';"sv));
    database->execute_statement(statement, [&](auto statement_id) -> ErrorOr<void> { version = database->result_column<u32>(statement_id, 0); return {}; });
    EXPECT_EQ(version, Optional<u32> { 3u });

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));

    // A cookie stored before cookies were partitioned stays a first-party cookie.
    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "foo"_string);
    EXPECT_EQ(cookies[0].value, "bar"_string);
    EXPECT_EQ(cookies[0].domain, "example.com"_string);
    EXPECT(cookies[0].partition_key.is_empty());
}

TEST_CASE(newer_cookie_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Cookies', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(third_party_cookies_are_partitioned_by_top_level_site)
{
    auto jar = WebView::CookieJar::create();
    auto url = parse_url("https://widget.example/"sv);
    auto context = [](StringView top_level_site, bool has_cross_site_ancestor = false) {
        return Optional<HTTP::Cookie::PartitionContext> { { Utf16String::from_utf8(top_level_site), has_cross_site_ancestor } };
    };
    auto first_top_level_site = context("https://first.example"sv);
    auto second_top_level_site = context("https://second.example"sv);
    auto widget_site = context("https://widget.example"sv);

    // The widget's site framed in another site's frame under the widget's own top-level document.
    auto widget_site_with_cross_site_ancestor = context("https://widget.example"sv, true);

    auto set_cookie = [&](StringView name, Optional<HTTP::Cookie::PartitionContext> const& partition_context) {
        HTTP::Cookie::ParsedCookie cookie {
            .name = MUST(String::from_utf8(name)),
            .value = "1"_string,
            .path = "/"_string,
        };
        jar->set_cookie(url, cookie, HTTP::Cookie::Source::Http, partition_context, HTTP::Cookie::SameSiteContext::same_site());
    };

    set_cookie("embedded"sv, first_top_level_site);
    set_cookie("first-party"sv, widget_site);
    set_cookie("nested"sv, widget_site_with_cross_site_ancestor);

    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, first_top_level_site, HTTP::Cookie::SameSiteContext::same_site()), "embedded=1"_string);
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, second_top_level_site, HTTP::Cookie::SameSiteContext::same_site()), String {});
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, widget_site, HTTP::Cookie::SameSiteContext::same_site()), "first-party=1"_string);

    // A context with a cross-site ancestor is third-party even under a top-level document of its own site.
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, widget_site_with_cross_site_ancestor, HTTP::Cookie::SameSiteContext::same_site()), "nested=1"_string);

    // A context of nothing is a first-party context, as for the UI process's own requests.
    EXPECT_EQ(jar->get_cookie(url, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site()), "first-party=1"_string);

    // A WebSocket handshake to the widget's site is a first-party request under the widget's top-level documents.
    EXPECT_EQ(jar->get_cookie(parse_url("wss://widget.example/"sv), HTTP::Cookie::Source::Http, widget_site, HTTP::Cookie::SameSiteContext::same_site()), "first-party=1"_string);

    auto cookies = jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 3uz);
    for (auto const& cookie : cookies) {
        if (cookie.name == "embedded"sv)
            EXPECT_EQ(cookie.partition_key, "https://first.example"_utf16);
        else if (cookie.name == "nested"sv)
            EXPECT_EQ(cookie.partition_key, "https://widget.example"_utf16);
        else
            EXPECT(cookie.partition_key.is_empty());
    }
}

TEST_CASE(host_only_cookie_is_stored_apart_from_domain_cookie_for_same_host)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto url = parse_url("https://www.example.com/"sv);
    auto set_cookie = [&](WebView::CookieJar& jar, StringView value, Optional<String> domain) {
        HTTP::Cookie::ParsedCookie cookie {
            .name = "a"_string,
            .value = MUST(String::from_utf8(value)),
            .expiry_time_from_expires_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600),
            .domain = move(domain),
            .path = "/"_string,
        };
        jar.set_cookie(url, cookie, HTTP::Cookie::Source::NonHttp, {}, HTTP::Cookie::SameSiteContext::same_site());
    };
    auto values = [](WebView::CookieJar& jar) {
        Vector<String> values;
        for (auto const& cookie : jar.get_all_cookies())
            values.append(cookie.value);
        quick_sort(values);
        return values;
    };

    {
        auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));
        set_cookie(*jar, "domain"sv, "www.example.com"_string);
        set_cookie(*jar, "host-only"sv, {});
        EXPECT_EQ(values(*jar), (Vector { "domain"_string, "host-only"_string }));
    }

    auto jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));
    EXPECT_EQ(values(*jar), (Vector { "domain"_string, "host-only"_string }));

    HTTP::Cookie::ParsedCookie expired_cookie {
        .name = "a"_string,
        .value = {},
        .expiry_time_from_expires_attribute = UnixDateTime::earliest(),
        .domain = "www.example.com"_string,
        .path = "/"_string,
    };
    jar->set_cookie(url, expired_cookie, HTTP::Cookie::Source::NonHttp, {}, HTTP::Cookie::SameSiteContext::same_site());
    EXPECT_EQ(values(*jar), Vector { "host-only"_string });
}

namespace {

using Web::StorageAPI::SiteDataPolicy;
using Web::StorageAPI::SiteDataRules;

// A jar and the contexts that use the cookies of one site: as a first party, in a frame of another site, and without a
// context, as the UI process does.
struct SiteDataRulesFixture {
    static Optional<HTTP::Cookie::PartitionContext> context(StringView top_level_site, bool has_cross_site_ancestor = false)
    {
        return HTTP::Cookie::PartitionContext { Utf16String::from_utf8(top_level_site), has_cross_site_ancestor };
    }

    void set_cookie(StringView name, Optional<HTTP::Cookie::PartitionContext> const& partition_context, HTTP::Cookie::Source source = HTTP::Cookie::Source::Http)
    {
        HTTP::Cookie::ParsedCookie cookie {
            .name = MUST(String::from_utf8(name)),
            .value = "1"_string,
            .path = "/"_string,
        };
        jar->set_cookie(url, cookie, source, partition_context, HTTP::Cookie::SameSiteContext::same_site());
    }

    String get_cookie(Optional<HTTP::Cookie::PartitionContext> const& partition_context, HTTP::Cookie::Source source = HTTP::Cookie::Source::Http)
    {
        return jar->get_cookie(url, source, partition_context, HTTP::Cookie::SameSiteContext::same_site());
    }

    NonnullOwnPtr<WebView::CookieJar> jar { WebView::CookieJar::create() };
    URL::URL url { parse_url("https://widget.example/"sv) };
    Optional<HTTP::Cookie::PartitionContext> first_party { context("https://widget.example"sv) };
    Optional<HTTP::Cookie::PartitionContext> embedded { context("https://news.example"sv) };
    Optional<HTTP::Cookie::PartitionContext> embedded_elsewhere { context("https://shop.example"sv) };
    Optional<HTTP::Cookie::PartitionContext> nested { context("https://widget.example"sv, true) };
};

}

TEST_CASE(a_new_jar_allows_every_site_to_keep_cookies)
{
    auto jar = WebView::CookieJar::create();
    EXPECT(jar->site_data_rules().allow_everything());
    EXPECT_EQ(jar->site_data_rules().default_policy(), SiteDataPolicy::Allow);
}

TEST_CASE(blocking_third_party_cookies_leaves_first_party_cookies_alone)
{
    SiteDataRulesFixture f;
    f.jar->set_site_data_rules({ SiteDataPolicy::BlockThirdParty, {} });

    f.set_cookie("first-party"sv, f.first_party);
    f.set_cookie("embedded"sv, f.embedded);
    f.set_cookie("nested"sv, f.nested);

    EXPECT_EQ(f.get_cookie(f.first_party), "first-party=1"_string);
    EXPECT_EQ(f.get_cookie({}), "first-party=1"_string);
    EXPECT_EQ(f.get_cookie(f.embedded), String {});
    // A context with an ancestor of another site is a third party even under a top-level document of its own site.
    EXPECT_EQ(f.get_cookie(f.nested), String {});

    // The cookies a third party set were ignored, not stored out of sight.
    EXPECT_EQ(f.jar->get_all_cookies().size(), 1uz);

    // A request without a context is a first party to its own site.
    f.set_cookie("no-context"sv, {});
    EXPECT_EQ(f.jar->get_all_cookies().size(), 2uz);
}

TEST_CASE(blocking_third_party_cookies_hides_the_partitioned_cookies_already_stored)
{
    SiteDataRulesFixture f;
    f.set_cookie("embedded"sv, f.embedded);
    EXPECT_EQ(f.get_cookie(f.embedded), "embedded=1"_string);

    f.jar->set_site_data_rules({ SiteDataPolicy::BlockThirdParty, {} });
    EXPECT_EQ(f.get_cookie(f.embedded), String {});
    EXPECT(f.jar->get_all_cookies_cookiestore(f.url, f.embedded, HTTP::Cookie::SameSiteContext::same_site()).is_empty());

    // Blocking does not delete: The cookie is there again once its context may use cookies.
    f.jar->set_site_data_rules({});
    EXPECT_EQ(f.get_cookie(f.embedded), "embedded=1"_string);
    EXPECT_EQ(f.jar->get_all_cookies_cookiestore(f.url, f.embedded, HTTP::Cookie::SameSiteContext::same_site()).size(), 1uz);
}

TEST_CASE(blocking_all_cookies_denies_every_context)
{
    SiteDataRulesFixture f;
    f.set_cookie("before"sv, f.first_party);
    f.jar->set_site_data_rules({ SiteDataPolicy::Block, {} });

    f.set_cookie("first-party"sv, f.first_party);
    f.set_cookie("no-context"sv, {});
    f.set_cookie("embedded"sv, f.embedded);
    f.set_cookie("script"sv, f.first_party, HTTP::Cookie::Source::NonHttp);

    EXPECT_EQ(f.get_cookie(f.first_party), String {});
    EXPECT_EQ(f.get_cookie(f.first_party, HTTP::Cookie::Source::NonHttp), String {});
    EXPECT_EQ(f.get_cookie({}), String {});
    EXPECT_EQ(f.get_cookie(f.embedded), String {});
    EXPECT(f.jar->get_all_cookies_cookiestore(f.url, f.first_party, HTTP::Cookie::SameSiteContext::same_site()).is_empty());

    auto cookies = f.jar->get_all_cookies();
    EXPECT_EQ(cookies.size(), 1uz);
    EXPECT_EQ(cookies[0].name, "before"_string);
}

TEST_CASE(an_exception_decides_for_the_pages_of_its_site)
{
    SiteDataRulesFixture f;
    OrderedHashMap<String, SiteDataPolicy> exceptions;
    exceptions.set("news.example"_string, SiteDataPolicy::Allow);
    exceptions.set("shop.example"_string, SiteDataPolicy::Block);
    f.jar->set_site_data_rules({ SiteDataPolicy::BlockThirdParty, move(exceptions) });

    f.set_cookie("under-news"sv, f.embedded);
    f.set_cookie("under-shop"sv, f.embedded_elsewhere);
    f.set_cookie("first-party"sv, f.first_party);

    // The widget may keep cookies in its frames on the news site, in the partition of that site.
    EXPECT_EQ(f.get_cookie(f.embedded), "under-news=1"_string);
    EXPECT_EQ(f.get_cookie(f.embedded_elsewhere), String {});
    EXPECT_EQ(f.get_cookie(f.first_party), "first-party=1"_string);

    // The exception for the shop is for the pages of the shop, whoever the cookies belong to.
    auto shop_url = parse_url("https://shop.example/"sv);
    HTTP::Cookie::ParsedCookie cookie { .name = "shop"_string, .value = "1"_string, .path = "/"_string };
    f.jar->set_cookie(shop_url, cookie, HTTP::Cookie::Source::Http, f.embedded_elsewhere, HTTP::Cookie::SameSiteContext::same_site());
    f.jar->set_cookie(shop_url, cookie, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site());
    EXPECT_EQ(f.jar->get_cookie(shop_url, HTTP::Cookie::Source::Http, f.embedded_elsewhere, HTTP::Cookie::SameSiteContext::same_site()), String {});
    EXPECT_EQ(f.jar->get_cookie(shop_url, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site()), String {});

    // The shop's frames on the news site are not pages of the shop.
    f.jar->set_cookie(shop_url, cookie, HTTP::Cookie::Source::Http, f.embedded, HTTP::Cookie::SameSiteContext::same_site());
    EXPECT_EQ(f.jar->get_cookie(shop_url, HTTP::Cookie::Source::Http, f.embedded, HTTP::Cookie::SameSiteContext::same_site()), "shop=1"_string);
}

TEST_CASE(a_websocket_handshake_follows_the_rules_of_its_site)
{
    SiteDataRulesFixture f;
    f.set_cookie("first-party"sv, f.first_party);

    OrderedHashMap<String, SiteDataPolicy> exceptions;
    exceptions.set("widget.example"_string, SiteDataPolicy::Block);
    f.jar->set_site_data_rules({ SiteDataPolicy::Allow, move(exceptions) });

    auto websocket_url = parse_url("wss://widget.example/"sv);
    EXPECT_EQ(f.jar->get_cookie(websocket_url, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site()), String {});
    EXPECT_EQ(f.jar->get_cookie(websocket_url, HTTP::Cookie::Source::Http, f.first_party, HTTP::Cookie::SameSiteContext::same_site()), String {});
}

TEST_CASE(webdriver_sees_the_cookies_a_blocked_context_cannot_use)
{
    SiteDataRulesFixture f;
    f.set_cookie("first-party"sv, f.first_party);
    f.jar->set_site_data_rules({ SiteDataPolicy::Block, {} });

    EXPECT_EQ(f.get_cookie(f.first_party), String {});
    EXPECT_EQ(f.jar->get_all_cookies_webdriver(f.url).size(), 1uz);
    EXPECT(f.jar->get_named_cookie(f.url, "first-party"sv).has_value());
}
