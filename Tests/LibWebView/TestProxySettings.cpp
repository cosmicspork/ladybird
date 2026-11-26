/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/ProxySettings.h>

using WebView::ProxySettings;
using WebView::ProxySettingsForcedBy;

static ProxySettings manual_proxy(StringView type, StringView host, i64 port, Vector<String> bypass = {})
{
    ProxySettings settings;
    settings.mode = ProxySettings::Mode::Manual;
    settings.type = MUST(String::from_utf8(type));
    settings.host = MUST(String::from_utf8(host));
    settings.port = port;
    settings.bypass = move(bypass);
    return settings;
}

static ProxySettings proxy_settings_from(StringView json)
{
    return ProxySettings::from_json(MUST(JsonValue::from_string(json)));
}

static HTTP::ProxyConfiguration environment_with(HashMap<StringView, StringView> const& environment)
{
    return HTTP::ProxyConfiguration::from_environment([&](StringView name) { return environment.get(name); });
}

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

TEST_CASE(the_environment_decides_by_default)
{
    ProxySettings settings;
    EXPECT_EQ(settings.mode, ProxySettings::Mode::System);

    auto resolved = WebView::resolve_proxy_settings(settings, {}, false);
    EXPECT(resolved.configuration.is_empty());
    EXPECT(!resolved.configuration.blocks_all_requests());
    EXPECT_EQ(resolved.forced_by, ProxySettingsForcedBy::Nothing);
    EXPECT(!resolved.error.has_value());

    auto environment = environment_with({ { "https_proxy"sv, "http://secure.example:3128"sv } });
    resolved = WebView::resolve_proxy_settings(settings, environment, false);
    EXPECT_EQ(resolved.configuration.proxy_for(url("https://example.com/"sv))->host, "secure.example"sv);
    EXPECT(!resolved.configuration.proxy_for(url("http://example.com/"sv)).has_value());
}

TEST_CASE(proxy_settings_survive_json)
{
    auto proxy = manual_proxy("https"sv, "Proxy.Example"sv, 8443, { "10.0.0.0/8"_string, "*"_string });
    EXPECT_EQ(ProxySettings::from_json(proxy.to_json()), proxy);
    EXPECT_EQ(ProxySettings::from_json(ProxySettings {}.to_json()), ProxySettings {});

    auto settings = proxy_settings_from(R"({"mode":"manual","type":"socks5h","host":"127.0.0.1","port":9050,"bypass":["localhost","127.0.0.1",".internal.example"]})"sv);
    EXPECT_EQ(settings, manual_proxy("socks5h"sv, "127.0.0.1"sv, 9050, { "localhost"_string, "127.0.0.1"_string, ".internal.example"_string }));
}

TEST_CASE(an_unknown_mode_reads_as_the_default)
{
    for (auto json : { R"({})"sv, R"({"mode":"system"})"sv, R"({"mode":"pac"})"sv, R"({"mode":"MANUAL"})"sv, R"({"mode":2})"sv, R"({"mode":null})"sv, R"("manual")"sv, R"([])"sv, R"(null)"sv })
        EXPECT_EQ(proxy_settings_from(json).mode, ProxySettings::Mode::System);

    EXPECT_EQ(proxy_settings_from(R"({"mode":"none"})"sv).mode, ProxySettings::Mode::None);
    EXPECT_EQ(proxy_settings_from(R"({"mode":"manual"})"sv).mode, ProxySettings::Mode::Manual);
}

TEST_CASE(a_manual_proxy_with_missing_or_mistyped_fields_stays_manual)
{
    // Dropping such a setting would turn a request for a proxy into a direct connection.
    for (auto json : {
             R"({"mode":"manual"})"sv,
             R"({"mode":"manual","type":5,"host":["127.0.0.1"],"port":"9050","bypass":"localhost"})"sv,
             R"({"mode":"manual","type":"socks5h","host":"127.0.0.1","port":9050.5})"sv,
             R"({"mode":"manual","type":"socks5h","host":"127.0.0.1","port":1e20})"sv,
             R"({"mode":"manual","type":"socks5h","host":"127.0.0.1","port":-1})"sv,
             R"({"mode":"manual","type":"socks5h","host":"","port":9050})"sv,
             R"({"mode":"manual","type":"socks6","host":"127.0.0.1","port":9050})"sv,
             R"({"mode":"manual","type":"socks5h","host":"user:secret@127.0.0.1","port":9050})"sv,
             R"({"mode":"manual","type":"socks5h","host":"127.0.0.1","port":9050,"bypass":["10.0.0.0/64"]})"sv,
         }) {
        auto settings = proxy_settings_from(json);
        EXPECT_EQ(settings.mode, ProxySettings::Mode::Manual);

        auto resolved = WebView::resolve_proxy_settings(settings, {}, false);
        EXPECT(resolved.configuration.blocks_all_requests());
        EXPECT(!resolved.configuration.is_empty());
        EXPECT(resolved.error.has_value());
        EXPECT_EQ(resolved.forced_by, ProxySettingsForcedBy::Nothing);
    }
}

TEST_CASE(bypass_entries_that_are_not_strings_are_left_out)
{
    // Leaving an entry out only sends more through the proxy.
    auto settings = proxy_settings_from(R"({"mode":"manual","type":"http","host":"proxy.example","port":3128,"bypass":["localhost",7,null,["a"],"example.org"]})"sv);
    EXPECT_EQ(settings.bypass, (Vector<String> { "localhost"_string, "example.org"_string }));
}

