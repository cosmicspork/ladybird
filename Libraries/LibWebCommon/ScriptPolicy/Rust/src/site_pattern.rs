/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// The longest host name DNS allows, without the trailing dot.
const MAX_HOST_LENGTH: usize = 253;
/// The longest label DNS allows.
const MAX_LABEL_LENGTH: usize = 63;

/// The site an exception is for: a host name, which also stands for the hosts under it, or an IP address, which
/// stands for itself only.
///
/// A pattern is kept in one spelling, lowercase and without scheme, port, path or trailing dot, so that two patterns
/// for the same site are equal.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SitePattern(String);

impl SitePattern {
    /// The pattern for what the user typed or a caller derived from a URL, or `None` if it names no site.
    ///
    /// A scheme, a port and anything from the path on are dropped, as are a leading `*.` or `.` and a trailing dot.
    /// Host names must already be ASCII, which is how URLs serialize internationalized names.
    pub fn parse(input: &str) -> Option<Self> {
        let input = input.trim_matches(|character: char| character.is_ascii_whitespace());
        if input.is_empty() || !input.bytes().all(|byte| byte.is_ascii_graphic()) {
            return None;
        }
        let input = input.to_ascii_lowercase();

        let without_scheme = match input.split_once("://") {
            Some((scheme, rest)) if is_scheme(scheme) => rest,
            Some(_) => return None,
            None => &input,
        };
        let authority = without_scheme.split(['/', '?', '#']).next().unwrap_or_default();
        if authority.contains('@') {
            return None;
        }

        if authority.starts_with('[') {
            return Self::parse_bracketed_address(authority);
        }

        let host = match authority.split_once(':') {
            Some((host, port)) if is_port(port) => host,
            Some(_) => return None,
            None => authority,
        };
        let host = host
            .strip_prefix("*.")
            .or_else(|| host.strip_prefix('.'))
            .unwrap_or(host);
        let host = host.strip_suffix('.').unwrap_or(host);

        if host.is_empty() || host.len() > MAX_HOST_LENGTH || !host.split('.').all(is_label) {
            return None;
        }
        Some(Self(host.to_string()))
    }

    /// An IPv6 address in brackets, with or without a port after it.
    fn parse_bracketed_address(authority: &str) -> Option<Self> {
        let (address, rest) = authority.split_once(']')?;
        let digits = &address[1..];
        if digits.is_empty()
            || !digits
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit() || byte == b':' || byte == b'.')
        {
            return None;
        }
        match rest.strip_prefix(':') {
            Some(port) if is_port(port) => {}
            Some(_) => return None,
            None if rest.is_empty() => {}
            None => return None,
        }
        Some(Self(format!("{address}]")))
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Whether the pattern covers the host, which must be in the spelling of [`normalize_host`]. A host name covers
    /// itself and the hosts under it. Addresses have no hosts under them, and are under no host.
    pub(crate) fn matches(&self, host: &str) -> bool {
        if self.0 == host {
            return true;
        }
        if is_address(&self.0) || is_address(host) {
            return false;
        }
        host.strip_suffix(self.0.as_str())
            .is_some_and(|prefix| prefix.ends_with('.'))
    }

    /// Of two patterns that cover the same host, the one with the higher value is the closer match.
    pub(crate) fn specificity(&self) -> usize {
        self.0.len()
    }
}

/// The host in the spelling that patterns are matched against: lowercase and without a trailing dot.
pub(crate) fn normalize_host(host: &str) -> String {
    let host = host.strip_suffix('.').unwrap_or(host);
    host.to_ascii_lowercase()
}

fn is_scheme(scheme: &str) -> bool {
    let mut bytes = scheme.bytes();
    bytes.next().is_some_and(|byte| byte.is_ascii_lowercase())
        && bytes.all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'+' | b'-' | b'.'))
}

fn is_port(port: &str) -> bool {
    port.len() <= 5 && port.bytes().all(|byte| byte.is_ascii_digit())
}

fn is_label(label: &str) -> bool {
    !label.is_empty()
        && label.len() <= MAX_LABEL_LENGTH
        && label
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'-' | b'_'))
}

