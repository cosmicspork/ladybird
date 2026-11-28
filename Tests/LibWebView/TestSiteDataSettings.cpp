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
#include <LibWebCommon/StorageAPI/SiteDataPolicy.h>
#include <LibWebView/Settings.h>

using Web::StorageAPI::SiteDataAccess;
using Web::StorageAPI::SiteDataPolicy;

static ByteString settings_path(StringView name)
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-site-data-{}-{}.json", name, Core::System::getpid())).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    return path;
}

TEST_CASE(new_settings_allow_every_site_to_keep_cookies)
{
    auto path = settings_path("defaults"sv);
    auto settings = WebView::Settings::create(path);

    EXPECT_EQ(settings.site_data_settings().policy, SiteDataPolicy::Allow);
    EXPECT(settings.site_data_settings().site_filters.is_empty());
    EXPECT(settings.site_data_rules().allow_everything());
}

TEST_CASE(site_data_rules_survive_restart)
{
    auto path = settings_path("restart"sv);
    {
        auto settings = WebView::Settings::create(path);
        settings.set_site_data_policy(SiteDataPolicy::BlockThirdParty);
        EXPECT(!settings.add_site_data_site_filter("https://www.Example.com:8443/path"sv, SiteDataPolicy::Block).is_error());
        EXPECT(!settings.add_site_data_site_filter("shop.example"sv, SiteDataPolicy::Allow).is_error());
        EXPECT(!settings.add_site_data_site_filter("other.example"sv, SiteDataPolicy::Allow).is_error());
        EXPECT(settings.add_site_data_site_filter("not a site"sv, SiteDataPolicy::Block).is_error());
        EXPECT(settings.add_site_data_site_filter("com"sv, SiteDataPolicy::Block).is_error());

        EXPECT_EQ(settings.site_data_settings().site_filters.size(), 3u);
        EXPECT_EQ(settings.site_data_settings().site_filters.get("example.com"sv), SiteDataPolicy::Block);
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT_EQ(settings.site_data_settings().policy, SiteDataPolicy::BlockThirdParty);
        EXPECT_EQ(settings.site_data_settings().site_filters.size(), 3u);

        auto rules = settings.site_data_rules();
        EXPECT_EQ(rules.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Block);
        EXPECT_EQ(rules.policy_for_top_level_site("https://shop.example"sv), SiteDataPolicy::Allow);
        EXPECT_EQ(rules.policy_for_top_level_site("https://example.org"sv), SiteDataPolicy::BlockThirdParty);
        EXPECT_EQ(rules.cookie_access("https://example.org"sv, true), SiteDataAccess::Denied);
        EXPECT_EQ(rules.cookie_access("https://shop.example"sv, true), SiteDataAccess::Partitioned);

        // NB: A filter is replaced and removed by any spelling of its site.
        EXPECT(!settings.add_site_data_site_filter("EXAMPLE.com"sv, SiteDataPolicy::Allow).is_error());
        settings.remove_site_data_site_filter("http://www.other.example:8080/"sv);
        EXPECT_EQ(settings.site_data_settings().site_filters.size(), 2u);
    }
    {
        auto settings = WebView::Settings::create(path);
        EXPECT_EQ(settings.site_data_settings().site_filters.size(), 2u);
        EXPECT_EQ(settings.site_data_rules().policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Allow);
        EXPECT_EQ(settings.site_data_rules().policy_for_top_level_site("https://other.example"sv), SiteDataPolicy::BlockThirdParty);

        settings.remove_all_site_data_site_filters();
        settings.set_site_data_policy(SiteDataPolicy::Block);
    }
    auto settings = WebView::Settings::create(path);
    EXPECT_EQ(settings.site_data_settings().policy, SiteDataPolicy::Block);
    EXPECT(settings.site_data_settings().site_filters.is_empty());
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(site_data_rules_are_saved_by_name)
{
    auto path = settings_path("names"sv);
    auto settings = WebView::Settings::create(path);
    settings.set_site_data_policy(SiteDataPolicy::Block);
    EXPECT(!settings.add_site_data_site_filter("example.com"sv, SiteDataPolicy::BlockThirdParty).is_error());

    auto json = settings.serialize_json();
    auto site_data = json.as_object().get_object("siteData"sv);
    VERIFY(site_data.has_value());
    EXPECT_EQ(site_data->get_string("policy"sv), "block"sv);

    auto site_filters = site_data->get_array("siteFilters"sv);
    VERIFY(site_filters.has_value());
    EXPECT_EQ(site_filters->size(), 1u);
    EXPECT_EQ(site_filters->at(0).as_object().get_string("filter"sv), "example.com"sv);
    EXPECT_EQ(site_filters->at(0).as_object().get_string("policy"sv), "block-third-party"sv);

    auto copy = WebView::Settings::create_from_json(json);
    EXPECT_EQ(copy.site_data_settings().policy, SiteDataPolicy::Block);
    EXPECT_EQ(copy.site_data_rules().policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::BlockThirdParty);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}

TEST_CASE(site_data_ignores_invalid_saved_rules)
{
    auto json = MUST(JsonValue::from_string(R"({"siteData":{"policy":"sometimes","siteFilters":[
        {"filter":"example.com","policy":"block"},
        {"filter":"HTTPS://www.Upper.Example:8080/","policy":"block-third-party"},
        {"filter":"","policy":"block"},
        {"filter":"not a site","policy":"block"},
        {"filter":"com","policy":"block"},
        {"filter":"unknown.example","policy":"sometimes"},
        {"filter":"number.example","policy":2},
        {"policy":"block"},
        "string.example",
        null
    ]}})"sv));
    auto settings = WebView::Settings::create_from_json(json);
    EXPECT_EQ(settings.site_data_settings().policy, SiteDataPolicy::Allow);
    EXPECT_EQ(settings.site_data_settings().site_filters.size(), 2u);
    EXPECT_EQ(settings.site_data_settings().site_filters.get("example.com"sv), SiteDataPolicy::Block);
    EXPECT_EQ(settings.site_data_settings().site_filters.get("upper.example"sv), SiteDataPolicy::BlockThirdParty);

    auto without_rules = WebView::Settings::create_from_json(MUST(JsonValue::from_string(R"({"siteData":"block"})"sv)));
    EXPECT_EQ(without_rules.site_data_settings().policy, SiteDataPolicy::Allow);
    EXPECT(without_rules.site_data_settings().site_filters.is_empty());
}

