/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The user's rules for which sites may keep cookies and other site data, and the decisions made from them.
//!
//! A [`Rules`] holds the policy for sites the user said nothing about, and the exceptions the user made for single
//! sites. The site is the one in the address bar: an exception for it covers every frame of the pages it shows, and
//! is found by the site of the top-level document.
//!
//! A decision is an [`Access`]: whether a context may use site data at all, and if so, whether it uses the data its
//! site keeps as a first party or only the data it keeps under the top-level site.
//!
//! This crate only decides. It does no I/O, knows nothing of URLs or of the public suffix list, and has no unsafe
//! code: whoever asks tells it the site of the top-level document and whether the context is a third party to it.

mod policy;
mod rules;
mod site;

pub use policy::{Access, Party, Policy};
pub use rules::Rules;
pub use site::{SiteHost, host_of_serialized_site};
