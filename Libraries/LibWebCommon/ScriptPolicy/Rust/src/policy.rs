/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// What a site may run. The values cross the FFI boundary and match `Web::HTML::ScriptingPolicy`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Policy {
    /// Every script runs.
    Allow = 0,
    /// Scripts from the site itself run. Scripts from other sites, and scripts in frames from other sites, do not.
    BlockThirdParty = 1,
    /// No script runs.
    Block = 2,
}

/// Where a script is from, relative to the site of the top-level document.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Party {
    First,
    Third,
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

    /// Whether a script from the given party runs under this policy.
    pub fn allows(self, party: Party) -> bool {
        match self {
            Self::Allow => true,
            Self::BlockThirdParty => party == Party::First,
            Self::Block => false,
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
    fn allow_runs_scripts_of_both_parties() {
        assert!(Policy::Allow.allows(Party::First));
        assert!(Policy::Allow.allows(Party::Third));
    }

    #[test]
    fn block_third_party_runs_first_party_scripts_only() {
        assert!(Policy::BlockThirdParty.allows(Party::First));
        assert!(!Policy::BlockThirdParty.allows(Party::Third));
    }

    #[test]
    fn block_runs_no_scripts() {
        assert!(!Policy::Block.allows(Party::First));
        assert!(!Policy::Block.allows(Party::Third));
    }
}
