/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::policy::{Party, Policy};
use crate::site_pattern::{SitePattern, normalize_host};

/// The policy for every site, and the exceptions to it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rules {
    default_policy: Policy,
    exceptions: Vec<(SitePattern, Policy)>,
}

impl Rules {
    pub fn new(default_policy: Policy) -> Self {
        Self {
            default_policy,
            exceptions: Vec::new(),
        }
    }

    pub fn default_policy(&self) -> Policy {
        self.default_policy
    }

    /// The exceptions, in the order they were first made.
    pub fn exceptions(&self) -> impl Iterator<Item = (&SitePattern, Policy)> {
        self.exceptions.iter().map(|(pattern, policy)| (pattern, *policy))
    }

    /// Makes an exception for the site, in place of the one it already has, if any.
    pub fn set_exception(&mut self, pattern: SitePattern, policy: Policy) {
        match self.exceptions.iter_mut().find(|(existing, _)| *existing == pattern) {
            Some(exception) => exception.1 = policy,
            None => self.exceptions.push((pattern, policy)),
        }
    }

    /// Whether every script runs on every site, so that nothing has to be asked of these rules.
    pub fn allow_everything(&self) -> bool {
        self.default_policy == Policy::Allow && self.exceptions.iter().all(|(_, policy)| *policy == Policy::Allow)
    }

    /// The policy for pages whose top-level document is from the host. The exception that matches the host most
    /// closely decides, so that one for `mail.example.com` wins over one for `example.com`. A host that no exception
    /// covers, and an empty host, which is that of a document that is from no host, get the default.
    pub fn policy_for_host(&self, top_level_host: &str) -> Policy {
        let host = normalize_host(top_level_host);
        self.exceptions
            .iter()
            .filter(|(pattern, _)| pattern.matches(&host))
            .max_by_key(|(pattern, _)| pattern.specificity())
            .map_or(self.default_policy, |(_, policy)| *policy)
    }

