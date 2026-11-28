/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::policy::{Access, Party, Policy};
use crate::site::{SiteHost, host_of_serialized_site};

/// The user's policy for every site and the exceptions to it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rules {
    default_policy: Policy,
    exceptions: Vec<(SiteHost, Policy)>,
}

impl Rules {
    /// Rules without exceptions.
    pub fn new(default_policy: Policy) -> Self {
        Self {
            default_policy,
            exceptions: Vec::new(),
        }
    }

    pub fn default_policy(&self) -> Policy {
        self.default_policy
    }

    /// Makes an exception for the pages of a site, in place of any exception the site already has.
    pub fn set_exception(&mut self, site: SiteHost, policy: Policy) {
        if let Some(exception) = self.exceptions.iter_mut().find(|(existing, _)| *existing == site) {
            exception.1 = policy;
        } else {
            self.exceptions.push((site, policy));
        }
    }

    /// Removes the exception for a site, and returns whether it had one.
    pub fn remove_exception(&mut self, site: &SiteHost) -> bool {
        let count = self.exceptions.len();
        self.exceptions.retain(|(existing, _)| existing != site);
        self.exceptions.len() != count
    }

    /// Whether these rules decide as `Policy::Allow` does for every site, so that nothing has to be asked of them.
    pub fn allow_everything(&self) -> bool {
        self.default_policy == Policy::Allow && self.exceptions.iter().all(|(_, policy)| *policy == Policy::Allow)
    }

    /// The policy for pages whose top-level document is of the site, which is serialized as
    /// [`host_of_serialized_site`] describes.
    ///
    /// A site without a host has no exception. That is the site of file: URLs, and the empty text that stands for a
    /// top-level document of no site.
    pub fn policy_for_top_level_site(&self, top_level_site: &str) -> Policy {
        let Some(host) = host_of_serialized_site(top_level_site) else {
            return self.default_policy;
        };
        self.exceptions
            .iter()
            .filter(|(site, _)| site.covers(host))
            .max_by_key(|(site, _)| site.specificity())
            .map_or(self.default_policy, |(_, policy)| *policy)
    }