TEST_CASE(a_manual_proxy_is_used_for_all_traffic)
{
    struct Case {
        StringView type;
        StringView curl_url;
        bool resolves_hostnames;
    };
    for (auto const& test : {
             Case { "http"sv, "http://proxy.example:8080"sv, true },
             Case { "https"sv, "https://proxy.example:8080"sv, true },
             Case { "socks4"sv, "socks4://proxy.example:8080"sv, false },
             Case { "socks4a"sv, "socks4a://proxy.example:8080"sv, true },
             Case { "socks5"sv, "socks5://proxy.example:8080"sv, false },
             Case { "socks5h"sv, "socks5h://proxy.example:8080"sv, true },
         }) {
        auto resolved = WebView::resolve_proxy_settings(manual_proxy(test.type, " Proxy.Example "sv, 8080), {}, false);
        EXPECT(!resolved.error.has_value());
        EXPECT(!resolved.configuration.blocks_all_requests());
        EXPECT(!resolved.configuration.is_empty());

        for (auto target : { "http://example.com/"sv, "https://example.com/"sv, "ws://example.com/"sv, "wss://example.com/"sv }) {
            auto proxy = resolved.configuration.proxy_for(url(target));
            VERIFY(proxy.has_value());
            EXPECT_EQ(proxy->to_curl_url(), test.curl_url);
            EXPECT_EQ(proxy->resolves_hostnames(), test.resolves_hostnames);
            EXPECT(proxy->userinfo.is_empty());
        }
    }

    auto resolved = WebView::resolve_proxy_settings(manual_proxy("socks5h"sv, "[::1]"sv, 9050), {}, false);
    EXPECT_EQ(resolved.configuration.proxy_for(url("https://example.com/"sv))->to_curl_url(), "socks5h://[::1]:9050"sv);
}

TEST_CASE(the_bypass_list_of_a_manual_proxy_is_applied)
{
    auto resolved = WebView::resolve_proxy_settings(manual_proxy("socks5h"sv, "127.0.0.1"sv, 9050, { "LOCALHOST"_string, ".internal.example, 10.0.0.0/8"_string, "::1"_string }), {}, false);
    EXPECT(!resolved.error.has_value());

    auto is_proxied = [&](StringView target) { return resolved.configuration.proxy_for(url(target)).has_value(); };
    EXPECT(is_proxied("https://example.com/"sv));
    EXPECT(is_proxied("http://notlocalhost/"sv));
    EXPECT(is_proxied("http://11.0.0.1/"sv));
    EXPECT(!is_proxied("http://localhost:8000/"sv));
    EXPECT(!is_proxied("https://a.internal.example/"sv));
    EXPECT(!is_proxied("http://10.1.2.3/"sv));
    EXPECT(!is_proxied("ws://[::1]/"sv));
}

TEST_CASE(the_environment_wins_over_saved_settings)
{
    auto environment = environment_with({ { "all_proxy"sv, "socks5h://tor.example:9050"sv }, { "no_proxy"sv, "localhost"sv } });

    ProxySettings none;
    none.mode = ProxySettings::Mode::None;
    for (auto const& saved : { ProxySettings {}, none, manual_proxy("http"sv, "proxy.example"sv, 3128), manual_proxy("bogus"sv, ""sv, 0) }) {
        auto resolved = WebView::resolve_proxy_settings(saved, environment, false);
        EXPECT_EQ(resolved.forced_by, ProxySettingsForcedBy::Environment);
        EXPECT(!resolved.configuration.blocks_all_requests());
        EXPECT_EQ(resolved.configuration.proxy_for(url("https://example.com/"sv))->to_curl_url(), "socks5h://tor.example:9050"sv);
        EXPECT_EQ(resolved.configuration.proxy_for(url("http://example.com/"sv))->to_curl_url(), "socks5h://tor.example:9050"sv);
        EXPECT(!resolved.configuration.proxy_for(url("http://localhost/"sv)).has_value());
    }

    // What is wrong with the saved proxy is still reported, for when the environment no longer overrides it.
    EXPECT(WebView::resolve_proxy_settings(manual_proxy("bogus"sv, ""sv, 0), environment, false).error.has_value());
    EXPECT(!WebView::resolve_proxy_settings(manual_proxy("http"sv, "proxy.example"sv, 3128), environment, false).error.has_value());
}

TEST_CASE(the_command_line_wins_over_saved_settings)
{
    // With --proxy-autoconfig-url, the saved proxy must not be handed to RequestServer.
    auto resolved = WebView::resolve_proxy_settings(manual_proxy("http"sv, "proxy.example"sv, 3128), {}, true);
    EXPECT_EQ(resolved.forced_by, ProxySettingsForcedBy::CommandLine);
    EXPECT(resolved.configuration.is_empty());

    auto environment = environment_with({ { "https_proxy"sv, "http://secure.example:3128"sv } });
    resolved = WebView::resolve_proxy_settings(manual_proxy("http"sv, "proxy.example"sv, 3128), environment, true);
    EXPECT_EQ(resolved.forced_by, ProxySettingsForcedBy::CommandLine);
    EXPECT_EQ(resolved.configuration.proxy_for(url("https://example.com/"sv))->host, "secure.example"sv);
    EXPECT(!resolved.configuration.proxy_for(url("http://example.com/"sv)).has_value());
}
