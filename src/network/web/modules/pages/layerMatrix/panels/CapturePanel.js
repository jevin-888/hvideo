// 采集图层（10/11）参数 HTML。
import { fitModeBlockHtml, shapeBlockHtml } from './SharedBlocks.js';
import { createNumInput } from './CommonControls.js';
import { apiGet, apiPost, apiPut, getLastApiError } from '../../../core/api.js';
import { showSuccess, showError } from '../../../components/toast.js';

export function generateCaptureHtml(layer) {
    const captureType = layer.capture_type ?? layer.captureType ?? 'AUTO';
    const captureIndex = layer.capture_index ?? layer.captureIndex ?? 0;
    const captureWidth = layer.capture_width ?? layer.captureWidth ?? 1920;
    const captureHeight = layer.capture_height ?? layer.captureHeight ?? 1080;
    const captureResolutionLocked = layer.capture_resolution_locked ?? layer.captureResolutionLocked ?? false;
    const blur = Math.round(layer.gaussian_blur ?? layer.gaussianBlur ?? 0);

    return `
        <div class="setting-section compact-advanced-params compact-advanced-params--capture">
            <div class="setting-item" style="grid-column: span 1;">
                <label for="capture-type">输入选择:</label>
                <select id="capture-type" class="form-control">
                    <option value="AUTO" ${captureType === 'AUTO' ? 'selected' : ''}>自动</option>
                    <option value="HDMI" ${captureType === 'HDMI' ? 'selected' : ''}>HDMI</option>
                    <option value="USB" ${captureType === 'USB' ? 'selected' : ''}>USB</option>
                    <option value="MIPI" ${captureType === 'MIPI' ? 'selected' : ''}>MIPI</option>
                </select>
            </div>
            ${createNumInput('capture-index', '设备索引', captureIndex, 0, null, 1)}
            <div class="capture-resolution-card" data-param-tab="advanced" style="grid-column: 1 / -1;">
                <div class="capture-resolution-row" role="group" aria-label="采集分辨率">
                    <select id="capture-edid-mode" class="form-control" aria-label="分辨率模式" disabled><option value="default">自动</option><option value="custom">自定义</option></select>
                    <span class="capture-dimensions capture-custom-only" hidden>
                        <input id="capture-width" type="number" class="form-control" min="320" max="4088" step="8" value="${captureWidth}" aria-label="采集宽度" disabled>
                        <span aria-hidden="true">×</span>
                        <input id="capture-height" type="number" class="form-control" min="240" max="2160" step="1" value="${captureHeight}" aria-label="采集高度" disabled>
                    </span>
                    <span class="capture-refresh-input capture-custom-only" hidden><span aria-hidden="true">@</span><input id="capture-edid-refresh" class="form-control" type="number" min="24" max="60" step="1" value="30" aria-label="采集刷新率" disabled><span>Hz</span></span>
                    <label class="capture-lock-label capture-custom-only" hidden title="锁定当前采集分辨率，EDID 只公布此模式"><input id="capture-resolution-locked" type="checkbox" ${captureResolutionLocked ? 'checked' : ''} disabled>锁定分辨率</label>
                    <button type="button" id="capture-edid-apply" class="btn small primary" disabled>应用</button>
                    <button type="button" id="capture-edid-refresh-status" class="btn small" aria-label="刷新采集状态" disabled>刷新</button>
                </div>
                <div id="capture-edid-row" class="capture-edid-state" role="status" aria-live="polite"><span class="capture-state-dot"></span><span id="capture-edid-mode-status">读取中</span></div>
            </div>
            ${createNumInput('video-gaussian-blur', '高斯模糊', blur, 0, 10, 1)}
            ${fitModeBlockHtml(layer)}
            ${shapeBlockHtml(layer, { includeInvert: true })}
        </div>
    `;
}

// Reuse the existing config APIs for one explicit EDID + capture operation.
export async function saveCaptureResolutionConfig(layerId, capture) {
    const patch = { captureResolutionLocked: capture.locked };
    if (capture.mode === 'custom') {
        patch.captureWidth = capture.width;
        patch.captureHeight = capture.height;
    }
    const config = await apiPut(`/config/layers/${layerId}`, patch, { retries: 0 });
    if (!config || Object.entries(patch).some(([key, value]) => config[key] !== value)) return null;
    if (!await apiPost('/config/save', {}, { retries: 0 })) return null;
    return config;
}

