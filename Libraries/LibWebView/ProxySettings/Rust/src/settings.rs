/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::net::{Ipv4Addr, Ipv6Addr};

/// The `mode` of the saved proxy settings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProxyMode {
    /// The proxy environment variables decide.
    System,
    /// No proxy.
    None,
    /// The proxy the settings name.
    Manual,
}

/// The kinds of proxy a setting can name, in the order of `HTTP::Proxy::Type`.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProxyType {
    Http = 0,
    Https = 1,
    Socks4 = 2,
    Socks4a = 3,
    Socks5 = 4,
    Socks5Hostname = 5,
}

impl ProxyType {
    fn from_setting(setting: &str) -> Option<Self> {
        const TYPES: [(&str, ProxyType); 6] = [
            ("http", ProxyType::Http),
            ("https", ProxyType::Https),
            ("socks4", ProxyType::Socks4),
            ("socks4a", ProxyType::Socks4a),
            ("socks5", ProxyType::Socks5),
            ("socks5h", ProxyType::Socks5Hostname),
        ];
        let setting = setting.trim_ascii();
        TYPES
            .iter()
            .find(|(name, _)| setting.eq_ignore_ascii_case(name))
            .map(|&(_, proxy_type)| proxy_type)
    }
}

/// Why the saved settings do not name a usable proxy.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValidationError {
    UnknownType,
    MissingHost,
    HostHasCredentials,
    InvalidHost,
    InvalidPort,
    InvalidBypassEntry,
}

impl ValidationError {
    /// What the settings page tells the user.
    pub fn message(self) -> &'static str {
        match self {
            Self::UnknownType => "The proxy type is not one that is supported.",
            Self::MissingHost => "The proxy has no host.",
            Self::HostHasCredentials => "A user name and password cannot be saved with the proxy.",
            Self::InvalidHost => "The proxy host is not a host name or an IP address.",
            Self::InvalidPort => "The proxy port has to be between 1 and 65535.",
            Self::InvalidBypassEntry => {
                "The list of hosts that bypass the proxy has an entry that is not a host name, an IP address or an address range."
            }
        }
    }
}

/// The proxy of the saved settings, as written.
#[derive(Clone, Copy, Debug)]
pub struct SavedProxy<'a> {
    pub proxy_type: &'a str,
    pub host: &'a str,
    pub port: i64,
    /// Entries in the syntax of the `no_proxy` environment variable, separated by commas or whitespace.
    pub bypass: &'a str,
}

/// A proxy that passed validation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ManualProxy {
    pub proxy_type: ProxyType,
    /// A lowercase host name or an IP address. An IPv6 address has no brackets.
    pub host: String,
    pub port: u16,
    /// Lowercase entries for `HTTP::ProxyConfiguration` to match hosts against.
    pub bypass: Vec<String>,
}

/// What else, besides the saved settings, has a say in the proxy.
#[derive(Clone, Copy, Debug, Default)]
pub struct Environment {
    /// Whether the proxy environment variables name a proxy.
    pub names_a_proxy: bool,
    /// Whether the browser was launched with --proxy-autoconfig-url.
    pub has_proxy_autoconfig_url: bool,
}

/// How requests leave the browser.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Decision {
    /// Without a proxy.
    Direct,
    /// As the environment and the command line say.
    Environment,
    /// Through the proxy of the saved settings.
    Manual(ManualProxy),
    /// Not at all.
    Block,
}

/// What overrides the saved settings.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ForcedBy {
    Nothing,
    Environment,
    CommandLine,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Resolution {
    pub decision: Decision,
    pub forced_by: ForcedBy,
    /// What is wrong with the proxy of the saved settings, whether or not the decision rests on it.
    pub error: Option<ValidationError>,
}

