/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::FFI {

struct ScriptPolicyRules;

}

namespace Web::HTML {

// What a site may run. The values are those of the Policy enum of the script policy crate, which decides by them
// (Libraries/LibWebCommon/ScriptPolicy/Rust).
enum class ScriptingPolicy : u8 {
    // Every script runs.
    Allow = 0,
    // Scripts from the site itself run. Scripts from other sites, and scripts in frames from other sites, do not.
    BlockThirdParty = 1,
    // No script runs.
    Block = 2,
};

// The names the policies are saved under.
WEBCOMMON_API Optional<ScriptingPolicy> scripting_policy_from_string(StringView);
WEBCOMMON_API StringView scripting_policy_to_string(ScriptingPolicy);

// The spelling an exception for a site is kept in, or nothing if the text names no site. What the user typed may have
// a scheme, a port or a path, which are dropped. The site is a host name, which also stands for the hosts under it,
// or an IP address.
WEBCOMMON_API Optional<String> normalize_scripting_site(StringView);

// The site an exception made while visiting a page of the origin is for: the registrable domain of its host, or the
// host itself where it has none, as for an IP address. An opaque origin has no site.
WEBCOMMON_API Optional<String> scripting_site_for_origin(URL::Origin const&);

// The user's policy for every site and the exceptions to it, which are found by the host of the top-level document.
class WEBCOMMON_API ScriptingRules {
    AK_MAKE_NONCOPYABLE(ScriptingRules);

public:
    // Rules under which every script runs.
    ScriptingRules();
    // NB: A site policy whose site is not in the spelling of normalize_scripting_site() is normalized; one that names
    //     no site is left out.
    ScriptingRules(ScriptingPolicy default_policy, OrderedHashMap<String, ScriptingPolicy> const& site_policies);
    ScriptingRules(ScriptingRules&&);
    ScriptingRules& operator=(ScriptingRules&&);
    ~ScriptingRules();

    // Whether every script runs on every site, so that nothing has to be asked of these rules.
    bool allow_everything() const { return m_allow_everything; }

    // The policy for pages whose top-level document is from the host. The host is empty for a document from no host,
    // which no exception is for.
    ScriptingPolicy policy_for_host(StringView top_level_host) const;

    // Whether a script runs on pages whose top-level document is from the host. A third-party script is one that is
    // not from the site of that document.
    bool allows(StringView top_level_host, bool is_third_party) const;

private:
    FFI::ScriptPolicyRules* m_rules { nullptr };
    bool m_allow_everything { true };
};

}

namespace AK {

template<>
struct Formatter<Web::HTML::ScriptingPolicy> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::HTML::ScriptingPolicy policy)
    {
        return Formatter<StringView>::format(builder, Web::HTML::scripting_policy_to_string(policy));
    }
};

}
