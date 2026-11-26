/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibHTTP/Proxy.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The proxy settings as they are saved. The fields of the proxy are kept as written, usable or not, so that the
// settings page can show what was entered and what is wrong with it.
struct WEBVIEW_API ProxySettings {
    enum class Mode : u8 {
        // The proxy environment variables decide.
        System,
        None,
        // The proxy these settings name.
        Manual,
    };

    // proxySettings :: { mode: "system" | "none" | "manual", type: string, host: string, port: number, bypass: string[] }
    static ProxySettings from_json(JsonValue const&);
    JsonObject to_json() const;

    bool operator==(ProxySettings const&) const = default;

    Mode mode { Mode::System };
    // One of http, https, socks4, socks4a, socks5 and socks5h.
    String type;
    String host;
    i64 port { 0 };
    // Hosts that are connected to directly, in the syntax of the no_proxy environment variable.
    Vector<String> bypass;
};

enum class ProxySettingsForcedBy : u8 {
    Nothing,
    Environment,
    CommandLine,
};

struct ResolvedProxySettings {
    // What RequestServer is given.
    HTTP::ProxyConfiguration configuration;
    // What overrides the saved settings.
    ProxySettingsForcedBy forced_by { ProxySettingsForcedBy::Nothing };
    // What is wrong with the proxy of the saved settings, for the user to read.
    Optional<String> error;
};

// Decides which proxy configuration is in effect. The command line and the environment win over the saved settings,
// and saved settings that name an unusable proxy block every request rather than let any connect directly.
WEBVIEW_API ResolvedProxySettings resolve_proxy_settings(ProxySettings const&, HTTP::ProxyConfiguration const& environment, bool has_proxy_autoconfig_url);

}
