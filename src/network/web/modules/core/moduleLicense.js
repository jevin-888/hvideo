import { apiGet } from './api.js';

export const MODULE_LABELS = Object.freeze({
    TV: 'TV', '投屏': '投屏', '霸屏': '霸屏', fusion: '投影融合',
    effects: '特效管理', scene: '场景模板', cave: 'CAVE校正',
    stage: '舞台控制', vod: 'VOD配置'
});

const PAGE_MODULES = Object.freeze({
    'scene-template': 'scene', 'effect-control': 'effects', fusion: 'fusion',
    'projection-correction': 'cave', 'stage-control': 'stage'
});
let enabledModules = new Set();

export function isModuleLicensed(module) {
    return enabledModules.has(module);
}

export function isPageLicensed(page) {
    return !PAGE_MODULES[page] || isModuleLicensed(PAGE_MODULES[page]);
}

export function applyModuleLicense(license) {
    const valid = !!license && ['valid', 'time_calibrating'].includes(license.status);
    enabledModules = new Set(valid && Array.isArray(license.modules) ? license.modules : []);
    for (const [page, module] of Object.entries(PAGE_MODULES)) {
        const allowed = isModuleLicensed(module);
        document.querySelectorAll(`.menu-item[data-page="${page}"]`).forEach(item => {
            item.hidden = !allowed;
            item.style.display = allowed ? '' : 'none';
        });
        const panel = document.getElementById(`${page}-page`);
        if (panel) {
            panel.style.display = allowed ? '' : 'none';
            if (!allowed) panel.classList.remove('active');
        }
    }
    const vodAllowed = isModuleLicensed('vod');
    const vodTab = document.getElementById('common-config-tab-vod');
    if (vodTab) {
        const wasActive = vodTab.classList.contains('active');
        vodTab.hidden = !vodAllowed;
        vodTab.style.display = vodAllowed ? '' : 'none';
        vodTab.disabled = !vodAllowed;
        if (!vodAllowed && wasActive) {
            document.querySelector('[data-common-config-target]:not([data-common-config-target="vod"])')?.click();
        }
    }
    document.querySelectorAll('[data-common-config-panel="vod"]').forEach(panel => {
        panel.style.display = vodAllowed ? '' : 'none';
        if (!vodAllowed) panel.hidden = true;
        panel.querySelectorAll('input, select, button, textarea').forEach(control => {
            control.disabled = !vodAllowed;
        });
    });
    window.dispatchEvent(new CustomEvent('moduleLicenseChanged'));
}

export async function refreshModuleLicense() {
    try {
        applyModuleLicense(await apiGet('/system/license'));
    } catch (_) {
        applyModuleLicense(null);
    }
}
