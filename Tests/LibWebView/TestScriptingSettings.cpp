/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/ScriptingPolicy.h>
#include <LibWebView/Settings.h>

using Web::HTML::ScriptingPolicy;

static ByteString settings_path(StringView name)
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-scripting-{}-{}.json", name, Core::System::getpid())).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    return path;
}

TEST_CASE(policy_names_round_trip)
{
    for (auto policy : { ScriptingPolicy::Allow, ScriptingPolicy::BlockThirdParty, ScriptingPolicy::Block })
        EXPECT_EQ(Web::HTML::scripting_policy_from_string(Web::HTML::scripting_policy_to_string(policy)), policy);

    EXPECT(!Web::HTML::scripting_policy_from_string(""sv).has_value());
    EXPECT(!Web::HTML::scripting_policy_from_string("Allow"sv).has_value());
    EXPECT(!Web::HTML::scripting_policy_from_string("block-first-party"sv).has_value());
}

TEST_CASE(sites_are_kept_in_one_spelling)
{
    EXPECT_EQ(Web::HTML::normalize_scripting_site("https://Example.COM:8080/path?query"sv), "example.com"_string);
    EXPECT_EQ(Web::HTML::normalize_scripting_site("*.example.com."sv), "example.com"_string);
    EXPECT_EQ(Web::HTML::normalize_scripting_site("127.0.0.1:8000"sv), "127.0.0.1"_string);
    EXPECT(!Web::HTML::normalize_scripting_site(""sv).has_value());
    EXPECT(!Web::HTML::normalize_scripting_site("not a site"sv).has_value());
}

TEST_CASE(an_exception_made_on_a_page_is_for_its_site)
{
    auto site_for = [](StringView url) {
        return Web::HTML::scripting_site_for_origin(URL::Parser::basic_parse(url).release_value().origin());
    };

    EXPECT_EQ(site_for("https://www.example.com/page"sv), "example.com"_string);
    EXPECT_EQ(site_for("http://a.b.example.co.uk:8080/"sv), "example.co.uk"_string);
    EXPECT_EQ(site_for("http://localhost:8000/"sv), "localhost"_string);
    EXPECT_EQ(site_for("http://127.0.0.1:8000/"sv), "127.0.0.1"_string);
    EXPECT(!site_for("data:text/html,page"sv).has_value());
}

TEST_CASE(rules_decide_by_the_closest_site)
{
    OrderedHashMap<String, ScriptingPolicy> site_policies;
    site_policies.set("example.com"_string, ScriptingPolicy::Block);
    site_policies.set("mail.example.com"_string, ScriptingPolicy::BlockThirdParty);
    // NB: Rules made from sites in another spelling find them all the same.
    site_policies.set("HTTPS://Other.Example/"_string, ScriptingPolicy::Block);
    site_policies.set("not a site"_string, ScriptingPolicy::Block);
    Web::HTML::ScriptingRules rules { ScriptingPolicy::Allow, site_policies };

    EXPECT(!rules.allow_everything());
    EXPECT_EQ(rules.policy_for_host("example.com"sv), ScriptingPolicy::Block);
    EXPECT_EQ(rules.policy_for_host("www.example.com"sv), ScriptingPolicy::Block);
    EXPECT_EQ(rules.policy_for_host("mail.example.com"sv), ScriptingPolicy::BlockThirdParty);
    EXPECT_EQ(rules.policy_for_host("other.example"sv), ScriptingPolicy::Block);
    EXPECT_EQ(rules.policy_for_host("example.org"sv), ScriptingPolicy::Allow);
    EXPECT_EQ(rules.policy_for_host({}), ScriptingPolicy::Allow);

    EXPECT(!rules.allows("example.com"sv, false));
    EXPECT(rules.allows("mail.example.com"sv, false));
    EXPECT(!rules.allows("mail.example.com"sv, true));
    EXPECT(rules.allows("example.org"sv, true));

    Web::HTML::ScriptingRules moved { move(rules) };
    EXPECT_EQ(moved.policy_for_host("example.com"sv), ScriptingPolicy::Block);
    rules = Web::HTML::ScriptingRules { ScriptingPolicy::Block, {} };
    EXPECT(!rules.allows("example.org"sv, false));

    EXPECT(Web::HTML::ScriptingRules {}.allow_everything());
    EXPECT(Web::HTML::ScriptingRules {}.allows("example.com"sv, true));
}

