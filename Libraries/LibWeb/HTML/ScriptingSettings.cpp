/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    m_policy = policy;
    m_site_policies = move(site_policies);
    rebuild_rules();
}

void ScriptingSettings::set_site_policy_for_testing(String const& site, Optional<ScriptingPolicy> policy)
{
    if (policy.has_value())
        m_site_policies.set(site, *policy);
    else
        m_site_policies.remove(site);
    rebuild_rules();
}

void ScriptingSettings::rebuild_rules()
{
    m_rules = ScriptingRules { m_policy, m_site_policies };
    m_recent_decisions.clear();
    m_next_recent_decision = 0;
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
    for (auto const& decision : m_recent_decisions) {
        if (decision.top_level_origin.is_same_origin(top_level_origin) && decision.script_origin.is_same_origin(script_origin))
            return decision.allowed;
    }

    // NB: An opaque origin has no host, and so no exception of its own.
    auto top_level_host = top_level_origin.is_opaque() ? String {} : top_level_origin.host().serialize();

    // NB: Telling the parties apart takes a search of the public suffix list, so it is only done where the party
    //     decides.
    bool allowed = false;
    if (m_rules.allows(top_level_host, true))
        allowed = true;
    else if (m_rules.allows(top_level_host, false))
        allowed = !is_third_party(top_level_origin, script_origin);

    Decision decision { top_level_origin, script_origin, allowed };
    if (m_recent_decisions.size() < MAX_RECENT_DECISIONS) {
        m_recent_decisions.append(move(decision));
    } else {
        m_recent_decisions[m_next_recent_decision] = move(decision);
        m_next_recent_decision = (m_next_recent_decision + 1) % MAX_RECENT_DECISIONS;
    }
    return allowed;
}

bool ScriptingSettings::allows_scripting_in(DOM::Document const& document, EnvironmentSettingsObject const& settings) const
{
    if (m_rules.allow_everything())
        return true;

    // NB: WebUI pages are internal pages requiring javascript, so we do not consider user configuration for these.
    if (URL::is_webui_url(document.url()))
        return true;

    // NB: A document is in a page of its own site until its environment says which page it is in.
    auto const& origin = document.origin();
    auto const& top_level_origin = settings.top_level_origin.has_value() ? *settings.top_level_origin : origin;
    return allows(top_level_origin, origin);
}

bool ScriptingSettings::allows_request(Fetch::Infrastructure::Request const& request) const
{
    if (m_rules.allow_everything())
        return true;
    if (!request.destination_is_script_like())
        return true;

    auto const& client = request.client_snapshot();
    if (!client || !client->top_level_origin.has_value())
        return true;
    if (client->top_level_creation_url.has_value() && URL::is_webui_url(*client->top_level_creation_url))
        return true;

    // NB: A local URL, such as a blob: or data: URL, holds a script that the document asking for it made or wrote
    //     out, so the script is from where that document is from.
    auto const& url = request.current_url();
    if (Fetch::Infrastructure::is_local_url(url))
        return allows(*client->top_level_origin, client->origin);
    return allows(*client->top_level_origin, url.origin());
}

}
