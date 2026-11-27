/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/RWLockProtected.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/ScriptingPolicy.h>

namespace Web::HTML {

// Holds the user's rules for which sites may run scripts, for this process: a policy for every site and the
// exceptions to it. Populated from the browser process.
//
// The site that decides is the one of the top-level document, so a frame runs scripts by the rules of the page it is
// in. Under a policy that only blocks third-party scripts, a script is third-party if it is fetched from another site
// than that of the top-level document, or if it is in a frame whose document is from another site.
//
// The rules are set on the main thread. Any thread may ask them, as a worker's thread does when it fetches a script.
class WEB_API ScriptingSettings {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ScriptingSettings& the();

    void set_policy(ScriptingPolicy, OrderedHashMap<String, ScriptingPolicy> site_policies);

    // Changes one exception, or removes it when no policy is given, and keeps the rest of the rules.
    void set_site_policy_for_testing(String const& site, Optional<ScriptingPolicy>);

    // Whether the user lets scripts run in the document, which is that of the Window of the settings object.
    bool allows_scripting_in(DOM::Document const&, EnvironmentSettingsObject const&) const;

    // Whether the user lets the request go ahead. Only a request for a script is ever held back.
    bool allows_request(Fetch::Infrastructure::Request const&) const;

private:
    ScriptingSettings();
    ~ScriptingSettings();

    bool allows(URL::Origin const& top_level_origin, URL::Origin const& script_origin) const;

    struct Rules {
        ScriptingPolicy policy { ScriptingPolicy::Allow };
        OrderedHashMap<String, ScriptingPolicy> site_policies;
        ScriptingRules rules;
    };
    void did_change_rules(Rules&);

    RWLockProtected<Rules> m_rules;

    // Whether every script runs on every site, which is so until the user says otherwise.
    Atomic<bool> m_allow_everything { true };

    // Counts the changes of the rules, so that a thread can tell that the decisions it remembers are out of date.
    Atomic<u64> m_generation { 0 };
};

}
