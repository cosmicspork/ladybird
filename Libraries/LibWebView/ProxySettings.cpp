/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <LibWebView/ProxySettings.h>
#include <LibWebView/ProxySettingsRustFFI.h>

namespace WebView {

static constexpr auto MODE_KEY = "mode"sv;
static constexpr auto TYPE_KEY = "type"sv;
static constexpr auto HOST_KEY = "host"sv;
static constexpr auto PORT_KEY = "port"sv;
static constexpr auto BYPASS_KEY = "bypass"sv;

static StringView mode_to_string(ProxySettings::Mode mode)
{
    switch (mode) {
    case ProxySettings::Mode::System:
        return "system"sv;
    case ProxySettings::Mode::None:
        return "none"sv;
    case ProxySettings::Mode::Manual:
        return "manual"sv;
    }
    VERIFY_NOT_REACHED();
}

ProxySettings ProxySettings::from_json(JsonValue const& json)
{
    ProxySettings settings;
    if (!json.is_object())
        return settings;
    auto const& object = json.as_object();

    if (auto mode = object.get_string(MODE_KEY); mode.has_value()) {
        if (*mode == "none"sv)
            settings.mode = Mode::None;
        else if (*mode == "manual"sv)
            settings.mode = Mode::Manual;
    }

    // NB: Nothing is checked here. A proxy that cannot be used has to stay one, for resolve_proxy_settings() to block
    //     requests over it, rather than be dropped and so become no proxy at all.
    if (auto type = object.get_string(TYPE_KEY); type.has_value())
        settings.type = *type;
    if (auto host = object.get_string(HOST_KEY); host.has_value())
        settings.host = *host;
    // A port with a fraction is not rounded to one that might be in use.
    if (auto port = object.get(PORT_KEY); port.has_value()) {
        if (auto integer = port->get_i64(); integer.has_value() && port->get_double_with_precision_loss() == static_cast<double>(*integer))
            settings.port = *integer;
    }
    if (auto bypass = object.get_array(BYPASS_KEY); bypass.has_value()) {
        bypass->for_each([&](JsonValue const& entry) {
            if (entry.is_string())
                settings.bypass.append(entry.as_string());
        });
    }

    return settings;
}

JsonObject ProxySettings::to_json() const
{
    JsonArray bypass_entries;
    for (auto const& entry : bypass)
        bypass_entries.must_append(entry);

    JsonObject object;
    object.set(MODE_KEY, mode_to_string(mode));
    object.set(TYPE_KEY, type);
    object.set(HOST_KEY, host);
    object.set(PORT_KEY, port);
    object.set(BYPASS_KEY, move(bypass_entries));
    return object;
}

static u8 mode_for_ffi(ProxySettings::Mode mode)
{
    switch (mode) {
    case ProxySettings::Mode::System:
        return 0;
    case ProxySettings::Mode::None:
        return 1;
    case ProxySettings::Mode::Manual:
        return 2;
    }
    VERIFY_NOT_REACHED();
}

ResolvedProxySettings resolve_proxy_settings(ProxySettings const& settings, HTTP::ProxyConfiguration const& environment, bool has_proxy_autoconfig_url)
{
    struct ManualProxy {
        ByteString host;
        Vector<ByteString> bypass;
    } manual_proxy;

    StringBuilder bypass;
    bypass.join(',', settings.bypass);
    auto bypass_list = bypass.string_view();

    auto resolution = FFI::webview_proxy_settings_resolve(
        mode_for_ffi(settings.mode),
        settings.type.bytes().data(), settings.type.bytes().size(),
        settings.host.bytes().data(), settings.host.bytes().size(),
        settings.port,
        bypass_list.bytes().data(), bypass_list.length(),
        !environment.is_empty(),
        has_proxy_autoconfig_url,
        &manual_proxy,
        [](void* context, u8 const* data, size_t length) {
            static_cast<ManualProxy*>(context)->host = ByteString { StringView { data, length } };
        },
        [](void* context, u8 const* data, size_t length) {
            static_cast<ManualProxy*>(context)->bypass.append(ByteString { StringView { data, length } });
        });

    ResolvedProxySettings resolved;

    switch (resolution.forced_by) {
    case FFI::ProxySettingsForcedBy::Nothing:
        resolved.forced_by = ProxySettingsForcedBy::Nothing;
        break;
    case FFI::ProxySettingsForcedBy::Environment:
        resolved.forced_by = ProxySettingsForcedBy::Environment;
        break;
    case FFI::ProxySettingsForcedBy::CommandLine:
        resolved.forced_by = ProxySettingsForcedBy::CommandLine;
        break;
    }

    if (resolution.error_len != 0)
        resolved.error = String::from_utf8_without_validation({ resolution.error, resolution.error_len });

    switch (resolution.decision) {
    case FFI::ProxySettingsDecision::Direct:
        break;
    case FFI::ProxySettingsDecision::Environment:
        resolved.configuration = environment;
        break;
    case FFI::ProxySettingsDecision::Manual: {
        VERIFY(resolution.proxy_type <= to_underlying(HTTP::Proxy::Type::SOCKS5Hostname));
        VERIFY(!manual_proxy.host.is_empty());
        VERIFY(resolution.port != 0);

        HTTP::Proxy proxy;
        proxy.type = static_cast<HTTP::Proxy::Type>(resolution.proxy_type);
        proxy.host = move(manual_proxy.host);
        proxy.port = resolution.port;
        resolved.configuration = HTTP::ProxyConfiguration { proxy, proxy, move(manual_proxy.bypass) };
        break;
    }
    case FFI::ProxySettingsDecision::Block:
        resolved.configuration = HTTP::ProxyConfiguration::blocking_all_requests();
        break;
    }

    return resolved;
}

}