export async function setupCaptureEdidEvents(container, isCurrentRender = () => true, applyCaptureConfig = async () => false) {
    const find = id => container.querySelector(`#capture-edid-${id}`);
    const mode = find('mode'), status = find('mode-status'), refresh = find('refresh');
    const apply = find('apply'), reload = find('refresh-status');
    const lock = container.querySelector('#capture-resolution-locked');
    const width = container.querySelector('#capture-width'), height = container.querySelector('#capture-height');
    const dot = container.querySelector('.capture-state-dot');
    if (!mode || !status || !refresh || !apply || !reload || !lock || !width || !height) return;
    let busy = false, ready = false, failure = '';
    const current = () => isCurrentRender() && container.isConnected;
    const paint = (message, color) => {
        if (!current()) return;
        status.textContent = message;
        if (dot) dot.style.background = color;
    };
    const controls = () => {
        const custom = mode.value === 'custom';
        mode.disabled = busy || !ready;
        apply.disabled = busy || !ready;
        reload.disabled = busy;
        [width, height, refresh, lock].forEach(input => { input.disabled = busy || !ready || !custom; });
        container.querySelectorAll('.capture-custom-only').forEach(element => { element.hidden = !custom; });
    };
    const dirty = () => {
        if (busy || !ready) return;
        if (mode.value !== 'custom') lock.checked = false;
        controls();
        paint(mode.value !== 'custom' ? '待应用：自动适配，使用默认 EDID' :
            `待应用：${lock.checked ? '锁定分辨率' : '优先分辨率'} ${width.value} × ${height.value} @ ${refresh.value} Hz`, '#e6ba6b');
    };
    const show = data => {
        if (!current()) return;
        mode.value = data.mode === 'custom' ? 'custom' : 'default';
        lock.checked = data.mode === 'custom' && data.singleMode === true;
        if (data.mode === 'custom' && data.requested) {
            width.value = data.requested.width;
            height.value = data.requested.height;
            refresh.value = data.requested.refreshRate;
        }
        paint(data.applied === false ? '保存的设置尚未生效，请重新应用' :
            data.mode !== 'custom' ? '自动：EDID 默认，采集适配信号源' :
            data.singleMode ? 'EDID 仅公布此模式' : 'EDID 首选此模式，保留其他模式',
            data.applied === false ? '#e6ba6b' : '#87d69a');
    };
    const load = async () => {
        if (busy || !current()) return;
        busy = true;
        controls();
        try {
            const data = await apiGet('/system/capture-edid', { timeoutMs: 8000, retries: 0 });
            if (!current()) return;
            ready = !!data;
            if (data) show(data);
            else throw new Error(getLastApiError()?.message || '当前设备无法读取 RK628 EDID');
        } catch (error) {
            ready = false;
            failure = error.message || 'RK628 状态读取失败';
            if (current()) showError(failure);
        } finally {
            busy = false;
            if (current()) {
                controls();
                if (failure) paint(failure, '#e6ba6b');
            }
        }
    };
    mode.addEventListener('change', dirty);
    [width, height, refresh, lock].forEach(input => {
        input.addEventListener('input', dirty);
        input.addEventListener('change', dirty);
    });
    reload.addEventListener('click', async () => {
        if (busy) return;
        failure = '';
        await load();
    });
    apply.addEventListener('click', async () => {
        if (busy || !ready || !current()) return;
        const custom = mode.value === 'custom';
        const body = { mode: custom ? 'custom' : 'default' };
        if (custom) {
            const values = [width.value, height.value, refresh.value].map(Number);
            if (!values.every(Number.isInteger) || values[0] < 320 || values[0] > 4088 || values[0] % 8 ||
                values[1] < 240 || values[1] > 2160 || values[2] < 24 || values[2] > 60) {
                showError('自定义分辨率宽度必须为320至4088的8倍数，高度为240至2160，刷新率为24至60Hz整数');
                return;
            }
            [body.width, body.height, body.refreshRate] = values;
            body.singleMode = lock.checked;
        }
        // Capture the submitted values before waiting for the receiver or refreshing the UI.
        const capture = { mode: body.mode, locked: custom && body.singleMode };
        if (custom) { capture.width = body.width; capture.height = body.height; }
        busy = true;
        failure = '';
        controls();
        try {
            const data = await apiPost('/system/capture-edid', body, { timeoutMs: 12000, retries: 0 });
            if (!current()) return;
            if (!data) throw new Error(getLastApiError()?.message || '采集首选分辨率应用失败');
            if (data.applied === false) throw new Error('保存的设置尚未生效，请重新应用');
            if (await applyCaptureConfig(capture) !== true) throw new Error('采集配置保存失败，请重试');
            if (!current()) return;
            show(data);
            showSuccess(!custom ? '已切换为自动适配和默认 EDID' :
                capture.locked ? '采集分辨率已锁定，EDID 仅公布此模式' :
                '已将该分辨率设为 EDID 首选模式，其他兼容模式保持可选');
        } catch (error) {
            failure = error.message || '采集首选分辨率应用失败';
            if (current()) showError(failure);
        } finally {
            busy = false;
            // A failed/lost write response may still have changed the receiver. Re-read it,
            // keeping any partial-save failure visible instead of claiming full success.
            if (current()) await load();
        }
    });
    await load();
}
