/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::FFI {

struct SiteDataPolicyRules;

}

namespace Web::StorageAPI {

// What the pages of a site may keep: cookies and, in time, the other data a site stores in the browser. The values are
// those of the Policy enum of the site data policy crate, which decides by them
// (Libraries/LibWebCommon/SiteDataPolicy/Rust).
enum class SiteDataPolicy : u8 {
    // Every context may keep site data. A third party keeps it apart for each top-level site.
    Allow = 0,
    // The site itself may keep site data. Content from other sites, and frames from other sites, may not.
    BlockThirdParty = 1,
    // Nothing may keep site data.
    Block = 2,
};

// Whether a context may use site data, and which. The values are those of the Access enum of the crate.
enum class SiteDataAccess : u8 {
    // The context reads nothing and what it writes is dropped.
    Denied = 0,
    // The context uses the data its site keeps as a first party.
    FirstParty = 1,
    // The context uses the data its site keeps under the top-level site, and no other.
    Partitioned = 2,
};

// The names the policies are saved under.
WEBCOMMON_API Optional<SiteDataPolicy> site_data_policy_from_string(StringView);
WEBCOMMON_API StringView site_data_policy_to_string(SiteDataPolicy);

// The site an exception made for the pages of the origin is for: the registrable domain of its host, or the host
// itself where it has none, as for an IP address. An opaque origin has no site, and a public suffix such as "com" is
// not one.
WEBCOMMON_API Optional<String> site_data_site_for_origin(URL::Origin const&);

// The site an exception is kept for, from what a user typed, or nothing if the text names no site. The text may be a
// host name or a URL; its scheme, port and path are dropped, and so are the labels below the registrable domain.
WEBCOMMON_API Optional<String> normalize_site_data_site(StringView);

// The user's policy for every site and the exceptions to it, which are found by the site of the top-level document.
class WEBCOMMON_API SiteDataRules {
public:
    // Rules under which every site may keep site data.
    SiteDataRules();
    // NB: A site policy whose site is not in the spelling of normalize_site_data_site() is left out.
    SiteDataRules(SiteDataPolicy default_policy, OrderedHashMap<String, SiteDataPolicy> site_policies);
    SiteDataRules(SiteDataRules const&);
    SiteDataRules(SiteDataRules&&);
    SiteDataRules& operator=(SiteDataRules const&);
    SiteDataRules& operator=(SiteDataRules&&);
    ~SiteDataRules();

    SiteDataPolicy default_policy() const { return m_default_policy; }
    OrderedHashMap<String, SiteDataPolicy> const& site_policies() const { return m_site_policies; }

    // Whether the rules decide as SiteDataPolicy::Allow does for every site, so that nothing has to be asked of them.
    bool allow_everything() const { return m_allow_everything; }

    // The policy for pages whose top-level document is of the site. The site is serialized as
    // URL::Site::serialize_for_partitioning() does, and is empty for a top-level document of no site, which no
    // exception is for.
    SiteDataPolicy policy_for_top_level_site(StringView top_level_site) const;

    // The cookies a context uses on pages whose top-level document is of the site. A third-party context is one that
    // is not of the site of that document, or that has an ancestor of another site.
    SiteDataAccess cookie_access(StringView top_level_site, bool is_third_party) const;

private:
    void rebuild();

    SiteDataPolicy m_default_policy { SiteDataPolicy::Allow };
    OrderedHashMap<String, SiteDataPolicy> m_site_policies;

    FFI::SiteDataPolicyRules* m_rules { nullptr };
    bool m_allow_everything { true };
};

}

namespace AK {

template<>
struct Formatter<Web::StorageAPI::SiteDataPolicy> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::StorageAPI::SiteDataPolicy policy)
    {
        return Formatter<StringView>::format(builder, Web::StorageAPI::site_data_policy_to_string(policy));
    }
};

template<>
struct Formatter<Web::StorageAPI::SiteDataAccess> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::StorageAPI::SiteDataAccess access)
    {
        switch (access) {
        case Web::StorageAPI::SiteDataAccess::Denied:
            return Formatter<StringView>::format(builder, "denied"sv);
        case Web::StorageAPI::SiteDataAccess::FirstParty:
            return Formatter<StringView>::format(builder, "first-party"sv);
        case Web::StorageAPI::SiteDataAccess::Partitioned:
            return Formatter<StringView>::format(builder, "partitioned"sv);
        }
        VERIFY_NOT_REACHED();
    }
};

}
