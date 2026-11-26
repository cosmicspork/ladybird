const manualProxySettings = document.querySelector("#manual-proxy-settings");
const proxyForciblyEnabled = document.querySelector("#proxy-forcibly-enabled");
const proxyError = document.querySelector("#proxy-error");
const proxyRemoteDnsContainer = document.querySelector("#proxy-remote-dns-container");

const proxyMode = document.querySelector("#proxy-mode");
const proxyType = document.querySelector("#proxy-type");
const proxyRemoteDns = document.querySelector("#proxy-remote-dns");
const proxyHost = document.querySelector("#proxy-host");
const proxyPort = document.querySelector("#proxy-port");
const proxyBypass = document.querySelector("#proxy-bypass");

const controls = [proxyMode, proxyType, proxyRemoteDns, proxyHost, proxyPort, proxyBypass];

let PROXY_SETTINGS = {};

function loadSettings(settings) {
    if (!Object.hasOwn(settings, "proxySettings")) {
        return;
    }

    PROXY_SETTINGS = settings.proxySettings || {};
    loadProxySettings();
}

function loadProxySettings() {
    proxyMode.value = PROXY_SETTINGS.mode || "system";

    // SOCKS5 with host names resolved by the proxy is the type socks5h, which the switch stands for.
    const type = PROXY_SETTINGS.type || "http";
    proxyType.value = type === "socks5h" ? "socks5" : type;
    proxyRemoteDns.checked = type !== "socks5";

    proxyHost.value = PROXY_SETTINGS.host || "";
    proxyPort.value = PROXY_SETTINGS.port || "";
    proxyBypass.value = (PROXY_SETTINGS.bypass || []).join(", ");

    showManualProxySettings();

    if (PROXY_SETTINGS.forcedBy) {
        proxyForciblyEnabled.textContent =
            PROXY_SETTINGS.forcedBy === "commandLine"
                ? "This setting is controlled via the command line"
                : "This setting is controlled via the proxy environment variables";
        proxyForciblyEnabled.classList.remove("hidden");
    } else {
        proxyForciblyEnabled.classList.add("hidden");
    }

    for (const control of controls) {
        control.disabled = !!PROXY_SETTINGS.forcedBy;
    }

    if (PROXY_SETTINGS.error && proxyMode.value === "manual") {
        proxyError.textContent = PROXY_SETTINGS.forcedBy
            ? PROXY_SETTINGS.error
            : `${PROXY_SETTINGS.error} Nothing is loaded from the network until this is corrected.`;
        proxyError.classList.remove("hidden");
    } else {
        proxyError.classList.add("hidden");
    }
}

function showManualProxySettings() {
    manualProxySettings.classList.toggle("hidden", proxyMode.value !== "manual");
    proxyRemoteDnsContainer.classList.toggle("hidden", proxyType.value !== "socks5");
}

function updateProxySettings() {
    showManualProxySettings();

    const port = proxyPort.value.trim();

    ladybird.sendMessage("setProxySettings", {
        mode: proxyMode.value,
        type: proxyType.value === "socks5" && proxyRemoteDns.checked ? "socks5h" : proxyType.value,
        host: proxyHost.value.trim(),
        // Anything that is not a whole number is sent as a port that cannot be used, and is reported as one.
        port: /^[0-9]{1,5}$/.test(port) ? Number(port) : 0,
        bypass: proxyBypass.value.split(/[\s,]+/).filter(entry => entry.length !== 0),
    });
}

proxyMode.addEventListener("change", () => {
    showManualProxySettings();

    // A manual proxy takes effect once it has been filled in, so that choosing it does not cut off the network while
    // its fields are still empty.
    if (proxyMode.value === "manual" && (proxyHost.value.trim().length === 0 || proxyPort.value.trim().length === 0)) {
        return;
    }

    updateProxySettings();
});

for (const control of [proxyType, proxyRemoteDns, proxyHost, proxyPort, proxyBypass]) {
    control.addEventListener("change", updateProxySettings);
}

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
