/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibURL/URL.h>
#include <LibWebCommon/RustFFI.h>
#include <LibWebCommon/StorageAPI/SiteDataPolicy.h>

namespace Web::StorageAPI {

Optional<SiteDataPolicy> site_data_policy_from_string(StringView string)
{
    if (string == "allow"sv)
        return SiteDataPolicy::Allow;
    if (string == "block-third-party"sv)
        return SiteDataPolicy::BlockThirdParty;
    if (string == "block"sv)
        return SiteDataPolicy::Block;
    return {};
}

StringView site_data_policy_to_string(SiteDataPolicy policy)
{
    switch (policy) {
    case SiteDataPolicy::Allow:
        return "allow"sv;
    case SiteDataPolicy::BlockThirdParty:
        return "block-third-party"sv;
    case SiteDataPolicy::Block:
        return "block"sv;
    }

    VERIFY_NOT_REACHED();
}

Optional<String> site_data_site_for_origin(URL::Origin const& origin)
{
    if (origin.is_opaque())
        return {};

    auto const& host = origin.host();
    auto site = host.registrable_domain().value_or_lazy_evaluated([&] { return host.serialize(); });

    // NB: An exception also covers the sites below it, so one for a public suffix would be one for every site that
    //     ends in it. A name that is only a suffix by the rule for unlisted top-level domains, such as "localhost" or
    //     that of an intranet, is a site.
    if (URL::PublicSuffixData::is_matching_public_suffix(site, URL::PublicSuffixData::IncludeStarRule::No))
        return {};

    if (!FFI::site_data_policy_is_site_host(site.bytes()))
        return {};
    return site;
}

Optional<String> normalize_site_data_site(StringView input)
{
    input = input.trim_whitespace();
    if (input.is_empty())
        return {};

    // NB: A host name with a port, such as "localhost:8080", would parse as a URL with the scheme "localhost".
    Optional<URL::URL> url;
    if (input.contains("://"sv))
        url = URL::Parser::basic_parse(input);
    else {
        auto with_scheme = MUST(String::formatted("https://{}", input));
        url = URL::Parser::basic_parse(with_scheme.bytes_as_string_view());
    }

    if (!url.has_value())
        return {};
    return site_data_site_for_origin(url->origin());
}

SiteDataRules::SiteDataRules()
{
    rebuild();
}

SiteDataRules::SiteDataRules(SiteDataPolicy default_policy, OrderedHashMap<String, SiteDataPolicy> site_policies)
    : m_default_policy(default_policy)
    , m_site_policies(move(site_policies))
{
    rebuild();
}

SiteDataRules::SiteDataRules(SiteDataRules const& other)
    : m_default_policy(other.m_default_policy)
    , m_site_policies(other.m_site_policies)
{
    rebuild();
}

SiteDataRules::SiteDataRules(SiteDataRules&& other)
    : m_default_policy(other.m_default_policy)
    , m_site_policies(move(other.m_site_policies))
    , m_rules(exchange(other.m_rules, nullptr))
    , m_allow_everything(other.m_allow_everything)
{
    // NB: Rules that were moved from are those of the default constructor.
    other = SiteDataRules {};
}

SiteDataRules& SiteDataRules::operator=(SiteDataRules const& other)
{
    if (this != &other) {
        m_default_policy = other.m_default_policy;
        m_site_policies = other.m_site_policies;
        rebuild();
    }
    return *this;
}

SiteDataRules& SiteDataRules::operator=(SiteDataRules&& other)
{
    if (this != &other) {
        swap(m_default_policy, other.m_default_policy);
        swap(m_site_policies, other.m_site_policies);
        swap(m_rules, other.m_rules);
        swap(m_allow_everything, other.m_allow_everything);
    }
    return *this;
}

SiteDataRules::~SiteDataRules()
{
    FFI::site_data_policy_rules_free(m_rules);
}

void SiteDataRules::rebuild()
{
    FFI::site_data_policy_rules_free(m_rules);
    m_rules = FFI::site_data_policy_rules_create(to_underlying(m_default_policy));

    Vector<String> sites_that_are_none;
    for (auto const& [site, policy] : m_site_policies) {
        if (!FFI::site_data_policy_rules_set_exception(m_rules, site.bytes(), to_underlying(policy)))
            sites_that_are_none.append(site);
    }
    for (auto const& site : sites_that_are_none)
        m_site_policies.remove(site);

    m_allow_everything = FFI::site_data_policy_rules_allow_everything(m_rules);
}

SiteDataPolicy SiteDataRules::policy_for_top_level_site(StringView top_level_site) const
{
    switch (FFI::site_data_policy_rules_policy_for_top_level_site(m_rules, top_level_site.bytes())) {
    case to_underlying(SiteDataPolicy::Allow):
        return SiteDataPolicy::Allow;
    case to_underlying(SiteDataPolicy::BlockThirdParty):
        return SiteDataPolicy::BlockThirdParty;
    case to_underlying(SiteDataPolicy::Block):
        return SiteDataPolicy::Block;
    }

    VERIFY_NOT_REACHED();
}

SiteDataAccess SiteDataRules::cookie_access(StringView top_level_site, bool is_third_party) const
{
    switch (FFI::site_data_policy_rules_cookie_access(m_rules, top_level_site.bytes(), is_third_party)) {
    case to_underlying(SiteDataAccess::Denied):
        return SiteDataAccess::Denied;
    case to_underlying(SiteDataAccess::FirstParty):
        return SiteDataAccess::FirstParty;
    case to_underlying(SiteDataAccess::Partitioned):
        return SiteDataAccess::Partitioned;
    }

    VERIFY_NOT_REACHED();
}

}