    /// Whether a script from the given party runs on pages whose top-level document is from the host.
    pub fn allows(&self, top_level_host: &str, party: Party) -> bool {
        self.policy_for_host(top_level_host).allows(party)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rules(default_policy: Policy, exceptions: &[(&str, Policy)]) -> Rules {
        let mut rules = Rules::new(default_policy);
        for (site, policy) in exceptions {
            rules.set_exception(SitePattern::parse(site).expect("a valid pattern"), *policy);
        }
        rules
    }

    #[test]
    fn the_default_decides_without_exceptions() {
        for policy in [Policy::Allow, Policy::BlockThirdParty, Policy::Block] {
            let rules = rules(policy, &[]);
            assert_eq!(rules.default_policy(), policy);
            assert_eq!(rules.policy_for_host("example.com"), policy);
            assert_eq!(rules.policy_for_host(""), policy);
        }
    }

    #[test]
    fn an_exception_decides_for_its_site_only() {
        let rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        assert_eq!(rules.policy_for_host("example.com"), Policy::Block);
        assert_eq!(rules.policy_for_host("www.example.com"), Policy::Block);
        assert_eq!(rules.policy_for_host("example.org"), Policy::Allow);
        assert_eq!(rules.policy_for_host("notexample.com"), Policy::Allow);
    }

    #[test]
    fn an_exception_can_allow_a_site_when_the_default_blocks() {
        let rules = rules(Policy::Block, &[("example.com", Policy::Allow)]);
        assert!(rules.allows("example.com", Party::First));
        assert!(rules.allows("example.com", Party::Third));
        assert!(!rules.allows("example.org", Party::First));
        assert!(!rules.allows("example.org", Party::Third));
    }

    #[test]
    fn the_closest_exception_wins_whatever_the_order() {
        let exceptions = [
            ("example.com", Policy::Block),
            ("mail.example.com", Policy::Allow),
            ("com", Policy::BlockThirdParty),
        ];
        let mut reversed = exceptions;
        reversed.reverse();

        for exceptions in [exceptions, reversed] {
            let rules = rules(Policy::Allow, &exceptions);
            assert_eq!(rules.policy_for_host("mail.example.com"), Policy::Allow);
            assert_eq!(rules.policy_for_host("a.mail.example.com"), Policy::Allow);
            assert_eq!(rules.policy_for_host("example.com"), Policy::Block);
            assert_eq!(rules.policy_for_host("www.example.com"), Policy::Block);
            assert_eq!(rules.policy_for_host("other.com"), Policy::BlockThirdParty);
            assert_eq!(rules.policy_for_host("example.org"), Policy::Allow);
        }
    }

    #[test]
    fn the_host_is_matched_whatever_its_case_or_trailing_dot() {
        let rules = rules(Policy::Allow, &[("example.com", Policy::Block)]);
        assert_eq!(rules.policy_for_host("EXAMPLE.com"), Policy::Block);
        assert_eq!(rules.policy_for_host("www.example.com."), Policy::Block);
    }

    #[test]
    fn an_empty_host_matches_no_exception() {
        let rules = rules(
            Policy::Allow,
            &[("example.com", Policy::Block), ("localhost", Policy::Block)],
        );
        assert_eq!(rules.policy_for_host(""), Policy::Allow);
    }

    #[test]
    fn addresses_are_matched_exactly() {
        let rules = rules(
            Policy::Allow,
            &[("127.0.0.1", Policy::Block), ("[::1]", Policy::BlockThirdParty)],
        );
        assert_eq!(rules.policy_for_host("127.0.0.1"), Policy::Block);
        assert_eq!(rules.policy_for_host("127.0.0.2"), Policy::Allow);
        assert_eq!(rules.policy_for_host("[::1]"), Policy::BlockThirdParty);
        assert_eq!(rules.policy_for_host("localhost"), Policy::Allow);
    }

    #[test]
    fn setting_an_exception_again_replaces_it_in_place() {
        let mut rules = rules(
            Policy::Allow,
            &[("example.com", Policy::Block), ("example.org", Policy::Block)],
        );
        rules.set_exception(
            SitePattern::parse("https://EXAMPLE.com/").expect("a valid pattern"),
            Policy::Allow,
        );

        let exceptions: Vec<_> = rules
            .exceptions()
            .map(|(pattern, policy)| (pattern.as_str(), policy))
            .collect();
        assert_eq!(
            exceptions,
            [("example.com", Policy::Allow), ("example.org", Policy::Block)]
        );
    }

    #[test]
    fn first_party_and_third_party_scripts_are_told_apart() {
        let rules = rules(
            Policy::BlockThirdParty,
            &[("trusted.example", Policy::Allow), ("blocked.example", Policy::Block)],
        );

        assert!(rules.allows("news.example", Party::First));
        assert!(!rules.allows("news.example", Party::Third));

        assert!(rules.allows("trusted.example", Party::First));
        assert!(rules.allows("trusted.example", Party::Third));

        assert!(!rules.allows("blocked.example", Party::First));
        assert!(!rules.allows("blocked.example", Party::Third));
    }

    #[test]
    fn a_site_can_block_third_party_scripts_when_the_default_allows() {
        let rules = rules(Policy::Allow, &[("news.example", Policy::BlockThirdParty)]);
        assert!(rules.allows("news.example", Party::First));
        assert!(!rules.allows("www.news.example", Party::Third));
        assert!(rules.allows("other.example", Party::Third));
    }

    #[test]
    fn rules_that_allow_everything_say_so() {
        assert!(rules(Policy::Allow, &[]).allow_everything());
        assert!(rules(Policy::Allow, &[("example.com", Policy::Allow)]).allow_everything());
        assert!(!rules(Policy::Allow, &[("example.com", Policy::Block)]).allow_everything());
        assert!(!rules(Policy::Allow, &[("example.com", Policy::BlockThirdParty)]).allow_everything());
        assert!(!rules(Policy::BlockThirdParty, &[]).allow_everything());
        assert!(!rules(Policy::Block, &[("example.com", Policy::Allow)]).allow_everything());
    }
}
