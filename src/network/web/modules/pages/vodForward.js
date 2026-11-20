import { apiGet, apiAction, getLastApiError } from '../core/api.js';
import { addToCommandLog } from '/shared/commandLog.js';

const functions = [
    ['Song', 'songNo', '歌曲编号'], ['Song', 'categoryCode', '歌曲分类码'],
    ['Song', 'lightCode', '歌曲灯光码'], ['SetLight', 'scene', '灯光场景'],
    ['SetEffect', 'mode', '音效模式'], ['SetAC', 'power', '空调开关'],
    ['SetAC', 'temp', '空调温度'], ['SetAC', 'mode', '空调模式'],
    ['SetAC', 'wind', '空调风速']
];
let initialized = false;
let saving = false;
let dirty = false;
let loading = false;
const element = id => document.getElementById(id);
function status(message, failed = false) {
    element('vod-forward-status').textContent = message;
    element('vod-forward-status').style.color = failed ? '#ff7c7c' : '';
}
function requireResult(value) {
    if (value == null) throw new Error(getLastApiError()?.message || '请求失败，请重试');
    return value;
}
function addRule(rule = {}) {
    const row = document.createElement('div');
    row.className = 'vod-forward-rule';
    const select = document.createElement('select');
    select.className = 'form-control'; select.setAttribute('aria-label', '触发功能');
    for (const [event, field, label] of functions) select.add(new Option(label, `${event}.${field}`));
    if (rule.event) select.value = `${rule.event}.${rule.field}`;
    const value = document.createElement('input');
    value.className = 'form-control'; value.placeholder = '匹配值，如分类码 11';
    value.setAttribute('aria-label', '匹配值'); value.value = rule.value || '';
    const hex = document.createElement('input');
    hex.className = 'form-control'; hex.placeholder = '发送Hex，如 AA BB 01';
    hex.setAttribute('aria-label', '发送Hex'); hex.value = rule.hex || '';
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'btn'; remove.textContent = '删除';
    remove.onclick = () => { row.remove(); dirty = true; };
    row.append(select, value, hex, remove);
    element('vod-forward-rule-list').append(row);
}
function populate(c) {
    element('vod-forward-active').checked = c.enabled === true;
    element('vod-forward-type').value = c.protocol;
    element('vod-forward-port').value = c.port;
    element('vod-forward-baud').value = String(c.baudrate);
    element('vod-forward-rule-list').replaceChildren();
    (c.rules || []).forEach(addRule);
    dirty = false;
}
export function collectVodForwardConfig() {
    let port = element('vod-forward-port').value.trim();
    if (/^\d+$/.test(port)) port = `/dev/ttyS${Number(port)}`;
    if (!/^\/dev\/tty[\w-]+$/.test(port)) throw new Error('请填写串口号或完整的 /dev/tty… 路径');
    const rules = [...element('vod-forward-rule-list').children].map(row => {
        const [event, field] = row.querySelector('select').value.split('.');
        const inputs = row.querySelectorAll('input');
        const value = inputs[0].value.trim();
        const hex = inputs[1].value.replace(/\s+/g, '').toUpperCase();
        if (!value) throw new Error('每条映射都需要填写匹配值');
        if (!/^(?:[0-9A-F]{2}){1,1024}$/.test(hex)) throw new Error('Hex需为完整十六进制字节，最多1024字节');
        return { event, field, value, hex };
    });
    if (rules.length > 128) throw new Error('最多配置128条映射');
    return { enabled: element('vod-forward-active').checked, protocol: element('vod-forward-type').value,
        port, baudrate: Number(element('vod-forward-baud').value), rules };
}
export async function refreshVodForward() {
    if (loading || saving) return;
    loading = true;
    try {
        const system = requireResult(await apiGet('/config.json'));
        const mode = Number(system.vodMode ?? (system.enableVod ? 2 : 0));
        const visible = mode === 1 || mode === 2;
        const item = element('vod-forward-peripheral-item');
        item.style.display = visible ? 'flex' : 'none';
        if (!visible) {
            if (item.classList.contains('active')) document.querySelector('[data-peripheral="dmx512"]')?.click();
            element('config-vod-forward').classList.remove('active');
            return;
        }
        if (!dirty) {
            const config = requireResult(await apiAction('peripherals', 'get_config', { peripheral_type: 'vod_forward' }));
            if (!Array.isArray(config.rules)) throw new Error('设备尚未支持VOD转发配置，请更新播放器');
            populate(config);
            status('配置已读取；保存配置不会发送串口指令。');
        }
    } catch (error) { status(error.message, true); }
    finally { loading = false; }
}
export async function saveVodForward() {
    if (saving || loading) return;
    saving = true;
    const button = element('save-vod-forward-config-btn');
    button.disabled = true;
    try {
        const config = collectVodForwardConfig();
        requireResult(await apiAction('peripherals', 'set_config', { peripheral_type: 'vod_forward', ...config }));
        const persisted = requireResult(await apiAction('peripherals', 'get_config', { peripheral_type: 'vod_forward' }));
        const sameRules = Array.isArray(persisted.rules) && persisted.rules.length === config.rules.length &&
            config.rules.every((rule, index) => ['event', 'field', 'value', 'hex'].every(key => persisted.rules[index]?.[key] === rule[key]));
        if (!sameRules || ['enabled', 'protocol', 'port', 'baudrate'].some(key => persisted[key] !== config[key])) {
            throw new Error('保存后回读不一致，请重试');
        }
        populate(persisted);
        status('配置已保存并回读确认。');
        addToCommandLog('VOD转发', 'success', '端口及功能映射已保存');
    } catch (error) {
        status(error.message, true);
        addToCommandLog('VOD转发保存失败', 'error', error.message);
    } finally { saving = false; button.disabled = false; }
}
export function initializeVodForward() {
    if (initialized || !element('vod-forward-peripheral-item')) return;
    initialized = true;
    element('save-vod-forward-config-btn').addEventListener('click', saveVodForward);
    element('vod-forward-add-rule').addEventListener('click', () => { addRule(); dirty = true; });
    element('config-vod-forward').addEventListener('input', () => { dirty = true; });
    element('config-vod-forward').addEventListener('change', () => { dirty = true; });
    document.querySelector('[data-page="peripheral-management"]')?.addEventListener('click', refreshVodForward);
    refreshVodForward();
}
