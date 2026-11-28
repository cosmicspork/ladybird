/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// The host of a site that an exception is for: a registrable domain such as `example.com`, a host name that has no
/// registrable domain such as `localhost`, or an IP address such as `127.0.0.1` or `[::1]`.
///
/// It is kept as a URL parser serializes a host, in lower case and without a trailing dot. Whoever makes one from what
/// a user typed parses that as a URL first; this type only refuses text that cannot be such a host, so that a damaged
/// settings file cannot produce an exception that matches by accident.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct SiteHost(String);

impl SiteHost {
    /// The host named by the text, or `None` if the text is not a serialized host.
    pub fn parse(text: &str) -> Option<Self> {
        if text.is_empty() || text.len() > 255 {
            return None;
        }

        if let Some(address) = text.strip_prefix('[') {
            let address = address.strip_suffix(']')?;
            let is_ipv6 = !address.is_empty()
                && address
                    .bytes()
                    .all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f' | b':' | b'.'));
            return is_ipv6.then(|| Self(text.to_owned()));
        }

        let is_label_byte = |byte: u8| matches!(byte, b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_');
        let is_host_name = text
            .split('.')
            .all(|label| !label.is_empty() && label.bytes().all(is_label_byte));
        is_host_name.then(|| Self(text.to_owned()))
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Whether this is an IP address, which stands for itself only.
    fn is_ip_address(&self) -> bool {
        self.0.starts_with('[') || is_ipv4_address(&self.0)
    }

    /// Whether an exception for this host is for the host: it is, or it is a domain above it.
    ///
    /// The host of a site usually is a registrable domain already, and then only the same domain matches. A host
    /// without a registrable domain, such as `intranet.corp`, is a site of its own, and an exception for `corp`
    /// covers it.
    pub fn covers(&self, host: &str) -> bool {
        if host == self.0 {
            return true;
        }
        if self.is_ip_address() || is_ipv4_address(host) || host.starts_with('[') {
            return false;
        }
        host.strip_suffix(self.as_str()).is_some_and(|rest| rest.ends_with('.'))
    }

    /// How closely an exception for this host names a host it covers: of two that cover a host, the longer wins.
    pub fn specificity(&self) -> usize {
        self.0.len()
    }
}

fn is_ipv4_address(host: &str) -> bool {
    let mut parts = 0;
    for part in host.split('.') {
        if part.is_empty() || part.len() > 3 || !part.bytes().all(|byte| byte.is_ascii_digit()) {
            return false;
        }
        parts += 1;
    }
    parts == 4
}

/// The host of a serialized site, such as `example.com` for `https://example.com`.
///
/// The site is in the spelling state is partitioned by (`URL::Site::serialize_for_partitioning()` in LibURL): a scheme,
/// `://` and a host. The site of file: URLs, `file://`, has no host, and text that is not a serialized site has none.
pub fn host_of_serialized_site(site: &str) -> Option<&str> {
    let (scheme, host) = site.split_once("://")?;
    if scheme.is_empty() || host.is_empty() {
        return None;
    }
    // NB: A site has no port, path or credentials. Text with any of them is something else, which has no host here.
    if host.contains(['/', '@', '?', '#']) {
        return None;
    }
    if !host.starts_with('[') && host.contains(':') {
        return None;
    }
    Some(host)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn site(text: &str) -> SiteHost {
        SiteHost::parse(text).unwrap_or_else(|| panic!("{text:?} should be a host"))
    }

    #[test]
    fn parses_serialized_hosts() {
        for text in [
            "example.com",
            "localhost",
            "a-b.example",
            "xn--bcher-kva.example",
            "127.0.0.1",
            "[::1]",
            "[2001:db8::1]",
            "under_score.example",
        ] {
            assert_eq!(site(text).as_str(), text);
        }
    }

    #[test]
    fn refuses_text_that_is_not_a_serialized_host() {
        for text in [
            "",
            " ",
            "Example.com",
            "example.com.",
            ".example.com",
            "example..com",
            "https://example.com",
            "example.com:8080",
            "example.com/path",
            "user@example.com",
            "*.example.com",
            "exa mple.com",
            "bücher.example",
            "[]",
            "[::1",
            "::1",
            "[::g]",
        ] {
            assert_eq!(SiteHost::parse(text), None, "{text:?}");
        }
        assert_eq!(SiteHost::parse(&"a".repeat(256)), None);
    }

    #[test]
    fn covers_itself() {
        assert!(site("example.com").covers("example.com"));
        assert!(site("localhost").covers("localhost"));
        assert!(site("127.0.0.1").covers("127.0.0.1"));
        assert!(site("[::1]").covers("[::1]"));
    }

    #[test]
    fn covers_hosts_under_it() {
        assert!(site("example.com").covers("www.example.com"));
        assert!(site("example.com").covers("a.b.example.com"));
        assert!(site("corp").covers("intranet.corp"));
    }

    #[test]
    fn does_not_cover_other_hosts() {
        assert!(!site("example.com").covers("example.org"));
        assert!(!site("example.com").covers("notexample.com"));
        assert!(!site("www.example.com").covers("example.com"));
        assert!(!site("example.com").covers("com"));
        assert!(!site("example.com").covers(""));
    }

    #[test]
    fn an_ip_address_only_covers_itself() {
        assert!(!site("0.0.1").covers("127.0.0.1"));
        assert!(!site("1").covers("127.0.0.1"));
        assert!(!site("127.0.0.1").covers("1.127.0.0.1"));
        assert!(!site("[::1]").covers("a.[::1]"));
    }

    #[test]
    fn finds_the_host_of_a_serialized_site() {
        assert_eq!(host_of_serialized_site("https://example.com"), Some("example.com"));
        assert_eq!(host_of_serialized_site("http://localhost"), Some("localhost"));
        assert_eq!(host_of_serialized_site("https://127.0.0.1"), Some("127.0.0.1"));
        assert_eq!(host_of_serialized_site("https://[::1]"), Some("[::1]"));
    }

    #[test]
    fn text_that_is_not_a_serialized_site_has_no_host() {
        for text in [
            "",
            "file://",
            "null",
            "example.com",
            "://example.com",
            "https://example.com/",
            "https://example.com:8080",
            "https://user@example.com",
            "https://example.com?query",
            "https://example.com#fragment",
        ] {
            assert_eq!(host_of_serialized_site(text), None, "{text:?}");
        }
    }
}