TEST_CASE(scripting_rules_survive_restart)
{
    auto path = settings_path("restart"sv);
    {
        auto settings = WebView::Settings::create(path);
        EXPECT_EQ(settings.scripting_settings().policy, ScriptingPolicy::Allow);
        EXPECT(settings.scripting_settings().site_filters.is_empty());
        EXPECT_EQ(settings.scripting_policy_for_host("example.com"sv), ScriptingPolicy::Allow);

        settings.set_scripting_policy(ScriptingPolicy::BlockThirdParty);
        EXPECT(!settings.add_scripting_site_filter("https://Example.com/path"sv, ScriptingPolicy::Block).is_error());
        EXPECT(!settings.add_scripting_site_filter("mail.example.com"sv, ScriptingPolicy::Allow).is_error());
        EXPECT(!settings.add_scripting_site_filter("other.example"sv, ScriptingPolicy::Allow).is_error());
        EXPECT(settings.add_scripting_site_filter("not a site"sv, ScriptingPolicy::Block).is_error());

        EXPECT_EQ(settings.scripting_settings().site_filters.size(), 3u);
        EXPECT_EQ(settings.scripting_settings().site_filters.get("example.com"sv), ScriptingPolicy::Block);
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT_EQ(settings.scripting_settings().policy, ScriptingPolicy::BlockThirdParty);
        EXPECT_EQ(settings.scripting_settings().site_filters.size(), 3u);
        EXPECT_EQ(settings.scripting_policy_for_host("example.com"sv), ScriptingPolicy::Block);
        EXPECT_EQ(settings.scripting_policy_for_host("www.example.com"sv), ScriptingPolicy::Block);
        EXPECT_EQ(settings.scripting_policy_for_host("mail.example.com"sv), ScriptingPolicy::Allow);
        EXPECT_EQ(settings.scripting_policy_for_host("other.example"sv), ScriptingPolicy::Allow);
        EXPECT_EQ(settings.scripting_policy_for_host("example.org"sv), ScriptingPolicy::BlockThirdParty);

        // NB: A filter is replaced and removed by any spelling of its site.
        EXPECT(!settings.add_scripting_site_filter("EXAMPLE.com"sv, ScriptingPolicy::Allow).is_error());
        settings.remove_scripting_site_filter("http://other.example:8080/"sv);
        EXPECT_EQ(settings.scripting_settings().site_filters.size(), 2u);
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT_EQ(settings.scripting_settings().site_filters.size(), 2u);
        EXPECT_EQ(settings.scripting_policy_for_host("example.com"sv), ScriptingPolicy::Allow);
        EXPECT_EQ(settings.scripting_policy_for_host("other.example"sv), ScriptingPolicy::BlockThirdParty);

        settings.remove_all_scripting_site_filters();
        settings.set_scripting_policy(ScriptingPolicy::Block);
    }
    auto settings = WebView::Settings::create(path);
    EXPECT_EQ(settings.scripting_settings().policy, ScriptingPolicy::Block);
    EXPECT(settings.scripting_settings().site_filters.is_empty());
    EXPECT_EQ(settings.scripting_policy_for_host("example.com"sv), ScriptingPolicy::Block);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(scripting_rules_are_saved_by_name)
{
    auto path = settings_path("names"sv);
    auto settings = WebView::Settings::create(path);
    settings.set_scripting_policy(ScriptingPolicy::Block);
    EXPECT(!settings.add_scripting_site_filter("example.com"sv, ScriptingPolicy::BlockThirdParty).is_error());

    auto json = settings.serialize_json();
    auto scripting = json.as_object().get_object("javascript"sv);
    VERIFY(scripting.has_value());
    EXPECT_EQ(scripting->get_string("policy"sv), "block"sv);

    auto site_filters = scripting->get_array("siteFilters"sv);
    VERIFY(site_filters.has_value());
    EXPECT_EQ(site_filters->size(), 1u);
    EXPECT_EQ(site_filters->at(0).as_object().get_string("filter"sv), "example.com"sv);
    EXPECT_EQ(site_filters->at(0).as_object().get_string("policy"sv), "block-third-party"sv);

    auto copy = WebView::Settings::create_from_json(json);
    EXPECT_EQ(copy.scripting_settings().policy, ScriptingPolicy::Block);
    EXPECT_EQ(copy.scripting_policy_for_host("www.example.com"sv), ScriptingPolicy::BlockThirdParty);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(scripting_ignores_invalid_saved_rules)
{
    auto json = MUST(JsonValue::from_string(R"({"javascript":{"policy":"sometimes","siteFilters":[
        {"filter":"example.com","policy":"block"},
        {"filter":"HTTPS://Upper.Example:8080/","policy":"block-third-party"},
        {"filter":"","policy":"block"},
        {"filter":"not a site","policy":"block"},
        {"filter":"unknown.example","policy":"sometimes"},
        {"filter":"number.example","policy":2},
        {"policy":"block"},
        "string.example",
        null
    ]}})"sv));
    auto settings = WebView::Settings::create_from_json(json);
    EXPECT_EQ(settings.scripting_settings().policy, ScriptingPolicy::Allow);
    EXPECT_EQ(settings.scripting_settings().site_filters.size(), 2u);
    EXPECT_EQ(settings.scripting_settings().site_filters.get("example.com"sv), ScriptingPolicy::Block);
    EXPECT_EQ(settings.scripting_settings().site_filters.get("upper.example"sv), ScriptingPolicy::BlockThirdParty);

    auto without_rules = WebView::Settings::create_from_json(MUST(JsonValue::from_string(R"({"javascript":"block"})"sv)));
    EXPECT_EQ(without_rules.scripting_settings().policy, ScriptingPolicy::Allow);
    EXPECT(without_rules.scripting_settings().site_filters.is_empty());
}

