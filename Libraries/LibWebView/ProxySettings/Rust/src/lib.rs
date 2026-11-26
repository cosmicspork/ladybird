/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decides which proxy the browser uses from the saved proxy settings, the environment and the command line.
//!
//! This is the only code that reads what the user typed into the proxy settings. A setting that cannot be used never
//! turns into a direct connection: it turns into a decision to block every request.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

mod ffi;
pub mod settings;

pub use settings::{
    Decision, Environment, ForcedBy, ManualProxy, ProxyMode, ProxyType, Resolution, SavedProxy, ValidationError,
    resolve, validate_manual_proxy,
};
