/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utility.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/HTML/ScriptingPolicy.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::HTML {

Optional<ScriptingPolicy> scripting_policy_from_string(StringView string)
{
    if (string == "allow"sv)
        return ScriptingPolicy::Allow;
    if (string == "block-third-party"sv)
        return ScriptingPolicy::BlockThirdParty;
    if (string == "block"sv)
        return ScriptingPolicy::Block;
    return {};
}

StringView scripting_policy_to_string(ScriptingPolicy policy)
{
    switch (policy) {
    case ScriptingPolicy::Allow:
        return "allow"sv;
    case ScriptingPolicy::BlockThirdParty:
        return "block-third-party"sv;
    case ScriptingPolicy::Block:
        return "block"sv;
    }

    VERIFY_NOT_REACHED();
}

static void set_site_from_ffi(void* ctx, ReadonlyBytes site)
{
    // NB: The crate only accepts ASCII.
    *static_cast<Optional<String>*>(ctx) = String::from_ascii_without_validation(site);
}

Optional<String> normalize_scripting_site(StringView site)
{
    Optional<String> normalized;
    FFI::script_policy_normalize_site(site.bytes(), &normalized, set_site_from_ffi);
    return normalized;
}

Optional<String> scripting_site_for_origin(URL::Origin const& origin)
{
    if (origin.is_opaque())
        return {};

    auto const& host = origin.host();
    if (auto registrable_domain = host.registrable_domain(); registrable_domain.has_value())
        return normalize_scripting_site(*registrable_domain);
    return normalize_scripting_site(host.serialize());
}

ScriptingRules::ScriptingRules()
    : ScriptingRules(ScriptingPolicy::Allow, {})
{
}

ScriptingRules::ScriptingRules(ScriptingPolicy default_policy, OrderedHashMap<String, ScriptingPolicy> const& site_policies)
    : m_rules(FFI::script_policy_rules_create(to_underlying(default_policy)))
{
    for (auto const& [site, policy] : site_policies)
        FFI::script_policy_rules_set_exception(m_rules, site.bytes(), to_underlying(policy));

    m_allow_everything = FFI::script_policy_rules_allow_everything(m_rules);
}

ScriptingRules::ScriptingRules(ScriptingRules&& other)
    : m_rules(exchange(other.m_rules, nullptr))
    , m_allow_everything(exchange(other.m_allow_everything, true))
{
}

ScriptingRules& ScriptingRules::operator=(ScriptingRules&& other)
{
    if (this != &other) {
        FFI::script_policy_rules_free(m_rules);
        m_rules = exchange(other.m_rules, nullptr);
        m_allow_everything = exchange(other.m_allow_everything, true);
    }
    return *this;
}

ScriptingRules::~ScriptingRules()
{
    FFI::script_policy_rules_free(m_rules);
}

ScriptingPolicy ScriptingRules::policy_for_host(StringView top_level_host) const
{
    // NB: Rules that were moved from allow everything.
    if (!m_rules)
        return ScriptingPolicy::Allow;

    switch (FFI::script_policy_rules_policy_for_host(m_rules, top_level_host.bytes())) {
    case to_underlying(ScriptingPolicy::Allow):
        return ScriptingPolicy::Allow;
    case to_underlying(ScriptingPolicy::BlockThirdParty):
        return ScriptingPolicy::BlockThirdParty;
    case to_underlying(ScriptingPolicy::Block):
        return ScriptingPolicy::Block;
    }

    VERIFY_NOT_REACHED();
}

bool ScriptingRules::allows(StringView top_level_host, bool is_third_party) const
{
    if (m_allow_everything)
        return true;
    return FFI::script_policy_rules_allows(m_rules, top_level_host.bytes(), is_third_party);
}

}