/// Decides how requests leave the browser.
///
/// The command line and the environment win over the saved settings, so that a launcher that names a proxy is never
/// undone by a setting saved earlier. Saved settings that ask for a proxy either yield that proxy or block every
/// request: they never yield a direct connection.
pub fn resolve(mode: ProxyMode, saved: &SavedProxy<'_>, environment: Environment) -> Resolution {
    let manual = (mode == ProxyMode::Manual).then(|| validate_manual_proxy(saved));
    let error = manual.as_ref().and_then(|manual| manual.as_ref().err().copied());

    let forced_by = if environment.has_proxy_autoconfig_url {
        ForcedBy::CommandLine
    } else if environment.names_a_proxy {
        ForcedBy::Environment
    } else {
        ForcedBy::Nothing
    };

    let decision = if forced_by != ForcedBy::Nothing {
        Decision::Environment
    } else {
        match manual {
            Some(Ok(proxy)) => Decision::Manual(proxy),
            Some(Err(_)) => Decision::Block,
            None => Decision::Direct,
        }
    };

    Resolution {
        decision,
        forced_by,
        error,
    }
}

pub fn validate_manual_proxy(saved: &SavedProxy<'_>) -> Result<ManualProxy, ValidationError> {
    let proxy_type = ProxyType::from_setting(saved.proxy_type).ok_or(ValidationError::UnknownType)?;
    let host = validate_host(saved.host)?;
    let port = u16::try_from(saved.port)
        .ok()
        .filter(|&port| port != 0)
        .ok_or(ValidationError::InvalidPort)?;
    let bypass = saved
        .bypass
        .split(|c: char| c == ',' || c.is_ascii_whitespace())
        .filter(|entry| !entry.is_empty())
        .map(validate_bypass_entry)
        .collect::<Result<Vec<_>, _>>()?;

    Ok(ManualProxy {
        proxy_type,
        host,
        port,
        bypass,
    })
}

fn validate_host(host: &str) -> Result<String, ValidationError> {
    let host = host.trim_ascii();
    if host.is_empty() {
        return Err(ValidationError::MissingHost);
    }
    if host.contains('@') {
        return Err(ValidationError::HostHasCredentials);
    }

    let (unbracketed, is_bracketed) = match host.strip_prefix('[') {
        Some(rest) => (rest.strip_suffix(']').ok_or(ValidationError::InvalidHost)?, true),
        None => (host, false),
    };

    // The port is a setting of its own, so a colon can only be part of an IPv6 address.
    if is_bracketed || unbracketed.contains(':') {
        return validate_ipv6_host(unbracketed).ok_or(ValidationError::InvalidHost);
    }
    if let Ok(address) = unbracketed.parse::<Ipv4Addr>() {
        return Ok(address.to_string());
    }
    if !is_host_name(unbracketed) {
        return Err(ValidationError::InvalidHost);
    }
    Ok(unbracketed.to_ascii_lowercase())
}

/// An IPv6 address, which may name a zone, e.g. `fe80::1%eth0`. The zone is kept as written, because it names a
/// network interface.
fn validate_ipv6_host(host: &str) -> Option<String> {
    let (address, zone) = match host.split_once('%') {
        Some((address, zone)) => (address, Some(zone)),
        None => (host, None),
    };
    address.parse::<Ipv6Addr>().ok()?;

    let Some(zone) = zone else {
        return Some(address.to_ascii_lowercase());
    };
    let is_zone_character = |c: char| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_' | '~');
    // A zone may be written as it is in a URL, with its percent sign encoded as %25.
    let name = zone.strip_prefix("25").filter(|name| !name.is_empty()).unwrap_or(zone);
    if name.is_empty() || !name.chars().all(is_zone_character) {
        return None;
    }
    Some(format!("{}%{zone}", address.to_ascii_lowercase()))
}

