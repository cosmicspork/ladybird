/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Diagnostics.h>
#include <AK/Vector.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/ScriptingSettings.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::HTML {

ScriptingSettings& ScriptingSettings::the()
{
    static auto& settings = *new ScriptingSettings;
    return settings;
}

ScriptingSettings::ScriptingSettings() = default;
ScriptingSettings::~ScriptingSettings() = default;

void ScriptingSettings::set_policy(ScriptingPolicy policy, OrderedHashMap<String, ScriptingPolicy> site_policies)
{
    m_rules.with_write_locked([&](Rules& rules) {
        rules.policy = policy;
        rules.site_policies = move(site_policies);
        did_change_rules(rules);
    });
}

void ScriptingSettings::set_site_policy_for_testing(String const& site, Optional<ScriptingPolicy> policy)
{
    m_rules.with_write_locked([&](Rules& rules) {
        if (policy.has_value())
            rules.site_policies.set(site, *policy);
        else
            rules.site_policies.remove(site);
        did_change_rules(rules);
    });
}

void ScriptingSettings::did_change_rules(Rules& rules)
{
    rules.rules = ScriptingRules { rules.policy, rules.site_policies };
    m_allow_everything.store(rules.rules.allow_everything());
    m_generation.fetch_add(1);
}

// The last few decisions of a thread, since the same few documents ask before each script and event handler they run.
struct RecentDecisions {
    struct Decision {
        URL::Origin top_level_origin;
        URL::Origin script_origin;
        bool allowed { false };
    };

    static constexpr size_t MAX_DECISIONS = 8;

    u64 generation { 0 };
    Vector<Decision, MAX_DECISIONS> decisions;
    size_t next_decision { 0 };
};

static RecentDecisions& recent_decisions()
{
    // NB: Free what this thread kept when it exits.
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local RecentDecisions decisions)
#else
    static thread_local RecentDecisions decisions;
#endif
    return decisions;
}

// Whether a script from the origin is one from another site than the top-level document's. Exceptions are for hosts
// whatever the scheme, so sites are compared without their schemes too.
static bool is_third_party(URL::Origin const& top_level_origin, URL::Origin const& script_origin)
{
    if (top_level_origin.is_same_origin(script_origin))
        return false;

    // NB: Like other browsers, we treat file: URLs, whose origins are otherwise opaque, as one site.
    if (top_level_origin.is_opaque_file_origin() && script_origin.is_opaque_file_origin())
        return false;

    return !top_level_origin.is_schemelessly_same_site(script_origin);
}

bool ScriptingSettings::allows(URL::Origin const& top_level_origin, URL::Origin const& script_origin) const
{
    // NB: The generation is read before the rules are, so a decision by rules that changed since is not kept for long.
    auto& recent = recent_decisions();
    if (auto generation = m_generation.load(); recent.generation != generation) {
        recent.generation = generation;
        recent.decisions.clear();
        recent.next_decision = 0;
    }

    for (auto const& decision : recent.decisions) {
        if (decision.top_level_origin.is_same_origin(top_level_origin) && decision.script_origin.is_same_origin(script_origin))
            return decision.allowed;
    }

    // NB: An opaque origin has no host, and so no exception of its own.
    auto top_level_host = top_level_origin.is_opaque() ? String {} : top_level_origin.host().serialize();

    enum class Allowed : u8 {
        Everything,
        FirstParty,
        Nothing,
    };
    auto allowed_by_rules = m_rules.with_read_locked([&](Rules const& rules) {
        if (rules.rules.allows(top_level_host, true))
            return Allowed::Everything;
        if (rules.rules.allows(top_level_host, false))
            return Allowed::FirstParty;
        return Allowed::Nothing;
    });

    // NB: Telling the parties apart takes a search of the public suffix list, so it is only done where the party
    //     decides.
    bool allowed = allowed_by_rules == Allowed::Everything
        || (allowed_by_rules == Allowed::FirstParty && !is_third_party(top_level_origin, script_origin));

    RecentDecisions::Decision decision { top_level_origin, script_origin, allowed };
    if (recent.decisions.size() < RecentDecisions::MAX_DECISIONS) {
        recent.decisions.append(move(decision));
    } else {
        recent.decisions[recent.next_decision] = move(decision);
        recent.next_decision = (recent.next_decision + 1) % RecentDecisions::MAX_DECISIONS;
    }
    return allowed;
}

// NB: A WebUI page keeps its place in its fragment, as about:settings does for its tabs and dialogs, and is the same
//     page whatever the fragment.
static bool is_webui_page(URL::URL const& url)
{
    if (!url.fragment().has_value())
        return URL::is_webui_url(url);

    auto url_without_fragment = url;
    url_without_fragment.set_fragment({});
    return URL::is_webui_url(url_without_fragment);
}

bool ScriptingSettings::allows_scripting_in(DOM::Document const& document, EnvironmentSettingsObject const& settings) const
{
    if (m_allow_everything.load())
        return true;

    // NB: WebUI pages are internal pages requiring javascript, so we do not consider user configuration for these.
    if (is_webui_page(document.url()))
        return true;

    // NB: A document is in a page of its own site until its environment says which page it is in.
    auto const& origin = document.origin();
    auto const& top_level_origin = settings.top_level_origin.has_value() ? *settings.top_level_origin : origin;
    return allows(top_level_origin, origin);
}

bool ScriptingSettings::allows_request(Fetch::Infrastructure::Request const& request) const
{
    if (m_allow_everything.load())
        return true;
    if (!request.destination_is_script_like())
        return true;

    auto const& client = request.client_snapshot();
    if (!client || !client->top_level_origin.has_value())
        return true;
    if (client->top_level_creation_url.has_value() && is_webui_page(*client->top_level_creation_url))
        return true;

    // NB: A local URL, such as a blob: or data: URL, holds a script that the document asking for it made or wrote
    //     out, so the script is from where that document is from.
    auto const& url = request.current_url();
    if (Fetch::Infrastructure::is_local_url(url))
        return allows(*client->top_level_origin, client->origin);
    return allows(*client->top_level_origin, url.origin());
}

}