    /// The cookies a context uses on pages whose top-level document is of the site.
    pub fn cookie_access(&self, top_level_site: &str, party: Party) -> Access {
        self.policy_for_top_level_site(top_level_site).cookie_access(party)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn site(text: &str) -> SiteHost {
        SiteHost::parse(text).unwrap()
    }

    fn rules(default_policy: Policy, exceptions: &[(&str, Policy)]) -> Rules {
        let mut rules = Rules::new(default_policy);
        for (text, policy) in exceptions {
            rules.set_exception(site(text), *policy);
        }
        rules
    }

    #[test]
    fn rules_without_exceptions_use_the_default_policy() {
        for policy in [Policy::Allow, Policy::BlockThirdParty, Policy::Block] {
            let rules = Rules::new(policy);
            assert_eq!(rules.default_policy(), policy);
            assert_eq!(rules.policy_for_top_level_site("https://example.com"), policy);
            assert_eq!(rules.policy_for_top_level_site("file://"), policy);
            assert_eq!(rules.policy_for_top_level_site(""), policy);
        }
    }

    #[test]
    fn an_exception_replaces_the_default_policy_for_its_site() {
        let rules = rules(Policy::Block, &[("example.com", Policy::Allow)]);
        assert_eq!(rules.policy_for_top_level_site("https://example.com"), Policy::Allow);
        assert_eq!(rules.policy_for_top_level_site("https://example.org"), Policy::Block);
    }

    #[test]
    fn an_exception_is_for_every_scheme_of_its_site() {
        let rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        assert_eq!(rules.policy_for_top_level_site("http://example.com"), Policy::Block);
        assert_eq!(rules.policy_for_top_level_site("https://example.com"), Policy::Block);
    }

    #[test]
    fn the_closest_exception_wins() {
        let rules = rules(
            Policy::Allow,
            &[("corp", Policy::Block), ("intranet.corp", Policy::BlockThirdParty)],
        );
        assert_eq!(
            rules.policy_for_top_level_site("https://intranet.corp"),
            Policy::BlockThirdParty
        );
        assert_eq!(
            rules.policy_for_top_level_site("https://wiki.intranet.corp"),
            Policy::BlockThirdParty
        );
        assert_eq!(rules.policy_for_top_level_site("https://mail.corp"), Policy::Block);

        // NB: The order the exceptions were made in does not matter.
        let reversed = super::Rules {
            default_policy: Policy::Allow,
            exceptions: rules.exceptions.iter().rev().cloned().collect(),
        };
        assert_eq!(
            reversed.policy_for_top_level_site("https://wiki.intranet.corp"),
            Policy::BlockThirdParty
        );
    }

    #[test]
    fn a_site_without_a_host_has_no_exception() {
        let rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        assert_eq!(rules.policy_for_top_level_site("file://"), Policy::Allow);
        assert_eq!(rules.policy_for_top_level_site(""), Policy::Allow);
        assert_eq!(rules.policy_for_top_level_site("example.com"), Policy::Allow);
        assert_eq!(
            rules.policy_for_top_level_site("https://example.com:8080"),
            Policy::Allow
        );
    }

    #[test]
    fn setting_an_exception_again_replaces_it() {
        let mut rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        rules.set_exception(site("example.com"), Policy::BlockThirdParty);
        assert_eq!(
            rules.policy_for_top_level_site("https://example.com"),
            Policy::BlockThirdParty
        );
        assert_eq!(rules.exceptions.len(), 1);
    }

    #[test]
    fn removing_an_exception_restores_the_default_policy() {
        let mut rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        assert!(rules.remove_exception(&site("example.com")));
        assert!(!rules.remove_exception(&site("example.com")));
        assert_eq!(rules.policy_for_top_level_site("https://example.com"), Policy::Allow);
    }

    #[test]
    fn only_rules_that_allow_everywhere_allow_everything() {
        assert!(Rules::new(Policy::Allow).allow_everything());
        assert!(rules(Policy::Allow, &[("example.com", Policy::Allow)]).allow_everything());
        assert!(!Rules::new(Policy::BlockThirdParty).allow_everything());
        assert!(!Rules::new(Policy::Block).allow_everything());
        assert!(!rules(Policy::Allow, &[("example.com", Policy::BlockThirdParty)]).allow_everything());
        assert!(!rules(Policy::Block, &[("example.com", Policy::Allow)]).allow_everything());
    }

    #[test]
    fn cookie_access_follows_the_policy_of_the_top_level_site() {
        let rules = rules(
            Policy::BlockThirdParty,
            &[("shop.example", Policy::Allow), ("tracker.example", Policy::Block)],
        );

        assert_eq!(
            rules.cookie_access("https://news.example", Party::First),
            Access::FirstParty
        );
        assert_eq!(
            rules.cookie_access("https://news.example", Party::Third),
            Access::Denied
        );

        assert_eq!(
            rules.cookie_access("https://shop.example", Party::First),
            Access::FirstParty
        );
        assert_eq!(
            rules.cookie_access("https://shop.example", Party::Third),
            Access::Partitioned
        );

        assert_eq!(
            rules.cookie_access("https://tracker.example", Party::First),
            Access::Denied
        );
        assert_eq!(
            rules.cookie_access("https://tracker.example", Party::Third),
            Access::Denied
        );
    }

    #[test]
    fn an_exception_for_an_embedded_site_does_not_apply_under_another_top_level_site() {
        // The exception for tracker.example is for the pages it shows, not for its frames on other sites.
        let rules = rules(Policy::Allow, &[("tracker.example", Policy::Block)]);
        assert_eq!(
            rules.cookie_access("https://news.example", Party::Third),
            Access::Partitioned
        );
    }
}