TEST_CASE(scripting_notifies_when_the_rules_change)
{
    auto path = settings_path("observer"sv);
    auto settings = WebView::Settings::create(path);
    struct Observer : WebView::SettingsObserver {
        explicit Observer(WebView::Settings& settings)
            : SettingsObserver(settings)
        {
        }
        virtual void scripting_settings_changed() override { ++changes; }
        size_t changes { 0 };
    } observer(settings);

    EXPECT(!settings.add_scripting_site_filter("example.com"sv, ScriptingPolicy::Block).is_error());
    EXPECT_EQ(observer.changes, 1u);
    EXPECT(settings.add_scripting_site_filter("not a site"sv, ScriptingPolicy::Block).is_error());
    settings.remove_scripting_site_filter("missing.example"sv);
    settings.remove_scripting_site_filter("not a site"sv);
    EXPECT_EQ(observer.changes, 1u);
    settings.set_scripting_policy(ScriptingPolicy::BlockThirdParty);
    EXPECT_EQ(observer.changes, 2u);
    settings.remove_scripting_site_filter("example.com"sv);
    EXPECT_EQ(observer.changes, 3u);

    // NB: Settings that are replaced as a whole, as the ones another process sends are, only tell of what differs.
    auto json = settings.serialize_json();
    settings.replace_with_json(json, WebView::Settings::PersistChange::No);
    EXPECT_EQ(observer.changes, 3u);

    auto other = WebView::Settings::create_from_json(json);
    EXPECT(!other.add_scripting_site_filter("example.org"sv, ScriptingPolicy::Allow).is_error());
    settings.replace_with_json(other.serialize_json(), WebView::Settings::PersistChange::No);
    EXPECT_EQ(observer.changes, 4u);
    EXPECT_EQ(settings.scripting_policy_for_host("example.org"sv), ScriptingPolicy::Allow);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}