TEST_CASE(site_data_notifies_when_the_rules_change)
{
    auto path = settings_path("observer"sv);
    auto settings = WebView::Settings::create(path);
    struct Observer : WebView::SettingsObserver {
        explicit Observer(WebView::Settings& settings)
            : SettingsObserver(settings)
        {
        }
        virtual void site_data_settings_changed() override { ++changes; }
        size_t changes { 0 };
    } observer(settings);

    EXPECT(!settings.add_site_data_site_filter("example.com"sv, SiteDataPolicy::Block).is_error());
    EXPECT_EQ(observer.changes, 1u);
    EXPECT(settings.add_site_data_site_filter("not a site"sv, SiteDataPolicy::Block).is_error());
    settings.remove_site_data_site_filter("missing.example"sv);
    settings.remove_site_data_site_filter("not a site"sv);
    EXPECT_EQ(observer.changes, 1u);
    settings.set_site_data_policy(SiteDataPolicy::BlockThirdParty);
    EXPECT_EQ(observer.changes, 2u);
    settings.remove_site_data_site_filter("example.com"sv);
    EXPECT_EQ(observer.changes, 3u);

    // NB: Settings that are replaced as a whole, as the ones another process sends are, only tell of what differs.
    auto json = settings.serialize_json();
    settings.replace_with_json(json, WebView::Settings::PersistChange::No);
    EXPECT_EQ(observer.changes, 3u);

    auto other = WebView::Settings::create_from_json(json);
    EXPECT(!other.add_site_data_site_filter("example.org"sv, SiteDataPolicy::Allow).is_error());
    settings.replace_with_json(other.serialize_json(), WebView::Settings::PersistChange::No);
    EXPECT_EQ(observer.changes, 4u);
    EXPECT_EQ(settings.site_data_rules().policy_for_top_level_site("https://example.org"sv), SiteDataPolicy::Allow);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
}
