(function() {
    // Only the top document hosts the player, top bar and audio graph; child frames
    // (sign-in widgets, embeds) get nothing. Comparing WindowProxies never throws cross-origin.
    try {
        if (window.top !== window) return;
    } catch (_) {
        return;
    }
    if (window.__ytr_bridge_installed) {
        if (typeof window.__ytr_recheck === 'function') window.__ytr_recheck();
        return;
    }
    window.__ytr_bridge_installed = true;

    function postToHost(obj) {
        try {
            if (window.chrome && window.chrome.webview) {
                window.chrome.webview.postMessage(JSON.stringify(obj));
            }
        } catch (_) {}
    }

    function logToHost(message) {
        postToHost({ type: 'log', message: String(message) });
    }

    function lsGet(key) {
        try {
            return localStorage.getItem(key);
        } catch (_) {
            return null;
        }
    }

    function lsSet(key, value) {
        try {
            localStorage.setItem(key, value);
        } catch (_) {}
    }

    // 0. High-Performance Innertube / Player Ad Stripper (Intercepts API responses before YouTube parses them)
    // Runs on every JSON.parse in the page: no allocation, no recursion, and objects
    // without ad keys cost six property probes.
    function stripAds(o) {
        if ('playerAds' in o) delete o.playerAds;
        if ('adPlacements' in o) delete o.adPlacements;
        if ('adSlots' in o) delete o.adSlots;
        if ('adBreakHeartbeatParams' in o) delete o.adBreakHeartbeatParams;
        if ('adBreakHeartbeatRenderer' in o) delete o.adBreakHeartbeatRenderer;
        if ('adThrottled' in o) delete o.adThrottled;
    }

    function sanitizeAdObject(obj) {
        for (let depth = 0; depth < 4 && obj !== null && typeof obj === 'object'; depth++) {
            stripAds(obj);
            const next = obj.playerResponse;
            if (next === obj) break;
            obj = next;
        }
    }

    // Intercept Response.prototype.json to sanitize all incoming /youtubei/v1/player & /next responses
    try {
        const origResponseJson = Response.prototype.json;
        Response.prototype.json = async function() {
            const data = await origResponseJson.apply(this, arguments);
            if (data !== null && typeof data === 'object') {
                try {
                    sanitizeAdObject(data);
                } catch (_) {}
            }
            return data;
        };
    } catch (_) {}

    // Intercept JSON.parse for embedded responses and initial state
    try {
        const origJsonParse = JSON.parse;
        JSON.parse = function(text, reviver) {
            const data = origJsonParse.apply(this, arguments);
            if (data !== null && typeof data === 'object') {
                try {
                    sanitizeAdObject(data);
                } catch (_) {}
            }
            return data;
        };
    } catch (_) {}

    try {
        // Trap window.ytInitialPlayerResponse
        let rawInitialPlayerResponse = window.ytInitialPlayerResponse;
        Object.defineProperty(window, 'ytInitialPlayerResponse', {
            configurable: true,
            enumerable: true,
            get() { return rawInitialPlayerResponse; },
            set(val) {
                if (val && typeof val === 'object') sanitizeAdObject(val);
                rawInitialPlayerResponse = val;
            }
        });

    } catch (_) {}

    // YTM assigns navigator.mediaSession.metadata once per track; this is the cheapest
    // reliable signal that title/artist/artwork changed.
    try {
        const mdDesc = typeof MediaSession !== 'undefined'
            ? Object.getOwnPropertyDescriptor(MediaSession.prototype, 'metadata')
            : null;
        if (mdDesc && mdDesc.get && mdDesc.set) {
            Object.defineProperty(MediaSession.prototype, 'metadata', {
                configurable: true,
                enumerable: mdDesc.enumerable,
                get: mdDesc.get,
                set(value) {
                    mdDesc.set.call(this, value);
                    try {
                        queueState(50);
                    } catch (_) {}
                }
            });
        }
    } catch (_) {}

    // 1. Safe CSS Injection: Topbar + Layout Offset + Complete Ad/Promo Blocking
    const CORE_CSS = `
                /* Complete Ad & Promo Blocker Rules */
                .ytp-ad-overlay-container,
                .ytp-ad-message-container,
                .ytp-ad-action-interstitial,
                #player-ads,
                ytmusic-mealbar-promo-renderer,
                ytd-ad-slot-renderer,
                ytmusic-banner-promo-renderer,
                a[href*="/music_premium"],
                a[href*="/upgrade"],
                #upgrade-button,
                .ytmusic-nav-bar #upgrade-button,
                ytmusic-upsell-dialog-renderer,
                ytmusic-guide-entry-renderer[ytr-hide],
                .ytp-ad-preview-container,
                .ytp-ad-preview-text,
                .ytp-ad-text,
                .ytp-ad-player-overlay,
                .ytp-ad-player-overlay-layout,
                .ytp-ad-image-overlay,
                .ytp-ad-skip-button-container {
                    display: none !important;
                    visibility: hidden !important;
                    pointer-events: none !important;
                    height: 0 !important;
                    opacity: 0 !important;
                }

                /* Kept apart so an unsupported :has() form can only drop these two selectors */
                tp-yt-paper-dialog:has(> ytmusic-mealbar-promo-renderer:only-child),
                ytmusic-pivot-bar-item-renderer:has(a[href*="/upgrade"]) {
                    display: none !important;
                }

                /* Topbar Layout Offset: pushes YouTube Music down 36px so it never collides */
                :root {
                    --menu-bar-height: 36px !important;
                }

                body {
                    padding-top: 36px !important;
                    box-sizing: border-box !important;
                }

                ytmusic-app-layout {
                    overflow: auto scroll !important;
                    height: calc(100vh - 36px) !important;
                }

                ytmusic-app-layout > #content {
                    padding-top: 36px !important;
                }

                ytmusic-app-layout > [slot='nav-bar'],
                #nav-bar-background.ytmusic-app-layout,
                ytmusic-nav-bar {
                    top: 36px !important;
                }

                #nav-bar-divider.ytmusic-app-layout {
                    top: calc(var(--ytmusic-nav-bar-height, 64px) + 36px) !important;
                }

                ytmusic-app[is-bauhaus-sidenav-enabled] #guide-spacer.ytmusic-app,
                ytmusic-app[is-bauhaus-sidenav-enabled] #mini-guide-spacer.ytmusic-app {
                    margin-top: calc(var(--ytmusic-nav-bar-height, 64px) + 36px) !important;
                }

                ytmusic-app-layout > [slot='player-page'] {
                    margin-top: 36px !important;
                }

                ytmusic-guide-renderer {
                    height: calc(100vh - 36px - var(--ytmusic-nav-bar-height, 64px)) !important;
                }

                /* In-App Topbar Navigation & Actions Panel */
                #ytmd-title-bar-main-panel {
                    position: fixed !important;
                    top: 0 !important;
                    left: 0 !important;
                    right: 0 !important;
                    width: 100% !important;
                    height: 36px !important;
                    background: #0b0c10 !important;
                    border-bottom: 1px solid rgba(255, 255, 255, 0.06) !important;
                    z-index: 2147483647 !important;
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                    padding: 0 8px 0 12px !important;
                    box-sizing: border-box !important;
                    user-select: none !important;
                    font-family: "Segoe UI Variable Text", "Segoe UI", -apple-system, Roboto, sans-serif !important;
                    box-shadow: none !important;
                }

                .ytr-topbar-left,
                .ytr-topbar-right {
                    display: flex;
                    align-items: center;
                    gap: 2px;
                    flex-shrink: 0;
                }

                .ytr-topbar-brand {
                    display: flex;
                    align-items: center;
                    gap: 7px;
                    margin-right: 10px;
                }

                .ytr-brand-title {
                    color: #ffffff;
                    font-size: 12px;
                    font-weight: 600;
                    letter-spacing: 0.1px;
                }

                .ytr-icon-btn {
                    width: 28px;
                    height: 26px;
                    padding: 0;
                    display: inline-flex;
                    align-items: center;
                    justify-content: center;
                    background: transparent;
                    border: none;
                    border-radius: 6px;
                    color: rgba(255, 255, 255, 0.6);
                    cursor: pointer;
                    transition: background 0.12s ease, color 0.12s ease;
                }

                .ytr-icon-btn:hover,
                .ytr-menu-open > .ytr-icon-btn {
                    background: rgba(255, 255, 255, 0.08);
                    color: #ffffff;
                }

                .ytr-icon-btn:active,
                .ytr-search-btn:active,
                .ytr-audio-btn:active {
                    transform: translateY(0.5px);
                }

                .ytr-icon-btn:focus-visible,
                .ytr-search-btn:focus-visible,
                .ytr-audio-btn:focus-visible,
                .ytr-menu-item:focus-visible {
                    outline: 2px solid rgba(255, 90, 31, 0.75);
                    outline-offset: 1px;
                }

                .ytr-search-btn {
                    display: inline-flex;
                    align-items: center;
                    gap: 8px;
                    height: 26px;
                    width: 220px;
                    margin-left: 8px;
                    padding: 0 5px 0 9px;
                    background: rgba(255, 255, 255, 0.05);
                    border: 1px solid rgba(255, 255, 255, 0.07);
                    border-radius: 7px;
                    color: rgba(255, 255, 255, 0.45);
                    font: inherit;
                    font-size: 11.5px;
                    cursor: pointer;
                    transition: background 0.12s ease, border-color 0.12s ease, color 0.12s ease;
                }

                .ytr-search-btn:hover {
                    background: rgba(255, 255, 255, 0.08);
                    border-color: rgba(255, 255, 255, 0.13);
                    color: rgba(255, 255, 255, 0.75);
                }

                .ytr-search-label {
                    flex: 1;
                    text-align: left;
                }

                .ytr-kbd {
                    font: 600 9.5px/1 "Segoe UI", sans-serif;
                    padding: 3px 5px;
                    border-radius: 4px;
                    background: rgba(255, 255, 255, 0.07);
                    color: rgba(255, 255, 255, 0.5);
                    letter-spacing: 0.2px;
                }

                .ytr-topbar-center {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    flex: 1;
                    min-width: 0;
                    max-width: 440px;
                    margin: 0 12px;
                }

                #ytr-topbar-ticker {
                    color: rgba(255, 255, 255, 0.5);
                    font-size: 11.5px;
                    overflow: hidden;
                    text-overflow: ellipsis;
                    white-space: nowrap;
                }

                .ytr-audio-btn {
                    display: inline-flex;
                    align-items: center;
                    gap: 7px;
                    height: 26px;
                    margin-right: 4px;
                    padding: 0 10px 0 8px;
                    background: transparent;
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    border-radius: 7px;
                    color: rgba(255, 255, 255, 0.7);
                    font: inherit;
                    font-size: 11.5px;
                    font-weight: 500;
                    cursor: pointer;
                    transition: background 0.12s ease, border-color 0.12s ease, color 0.12s ease;
                }

                .ytr-audio-btn:hover {
                    background: rgba(255, 255, 255, 0.06);
                    border-color: rgba(255, 255, 255, 0.18);
                    color: #ffffff;
                }

                .ytr-audio-btn.ytr-eq-on {
                    color: #ffffff;
                    background: rgba(255, 61, 0, 0.09);
                    border-color: rgba(255, 61, 0, 0.38);
                }

                .ytr-audio-btn.ytr-eq-on svg {
                    color: #ff5a1f;
                }

                .ytr-fade-dot {
                    width: 6px;
                    height: 6px;
                    border-radius: 50%;
                    background: rgba(255, 255, 255, 0.22);
                    transition: background 0.2s ease, box-shadow 0.2s ease;
                }

                .ytr-fade-dot.ytr-on {
                    background: #4ade80;
                    box-shadow: 0 0 6px rgba(74, 222, 128, 0.55);
                }

                .ytr-menu-dropdown {
                    position: relative;
                }

                .ytr-dropdown-content {
                    display: none;
                    position: absolute;
                    top: calc(100% + 6px);
                    right: 0;
                    min-width: 210px;
                    padding: 5px;
                    background: #14161d;
                    border: 1px solid rgba(255, 255, 255, 0.09);
                    border-radius: 10px;
                    box-shadow: 0 14px 36px rgba(0, 0, 0, 0.6);
                    z-index: 2147483647;
                }

                .ytr-menu-open .ytr-dropdown-content {
                    display: block;
                    animation: ytrMenuIn 0.12s ease-out;
                }

                @keyframes ytrMenuIn {
                    from { opacity: 0; transform: translateY(-4px); }
                    to { opacity: 1; transform: none; }
                }

                .ytr-menu-item {
                    display: flex;
                    align-items: center;
                    justify-content: space-between;
                    gap: 14px;
                    padding: 7px 10px;
                    border-radius: 6px;
                    font-size: 12px;
                    color: rgba(255, 255, 255, 0.82);
                    text-decoration: none;
                    cursor: pointer;
                }

                .ytr-menu-item:hover {
                    background: rgba(255, 255, 255, 0.07);
                    color: #ffffff;
                }

                .ytr-menu-item.ytr-danger:hover {
                    background: rgba(239, 68, 68, 0.14);
                    color: #fca5a5;
                }

                .ytr-divider {
                    height: 1px;
                    margin: 5px 4px;
                    background: rgba(255, 255, 255, 0.07);
                }

                .ytr-menu-foot {
                    display: block;
                    padding: 6px 10px 4px;
                    font-size: 10.5px;
                    color: rgba(255, 255, 255, 0.35);
                    text-decoration: none;
                }

                .ytr-menu-foot:hover {
                    color: #ff5a1f;
                }
            `;

    const EQ_CSS = `
                /* Equalizer Modal & Backdrop */
                #ytr-eq-overlay {
                    position: fixed !important;
                    top: 0 !important;
                    left: 0 !important;
                    right: 0 !important;
                    bottom: 0 !important;
                    background: rgba(0, 0, 0, 0.70) !important;
                    backdrop-filter: blur(8px) !important;
                    z-index: 2147483646 !important;
                    display: none;
                    align-items: center !important;
                    justify-content: center !important;
                }

                #ytr-eq-overlay.ytr-open {
                    display: flex !important;
                }

                #ytr-eq-modal {
                    width: 620px !important;
                    max-width: 94vw !important;
                    background: #11141c !important;
                    border: 1px solid rgba(255, 255, 255, 0.16) !important;
                    border-radius: 14px !important;
                    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.85), 0 0 20px rgba(255, 61, 0, 0.15) !important;
                    padding: 22px 24px !important;
                    color: #ffffff !important;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif !important;
                    user-select: none !important;
                    display: flex !important;
                    flex-direction: column !important;
                    gap: 16px !important;
                    animation: ytrModalScale 0.18s cubic-bezier(0.16, 1, 0.3, 1) !important;
                }

                @keyframes ytrModalScale {
                    from { opacity: 0; transform: scale(0.95); }
                    to { opacity: 1; transform: scale(1); }
                }

                .ytr-eq-header {
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                }

                .ytr-eq-title-group {
                    display: flex !important;
                    align-items: center !important;
                    gap: 10px !important;
                }

                .ytr-eq-title {
                    font-size: 15px !important;
                    font-weight: 700 !important;
                    color: #ffffff !important;
                    display: flex !important;
                    align-items: center !important;
                    gap: 8px !important;
                }

                .ytr-eq-badge {
                    font-size: 11px !important;
                    font-weight: 600 !important;
                    padding: 2px 8px !important;
                    border-radius: 12px !important;
                    background: rgba(255, 61, 0, 0.16) !important;
                    color: #ff5722 !important;
                    border: 1px solid rgba(255, 61, 0, 0.40) !important;
                }

                .ytr-eq-header-actions {
                    display: flex !important;
                    align-items: center !important;
                    gap: 10px !important;
                }

                .ytr-eq-toggle-btn {
                    font-size: 11px !important;
                    font-weight: 700 !important;
                    padding: 5px 12px !important;
                    border-radius: 6px !important;
                    border: 1px solid rgba(255, 255, 255, 0.15) !important;
                    background: rgba(255, 255, 255, 0.08) !important;
                    color: #d1d5db !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-toggle-btn.ytr-active {
                    background: rgba(74, 222, 128, 0.18) !important;
                    border-color: rgba(74, 222, 128, 0.50) !important;
                    color: #4ade80 !important;
                }

                .ytr-eq-close-btn {
                    background: transparent !important;
                    border: none !important;
                    color: rgba(255, 255, 255, 0.55) !important;
                    font-size: 20px !important;
                    cursor: pointer !important;
                    padding: 0 4px !important;
                    line-height: 1 !important;
                    transition: color 0.15s ease !important;
                }

                .ytr-eq-close-btn:hover {
                    color: #ffffff !important;
                }

                .ytr-eq-presets-wrap {
                    display: flex !important;
                    flex-direction: column !important;
                    gap: 6px !important;
                }

                .ytr-eq-presets-label {
                    font-size: 10px !important;
                    font-weight: 700 !important;
                    color: rgba(255, 255, 255, 0.5) !important;
                    text-transform: uppercase !important;
                    letter-spacing: 0.5px !important;
                }

                .ytr-eq-presets-row {
                    display: flex !important;
                    flex-wrap: wrap !important;
                    align-items: center !important;
                    gap: 6px !important;
                }

                .ytr-eq-preset-chip {
                    font-size: 11px !important;
                    font-weight: 500 !important;
                    padding: 4px 10px !important;
                    border-radius: 6px !important;
                    border: 1px solid rgba(255, 255, 255, 0.1) !important;
                    background: rgba(255, 255, 255, 0.05) !important;
                    color: #9ca3af !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-preset-chip:hover {
                    background: rgba(255, 255, 255, 0.12) !important;
                    color: #ffffff !important;
                }

                .ytr-eq-preset-chip.ytr-selected {
                    background: linear-gradient(135deg, rgba(255, 61, 0, 0.35) 0%, rgba(255, 120, 50, 0.22) 100%) !important;
                    border-color: #ff3d00 !important;
                    color: #ffffff !important;
                    font-weight: 600 !important;
                    box-shadow: 0 0 10px rgba(255, 61, 0, 0.3) !important;
                }

                .ytr-eq-sliders-box {
                    display: flex !important;
                    justify-content: space-between !important;
                    align-items: center !important;
                    background: rgba(0, 0, 0, 0.40) !important;
                    border: 1px solid rgba(255, 255, 255, 0.07) !important;
                    border-radius: 10px !important;
                    padding: 16px 12px !important;
                    gap: 4px !important;
                }

                .ytr-eq-col {
                    display: flex !important;
                    flex-direction: column !important;
                    align-items: center !important;
                    flex: 1 !important;
                    gap: 8px !important;
                }

                .ytr-eq-db {
                    font-size: 10px !important;
                    font-family: ui-monospace, SFMono-Regular, Menlo, monospace !important;
                    color: #38bdf8 !important;
                    min-height: 14px !important;
                    text-align: center !important;
                }

                .ytr-eq-slider {
                    -webkit-appearance: slider-vertical !important;
                    writing-mode: bt-lr !important;
                    width: 20px !important;
                    height: 120px !important;
                    cursor: pointer !important;
                    background: transparent !important;
                    accent-color: #ff3d00 !important;
                    margin: 0 !important;
                }

                .ytr-eq-freq {
                    font-size: 10px !important;
                    font-weight: 600 !important;
                    color: #9ca3af !important;
                    text-align: center !important;
                }

                .ytr-eq-footer {
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                    font-size: 11px !important;
                    color: rgba(255, 255, 255, 0.45) !important;
                }

                .ytr-eq-reset-btn {
                    font-size: 11px !important;
                    background: rgba(255, 255, 255, 0.06) !important;
                    border: 1px solid rgba(255, 255, 255, 0.15) !important;
                    color: #d1d5db !important;
                    padding: 4px 11px !important;
                    border-radius: 4px !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-reset-btn:hover {
                    background: rgba(255, 255, 255, 0.14) !important;
                    color: #ffffff !important;
                }

                .ytr-fade-row {
                    display: flex;
                    align-items: center;
                    gap: 10px;
                    padding: 10px 12px;
                    background: rgba(255, 255, 255, 0.03);
                    border: 1px solid rgba(255, 255, 255, 0.07);
                    border-radius: 10px;
                }

                .ytr-fade-title {
                    display: flex;
                    flex-direction: column;
                    gap: 2px;
                    margin-right: auto;
                }

                .ytr-fade-title b {
                    font-size: 12.5px;
                    font-weight: 600;
                    color: #ffffff;
                }

                .ytr-fade-title span {
                    font-size: 11px;
                    color: rgba(255, 255, 255, 0.45);
                }

                .ytr-fade-chip {
                    padding: 5px 10px;
                    background: transparent;
                    border: 1px solid rgba(255, 255, 255, 0.12);
                    border-radius: 999px;
                    color: rgba(255, 255, 255, 0.7);
                    font: inherit;
                    font-size: 11px;
                    cursor: pointer;
                    transition: background 0.12s ease, border-color 0.12s ease, color 0.12s ease;
                }

                .ytr-fade-chip:hover {
                    border-color: rgba(255, 255, 255, 0.25);
                    color: #ffffff;
                }

                .ytr-fade-chip.ytr-selected {
                    background: rgba(74, 222, 128, 0.12);
                    border-color: rgba(74, 222, 128, 0.5);
                    color: #86efac;
                }

                .ytr-fade-row.ytr-off .ytr-fade-chip {
                    opacity: 0.4;
                }

                .ytr-eq-switch-label {
                    font-size: 12px;
                    color: rgba(255, 255, 255, 0.6);
                }

                .ytr-switch {
                    position: relative;
                    width: 34px;
                    height: 20px;
                    flex-shrink: 0;
                    padding: 0;
                    background: rgba(255, 255, 255, 0.14);
                    border: none;
                    border-radius: 999px;
                    cursor: pointer;
                    transition: background 0.15s ease;
                }

                .ytr-switch::after {
                    content: '';
                    position: absolute;
                    top: 3px;
                    left: 3px;
                    width: 14px;
                    height: 14px;
                    border-radius: 50%;
                    background: #ffffff;
                    transition: transform 0.15s ease;
                }

                .ytr-switch.ytr-on {
                    background: #22c55e;
                }

                .ytr-switch.ytr-on::after {
                    transform: translateX(14px);
                }
            `;

    function ensureStyles() {
        try {
            if (document.getElementById('ytr-adblock-styles')) return;
            const target = document.head || document.documentElement;
            if (!target) return;
            const style = document.createElement('style');
            style.id = 'ytr-adblock-styles';
            style.textContent = CORE_CSS;
            target.appendChild(style);
        } catch (_) {}
    }

    function ensureEqStyles() {
        try {
            if (document.getElementById('ytr-eq-styles')) return;
            const target = document.head || document.documentElement;
            if (!target) return;
            const style = document.createElement('style');
            style.id = 'ytr-eq-styles';
            style.textContent = EQ_CSS;
            target.appendChild(style);
        } catch (_) {}
    }

    // Attach the core sheet before the first paint so the 36px offset never shifts the layout.
    if (document.documentElement) {
        ensureStyles();
    } else {
        try {
            const earlyCss = new MutationObserver(() => {
                if (document.documentElement) {
                    earlyCss.disconnect();
                    ensureStyles();
                }
            });
            earlyCss.observe(document, { childList: true });
        } catch (_) {}
    }

    // 2. Ad Detection & Skipper (event driven: player class observer + media events; a 250 ms
    // timer exists only while an ad is on screen)
    const AD_SKIP_SEL = '.ytp-skip-ad-button, .ytp-ad-skip-button-modern, .ytp-ad-skip-button, button.ytp-ad-skip-button-slot, button.videoAdUiSkipButton';
    const AD_OVERLAY_SEL = '.ytp-ad-player-overlay, .ytp-ad-player-overlay-layout, .video-ads';

    let playerRef = null;
    let ytmPlayerRef = null;
    let videoRef = null;

    function getPlayer() {
        if (playerRef && playerRef.isConnected) return playerRef;
        playerRef = document.getElementById('movie_player');
        return playerRef;
    }

    function getYtmPlayer() {
        if (ytmPlayerRef && ytmPlayerRef.isConnected) return ytmPlayerRef;
        const player = getPlayer();
        ytmPlayerRef = player ? player.closest('ytmusic-player') : document.querySelector('ytmusic-player');
        return ytmPlayerRef;
    }

    function findMainVideo() {
        const p = getPlayer();
        let v = p ? (p.querySelector('video.html5-main-video') || p.querySelector('video')) : null;
        if (!v) {
            const yp = getYtmPlayer();
            if (yp) v = yp.querySelector('video');
        }
        return v;
    }

    // The player's own <video>, never an unrelated media element.
    function getMainVideo() {
        if (videoRef && videoRef.isConnected) return videoRef;
        videoRef = findMainVideo();
        return videoRef;
    }

    function getVideo() {
        return getMainVideo() || document.querySelector('video');
    }

    // Media events are caught at document level (capture), which survives <video> swaps.
    function mediaTarget(e) {
        const t = e.target;
        if (t === videoRef) return t;
        if (!(t instanceof HTMLMediaElement)) return null;
        if (t.closest('#movie_player, ytmusic-player') || !findMainVideo()) {
            videoRef = t;
            return t;
        }
        return null;
    }

    let adObs = null;
    let adObsPlayer = null;
    let adObsYtm = null;
    let adTimer = 0;
    let adActive = false;
    let adWasMuted = false;
    let userWasMuted = false;
    let adSeekKey = '';
    let adActionAt = -1e9;

    function isAdNow() {
        const player = getPlayer();
        if (player) {
            const cl = player.classList;
            if (cl.contains('ad-showing') || cl.contains('ad-interrupting') || player.hasAttribute('ad-interrupting')) {
                return true;
            }
            try {
                if (typeof player.getAdState === 'function' && player.getAdState() > 0) return true;
                if (typeof player.isAd === 'function' && player.isAd()) return true;
            } catch (_) {}
        }
        const ytmPlayer = getYtmPlayer();
        if (ytmPlayer && ytmPlayer.hasAttribute('ad-interrupting')) return true;
        const video = getMainVideo();
        return !!(video && video.classList.contains('ad-showing'));
    }

    // click() works on display:none elements, so the CSS that hides the skip container
    // does not stop this. No visibility probe: that would force a layout per tick.
    function clickAdSkip(player) {
        try {
            let b = player ? player.querySelector(AD_SKIP_SEL) : null;
            if (!b) b = document.querySelector(AD_SKIP_SEL);
            if (b && !b.disabled && !b.hasAttribute('disabled')) {
                b.click();
                return true;
            }
        } catch (_) {}
        return false;
    }

    function adStep(act) {
        const video = getVideo();
        if (!video) return;
        const player = getPlayer();
        adActive = true;

        // 1. Visually hide it immediately so user NEVER sees any ad frames
        if (video.style.opacity !== '0') video.style.opacity = '0';
        const adOverlays = (player || document).querySelectorAll(AD_OVERLAY_SEL);
        for (let i = 0; i < adOverlays.length; i++) {
            if (adOverlays[i].style.display !== 'none') adOverlays[i].style.display = 'none';
        }

        // 2. Mute audio completely (re-applied: the player may restore its own mute state when
        // it loads the next ad of a pod)
        if (!adWasMuted) {
            userWasMuted = video.muted;
            adWasMuted = true;
        }
        if (!video.muted) video.muted = true;

        if (act) {
            // 3. Try calling player API skipAd()
            try {
                if (player && typeof player.skipAd === 'function') {
                    player.skipAd();
                }
            } catch (_) {}

            // 4. Click skip button if present
            clickAdSkip(player);
        }

        // 5. Jump to the end of the ad stream once per stream: every completed seek queues a
        // timeupdate, so an unconditional seek here would feed itself.
        try {
            const d = video.duration;
            if (Number.isFinite(d) && d > 0 && !video.seeking && video.currentTime < d - 0.3) {
                const key = video.currentSrc + '|' + d;
                if (key !== adSeekKey) {
                    adSeekKey = key;
                    video.currentTime = d;
                }
            }
            if (video.playbackRate !== 16) video.playbackRate = 16.0;
        } catch (_) {}
    }

    function adRestore() {
        // Normal song playback: restore full visibility, normal speed, and unmute
        const video = getVideo();
        if (video) {
            if (video.style.opacity === '0') video.style.opacity = '1';
            if (video.playbackRate > 2.0) video.playbackRate = 1.0;
            if (adWasMuted) {
                if (!userWasMuted) video.muted = false;
                adWasMuted = false;
            }
        }
        adSeekKey = '';
        if (adActive) {
            adActive = false;
            queueState(0);
        }
    }

    function adTick() {
        if (isAdNow()) {
            // skipAd() and the skip click can rewrite the observed class attribute synchronously;
            // rate-limiting them keeps observer -> adStep -> observer from spinning in microtasks.
            // Hiding and muting stay immediate: they only write when a value differs.
            const now = performance.now();
            const act = now - adActionAt >= 100;
            if (act) adActionAt = now;
            adStep(act);
            // The timer ends itself through isAdNow(), so a missed class change can never
            // leave a song muted, invisible and at 16x.
            if (!adTimer) adTimer = setInterval(adTick, 250);
        } else {
            if (adTimer) {
                clearInterval(adTimer);
                adTimer = 0;
            }
            adRestore();
        }
    }

    function attachAdObserver() {
        const player = getPlayer();
        const ytmPlayer = getYtmPlayer();
        if (player === adObsPlayer && ytmPlayer === adObsYtm) return !!player;
        if (!adObs) adObs = new MutationObserver(adTick);
        adObs.disconnect();
        if (player) adObs.observe(player, { attributes: true, attributeFilter: ['class', 'ad-interrupting'] });
        if (ytmPlayer && ytmPlayer !== player) adObs.observe(ytmPlayer, { attributes: true, attributeFilter: ['ad-interrupting'] });
        adObsPlayer = player;
        adObsYtm = ytmPlayer;
        return !!player;
    }

    function isPromoDialog(dialog) {
        let promo = false;
        for (let c = dialog.firstElementChild; c; c = c.nextElementSibling) {
            const tag = c.tagName;
            if (tag === 'YTMUSIC-UPSELL-DIALOG-RENDERER' || tag === 'YTMUSIC-MEALBAR-PROMO-RENDERER') promo = true;
            else if (tag.indexOf('-RENDERER') !== -1) return false;
        }
        return promo;
    }

    // Hiding an open modal with CSS would leave its backdrop and scroll lock behind; close it.
    function onOverlayOpened(e) {
        const d = e.composedPath ? e.composedPath()[0] : e.target;
        if (d && d.tagName === 'TP-YT-PAPER-DIALOG' && isPromoDialog(d)) {
            try {
                d.close();
            } catch (_) {}
        }
    }

    // 3. Sidebar "Upgrade" entry: matched by its endpoint, not by text, so user playlists
    // whose names contain "upgrade" stay visible. Toggled both ways because dom-repeat
    // reuses entry elements.
    const guideEntries = document.getElementsByTagName('ytmusic-guide-entry-renderer');
    let guideObs = null;
    let guideRoots = [];
    let guideScanT = 0;

    function isUpgradeEntry(entry) {
        const d = entry.data;
        if (d && typeof d === 'object') {
            const ep = d.navigationEndpoint && d.navigationEndpoint.browseEndpoint;
            if (ep && ep.browseId === 'SPunlimited') return true;
            return !!(d.icon && d.icon.iconType === 'TAB_MUSIC_PREMIUM');
        }
        const t = entry.querySelector('.title');
        return !!t && t.textContent.trim() === 'Upgrade';
    }

    function hideUpgradeEntries() {
        for (let i = 0; i < guideEntries.length; i++) {
            const entry = guideEntries[i];
            const hide = isUpgradeEntry(entry);
            if (hide !== entry.hasAttribute('ytr-hide')) entry.toggleAttribute('ytr-hide', hide);
        }
    }

    function scheduleGuideScan() {
        if (document.hidden || guideScanT) return;
        guideScanT = setTimeout(() => {
            guideScanT = 0;
            try {
                hideUpgradeEntries();
            } catch (_) {}
        }, 250);
    }

    function attachGuideObserver() {
        const roots = document.getElementsByTagName('ytmusic-guide-renderer');
        if (!roots.length) return false;
        let same = roots.length === guideRoots.length;
        for (let i = 0; same && i < roots.length; i++) same = roots[i] === guideRoots[i];
        if (same) return true;
        if (!guideObs) guideObs = new MutationObserver(scheduleGuideScan);
        guideObs.disconnect();
        guideRoots = Array.prototype.slice.call(roots);
        for (let i = 0; i < guideRoots.length; i++) {
            guideObs.observe(guideRoots[i], { childList: true, subtree: true });
        }
        scheduleGuideScan();
        return true;
    }

    // 4. Safe DOM Construction Helpers (Pure DOM Nodes - 100% immune to Trusted Types CSP restrictions)
    function createEl(tag, attrs, text) {
        const el = document.createElement(tag);
        if (attrs) {
            for (const k in attrs) {
                if (k === 'style') el.style.cssText = attrs[k];
                else if (k === 'className') el.className = attrs[k];
                else el.setAttribute(k, attrs[k]);
            }
        }
        if (text) el.textContent = text;
        return el;
    }

    function createSvg(width, height, viewBox, innerShapes) {
        const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        svg.setAttribute('viewBox', viewBox);
        svg.setAttribute('width', String(width));
        svg.setAttribute('height', String(height));
        svg.style.width = width + 'px';
        svg.style.height = height + 'px';
        svg.style.flexShrink = '0';
        if (innerShapes) {
            for (let i = 0; i < innerShapes.length; i++) {
                const s = document.createElementNS('http://www.w3.org/2000/svg', innerShapes[i].tag);
                const a = innerShapes[i].attrs;
                if (a) {
                    for (const k in a) {
                        s.setAttribute(k, a[k]);
                    }
                }
                svg.appendChild(s);
            }
        }
        return svg;
    }

    function focusSearchBox() {
        try {
            const searchBtn = document.querySelector('ytmusic-search-box tp-yt-paper-icon-button, ytmusic-search-box [aria-label*="Search" i], tp-yt-paper-icon-button#search-button');
            if (searchBtn) searchBtn.click();
            setTimeout(() => {
                const input = document.querySelector('ytmusic-search-box input, input#input, input[type="search"]');
                if (input) {
                    input.focus();
                    input.select();
                }
            }, 60);
        } catch (_) {}
    }

    // --- Studio-Grade Web Audio Equalizer (10-Band Biquad Filters & Presets) ---
    const EQ_FREQUENCIES = [32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
    const EQ_FREQ_LABELS = ['32', '64', '125', '250', '500', '1k', '2k', '4k', '8k', '16k'];
    const EQ_PRESETS = {
        'Spatial':     [4, 3, 1, 0, -1, 1, 2, 3, 4, 5],
        'Studio':      [3, 2, 1, 0, 1, 2, 2, 2, 3, 3],
        'Cinema':      [5, 4, 2, -1, 0, 1, 2, 3, 4, 3],
        'Bass Boost':  [6, 5, 4, 2, 0, 0, 0, 1, 1, 1],
        'Vocal':       [-2, -1, 0, 1, 3, 3, 2, 1, 0, 0],
        'Rock':        [4, 3, 1, -1, -1, 1, 2, 3, 4, 4],
        'Gaming':      [3, 2, 0, -2, 1, 2, 4, 3, 2, 1],
        'Flat':        [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    };
    const DEFAULT_PRESET = 'Spatial';
    // Preset names used before 4.2; saved selections are migrated on load.
    const LEGACY_PRESETS = { 'Dolby Atmos': 'Spatial', 'Dolby Music': 'Studio', 'Dolby Movie': 'Cinema' };

    function loadPresetName() {
        let name = lsGet('ytr_eq_preset');
        if (name && LEGACY_PRESETS[name]) {
            name = LEGACY_PRESETS[name];
            try { localStorage.setItem('ytr_eq_preset', name); } catch (_) {}
        }
        return name || DEFAULT_PRESET;
    }

    function isValidGains(g) {
        if (!Array.isArray(g) || g.length !== 10) return false;
        for (let i = 0; i < 10; i++) {
            if (typeof g[i] !== 'number' || !isFinite(g[i])) return false;
        }
        return true;
    }

    window.__ytr_audio = window.__ytr_audio || {
        ctx: null,
        source: null,
        filters: [],
        fadeGain: null,
        video: null,
        masterEnabled: lsGet('ytr_eq_enabled') !== 'false',
        presetName: loadPresetName(),
        gains: null
    };

    const A = window.__ytr_audio;
    if (LEGACY_PRESETS[A.presetName]) A.presetName = LEGACY_PRESETS[A.presetName];
    if (!isValidGains(A.gains)) {
        A.gains = null;
        try {
            const saved = lsGet('ytr_eq_gains');
            if (saved) A.gains = JSON.parse(saved);
        } catch (_) {}
        if (!isValidGains(A.gains)) {
            A.gains = [...(EQ_PRESETS[A.presetName] || EQ_PRESETS[DEFAULT_PRESET])];
        }
    }
    if (!A.sources) A.sources = new WeakMap();
    if (!A.failed) A.failed = new WeakSet();
    if (!A.stale) A.stale = [];
    if (typeof A.fadeInAt !== 'number') A.fadeInAt = -1;
    A.pending = A.pending || null;

    let smoothAudioEnabled = lsGet('ytr_smooth_audio') === 'true';
    // Fade length in seconds (Quick / Smooth / Long in the audio panel).
    const FADE_CHOICES = [{ label: 'Quick', sec: 0.25 }, { label: 'Smooth', sec: 0.5 }, { label: 'Long', sec: 1.0 }];
    let fadeSec = (() => {
        const v = parseFloat(lsGet('ytr_fade_sec'));
        return Number.isFinite(v) && v >= 0.1 && v <= 2 ? v : 0.5;
    })();
    // Exponential ramps cannot reach 0; stopping at -60 dB keeps the audible part of the curve
    // spread over the whole fade (the pause itself cuts the rest).
    const FADE_FLOOR = 0.001;
    function fadeOutSec() { return fadeSec; }
    function fadeInSec() { return Math.min(2, fadeSec * 1.2); }
    // A skip should feel responsive even with long fades.
    function skipOutSec() { return Math.min(fadeSec, 0.6); }

    function eqIsActive() {
        if (!A.masterEnabled) return false;
        for (let i = 0; i < 10; i++) {
            if (Math.abs(A.gains[i]) >= 0.05) return true;
        }
        return false;
    }

    // Holds param at its current value until t0, then ramps to target over dur seconds
    // (dur 0: jump now, used while the context is not rendering).
    function rampParam(param, target, t0, dur) {
        const now = A.ctx.currentTime;
        try {
            param.cancelScheduledValues(now);
            if (dur > 0) {
                const cur = param.value;
                param.setValueAtTime(cur, now);
                if (t0 > now) param.setValueAtTime(cur, t0);
                param.linearRampToValueAtTime(target, Math.max(t0, now) + dur);
            } else {
                param.setValueAtTime(target, now);
            }
        } catch (_) {
            param.value = target;
        }
    }

    // Ramps are exponential (linear in dB), which the ear hears as an even fade; a linear
    // gain ramp sounds like a late, abrupt cut.
    function rampFade(target, dur, from) {
        if (!A.ctx || !A.fadeGain) return;
        const g = A.fadeGain.gain;
        const now = A.ctx.currentTime;
        const start = Math.max(FADE_FLOOR, from === undefined ? g.value : from);
        const end = Math.max(FADE_FLOOR, target);
        try {
            g.cancelScheduledValues(now);
            g.setValueAtTime(start, now);
            g.exponentialRampToValueAtTime(end, now + Math.max(0.01, dur));
        } catch (_) {
            g.value = end;
        }
    }

    function connectEqChain() {
        try { A.filters[9].connect(A.wetGain); } catch (_) {}
        if (A.source) {
            try { A.source.connect(A.filters[0]); } catch (_) {}
        }
        A.eqChainOn = true;
    }

    function disconnectEqChain() {
        if (A.source) {
            try { A.source.disconnect(A.filters[0]); } catch (_) {}
        }
        try { A.filters[9].disconnect(A.wetGain); } catch (_) {}
        A.eqChainOn = false;
    }

    // Wet and dry always sum to 1, so a crossfade between identical signals is silent.
    function setEqMix(wet, t0, dur) {
        rampParam(A.wetGain.gain, wet ? 1 : 0, t0, dur);
        rampParam(A.dryGain.gain, wet ? 0 : 1, t0, dur);
        A.eqWet = wet;
        A.eqMixEnd = Math.max(t0, A.ctx.currentTime) + dur;
    }

    function scheduleEqRelease() {
        if (A.bypassTimer) clearTimeout(A.bypassTimer);
        A.bypassTimer = setTimeout(function release() {
            A.bypassTimer = 0;
            if (A.eqWet || !A.eqChainOn || eqIsActive() || !A.ctx) return;
            if (A.ctx.state === 'running') {
                if (A.ctx.currentTime < A.eqMixEnd + 0.01) {
                    A.bypassTimer = setTimeout(release, 100);
                    return;
                }
            } else {
                // Not rendering, so snapping to the final mix cannot click.
                setEqMix(false, A.ctx.currentTime, 0);
            }
            disconnectEqChain();
        }, 150);
    }

    // Applies A.gains / A.masterEnabled to the graph (band -1 = all bands). When the EQ is
    // OFF or flat the filter chain is crossfaded out and disconnected so the biquads stop
    // running; it is reconnected while still at 0 dB (identity) before fading back in.
    function refreshEqAudio(band) {
        const ctx = A.ctx;
        if (!ctx || !A.wetGain || A.filters.length !== 10) return;
        const running = ctx.state === 'running';
        const now = ctx.currentTime;
        const want = eqIsActive();
        let t0 = now;
        if (want) {
            if (A.bypassTimer) {
                clearTimeout(A.bypassTimer);
                A.bypassTimer = 0;
            }
            if (!A.eqChainOn) connectEqChain();
            if (!A.eqWet) {
                t0 = running ? now + 0.02 : now;
                setEqMix(true, t0, running ? 0.025 : 0);
            }
        }
        const first = band >= 0 ? band : 0;
        const last = band >= 0 ? band : 9;
        for (let i = first; i <= last; i++) {
            rampParam(A.filters[i].gain, A.masterEnabled ? A.gains[i] : 0.0, t0, running ? 0.05 : 0);
        }
        if (!want) {
            if (A.eqWet) setEqMix(false, now + (running ? 0.05 : 0), running ? 0.025 : 0);
            if (A.eqChainOn) {
                if (running) scheduleEqRelease();
                else disconnectEqChain();
            }
        }
    }

    function ensureAudioCtx() {
        if (A.ctx) return A.ctx;
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (!AudioContextClass) return null;
        let ctx = null;
        try {
            // 'playback' uses larger render buffers: fewer audio-thread wakeups for ~10 ms latency.
            ctx = new AudioContextClass({ latencyHint: 'playback' });
        } catch (_) {
            try {
                ctx = new AudioContextClass();
            } catch (e) {
                console.warn('[ytr-audio] AudioContext error:', e);
                return null;
            }
        }
        A.ctx = ctx;

        // Source -> DryGain ------------------------------------> FadeGain -> Destination
        // Source -> Filter[0] -> ... -> Filter[9] -> WetGain --->   (only while the EQ is active)
        A.filters = [];
        for (let i = 0; i < EQ_FREQUENCIES.length; i++) {
            const filter = ctx.createBiquadFilter();
            if (i === 0) {
                filter.type = 'lowshelf';
            } else if (i === EQ_FREQUENCIES.length - 1) {
                filter.type = 'highshelf';
            } else {
                filter.type = 'peaking';
                filter.Q.value = 1.4;
            }
            filter.frequency.value = EQ_FREQUENCIES[i];
            filter.gain.value = A.masterEnabled ? A.gains[i] : 0.0;
            if (i > 0) A.filters[i - 1].connect(filter);
            A.filters.push(filter);
        }

        // Dedicated GainNode for Smooth Audio Fading
        A.fadeGain = ctx.createGain();
        A.fadeGain.gain.value = 1.0;
        A.dryGain = ctx.createGain();
        A.wetGain = ctx.createGain();
        A.dryGain.connect(A.fadeGain);
        A.wetGain.connect(A.fadeGain);
        A.fadeGain.connect(ctx.destination);

        A.eqChainOn = false;
        A.eqWet = eqIsActive();
        A.wetGain.gain.value = A.eqWet ? 1 : 0;
        A.dryGain.gain.value = A.eqWet ? 0 : 1;
        if (A.eqWet) connectEqChain();

        A.ctxStarted = ctx.state === 'running';
        ctx.addEventListener('statechange', onCtxStateChange);

        window.__ytr_audioCtx = ctx;
        window.__ytr_eqFilters = A.filters;
        window.__ytr_fadeGain = A.fadeGain;
        return ctx;
    }

    function sweepStaleSources() {
        for (let i = A.stale.length - 1; i >= 0; i--) {
            const el = A.stale[i];
            if (el === A.video) {
                A.stale.splice(i, 1);
                continue;
            }
            // A captured element is only audible through the graph: never cut one that may still play.
            if (!el.isConnected && (el.paused || el.ended)) {
                const src = A.sources.get(el);
                if (src) {
                    try { src.disconnect(); } catch (_) {}
                }
                A.stale.splice(i, 1);
            }
        }
    }

    function initAudioGraph(video) {
        if (!video || A.video === video || A.failed.has(video)) return;
        const ctx = ensureAudioCtx();
        if (!ctx) return;

        let src = A.sources.get(video);
        if (!src) {
            if (!A.ctxStarted && ctx.state !== 'running') {
                // Capturing the element into a context the autoplay policy has not let start
                // would mute it; attach once the context runs (onCtxStateChange).
                A.waitingVideo = video;
                if (!video.paused) ctx.resume().catch(() => {});
                return;
            }
            try {
                src = ctx.createMediaElementSource(video);
                A.sources.set(video, src);
            } catch (err) {
                A.failed.add(video);
                console.warn('[ytr-audio] createMediaElementSource warning:', err);
                return;
            }
        }

        try { src.connect(A.dryGain); } catch (_) {}
        if (A.eqChainOn) {
            try { src.connect(A.filters[0]); } catch (_) {}
        }
        const oldVideo = A.video;
        A.video = video;
        A.source = src;
        A.waitingVideo = null;
        if (oldVideo && A.stale.indexOf(oldVideo) === -1) A.stale.push(oldVideo);
        sweepStaleSources();
    }

    function wakeCtx() {
        if (A.idleT) {
            clearTimeout(A.idleT);
            A.idleT = 0;
        }
        const ctx = A.ctx;
        if (ctx && ctx.state !== 'running' && ctx.state !== 'closed') ctx.resume().catch(() => {});
    }

    // A running context keeps the audio thread rendering silence; stop it after a short idle pause.
    function armIdleSuspend() {
        // Not rendering already; onCtxStateChange re-arms if a resume lands while paused.
        if (!A.ctx || A.ctx.state !== 'running') return;
        if (A.idleT) clearTimeout(A.idleT);
        A.idleT = setTimeout(() => {
            A.idleT = 0;
            const ctx = A.ctx;
            const v = A.video;
            if (!ctx || ctx.state !== 'running' || A.pending) return;
            if (v && (!v.paused || v.seeking)) return;
            sweepStaleSources();
            ctx.suspend().catch(() => {});
        }, 3000);
    }

    function onCtxStateChange() {
        const ctx = A.ctx;
        if (!ctx || ctx.state !== 'running') return;
        A.ctxStarted = true;
        const waiting = A.waitingVideo;
        if (waiting) {
            A.waitingVideo = null;
            if (waiting.isConnected && !waiting.paused) initAudioGraph(waiting);
        }
        const v = A.video;
        if (!A.idleT && !A.pending && (!v || v.paused)) armIdleSuspend();
    }

    // Only resumes for media that is actually playing; a click while paused must not undo
    // the idle suspend.
    function unlockAudio() {
        const ctx = A.ctx;
        if (!ctx || ctx.state === 'running' || ctx.state === 'closed') return;
        const v = A.waitingVideo || A.video;
        if (v && !v.paused) ctx.resume().catch(() => {});
    }

    function formatDb(val) {
        return (val > 0 ? '+' : '') + val + ' dB';
    }

    function saveEqSettings() {
        lsSet('ytr_eq_gains', JSON.stringify(A.gains));
        lsSet('ytr_eq_preset', A.presetName);
    }

    let eqRaf = 0;
    const eqQueuedBands = [];

    function flushEqBands() {
        eqRaf = 0;
        for (let k = 0; k < eqQueuedBands.length; k++) {
            const i = eqQueuedBands[k];
            refreshEqAudio(i);
            const dbText = document.getElementById(`ytr-eq-db-${i}`);
            if (dbText) dbText.textContent = formatDb(A.gains[i]);
        }
        eqQueuedBands.length = 0;
    }

    function queueEqBand(i) {
        if (eqQueuedBands.indexOf(i) === -1) eqQueuedBands.push(i);
        if (!eqRaf) eqRaf = requestAnimationFrame(flushEqBands);
    }

    function applyEqGains(gains, presetName) {
        if (eqRaf) {
            cancelAnimationFrame(eqRaf);
            eqRaf = 0;
            eqQueuedBands.length = 0;
        }
        A.gains = [...gains];
        if (presetName) A.presetName = presetName;
        lsSet('ytr_eq_gains', JSON.stringify(A.gains));
        if (presetName) lsSet('ytr_eq_preset', A.presetName);
        refreshEqAudio(-1);
        updateEqualizerModalUI();
        updateEqButtonUI();
    }

    function setEqMasterEnabled(enabled) {
        A.masterEnabled = !!enabled;
        lsSet('ytr_eq_enabled', A.masterEnabled ? 'true' : 'false');
        applyEqGains(A.gains, A.presetName);
    }

    function updateEqButtonUI() {
        const btn = document.getElementById('ytr-btn-eq');
        const text = document.getElementById('ytr-btn-eq-text');
        if (!btn) return;
        btn.classList.toggle('ytr-eq-on', !!A.masterEnabled);
        if (text) {
            const label = A.masterEnabled ? A.presetName : 'EQ off';
            if (text.textContent !== label) text.textContent = label;
        }
    }

    function updateEqualizerModalUI() {
        const overlay = document.getElementById('ytr-eq-overlay');
        if (!overlay) return;
        const badge = document.getElementById('ytr-eq-preset-badge');
        if (badge) badge.textContent = A.presetName;

        const toggleBtn = document.getElementById('ytr-eq-master-toggle');
        if (toggleBtn) toggleBtn.classList.toggle('ytr-on', !!A.masterEnabled);

        const chips = overlay.querySelectorAll('.ytr-eq-preset-chip');
        chips.forEach(chip => {
            const pName = chip.getAttribute('data-preset');
            if (pName === A.presetName) chip.classList.add('ytr-selected');
            else chip.classList.remove('ytr-selected');
        });

        for (let i = 0; i < 10; i++) {
            const slider = document.getElementById(`ytr-eq-slider-${i}`);
            const dbText = document.getElementById(`ytr-eq-db-${i}`);
            const val = A.gains[i];
            if (slider && parseFloat(slider.value) !== val) {
                slider.value = String(val);
            }
            if (dbText) {
                dbText.textContent = formatDb(val);
            }
        }
    }

    function toggleEqualizerModal() {
        let overlay = document.getElementById('ytr-eq-overlay');
        if (!overlay) {
            overlay = buildEqualizerModal();
        }
        if (overlay) {
            const isOpen = overlay.classList.toggle('ytr-open');
            if (isOpen) {
                updateEqualizerModalUI();
                updateSmoothAudioUI();
            }
        }
    }

    function buildEqualizerModal() {
        let overlay = document.getElementById('ytr-eq-overlay');
        if (overlay) return overlay;
        if (!document.body) return null;

        overlay = createEl('div', { id: 'ytr-eq-overlay' });
        const modal = createEl('div', { id: 'ytr-eq-modal' });

        // 1. Header
        const header = createEl('div', { className: 'ytr-eq-header' });
        const titleGrp = createEl('div', { className: 'ytr-eq-title-group' });
        const titleSvg = createSvg(16, 16, '0 0 24 24', [
            { tag: 'path', attrs: { d: 'M10 20h4V4h-4v16zm-6 0h4v-8H4v8zM16 9v11h4V9h-4z', fill: '#ff3d00' } }
        ]);
        const titleSpan = createEl('span', { className: 'ytr-eq-title' });
        titleSpan.appendChild(titleSvg);
        titleSpan.appendChild(document.createTextNode('Audio'));
        const badge = createEl('span', { id: 'ytr-eq-preset-badge', className: 'ytr-eq-badge' }, A.presetName);
        titleGrp.appendChild(titleSpan);
        titleGrp.appendChild(badge);

        const actions = createEl('div', { className: 'ytr-eq-header-actions' });
        const toggleBtn = createEl('button', {
            id: 'ytr-eq-master-toggle',
            className: 'ytr-switch' + (A.masterEnabled ? ' ytr-on' : ''),
            title: 'Equalizer on/off'
        });
        toggleBtn.addEventListener('click', () => {
            setEqMasterEnabled(!A.masterEnabled);
        });

        const closeBtn = createEl('button', { className: 'ytr-eq-close-btn', title: 'Close (Esc)' }, '\u2715');
        closeBtn.addEventListener('click', () => overlay.classList.remove('ytr-open'));

        actions.appendChild(createEl('span', { className: 'ytr-eq-switch-label' }, 'Equalizer'));
        actions.appendChild(toggleBtn);
        actions.appendChild(closeBtn);

        header.appendChild(titleGrp);
        header.appendChild(actions);

        // Smooth fades: on/off and length
        const fadeRow = createEl('div', { id: 'ytr-fade-row', className: 'ytr-fade-row' });
        const fadeTitle = createEl('div', { className: 'ytr-fade-title' });
        fadeTitle.appendChild(createEl('b', {}, 'Smooth fades'));
        fadeTitle.appendChild(createEl('span', {}, 'Fade out and in on pause, resume and skip'));
        fadeRow.appendChild(fadeTitle);
        for (let i = 0; i < FADE_CHOICES.length; i++) {
            const c = FADE_CHOICES[i];
            const chip = createEl('button', {
                className: 'ytr-fade-chip',
                'data-sec': String(c.sec),
                title: c.label + ' fade (' + c.sec + ' s)'
            }, c.label);
            chip.addEventListener('click', () => setFadeLength(c.sec));
            fadeRow.appendChild(chip);
        }
        const fadeSwitch = createEl('button', { id: 'ytr-fade-switch', className: 'ytr-switch', title: 'Smooth fades on/off' });
        fadeSwitch.addEventListener('click', toggleSmoothAudio);
        fadeRow.appendChild(fadeSwitch);

        // 2. Presets Row
        const presetsWrap = createEl('div', { className: 'ytr-eq-presets-wrap' });
        const presetsLabel = createEl('div', { className: 'ytr-eq-presets-label' }, 'Presets');
        const presetsRow = createEl('div', { className: 'ytr-eq-presets-row' });

        for (const pName in EQ_PRESETS) {
            const chip = createEl('button', {
                className: 'ytr-eq-preset-chip' + (pName === A.presetName ? ' ytr-selected' : ''),
                'data-preset': pName
            }, pName);
            chip.addEventListener('click', () => {
                applyEqGains(EQ_PRESETS[pName], pName);
            });
            presetsRow.appendChild(chip);
        }
        presetsWrap.appendChild(presetsLabel);
        presetsWrap.appendChild(presetsRow);

        // 3. 10 Vertical Sliders Box
        const slidersBox = createEl('div', { className: 'ytr-eq-sliders-box' });
        for (let i = 0; i < 10; i++) {
            const col = createEl('div', { className: 'ytr-eq-col' });
            const val = A.gains[i];
            const dbText = createEl('span', { id: `ytr-eq-db-${i}`, className: 'ytr-eq-db' }, formatDb(val));
            const slider = createEl('input', {
                id: `ytr-eq-slider-${i}`,
                type: 'range',
                min: '-12',
                max: '12',
                step: '0.5',
                value: String(val),
                orient: 'vertical',
                className: 'ytr-eq-slider',
                title: `${EQ_FREQ_LABELS[i]}Hz (${val} dB)`
            });
            // Drag updates are applied once per frame; storage is written on release.
            slider.addEventListener('input', (e) => {
                const v = parseFloat(e.target.value);
                if (!Number.isFinite(v)) return;
                A.gains[i] = v;
                if (A.presetName !== 'Custom') {
                    A.presetName = 'Custom';
                    updateEqualizerModalUI();
                    updateEqButtonUI();
                }
                queueEqBand(i);
            });
            slider.addEventListener('change', saveEqSettings);
            const freqText = createEl('span', { className: 'ytr-eq-freq' }, EQ_FREQ_LABELS[i]);

            col.appendChild(dbText);
            col.appendChild(slider);
            col.appendChild(freqText);
            slidersBox.appendChild(col);
        }

        // 4. Footer
        const footer = createEl('div', { className: 'ytr-eq-footer' });
        const hint = createEl('span', {}, '10-band equalizer \u2022 32 Hz \u2013 16 kHz \u2022 bypassed when off or flat');
        const resetBtn = createEl('button', { className: 'ytr-eq-reset-btn' }, 'Reset to Flat (0 dB)');
        resetBtn.addEventListener('click', () => {
            applyEqGains(EQ_PRESETS['Flat'], 'Flat');
        });
        footer.appendChild(hint);
        footer.appendChild(resetBtn);

        modal.appendChild(header);
        modal.appendChild(fadeRow);
        modal.appendChild(presetsWrap);
        modal.appendChild(slidersBox);
        modal.appendChild(footer);

        overlay.appendChild(modal);

        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) overlay.classList.remove('ytr-open');
        });

        // #ytr-eq-overlay { display: none } lives in this sheet, so it must exist before the overlay.
        ensureEqStyles();
        document.body.appendChild(overlay);
        return overlay;
    }

    // --- Studio-Grade Smooth Audio Transitions & Playback Control ---
    function updateSmoothAudioUI() {
        try {
            const dot = document.getElementById('ytr-fade-dot');
            if (dot) dot.classList.toggle('ytr-on', smoothAudioEnabled);
            const audioBtn = document.getElementById('ytr-btn-eq');
            if (audioBtn) audioBtn.title = 'Audio: equalizer and smooth fades (Ctrl+Alt+E)\nSmooth fades: ' +
                (smoothAudioEnabled ? 'on, ' + fadeSec + ' s' : 'off');
            const sw = document.getElementById('ytr-fade-switch');
            if (sw) sw.classList.toggle('ytr-on', smoothAudioEnabled);
            const row = document.getElementById('ytr-fade-row');
            if (row) {
                row.classList.toggle('ytr-off', !smoothAudioEnabled);
                const chips = row.querySelectorAll('.ytr-fade-chip');
                for (let i = 0; i < chips.length; i++) {
                    chips[i].classList.toggle('ytr-selected', parseFloat(chips[i].getAttribute('data-sec')) === fadeSec);
                }
            }
        } catch (_) {}
    }

    function setFadeLength(sec) {
        fadeSec = sec;
        lsSet('ytr_fade_sec', String(sec));
        // Picking a length is a clear sign the user wants fades.
        if (!smoothAudioEnabled) toggleSmoothAudio();
        else updateSmoothAudioUI();
    }

    function toggleSmoothAudio() {
        smoothAudioEnabled = !smoothAudioEnabled;
        lsSet('ytr_smooth_audio', smoothAudioEnabled ? 'true' : 'false');
        if (!smoothAudioEnabled) {
            // A pause or skip fade in flight would otherwise leave the gain parked low.
            flushPending();
            if (A.fadeGain && A.fadeGain.gain.value < 0.99) rampFade(1.0, 0.03);
        }
        updateSmoothAudioUI();
    }

    function inFadeGraph(v) {
        return !!(A.fadeGain && v && A.video === v);
    }

    // After a fade-out the gain stays low until playback starts again ('play' handler).
    function fadeInOnPlay(v) {
        if (!inFadeGraph(v) || A.pending) return;
        const now = A.ctx.currentTime;
        if (smoothAudioEnabled) {
            // playerPlayPause may already have started this ramp.
            if (A.fadeInAt >= 0 && now - A.fadeInAt < fadeInSec()) return;
            rampFade(1.0, fadeInSec(), FADE_FLOOR);
            A.fadeInAt = now;
        } else if (A.fadeGain.gain.value < 0.99) {
            rampFade(1.0, 0.03);
        }
    }

    // Recovery for track changes that start without a 'play' event (e.g. previous restarting the song).
    function ensureAudible(v) {
        if (!inFadeGraph(v) || A.pending || v.paused) return;
        const now = A.ctx.currentTime;
        // A fade-in started by the 'play' handler is still running; restarting it would stretch it.
        if (A.fadeInAt >= 0 && now - A.fadeInAt < fadeInSec() + 0.1) return;
        if (A.fadeGain.gain.value < 0.5) {
            rampFade(1.0, smoothAudioEnabled ? fadeInSec() : 0.03);
            A.fadeInAt = now;
        }
    }

    function playerPause(mp, v) {
        if (mp && typeof mp.pauseVideo === 'function') mp.pauseVideo();
        else if (v) v.pause();
    }

    function playerResume(mp, v) {
        if (mp && typeof mp.playVideo === 'function') {
            mp.playVideo();
        } else if (v) {
            const p = v.play();
            if (p && typeof p.catch === 'function') p.catch(() => {});
        }
    }

    // Set while we re-click a native player-bar button ourselves, so the interceptor lets it through.
    let nativeClickBypass = false;

    function clickNative(btn) {
        nativeClickBypass = true;
        try { btn.click(); } finally { nativeClickBypass = false; }
    }

    function skipNow(kind, btn) {
        if (btn && btn.isConnected) {
            clickNative(btn);
            return;
        }
        const mp = getPlayer();
        const fn = kind === 'next' ? 'nextVideo' : 'previousVideo';
        if (mp && typeof mp[fn] === 'function') {
            mp[fn]();
            return;
        }
        const nb = document.querySelector(kind === 'next'
            ? '.next-button.ytmusic-player-bar, #next-button'
            : '.previous-button.ytmusic-player-bar, #previous-button');
        if (nb) clickNative(nb);
    }

    function runSkips(kind, count, btn) {
        skipNow(kind, btn);
        for (let i = 1; i < count; i++) {
            setTimeout(() => skipNow(kind, btn), 120 * i);
        }
        if (A.fadeGain && A.fadeGain.gain.value < 0.5) {
            setTimeout(() => ensureAudible(getMainVideo()), 120 * count + 600);
        }
    }

    // Identifies the playing track, so a deferred skip can tell whether the track already changed.
    function trackKey() {
        try {
            const mp = getPlayer();
            const d = mp && typeof mp.getVideoData === 'function' ? mp.getVideoData() : null;
            if (d && d.video_id) return d.video_id;
        } catch (_) {}
        const v = getMainVideo();
        return v ? v.currentSrc : '';
    }

    // A skip deferred behind the fade-out is spent if the track changed meanwhile (auto-advance,
    // a click in the queue), otherwise it would skip the new track unheard.
    function runDeferredSkips(p) {
        let n = p.count;
        if (p.kind === 'next' && p.track && trackKey() !== p.track) n--;
        if (n > 0) runSkips(p.kind, n, p.btn);
        else ensureAudible(getMainVideo());
    }

    function flushPending() {
        const p = A.pending;
        if (!p) return;
        clearTimeout(p.timer);
        A.pending = null;
        if (p.kind === 'pause') playerPause(getPlayer(), getVideo());
        else runDeferredSkips(p);
    }

    function playerPlayPause() {
        const pend = A.pending;
        if (pend) {
            if (pend.kind === 'pause') {
                // Second press inside the fade-out: the latest intent is to keep playing.
                clearTimeout(pend.timer);
                A.pending = null;
                rampFade(1.0, 0.08);
                return;
            }
            flushPending();
        }

        const mp = getPlayer();
        const v = getVideo();
        let playing;
        if (v) {
            playing = !v.paused;
        } else if (mp && typeof mp.getPlayerState === 'function') {
            const state = mp.getPlayerState(); // 1 = playing, 3 = buffering (a play is in progress)
            playing = state === 1 || state === 3;
        } else {
            return;
        }

        if (playing) {
            if (smoothAudioEnabled && inFadeGraph(v)) {
                rampFade(FADE_FLOOR, fadeOutSec());
                A.pending = {
                    kind: 'pause',
                    timer: setTimeout(() => {
                        A.pending = null;
                        try { playerPause(getPlayer(), getVideo()); } catch (_) {}
                    }, fadeOutSec() * 1000 + 15)
                };
            } else {
                playerPause(mp, v);
            }
        } else {
            // Resume is fire-and-forget: gating playVideo on it could block playback forever.
            wakeCtx();
            if (smoothAudioEnabled && inFadeGraph(v)) {
                rampFade(1.0, fadeInSec(), FADE_FLOOR);
                A.fadeInAt = A.ctx.currentTime;
            }
            playerResume(mp, v);
        }
    }

    function playerSkip(kind, btn) {
        const pend = A.pending;
        if (pend) {
            if (pend.kind === kind) {
                pend.count++;
                return;
            }
            flushPending();
        }
        const v = getVideo();
        if (!smoothAudioEnabled || !inFadeGraph(v) || v.paused) {
            runSkips(kind, 1, btn);
            return;
        }
        rampFade(FADE_FLOOR, skipOutSec());
        A.pending = {
            kind: kind,
            count: 1,
            btn: btn,
            track: trackKey(),
            timer: setTimeout(() => {
                const p = A.pending;
                A.pending = null;
                try { runDeferredSkips(p || { kind: kind, count: 1, track: '' }); } catch (_) {}
            }, skipOutSec() * 1000 + 10)
        };
    }

    // YouTube Music's own play/pause, next and previous buttons would cut the audio instantly.
    // With Smooth Audio on, fade first; skips then re-click the same button so YTM's own rules
    // (shuffle, repeat, previous restarting the track) still decide what plays next.
    function onNativeControlClick(e) {
        if (nativeClickBypass || !smoothAudioEnabled || e.button !== 0) return;
        const path = e.composedPath ? e.composedPath() : [];
        let kind = null;
        let btn = null;
        let inPlayerBar = false;
        for (let i = 0; i < path.length; i++) {
            const el = path[i];
            if (!el || el.nodeType !== 1) continue;
            if (!kind) {
                if (el.id === 'play-pause-button' || (el.classList && el.classList.contains('play-pause-button'))) {
                    kind = 'toggle';
                } else if (el.classList && el.classList.contains('next-button')) {
                    kind = 'next';
                    btn = el;
                } else if (el.classList && el.classList.contains('previous-button')) {
                    kind = 'prev';
                    btn = el;
                }
            }
            if (el.tagName === 'YTMUSIC-PLAYER-BAR') {
                inPlayerBar = true;
                break;
            }
        }
        if (!kind || !inPlayerBar || isAdNow()) return;
        const v = getVideo();
        if (!inFadeGraph(v)) return;
        e.preventDefault();
        e.stopImmediatePropagation();
        if (kind === 'toggle') playerPlayPause();
        else playerSkip(kind, btn);
    }

    function playerNext() {
        playerSkip('next');
    }

    function playerPrev() {
        playerSkip('prev');
    }

    window.__ytr_playerPlayPause = playerPlayPause;
    window.__ytr_playerNext = playerNext;
    window.__ytr_playerPrev = playerPrev;
    window.__ytr_smoothToggle = playerPlayPause;
    window.__ytr_smoothNext = playerNext;
    window.__ytr_smoothPrev = playerPrev;
    window.__ytr_toggleEqualizer = toggleEqualizerModal;
    window.__ytr_setEqMaster = setEqMasterEnabled;
    window.__ytr_applyPreset = (p) => {
        if (LEGACY_PRESETS[p]) p = LEGACY_PRESETS[p];
        applyEqGains(EQ_PRESETS[p] || EQ_PRESETS['Flat'], EQ_PRESETS[p] ? p : 'Flat');
    };

    let topMenuDocListeners = false;

    function installTopBarUI() {
        try {
            ensureStyles();
            if (!document.body) return;

            let bar = document.getElementById('ytmd-title-bar-main-panel');
            if (bar) {
                if (bar.parentElement !== document.body) {
                    document.body.prepend(bar);
                    // Toggles made while it was detached could not reach it.
                    updateSmoothAudioUI();
                    updateEqButtonUI();
                }
                return;
            }

            bar = createEl('nav', { id: 'ytmd-title-bar-main-panel' });

            const icon = (d, size) => createSvg(size || 16, size || 16, '0 0 24 24', [
                { tag: 'path', attrs: { d: d, fill: 'currentColor' } }
            ]);

            // Left: brand, history, search
            const topbarLeft = createEl('div', { className: 'ytr-topbar-left' });
            const brand = createEl('div', { className: 'ytr-topbar-brand', title: 'ytr-music \u2022 iALTURKi Edition' });
            brand.appendChild(createSvg(16, 16, '0 0 24 24', [
                { tag: 'circle', attrs: { cx: '12', cy: '12', r: '11', fill: '#ff0000' } },
                { tag: 'polygon', attrs: { points: '9.5,7.5 16.5,12 9.5,16.5', fill: '#ffffff' } }
            ]));
            brand.appendChild(createEl('span', { className: 'ytr-brand-title' }, 'ytr-music'));

            const btnBack = createEl('button', { className: 'ytr-icon-btn', id: 'ytr-btn-back', title: 'Back (Alt+Left)' });
            btnBack.appendChild(icon('M15.4 7.4 14 6l-6 6 6 6 1.4-1.4L10.8 12z'));
            const btnForward = createEl('button', { className: 'ytr-icon-btn', id: 'ytr-btn-forward', title: 'Forward (Alt+Right)' });
            btnForward.appendChild(icon('M8.6 16.6 10 18l6-6-6-6-1.4 1.4 4.6 4.6z'));

            const btnSearch = createEl('button', { className: 'ytr-search-btn', id: 'ytr-btn-search', title: 'Search (Ctrl+K or /)' });
            btnSearch.appendChild(icon('M15.5 14h-.79l-.28-.27A6.47 6.47 0 0 0 16 9.5 6.5 6.5 0 1 0 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z', 14));
            btnSearch.appendChild(createEl('span', { className: 'ytr-search-label' }, 'Search'));
            btnSearch.appendChild(createEl('kbd', { className: 'ytr-kbd' }, 'Ctrl K'));

            topbarLeft.appendChild(brand);
            topbarLeft.appendChild(btnBack);
            topbarLeft.appendChild(btnForward);
            topbarLeft.appendChild(btnSearch);

            // Center: now playing
            const topbarCenter = createEl('div', { className: 'ytr-topbar-center' });
            topbarCenter.appendChild(createEl('span', { id: 'ytr-topbar-ticker' }, ''));

            // Right: audio, miniplayer, overflow menu
            const topbarRight = createEl('div', { className: 'ytr-topbar-right' });
            const btnAudio = createEl('button', { className: 'ytr-audio-btn', id: 'ytr-btn-eq' });
            btnAudio.appendChild(icon('M10 20h4V4h-4v16zm-6 0h4v-8H4v8zM16 9v11h4V9h-4z', 13));
            btnAudio.appendChild(createEl('span', { id: 'ytr-btn-eq-text' }, 'EQ'));
            btnAudio.appendChild(createEl('span', { id: 'ytr-fade-dot', className: 'ytr-fade-dot' }));

            const btnPip = createEl('button', { className: 'ytr-icon-btn', id: 'ytr-pip-btn', title: 'Miniplayer: close to the floating desktop player' });
            btnPip.appendChild(icon('M19 11h-8v6h8v-6zm4 8V4.98C23 3.88 22.1 3 21 3H3c-1.1 0-2 .88-2 1.98V19c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2zm-2 .02H3V4.97h18v14.05z', 15));

            const menu = createEl('div', { className: 'ytr-menu-dropdown', id: 'ytr-more' });
            const btnMore = createEl('button', { className: 'ytr-icon-btn', id: 'ytr-btn-more', title: 'More' });
            btnMore.appendChild(icon('M12 8c1.1 0 2-.9 2-2s-.9-2-2-2-2 .9-2 2 .9 2 2 2zm0 2c-1.1 0-2 .9-2 2s.9 2 2 2 2-.9 2-2-.9-2-2-2zm0 6c-1.1 0-2 .9-2 2s.9 2 2 2 2-.9 2-2-.9-2-2-2z'));
            const content = createEl('div', { className: 'ytr-dropdown-content' });
            const menuItem = (id, text, kbd, extraClass) => {
                const it = createEl('div', { className: 'ytr-menu-item' + (extraClass ? ' ' + extraClass : ''), id: id, tabindex: '0' });
                it.appendChild(createEl('span', {}, text));
                if (kbd) it.appendChild(createEl('kbd', { className: 'ytr-kbd' }, kbd));
                return it;
            };
            content.appendChild(menuItem('ytr-menu-home', 'Home'));
            content.appendChild(menuItem('ytr-menu-explore', 'Explore'));
            content.appendChild(menuItem('ytr-menu-library', 'Library'));
            content.appendChild(createEl('div', { className: 'ytr-divider' }));
            content.appendChild(menuItem('ytr-menu-pip', 'Miniplayer', 'Ctrl Alt M'));
            content.appendChild(menuItem('ytr-menu-eq', 'Audio settings', 'Ctrl Alt E'));
            content.appendChild(menuItem('ytr-menu-reload', 'Reload', 'F5'));
            content.appendChild(createEl('div', { className: 'ytr-divider' }));
            content.appendChild(menuItem('ytr-menu-quit', 'Quit', null, 'ytr-danger'));
            content.appendChild(createEl('div', { className: 'ytr-divider' }));
            content.appendChild(createEl('a', {
                className: 'ytr-menu-foot',
                href: 'https://github.com/iAlturki/ytr-music',
                target: '_blank',
                title: 'ytr-music on GitHub'
            }, 'ytr-music \u2022 iALTURKi Edition'));
            menu.appendChild(btnMore);
            menu.appendChild(content);

            topbarRight.appendChild(btnAudio);
            topbarRight.appendChild(btnPip);
            topbarRight.appendChild(menu);

            bar.appendChild(topbarLeft);
            bar.appendChild(topbarCenter);
            bar.appendChild(topbarRight);

            // Listeners are attached in code (no inline handlers: Trusted Types / CSP).
            const closeMenu = () => menu.classList.remove('ytr-menu-open');
            btnMore.addEventListener('click', (e) => {
                e.stopPropagation();
                menu.classList.toggle('ytr-menu-open');
            });
            if (!topMenuDocListeners) {
                // Registered once; looked up by id so a re-created bar is covered too.
                topMenuDocListeners = true;
                document.addEventListener('click', (e) => {
                    const m = document.getElementById('ytr-more');
                    if (m && !m.contains(e.target)) m.classList.remove('ytr-menu-open');
                }, true);
                document.addEventListener('keydown', (e) => {
                    const m = document.getElementById('ytr-more');
                    if (m && e.key === 'Escape') m.classList.remove('ytr-menu-open');
                }, true);
            }
            const onItem = (id, fn) => {
                const el = bar.querySelector('#' + id);
                if (!el) return;
                el.addEventListener('click', () => { closeMenu(); fn(); });
                el.addEventListener('keydown', (e) => {
                    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); closeMenu(); fn(); }
                });
            };

            btnBack.addEventListener('click', () => window.history.back());
            btnForward.addEventListener('click', () => window.history.forward());
            btnSearch.addEventListener('click', focusSearchBox);
            btnAudio.addEventListener('click', toggleEqualizerModal);
            btnPip.addEventListener('click', () => postToHost({ type: 'enter_pip' }));
            onItem('ytr-menu-pip', () => postToHost({ type: 'enter_pip' }));
            onItem('ytr-menu-eq', toggleEqualizerModal);
            onItem('ytr-menu-reload', () => window.location.reload());
            onItem('ytr-menu-quit', () => postToHost({ type: 'quit' }));
            onItem('ytr-menu-home', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(1), a[href="/"]');
                if (el) el.click(); else window.location.href = '/';
            });
            onItem('ytr-menu-explore', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(2), a[href*="explore"]');
                if (el) el.click(); else window.location.href = '/explore';
            });
            onItem('ytr-menu-library', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(3), a[href*="library"]');
                if (el) el.click(); else window.location.href = '/library';
            });

            document.body.prepend(bar);
            // The UI sync looks its elements up by id, so it only works once the bar is in the document.
            updateSmoothAudioUI();
            updateEqButtonUI();
            refreshTicker();
        } catch (err) {
            logToHost('TopBar install error: ' + (err && err.message));
        }
    }

    // Top bar and sidebar upkeep: observer driven, and skipped entirely while the page is hidden.
    let bodyObs = null;
    let bodyObsTarget = null;
    let topBarRaf = 0;

    function ensureTopBar() {
        if (!document.body) return;
        const bar = document.getElementById('ytmd-title-bar-main-panel');
        if (bar && bar.parentElement === document.body) return;
        installTopBarUI();
    }

    function onBodyChildren() {
        if (document.hidden || topBarRaf) return;
        topBarRaf = requestAnimationFrame(() => {
            topBarRaf = 0;
            ensureTopBar();
        });
    }

    function startBodyObserver() {
        const body = document.body;
        if (!body || body === bodyObsTarget) return;
        if (!bodyObs) bodyObs = new MutationObserver(onBodyChildren);
        bodyObs.disconnect();
        bodyObs.observe(body, { childList: true });
        bodyObsTarget = body;
    }

    // 5. Track metadata and state reporter
    const last = {
        title: null,
        artist: null,
        artwork: null,
        paused: null,
        liked: null,
        disliked: null,
        time: -1,
        duration: -1,
        volume: -1
    };
    let stateTimer = 0;
    let stateDue = 0;
    let lastTickSec = -1;

    function flushState() {
        stateTimer = 0;
        sendState(false);
    }

    // Coalesces bursts of media/DOM events into one sendState; a shorter delay pre-empts a longer one.
    function queueState(delay) {
        const due = performance.now() + delay;
        if (stateTimer) {
            if (due >= stateDue) return;
            clearTimeout(stateTimer);
        }
        stateDue = due;
        stateTimer = setTimeout(flushState, delay);
    }

    let barRef = null;
    const barParts = { title: null, byline: null, image: null, like: null };
    const observedParts = { title: null, byline: null, image: null, like: null };
    let stateObs = null;

    function getPlayerBar() {
        if (barRef && barRef.isConnected) return barRef;
        barRef = document.querySelector('ytmusic-player-bar');
        return barRef;
    }

    function resolveBarPart(key, selector, bar) {
        const cur = barParts[key];
        if (cur && cur.isConnected) return;
        barParts[key] = bar ? bar.querySelector(selector) : null;
    }

    function refreshBarParts(allowDocumentLike) {
        const bar = getPlayerBar();
        resolveBarPart('title', '.title', bar);
        resolveBarPart('byline', '.byline', bar);
        resolveBarPart('image', '.image', bar);
        resolveBarPart('like', 'ytmusic-like-button-renderer, #like-button-renderer', bar);
        if (!barParts.like && allowDocumentLike) {
            barParts.like = document.querySelector('ytmusic-like-button-renderer, #like-button-renderer');
        }
        return barParts;
    }

    function onPlayerBarMutation() {
        queueState(50);
    }

    function attachStateObservers() {
        const parts = refreshBarParts(true);
        if (parts.title === observedParts.title && parts.byline === observedParts.byline &&
            parts.image === observedParts.image && parts.like === observedParts.like) {
            return !!parts.title;
        }
        if (!stateObs) stateObs = new MutationObserver(onPlayerBarMutation);
        stateObs.disconnect();
        const textOpts = { childList: true, characterData: true, subtree: true };
        if (parts.title) stateObs.observe(parts.title, textOpts);
        if (parts.byline) stateObs.observe(parts.byline, textOpts);
        if (parts.image) stateObs.observe(parts.image, { attributes: true, attributeFilter: ['src'] });
        if (parts.like) stateObs.observe(parts.like, { attributes: true, attributeFilter: ['like-status'] });
        observedParts.title = parts.title;
        observedParts.byline = parts.byline;
        observedParts.image = parts.image;
        observedParts.like = parts.like;
        return !!parts.title;
    }

    let tickerRef = null;
    let tickerText = null;

    function updateTicker(title, artist, isLiked) {
        if (document.hidden) return;
        if (!tickerRef || !tickerRef.isConnected) {
            tickerRef = document.getElementById('ytr-topbar-ticker');
            tickerText = null;
            if (!tickerRef) return;
        }
        const likeBadge = isLiked ? ' \u2665' : '';
        let text;
        if (title && artist) {
            text = `\uD83C\uDFB5 ${title} \u2014 ${artist}${likeBadge}`;
        } else if (title) {
            text = `\uD83C\uDFB5 ${title}${likeBadge}`;
        } else {
            text = '\uD83C\uDFB5 Ready to Play';
        }
        if (text !== tickerText) {
            tickerText = text;
            tickerRef.textContent = text;
        }
    }

    function refreshTicker() {
        updateTicker(last.title || '', last.artist || '', !!last.liked);
    }

    function readVolume(video) {
        try {
            const player = getPlayer();
            if (player && typeof player.getVolume === 'function') {
                const pv = player.getVolume();
                if (typeof pv === 'number' && !isNaN(pv)) return Math.round(pv);
            } else if (video && typeof video.volume === 'number') {
                return Math.round(Math.sqrt(video.volume) * 100);
            }
        } catch (_) {}
        return 100;
    }

    function sendState(force) {
        try {
            const video = getVideo();
            const media = navigator.mediaSession ? navigator.mediaSession.metadata : null;
            const parts = refreshBarParts(false);

            let title = (media && media.title) ? media.title : '';
            let artist = (media && media.artist) ? media.artist : '';
            let artwork = '';
            if (media) {
                const art = media.artwork;
                if (art && art.length) artwork = art[art.length - 1].src || '';
            }
            if (!title && parts.title) title = parts.title.textContent.trim();
            if (!artist && parts.byline) artist = parts.byline.textContent.trim();
            if (!artwork && parts.image) artwork = parts.image.src || '';

            const paused = video ? video.paused : true;
            const time = video ? Math.floor(video.currentTime) : 0;
            const duration = video && isFinite(video.duration) ? Math.floor(video.duration) : 0;
            const volume = readVolume(video);
            const likeStatus = parts.like ? parts.like.getAttribute('like-status') : '';
            const isLiked = likeStatus === 'LIKE';
            const isDisliked = likeStatus === 'DISLIKE';

            updateTicker(title, artist, isLiked);

            if (!force && title === last.title && artist === last.artist && artwork === last.artwork &&
                paused === last.paused && isLiked === last.liked && isDisliked === last.disliked &&
                time === last.time && duration === last.duration && volume === last.volume) {
                return;
            }
            last.title = title;
            last.artist = artist;
            last.artwork = artwork;
            last.paused = paused;
            last.liked = isLiked;
            last.disliked = isDisliked;
            last.time = time;
            last.duration = duration;
            last.volume = volume;

            postToHost({
                type: 'state',
                title: title || 'Ready to Play',
                artist: artist || 'YouTube Music',
                artwork: artwork,
                paused: paused,
                currentTime: time,
                duration: duration,
                volume: volume,
                isLiked: isLiked,
                isDisliked: isDisliked
            });
        } catch (_) {}
    }

    // 6. Media event wiring (document capture). timeupdate is the only per-second path and
    // returns after one comparison unless the whole second changed.
    function onMediaPlay(e) {
        const v = mediaTarget(e);
        if (!v) return;
        wakeCtx();
        try { initAudioGraph(v); } catch (_) {}
        try { fadeInOnPlay(v); } catch (_) {}
        queueState(0);
    }

    function onMediaPlaying(e) {
        const v = mediaTarget(e);
        if (!v) return;
        wakeCtx();
        try { initAudioGraph(v); } catch (_) {}
        try { ensureAudible(v); } catch (_) {}
        attachAdObserver();
        adTick();
        queueState(0);
    }

    function onMediaPause(e) {
        const v = mediaTarget(e);
        if (!v) return;
        if (!A.video || A.video === v) armIdleSuspend();
        queueState(0);
    }

    function onMediaSeeked(e) {
        const v = mediaTarget(e);
        if (!v) return;
        if (v.paused) {
            if (!A.video || A.video === v) armIdleSuspend();
        } else {
            try { ensureAudible(v); } catch (_) {}
        }
        queueState(0);
    }

    function onMediaLoaded(e) {
        if (!mediaTarget(e)) return;
        attachAdObserver();
        attachStateObservers();
        // Once per track; covers a guide stamped after the bounded startup wait gave up.
        attachGuideObserver();
        adTick();
        queueState(0);
    }

    function onMediaDuration(e) {
        if (!mediaTarget(e)) return;
        adTick();
        queueState(30);
    }

    function onMediaChange(e) {
        if (!mediaTarget(e)) return;
        queueState(30);
    }

    function onMediaVolume(e) {
        if (!mediaTarget(e)) return;
        queueState(150);
    }

    function onTimeUpdate(e) {
        const v = mediaTarget(e);
        if (!v) return;
        const sec = Math.floor(v.currentTime);
        if (sec === lastTickSec) return;
        lastTickSec = sec;
        // The captured element is only audible through the graph: if a resume was lost to the
        // idle suspend, bring it back (only ever while this element is actually playing).
        if (A.ctxStarted && A.video === v && !v.paused && A.ctx.state !== 'running') wakeCtx();
        // Media events are not timer-throttled, so this also drives ad handling while hidden.
        if (adTimer || isAdNow()) {
            adTick();
            return;
        }
        ensureAudible(v);
        if (!v.paused) sendState(false);
    }

    function onKeyDown(e) {
        unlockAudio();
        const path0 = e.composedPath ? e.composedPath()[0] : null;
        const target = (path0 && path0.nodeType === 1) ? path0 : e.target;
        const isEditing = !!target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable);
        if (e.key === 'Escape') {
            const overlay = document.getElementById('ytr-eq-overlay');
            if (overlay && overlay.classList.contains('ytr-open')) {
                overlay.classList.remove('ytr-open');
                return;
            }
        }
        if (e.ctrlKey && e.altKey && (e.key === 'e' || e.key === 'E')) {
            e.preventDefault();
            if (!e.repeat) toggleEqualizerModal();
            return;
        }
        if ((e.ctrlKey && e.key === 'k') || (e.key === '/' && !isEditing)) {
            e.preventDefault();
            focusSearchBox();
            return;
        }
        if ((e.code === 'Space' || e.key === ' ') && !isEditing) {
            // Registered at document start, so this runs before YTM's own Space handling;
            // auto-repeat would otherwise flap pause/resume.
            e.preventDefault();
            e.stopImmediatePropagation();
            if (!e.repeat) playerPlayPause();
            return;
        }
    }

    // 7. Initialization
    let bridgeStarted = false;
    let attachWaitT = 0;
    let attachWaitStep = 0;
    const ATTACH_WAIT_MS = [250, 500, 1000, 2000, 4000, 8000, 15000, 30000];

    function attachAll() {
        const ad = attachAdObserver();
        const st = attachStateObservers();
        const guide = attachGuideObserver();
        return ad && st && guide;
    }

    // Bounded one-shot wait while YTM stamps the player, player bar and guide.
    function waitForPlayerParts() {
        if (attachWaitT || attachAll()) return;
        if (attachWaitStep >= ATTACH_WAIT_MS.length) return;
        attachWaitT = setTimeout(() => {
            attachWaitT = 0;
            waitForPlayerParts();
        }, ATTACH_WAIT_MS[attachWaitStep++]);
    }

    function runUiMaintenance() {
        ensureStyles();
        startBodyObserver();
        ensureTopBar();
        attachGuideObserver();
        scheduleGuideScan();
        refreshTicker();
    }

    function onVisibilityChange() {
        if (document.hidden || !bridgeStarted) return;
        try {
            runUiMaintenance();
        } catch (_) {}
    }

    function initBridge() {
        if (bridgeStarted) {
            recheck();
            return;
        }
        bridgeStarted = true;
        try {
            ensureStyles();
            startBodyObserver();
            if (!document.hidden) ensureTopBar();
            waitForPlayerParts();
            adTick();
            sendState(true);
            logToHost('Bridge initialized');
        } catch (e) {
            logToHost('initBridge error: ' + (e && e.message));
        }
    }

    function recheck() {
        if (!bridgeStarted) {
            if (document.readyState !== 'loading') initBridge();
            return;
        }
        try {
            ensureStyles();
            if (!document.hidden) runUiMaintenance();
            startBodyObserver();
            attachAll();
            adTick();
            if (A.ctx) {
                const v = getMainVideo();
                if (v && !v.paused) initAudioGraph(v);
            }
            sendState(true);
        } catch (_) {}
    }
    window.__ytr_recheck = recheck;
    window.__ytr_init = initBridge;

    try {
        document.addEventListener('click', onNativeControlClick, true);
        document.addEventListener('play', onMediaPlay, true);
        document.addEventListener('playing', onMediaPlaying, true);
        document.addEventListener('pause', onMediaPause, true);
        document.addEventListener('seeked', onMediaSeeked, true);
        document.addEventListener('loadedmetadata', onMediaLoaded, true);
        document.addEventListener('durationchange', onMediaDuration, true);
        document.addEventListener('emptied', onMediaChange, true);
        document.addEventListener('ended', onMediaChange, true);
        document.addEventListener('ratechange', onMediaChange, true);
        document.addEventListener('volumechange', onMediaVolume, true);
        document.addEventListener('timeupdate', onTimeUpdate, true);
        document.addEventListener('iron-overlay-opened', onOverlayOpened, true);
        document.addEventListener('visibilitychange', onVisibilityChange);
        window.addEventListener('click', unlockAudio, true);
        window.addEventListener('keydown', onKeyDown, true);
    } catch (_) {}

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initBridge);
    } else {
        initBridge();
    }
})();