/// Whether this is a host name that cannot be mistaken for an IP address in any notation.
fn is_host_name(name: &str) -> bool {
    let name = name.strip_suffix('.').unwrap_or(name);
    if name.is_empty() || name.len() > 253 {
        return false;
    }

    let is_label = |label: &str| {
        !label.is_empty()
            && label.len() <= 63
            && !label.starts_with('-')
            && !label.ends_with('-')
            && label.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
    };
    if !name.split('.').all(is_label) {
        return false;
    }

    // A name whose last label is a number is read as an IPv4 address by whatever connects to it, e.g. 127.1 or
    // 0x7f.0.0.1.
    let last_label = name.rsplit('.').next().unwrap_or(name);
    let hex_digits = last_label.strip_prefix("0x").or_else(|| last_label.strip_prefix("0X"));
    let is_number = last_label.chars().all(|c| c.is_ascii_digit())
        || hex_digits.is_some_and(|digits| digits.chars().all(|c| c.is_ascii_hexdigit()));
    !is_number
}

/// An entry of the bypass list: `*`, an IP address, an address range such as `10.0.0.0/8`, or a host name that also
/// stands for every name below it.
fn validate_bypass_entry(entry: &str) -> Result<String, ValidationError> {
    let entry = entry.to_ascii_lowercase();
    if entry == "*" {
        return Ok(entry);
    }

    let (network, prefix_length) = match entry.split_once('/') {
        Some((network, prefix_length)) => (network, Some(prefix_length)),
        None => (entry.as_str(), None),
    };
    let prefix_length_is_at_most = |maximum: u8| {
        prefix_length.is_none_or(|length| {
            !length.is_empty()
                && length.chars().all(|c| c.is_ascii_digit())
                && length.parse::<u8>().is_ok_and(|length| length <= maximum)
        })
    };

    let address = network
        .strip_prefix('[')
        .and_then(|rest| rest.strip_suffix(']'))
        .unwrap_or(network);
    let is_valid = if network.parse::<Ipv4Addr>().is_ok() {
        prefix_length_is_at_most(32)
    } else if address.parse::<Ipv6Addr>().is_ok() {
        prefix_length_is_at_most(128)
    } else {
        prefix_length.is_none() && is_host_name(network.strip_prefix('.').unwrap_or(network))
    };

    if is_valid {
        Ok(entry)
    } else {
        Err(ValidationError::InvalidBypassEntry)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn saved<'a>(proxy_type: &'a str, host: &'a str, port: i64, bypass: &'a str) -> SavedProxy<'a> {
        SavedProxy {
            proxy_type,
            host,
            port,
            bypass,
        }
    }

    fn valid() -> SavedProxy<'static> {
        saved("socks5h", "127.0.0.1", 9050, "")
    }

    const NO_ENVIRONMENT: Environment = Environment {
        names_a_proxy: false,
        has_proxy_autoconfig_url: false,
    };
    const PROXY_IN_ENVIRONMENT: Environment = Environment {
        names_a_proxy: true,
        has_proxy_autoconfig_url: false,
    };
    const PROXY_AUTOCONFIG_URL: Environment = Environment {
        names_a_proxy: false,
        has_proxy_autoconfig_url: true,
    };

    #[test]
    fn every_proxy_type_is_accepted() {
        let types = [
            ("http", ProxyType::Http),
            ("https", ProxyType::Https),
            ("socks4", ProxyType::Socks4),
            ("socks4a", ProxyType::Socks4a),
            ("socks5", ProxyType::Socks5),
            ("socks5h", ProxyType::Socks5Hostname),
            ("SOCKS5H", ProxyType::Socks5Hostname),
            (" http ", ProxyType::Http),
        ];
        for (setting, expected) in types {
            let proxy = validate_manual_proxy(&saved(setting, "proxy.example", 8080, "")).unwrap();
            assert_eq!(proxy.proxy_type, expected, "{setting}");
        }
    }

    #[test]
    fn unknown_proxy_types_are_rejected() {
        for setting in ["", "socks", "socks6", "ftp", "http://", "socks5 h", "direct"] {
            assert_eq!(
                validate_manual_proxy(&saved(setting, "proxy.example", 8080, "")),
                Err(ValidationError::UnknownType),
                "{setting}"
            );
        }
    }

    #[test]
    fn hosts_are_normalized() {
        let hosts = [
            ("proxy.example", "proxy.example"),
            ("Proxy.Example", "proxy.example"),
            ("  localhost\t", "localhost"),
            ("proxy.example.", "proxy.example."),
            ("my_proxy-1.internal", "my_proxy-1.internal"),
            ("127.0.0.1", "127.0.0.1"),
            ("::1", "::1"),
            ("[::1]", "::1"),
            ("[2001:DB8::1]", "2001:db8::1"),
            ("::ffff:192.0.2.1", "::ffff:192.0.2.1"),
            ("fe80::1%eth0", "fe80::1%eth0"),
            ("[FE80::1%Eth0]", "fe80::1%Eth0"),
            ("fe80::1%25eth0", "fe80::1%25eth0"),
            ("fe80::1%25", "fe80::1%25"),
        ];
        for (setting, expected) in hosts {
            let proxy = validate_manual_proxy(&saved("http", setting, 8080, "")).unwrap();
            assert_eq!(proxy.host, expected, "{setting}");
        }
    }

    #[test]
    fn missing_hosts_are_rejected() {
        for setting in ["", " ", "\t\n"] {
            assert_eq!(
                validate_manual_proxy(&saved("http", setting, 8080, "")),
                Err(ValidationError::MissingHost)
            );
        }
    }

    #[test]
    fn hosts_with_credentials_are_rejected() {
        for setting in ["user@proxy.example", "user:password@proxy.example", "@"] {
            assert_eq!(
                validate_manual_proxy(&saved("http", setting, 8080, "")),
                Err(ValidationError::HostHasCredentials),
                "{setting}"
            );
        }
    }

    #[test]
    fn invalid_hosts_are_rejected() {
        let hosts = [
            // Anything that is more than a host.
            "http://proxy.example",
            "proxy.example:8080",
            "proxy.example/path",
            "proxy.example?query",
            "proxy.example#fragment",
            "proxy example",
            "proxy.example,other.example",
            "proxy%2eexample",
            "pröxy.example",
            // Malformed names.
            ".",
            "..",
            ".proxy.example",
            "proxy..example",
            "-proxy.example",
            "proxy-.example",
            // Numbers that something may read as an IPv4 address.
            "127.1",
            "2130706433",
            "0x7f.0.0.1",
            "127.0.0.0x1",
            "1.2.3.4.5",
            "256.0.0.1",
            "proxy.example.1",
            // Malformed IPv6 addresses.
            "[::1",
            "::1]",
            "[]",
            "[proxy.example]",
            "[127.0.0.1]",
            "[::1]:8080",
            ":::1",
            "::g",
            "fe80::1%",
            "fe80::1%eth 0",
            "fe80::1%eth0/64",
        ];
        for setting in hosts {
            assert_eq!(
                validate_manual_proxy(&saved("http", setting, 8080, "")),
                Err(ValidationError::InvalidHost),
                "{setting}"
            );
        }

        let long_label = "a".repeat(64);
        assert_eq!(
            validate_manual_proxy(&saved("http", &long_label, 8080, "")),
            Err(ValidationError::InvalidHost)
        );
        let long_name = ["a".repeat(63).as_str(); 4].join(".");
        assert_eq!(long_name.len(), 255);
        assert_eq!(
            validate_manual_proxy(&saved("http", &long_name, 8080, "")),
            Err(ValidationError::InvalidHost)
        );
    }

    #[test]
    fn ports_are_bounded() {
        for port in [1, 80, 9050, 65535] {
            let proxy = validate_manual_proxy(&saved("http", "proxy.example", port, "")).unwrap();
            assert_eq!(i64::from(proxy.port), port);
        }
        for port in [0, -1, 65536, 70000, i64::MIN, i64::MAX] {
            assert_eq!(
                validate_manual_proxy(&saved("http", "proxy.example", port, "")),
                Err(ValidationError::InvalidPort),
                "{port}"
            );
        }
    }

    #[test]
    fn bypass_entries_are_split_and_normalized() {
        let proxy = validate_manual_proxy(&saved(
            "http",
            "proxy.example",
            8080,
            " LocalHost, 127.0.0.1\n10.0.0.0/8\t.Internal.Example,,example.org. ::1 [::1] FE80::/10 [fd00::]/8 * ",
        ))
        .unwrap();
        assert_eq!(
            proxy.bypass,
            [
                "localhost",
                "127.0.0.1",
                "10.0.0.0/8",
                ".internal.example",
                "example.org.",
                "::1",
                "[::1]",
                "fe80::/10",
                "[fd00::]/8",
                "*",
            ]
        );

        let proxy = validate_manual_proxy(&saved("http", "proxy.example", 8080, " ,\n, ")).unwrap();
        assert!(proxy.bypass.is_empty());

        for entry in ["0.0.0.0/0", "192.0.2.1/32", "::/0", "::1/128"] {
            assert!(
                validate_manual_proxy(&saved("http", "proxy.example", 8080, entry)).is_ok(),
                "{entry}"
            );
        }
    }

    #[test]
    fn invalid_bypass_entries_are_rejected() {
        let entries = [
            "10.0.0.0/33",
            "10.0.0.0/",
            "10.0.0.0/-1",
            "10.0.0.0/+8",
            "10.0.0.0/8/8",
            "10.0.0.0/255.0.0.0",
            "::1/129",
            "[::1",
            "[::1]:80",
            "10.0.0",
            "localhost:8080",
            "example.org/24",
            "http://example.org",
            "*.example.org",
            "..example.org",
            ".",
            "exämple.org",
            "user@example.org",
        ];
        for entry in entries {
            assert_eq!(
                validate_manual_proxy(&saved("http", "proxy.example", 8080, entry)),
                Err(ValidationError::InvalidBypassEntry),
                "{entry}"
            );
            // One bad entry spoils the list, wherever it is.
            let list = format!("localhost,{entry},127.0.0.1");
            assert_eq!(
                validate_manual_proxy(&saved("http", "proxy.example", 8080, &list)),
                Err(ValidationError::InvalidBypassEntry),
                "{list}"
            );
        }
    }

    #[test]
    fn system_and_none_modes_connect_directly_without_an_environment() {
        for mode in [ProxyMode::System, ProxyMode::None] {
            // The proxy fields are not looked at in these modes, valid or not.
            for saved in [valid(), saved("bogus", "", 0, "/")] {
                assert_eq!(
                    resolve(mode, &saved, NO_ENVIRONMENT),
                    Resolution {
                        decision: Decision::Direct,
                        forced_by: ForcedBy::Nothing,
                        error: None,
                    }
                );
            }
        }
    }

    #[test]
    fn manual_mode_uses_the_saved_proxy() {
        let resolution = resolve(
            ProxyMode::Manual,
            &saved("socks5h", "LOCALHOST", 9050, "localhost .internal.example"),
            NO_ENVIRONMENT,
        );
        assert_eq!(
            resolution,
            Resolution {
                decision: Decision::Manual(ManualProxy {
                    proxy_type: ProxyType::Socks5Hostname,
                    host: "localhost".to_string(),
                    port: 9050,
                    bypass: vec!["localhost".to_string(), ".internal.example".to_string()],
                }),
                forced_by: ForcedBy::Nothing,
                error: None,
            }
        );
    }

    #[test]
    fn every_invalid_manual_proxy_blocks_all_requests() {
        let invalid = [
            (saved("", "127.0.0.1", 9050, ""), ValidationError::UnknownType),
            (saved("socks6", "127.0.0.1", 9050, ""), ValidationError::UnknownType),
            (saved("socks5h", "", 9050, ""), ValidationError::MissingHost),
            (
                saved("socks5h", "user@127.0.0.1", 9050, ""),
                ValidationError::HostHasCredentials,
            ),
            (
                saved("socks5h", "127.0.0.1:9050", 9050, ""),
                ValidationError::InvalidHost,
            ),
            (saved("socks5h", "127.0.0.1", 0, ""), ValidationError::InvalidPort),
            (saved("socks5h", "127.0.0.1", 65536, ""), ValidationError::InvalidPort),
            (
                saved("socks5h", "127.0.0.1", 9050, "10.0.0.0/40"),
                ValidationError::InvalidBypassEntry,
            ),
        ];
        for (saved, error) in invalid {
            assert_eq!(
                resolve(ProxyMode::Manual, &saved, NO_ENVIRONMENT),
                Resolution {
                    decision: Decision::Block,
                    forced_by: ForcedBy::Nothing,
                    error: Some(error),
                },
                "{saved:?}"
            );
        }
    }

    #[test]
    fn the_environment_wins_over_saved_settings() {
        for mode in [ProxyMode::System, ProxyMode::None, ProxyMode::Manual] {
            let resolution = resolve(mode, &valid(), PROXY_IN_ENVIRONMENT);
            assert_eq!(resolution.decision, Decision::Environment, "{mode:?}");
            assert_eq!(resolution.forced_by, ForcedBy::Environment, "{mode:?}");
            assert_eq!(resolution.error, None, "{mode:?}");
        }
    }

    #[test]
    fn the_command_line_wins_over_the_environment_and_saved_settings() {
        let both = Environment {
            names_a_proxy: true,
            has_proxy_autoconfig_url: true,
        };
        for mode in [ProxyMode::System, ProxyMode::None, ProxyMode::Manual] {
            for environment in [PROXY_AUTOCONFIG_URL, both] {
                let resolution = resolve(mode, &valid(), environment);
                assert_eq!(resolution.decision, Decision::Environment, "{mode:?}");
                assert_eq!(resolution.forced_by, ForcedBy::CommandLine, "{mode:?}");
            }
        }
    }

    #[test]
    fn an_overridden_manual_proxy_still_reports_what_is_wrong_with_it() {
        let invalid = saved("socks5h", "127.0.0.1", 0, "");
        for environment in [PROXY_IN_ENVIRONMENT, PROXY_AUTOCONFIG_URL] {
            let resolution = resolve(ProxyMode::Manual, &invalid, environment);
            assert_eq!(resolution.decision, Decision::Environment);
            assert_eq!(resolution.error, Some(ValidationError::InvalidPort));
        }
        // The other modes do not use the proxy fields, so there is nothing to report.
        for mode in [ProxyMode::System, ProxyMode::None] {
            assert_eq!(resolve(mode, &invalid, PROXY_IN_ENVIRONMENT).error, None);
        }
    }

    #[test]
    fn no_setting_that_asks_for_a_proxy_yields_a_direct_connection() {
        let types = ["http", "socks5h", "", "bogus"];
        let hosts = ["127.0.0.1", "proxy.example", "", "a b", "user@host", "[::1"];
        let ports = [-1, 0, 1, 9050, 65535, 65536];
        let bypasses = ["", "localhost", "*", "10.0.0.0/99", "a b/c"];
        for proxy_type in types {
            for host in hosts {
                for port in ports {
                    for bypass in bypasses {
                        let saved = saved(proxy_type, host, port, bypass);
                        let resolution = resolve(ProxyMode::Manual, &saved, NO_ENVIRONMENT);
                        assert_ne!(resolution.decision, Decision::Direct, "{saved:?}");
                        assert_eq!(
                            resolution.decision == Decision::Block,
                            resolution.error.is_some(),
                            "{saved:?}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn every_error_has_a_message() {
        let errors = [
            ValidationError::UnknownType,
            ValidationError::MissingHost,
            ValidationError::HostHasCredentials,
            ValidationError::InvalidHost,
            ValidationError::InvalidPort,
            ValidationError::InvalidBypassEntry,
        ];
        for error in errors {
            assert!(error.message().ends_with('.'), "{error:?}");
        }
    }
}
