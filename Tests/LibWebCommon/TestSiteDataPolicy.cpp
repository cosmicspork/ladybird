/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/StorageAPI/SiteDataPolicy.h>

using Web::StorageAPI::normalize_site_data_site;
using Web::StorageAPI::SiteDataAccess;
using Web::StorageAPI::SiteDataPolicy;
using Web::StorageAPI::SiteDataRules;

TEST_CASE(policies_round_trip_through_their_names)
{
    for (auto policy : { SiteDataPolicy::Allow, SiteDataPolicy::BlockThirdParty, SiteDataPolicy::Block })
        EXPECT_EQ(Web::StorageAPI::site_data_policy_from_string(Web::StorageAPI::site_data_policy_to_string(policy)), policy);

    EXPECT(!Web::StorageAPI::site_data_policy_from_string("Allow"sv).has_value());
    EXPECT(!Web::StorageAPI::site_data_policy_from_string(""sv).has_value());
    EXPECT(!Web::StorageAPI::site_data_policy_from_string("ask"sv).has_value());
}

TEST_CASE(a_site_is_the_registrable_domain_of_what_the_user_typed)
{
    EXPECT_EQ(normalize_site_data_site("example.com"sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("  Example.COM  "sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("www.example.com"sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("https://www.example.com:8443/path?query#fragment"sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("http://example.com"sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("example.com:8080"sv), "example.com"_string);
    EXPECT_EQ(normalize_site_data_site("a.b.example.co.uk"sv), "example.co.uk"_string);
    EXPECT_EQ(normalize_site_data_site("bücher.example"sv), "xn--bcher-kva.example"_string);
}

TEST_CASE(a_host_without_a_registrable_domain_is_a_site)
{
    EXPECT_EQ(normalize_site_data_site("localhost"sv), "localhost"_string);
    EXPECT_EQ(normalize_site_data_site("localhost:8080"sv), "localhost"_string);
    EXPECT_EQ(normalize_site_data_site("http://127.0.0.1:8000/"sv), "127.0.0.1"_string);
    EXPECT_EQ(normalize_site_data_site("[::1]"sv), "[::1]"_string);
}

TEST_CASE(text_that_names_no_site_is_refused)
{
    EXPECT(!normalize_site_data_site(""sv).has_value());
    EXPECT(!normalize_site_data_site("   "sv).has_value());
    EXPECT(!normalize_site_data_site("exa mple.com"sv).has_value());
    EXPECT(!normalize_site_data_site("about:blank"sv).has_value());
    EXPECT(!normalize_site_data_site("data:text/plain,example.com"sv).has_value());
    EXPECT(!normalize_site_data_site("file:///home/user/page.html"sv).has_value());
}

TEST_CASE(a_public_suffix_is_not_a_site)
{
    // An exception also covers the sites below it, so one for "com" would be one for most of the web.
    EXPECT(!normalize_site_data_site("com"sv).has_value());
    EXPECT(!normalize_site_data_site("co.uk"sv).has_value());
    EXPECT(!normalize_site_data_site("https://com/"sv).has_value());
}

TEST_CASE(an_origin_has_the_site_of_its_host)
{
    auto origin = [](StringView url) { return URL::Parser::basic_parse(url)->origin(); };
    EXPECT_EQ(Web::StorageAPI::site_data_site_for_origin(origin("https://www.example.com:8443/"sv)), "example.com"_string);
    EXPECT_EQ(Web::StorageAPI::site_data_site_for_origin(origin("http://localhost:8080/"sv)), "localhost"_string);
    EXPECT(!Web::StorageAPI::site_data_site_for_origin(origin("data:text/plain,hello"sv)).has_value());
    EXPECT(!Web::StorageAPI::site_data_site_for_origin(URL::Origin::create_opaque()).has_value());
}

TEST_CASE(default_rules_allow_everything)
{
    SiteDataRules rules;
    EXPECT(rules.allow_everything());
    EXPECT_EQ(rules.default_policy(), SiteDataPolicy::Allow);
    EXPECT_EQ(rules.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Allow);
    EXPECT_EQ(rules.cookie_access("https://example.com"sv, false), SiteDataAccess::FirstParty);
    EXPECT_EQ(rules.cookie_access("https://example.com"sv, true), SiteDataAccess::Partitioned);
}

TEST_CASE(rules_decide_by_the_top_level_site)
{
    OrderedHashMap<String, SiteDataPolicy> exceptions;
    exceptions.set("shop.example"_string, SiteDataPolicy::Allow);
    exceptions.set("tracker.example"_string, SiteDataPolicy::Block);
    SiteDataRules rules { SiteDataPolicy::BlockThirdParty, move(exceptions) };

    EXPECT(!rules.allow_everything());
    EXPECT_EQ(rules.policy_for_top_level_site("https://news.example"sv), SiteDataPolicy::BlockThirdParty);
    EXPECT_EQ(rules.policy_for_top_level_site("https://shop.example"sv), SiteDataPolicy::Allow);
    EXPECT_EQ(rules.policy_for_top_level_site("http://shop.example"sv), SiteDataPolicy::Allow);
    EXPECT_EQ(rules.policy_for_top_level_site("https://tracker.example"sv), SiteDataPolicy::Block);
    EXPECT_EQ(rules.policy_for_top_level_site("file://"sv), SiteDataPolicy::BlockThirdParty);
    EXPECT_EQ(rules.policy_for_top_level_site(""sv), SiteDataPolicy::BlockThirdParty);

    EXPECT_EQ(rules.cookie_access("https://news.example"sv, false), SiteDataAccess::FirstParty);
    EXPECT_EQ(rules.cookie_access("https://news.example"sv, true), SiteDataAccess::Denied);
    EXPECT_EQ(rules.cookie_access("https://shop.example"sv, true), SiteDataAccess::Partitioned);
    EXPECT_EQ(rules.cookie_access("https://tracker.example"sv, false), SiteDataAccess::Denied);
}

TEST_CASE(rules_drop_exceptions_that_name_no_site)
{
    OrderedHashMap<String, SiteDataPolicy> exceptions;
    exceptions.set("example.com"_string, SiteDataPolicy::Block);
    exceptions.set("https://example.org"_string, SiteDataPolicy::Block);
    exceptions.set("*"_string, SiteDataPolicy::Block);
    exceptions.set(String {}, SiteDataPolicy::Block);
    SiteDataRules rules { SiteDataPolicy::Allow, move(exceptions) };

    EXPECT_EQ(rules.site_policies().size(), 1uz);
    EXPECT_EQ(rules.site_policies().get("example.com"sv), SiteDataPolicy::Block);
    EXPECT_EQ(rules.policy_for_top_level_site("https://example.org"sv), SiteDataPolicy::Allow);
}

TEST_CASE(rules_can_be_copied_and_moved)
{
    OrderedHashMap<String, SiteDataPolicy> exceptions;
    exceptions.set("example.com"_string, SiteDataPolicy::Block);
    SiteDataRules rules { SiteDataPolicy::Allow, move(exceptions) };

    SiteDataRules copy { rules };
    EXPECT_EQ(copy.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Block);
    EXPECT_EQ(rules.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Block);

    SiteDataRules moved { move(rules) };
    EXPECT_EQ(moved.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Block);
    // NB: Rules that were moved from allow everything.
    EXPECT(rules.allow_everything()); // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(rules.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Allow);

    SiteDataRules assigned;
    assigned = copy;
    EXPECT_EQ(assigned.policy_for_top_level_site("https://example.com"sv), SiteDataPolicy::Block);
    assigned = SiteDataRules { SiteDataPolicy::Block, {} };
    EXPECT_EQ(assigned.policy_for_top_level_site("https://example.org"sv), SiteDataPolicy::Block);
    EXPECT(assigned.site_policies().is_empty());
}
