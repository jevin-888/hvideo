// Shared presentation-only localization, following Hvideo_server/static/admin/i18n.js.
(() => {
    'use strict';
    const catalog = window.PlayerTranslations || {};
    const locales = { zh: 'zh-CN', en: 'en-US', id: 'id-ID', vi: 'vi-VN', th: 'th-TH' };
    const preferenceKey = 'hvideo_player_language';
    const textSources = new WeakMap(), attributeSources = new WeakMap();
    const ignore = 'script,style,textarea,code,pre,[translate="no"],[data-i18n-ignore],.material-name,.material-folder,.playlist-item-name,.playlist-item-folder,.template-name,.video-title,.video-tag,.progress-name,.folder-tag[data-folder],#command-log,#log-content,#log-file-list .log-list-item > span:first-child,#current-serial,#stored-serial,#target-ip,#device-name,#device-serial,#license-supplier,#license-customer,[data-lan-name]';
    const attributes = ['placeholder', 'title', 'aria-label', 'alt', 'label'];
    let language = 'zh';
    try { language = localStorage.getItem(preferenceKey) || 'zh'; } catch (_) { /* Storage may be disabled. */ }
    const supported = code => Object.prototype.hasOwnProperty.call(locales, code);
    if (!supported(language)) language = 'zh';
    const normalize = value => String(value ?? '').trim().replace(/\s+/g, ' ');
    const escapePattern = value => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const templates = Object.keys(catalog).filter(key => /\{\d+\}/.test(key))
        .sort((a, b) => b.replace(/\{\d+\}/g, '').length - a.replace(/\{\d+\}/g, '').length)
        .map(key => {
            const slots = [];
            const pattern = key.split(/(\{\d+\})/).map(part => {
                if (/^\{\d+\}$/.test(part)) {
                    slots.push(part);
                    // Numeric labels must not match the start of an unrelated Chinese sentence.
                    if (['设备{0}', '图层{0}', '图层 {0}', '区域 {0}', '视频 {0}'].includes(key)) return '([\\d.-]+)';
                    return '(.+?)';
                }
                return escapePattern(part);
            }).join('');
            return { key, slots, pattern: new RegExp('^' + pattern + '$') };
        });
    function t(value, code = language) {
        const original = String(value ?? ''), key = normalize(original);
        if (code === 'zh' || !key || !supported(code)) return original;
        let translated = catalog[key]?.[code];
        if (!translated && /[:：]$/.test(key)) {
            const bare = key.slice(0, -1);
            if (catalog[bare]?.[code]) translated = catalog[bare][code] + ':';
        }
        if (!translated) for (const entry of templates) {
            const match = key.match(entry.pattern);
            if (!match) continue;
            translated = catalog[entry.key][code].replace(/\{\d+\}/g, slot => match[entry.slots.indexOf(slot) + 1] ?? slot);
            break;
        }
        return translated ? original.replace(original.trim(), translated) : original;
    }
    function remembered(value, previous) {
        const source = previous && value === previous.rendered ? previous.source : value;
        return { source, rendered: t(source) };
    }
    function translateNode(node) {
        if (!node.parentElement || node.parentElement.closest(ignore)) return;
        const state = remembered(node.nodeValue, textSources.get(node));
        textSources.set(node, state);
        if (node.nodeValue === state.rendered) return;
        const option = node.parentElement.closest('option');
        if (option && !option.hasAttribute('value')) option.value = option.textContent;
        node.nodeValue = state.rendered;
    }
    function translateElement(element) {
        if (element.closest(ignore)) return;
        const states = attributeSources.get(element) || {};
        for (const name of attributes) {
            if (name === 'label' && !element.matches('optgroup,option')) continue;
            if (!element.hasAttribute(name)) continue;
            states[name] = remembered(element.getAttribute(name), states[name]);
            if (element.getAttribute(name) !== states[name].rendered) element.setAttribute(name, states[name].rendered);
        }
        attributeSources.set(element, states);
    }
    function translateTree(root) {
        if (root.nodeType === Node.TEXT_NODE) { translateNode(root); return; }
        if (root.nodeType !== Node.ELEMENT_NODE || root.closest(ignore)) return;
        translateElement(root);
        const walker = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
        while (walker.nextNode()) {
            if (walker.currentNode.nodeType === Node.TEXT_NODE) translateNode(walker.currentNode);
            else translateElement(walker.currentNode);
        }
    }
    function sourceText(element) {
        if (!element) return '';
        const walker = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
        let result = '';
        while (walker.nextNode()) {
            const node = walker.currentNode, previous = textSources.get(node);
            result += previous && node.nodeValue === previous.rendered ? previous.source : node.nodeValue;
        }
        return result;
    }
    const observer = new MutationObserver(records => {
        // Flush the whole batch while disconnected to avoid observing our own translations.
        observer.disconnect();
        const roots = new Set();
        for (const record of records) {
            if (record.type === 'childList') for (const node of record.addedNodes) roots.add(node);
            else roots.add(record.target);
        }
        for (const root of roots) if (root.isConnected) translateTree(root);
        observe();
    });
    function observe() {
        observer.observe(document.documentElement, { subtree: true, childList: true, characterData: true,
            attributes: true, attributeFilter: attributes });
    }
    function apply(code) {
        // Preserve external writes made in the same task before changing languages.
        const pending = observer.takeRecords();
        observer.disconnect();
        for (const record of pending) {
            if (record.type === 'characterData') {
                const previous = textSources.get(record.target);
                if (previous && record.target.nodeValue !== previous.rendered) textSources.delete(record.target);
            }
        }
        language = code;
        document.documentElement.lang = locales[language];
        translateTree(document.documentElement);
        document.querySelectorAll('[data-player-language]').forEach(select => { select.value = language; });
        observe();
        document.dispatchEvent(new CustomEvent('player-language-changed', { detail: { language, locale: locales[language] } }));
    }
    function setLanguage(code) {
        if (!supported(code)) return;
        apply(code);
        try { localStorage.setItem(preferenceKey, code); } catch (_) { /* Still usable without persistence. */ }
    }
    window.PlayerI18n = { t, setLanguage, sourceText, get language() { return language; }, get locale() { return locales[language]; } };
    function initialize() {
        document.querySelectorAll('[data-player-language]').forEach(select => select.addEventListener('change', () => setLanguage(select.value)));
        apply(language);
    }
    window.addEventListener('storage', event => {
        if (event.key === preferenceKey) apply(supported(event.newValue) ? event.newValue : 'zh');
    });
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initialize, { once: true });
    else initialize();
})();
