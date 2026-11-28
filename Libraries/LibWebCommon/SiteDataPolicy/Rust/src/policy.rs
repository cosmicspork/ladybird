/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// What the pages of a site may keep. The values cross the FFI boundary and match
/// `Web::StorageAPI::SiteDataPolicy`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Policy {
    /// Every context may keep site data. A third party keeps it apart for each top-level site.
    Allow = 0,
    /// The site itself may keep site data. Content from other sites, and frames from other sites, may not.
    BlockThirdParty = 1,
    /// Nothing may keep site data.
    Block = 2,
}

/// What a context that uses site data is, relative to the site of the top-level document.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Party {
    First,
    Third,
}

/// Whether a context may use site data, and which. The values cross the FFI boundary and match
/// `Web::StorageAPI::SiteDataAccess`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Access {
    /// The context reads nothing and what it writes is dropped.
    Denied = 0,
    /// The context uses the data its site keeps as a first party.
    FirstParty = 1,
    /// The context uses the data its site keeps under the top-level site, and no other.
    Partitioned = 2,
}

impl Policy {
    /// The policy with the given value, or `None` for a value that is not a policy.
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Allow),
            1 => Some(Self::BlockThirdParty),
            2 => Some(Self::Block),
            _ => None,
        }
    }

    /// The cookies a context of the given party uses under this policy.
    ///
    /// A third party never uses the cookies its site keeps as a first party, whatever the policy: where it may keep
    /// cookies at all, they are partitioned by the top-level site.
    pub fn cookie_access(self, party: Party) -> Access {
        match (self, party) {
            (Self::Block, _) | (Self::BlockThirdParty, Party::Third) => Access::Denied,
            (Self::Allow | Self::BlockThirdParty, Party::First) => Access::FirstParty,
            (Self::Allow, Party::Third) => Access::Partitioned,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const POLICIES: [Policy; 3] = [Policy::Allow, Policy::BlockThirdParty, Policy::Block];

    #[test]
    fn values_round_trip() {
        for policy in POLICIES {
            assert_eq!(Policy::from_u8(policy as u8), Some(policy));
        }
        assert_eq!(Policy::from_u8(3), None);
        assert_eq!(Policy::from_u8(u8::MAX), None);
    }

    #[test]
    fn allow_gives_a_first_party_its_cookies_and_a_third_party_a_partition() {
        assert_eq!(Policy::Allow.cookie_access(Party::First), Access::FirstParty);
        assert_eq!(Policy::Allow.cookie_access(Party::Third), Access::Partitioned);
    }

    #[test]
    fn block_third_party_denies_third_parties_only() {
        assert_eq!(Policy::BlockThirdParty.cookie_access(Party::First), Access::FirstParty);
        assert_eq!(Policy::BlockThirdParty.cookie_access(Party::Third), Access::Denied);
    }

    #[test]
    fn block_denies_both_parties() {
        assert_eq!(Policy::Block.cookie_access(Party::First), Access::Denied);
        assert_eq!(Policy::Block.cookie_access(Party::Third), Access::Denied);
    }

    #[test]
    fn no_policy_gives_a_third_party_first_party_cookies() {
        for policy in POLICIES {
            assert_ne!(policy.cookie_access(Party::Third), Access::FirstParty);
        }
    }
}