/// Whether the host is an IP address rather than a name. The URL parser reads every host whose last label is a
/// number as an IPv4 address, so this does too.
fn is_address(host: &str) -> bool {
    if host.starts_with('[') {
        return true;
    }
    host.rsplit('.')
        .next()
        .is_some_and(|label| !label.is_empty() && label.bytes().all(|byte| byte.is_ascii_digit()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed(input: &str) -> Option<String> {
        SitePattern::parse(input).map(|pattern| pattern.as_str().to_string())
    }

    fn pattern(input: &str) -> SitePattern {
        SitePattern::parse(input).expect("a valid pattern")
    }

    #[test]
    fn a_host_name_is_kept() {
        assert_eq!(parsed("example.com").as_deref(), Some("example.com"));
        assert_eq!(parsed("sub.example.co.uk").as_deref(), Some("sub.example.co.uk"));
        assert_eq!(parsed("localhost").as_deref(), Some("localhost"));
        assert_eq!(
            parsed("xn--bcher-kva.example").as_deref(),
            Some("xn--bcher-kva.example")
        );
        assert_eq!(parsed("my_host.example").as_deref(), Some("my_host.example"));
    }

    #[test]
    fn case_and_surrounding_whitespace_are_dropped() {
        assert_eq!(parsed("  Example.COM\t").as_deref(), Some("example.com"));
        assert_eq!(parsed("HTTPS://EXAMPLE.COM").as_deref(), Some("example.com"));
    }

    #[test]
    fn scheme_port_and_path_are_dropped() {
        assert_eq!(parsed("https://example.com").as_deref(), Some("example.com"));
        assert_eq!(parsed("http://example.com:8080").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com:443").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com:").as_deref(), Some("example.com"));
        assert_eq!(parsed("https://example.com/a/b?c=d#e").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com/").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com?query").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com#fragment").as_deref(), Some("example.com"));
    }

    #[test]
    fn wildcard_prefix_leading_dot_and_trailing_dot_are_dropped() {
        assert_eq!(parsed("*.example.com").as_deref(), Some("example.com"));
        assert_eq!(parsed(".example.com").as_deref(), Some("example.com"));
        assert_eq!(parsed("example.com.").as_deref(), Some("example.com"));
        assert_eq!(parsed("https://*.example.com./path").as_deref(), Some("example.com"));
    }

    #[test]
    fn addresses_are_kept() {
        assert_eq!(parsed("127.0.0.1").as_deref(), Some("127.0.0.1"));
        assert_eq!(parsed("http://127.0.0.1:8000/").as_deref(), Some("127.0.0.1"));
        assert_eq!(parsed("[::1]").as_deref(), Some("[::1]"));
        assert_eq!(
            parsed("http://[2001:DB8::1]:8080/path").as_deref(),
            Some("[2001:db8::1]")
        );
        assert_eq!(parsed("[::ffff:192.0.2.1]").as_deref(), Some("[::ffff:192.0.2.1]"));
    }

    #[test]
    fn input_that_names_no_site_is_rejected() {
        for input in [
            "",
            "   ",
            "*",
            "*.",
            ".",
            "https://",
            "https:///path",
            "://example.com",
            "1http://example.com",
            "exa mple.com",
            "example..com",
            "*.*.example.com",
            "user@example.com",
            "https://user:password@example.com",
            "example.com:port",
            "example.com:123456",
            "::1",
            "[]",
            "[::1",
            "[::1]suffix",
            "[::1]:port",
            "[not-an-address]",
            "bücher.example",
            "example.com\u{0}",
            "exam\u{7f}ple.com",
            "exa!mple.com",
        ] {
            assert_eq!(parsed(input), None, "{input:?}");
        }
    }

    #[test]
    fn overlong_names_are_rejected() {
        let label = "a".repeat(MAX_LABEL_LENGTH);
        assert!(parsed(&format!("{label}.example")).is_some());
        assert_eq!(parsed(&format!("{label}a.example")), None);

        let name = [label.as_str(); 4].join(".");
        assert_eq!(name.len(), 255);
        assert_eq!(parsed(&name), None);
        assert!(parsed(&name[2..]).is_some());
    }

    #[test]
    fn parsing_a_pattern_again_changes_nothing() {
        for input in [
            "https://*.Example.com:8080/path",
            "[2001:DB8::1]:80",
            "127.0.0.1",
            "localhost.",
        ] {
            let once = parsed(input).expect("a valid pattern");
            assert_eq!(parsed(&once).as_deref(), Some(once.as_str()));
        }
    }

    #[test]
    fn a_host_name_covers_itself_and_the_hosts_under_it() {
        let example = pattern("example.com");
        assert!(example.matches("example.com"));
        assert!(example.matches("www.example.com"));
        assert!(example.matches("a.b.example.com"));
    }

    #[test]
    fn a_host_name_does_not_cover_other_hosts() {
        let example = pattern("example.com");
        assert!(!example.matches("example.org"));
        assert!(!example.matches("com"));
        assert!(!example.matches("notexample.com"));
        assert!(!example.matches("example.com.evil.test"));
        assert!(!example.matches("example.co"));
        assert!(!example.matches(""));

        let sub = pattern("www.example.com");
        assert!(!sub.matches("example.com"));
        assert!(!sub.matches("other.example.com"));
    }

    #[test]
    fn an_address_covers_itself_only() {
        let address = pattern("0.0.1");
        assert!(address.matches("0.0.1"));
        assert!(!address.matches("127.0.0.1"));

        let loopback = pattern("127.0.0.1");
        assert!(loopback.matches("127.0.0.1"));
        assert!(!loopback.matches("1.127.0.0.1"));

        let v6 = pattern("[::1]");
        assert!(v6.matches("[::1]"));
        assert!(!v6.matches("[::2]"));
        assert!(!v6.matches("a.[::1]"));
    }

    #[test]
    fn a_host_name_does_not_cover_an_address() {
        // NB: No address is under a name, even one that ends in what looks like the name.
        assert!(!pattern("1").matches("127.0.0.1"));
    }

    #[test]
    fn hosts_are_normalized_before_matching() {
        assert_eq!(normalize_host("WWW.Example.COM."), "www.example.com");
        assert_eq!(normalize_host("example.com"), "example.com");
        assert_eq!(normalize_host(""), "");
    }

    #[test]
    fn a_longer_pattern_is_more_specific() {
        assert!(pattern("www.example.com").specificity() > pattern("example.com").specificity());
        assert!(pattern("example.com").specificity() > pattern("com").specificity());
    }
}
