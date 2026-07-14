/**
 * @file appTitle.js（文件名）
 * @brief 说明：头部和应用标题辅助函数。
 */

import { apiGet } from './api.js';

const DEFAULT_TITLE = 'HVIDEO控制软件';
const TITLE_SUFFIX = '控制软件';

let appTitleName = DEFAULT_TITLE;
let appVersion = '';

function normalizeText(value) {
    return typeof value === 'string' ? value.trim() : '';
}

function buildTitle() {
    return appVersion ? `${appTitleName} v${appVersion}` : appTitleName;
}

function buildTitleNameFromSupplier(supplierName) {
    const supplier = normalizeText(supplierName);
    if (!supplier) {
        return DEFAULT_TITLE;
    }
    return supplier.endsWith(TITLE_SUFFIX) ? supplier : `${supplier}${TITLE_SUFFIX}`;
}

function updateTitleElement() {
    const title = buildTitle();
    const appTitle = document.getElementById('app-title');
    if (appTitle) {
        appTitle.textContent = '';
        appTitle.appendChild(document.createTextNode(appTitleName));
        if (appVersion) {
            const version = document.createElement('span');
            version.className = 'header-version';
            version.textContent = `v${appVersion}`;
            appTitle.appendChild(version);
        }
    }
    document.title = title;
}

export function setAppTitleVersion(version) {
    appVersion = normalizeText(version);
    updateTitleElement();
}

export function applyAppTitleFromLicense(license) {
    appTitleName = buildTitleNameFromSupplier(license && license.supplier_name);
    updateTitleElement();
}

export async function refreshAppTitleFromLicense() {
    const browserTime = Math.floor(Date.now() / 1000);
    const license = await apiGet(`/system/license?browser_time=${browserTime}`);
    if (license) {
        applyAppTitleFromLicense(license);
    } else {
        updateTitleElement();
    }
}
