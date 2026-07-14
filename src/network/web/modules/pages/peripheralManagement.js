// 中控配置页面模块
import { apiGet, apiPost, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { subscribeSSEEvent } from '../core/connectionManager.js';
import { showConfirm, showPrompt, escapeHtml } from '../components/toast.js';

// SSE事件订阅（用于接收TCP/UDP/串口数据）
let peripheralSSEUnsubscribers = [];
let knownSerialTemplateCodes = new Set();
let serialTemplatesInitialized = false;
let preferredSerialTemplateCode = '';
const FORWARD_FUNCTION_ID = 'forward_payload';
let serialPortOptions = [];
let rs232SerialPortOptions = [];
let rs485SerialPortOptions = [];

const DEFAULT_FUNCTION_CONFIG = {
    hvac: {
        enabled: true,
        title: '空调',
        default_temperature: 26,
        temperature_min: 16,
        temperature_max: 32,
        default_mode: 'cool',
        default_fan_speed: 3,
        items: [
            { id: 'temp_down', label: '降温', kind: 'temperature', enabled: true, functions: [] },
            { id: 'temp_up', label: '升温', kind: 'temperature', enabled: true, functions: [] },
            { id: 'power', label: '打开', kind: 'power', enabled: true, functions: [] },
            { id: 'cool', label: '制冷', kind: 'mode', mode: 'cool', enabled: true, functions: [] },
            { id: 'heat', label: '制热', kind: 'mode', mode: 'heat', enabled: true, functions: [] },
            { id: 'fan_low', label: '低风', kind: 'fan', speed: 1, enabled: true, functions: [] },
            { id: 'fan_medium', label: '中风', kind: 'fan', speed: 2, enabled: true, functions: [] },
            { id: 'fan_high', label: '高风', kind: 'fan', speed: 3, enabled: true, functions: [] }
        ]
    },
    lighting: {
        enabled: true,
        title: '灯光',
        dimmer: {
            enabled: true,
            default_value: 50
        },
        items: [
            { id: 'spot', label: '射灯', enabled: true, default_on: true, functions: [] },
            { id: 'strip', label: '灯带', enabled: true, default_on: false, functions: [] },
            { id: 'floor', label: '地台灯', enabled: true, default_on: false, functions: [] },
            { id: 'flood', label: '聚光灯', enabled: true, default_on: false, functions: [] }
        ]
    }
};

let functionConfigState = cloneFunctionConfig(DEFAULT_FUNCTION_CONFIG);
let activeFunctionRegion = 'hvac';
let activeFunctionButtonId = 'power';
let functionConfigAvailableFunctions = [];

function cloneFunctionConfig(config) {
    return JSON.parse(JSON.stringify(config || DEFAULT_FUNCTION_CONFIG));
}

function clampNumber(value, min, max, fallback) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return fallback;
    return Math.max(min, Math.min(max, numeric));
}

function normalizeFunctionConfig(config = {}) {
    const normalized = cloneFunctionConfig(DEFAULT_FUNCTION_CONFIG);
    const incoming = config && typeof config === 'object' ? config : {};
    const incomingHvac = incoming.hvac && typeof incoming.hvac === 'object' ? incoming.hvac : {};
    const incomingLighting = incoming.lighting && typeof incoming.lighting === 'object' ? incoming.lighting : {};

    normalized.hvac.enabled = incomingHvac.enabled !== false;
    normalized.hvac.title = String(incomingHvac.title || normalized.hvac.title);
    normalized.hvac.temperature_min = clampNumber(incomingHvac.temperature_min, 0, 50, normalized.hvac.temperature_min);
    normalized.hvac.temperature_max = clampNumber(incomingHvac.temperature_max, 1, 60, normalized.hvac.temperature_max);
    if (normalized.hvac.temperature_max < normalized.hvac.temperature_min) {
        normalized.hvac.temperature_max = normalized.hvac.temperature_min;
    }
    normalized.hvac.default_temperature = clampNumber(
        incomingHvac.default_temperature,
        normalized.hvac.temperature_min,
        normalized.hvac.temperature_max,
        normalized.hvac.default_temperature
    );
    normalized.hvac.default_mode = String(incomingHvac.default_mode || normalized.hvac.default_mode);
    normalized.hvac.default_fan_speed = clampNumber(incomingHvac.default_fan_speed, 1, 3, normalized.hvac.default_fan_speed);

    const hvacItemsById = new Map((Array.isArray(incomingHvac.items) ? incomingHvac.items : [])
        .filter(item => item && typeof item === 'object')
        .map(item => [String(item.id || ''), item]));
    normalized.hvac.items = normalized.hvac.items.map(item => {
        const incomingItem = hvacItemsById.get(item.id) || {};
        return {
            ...item,
            label: String(incomingItem.label || item.label),
            enabled: incomingItem.enabled !== false,
            functions: Array.isArray(incomingItem.functions)
                ? incomingItem.functions
                : (Array.isArray(item.functions) ? item.functions : [])
        };
    });

    normalized.lighting.enabled = incomingLighting.enabled !== false;
    normalized.lighting.title = String(incomingLighting.title || normalized.lighting.title);
    const incomingDimmer = incomingLighting.dimmer && typeof incomingLighting.dimmer === 'object'
        ? incomingLighting.dimmer
        : {};
    normalized.lighting.dimmer.enabled = incomingDimmer.enabled !== false;
    normalized.lighting.dimmer.default_value = clampNumber(
        incomingDimmer.default_value,
        0,
        100,
        normalized.lighting.dimmer.default_value
    );

    const lightItemsById = new Map((Array.isArray(incomingLighting.items) ? incomingLighting.items : [])
        .filter(item => item && typeof item === 'object')
        .map(item => [String(item.id || ''), item]));
    normalized.lighting.items = normalized.lighting.items.map(item => {
        const incomingItem = lightItemsById.get(item.id) || {};
        return {
            ...item,
            label: String(incomingItem.label || item.label),
            enabled: incomingItem.enabled !== false,
            default_on: incomingItem.default_on === true,
            functions: Array.isArray(incomingItem.functions)
                ? incomingItem.functions
                : (Array.isArray(item.functions) ? item.functions : [])
        };
    });

    return normalized;
}

function normalizeHexInput(value) {
    const raw = String(value || '').trim();
    if (!raw) {
        return { ok: false, message: '命令码不能为空' };
    }

    const compact = raw
        .replace(/0x/gi, '')
        .replace(/[\s:-]+/g, '')
        .toUpperCase();

    if (!compact) {
        return { ok: false, message: '命令码不能为空' };
    }
    if (!/^[0-9A-F]+$/.test(compact)) {
        return { ok: false, message: '命令码只能包含 0-9 / A-F' };
    }
    if (compact.length % 2 !== 0) {
        return { ok: false, message: '命令码长度必须为偶数' };
    }

    return { ok: true, value: compact };
}

function normalizeForwardCommandMappings(value) {
    let items = value;
    if (typeof items === 'string') {
        try {
            items = JSON.parse(items);
        } catch (_) {
            items = [];
        }
    }
    if (!Array.isArray(items)) return [];

    return items.map(item => {
        if (!item || typeof item !== 'object') return null;
        const sourceRaw = item.source_hex || item.source || item.from || item.input_hex || item.trigger || '';
        const targetRaw = item.target_hex || item.target || item.to || item.output_hex || item.data || item.hex || '';
        const source = normalizeHexInput(sourceRaw);
        const target = normalizeHexInput(targetRaw);
        if (!source.ok || !target.ok) return null;
        return {
            source_hex: source.value,
            target_hex: target.value
        };
    }).filter(Boolean);
}

function forwardCommandMappingSignature(mappings) {
    return normalizeForwardCommandMappings(mappings)
        .map(item => `${item.source_hex}>${item.target_hex}`)
        .join('|');
}

function forwardFunctionIdentity(config) {
    const fixedType = config?.fixed_payload_type || config?.payload_type || '';
    const fixedPayload = config?.payload || '';
    const fixedHex = config?.hex || '';
    return [
        FORWARD_FUNCTION_ID,
        config?.forward_target || '',
        config?.forward_port || '',
        config?.forward_baudrate || '',
        config?.forward_address || '',
        config?.forward_mode || '',
        config?.multicast_group || '',
        config?.broadcast ? 'broadcast' : '',
        config?.multicast ? 'multicast' : '',
        config?.fixed_send ? 'fixed' : '',
        fixedType,
        fixedPayload,
        fixedHex,
        forwardCommandMappingSignature(config?.command_mappings)
    ].join(':');
}

function textToHexString(value) {
    return Array.from(new TextEncoder().encode(String(value || '')))
        .map(byte => byte.toString(16).padStart(2, '0'))
        .join('')
        .toUpperCase();
}

function isFixedForwardConfig(config) {
    return !!config && (
        config.fixed_send === true ||
        config.forward_fixed === true ||
        !!config.fixed_payload_type ||
        !!config.payload ||
        !!config.hex
    );
}

function getForwardPayloadPreview(value, maxLength = 18) {
    const text = String(value || '');
    if (text.length <= maxLength) return text;
    return `${text.slice(0, maxLength)}...`;
}

function networkTriggerMatchesHex(trigger, hex) {
    const triggerHex = normalizeHexInput(trigger);
    const incomingHex = normalizeHexInput(hex);
    return triggerHex.ok && incomingHex.ok && triggerHex.value === incomingHex.value;
}

function shouldUseNameAsNetworkTrigger(trigger, name) {
    const triggerValue = String(trigger || '').trim();
    const nameValue = String(name || '').trim();
    if (!triggerValue || !nameValue || triggerValue === nameValue) return false;

    const looksLikeRowNumber = /^[1-9]\d{0,2}$/.test(triggerValue);
    const nameHex = normalizeHexInput(nameValue);
    return looksLikeRowNumber && nameHex.ok && nameHex.value.length >= 4;
}

function getEffectiveNetworkTriggerValue(item) {
    if (!item) return '';
    const rawTrigger = String(item.trigger || '').trim();
    const rawName = String(item.name || '').trim();
    const value = shouldUseNameAsNetworkTrigger(rawTrigger, rawName)
        ? rawName
        : rawTrigger;
    const normalized = normalizeHexInput(value);
    return normalized.ok ? normalized.value : value;
}

function buildForwardFunctionEntry() {
    return {
        id: FORWARD_FUNCTION_ID,
        action: 'forward_payload',
        code: 0,
        category: '转发',
        name: '转发收到的指令',
        description: '将命中的原始指令转发到其他端口或协议'
    };
}

function buildFixedSendFunctionEntry() {
    return {
        id: FORWARD_FUNCTION_ID,
        action: 'forward_payload',
        code: 0,
        category: '外设指令',
        name: '发送固定指令',
        description: '点击手机控制按钮时，向串口、UDP、TCP 或 WebSocket 主动发送固定内容',
        fixedSend: true
    };
}

function isForwardFunctionLike(func) {
    return !!func && (func.id === FORWARD_FUNCTION_ID || func.action === 'forward_payload');
}

function isForwardChip(chip) {
    return !!chip && chip.dataset.functionId === FORWARD_FUNCTION_ID;
}

function functionIdentityFromChip(chip) {
    if (!chip) return '';
    const functionId = chip.dataset.functionId || '';
    if (functionId === FORWARD_FUNCTION_ID) {
        return forwardFunctionIdentity(getForwardConfigFromChip(chip));
    }
    return functionId;
}

/**
 * @brief 初始化SSE连接以接收外设数据
 */
function initializePeripheralSSE() {
    peripheralSSEUnsubscribers.forEach(unsubscribe => unsubscribe());
    peripheralSSEUnsubscribers = [];

    peripheralSSEUnsubscribers.push(subscribeSSEEvent('tcp_recv', function (event) {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'tcp_recv') {
                const hex = data.hex || '';
                const preview = data.preview || '';
                const fd = data.fd || '?';
                const ip = data.ip || 'unknown';
                const port = data.port || '?';
                appendLog('RECV', `[TCP ${ip}:${port} fd=${fd}] ${preview} | hex: ${hex}`);
            }
        } catch (e) {
            console.error('Error handling tcp_recv SSE:', e);
        }
    }));

    peripheralSSEUnsubscribers.push(subscribeSSEEvent('udp_recv', function (event) {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'udp_recv') {
                const hex = data.hex || '';
                const preview = data.preview || '';
                appendLog('RECV', `[UDP] ${preview} | hex: ${hex}`);
            }
        } catch (e) {
            console.error('Error handling udp_recv SSE:', e);
        }
    }));

    peripheralSSEUnsubscribers.push(subscribeSSEEvent('serial_recv', function (event) {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'serial_recv') {
                const port = data.port || '';
                const hex = data.hex || '';
                const preview = data.preview || '';
                const serialType = data.serial_type || (isRs485Port(port) ? 'rs485' : 'rs232');
                appendLog('RECV', `[${serialType.toUpperCase()} ${port}] ${preview} | hex: ${hex}`);
            }
        } catch (e) {
            console.error('Error handling serial_recv SSE:', e);
        }
    }));

    peripheralSSEUnsubscribers.push(subscribeSSEEvent('peripheral_trigger', function (event) {
        try {
            const data = JSON.parse(event.data);
            if (data.type === 'peripheral_trigger') {
                const source = data.source || 'PERIPHERAL';
                const status = data.status || 'info';
                const trigger = data.trigger ? ` ${data.trigger}` : '';
                const hex = data.hex ? ` hex:${data.hex}` : '';
                const detail = data.message || '';
                const configured = typeof data.trigger_count !== 'undefined'
                    ? ` | configured:${data.trigger_count}${data.configured ? ` ${data.configured}` : ''}`
                    : '';
                const param = data.param ? ` | param:${data.param}` : '';
                const logType = status === 'success' || status === 'matched'
                    ? 'SYSTEM'
                    : (status === 'miss' || status === 'empty' || status === 'failed' || status === 'error' ? 'ERROR' : 'SYSTEM');
                appendLog(logType, `[${source}] ${status}${trigger}${hex} ${detail}${configured}${param}`);
                if (status === 'miss') {
                    autosaveLocalNetworkConfigIfMissed(data);
                }
            }
        } catch (e) {
            console.error('Error handling peripheral_trigger SSE:', e);
        }
    }));
}

/**
 * UDP/TCP 墙板状态：每种类型保存一组触发模板
 * 每个触发模板形状：{ code: string, trigger: string, name: string, functions: Array<FunctionData> }
 *  - code:    本地稳定 ID（与触发字符串相同，便于持久化与恢复）
 *  - trigger: 实际匹配字符串
 *  - name:    友好名称
 *  - functions: 命中触发后按顺序执行的功能链
 */
const networkTriggers = {
    udp: [],
    tcp: []
};

const networkAutosaveTimers = {
    udp: null,
    tcp: null
};

// 当前选中触发的 code（每种类型独立）
const networkActiveTriggerCode = {
    udp: '',
    tcp: ''
};

let dmxPlaylistCache = null;
let lastDmxPlaylistId = null;
let lastDmxPlaylistValue = null;
const DMX_MATERIAL_DIRECTORY_STEP = 10;
const DMX_DEFAULT_FINE_TUNE_STEP = 5;
const DMX_BPM_DEFAULT_VALUE = 0;
const DMX_BPM_TABLE = [
    95, 115, 118, 126, 128, 132, 140, 150, 155
];
const DMX_EFFECT_COLOR_LABELS = {
    0: '0',
    5: '红',
    10: '绿',
    15: '蓝',
    20: '白',
    25: '七彩'
};
const DMX_EFFECT_LABELS = {
    1: '闪白',
    2: '闪黑',
    3: '红色',
    4: '绿色',
    5: '蓝色',
    6: '扫描',
    7: '散开',
    8: '描边',
    9: '反色',
    10: '扫描',
    12: '流光',
    13: '帘幕',
    14: '缩放',
    15: '旋转',
    16: '彩光',
    17: '分屏',
    18: '圆形',
    19: '三角',
    20: '矩形',
    21: '星形',
    22: '六边',
    23: '菱形',
    24: '心形',
    25: '花瓣',
    26: 'LOGO',
    27: '感情',
    28: '灵魂',
    29: '抖动',
    31: '毛刺',
    32: '幻觉',
    33: '立体',
    34: '跑马',
    35: '残影',
    36: '霓虹',
    37: '玻璃',
    38: '万花',
    39: '分割',
    40: '拼接'
};
const DMX_EFFECT_SEQUENCE = [
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    40, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 31, 32, 33, 34, 35, 36, 37, 38,
    39
];

const DMX_EFFECT_HELP_ROWS = [
    ['0', '关闭效果'],
    ...DMX_EFFECT_SEQUENCE.map((effectId, index) => {
        const start = index * DMX_DEFAULT_FINE_TUNE_STEP + 1;
        const end = (index + 1) * DMX_DEFAULT_FINE_TUNE_STEP;
        return [`${start}-${end}`, DMX_EFFECT_LABELS[effectId]];
    }),
    [`${DMX_EFFECT_SEQUENCE.length * DMX_DEFAULT_FINE_TUNE_STEP + 1}-255`, '未分配，不改变当前效果']
];

const DMX_CHANNEL_HELP = [
    {
        title: 'CH-01 开启',
        summary: 'DMX 总开关。关闭时其它通道不会继续控制画面。',
        rows: [
            ['0', '关闭 DMX 控制，并还原运行状态'],
            ['1-255', '开启 DMX 控制']
        ]
    },
    {
        title: 'CH-02 总控亮度',
        summary: '控制屏幕总亮度。',
        rows: [
            ['0', '黑屏'],
            ['1-255', '亮度由低到高']
        ]
    },
    {
        title: 'CH-03 R',
        summary: '素材颜色叠加的红色通道。',
        rows: [
            ['0', '红色关闭'],
            ['1-255', '红色强度由低到高'],
            ['CH3/4/5 全为 0', '关闭素材颜色叠加']
        ]
    },
    {
        title: 'CH-04 G',
        summary: '素材颜色叠加的绿色通道。',
        rows: [
            ['0', '绿色关闭'],
            ['1-255', '绿色强度由低到高'],
            ['CH3/4/5 全为 0', '关闭素材颜色叠加']
        ]
    },
    {
        title: 'CH-05 B',
        summary: '素材颜色叠加的蓝色通道。',
        rows: [
            ['0', '蓝色关闭'],
            ['1-255', '蓝色强度由低到高'],
            ['CH3/4/5 全为 0', '关闭素材颜色叠加']
        ]
    },
    {
        title: 'CH-06 素材目录',
        summary: '按 10 档选择绑定了 DMX ID 的播放列表。',
        rows: [
            ['0', '不切换目录，保持当前目录'],
            ['1-10', '选择 DMX ID 10 的播放列表'],
            ['11-20', '选择 DMX ID 20 的播放列表'],
            ['21-30', '选择 DMX ID 30 的播放列表'],
            ['以此类推', '每 10 一个档位']
        ]
    },
    {
        title: 'CH-07 指定素材',
        summary: '配合 CH-06，在当前播放列表里选择素材序号。',
        rows: [
            ['0', '默认播放第 1 个素材'],
            ['1-5', '第 1 个素材'],
            ['6-10', '第 2 个素材'],
            ['11-15', '第 3 个素材'],
            ['以此类推', '每 5 一个素材档位']
        ]
    },
    {
        title: 'CH-08 场景',
        summary: '按 5 档切换场景。',
        rows: [
            ['0', '不改变当前场景'],
            ['1-5', '第 1 个场景'],
            ['6-10', '第 2 个场景'],
            ['11-15', '第 3 个场景'],
            ['以此类推', '每 5 一个场景档位']
        ]
    },
    {
        title: 'CH-09 效果',
        summary: '按连续有效效果每 5 档选择，已删除的隧道/光源不占中间档位，目标图层由 CH-11 决定。',
        rows: DMX_EFFECT_HELP_ROWS
    },
    {
        title: 'CH-10 效果颜色',
        summary: '根据 CH-09 当前效果设置颜色；RGB 描边、流光、边缘跑马、霓虹描边响应颜色，形状分割可用彩色条块；卡点拼接固定黑底视频遮罩。',
        rows: [
            ['0', '当前效果使用默认/七彩'],
            ['1-5', '当前效果：红色'],
            ['6-10', '当前效果：绿色'],
            ['11-15', '当前效果：蓝色'],
            ['16-20', '当前效果：白色'],
            ['21-255', '当前效果：七彩；分割为黑底七彩条块，卡点拼接不变色']
        ]
    },
    {
        title: 'CH-11 联动切换',
        summary: '决定 CH-06 播放列表和 CH-09 效果关联到哪些图层。',
        rows: [
            ['0', '只控制 CH-06 选中播放列表绑定的图层'],
            ['1-50', '联动图层 2'],
            ['60-100', '联动图层 3'],
            ['110-150', '联动图层 4'],
            ['160-255', '联动图层 1-4']
        ]
    },
    {
        title: 'CH-12 BPM',
        summary: '决定效果节奏来源。',
        rows: [
            ['0', '音频检测'],
            ['5', '95 BPM'],
            ['10', '115 BPM'],
            ['15', '118 BPM'],
            ['20', '126 BPM'],
            ['25', '128 BPM'],
            ['30', '132 BPM'],
            ['35', '140 BPM'],
            ['40', '150 BPM'],
            ['45', '155 BPM']
        ]
    }
];

function normalizeDmxBucket(value, step = DMX_MATERIAL_DIRECTORY_STEP) {
    const numericValue = Math.max(0, Math.min(255, parseInt(value, 10) || 0));
    if (numericValue === 0) return 0;
    return Math.min(255, (Math.floor((numericValue - 1) / step) + 1) * step);
}

function normalizeDmxUiValue(offset, value) {
    if (offset === 5) {
        return normalizeDmxBucket(value);
    }
    const numericValue = Math.max(0, Math.min(255, parseInt(value, 10) || 0));
    if (offset === 8) {
        if (numericValue === 0) return 0;
        return Math.min(255, (Math.floor((numericValue - 1) / DMX_DEFAULT_FINE_TUNE_STEP) + 1) * DMX_DEFAULT_FINE_TUNE_STEP);
    }
    if (offset === 9) {
        if (numericValue === 0) return 0;
        return Math.min(50, (Math.floor((numericValue - 1) / DMX_DEFAULT_FINE_TUNE_STEP) + 1) * DMX_DEFAULT_FINE_TUNE_STEP);
    }
    if (offset === 11) {
        if (numericValue === 0) return 0;
        return Math.min(255, (Math.floor((numericValue - 1) / DMX_DEFAULT_FINE_TUNE_STEP) + 1) * DMX_DEFAULT_FINE_TUNE_STEP);
    }
    return numericValue;
}

function getDmxEffectColorLabel(value) {
    const numericValue = normalizeDmxUiValue(9, value);
    if (numericValue >= 25) return '七彩';
    return DMX_EFFECT_COLOR_LABELS[numericValue] || String(numericValue);
}

function getDmxEffectLabel(value) {
    const numericValue = normalizeDmxUiValue(8, value);
    if (numericValue === 0) return '0';
    const bucketIndex = Math.floor(numericValue / DMX_DEFAULT_FINE_TUNE_STEP) - 1;
    const effectId = DMX_EFFECT_SEQUENCE[bucketIndex];
    return DMX_EFFECT_LABELS[effectId] || '未分配';
}

function getDmxBpmFromValue(value) {
    const numericValue = Math.max(0, Math.min(255, parseInt(value, 10) || 0));
    if (numericValue === 0) return 'Audio';
    const bucket = Math.min(255, (Math.floor((numericValue - 1) / DMX_DEFAULT_FINE_TUNE_STEP) + 1) * DMX_DEFAULT_FINE_TUNE_STEP);
    const index = Math.max(0, Math.min(DMX_BPM_TABLE.length - 1, Math.floor(bucket / DMX_DEFAULT_FINE_TUNE_STEP) - 1));
    return DMX_BPM_TABLE[index];
}

let dmxHelpHideTimer = null;

function getDmxChannelHelp(offset) {
    return DMX_CHANNEL_HELP[offset] || {
        title: `CH-${String(offset + 1).padStart(2, '0')} 通道说明`,
        summary: '该通道暂无说明。',
        rows: []
    };
}

function ensureDmxHelpTooltip() {
    let tooltip = document.getElementById('dmx-channel-help-popover');
    if (tooltip) return tooltip;

    tooltip = document.createElement('div');
    tooltip.id = 'dmx-channel-help-popover';
    tooltip.className = 'dmx-help-popover';
    tooltip.setAttribute('role', 'tooltip');
    tooltip.innerHTML = `
        <div id="dmx-help-title" class="dmx-help-title">DMX 通道说明</div>
        <p id="dmx-help-summary" class="dmx-help-summary"></p>
        <div id="dmx-help-table" class="dmx-help-table"></div>
    `;
    document.body.appendChild(tooltip);

    tooltip.addEventListener('mouseenter', () => {
        if (dmxHelpHideTimer) {
            clearTimeout(dmxHelpHideTimer);
            dmxHelpHideTimer = null;
        }
    });
    tooltip.addEventListener('mouseleave', hideDmxChannelHelpSoon);

    const reposition = () => {
        if (tooltip.classList.contains('active') && tooltip._anchorEl) {
            positionDmxHelpTooltip(tooltip._anchorEl, tooltip);
        }
    };
    window.addEventListener('resize', reposition);
    window.addEventListener('scroll', reposition, true);
    return tooltip;
}

function positionDmxHelpTooltip(anchorEl, tooltip) {
    const rect = anchorEl.getBoundingClientRect();
    const gap = 10;
    const margin = 10;
    const viewportWidth = window.innerWidth || document.documentElement.clientWidth;
    const viewportHeight = window.innerHeight || document.documentElement.clientHeight;
    const tooltipWidth = tooltip.offsetWidth || 340;
    const tooltipHeight = tooltip.offsetHeight || 260;

    let left = rect.right + gap;
    let top = rect.top + (rect.height / 2) - (tooltipHeight / 2);
    let placement = 'right';

    if (left + tooltipWidth > viewportWidth - margin) {
        left = rect.left - tooltipWidth - gap;
        placement = 'left';
    }
    if (left < margin) {
        left = Math.max(margin, viewportWidth - tooltipWidth - margin);
        placement = 'center';
    }

    top = Math.max(margin, Math.min(top, viewportHeight - tooltipHeight - margin));
    tooltip.style.left = `${left}px`;
    tooltip.style.top = `${top}px`;
    tooltip.dataset.placement = placement;
}

function showDmxChannelHelp(offset, anchorEl) {
    const help = getDmxChannelHelp(offset);
    const tooltip = ensureDmxHelpTooltip();
    const titleEl = tooltip.querySelector('#dmx-help-title');
    const summaryEl = tooltip.querySelector('#dmx-help-summary');
    const tableEl = tooltip.querySelector('#dmx-help-table');

    if (titleEl) titleEl.textContent = help.title;
    if (summaryEl) summaryEl.textContent = help.summary;
    if (tableEl) {
        tableEl.innerHTML = help.rows.map(([range, description]) => `
            <div class="dmx-help-row">
                <div class="dmx-help-range">${escapeHtml(range)}</div>
                <div class="dmx-help-desc">${escapeHtml(description)}</div>
            </div>
        `).join('');
    }
    if (dmxHelpHideTimer) {
        clearTimeout(dmxHelpHideTimer);
        dmxHelpHideTimer = null;
    }
    tooltip._anchorEl = anchorEl || null;
    tooltip.classList.add('active');
    if (anchorEl) {
        anchorEl.setAttribute('aria-describedby', 'dmx-channel-help-popover');
        positionDmxHelpTooltip(anchorEl, tooltip);
    }
}

function hideDmxChannelHelpSoon() {
    if (dmxHelpHideTimer) clearTimeout(dmxHelpHideTimer);
    dmxHelpHideTimer = setTimeout(() => {
        const tooltip = document.getElementById('dmx-channel-help-popover');
        if (!tooltip) return;
        tooltip.classList.remove('active');
        if (tooltip._anchorEl) {
            tooltip._anchorEl.removeAttribute('aria-describedby');
            tooltip._anchorEl = null;
        }
    }, 120);
}

function updateDmxInputModeUI() {
    const mode = document.getElementById('dmx-input-mode')?.value || 'local';
    const isExternal = mode === 'external';
    document.querySelectorAll('.dmx-external-only').forEach(el => {
        el.style.display = isExternal ? '' : 'none';
    });
    const localPort = document.getElementById('dmx-port');
    const localBaud = document.getElementById('dmx-baudrate');
    if (localPort) localPort.disabled = isExternal;
    if (localBaud) {
        localBaud.value = isExternal ? '115200' : '250000';
        localBaud.disabled = true;
    }
}

function isDmxInfoHintEnabled() {
    const input = document.getElementById('dmx-info-show');
    return input ? input.checked : false;
}

async function syncPlaylistFromDmxValue(value) {
    const directoryBucket = normalizeDmxBucket(value);
    if (directoryBucket === 0) {
        lastDmxPlaylistValue = directoryBucket;
        return;
    }

    if (!dmxPlaylistCache) {
        const response = await apiGet('/playlists');
        dmxPlaylistCache = Array.isArray(response) ? response : [];
    }

    if (!dmxPlaylistCache.length) {
        lastDmxPlaylistValue = null;
        return;
    }

    let playlist = dmxPlaylistCache.find(item =>
        normalizeDmxBucket(item?.dmxId || 0) === directoryBucket
    );
    if (!playlist) {
        const response = await apiGet('/playlists');
        dmxPlaylistCache = Array.isArray(response) ? response : [];
        playlist = dmxPlaylistCache.find(item =>
            normalizeDmxBucket(item?.dmxId || 0) === directoryBucket
        );
    }
    if (!playlist) {
        lastDmxPlaylistValue = null;
        return;
    }
    if (lastDmxPlaylistValue === directoryBucket &&
        String(playlist.id) === String(lastDmxPlaylistId)) {
        return;
    }
    if (String(playlist.id) === String(lastDmxPlaylistId)) {
        lastDmxPlaylistValue = directoryBucket;
        return;
    }

    lastDmxPlaylistValue = directoryBucket;
    lastDmxPlaylistId = playlist.id;
    window.dispatchEvent(new CustomEvent('playlistSwitch', {
        detail: { playlistId: playlist.id }
    }));
}

// 初始化中控配置
export function initializePeripheralManagement() {
    // 初始化SSE连接以接收外设数据
    initializePeripheralSSE();

    const peripheralItems = document.querySelectorAll('.peripheral-item');
    const panelTabs = document.querySelectorAll('.panel-tab[data-peripheral-tab]');
    const sendTestBtn = document.getElementById('send-test-btn');
    const clearLogBtn = document.getElementById('clear-peripheral-log-btn');

    // 1. 左侧外设选择逻辑
    peripheralItems.forEach(item => {
        item.addEventListener('click', () => {
            const peripheral = item.getAttribute('data-peripheral');

            // 更新列表选中态
            peripheralItems.forEach(p => p.classList.remove('active'));
            item.classList.add('active');

            // 更新配置详情显示
            const sections = document.querySelectorAll('.config-section');
            sections.forEach(s => s.classList.remove('active'));

            const targetSec = document.getElementById(`config-${peripheral}`);
            if (targetSec) targetSec.classList.add('active');

            // 同步测试界面的 Badge
            const badge = document.getElementById('current-test-target-label');
            const targetLabels = {
                dmx512: 'DMX512',
                serial: '串口',
                udp: 'UDP',
                tcp: 'TCP',
                'function-config': '功能配置'
            };
            if (badge) badge.textContent = targetLabels[peripheral] || peripheral.toUpperCase();

            // 切换到串口/UDP/TCP 时确保功能列表已加载
            if (peripheral === 'serial') {
                setTimeout(async () => {
                    const functionsContainer = document.querySelector('#config-serial .function-buttons-grid');
                    if (functionsContainer && (!window.availableFunctions || window.availableFunctions.length === 0)) {
                        await loadSerialFunctions();
                    }
                }, 100);
            } else if (peripheral === 'udp' || peripheral === 'tcp') {
                setTimeout(async () => {
                    await loadNetworkFunctions(peripheral);
                }, 100);
            } else if (peripheral === 'function-config') {
                setTimeout(async () => {
                    await loadFunctionConfigFunctions();
                }, 100);
            }
        });
    });

    // 2. 右侧页签切换逻辑
    panelTabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const targetTabId = tab.getAttribute('data-peripheral-tab');

            // 更新页签选中态
            panelTabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');

            // 更新页签内容显示
            const tabContents = document.querySelectorAll('.peripheral-tab');
            tabContents.forEach(c => c.classList.remove('active'));

            const targetTab = document.getElementById(targetTabId);
            if (targetTab) targetTab.classList.add('active');
        });
    });

    // 2.1 串口类型标签页切换逻辑
    const serialTypeTabs = document.querySelectorAll('.panel-tab[data-serial-type]');
    serialTypeTabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const serialType = tab.getAttribute('data-serial-type');

            // 更新标签页选中态
            serialTypeTabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');

            // 更新配置内容显示
            const typeConfigs = document.querySelectorAll('.serial-type-config');
            typeConfigs.forEach(c => c.classList.remove('active'));

            const targetConfig = document.getElementById(`serial-${serialType}`);
            if (targetConfig) targetConfig.classList.add('active');
        });
    });

    // 3. 动态列表管理
    // DMX 推子按帧节流：每帧最多发一次各通道最新值，减少请求堆积、提升屏幕反应
    let dmxFlushScheduled = false;
    const scheduleDmxFlush = () => {
        if (dmxFlushScheduled) return;
        dmxFlushScheduled = true;
        requestAnimationFrame(() => {
            dmxFlushScheduled = false;
            const container = document.getElementById('dmx-mapping-container');
            if (!container) return;
            const cards = container.querySelectorAll('.dmx-channel-card');
            const enableCard = cards[0];
            const pendingEnableValue = enableCard && enableCard._dmxPendingValue !== undefined
                ? enableCard._dmxPendingValue
                : null;
            const enableSlider = enableCard ? enableCard.querySelector('.dmx-vertical-slider') : null;
            const enableValue = pendingEnableValue !== null
                ? pendingEnableValue
                : parseInt(enableSlider?.value || '0', 10);
            const dmxEnabled = enableValue !== 0;
            cards.forEach((card, idx) => {
                if (card._dmxPendingValue === undefined) return;
                let value = card._dmxPendingValue;
                card._dmxPendingValue = undefined;
                if (idx > 0 && idx < 12 && !dmxEnabled && enableCard) {
                    enableCard._dmxPendingValue = 255;
                    if (typeof enableCard.updateValue === 'function') {
                        enableCard.updateValue(255, 'auto');
                    }
                    scheduleDmxFlush();
                }
                apiAction('peripheral-events', 'set_channel', {
                        offset: idx,
                        value,
                        show_info: isDmxInfoHintEnabled()
                    }).catch(() => {});
            });
        });
    };

    // 添加 DMX 映射
    const addDmxChannelCard = (index = null) => {
        const container = document.getElementById('dmx-mapping-container');
        if (!container) return;

        const offset = index !== null ? index : container.querySelectorAll('.dmx-channel-card').length;
        const channelNum = offset + 1;
        const channelFuncs = [
            { value: 'mode', label: '开启' },
            { value: 'master', label: '总控亮度' },
            { value: 'r', label: 'R' },
            { value: 'g', label: 'G' },
            { value: 'b', label: 'B' },
            { value: 'material_directory', label: '素材目录' },
            { value: 'material', label: '指定素材' },
            { value: 'scene', label: '场景' },
            { value: 'effect', label: '效果' },
            { value: 'effect_color', label: '效果颜色' },
            { value: 'playlist_link', label: '联动切换' },
            { value: 'bpm', label: 'BPM' }
        ];
        const fixedFunctionLabel = channelFuncs[offset]?.label || '无功能';
        const fineTuneStep = offset === 5 ? DMX_MATERIAL_DIRECTORY_STEP : DMX_DEFAULT_FINE_TUNE_STEP;
        const sliderStep = (offset === 5 || offset === 8 || offset === 9 || offset === 11) ? fineTuneStep : 1;
        const initialValue = offset === 11 ? DMX_BPM_DEFAULT_VALUE : 0;
        const card = document.createElement('div');
        card.className = 'dmx-channel-card';
        card.innerHTML = `
            <div class="dmx-card-header">
                <span class="dmx-fixed-func-label">${fixedFunctionLabel}</span>
                <button class="dmx-help-btn" type="button" aria-label="查看 CH-${channelNum} ${fixedFunctionLabel} 数值说明">?</button>
            </div>
            <div class="dmx-card-body">
                <div class="dmx-value-display">0</div>
                <div class="dmx-slider-container">
                    <input type="range" class="dmx-vertical-slider" min="0" max="255" step="${sliderStep}" value="${initialValue}">
                </div>
            </div>
            <div class="dmx-card-footer">
                <div class="dmx-fine-tune">
                    <button class="fine-tune-btn" data-dir="-${fineTuneStep}" title="减小"><svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg></button>
                    <span class="fine-tune-val dmx-dynamic-value">0</span>
                    <button class="fine-tune-btn" data-dir="${fineTuneStep}" title="增大"><svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="9 18 15 12 9 6"/></svg></button>
                </div>
                <div class="dmx-ch-label">CH-${(channelNum).toString().padStart(2, '0')}</div>
            </div>
        `;
        container.appendChild(card);

        // 状态跟踪
        card._isInteracting = false;

        // 事件绑定
        const slider = card.querySelector('.dmx-vertical-slider');
        const display = card.querySelector('.dmx-dynamic-value');
        const fineTuneBtns = card.querySelectorAll('.fine-tune-btn');
        const helpBtn = card.querySelector('.dmx-help-btn');

        const updateValue = (val, source = 'manual') => {
            const numericVal = normalizeDmxUiValue(offset, val);
            if (slider.value != numericVal) slider.value = numericVal;

            const valueDisplayBox = card.querySelector('.dmx-value-display');
            if (valueDisplayBox) valueDisplayBox.textContent = numericVal;
            if (display) {
                display.textContent = offset === 11
                    ? getDmxBpmFromValue(numericVal)
                    : (offset === 9
                        ? getDmxEffectColorLabel(numericVal)
                        : (offset === 8 ? getDmxEffectLabel(numericVal) : numericVal));
            }

            // 更新推杆背景进度色 (90deg 因为是在 CSS 中旋转了 -90deg)
            const percent = (numericVal / 255) * 100;
            slider.style.background = `linear-gradient(90deg, #6366f1 ${percent}%, #ffffff ${percent}%)`;

            if (offset === 5 && source !== 'init') {
                syncPlaylistFromDmxValue(numericVal).catch(() => {});
            }

            // 实时调试：标记待发送，由 requestAnimationFrame 按帧统一发送，避免请求堆积、屏幕反应慢
            if (source === 'manual') {
                updateDmxModeUI(true);
                card._dmxPendingValue = numericVal;
                card._dmxManualHoldUntil = Date.now() + 2000;
                scheduleDmxFlush();
            }
        };

        // 暴露 updateValue 供外部同步使用
        card.updateValue = updateValue;

        slider.addEventListener('input', (e) => {
            updateValue(e.target.value, 'manual');
        });

        // 交互状态跟踪
        const setInteracting = (state) => card._isInteracting = state;
        slider.addEventListener('mousedown', () => setInteracting(true));
        slider.addEventListener('touchstart', () => setInteracting(true), { passive: true });
        slider.addEventListener('mouseup', () => setInteracting(false));
        slider.addEventListener('touchend', () => setInteracting(false), { passive: true });
        slider.addEventListener('mouseleave', () => setInteracting(false));

        fineTuneBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                const dir = parseInt(btn.getAttribute('data-dir'));
                updateValue(parseInt(slider.value) + dir, 'manual');
            });
        });
        if (helpBtn) {
            const showHelp = () => showDmxChannelHelp(offset, helpBtn);
            helpBtn.addEventListener('mouseenter', showHelp);
            helpBtn.addEventListener('focus', showHelp);
            helpBtn.addEventListener('mouseleave', hideDmxChannelHelpSoon);
            helpBtn.addEventListener('blur', hideDmxChannelHelpSoon);
            helpBtn.addEventListener('click', (event) => {
                event.preventDefault();
                event.stopPropagation();
                showHelp();
            });
        }

        // 初始化背景色
        updateValue(initialValue, 'init');
    };


    // 默认生成 12 个通道卡片
    const dmxMappingContainer = document.getElementById('dmx-mapping-container');
    if (dmxMappingContainer && dmxMappingContainer.children.length === 0) {
        for (let i = 0; i < 12; i++) {
            addDmxChannelCard(i);
        }
    }

    // DMX 模式切换与模拟清空
    const dmxModeBtn = document.getElementById('dmx-mode-toggle-btn');
    dmxModeBtn?.addEventListener('click', async () => {
        // 如果当前是模拟模式，点击则清空并恢复实时
        if (dmxModeBtn.classList.contains('is-manual')) {
            // 直接执行，移除确认弹窗
            try {
                await apiAction('peripheral-events', 'clear_simulation', {});
                updateDmxModeUI(false);
                addToCommandLog('切换至实时数据', 'success', '已恢复硬件同步');
            } catch (e) {
                addToCommandLog('切换失败', 'error', e.message);
            }
        } else {
            // 当前是实时模式，点击切换到模拟模式（锁定当前值）
            // 直接执行，移除确认弹窗
            try {
                const container = document.getElementById('dmx-mapping-container');
                if (container) {
                    const cards = Array.from(container.children);
                    // 遍历所有卡片，发送当前值以锁定
                    for (let i = 0; i < cards.length; i++) {
                        const card = cards[i];
                        const slider = card.querySelector('.dmx-vertical-slider');
                        const val = parseInt(slider.value) || 0;

                        // 调用 updateValue 发送指令 (source='manual' 会触发 API 调用)
                        if (typeof card.updateValue === 'function') {
                            card.updateValue(val, 'manual');
                        }
                    }
                }
                updateDmxModeUI(true);
                addToCommandLog('进入模拟调试', 'success', '已锁定当前通道值');
            } catch (e) {
                addToCommandLog('进入模拟失败', 'error', e.message);
            }
        }
    });


    // 添加 RS232 指令映射 (该按钮事件已整合到下方的统一逻辑中)

    // 统一配置保存按钮绑定
    const SAVE_BUTTONS = {
        'save-dmx-btn': 'dmx',
        'save-rs232-btn': 'rs232',
        'save-rs485-btn': 'rs485',
        'save-udp-config-btn': 'udp',
        'save-tcp-config-btn': 'tcp'
    };
    Object.entries(SAVE_BUTTONS).forEach(([id, type]) => {
        document.getElementById(id)?.addEventListener('click', () => savePeripheralConfig(type));
    });
    document.getElementById('save-serial-config-btn')?.addEventListener('click', () => {
        savePeripheralConfig(getActiveSerialConfig().type);
    });
    document.getElementById('save-function-config-btn')?.addEventListener('click', saveFunctionConfig);
    renderFunctionConfigEditor(functionConfigState);
    initFunctionConfigWorkbench();
    document.getElementById('dmx-input-mode')?.addEventListener('change', updateDmxInputModeUI);
    updateDmxInputModeUI();

    // 绑定添加 RS232 指令映射行按钮（UDP/TCP 已切换为墙板布局，触发添加按钮在 initNetworkPeripheralFeatures 内绑定）
    document.getElementById('add-rs232-cmd-btn')?.addEventListener('click', () => addCommandMappingRow('rs232'));


    if (sendTestBtn) {
        sendTestBtn.addEventListener('click', async () => {
            await sendPeripheralTest();
        });
    }

    if (clearLogBtn) {
        clearLogBtn.addEventListener('click', () => {
            const logContainer = document.getElementById('peripheral-test-log');
            if (logContainer) logContainer.innerHTML = '';
        });
    }

    // 5. 启动 DMX 实时监控 (Mini Grid)
    initDmxMiniMonitor();

    // 6. 立即加载一次 DMX 状态，同步后端实际状态（确保状态的唯一性和准确性）
    loadDmxStatusOnInit();

    // 7. 初始化串口/UDP/TCP 相关功能
    initSerialFeatures();
    initNetworkPeripheralFeatures('udp');
    initNetworkPeripheralFeatures('tcp');

    // 8. 加载外设配置（包括功能列表）
    loadPeripheralConfigs();

    // 9. 如果当前显示的是串口配置，立即加载功能列表
    const serialSection = document.getElementById('config-serial');
    if (serialSection && serialSection.classList.contains('active')) {
        setTimeout(async () => {
            await loadSerialFunctions();
        }, 200);
    }
}

/**
 * @brief 初始化串口相关功能
 */
function initSerialFeatures() {
    // 7.1 串口学习模式开关
    const serialLearnModeEl = document.getElementById('serial-learn-mode');
    if (serialLearnModeEl) {
        serialLearnModeEl.addEventListener('change', async (e) => {
            const isEnabled = e.target.checked;
            const serialConfig = getActiveSerialConfig();
            if (isEnabled && !serialConfig.port) {
                addToCommandLog('学习模式设置失败', 'error', '请先选择串口号');
                e.target.checked = false;
                return;
            }
            try {
                const { type: peripheralType, ...serialParams } = serialConfig;
                const response = await apiAction('peripherals', 'set_learn_mode', {
                    peripheral_type: peripheralType,
                    ...serialParams,
                    enabled: isEnabled
                });

                // 检查响应是否成功
                if (response != null) {
                    const detail = `${serialConfig.type.toUpperCase()} ${serialConfig.port} @ ${serialConfig.baudrate}`;
                    addToCommandLog('学习模式', isEnabled ? 'success' : 'info',
                        isEnabled ? `已启用: ${detail}` : `已禁用: ${detail}`);

                    // 如果启用学习模式，启动自动刷新列表
                    if (isEnabled) {
                        startAutoRefreshTemplates();
                    } else {
                        stopAutoRefreshTemplates();
                    }
                } else {
                    // 后端返回错误
                    const errorMsg = response?.message || '未知错误';
                    addToCommandLog('学习模式设置失败', 'error', errorMsg);
                    e.target.checked = !isEnabled; // 恢复原状态
                }
            } catch (error) {
                addToCommandLog('学习模式设置失败', 'error', error.message || '网络错误');
                e.target.checked = !isEnabled; // 恢复原状态
            }
        });
    }

    // 7.2 串口模板列表交互（先绑定事件，数据加载在loadPeripheralConfigs中）
    initSerialTemplateList();

    // 7.2.1 清空串口模板
    document.getElementById('serial-clear-templates-btn')?.addEventListener('click', async () => {
        const serialType = getActiveSerialTemplateType();
        const confirmed = await showConfirm(`确定要清空所有 ${serialType.toUpperCase()} 串口模板吗？此操作不可恢复。`, '清空模板');
        if (!confirmed) return;
        try {
            await apiAction('peripherals', 'clear_templates', {
                    peripheral_type: serialType
                });
            resetSerial工作区();
            addToCommandLog('清空模板', 'success', `已清空所有 ${serialType.toUpperCase()} 串口模板`);
            await loadSerialTemplates();
        } catch (error) {
            addToCommandLog('清空模板失败', 'error', error.message);
        }
    });

    document.getElementById('serial-add-template-btn')?.addEventListener('click', async () => {
        await addManualSerialTemplate();
    });

    // 7.3 功能列表按钮事件（先绑定事件，数据加载在loadPeripheralConfigs中）
    initSerialFunctionButtons();

    // 7.4 工作区功能芯片交互
    initSerial工作区();

    // 7.5 搜索功能
    initSerialSearch();

    // 7.6 保存模板功能配置按钮
    document.getElementById('save-template-functions-btn')?.addEventListener('click', async () => {
        await saveTemplateFunctions();
    });

    document.getElementById('serial-port-select')?.addEventListener('change', async () => {
        resetSerial工作区();
        serialTemplatesInitialized = false;
        knownSerialTemplateCodes = new Set();
        await loadSerialTemplates();
    });
}

function getActiveSerialTemplateType() {
    return getActiveSerialConfig().type === 'rs485' ? 'rs485' : 'rs232';
}

function getActiveSerialTemplateHex() {
    const activeTemplate = document.querySelector('#config-serial .template-item.bg-active');
    return activeTemplate?.dataset.templateHex || activeTemplate?.dataset.templateCode || '';
}

async function addManualSerialTemplate() {
    const serialType = getActiveSerialTemplateType();
    const rawHex = await showPrompt(
        `请输入 ${serialType.toUpperCase()} 命令码（支持空格、冒号、短横线分隔）`,
        '手动添加命令码'
    );
    if (rawHex === null) return;

    const normalized = normalizeHexInput(rawHex);
    if (!normalized.ok) {
        addToCommandLog('手动添加命令码', 'warning', normalized.message);
        return;
    }

    try {
        const response = await apiAction('peripherals', 'add_template', {
                peripheral_type: serialType,
                data: normalized.value
            });

        if (response == null) {
            throw new Error(response?.message || '新增模板失败');
        }

        preferredSerialTemplateCode = response.template_code || response.code || normalized.value;
        resetSerial工作区();
        await loadSerialTemplates();
        addToCommandLog(
            '手动添加命令码',
            'success',
            response.created === false
                ? `命令码已存在，已定位到模板: ${preferredSerialTemplateCode}`
                : `已添加命令码: ${preferredSerialTemplateCode}`
        );
    } catch (error) {
        addToCommandLog('手动添加命令码失败', 'error', error.message || '网络错误');
    }
}

function resetSerial工作区() {
    const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');
    const workspaceHeader = document.querySelector('#config-serial .workspace-header .badge-tag');
    const saveBtn = document.getElementById('save-template-functions-btn');

    document.querySelectorAll('#config-serial .template-item.bg-active')
        .forEach(item => item.classList.remove('bg-active'));

    if (workspaceHeader) {
        workspaceHeader.textContent = '未选择模板';
    }
    if (saveBtn) {
        saveBtn.style.display = 'none';
    }
    if (workspaceCanvas) {
        workspaceCanvas.innerHTML = '';
        const empty = document.createElement('div');
        empty.className = 'workspace-empty-state';
        empty.textContent = '从右侧选择功能，点击即可添加到此处';
        workspaceCanvas.appendChild(empty);
    }
}

/**
 * @brief 初始化串口模板列表交互
 */
function initSerialTemplateList() {
    const templateList = document.querySelectorAll('#config-serial .template-item');
    const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');
    const workspaceHeader = document.querySelector('#config-serial .workspace-header .badge-tag');

    templateList.forEach(item => {
        // 点击模板项选中
        item.addEventListener('click', (e) => {
            // 如果点击的是操作按钮，不触发选中
            if (e.target.closest('.text-success, .template-rename-btn, .template-delete-btn')) {
                return;
            }

            // 移除其他项的选中状态
            templateList.forEach(t => t.classList.remove('bg-active'));
            item.classList.add('bg-active');

            // 更新工作区标题
            const templateName = item.dataset.templateName || '未命名';
            if (workspaceHeader) {
                workspaceHeader.textContent = templateName;
            }

            // 加载模板功能到工作区（异步加载数据库中的功能）
            loadTemplateTo工作区(item);
        });

        // 删除按钮
        const deleteBtn = item.querySelector('.template-delete-btn');
        if (deleteBtn) {
            deleteBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                const templateCode = item.dataset.templateCode || '';
                const displayCode = (item.dataset.templateHex || '').trim();
                const templateName = item.dataset.templateName || '';
                const serialType = item.dataset.templateType || getActiveSerialTemplateType();

                const confirmed = await showConfirm(`确定要删除模板 "${templateName}" (${displayCode || templateCode}) 吗？`);
                if (!confirmed) return;

                try {
                    const response = await apiAction('peripherals', 'delete_template', {
                            peripheral_type: serialType,
                            template_code: templateCode,
                            data: displayCode
                        });
                    if (response == null) {
                        throw new Error('后端删除失败');
                    }
                    item.remove();
                    addToCommandLog('删除模板', 'success', `模板已删除: ${displayCode || templateCode}`);
                    // 刷新模板列表
                    await loadSerialTemplates();
                } catch (error) {
                    addToCommandLog('删除失败', 'error', error.message);
                }
            });
        }

        // 重命名按钮
        const renameBtn = item.querySelector('.template-rename-btn');
        if (renameBtn) {
            renameBtn.addEventListener('click', async (e) => {
                e.stopPropagation();
                const templateCode = item.dataset.templateCode || '';
                const currentName = item.dataset.templateName || '';
                const displayCode = (item.dataset.templateHex || '').trim();
                const serialType = item.dataset.templateType || getActiveSerialTemplateType();
                const newNameRaw = await showPrompt(
                    `修改墙板名称（${displayCode || templateCode}）`,
                    '重命名模板',
                    currentName || ''
                );
                if (newNameRaw === null) return;
                const newName = (newNameRaw || '').trim();
                if (!newName) {
                    addToCommandLog('重命名失败', 'warning', '名称不能为空');
                    return;
                }

                try {
                    await apiAction('peripherals', 'rename_template', {
                            peripheral_type: serialType,
                            template_code: templateCode,
                            name: newName
                        });
                    addToCommandLog('重命名模板', 'success', `已更新名称: ${newName}`);
                    await loadSerialTemplates();
                } catch (error) {
                    addToCommandLog('重命名失败', 'error', error.message);
                }
            });
        }
    });
}

/**
 * @brief 加载模板到工作区
 */
async function loadTemplateTo工作区(templateItem) {
    const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');
    if (!workspaceCanvas) return;

    // 清空工作区
    workspaceCanvas.innerHTML = '';

    const templateCode = templateItem.dataset.templateCode || '';
    const serialType = templateItem.dataset.templateType || getActiveSerialTemplateType();
    if (!templateCode) {
        const empty = document.createElement('div');
        empty.className = 'workspace-empty-state';
        empty.textContent = '从右侧选择功能，点击即可添加到此处';
        workspaceCanvas.appendChild(empty);
        return;
    }

    // 显示保存按钮
    const saveBtn = document.getElementById('save-template-functions-btn');
    if (saveBtn) {
        saveBtn.style.display = 'inline-block';
    }

    // 从数据库加载已保存的功能
    try {
        const response = await apiAction('peripherals', 'get_template_functions', {
                peripheral_type: serialType,
                template_code: templateCode
            });

        const functions = response && Array.isArray(response.functions)
            ? response.functions
            : [];
        const hasResponse = response && typeof response === 'object';

        if (hasResponse) {

            if (functions.length > 0) {
                functions.forEach(func => {
                    const chip = document.createElement('div');
                    chip.className = 'function-chip';
                    chip.dataset.functionId = func.function_id || '';
                    chip.dataset.action = func.action || '';
                    chip.dataset.functionCode = func.function_code || '';
                    if (isForwardFunctionLike(func)) {
                        chip.dataset.functionId = FORWARD_FUNCTION_ID;
                        chip.dataset.action = 'forward_payload';
                    }

                    // 如果是图层模板，恢复场景名称或默认标记
                    if (func.isTemplate) {
                        chip.dataset.isTemplate = 'true';
                        if (func.isDefault) {
                            chip.dataset.isDefault = 'true';
                        } else if (func.scene_name) {
                            chip.dataset.sceneName = func.scene_name;
                        }
                    }

                    // 如果是播放列表，恢复播放列表参数
                    if (func.isPlaylist && func.playlistId) {
                        chip.dataset.isPlaylist = 'true';
                        chip.dataset.playlistId = func.playlistId;
                        chip.dataset.playlistName = func.playlistName || '';
                        chip.dataset.targetLayerId = String(func.targetLayerId || func.layerId || 1);
                        chip.dataset.layerId = String(func.layerId || func.targetLayerId || 1);
                    }

                    chip.innerHTML = `
                        <div class="function-chip-main">
                            <span>${func.function_name || ''}</span>
                            ${isForwardFunctionLike(func) ? '<small class="function-chip-subtitle"></small>' : ''}
                        </div>
                        <div class="chip-actions">
                            <button class="move-btn move-up" title="上移">▲</button>
                            <button class="move-btn move-down" title="下移">▼</button>
                            <button class="close-btn" title="删除">×</button>
                        </div>
                    `;
                    if (isForwardFunctionLike(func)) {
                        applyForwardConfigToChip(chip, func);
                    }
                    workspaceCanvas.appendChild(chip);

                    // 绑定删除按钮
                    chip.querySelector('.close-btn')?.addEventListener('click', () => {
                        chip.remove();
                        const chips = workspaceCanvas.querySelectorAll('.function-chip');
                        if (chips.length === 0) {
                            const empty = document.createElement('div');
                            empty.className = 'workspace-empty-state';
                            empty.textContent = '从右侧选择功能，点击即可添加到此处';
                            workspaceCanvas.appendChild(empty);
                        }
                    });

                    // 绑定上移按钮
                    chip.querySelector('.move-up')?.addEventListener('click', (e) => {
                        e.stopPropagation();
                        moveFunctionChip(chip, 'up');
                    });

                    // 绑定下移按钮
                    chip.querySelector('.move-down')?.addEventListener('click', (e) => {
                        e.stopPropagation();
                        moveFunctionChip(chip, 'down');
                    });
                    if (isForwardChip(chip)) {
                        chip.addEventListener('dblclick', async () => {
                            const nextConfig = await showForwardConfigDialog({
                                ...getForwardConfigFromChip(chip),
                                default_source_hex: templateItem.dataset.templateHex || templateItem.dataset.templateCode || ''
                            });
                            if (nextConfig) applyForwardConfigToChip(chip, nextConfig);
                        });
                    }
                });
            } else {
                const empty = document.createElement('div');
                empty.className = 'workspace-empty-state';
                empty.textContent = '从右侧选择功能，点击即可添加到此处';
                workspaceCanvas.appendChild(empty);
            }
        } else {
            const empty = document.createElement('div');
            empty.className = 'workspace-empty-state';
            empty.textContent = '从右侧选择功能，点击即可添加到此处';
            workspaceCanvas.appendChild(empty);
        }
    } catch (error) {
        console.error('Failed to load template functions:', error);
        const empty = document.createElement('div');
        empty.className = 'workspace-empty-state';
        empty.textContent = '从右侧选择功能，点击即可添加到此处';
        workspaceCanvas.appendChild(empty);
    }
}

/**
 * @brief 初始化功能列表按钮事件
 */
function initSerialFunctionButtons() {
    const functionButtons = document.querySelectorAll('#config-serial .func-item-btn');

    functionButtons.forEach(btn => {
        btn.addEventListener('click', async () => {
            const functionId = btn.dataset.functionId || '';
            const action = btn.dataset.action || '';
            const code = btn.dataset.code || '';
            const functionName = btn.textContent.trim();

            // 优先尝试绑定到命令映射
            const cmdList = document.getElementById('rs232-command-list');
            if (cmdList && functionId && action) {
                const rows = cmdList.querySelectorAll('.command-map-row');

                // 查找第一个Hex为空或者最后一个行
                let targetRow = null;
                for (let i = 0; i < rows.length; i++) {
                    const hexInput = rows[i].querySelector('.serial-hex-input');
                    if (hexInput && !hexInput.value.trim()) {
                        targetRow = rows[i];
                        break;
                    }
                }

                if (!targetRow && rows.length > 0) {
                    targetRow = rows[rows.length - 1];
                }

                if (targetRow) {
                    const functionSelect = targetRow.querySelector('.serial-function-select');
                    if (functionSelect) {
                        const isTemplate = btn.dataset.isTemplate === 'true';
                        const isDefault = btn.dataset.isDefault === 'true';
                        const sceneName = btn.dataset.sceneName || '';
                        const isPlaylist = btn.dataset.isPlaylist === 'true';
                        const playlistId = btn.dataset.playlistId || '';
                        const playlistName = btn.dataset.playlistName || '';
                        const targetLayerId = btn.dataset.targetLayerId || '1';

                        const functionData = {
                            id: functionId,
                            action: action,
                            code: parseInt(code),
                            name: functionName
                        };

                        // 如果是图层模板，添加场景名称或标记为默认
                        if (isTemplate) {
                            if (isDefault) {
                                // 默认模板使用 load_default action
                                functionData.action = 'load_default';
                                functionData.isDefault = true;
                            } else if (sceneName) {
                                // 普通场景模板使用 switch_scene action
                                functionData.scene_name = sceneName;
                            }
                            functionData.isTemplate = true;
                        }

                        // 如果是播放列表，添加播放列表参数
                        if (isPlaylist && playlistId) {
                            functionData.playlistId = playlistId;
                            functionData.playlistName = playlistName;
                            functionData.layerId = parseInt(targetLayerId);
                            functionData.targetLayerId = parseInt(targetLayerId);
                            functionData.isPlaylist = true;
                        }

                        const functionValue = JSON.stringify(functionData);
                        functionSelect.value = functionValue.replace(/"/g, '&quot;');
                        addToCommandLog('绑定功能', 'success', `已绑定功能: ${functionName}`);
                        return;
                    }
                }
            }

            // 如果没有命令映射行，则添加到工作区（保持原有行为）
            const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');

            // 检查是否已存在该功能
            const existingChips = Array.from(workspaceCanvas?.querySelectorAll('.function-chip') || []);
            const alreadyExists = functionId !== FORWARD_FUNCTION_ID &&
                existingChips.some(chip => chip.textContent.includes(functionName));

            if (alreadyExists) {
                addToCommandLog('功能已存在', 'warning', `${functionName} 已在工作区中`);
                return;
            }

            // 添加功能芯片到工作区
            if (workspaceCanvas) {
                const chip = document.createElement('div');
                chip.className = 'function-chip';
                // 存储功能数据以便保存
                chip.dataset.functionId = functionId;
                chip.dataset.action = action;
                chip.dataset.functionCode = code;

                // 如果是图层模板，保存场景名称或默认标记
                const isTemplate = btn.dataset.isTemplate === 'true';
                if (isTemplate) {
                    const isDefault = btn.dataset.isDefault === 'true';
                    const sceneName = btn.dataset.sceneName || '';
                    chip.dataset.isTemplate = 'true';
                    if (isDefault) {
                        chip.dataset.isDefault = 'true';
                    } else if (sceneName) {
                        chip.dataset.sceneName = sceneName;
                    }
                }

                // 如果是播放列表，保存播放列表参数
                const isPlaylist = btn.dataset.isPlaylist === 'true';
                if (isPlaylist) {
                    chip.dataset.isPlaylist = 'true';
                    chip.dataset.playlistId = btn.dataset.playlistId || '';
                    chip.dataset.playlistName = btn.dataset.playlistName || '';
                    chip.dataset.targetLayerId = btn.dataset.targetLayerId || '1';
                    chip.dataset.layerId = btn.dataset.targetLayerId || '1';
                }

                chip.innerHTML = `
                    <div class="function-chip-main">
                        <span>${functionName}</span>
                        ${functionId === FORWARD_FUNCTION_ID ? '<small class="function-chip-subtitle"></small>' : ''}
                    </div>
                    <div class="chip-actions">
                        <button class="move-btn move-up" title="上移">▲</button>
                        <button class="move-btn move-down" title="下移">▼</button>
                        <button class="close-btn" title="删除">×</button>
                    </div>
                `;

                // 移除空状态提示
                const emptyState = workspaceCanvas.querySelector('.workspace-empty-state');
                if (emptyState) {
                    emptyState.remove();
                }

                workspaceCanvas.appendChild(chip);

                // 绑定删除按钮
                chip.querySelector('.close-btn')?.addEventListener('click', () => {
                    chip.remove();
                    // 如果没有功能了，显示空状态
                    const chips = workspaceCanvas.querySelectorAll('.function-chip');
                    if (chips.length === 0) {
                        const empty = document.createElement('div');
                        empty.className = 'workspace-empty-state';
                        empty.textContent = '从右侧选择功能，点击即可添加到此处';
                        workspaceCanvas.appendChild(empty);
                    }
                });

                // 绑定上移按钮
                chip.querySelector('.move-up')?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    moveFunctionChip(chip, 'up');
                });

                // 绑定下移按钮
                chip.querySelector('.move-down')?.addEventListener('click', (e) => {
                    e.stopPropagation();
                    moveFunctionChip(chip, 'down');
                });

                if (functionId === FORWARD_FUNCTION_ID) {
                    const forwardConfig = await showForwardConfigDialog({
                        default_source_hex: getActiveSerialTemplateHex()
                    });
                    if (!forwardConfig) {
                        chip.remove();
                        if (!workspaceCanvas.querySelector('.function-chip')) {
                            const empty = document.createElement('div');
                            empty.className = 'workspace-empty-state';
                            empty.textContent = '从右侧选择功能，点击即可添加到此处';
                            workspaceCanvas.appendChild(empty);
                        }
                        return;
                    }
                    applyForwardConfigToChip(chip, forwardConfig);
                    chip.addEventListener('dblclick', async () => {
                        const nextConfig = await showForwardConfigDialog({
                            ...getForwardConfigFromChip(chip),
                            default_source_hex: getActiveSerialTemplateHex()
                        });
                        if (nextConfig) applyForwardConfigToChip(chip, nextConfig);
                    });
                    addToCommandLog('添加功能', 'success', `已添加 ${functionName}`);
                } else {
                    addToCommandLog('添加功能', 'success', `已添加 ${functionName}`);
                }
            }
        });
    });
}

/**
 * @brief 保存模板功能配置
 */
async function saveTemplateFunctions() {
    // 获取当前选中的模板
    const activeTemplate = document.querySelector('#config-serial .template-item.bg-active');
    if (!activeTemplate) {
        addToCommandLog('保存功能配置', 'warning', '请先选择一个模板');
        return;
    }

    const templateCode = activeTemplate.dataset.templateCode || '';
    const serialType = activeTemplate.dataset.templateType || getActiveSerialTemplateType();
    if (!templateCode) {
        addToCommandLog('保存功能配置', 'error', '模板编码不存在');
        return;
    }

    // 收集工作区中的所有功能芯片
    const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');
    if (!workspaceCanvas) {
        addToCommandLog('保存功能配置', 'error', '工作区不存在');
        return;
    }

    const chips = workspaceCanvas.querySelectorAll('.function-chip');
    const functions = [];

    // 检查重复功能ID
    const seenFunctionIds = new Set();

    chips.forEach(chip => {
        const functionId = chip.dataset.functionId || '';
        const functionName = chip.querySelector('span')?.textContent.trim() || '';
        let action = chip.dataset.action || '';
        let functionCode = parseInt(chip.dataset.functionCode || '0', 10);
        const isTemplate = chip.dataset.isTemplate === 'true';
        const isDefault = chip.dataset.isDefault === 'true';
        const sceneName = chip.dataset.sceneName || '';
        const isPlaylist = chip.dataset.isPlaylist === 'true';
        const playlistId = chip.dataset.playlistId || '';
        const playlistName = chip.dataset.playlistName || '';
        const targetLayerId = chip.dataset.targetLayerId || '1';

        if (!functionId || !functionName) {
            return;
        }

        if (isPlaylist) {
            action = 'play';
            functionCode = 0x02;
        }

        // 防重复检查
        const identityKey = functionId === FORWARD_FUNCTION_ID
            ? functionIdentityFromChip(chip)
            : functionId;
        if (seenFunctionIds.has(identityKey)) {
            addToCommandLog('保存功能配置', 'warning', `功能 "${functionName}" 重复，已跳过`);
            return;
        }
        seenFunctionIds.add(identityKey);

        const functionData = {
            function_id: functionId,
            function_name: functionName,
            action: action,
            function_code: functionCode
        };

        // 如果是图层模板，添加场景名称参数或标记为默认
        if (isTemplate) {
            if (isDefault) {
                // 默认模板使用 load_default action
                functionData.action = 'load_default';
                functionData.isDefault = true;
            } else if (sceneName) {
                // 普通场景模板使用 switch_scene action
                functionData.scene_name = sceneName;
            }
            functionData.isTemplate = true;
        }

        // 如果是播放列表，添加播放列表参数
        if (isPlaylist && playlistId) {
            functionData.playlistId = playlistId;
            functionData.playlistName = playlistName;
            functionData.layerId = parseInt(targetLayerId);
            functionData.targetLayerId = parseInt(targetLayerId);
            functionData.isPlaylist = true;
        }

        if (functionId === FORWARD_FUNCTION_ID) {
            const forwardConfig = getForwardConfigFromChip(chip);
            Object.assign(functionData, forwardConfig);
        }

        functions.push(functionData);
    });

    try {
        const response = await apiAction('peripherals', 'save_template_functions', {
                peripheral_type: serialType,
                template_code: templateCode,
                functions: functions
            });

        if (response != null) {
            const count = response.count ?? functions.length;
            addToCommandLog('保存功能配置', 'success', `已保存 ${count} 个功能配置`);
        } else {
            throw new Error(response?.message || '保存失败');
        }
    } catch (error) {
        addToCommandLog('保存功能配置', 'error', error.message || '保存失败');
    }
}

/**
 * @brief 移动功能芯片位置
 * @param {HTMLElement} chip - 要移动的功能芯片元素
 * @param {string} direction - 移动方向：'up' 或 'down'
 */
function moveFunctionChip(chip, direction) {
    const workspaceCanvas = chip.closest('.workspace-canvas');
    if (!workspaceCanvas) return;

    const chips = Array.from(workspaceCanvas.querySelectorAll('.function-chip'));
    const currentIndex = chips.indexOf(chip);

    if (currentIndex === -1) return;

    let targetIndex;
    if (direction === 'up') {
        // 上移：与上一个元素交换位置
        if (currentIndex === 0) {
            addToCommandLog('位置调整', 'warning', '已经是第一个，无法上移');
            return;
        }
        targetIndex = currentIndex - 1;
    } else if (direction === 'down') {
        // 下移：与下一个元素交换位置
        if (currentIndex === chips.length - 1) {
            addToCommandLog('位置调整', 'warning', '已经是最后一个，无法下移');
            return;
        }
        targetIndex = currentIndex + 1;
    } else {
        return;
    }

    // 交换位置
    const targetChip = chips[targetIndex];
    if (direction === 'up') {
        workspaceCanvas.insertBefore(chip, targetChip);
    } else {
        workspaceCanvas.insertBefore(chip, targetChip.nextSibling);
    }

    addToCommandLog('位置调整', 'success', `已${direction === 'up' ? '上移' : '下移'}`);
}

/**
 * @brief 初始化工作区功能芯片交互
 */
function initSerial工作区() {
    const workspaceCanvas = document.querySelector('#config-serial .workspace-canvas');
    if (!workspaceCanvas) return;

    // 为现有的功能芯片绑定事件
    const existingChips = workspaceCanvas.querySelectorAll('.function-chip');
    existingChips.forEach(chip => {
        // 绑定删除按钮
        const closeBtn = chip.querySelector('.close-btn');
        if (closeBtn && !closeBtn.dataset.bound) {
            closeBtn.dataset.bound = 'true';
            closeBtn.addEventListener('click', () => {
                chip.remove();
                const chips = workspaceCanvas.querySelectorAll('.function-chip');
                if (chips.length === 0) {
                    const empty = document.createElement('div');
                    empty.className = 'workspace-empty-state';
                    empty.textContent = '从右侧选择功能，点击即可添加到此处';
                    workspaceCanvas.appendChild(empty);
                }
            });
        }

        // 绑定上移按钮
        const moveUpBtn = chip.querySelector('.move-up');
        if (moveUpBtn && !moveUpBtn.dataset.bound) {
            moveUpBtn.dataset.bound = 'true';
            moveUpBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                moveFunctionChip(chip, 'up');
            });
        }

        // 绑定下移按钮
        const moveDownBtn = chip.querySelector('.move-down');
        if (moveDownBtn && !moveDownBtn.dataset.bound) {
            moveDownBtn.dataset.bound = 'true';
            moveDownBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                moveFunctionChip(chip, 'down');
            });
        }
    });
}

/**
 * @brief 初始化搜索功能
 */
function initSerialSearch() {
    // 模板列表搜索
    const templateSearch = document.querySelector('#config-serial .template-col .search-input input');
    if (templateSearch) {
        templateSearch.addEventListener('input', (e) => {
            const keyword = e.target.value.trim().toLowerCase();
            const templateItems = document.querySelectorAll('#config-serial .template-item');

            templateItems.forEach(item => {
                const code = item.querySelector('span:nth-child(1)')?.textContent.toLowerCase() || '';
                const name = item.querySelector('span:nth-child(2)')?.textContent.toLowerCase() || '';

                if (code.includes(keyword) || name.includes(keyword)) {
                    item.style.display = '';
                } else {
                    item.style.display = 'none';
                }
            });
        });
    }

    // 功能列表搜索和图层过滤
    const functionSearch = document.querySelector('#config-serial .functions-col .simple-search-bar input');
    if (functionSearch) {
        functionSearch.addEventListener('input', () => {
            filterFunctions();
        });
    }

    // 图层过滤按钮
    const layerFilterBtns = document.querySelectorAll('#config-serial .layer-filter-btn');
    layerFilterBtns.forEach(btn => {
        btn.addEventListener('click', () => {
            // 更新按钮选中状态
            layerFilterBtns.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            // 执行过滤
            filterFunctions();
        });
    });
}

/**
 * @brief 判断功能按钮是否匹配图层过滤
 * 没有 layerId 的按钮是全局功能，选中任意图层时都应显示。
 */
function matchesFunctionLayer(btn, selectedLayer) {
    if (selectedLayer === 'all') return true;

    const filterLayerId = btn.dataset.layerId ||
        (btn.dataset.isPlaylist === 'true' ? btn.dataset.targetLayerId : '');

    return !filterLayerId || filterLayerId === selectedLayer;
}

/**
 * @brief 统一的功能过滤函数（支持搜索关键词和图层过滤）
 */
function filterFunctions() {
    const functionSearch = document.querySelector('#config-serial .functions-col .simple-search-bar input');
    const activeLayerBtn = document.querySelector('#config-serial .layer-filter-btn.active');
    const functionButtons = document.querySelectorAll('#config-serial .func-item-btn');

    const keyword = functionSearch ? functionSearch.value.trim().toLowerCase() : '';
    const selectedLayer = activeLayerBtn ? activeLayerBtn.dataset.layer : 'all';

    // 收集所有分类容器
    const categoryContainers = new Set();

    functionButtons.forEach(btn => {
        const text = btn.textContent.toLowerCase();

        // 检查搜索关键词匹配
        const matchesKeyword = !keyword || text.includes(keyword);

        // 检查图层匹配
        const matchesLayer = matchesFunctionLayer(btn, selectedLayer);

        // 同时满足搜索和图层过滤条件才显示
        if (matchesKeyword && matchesLayer) {
            btn.style.display = '';
        } else {
            btn.style.display = 'none';
        }

        // 收集分类容器
        const categoryDiv = btn.closest('.function-category');
        if (categoryDiv) {
            categoryContainers.add(categoryDiv);
        }
    });

    // 统一处理分类容器的显示/隐藏
    categoryContainers.forEach(categoryDiv => {
        const visibleButtons = Array.from(categoryDiv.querySelectorAll('.func-item-btn')).filter(b => b.style.display !== 'none');
        if (visibleButtons.length > 0) {
            categoryDiv.style.display = '';
        } else {
            categoryDiv.style.display = 'none';
        }
    });
}

/**
 * @brief 检查学习模式状态并启动自动刷新
 * 注意：这个函数现在由loadRs232Config调用，不需要单独调用
 */
async function checkLearnModeAndStartRefresh() {
    // 已移至loadRs232Config中处理
}

/**
 * @brief 初始化时立即加载 DMX 状态，完全依赖后端状态确保唯一性和准确性
 */
async function loadDmxStatusOnInit() {
    try {
        const response = await apiGet('/dmx/status');
        if (response && typeof response.simulationMode !== 'undefined') {
            // 完全依赖后端状态，确保状态的唯一性和准确性
            updateDmxModeUI(response.simulationMode);
            const modeEl = document.getElementById('dmx-input-mode');
            if (modeEl && response.mode) {
                modeEl.value = response.mode === 'external' ? 'external' : 'local';
                const cfg = response.config || {};
                const externalPortEl = document.getElementById('dmx-external-port');
                const infoShowEl = document.getElementById('dmx-info-show');
                if (externalPortEl && cfg.external_port) externalPortEl.value = cfg.external_port;
                if (infoShowEl && typeof cfg.dmx_info_show !== 'undefined') {
                    infoShowEl.checked = !!cfg.dmx_info_show;
                }
                updateDmxInputModeUI();
            }
        } else {
            // 如果后端没有返回状态，使用后端默认值（true，模拟模式）
            updateDmxModeUI(true);
        }
    } catch (e) {
        // 如果加载失败，使用后端默认值（true，模拟模式）
        updateDmxModeUI(true);
    }
}

/**
 * @brief 初始化 DMX 实时监控
 */
function initDmxMiniMonitor() {
    const grid = document.getElementById('dmx-mini-monitor-grid');
    if (!grid) return;

    // 默认生成 12 个展示单元 (对应通道 1-12)
    grid.innerHTML = '';
    for (let i = 0; i < 12; i++) {
        const cell = document.createElement('div');
        cell.className = 'dmx-mini-cell';
        cell.id = `dmx-mini-ch-${i}`;
        cell.innerHTML = `
            <span class="dmx-mini-ch-num">${i + 1}</span>
            <span class="dmx-mini-val">0</span>
        `;
        grid.appendChild(cell);
    }

    // 启动轮询
    const pollInterval = setInterval(async () => {
        // 检查页面是否还活跃
        const page = document.getElementById('peripheral-management-page');
        if (!page || !page.classList.contains('active')) return;

        try {
            const response = await apiGet('/dmx/channels');
            if (response && response.channels) {
                const activeChannels = response.channels.slice(0, 12);
                updateDmxMiniMonitor(activeChannels);

                // 同步模拟模式状态，完全依赖后端状态确保唯一性和准确性
                if (typeof response.simulationMode !== 'undefined') {
                    updateDmxModeUI(response.simulationMode);
                }

                // 同步推杆状态 (双向绑定)
                const dmxContainer = document.getElementById('dmx-mapping-container');
                if (dmxContainer) {
                    const cards = Array.from(dmxContainer.children);
                    cards.forEach((card, idx) => {
                        const holdActive = card && card._dmxManualHoldUntil &&
                            Date.now() < card._dmxManualHoldUntil;
                        if (card && !card._isInteracting && !holdActive &&
                            typeof card.updateValue === 'function') {
                            const serverVal = activeChannels[idx] || 0;
                            // 在模拟模式下，如果不处于交互状态，也应该更新 slider 显示值（后端可能被其他客户端修改）
                            // 但为防止冲突，可以只在非交互时更新
                            card.updateValue(serverVal, 'auto');
                        }
                    });
                }
            }
        } catch (e) {
            // 说明：忽略 fetch 错误
        }
    }, 200);

    // 页面销毁时清理 (可选，全局单例页面通常不清理)
}

/**
 * @brief 更新 DMX 实时监控数据显示
 */
function updateDmxMiniMonitor(channels) {
    for (let i = 0; i < 12; i++) {
        const val = channels[i] || 0;
        const cell = document.getElementById(`dmx-mini-ch-${i}`);
        if (!cell) continue;

        const valEl = cell.querySelector('.dmx-mini-val');
        if (valEl) valEl.textContent = val;

        // 统一使用 HSL 动态色彩逻辑，与监控页面保持一致
        if (val === 0) {
            cell.style.background = ''; // Keep CSS 默认 (dark semi-transparent)
            cell.style.borderColor = '';
            if (valEl) valEl.style.color = '';
        } else {
            const intensity = val / 255;
            const hue = 120 - (intensity * 120); // 示例/字段：绿色（120）到红色（0）
            const saturation = 70 + (intensity * 20); // 70-90%
            const lightness = 40 + (intensity * 20); // 40-60%

            cell.style.background = `hsl(${hue}, ${saturation}%, ${lightness}%)`;
            cell.style.borderColor = `hsl(${hue}, ${saturation}%, ${lightness - 10}%)`;
            if (valEl) valEl.style.color = intensity > 0.5 ? '#000' : '#fff';
        }
    }
}

/**
 * @brief 加载所有外设配置
 */
async function loadPeripheralConfigs() {
    await loadSerialPortOptions();

    const types = ['dmx', 'rs232', 'rs485', 'udp', 'tcp', 'function_config'];

    // 创建加载任务列表
    const tasks = types.map(type => loadConfigForType(type));

    try {
        await Promise.all(tasks);

        // 加载完成后，根据需要加载额外数据
        await loadSerialTemplates();
        await loadSerialFunctions();
        await Promise.all([
            loadNetworkFunctions('udp'),
            loadNetworkFunctions('tcp'),
            loadNetworkTemplates('udp'),
            loadNetworkTemplates('tcp'),
        ]);

        addToCommandLog('同步配置', 'success', '所有外设配置已完成同步');
    } catch (error) {
        console.error('Failed to load peripheral configs:', error);
    }
}

async function loadSerialPortOptions() {
    try {
        const response = await apiAction('peripherals', 'list_serial_ports', {});
        const rawPorts = Array.isArray(response?.ports) ? response.ports : [];

        // 统一转换为对象格式 {value, label}，并隐藏调试串口。
        const ports = rawPorts.map(port => {
            if (typeof port === 'string') {
                return { value: port, label: port };
            } else if (typeof port === 'object' && port !== null) {
                const value = port.value || '';
                const rawLabel = port.label || value;
                return {
                    value,
                    label: rawLabel.includes(value) ? rawLabel : `${rawLabel} (${value})`,
                    type: port.type || port.role || ''
                };
            }
            return { value: '', label: '' };
        }).filter(p => p.value !== '' && p.type !== 'debug' && !p.value.includes('ttyFIQ'));

        const rs485Ports = ports.filter(isRs485Port);
        const rs232Ports = ports.filter(port => !isRs485Port(port));
        serialPortOptions = ports;
        rs232SerialPortOptions = rs232Ports;
        rs485SerialPortOptions = rs485Ports;
        updateSerialPortSelect('dmx-port', rs485Ports, [
            { value: 'artnet', label: 'Art-Net (Network)' },
            { value: 'sacn', label: 'sACN (Network)' }
        ], true);
        updateSerialPortSelect('dmx-external-port', rs232Ports, [
            { value: '/dev/ttySWK4', label: 'RS232 (/dev/ttySWK4)' }
        ], true);
        updateSerialPortSelect('serial-port-select', ports, []);
        updateSerialPortSelect('rs485-port', rs485Ports, [], true);
        const forwardDialog = document.getElementById('forward-config-dialog');
        if (forwardDialog && forwardDialog.classList.contains('active')) {
            refreshForwardSerialPortOptions(forwardDialog);
        }
    } catch (error) {
        console.warn('Failed to load serial ports:', error);
    }
}

function isRs485Port(port) {
    // 兼容字符串和对象格式 {value, label}
    if (typeof port === 'object' && port !== null && port.type) {
        return port.type === 'rs485';
    }
    const portValue = (typeof port === 'object' && port !== null) ? port.value : port;
    const name = String(portValue || '').split('/').pop() || '';
    if (name === 'ttySWK4' || name === 'ttysWK4') return false;
    return name.startsWith('ttyRS') || name.startsWith('ttyUSB') ||
        name.startsWith('ttyXRUSB') || name.startsWith('ttySWK') || name.startsWith('ttysWK');
}

function getActiveSerialConfig() {
    const activeType = document.querySelector('.panel-tab[data-serial-type].active')?.getAttribute('data-serial-type') || 'rs232';
    let type = activeType === 'rs485' ? 'rs485' : 'rs232';
    const portSelect = document.getElementById('serial-port-select');
    const port = portSelect?.value || '';
    const selectedOptionType = portSelect?.selectedOptions?.[0]?.dataset?.type || '';
    if (selectedOptionType === 'rs485' || selectedOptionType === 'rs232') {
        type = selectedOptionType;
    } else if (isRs485Port(port)) {
        type = 'rs485';
    }
    const baudrate = parseInt(document.getElementById('serial-baud-select')?.value ||
        '9600', 10) || 9600;

    return { type, port, baudrate };
}

function applySerialLearnModeUI(config) {
    const learnModeEl = document.getElementById('serial-learn-mode');
    if (learnModeEl && typeof config.learn_mode !== 'undefined') {
        learnModeEl.checked = config.learn_mode;
        if (config.learn_mode) startAutoRefreshTemplates();
        else stopAutoRefreshTemplates();
    }
}

function updateSerialPortSelect(selectId, ports, extraOptions, strict = false) {
    const select = document.getElementById(selectId);
    if (!select) return;
    const currentValue = select.value;
    select.innerHTML = '';
    ports.forEach(port => {
        const option = document.createElement('option');
        // 兼容字符串和对象格式 {value, label}
        if (typeof port === 'object' && port !== null) {
            option.value = port.value;
            option.textContent = port.label;
            option.dataset.type = port.type || '';
        } else {
            option.value = port;
            option.textContent = port;
        }
        select.appendChild(option);
    });
    extraOptions.forEach(item => {
        const option = document.createElement('option');
        option.value = item.value;
        option.textContent = item.label;
        select.appendChild(option);
    });
    if (currentValue && (!strict || ports.some(p => (typeof p === 'object' ? p.value : p) === currentValue) || extraOptions.some(item => item.value === currentValue))) {
        if (!Array.from(select.options).some(option => option.value === currentValue)) {
            const option = document.createElement('option');
            option.value = currentValue;
            option.textContent = currentValue;
            select.insertBefore(option, select.firstChild);
        }
        select.value = currentValue;
    }
}

function getPortOptionValue(port) {
    return typeof port === 'object' && port !== null ? port.value : port;
}

function appendSerialPortOption(select, port) {
    const option = document.createElement('option');
    if (typeof port === 'object' && port !== null) {
        option.value = port.value;
        option.textContent = port.label || port.value;
        option.dataset.type = port.type || '';
    } else {
        option.value = port;
        option.textContent = port;
    }
    select.appendChild(option);
}

function getForwardSerialPortOptions(target) {
    if (target === 'rs485') {
        return rs485SerialPortOptions.length ? rs485SerialPortOptions : serialPortOptions.filter(isRs485Port);
    }
    if (target === 'rs232') {
        return rs232SerialPortOptions.length ? rs232SerialPortOptions : serialPortOptions.filter(port => !isRs485Port(port));
    }
    return [];
}

function refreshForwardSerialPortOptions(dialog, preferredValue = undefined) {
    const select = dialog?.querySelector('#forward-port-input');
    if (!select) return;

    const target = dialog.querySelector('#forward-target-select')?.value || 'rs232';
    const ports = getForwardSerialPortOptions(target);
    const desiredValue = preferredValue !== undefined ? preferredValue : select.value;
    const hasDesiredValue = !!desiredValue;
    const desiredInList = ports.some(port => getPortOptionValue(port) === desiredValue);
    const shouldKeepUnlistedValue = preferredValue !== undefined && hasDesiredValue && !desiredInList;

    select.innerHTML = '';
    if (!ports.length && !shouldKeepUnlistedValue) {
        const option = document.createElement('option');
        option.value = '';
        option.textContent = `未检测到${target.toUpperCase()}可用串口`;
        select.appendChild(option);
        select.value = '';
        return;
    }

    ports.forEach(port => appendSerialPortOption(select, port));
    if (shouldKeepUnlistedValue) {
        const option = document.createElement('option');
        option.value = desiredValue;
        option.textContent = `${desiredValue} (当前配置)`;
        select.insertBefore(option, select.firstChild);
    }

    if (desiredInList || shouldKeepUnlistedValue) {
        select.value = desiredValue;
    } else if (select.options.length > 0) {
        select.selectedIndex = 0;
    }
}
/**
 * @brief 为特定类型加载配置并更新 UI
 * @param {string} type 外设类型
 */
async function loadConfigForType(type) {
    // 映射前端 ID 到后端类型名 (后端通用 0x50 命令处理)
    const backendType = type === 'dmx' ? 'dmx512' : type;

    try {
        const response = await apiAction('peripherals', 'get_config', {
                peripheral_type: backendType
            });

        if (!response || typeof response !== 'object') return;

        const config = response;

        // 更新通用字段 (端口, 波特率)
        const usesSharedSerialControls = type === 'rs232' || type === 'rs485';
        const portId = usesSharedSerialControls ? 'serial-port-select' : `${type}-port`;
        const baudId = usesSharedSerialControls ? 'serial-baud-select' : `${type}-baudrate`;

        const portEl = document.getElementById(portId);
        const baudEl = document.getElementById(baudId);

        const shouldApplySharedSerialConfig = !usesSharedSerialControls ||
            type === 'rs485' || !isRs485Port(config.port);
        if (shouldApplySharedSerialConfig && portEl && config.port) portEl.value = config.port;
        if (shouldApplySharedSerialConfig && baudEl && config.baudrate) baudEl.value = config.baudrate.toString();

        // 为特定协议处理特有字段
        switch (type) {
            case 'dmx':
                const modeEl = document.getElementById('dmx-input-mode');
                const externalPortEl = document.getElementById('dmx-external-port');
                const addressEl = document.getElementById('dmx-start-address');
                const infoShowEl = document.getElementById('dmx-info-show');
                if (modeEl) modeEl.value = config.mode === 'external' ? 'external' : 'local';
                if (externalPortEl && config.external_port) externalPortEl.value = config.external_port;
                if (addressEl && config.address) addressEl.value = config.address;
                if (infoShowEl && typeof config.dmx_info_show !== 'undefined') {
                    infoShowEl.checked = !!config.dmx_info_show;
                }
                updateDmxInputModeUI();
                break;
            case 'rs232':
                if (shouldApplySharedSerialConfig) applySerialLearnModeUI(config);
                // 串口已改为模板驱动（save_template_functions），mappings 字段废弃
                break;
            case 'rs485':
                applySerialLearnModeUI(config);
                const slaveIdEl = document.getElementById('rs485-slave-id');
                if (slaveIdEl && config.slave_id) slaveIdEl.value = config.slave_id;
                break;
            case 'udp':
            case 'tcp':
                updateNetworkConfigUI(type, config);
                // 反序列化墙板状态：把 mappings 解析为本地触发列表
                deserializeNetworkTriggers(type, config.mappings);
                renderNetworkTriggerList(type);
                break;
            case 'function_config':
                functionConfigState = normalizeFunctionConfig(config);
                renderFunctionConfigEditor(functionConfigState);
                break;
        }
    } catch (error) {
        console.error(`Failed to load ${type} config:`, error);
    }
}

/**
 * @brief 更新网络协议相关的 UI (UDP/TCP/WS)
 */
function updateNetworkConfigUI(type, config) {
    const bindAddr = document.getElementById(`${type}-bind-address`);
    const bindPort = document.getElementById(`${type}-bind-port`);
    const targetAddr = document.getElementById(`${type}-target-address`);
    const targetPort = document.getElementById(`${type}-target-port`);
    const pathEl = document.getElementById(`${type}-path`);
    const modeEl = document.getElementById(`${type}-mode`);
    const multicastEl = document.getElementById(`${type}-multicast-group`);

    if (bindAddr && config.bind_address) bindAddr.value = config.bind_address;
    if (bindPort && config.bind_port) bindPort.value = config.bind_port;
    if (targetAddr && config.target_address) targetAddr.value = config.target_address;
    if (targetPort && config.target_port) targetPort.value = config.target_port;
    if (pathEl && config.path) pathEl.value = config.path;
    if (modeEl && config.mode) modeEl.value = config.mode;
    if (multicastEl !== null) multicastEl.value = config.multicast_group || '';
}
/**
 * @brief 自动刷新模板列表的定时器
 */
let templateRefreshTimer = null;

/**
 * @brief 启动自动刷新模板列表
 */
function startAutoRefreshTemplates() {
    // 如果已经有定时器在运行，先清除
    if (templateRefreshTimer) {
        clearInterval(templateRefreshTimer);
    }

    // 每1秒刷新一次模板列表 (提高学习模式下的响应速度)
    templateRefreshTimer = setInterval(async () => {
        await loadSerialTemplates();
    }, 1000);
}

/**
 * @brief 停止自动刷新模板列表
 */
function stopAutoRefreshTemplates() {
    if (templateRefreshTimer) {
        clearInterval(templateRefreshTimer);
        templateRefreshTimer = null;
    }
}

/**
 * @brief 从API加载串口模板列表
 */
async function loadSerialTemplates() {
    const container = document.querySelector('#config-serial .template-list-content');
    if (!container) return;

    const serialType = getActiveSerialTemplateType();

    try {
        const response = await apiAction('peripherals', 'get_templates', {
                peripheral_type: serialType
            });

        if (response && typeof response === 'object') {
            const templates = Array.isArray(response.templates) ? response.templates : [];

            syncSerialTemplateLog(templates);

            if (templates.length > 0) {
                renderSerialTemplates(templates, serialType);
            } else {
                resetSerial工作区();
                showEmptyState(container, 'clipboard', `暂无学习到的 ${serialType.toUpperCase()} 模板`);
            }

            // 如果学习模式开启，确保自动刷新正在运行
            const serialLearnModeEl = document.getElementById('serial-learn-mode');
            if (serialLearnModeEl && serialLearnModeEl.checked && !templateRefreshTimer) {
                startAutoRefreshTemplates();
            }
        } else {
            const errorMsg = response?.message || '未知错误';
            console.error('Failed to load templates:', errorMsg);
            showErrorState(container, '加载模板列表失败', errorMsg);
        }
    } catch (error) {
        console.error('Failed to load serial templates:', error);
        showErrorState(container, '加载模板列表失败', error.message || '网络错误');
    }
}

function syncSerialTemplateLog(templates) {
    const nextCodes = new Set();
    templates.forEach(tpl => {
        const code = tpl.code || tpl.template_code || tpl.data || '';
        if (!code) return;
        nextCodes.add(code);
        if (serialTemplatesInitialized && !knownSerialTemplateCodes.has(code)) {
            appendLog('RECV', `[学习] hex: ${code}`);
        }
    });
    knownSerialTemplateCodes = nextCodes;
    serialTemplatesInitialized = true;
}

/**
 * @brief 渲染串口模板列表
 */
function renderSerialTemplates(templates, serialType = getActiveSerialTemplateType()) {
    const container = document.querySelector('#config-serial .template-list-content');
    if (!container) {
        console.error('Template list container not found!');
        return;
    }

    // 获取当前选中的模板代码（刷新时保持选中状态）
    const activeTemplate = container.querySelector('.template-item.bg-active');
    const activeTemplateCode = activeTemplate?.dataset.templateCode || '';
    const shouldLoad工作区AfterRender = (activeItem) =>
        activeItem && (!activeTemplateCode || activeItem.dataset.templateCode !== activeTemplateCode);

    container.innerHTML = '';

    const preferredCode = preferredSerialTemplateCode;

    templates.forEach((template, index) => {
        const item = document.createElement('div');
        item.className = 'template-item';

        const code = template.code || template.template_code || '';
        const name = template.name || template.template_name || '未命名';
        const hexData = template.data || ''; // 获取模板的Hex数据
        const templateType = template.has_type === false ? serialType : (template.type || serialType);
        // 转换为大写字母显示
        const displayCode = hexData ? hexData.replace(/\s+/g, ' ').trim().toUpperCase() : code.toUpperCase();

        // 如果之前有选中的模板，恢复选中状态；否则默认选中第一个
        if ((preferredCode && code === preferredCode) ||
            (!preferredCode && activeTemplateCode && code === activeTemplateCode) ||
            (!preferredCode && !activeTemplateCode && index === 0)) {
            item.classList.add('bg-active');
        }

        // 设置拖拽属性
        item.draggable = true;
        item.dataset.templateCode = code;
        item.dataset.templateName = name;
        item.dataset.templateHex = hexData;
        item.dataset.templateType = templateType;

        item.innerHTML = `
            <span class="template-cell template-cell-code" title="${hexData}">${displayCode}</span>
            <span class="template-cell template-cell-name">
                <span class="template-name-text">${name}</span>
                <button type="button" class="template-rename-btn" title="修改名称">🖊</button>
            </span>
            <span class="template-cell template-cell-actions">
                <span class="template-delete-btn">删除</span>
            </span>
        `;

        container.appendChild(item);
    });

    // 如果有选中的模板，滚动到可见位置
    const activeItem = container.querySelector('.template-item.bg-active');
    if (activeItem) {
        activeItem.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    }

    // 重新绑定事件（因为DOM已更新）
    initSerialTemplateList();

    // 初始化拖拽事件
    initTemplateDragAndDrop();

    if (activeItem) {
        const workspaceHeader = document.querySelector('#config-serial .workspace-header .badge-tag');
        if (workspaceHeader) {
            workspaceHeader.textContent = activeItem.dataset.templateName || '未命名';
        }
    }

    if (shouldLoad工作区AfterRender(activeItem)) {
        loadTemplateTo工作区(activeItem);
    }

    if (preferredCode && activeItem?.dataset.templateCode === preferredCode) {
        preferredSerialTemplateCode = '';
    }
}

/**
 * @brief 初始化模板拖拽功能
 */
function initTemplateDragAndDrop() {
    const templateItems = document.querySelectorAll('#config-serial .template-item');

    templateItems.forEach(item => {
        // 拖拽开始
        item.addEventListener('dragstart', (e) => {
            e.dataTransfer.setData('text/plain', item.dataset.templateHex || '');
            e.dataTransfer.effectAllowed = 'copy';
            item.style.opacity = '0.5';
        });

        // 拖拽结束
        item.addEventListener('dragend', (e) => {
            item.style.opacity = '1';
        });
    });

    // 为所有Hex输入框添加拖拽接收功能
    const hexInputs = document.querySelectorAll('#rs232-command-list .serial-hex-input');
    hexInputs.forEach(input => {
        input.addEventListener('dragover', (e) => {
            e.preventDefault();
            e.dataTransfer.dropEffect = 'copy';
            input.style.backgroundColor = '#2a2a2a';
        });

        input.addEventListener('dragleave', (e) => {
            input.style.backgroundColor = '';
        });

        input.addEventListener('drop', (e) => {
            e.preventDefault();
            const hexData = e.dataTransfer.getData('text/plain');
            if (hexData) {
                input.value = hexData.trim();
                input.style.backgroundColor = '';
                addToCommandLog('拖拽成功', 'success', `已设置Hex: ${hexData}`);
            }
        });
    });

    // 使用事件委托处理动态添加的输入框
    const cmdList = document.getElementById('rs232-command-list');
    if (cmdList) {
        cmdList.addEventListener('dragover', (e) => {
            if (e.target.classList.contains('serial-hex-input')) {
                e.preventDefault();
                e.dataTransfer.dropEffect = 'copy';
                e.target.style.backgroundColor = '#2a2a2a';
            }
        });

        cmdList.addEventListener('dragleave', (e) => {
            if (e.target.classList.contains('serial-hex-input')) {
                e.target.style.backgroundColor = '';
            }
        });

        cmdList.addEventListener('drop', (e) => {
            if (e.target.classList.contains('serial-hex-input')) {
                e.preventDefault();
                const hexData = e.dataTransfer.getData('text/plain');
                if (hexData) {
                    e.target.value = hexData.trim();
                    e.target.style.backgroundColor = '';
                    addToCommandLog('拖拽成功', 'success', `已设置Hex: ${hexData}`);
                }
            }
        });
    }
}


/**
 * @brief 从API加载图层模板列表
 */
async function loadLayerTemplates() {
    const templates = [];

    try {
        const response = await apiGet('/scenes');
        const scenes = Array.isArray(response) ? response : [];

        // 将场景转换为功能项
        scenes.forEach(sceneName => {
            if (sceneName && typeof sceneName === 'string' &&
                !['默认配置', 'default', 'default.json', 'config.json'].includes(sceneName)) {
                templates.push({
                    id: `scene_${sceneName}`,
                    category: '图层模板',
                    code: 10, // 场景管理命令代码
                    action: 'switch_scene',
                    name: `加载模板: ${sceneName}`,
                    description: `加载图层模板: ${sceneName}`,
                    scene_name: sceneName,
                    isTemplate: true
                });
            }
        });
    } catch (error) {
        addToCommandLog('加载场景模板', 'warning', error.message || '加载失败');
    }

    return templates;
}

/**
 * @brief 从API加载播放列表
 */
async function loadPlaylists() {
    const playlists = [];

    try {
        const response = await apiGet('/playlists');
        const playlistList = Array.isArray(response) ? response : [];

        // 将播放列表转换为功能项（每个播放列表只有一个"切换播放列表"功能）
        playlistList.forEach(playlist => {
            if (playlist && playlist.id && playlist.name) {
                const playlistId = String(playlist.id);
                const playlistName = playlist.isDefault ? `★ ${playlist.name}` : playlist.name;
                const targetLayerId = playlist.targetLayerId || 1;

                playlists.push({
                    id: `playlist_${playlistId}_switch`,
                    category: '播放列表',
                    code: 0x02, // 与视频控制页 /api/video/play 保持一致
                    action: 'play',
                    name: `切换播放列表: ${playlistName}`,
                    description: `切换到播放列表: ${playlistName}`,
                    playlistId: playlistId,
                    playlistName: playlistName,
                    layerId: targetLayerId,
                    targetLayerId: targetLayerId,
                    isPlaylist: true
                });
            }
        });
    } catch (error) {
        addToCommandLog('加载播放列表', 'warning', error.message || '加载失败');
    }

    return playlists;
}

/**
 * @brief 从CommandList目录加载功能列表
 */
async function loadSerialFunctions() {
    const container = document.querySelector('#config-serial .function-buttons-grid');
    if (!container) return;

    try {
        // 从 huoshan/CommandList/playback 目录加载命令文件
        const functions = await loadCommandsFromCommandList();

        // 加载图层模板
        const templates = await loadLayerTemplates();

        // 加载播放列表
        const playlists = await loadPlaylists();

        // 合并功能列表、模板列表和播放列表
        const allFunctions = [buildForwardFunctionEntry(), ...functions, ...templates, ...playlists];

        if (allFunctions.length > 0) {
            renderSerialFunctions(allFunctions);
        } else {
            // 如果CommandList没有数据，尝试从API加载
            try {
                const response = await apiAction('peripherals', 'get_functions', {
                        peripheral_type: getActiveSerialTemplateType()
                    });

                let apiFunctions = [];
                if (response) {
                    if (response.functions && Array.isArray(response.functions)) {
                        apiFunctions = response.functions;
                    } else if (response.data && response.data.functions && Array.isArray(response.data.functions)) {
                        apiFunctions = response.data.functions;
                    } else if (response.data && Array.isArray(response.data)) {
                        apiFunctions = response.data;
                    } else if (Array.isArray(response)) {
                        apiFunctions = response;
                    }
                }

                if (apiFunctions.length > 0) {
                    renderSerialFunctions([buildForwardFunctionEntry(), ...apiFunctions]);
                } else {
                    showErrorState(container, '加载功能列表失败', '未找到可用的命令文件');
                }
            } catch (apiError) {
                console.error('API fallback failed:', apiError);
                showErrorState(container, '加载功能列表失败', apiError.message || '网络错误');
            }
        }
    } catch (error) {
        console.error('Failed to load serial functions:', error);
        showErrorState(container, '加载功能列表失败', error.message || '网络错误');
    }
}

/**
 * @brief 从CommandList目录加载命令文件并生成功能列表
 * 仅从后端获取，不使用前端默认回退
 */
async function loadCommandsFromCommandList() {
    const functions = [];

    // 批量获取所有命令列表
    try {
        const result = await apiGet('/peripherals/command-lists');
        if (result && Array.isArray(result)) {
            functions.push(...result);
        }
    } catch (error) {
        console.error('从后端获取命令列表失败:', error);
    }

    return functions;
}

/**
 * @brief 渲染功能列表
 */
function renderSerialFunctions(functions) {
    const container = document.querySelector('#config-serial .function-buttons-grid');
    if (!container) return;

    // 确保容器有正确的高度设置，以便滚动正常工作
    function updateContainerHeight() {
        const parent = container.parentElement;
        if (!parent || !parent.classList.contains('functions-col')) {
            return;
        }

        // 获取父容器的计算样式和实际高度
        const parentComputedStyle = window.getComputedStyle(parent);
        let parentHeight = parent.offsetHeight;

        // 如果offsetHeight为0，尝试使用clientHeight
        if (parentHeight === 0) {
            parentHeight = parent.clientHeight;
        }

        // 如果还是0，尝试使用getBoundingClientRect
        if (parentHeight === 0) {
            const rect = parent.getBoundingClientRect();
            parentHeight = rect.height;
        }

        // 如果父容器在flex布局中，尝试从父元素的父元素获取高度
        if (parentHeight === 0 || parentHeight < 100) {
            const grandParent = parent.parentElement;
            if (grandParent) {
                const grandParentHeight = grandParent.offsetHeight || grandParent.clientHeight;
                if (grandParentHeight > 0) {
                    // 假设三列布局，functions-col占1.5份
                    parentHeight = Math.floor(grandParentHeight * 0.3); // 大约30%的高度
                }
            }
        }

        // 计算固定元素的高度
        const header = parent.querySelector('.col-header');
        const searchBar = parent.querySelector('.simple-search-bar');
        const layerFilterBar = parent.querySelector('.layer-filter-bar');

        let fixedHeight = 0;
        if (header) {
            const headerHeight = header.offsetHeight || header.clientHeight;
            fixedHeight += headerHeight || 50; // 默认50px
        }
        if (searchBar) {
            const searchHeight = searchBar.offsetHeight || searchBar.clientHeight;
            fixedHeight += searchHeight || 50; // 默认50px
        }
        if (layerFilterBar) {
            const filterHeight = layerFilterBar.offsetHeight || layerFilterBar.clientHeight;
            fixedHeight += filterHeight || 44; // 默认44px (28px按钮高度 + 16px padding)
        }

        // 计算可用高度，减去固定元素
        const padding = 20; // padding总和
        const availableHeight = parentHeight - fixedHeight - padding;


        // 计算视口高度作为参考
        const viewportHeight = window.innerHeight;

        // 使用视口高度的65%作为目标高度（确保显示足够的内容）
        const targetHeight = Math.floor(viewportHeight * 0.65);

        // 优先使用计算出的高度，但如果太小，使用目标高度
        let finalHeight;
        if (availableHeight > 400 && availableHeight > targetHeight * 0.8) {
            // 如果计算出的高度合理，使用它
            finalHeight = availableHeight;
        } else {
            // 否则使用目标高度
            finalHeight = targetHeight;
        }

        // 确保最小高度至少是视口高度的50%
        const minHeight = Math.floor(viewportHeight * 0.5);
        finalHeight = Math.max(finalHeight, minHeight);

        // 设置容器高度
        container.style.height = `${finalHeight}px`;
        container.style.maxHeight = `${finalHeight}px`;
        container.style.minHeight = `${minHeight}px`;
        container.style.overflowY = 'auto';
        container.style.overflowX = 'hidden';
        container.style.flexShrink = '0';
        container.style.flexGrow = '0';
        container.style.position = 'relative';

    }

    // 立即更新一次
    updateContainerHeight();

    // 使用requestAnimationFrame确保在渲染后执行
    requestAnimationFrame(() => {
        updateContainerHeight();
        setTimeout(updateContainerHeight, 100);
        setTimeout(updateContainerHeight, 300);
        setTimeout(updateContainerHeight, 600);
    });

    // 监听窗口大小变化和父容器大小变化
    let resizeObserver;
    if (typeof ResizeObserver !== 'undefined') {
        resizeObserver = new ResizeObserver(() => {
            setTimeout(updateContainerHeight, 0);
        });

        const parent = container.parentElement;
        if (parent) {
            resizeObserver.observe(parent);

            // 同时观察父元素的父元素
            const grandParent = parent.parentElement;
            if (grandParent) {
                resizeObserver.observe(grandParent);
            }
        }
    }

    window.addEventListener('resize', () => {
        setTimeout(updateContainerHeight, 0);
    });

    container.innerHTML = '';

    if (!Array.isArray(functions) || functions.length === 0) {
        showEmptyState(container, 'settings', '暂无功能数据');
        return;
    }

    // 按分类分组
    const groupedFunctions = {};
    functions.forEach(func => {
        const category = func.category || '其他';
        if (!groupedFunctions[category]) {
            groupedFunctions[category] = [];
        }
        groupedFunctions[category].push(func);
    });

    // 按分类渲染
    Object.keys(groupedFunctions).sort().forEach(category => {
        const categoryDiv = document.createElement('div');
        categoryDiv.className = 'function-category';
        categoryDiv.style.marginBottom = '20px';

        const categoryTitle = document.createElement('div');
        categoryTitle.className = 'function-category-title';
        categoryTitle.textContent = category;
        categoryTitle.style.fontSize = '14px';
        categoryTitle.style.fontWeight = '600';
        categoryTitle.style.color = '#888';
        categoryTitle.style.marginBottom = '10px';
        categoryDiv.appendChild(categoryTitle);

        const buttonsDiv = document.createElement('div');
        buttonsDiv.className = 'function-buttons-row';
        buttonsDiv.style.display = 'flex';
        buttonsDiv.style.flexWrap = 'wrap';
        buttonsDiv.style.gap = '8px';

        groupedFunctions[category].forEach(func => {
            const btn = document.createElement('button');
            btn.className = 'func-item-btn';
            btn.textContent = func.name || func.description || func.id || func.action || '未知功能';
            btn.title = func.description || func.name || func.id || func.action || '未知功能';
            btn.dataset.functionId = func.id || func.action || '';
            btn.dataset.action = func.action || func.id || '';
            btn.dataset.code = func.code !== undefined ? func.code : '';
            btn.dataset.category = category;
            // 添加图层ID属性用于过滤
            if (func.layerId) {
                btn.dataset.layerId = func.layerId.toString();
            }
            // 如果是图层模板，添加模板相关属性
            if (func.isTemplate) {
                btn.dataset.isTemplate = 'true';
                btn.dataset.sceneName = func.scene_name || '';
                // 如果是默认模板，标记为默认
                if (func.isDefault) {
                    btn.dataset.isDefault = 'true';
                }
            }
            // 如果是播放列表，添加播放列表相关属性
            if (func.isPlaylist) {
                btn.dataset.isPlaylist = 'true';
                btn.dataset.playlistId = func.playlistId || '';
                btn.dataset.playlistName = func.playlistName || '';
                btn.dataset.targetLayerId = func.targetLayerId || '1';
            }
            buttonsDiv.appendChild(btn);
        });

        categoryDiv.appendChild(buttonsDiv);
        container.appendChild(categoryDiv);
    });

    // 重新绑定事件（因为DOM已更新）
    initSerialFunctionButtons();

    // 存储功能列表供后续使用
    window.availableFunctions = functions;

    // 再次更新容器高度，确保滚动正常工作
    setTimeout(updateContainerHeight, 200);

    // 初始化过滤状态（确保图层过滤正常工作）
    setTimeout(() => {
        filterFunctions();
    }, 100);
}

/** 空状态图标 SVG（viewBox 0 0 24 24，与 .empty-state-icon 搭配） */
const EMPTY_STATE_ICONS = {
    clipboard: '<svg class="icon-svg icon-svg--xl" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2"/><rect x="8" y="2" width="8" height="4" rx="1" ry="1"/></svg>',
    settings: '<svg class="icon-svg icon-svg--xl" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>'
};

/**
 * @brief 显示空状态
 * @param {HTMLElement} container - 容器
 * @param {string} iconKey - 图标键：'clipboard' | 'settings'，或传入 SVG 字符串
 * @param {string} message - 提示文案
 */
function showEmptyState(container, iconKey, message) {
    if (!container) return;
    const iconHtml = EMPTY_STATE_ICONS[iconKey] || iconKey;
    container.innerHTML = `
        <div class="empty-state" style="padding: 40px; text-align: center; color: #888; width: 100%;">
            <div class="empty-state-icon" style="margin-bottom: 16px;">${iconHtml}</div>
            <div>${message}</div>
        </div>
    `;
}

/** 错误状态警告图标 SVG */
const ERROR_ICON_SVG = '<svg class="icon-svg icon-svg--xl" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="color: var(--color-error, #ef4444);"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>';

/**
 * @brief 显示错误状态
 */
function showErrorState(container, title, message) {
    if (!container) return;
    container.innerHTML = `
        <div class="error-state" style="padding: 40px; text-align: center; color: #ef4444; width: 100%;">
            <div class="empty-state-icon" style="margin-bottom: 16px;">${ERROR_ICON_SVG}</div>
            <div style="font-weight: 600; margin-bottom: 8px;">${title}</div>
            <div style="font-size: 14px; color: #888;">${message}</div>
        </div>
    `;
}

function ensureForwardConfigDialog() {
    let dialog = document.getElementById('forward-config-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'forward-config-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content">
                <h3 id="forward-config-title">配置转发目标</h3>
                <div class="dialog-body forward-dialog-body">
                    <div class="forward-form-grid">
                        <div class="forward-form-item">
                            <label for="forward-target-select">目标协议</label>
                            <select id="forward-target-select" class="form-control">
                                <option value="rs232">RS232</option>
                                <option value="rs485">RS485</option>
                                <option value="udp">UDP</option>
                                <option value="tcp">TCP</option>
                                <option value="websocket">WebSocket</option>
                            </select>
                        </div>
                        <div class="forward-form-item forward-serial-only">
                            <label for="forward-port-input">串口</label>
                            <select id="forward-port-input" class="form-control"></select>
                        </div>
                        <div class="forward-form-item forward-serial-only">
                            <label for="forward-baudrate-input">波特率</label>
                            <input id="forward-baudrate-input" class="form-control" type="number" min="1" step="1" placeholder="9600">
                        </div>
                        <div class="forward-form-item forward-network-only">
                            <label for="forward-address-input">目标地址</label>
                            <input id="forward-address-input" class="form-control" type="text" placeholder="192.168.1.100">
                        </div>
                        <div class="forward-form-item forward-network-only">
                            <label for="forward-network-port-input">目标端口</label>
                            <input id="forward-network-port-input" class="form-control" type="number" min="1" max="65535" step="1" placeholder="8000">
                        </div>
                        <div class="forward-form-item forward-udp-only">
                            <label for="forward-multicast-group-input">组播地址</label>
                            <input id="forward-multicast-group-input" class="form-control" type="text" placeholder="239.255.0.1">
                        </div>
                        <div class="forward-form-item forward-target-mode">
                            <label for="forward-mode-select">发送方式</label>
                            <select id="forward-mode-select" class="form-control">
                                <option value="direct">定向发送</option>
                                <option value="broadcast">广播到已连接客户端</option>
                            </select>
                        </div>
                    </div>
                    <div class="forward-form-checks">
                        <label class="forward-check forward-udp-only">
                            <input id="forward-broadcast-check" type="checkbox">
                            <span>UDP 广播到已注册客户端</span>
                        </label>
                        <label class="forward-check forward-udp-only">
                            <input id="forward-multicast-check" type="checkbox">
                            <span>按组播发送</span>
                        </label>
                    </div>
                    <div class="forward-fixed-payload-section">
                        <div class="forward-mapping-header">
                            <span>发送内容</span>
                        </div>
                        <div class="forward-fixed-payload-row">
                            <select id="forward-fixed-payload-type" class="form-control">
                                <option value="hex">Hex</option>
                                <option value="text">文本</option>
                            </select>
                            <input id="forward-fixed-payload-input" class="form-control" type="text" placeholder="例如 AABBCC">
                        </div>
                        <div class="forward-dialog-hint forward-fixed-hint">Hex 会按字节发送；文本会按原文发送，串口目标会自动转换为对应字节。</div>
                    </div>
                    <div class="forward-mapping-section">
                        <div class="forward-mapping-header">
                            <span>命令映射</span>
                            <button id="forward-add-mapping-btn" type="button" class="btn small secondary">添加映射</button>
                        </div>
                        <div id="forward-command-mapping-list" class="forward-command-mapping-list"></div>
                    </div>
                    <div class="forward-dialog-hint forward-mapping-hint">未配置或未命中映射时继续转发原始指令；同一个收到 Hex 可添加多条映射，按列表顺序发送。</div>
                </div>
                <div class="dialog-footer">
                    <button id="forward-config-cancel-btn" class="btn">取消</button>
                    <button id="forward-config-ok-btn" class="btn primary">确定</button>
                </div>
            </div>
        `;
        document.body.appendChild(dialog);

        const toggle = (preferredSerialPort) => {
            const target = dialog.querySelector('#forward-target-select')?.value || 'rs232';
            const modeSelect = dialog.querySelector('#forward-mode-select');
            const serialOnly = dialog.querySelectorAll('.forward-serial-only');
            const networkOnly = dialog.querySelectorAll('.forward-network-only');
            const udpOnly = dialog.querySelectorAll('.forward-udp-only');
            const targetMode = dialog.querySelectorAll('.forward-target-mode');
            const isSerial = target === 'rs232' || target === 'rs485';
            const isUdp = target === 'udp';
            const supportsMode = target === 'tcp' || target === 'websocket';

            serialOnly.forEach(el => { el.style.display = isSerial ? '' : 'none'; });
            networkOnly.forEach(el => { el.style.display = isSerial ? 'none' : ''; });
            udpOnly.forEach(el => { el.style.display = isUdp ? '' : 'none'; });
            targetMode.forEach(el => { el.style.display = supportsMode ? '' : 'none'; });
            if (modeSelect) {
                modeSelect.disabled = !supportsMode;
                if (!supportsMode) modeSelect.value = 'direct';
            }
            if (isSerial) {
                refreshForwardSerialPortOptions(dialog, preferredSerialPort);
            }
        };

        dialog.querySelector('#forward-target-select')?.addEventListener('change', () => toggle());
        dialog.querySelector('#forward-fixed-payload-type')?.addEventListener('change', () => {
            const type = dialog.querySelector('#forward-fixed-payload-type')?.value || 'hex';
            const input = dialog.querySelector('#forward-fixed-payload-input');
            if (input) {
                input.placeholder = type === 'text' ? '例如 power on' : '例如 AABBCC';
            }
        });
        dialog.querySelector('#forward-add-mapping-btn')?.addEventListener('click', () => {
            appendForwardCommandMappingRow(dialog, {
                source_hex: dialog._forwardDefaultSourceHex || ''
            });
        });
        dialog.querySelector('#forward-config-cancel-btn')?.addEventListener('click', () => {
            const resolve = dialog._resolve;
            dialog.classList.remove('active');
            dialog._resolve = null;
            if (resolve) resolve(null);
        });
        dialog.querySelector('#forward-config-ok-btn')?.addEventListener('click', () => {
            const target = dialog.querySelector('#forward-target-select')?.value || 'rs232';
            const isSerial = target === 'rs232' || target === 'rs485';
            const fixedMode = !!dialog._forwardFixedMode;
            const config = {
                forward_target: target,
                forward_port: isSerial ? (dialog.querySelector('#forward-port-input')?.value?.trim() || '') : '',
                forward_baudrate: parseInt(dialog.querySelector('#forward-baudrate-input')?.value || '0', 10) || 0,
                forward_address: dialog.querySelector('#forward-address-input')?.value?.trim() || '',
                multicast_group: dialog.querySelector('#forward-multicast-group-input')?.value?.trim() || '',
                forward_mode: dialog.querySelector('#forward-mode-select')?.value || 'direct',
                broadcast: !!dialog.querySelector('#forward-broadcast-check')?.checked,
                multicast: !!dialog.querySelector('#forward-multicast-check')?.checked,
                command_mappings: []
            };
            if (fixedMode) {
                const payloadType = dialog.querySelector('#forward-fixed-payload-type')?.value || 'hex';
                const payloadInput = dialog.querySelector('#forward-fixed-payload-input');
                const rawValue = payloadInput?.value || '';
                if (!rawValue.trim()) {
                    addToCommandLog('发送固定指令', 'warning', '请填写要发送的内容');
                    return;
                }
                config.fixed_send = true;
                config.fixed_payload_type = payloadType;
                if (payloadType === 'text') {
                    config.payload = rawValue;
                    config.hex = textToHexString(rawValue);
                } else {
                    const normalized = normalizeHexInput(rawValue);
                    if (!normalized.ok) {
                        addToCommandLog('发送固定指令', 'warning', normalized.message);
                        return;
                    }
                    config.hex = normalized.value;
                    config.payload = '';
                }
            } else {
                const mappingResult = collectForwardCommandMappings(dialog);
                if (!mappingResult.ok) {
                    addToCommandLog('转发配置', 'warning', mappingResult.message);
                    return;
                }
                config.command_mappings = mappingResult.value;
            }
            const networkPort = parseInt(dialog.querySelector('#forward-network-port-input')?.value || '0', 10) || 0;
            if (isSerial) {
                if (!config.forward_port) {
                    addToCommandLog('转发配置', 'warning', '请选择目标串口');
                    return;
                }
            } else if (target === 'udp') {
                if (!config.broadcast && !config.multicast && !config.forward_address) {
                    addToCommandLog('转发配置', 'warning', '请填写 UDP 目标地址，或启用广播/组播');
                    return;
                }
                if (!config.broadcast && networkPort <= 0) {
                    addToCommandLog('转发配置', 'warning', '请填写 UDP 目标端口');
                    return;
                }
            } else if ((target === 'tcp' || target === 'websocket') &&
                       config.forward_mode !== 'broadcast') {
                addToCommandLog('转发配置', 'warning', `${target.toUpperCase()} 当前只支持广播到已连接客户端`);
                return;
            }
            if (networkPort > 0) {
                config.forward_port = networkPort;
            }
            const resolve = dialog._resolve;
            dialog.classList.remove('active');
            dialog._resolve = null;
            if (resolve) resolve(config);
        });
        dialog.addEventListener('click', (e) => {
            if (e.target === dialog) {
                const resolve = dialog._resolve;
                dialog.classList.remove('active');
                dialog._resolve = null;
                if (resolve) resolve(null);
            }
        });
        dialog._toggle = toggle;
    }
    return dialog;
}

function appendForwardCommandMappingRow(dialog, mapping = {}) {
    const list = dialog?.querySelector('#forward-command-mapping-list');
    if (!list) return null;

    const row = document.createElement('div');
    row.className = 'forward-command-mapping-row';
    row.innerHTML = `
        <input class="form-control forward-command-source" type="text" placeholder="收到 Hex" value="${mapping.source_hex || ''}">
        <span class="forward-command-arrow">→</span>
        <input class="form-control forward-command-target" type="text" placeholder="转发 Hex" value="${mapping.target_hex || ''}">
        <button type="button" class="btn small transparent forward-remove-mapping-btn" title="删除映射">×</button>
    `;
    row.querySelector('.forward-remove-mapping-btn')?.addEventListener('click', () => row.remove());
    list.appendChild(row);
    return row;
}

function renderForwardCommandMappings(dialog, mappings = []) {
    const list = dialog?.querySelector('#forward-command-mapping-list');
    if (!list) return;
    list.innerHTML = '';
    normalizeForwardCommandMappings(mappings).forEach(mapping => {
        appendForwardCommandMappingRow(dialog, mapping);
    });
}

function collectForwardCommandMappings(dialog) {
    const rows = dialog?.querySelectorAll('.forward-command-mapping-row') || [];
    const mappings = [];

    for (const row of rows) {
        const sourceRaw = row.querySelector('.forward-command-source')?.value?.trim() || '';
        const targetRaw = row.querySelector('.forward-command-target')?.value?.trim() || '';
        if (!sourceRaw && !targetRaw) continue;
        if (!sourceRaw || !targetRaw) {
            return { ok: false, message: '命令映射需要同时填写收到 Hex 和转发 Hex' };
        }
        const source = normalizeHexInput(sourceRaw);
        if (!source.ok) {
            return { ok: false, message: `收到 Hex 无效：${source.message}` };
        }
        const target = normalizeHexInput(targetRaw);
        if (!target.ok) {
            return { ok: false, message: `转发 Hex 无效：${target.message}` };
        }
        mappings.push({ source_hex: source.value, target_hex: target.value });
    }

    return { ok: true, value: mappings };
}

function showForwardConfigDialog(initial = {}) {
    return new Promise((resolve) => {
        const dialog = ensureForwardConfigDialog();
        const initialTarget = initial.forward_target || 'rs232';
        const isSerial = initialTarget === 'rs232' || initialTarget === 'rs485';
        const fixedMode = initial.fixedMode === true || isFixedForwardConfig(initial);
        const payloadType = initial.fixed_payload_type || initial.payload_type ||
            (initial.payload && !initial.hex ? 'text' : 'hex');
        const payloadValue = payloadType === 'text'
            ? (initial.payload || '')
            : (initial.hex || initial.payload || '');
        dialog._resolve = resolve;
        dialog._forwardFixedMode = fixedMode;
        const defaultSource = normalizeHexInput(initial.default_source_hex || '');
        dialog._forwardDefaultSourceHex = defaultSource.ok ? defaultSource.value : '';
        dialog.classList.toggle('is-fixed-send', fixedMode);
        dialog.querySelector('#forward-config-title').textContent = fixedMode ? '发送固定指令' : '配置转发目标';
        dialog.querySelector('#forward-config-ok-btn').textContent = fixedMode ? '保存' : '确定';
        dialog.querySelector('#forward-target-select').value = initialTarget;
        dialog.querySelector('#forward-baudrate-input').value = initial.forward_baudrate || '';
        dialog.querySelector('#forward-address-input').value = initial.forward_address || '';
        dialog.querySelector('#forward-network-port-input').value = isSerial ? '' : (initial.forward_port || '');
        dialog.querySelector('#forward-multicast-group-input').value = initial.multicast_group || '';
        dialog.querySelector('#forward-mode-select').value = initial.forward_mode || 'direct';
        dialog.querySelector('#forward-broadcast-check').checked = !!initial.broadcast;
        dialog.querySelector('#forward-multicast-check').checked = !!initial.multicast;
        dialog.querySelector('#forward-fixed-payload-type').value = payloadType;
        dialog.querySelector('#forward-fixed-payload-input').value = payloadValue;
        dialog.querySelector('#forward-fixed-payload-input').placeholder =
            payloadType === 'text' ? '例如 power on' : '例如 AABBCC';
        renderForwardCommandMappings(dialog, fixedMode ? [] : (initial.command_mappings || initial.forward_command_mappings || []));
        if (typeof dialog._toggle === 'function') {
            dialog._toggle(isSerial ? (initial.forward_port || '') : undefined);
        }
        dialog.classList.add('active');
    });
}

function getForwardConfigFromChip(chip) {
    const config = {
        forward_target: chip.dataset.forwardTarget || 'rs232',
        forward_port: chip.dataset.forwardPort || '',
        forward_baudrate: parseInt(chip.dataset.forwardBaudrate || '0', 10) || 0,
        forward_address: chip.dataset.forwardAddress || '',
        forward_mode: chip.dataset.forwardMode || 'direct',
        multicast_group: chip.dataset.multicastGroup || '',
        broadcast: chip.dataset.forwardBroadcast === 'true',
        multicast: chip.dataset.forwardMulticast === 'true',
        command_mappings: normalizeForwardCommandMappings(chip.dataset.commandMappings || '[]')
    };
    const fixedSend = chip.dataset.forwardFixedSend === 'true' ||
        !!chip.dataset.fixedPayloadType ||
        !!chip.dataset.forwardPayload ||
        !!chip.dataset.forwardHex;
    if (fixedSend) {
        config.fixed_send = true;
        config.fixed_payload_type = chip.dataset.fixedPayloadType || 'hex';
        config.payload = chip.dataset.forwardPayload || '';
        config.hex = chip.dataset.forwardHex || '';
        config.command_mappings = [];
    }
    return config;
}

function applyForwardConfigToChip(chip, config) {
    const commandMappings = normalizeForwardCommandMappings(config.command_mappings || config.forward_command_mappings || []);
    const fixedSend = isFixedForwardConfig(config);
    const fixedPayloadType = config.fixed_payload_type || config.payload_type ||
        (config.payload && !config.hex ? 'text' : 'hex');
    chip.dataset.forwardTarget = config.forward_target || '';
    chip.dataset.forwardPort = config.forward_port ? String(config.forward_port) : '';
    chip.dataset.forwardBaudrate = config.forward_baudrate ? String(config.forward_baudrate) : '';
    chip.dataset.forwardAddress = config.forward_address || '';
    chip.dataset.forwardMode = config.forward_mode || 'direct';
    chip.dataset.multicastGroup = config.multicast_group || '';
    chip.dataset.forwardBroadcast = config.broadcast ? 'true' : '';
    chip.dataset.forwardMulticast = config.multicast ? 'true' : '';
    chip.dataset.commandMappings = !fixedSend && commandMappings.length ? JSON.stringify(commandMappings) : '';
    chip.dataset.forwardFixedSend = fixedSend ? 'true' : '';
    chip.dataset.fixedPayloadType = fixedSend ? fixedPayloadType : '';
    chip.dataset.forwardPayload = fixedSend ? (config.payload || '') : '';
    chip.dataset.forwardHex = fixedSend ? (config.hex || '') : '';

    const parts = [];
    if (config.forward_target) parts.push(config.forward_target.toUpperCase());
    if (config.forward_address) parts.push(config.forward_address);
    if (config.forward_port) parts.push(String(config.forward_port));
    if (config.forward_port && (config.forward_target === 'rs232' || config.forward_target === 'rs485') && config.forward_baudrate) {
        parts.push(`@${config.forward_baudrate}`);
    }
    if (config.broadcast) parts.push('广播');
    if (config.multicast) parts.push('组播');
    if (commandMappings.length > 0) {
        parts.push(`映射${commandMappings.length}条`);
    }
    if (fixedSend) {
        const content = fixedPayloadType === 'text'
            ? `文本 ${getForwardPayloadPreview(config.payload)}`
            : `Hex ${getForwardPayloadPreview(config.hex)}`;
        parts.push(content);
    }
    const subtitle = chip.querySelector('.function-chip-subtitle');
    if (subtitle) {
        subtitle.textContent = parts.join(' ') || (fixedSend ? '未配置发送内容' : '未配置转发目标');
    }
}

function renderFunctionConfigEditor(config) {
    const normalized = normalizeFunctionConfig(config);
    functionConfigState = normalized;
    if (!functionConfigState[activeFunctionRegion]) {
        activeFunctionRegion = 'hvac';
    }
    const activeArea = getActiveFunctionArea();
    if (!activeArea.items.some(item => item.id === activeFunctionButtonId)) {
        activeFunctionButtonId = activeArea.items[0]?.id || '';
    }

    renderFunctionRegions();
    renderFunctionRegionSettings();
    renderFunctionButtons();
    renderActiveFunctionButtonWorkspace();
    renderFunctionConfigFunctions(functionConfigAvailableFunctions);
}

function initFunctionConfigWorkbench() {
    document.getElementById('function-region-list')?.addEventListener('click', event => {
        const item = event.target.closest('.function-region-item');
        if (!item) return;
        commitFunctionConfigEditor();
        activeFunctionRegion = item.dataset.region || 'hvac';
        const area = getActiveFunctionArea();
        activeFunctionButtonId = area.items[0]?.id || '';
        renderFunctionConfigEditor(functionConfigState);
    });

    document.getElementById('function-region-settings')?.addEventListener('input', updateActiveFunctionRegionFromFields);
    document.getElementById('function-region-settings')?.addEventListener('change', updateActiveFunctionRegionFromFields);

    document.getElementById('function-button-list')?.addEventListener('click', event => {
        const row = event.target.closest('.function-button-row');
        if (!row) return;
        commitFunctionConfigEditor();
        activeFunctionButtonId = row.dataset.buttonId || activeFunctionButtonId;
        renderFunctionButtons();
        renderActiveFunctionButtonWorkspace();
    });
    document.getElementById('function-button-list')?.addEventListener('input', updateFunctionButtonsFromRows);
    document.getElementById('function-button-list')?.addEventListener('change', updateFunctionButtonsFromRows);

    document.getElementById('function-config-search')?.addEventListener('input', filterFunctionConfigFunctions);
    document.querySelectorAll('#function-config-layer-filter .layer-filter-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('#function-config-layer-filter .layer-filter-btn')
                .forEach(item => item.classList.remove('active'));
            btn.classList.add('active');
            filterFunctionConfigFunctions();
        });
    });
}

function getActiveFunctionArea() {
    return functionConfigState[activeFunctionRegion] || functionConfigState.hvac;
}

function getActiveFunctionButton() {
    const area = getActiveFunctionArea();
    return area.items.find(item => item.id === activeFunctionButtonId) || area.items[0] || null;
}

function renderFunctionRegions() {
    const container = document.getElementById('function-region-list');
    if (!container) return;
    const regions = [
        { key: 'hvac', label: functionConfigState.hvac.title || '空调', desc: '温度、模式、风速按钮' },
        { key: 'lighting', label: functionConfigState.lighting.title || '灯光', desc: '灯光开关与调光区域' }
    ];
    container.innerHTML = regions.map(region => {
        const area = functionConfigState[region.key];
        const buttonCount = Array.isArray(area.items) ? area.items.length : 0;
        return `
            <div class="function-region-item ${region.key === activeFunctionRegion ? 'active' : ''}" data-region="${region.key}">
                <div>
                    <div class="function-region-title">${escapeHtml(region.label)}</div>
                    <div class="function-region-meta">${escapeHtml(region.desc)} · ${buttonCount} 个按钮</div>
                </div>
                <label class="preview-switch" title="启用区域">
                    <input type="checkbox" disabled ${area.enabled !== false ? 'checked' : ''}>
                    <span class="switch-slider"></span>
                </label>
            </div>
        `;
    }).join('');
}

function renderFunctionRegionSettings() {
    const container = document.getElementById('function-region-settings');
    if (!container) return;
    const area = getActiveFunctionArea();
    if (activeFunctionRegion === 'hvac') {
        container.innerHTML = `
            <div class="function-config-row-toggle">
                <span>启用空调区域</span>
                <label class="preview-switch">
                    <input type="checkbox" id="function-region-enabled" ${area.enabled !== false ? 'checked' : ''}>
                    <span class="switch-slider"></span>
                </label>
            </div>
            <div class="setting-item">
                <label>区域名称:</label>
                <input type="text" id="function-region-title" class="form-control dark-input peripheral-config-input" value="${escapeHtml(area.title || '')}">
            </div>
            <div class="function-config-inline-fields">
                <div class="setting-item">
                    <label>默认温度:</label>
                    <input type="number" id="function-hvac-default-temp" class="form-control dark-input peripheral-config-input" min="0" max="60" value="${area.default_temperature}">
                </div>
                <div class="setting-item">
                    <label>最低温度:</label>
                    <input type="number" id="function-hvac-temp-min" class="form-control dark-input peripheral-config-input" min="0" max="50" value="${area.temperature_min}">
                </div>
                <div class="setting-item">
                    <label>最高温度:</label>
                    <input type="number" id="function-hvac-temp-max" class="form-control dark-input peripheral-config-input" min="1" max="60" value="${area.temperature_max}">
                </div>
                <div class="setting-item">
                    <label>默认风速:</label>
                    <select id="function-hvac-default-fan" class="form-control dark-input peripheral-config-input">
                        <option value="1" ${Number(area.default_fan_speed) === 1 ? 'selected' : ''}>低风</option>
                        <option value="2" ${Number(area.default_fan_speed) === 2 ? 'selected' : ''}>中风</option>
                        <option value="3" ${Number(area.default_fan_speed) === 3 ? 'selected' : ''}>高风</option>
                    </select>
                </div>
            </div>
            <div class="setting-item">
                <label>默认模式:</label>
                <select id="function-hvac-default-mode" class="form-control dark-input peripheral-config-input">
                    ${['cool', 'heat', 'fan', 'dry', 'auto'].map(mode =>
                        `<option value="${mode}" ${area.default_mode === mode ? 'selected' : ''}>${escapeHtml(getHvacModeDisplayName(mode))}</option>`
                    ).join('')}
                </select>
            </div>
        `;
        return;
    }

    container.innerHTML = `
        <div class="function-config-row-toggle">
            <span>启用灯光区域</span>
            <label class="preview-switch">
                <input type="checkbox" id="function-region-enabled" ${area.enabled !== false ? 'checked' : ''}>
                <span class="switch-slider"></span>
            </label>
        </div>
        <div class="setting-item">
            <label>区域名称:</label>
            <input type="text" id="function-region-title" class="form-control dark-input peripheral-config-input" value="${escapeHtml(area.title || '')}">
        </div>
        <div class="function-config-inline-fields">
            <div class="setting-item">
                <label>默认亮度:</label>
                <input type="number" id="function-lighting-dimmer" class="form-control dark-input peripheral-config-input" min="0" max="100" value="${area.dimmer?.default_value ?? 50}">
            </div>
            <div class="setting-item">
                <label>显示调光:</label>
                <label class="preview-switch">
                    <input type="checkbox" id="function-lighting-dimmer-enabled" ${area.dimmer?.enabled !== false ? 'checked' : ''}>
                    <span class="switch-slider"></span>
                </label>
            </div>
        </div>
    `;
}

function renderFunctionButtons() {
    const container = document.getElementById('function-button-list');
    if (!container) return;
    const area = getActiveFunctionArea();
    container.innerHTML = area.items.map(item => `
        <div class="function-button-row ${item.id === activeFunctionButtonId ? 'active' : ''}" data-button-id="${escapeHtml(item.id)}">
            <div class="function-button-type">${escapeHtml(getFunctionButtonRoleLabel(item))}</div>
            <input type="text" class="form-control dark-input peripheral-config-input function-label-input"
                value="${escapeHtml(item.label || '')}" aria-label="${escapeHtml(item.id)} 显示名称">
            <div class="function-button-toggle">
                <span>显示</span>
                <label class="preview-switch">
                    <input type="checkbox" class="function-enabled-input" ${item.enabled !== false ? 'checked' : ''}>
                    <span class="switch-slider"></span>
                </label>
            </div>
            ${activeFunctionRegion === 'lighting' ? `
                <div class="function-button-toggle">
                    <span>默认开</span>
                    <label class="preview-switch">
                        <input type="checkbox" class="function-default-input" ${item.default_on ? 'checked' : ''}>
                        <span class="switch-slider"></span>
                    </label>
                </div>
            ` : `<span class="function-button-toggle">${(item.functions || []).length} 个功能</span>`}
        </div>
    `).join('');
    const activeButton = getActiveFunctionButton();
    const badge = document.getElementById('function-active-button-badge');
    if (badge && activeButton) {
        badge.textContent = `${area.title || ''} / ${activeButton.label || activeButton.id}`;
    }
}

function updateActiveFunctionRegionFromFields() {
    const area = getActiveFunctionArea();
    area.enabled = document.getElementById('function-region-enabled')?.checked !== false;
    area.title = document.getElementById('function-region-title')?.value?.trim() || area.title;
    if (activeFunctionRegion === 'hvac') {
        area.temperature_min = clampNumber(document.getElementById('function-hvac-temp-min')?.value, 0, 50, area.temperature_min);
        area.temperature_max = clampNumber(document.getElementById('function-hvac-temp-max')?.value, 1, 60, area.temperature_max);
        if (area.temperature_max < area.temperature_min) area.temperature_max = area.temperature_min;
        area.default_temperature = clampNumber(
            document.getElementById('function-hvac-default-temp')?.value,
            area.temperature_min,
            area.temperature_max,
            area.default_temperature
        );
        area.default_mode = document.getElementById('function-hvac-default-mode')?.value || area.default_mode;
        area.default_fan_speed = clampNumber(document.getElementById('function-hvac-default-fan')?.value, 1, 3, area.default_fan_speed);
    } else {
        area.dimmer = area.dimmer || {};
        area.dimmer.enabled = document.getElementById('function-lighting-dimmer-enabled')?.checked !== false;
        area.dimmer.default_value = clampNumber(document.getElementById('function-lighting-dimmer')?.value, 0, 100, area.dimmer.default_value || 50);
    }
    renderFunctionRegions();
}

function updateFunctionButtonsFromRows() {
    const area = getActiveFunctionArea();
    area.items = area.items.map(item => {
        const row = document.querySelector(`#function-button-list .function-button-row[data-button-id="${item.id}"]`);
        if (!row) return item;
        return {
            ...item,
            label: row.querySelector('.function-label-input')?.value?.trim() || item.label,
            enabled: row.querySelector('.function-enabled-input')?.checked !== false,
            default_on: activeFunctionRegion === 'lighting'
                ? row.querySelector('.function-default-input')?.checked === true
                : item.default_on
        };
    });
    renderFunctionRegions();
}

function commitFunctionWorkspaceToActiveButton() {
    const button = getActiveFunctionButton();
    const workspace = document.getElementById('function-button-workspace');
    if (!button || !workspace) return;
    const seen = new Set();
    const functions = [];
    workspace.querySelectorAll('.function-chip').forEach(chip => {
        const fd = chipToFunctionData(chip);
        if (!fd) return;
        const identityKey = fd.function_id === FORWARD_FUNCTION_ID
            ? forwardFunctionIdentity(fd)
            : fd.function_id;
        if (seen.has(identityKey)) return;
        seen.add(identityKey);
        functions.push(fd);
    });
    button.functions = functions;
}

function commitFunctionConfigEditor() {
    updateActiveFunctionRegionFromFields();
    updateFunctionButtonsFromRows();
    commitFunctionWorkspaceToActiveButton();
    functionConfigState = normalizeFunctionConfig(functionConfigState);
}

function renderActiveFunctionButtonWorkspace() {
    const workspace = document.getElementById('function-button-workspace');
    if (!workspace) return;
    const button = getActiveFunctionButton();
    workspace.innerHTML = '';
    if (!button) {
        workspace.innerHTML = '<div class="workspace-empty-state">请先选择按钮</div>';
        return;
    }
    const functions = Array.isArray(button.functions) ? button.functions : [];
    if (!functions.length) {
        workspace.innerHTML = '<div class="workspace-empty-state">从右侧选择功能，点击即可添加到此按钮</div>';
        return;
    }
    functions.forEach(func => {
        const chip = createFunctionChipFromData(func);
        workspace.appendChild(chip);
        bindFunctionConfigChipActions(chip);
    });
}

function createFunctionChipFromData(func) {
    const chip = document.createElement('div');
    chip.className = 'function-chip';
    chip.dataset.functionId = func.function_id || '';
    chip.dataset.action = func.action || '';
    chip.dataset.functionCode = func.function_code || '';
    if (isForwardFunctionLike(func)) {
        chip.dataset.functionId = FORWARD_FUNCTION_ID;
        chip.dataset.action = 'forward_payload';
    }
    if (func.layerId) chip.dataset.layerId = String(func.layerId);
    if (func.isTemplate) chip.dataset.isTemplate = 'true';
    if (func.isDefault) chip.dataset.isDefault = 'true';
    if (func.scene_name) chip.dataset.sceneName = func.scene_name;
    if (func.isPlaylist) chip.dataset.isPlaylist = 'true';
    if (func.playlistId) chip.dataset.playlistId = String(func.playlistId);
    if (func.playlistName) chip.dataset.playlistName = func.playlistName;
    if (func.targetLayerId) chip.dataset.targetLayerId = String(func.targetLayerId);
    if (func.layerId || func.targetLayerId) {
        chip.dataset.layerId = String(func.layerId || func.targetLayerId);
    }
    chip.innerHTML = `
        <div class="function-chip-main">
            <span>${escapeHtml(func.function_name || func.name || func.function_id || '')}</span>
            ${isForwardFunctionLike(func) ? '<small class="function-chip-subtitle"></small>' : ''}
        </div>
        <div class="chip-actions">
            <button class="move-btn move-up" title="上移">▲</button>
            <button class="move-btn move-down" title="下移">▼</button>
            <button class="close-btn" title="删除">×</button>
        </div>
    `;
    if (isForwardFunctionLike(func)) {
        applyForwardConfigToChip(chip, func);
    }
    return chip;
}

function createFunctionChipFromButton(btn) {
    const functionId = btn.dataset.functionId || '';
    const functionName = btn.textContent.trim();
    const func = {
        function_id: functionId,
        function_name: functionName,
        action: btn.dataset.action || '',
        function_code: parseInt(btn.dataset.code || '0', 10) || 0
    };
    if (btn.dataset.fixedSend === 'true') {
        func.fixed_send = true;
        func.fixed_payload_type = 'hex';
    }
    if (btn.dataset.layerId) func.layerId = parseInt(btn.dataset.layerId, 10) || undefined;
    if (btn.dataset.isTemplate) {
        func.isTemplate = true;
        if (btn.dataset.isDefault) func.isDefault = true;
        if (btn.dataset.sceneName) func.scene_name = btn.dataset.sceneName;
    }
    if (btn.dataset.isPlaylist) {
        func.isPlaylist = true;
        func.playlistId = btn.dataset.playlistId || '';
        func.playlistName = btn.dataset.playlistName || '';
        func.targetLayerId = parseInt(btn.dataset.targetLayerId || '1', 10) || 1;
        func.layerId = func.targetLayerId;
        func.action = 'play';
        func.function_code = 0x02;
    }
    return createFunctionChipFromData(func);
}

function addFunctionToActiveButton(btn) {
    const button = getActiveFunctionButton();
    const workspace = document.getElementById('function-button-workspace');
    if (!button || !workspace) {
        addToCommandLog('添加功能', 'warning', '请先选择按钮');
        return;
    }

    const functionId = btn.dataset.functionId || '';
    const functionName = btn.textContent.trim();
    if (functionId !== FORWARD_FUNCTION_ID &&
        workspace.querySelector(`.function-chip[data-function-id="${CSS.escape(functionId)}"]`)) {
        addToCommandLog('添加功能', 'warning', `"${functionName}" 已存在`);
        return;
    }

    workspace.querySelector('.workspace-empty-state')?.remove();
    const chip = createFunctionChipFromButton(btn);
    workspace.appendChild(chip);
    bindFunctionConfigChipActions(chip);

    if (functionId === FORWARD_FUNCTION_ID) {
        const fixedSend = btn.dataset.fixedSend === 'true';
        showForwardConfigDialog({
            default_source_hex: '',
            fixedMode: fixedSend,
            fixed_send: fixedSend
        }).then(config => {
            if (!config) {
                chip.remove();
                commitFunctionWorkspaceToActiveButton();
                if (!workspace.querySelector('.function-chip')) {
                    workspace.innerHTML = '<div class="workspace-empty-state">从右侧选择功能，点击即可添加到此按钮</div>';
                }
                return;
            }
            applyForwardConfigToChip(chip, config);
            commitFunctionWorkspaceToActiveButton();
            renderFunctionButtons();
        });
    }

    commitFunctionWorkspaceToActiveButton();
    renderFunctionButtons();
    addToCommandLog('添加功能', 'success', `已为按钮 "${button.label}" 添加 ${functionName}`);
}

function bindFunctionConfigChipActions(chip) {
    const workspace = document.getElementById('function-button-workspace');
    chip.querySelector('.close-btn')?.addEventListener('click', () => {
        chip.remove();
        commitFunctionWorkspaceToActiveButton();
        renderFunctionButtons();
        if (workspace && !workspace.querySelector('.function-chip')) {
            workspace.innerHTML = '<div class="workspace-empty-state">从右侧选择功能，点击即可添加到此按钮</div>';
        }
    });
    chip.querySelector('.move-up')?.addEventListener('click', event => {
        event.stopPropagation();
        moveFunctionChip(chip, 'up');
        commitFunctionWorkspaceToActiveButton();
    });
    chip.querySelector('.move-down')?.addEventListener('click', event => {
        event.stopPropagation();
        moveFunctionChip(chip, 'down');
        commitFunctionWorkspaceToActiveButton();
    });
    if (isForwardChip(chip)) {
        chip.addEventListener('dblclick', async () => {
            const config = await showForwardConfigDialog({
                ...getForwardConfigFromChip(chip),
                fixedMode: chip.dataset.forwardFixedSend === 'true'
            });
            if (!config) return;
            applyForwardConfigToChip(chip, config);
            commitFunctionWorkspaceToActiveButton();
        });
    }
}

async function loadFunctionConfigFunctions() {
    const container = document.getElementById('function-config-functions-grid');
    if (!container) return;
    try {
        const functions = await loadCommandsFromCommandList();
        const templates = await loadLayerTemplates();
        const playlists = await loadPlaylists();
        functionConfigAvailableFunctions = [buildFixedSendFunctionEntry(), ...functions, ...templates, ...playlists];
        renderFunctionConfigFunctions(functionConfigAvailableFunctions);
    } catch (error) {
        showErrorState(container, '加载功能列表失败', error.message || '网络错误');
    }
}

function renderFunctionConfigFunctions(functions) {
    const container = document.getElementById('function-config-functions-grid');
    if (!container) return;
    container.innerHTML = '';
    if (!Array.isArray(functions) || functions.length === 0) {
        showEmptyState(container, 'settings', '暂无功能数据');
        return;
    }
    const grouped = {};
    functions.forEach(func => {
        const cat = func.category || '其他';
        if (!grouped[cat]) grouped[cat] = [];
        grouped[cat].push(func);
    });
    Object.keys(grouped).sort().forEach(category => {
        const catDiv = document.createElement('div');
        catDiv.className = 'function-category';
        catDiv.style.marginBottom = '16px';
        const title = document.createElement('div');
        title.className = 'function-category-title';
        title.textContent = category;
        title.style.cssText = 'font-size:12px;font-weight:600;color:#64748b;margin-bottom:8px;text-transform:uppercase;letter-spacing:0.05em;';
        catDiv.appendChild(title);
        const row = document.createElement('div');
        row.className = 'function-buttons-row';
        row.style.cssText = 'display:flex;flex-wrap:wrap;gap:6px;';
        grouped[category].forEach(func => {
            const btn = document.createElement('button');
            btn.className = 'func-item-btn';
            btn.textContent = func.name || func.action || '未知功能';
            btn.title = func.description || func.name || '';
            btn.dataset.functionId = func.id || func.action || '';
            btn.dataset.action = func.action || func.id || '';
            btn.dataset.code = func.code !== undefined ? func.code : '';
            btn.dataset.category = category;
            if (func.fixedSend || func.fixed_send) btn.dataset.fixedSend = 'true';
            if (func.layerId) btn.dataset.layerId = String(func.layerId);
            if (func.isTemplate) btn.dataset.isTemplate = 'true';
            if (func.isDefault) btn.dataset.isDefault = 'true';
            if (func.scene_name) btn.dataset.sceneName = func.scene_name;
            if (func.isPlaylist) btn.dataset.isPlaylist = 'true';
            if (func.playlistId) btn.dataset.playlistId = String(func.playlistId);
            if (func.playlistName) btn.dataset.playlistName = func.playlistName;
            if (func.targetLayerId) btn.dataset.targetLayerId = String(func.targetLayerId);
            btn.addEventListener('click', () => addFunctionToActiveButton(btn));
            row.appendChild(btn);
        });
        catDiv.appendChild(row);
        container.appendChild(catDiv);
    });
    filterFunctionConfigFunctions();
}

function filterFunctionConfigFunctions() {
    const container = document.getElementById('function-config-functions-grid');
    if (!container) return;
    const keyword = (document.getElementById('function-config-search')?.value || '').trim().toLowerCase();
    const selectedLayer = document.querySelector('#function-config-layer-filter .layer-filter-btn.active')?.dataset.layer || 'all';
    const categories = new Set();
    container.querySelectorAll('.func-item-btn').forEach(btn => {
        const text = btn.textContent.toLowerCase();
        const visible = (!keyword || text.includes(keyword)) && matchesFunctionLayer(btn, selectedLayer);
        btn.style.display = visible ? '' : 'none';
        const cat = btn.closest('.function-category');
        if (cat) categories.add(cat);
    });
    categories.forEach(cat => {
        const visible = Array.from(cat.querySelectorAll('.func-item-btn')).some(btn => btn.style.display !== 'none');
        cat.style.display = visible ? '' : 'none';
    });
}

function getFunctionButtonRoleLabel(item) {
    if (item.kind === 'temperature') return item.id === 'temp_up' ? '升温' : '降温';
    if (item.kind === 'mode') return '模式';
    if (item.kind === 'fan') return '风速';
    if (item.kind === 'power') return '电源';
    return item.id;
}

function getHvacModeDisplayName(mode) {
    const names = { cool: '制冷', heat: '制热', fan: '送风', dry: '除湿', auto: '自动' };
    return names[mode] || mode;
}

function collectFunctionConfigFromEditor() {
    commitFunctionConfigEditor();
    return normalizeFunctionConfig(functionConfigState);
}

async function saveFunctionConfig() {
    const config = collectFunctionConfigFromEditor();
    functionConfigState = config;
    renderFunctionConfigEditor(config);

    try {
        const response = await apiAction('peripherals', 'set_config', {
                peripheral_type: 'function_config',
                ...config
            });

        if (response != null) {
            addToCommandLog('保存功能配置', 'success', '移动端空调/灯光配置已同步');
            appendLog('SYSTEM', '功能配置已持久化至后端');
        } else {
            throw new Error(response?.message || '引擎拒绝了配置请求');
        }
    } catch (error) {
        addToCommandLog('保存功能配置失败', 'error', error.message);
        appendLog('ERROR', `功能配置应用失败: ${error.message}`);
    }
}

/**
 * @brief 保存外设配置 (统一数据处理逻辑)
 * 示例/字段：@param {string} type 'rs232', 'rs485', 'dmx', 'udp', 'tcp'
 */
async function savePeripheralConfig(type) {
    let config = { peripheral_type: type };

    // 映射端口元素 ID
    const usesSharedSerialControls = type === 'rs232' || type === 'rs485';
    const portId = usesSharedSerialControls ? 'serial-port-select' : `${type}-port`;
    const baudId = usesSharedSerialControls ? 'serial-baud-select' : `${type}-baudrate`;

    const portEl = document.getElementById(portId);
    const baudEl = document.getElementById(baudId);

    if (portEl) config.port = portEl.value;
    if (baudEl) config.baudrate = parseInt(baudEl.value);

    // 协议特定参数收集
    switch (type) {
        case 'dmx':
            config.peripheral_type = 'dmx512';
            config.mode = document.getElementById('dmx-input-mode')?.value || 'local';
            config.external_port = document.getElementById('dmx-external-port')?.value || '/dev/ttySWK4';
            config.external_baudrate = 115200;
            config.external_data_bit = 8;
            config.external_stop_bit = 1;
            config.external_enable = true;
            config.electron_version = 1;
            config.effect_interval = 3000;
            config.stop_handle = false;
            config.stop_material = false;
            config.dmx_info_show = isDmxInfoHintEnabled();
            config.baudrate = 250000;
            config.address = parseInt(document.getElementById('dmx-start-address')?.value || '1');
            config.master = 255;
            config.mappings = harvestDmxMappings();
            break;

        case 'rs232':
            config.learn_mode = document.getElementById('serial-learn-mode')?.checked || false;
            config.mappings = harvestCommandMappings('rs232');
            break;

        case 'udp':
        case 'tcp':
            commit工作区ToActiveTrigger(type);
            collectNetworkConfig(type, config);
            // 序列化墙板状态：每个触发对应一组功能链
            config.mappings = harvestNetworkTriggers(type);
            break;

        case 'rs485':
            config.learn_mode = document.getElementById('serial-learn-mode')?.checked || false;
            break;
    }

    try {
        const response = await apiAction('peripherals', 'set_config', {
                ...config
            });

        if (response && typeof response === 'object') {
            if (usesSharedSerialControls) {
                const learnResponse = await apiAction('peripherals', 'set_learn_mode', {
                        peripheral_type: type,
                        port: config.port || '',
                        baudrate: config.baudrate || 9600,
                        enabled: Boolean(config.learn_mode)
                    });
                if (learnResponse == null) {
                    throw new Error(learnResponse?.message || '学习监听状态同步失败');
                }
            }
            addToCommandLog(`保存 ${type.toUpperCase()} 配置`, 'success', `已同步引擎设置`);
            appendLog('SYSTEM', `${type.toUpperCase()} 配置已持久化至后端`);
        } else {
            throw new Error(response?.message || '引擎拒绝了配置请求');
        }
    } catch (error) {
        addToCommandLog(`保存 ${type.toUpperCase()} 配置失败`, 'error', error.message);
        appendLog('ERROR', `${type.toUpperCase()} 配置应用失败: ${error.message}`);
    }
}

function scheduleNetworkConfigAutosave(type) {
    if (type !== 'udp' && type !== 'tcp') return;
    if (networkAutosaveTimers[type]) {
        clearTimeout(networkAutosaveTimers[type]);
    }
    networkAutosaveTimers[type] = setTimeout(async () => {
        networkAutosaveTimers[type] = null;
        commit工作区ToActiveTrigger(type);
        await savePeripheralConfig(type);
    }, 350);
}

function autosaveLocalNetworkConfigIfMissed(data) {
    const type = String(data?.source || '').toLowerCase();
    if (type !== 'udp' && type !== 'tcp') return;
    const hex = data?.hex || '';
    if (!hex) return;
    const localTrigger = (networkTriggers[type] || [])
        .find(t => networkTriggerMatchesHex(getEffectiveNetworkTriggerValue(t), hex));
    if (!localTrigger) return;

    appendLog('SYSTEM', `[${type.toUpperCase()}] 本地存在触发 ${localTrigger.trigger}，正在同步到后端`);
    scheduleNetworkConfigAutosave(type);
}

/**
 * @brief 动态添加指令映射行 (RS232 专用，UDP/TCP 改用墙板布局)
 * 示例/字段：@param {string} type 'rs232'
 */
async function addCommandMappingRow(type) {
    if (type !== 'rs232') return null; // UDP/TCP 已切换为墙板布局，不再使用单行映射
    const container = document.getElementById('rs232-command-list');
    if (!container) return null;

    // 串口特有逻辑：如果功能列表未加载，先加载
    if (!window.availableFunctions) {
        await loadSerialFunctions();
    }

    const row = document.createElement('div');
    row.className = 'command-map-row';

    let functionOptions = '<option value="">-- 请选择功能 --</option>';
    if (window.availableFunctions && Array.isArray(window.availableFunctions)) {
        const groupedFunctions = {};
        window.availableFunctions.forEach(f => {
            const cat = f.category || '其他';
            if (!groupedFunctions[cat]) groupedFunctions[cat] = [];
            groupedFunctions[cat].push(f);
        });
        Object.keys(groupedFunctions).sort().forEach(cat => {
            functionOptions += `<optgroup label="${cat}">`;
            groupedFunctions[cat].forEach(f => {
                const id = f.id || f.action || '';
                const val = JSON.stringify({ id, action: f.action, code: f.code, name: f.name || id });
                functionOptions += `<option value="${val.replace(/"/g, '&quot;')}">${f.name || id}</option>`;
            });
            functionOptions += '</optgroup>';
        });
    }

    row.innerHTML = `
        <input type="text" class="form-control small serial-hex-input" placeholder="收到 Hex (拖拽)" style="flex: 1;" readonly>
        <select class="form-control small serial-function-select" style="flex: 2;">${functionOptions}</select>
        <button class="btn small transparent remove-row-btn" style="color: #f87171;">×</button>
    `;
    container.appendChild(row);
    row.querySelector('.remove-row-btn').addEventListener('click', () => row.remove());
    return row;
}

/**
 * @brief 收集 RS232 指令映射数据 (UDP/TCP 使用墙板状态另行收集)
 */
function harvestCommandMappings(type) {
    if (type !== 'rs232') return [];
    const container = document.getElementById('rs232-command-list');
    if (!container) return [];

    const mappings = [];
    container.querySelectorAll('.command-map-row').forEach(row => {
        const input = row.querySelector('input');
        const select = row.querySelector('select');
        if (!input || !select) return;

        const trigger = input.value.trim();
        if (!trigger) return;

        let action = select.value;
        if (action) {
            try { action = JSON.parse(action); } catch (e) {}
        }

        mappings.push({ trigger, action });
    });
    return mappings;
}

/**
 * @brief 收集网络协议相关的配置数据
 */
function collectNetworkConfig(type, config) {
    const bindAddr = document.getElementById(`${type}-bind-address`);
    const bindPort = document.getElementById(`${type}-bind-port`);
    const targetAddr = document.getElementById(`${type}-target-address`);
    const targetPort = document.getElementById(`${type}-target-port`);
    const pathEl = document.getElementById(`${type}-path`);
    const modeEl = document.getElementById(`${type}-mode`);
    const multicastEl = document.getElementById(`${type}-multicast-group`);

    if (bindAddr) config.bind_address = bindAddr.value;
    if (bindPort) config.bind_port = parseInt(bindPort.value);
    if (targetAddr) config.target_address = targetAddr.value;
    if (targetPort) config.target_port = parseInt(targetPort.value);
    if (pathEl) config.path = pathEl.value;
    if (modeEl) config.mode = modeEl.value;
    if (multicastEl !== null) config.multicast_group = multicastEl.value.trim();
}

/**
 * @brief 收集 DMX 映射数据
 */
function harvestDmxMappings() {
    const mappings = [];
    document.querySelectorAll('#dmx-mapping-container .dmx-channel-card').forEach((card, idx) => {
        const funcSelect = card.querySelector('select');
        const sliderEl = card.querySelector('.dmx-vertical-slider');
        const func = funcSelect ? funcSelect.value : 'none';
        const val = sliderEl ? sliderEl.value : 0;

        if (func !== 'none') {
            mappings.push({
                offset: idx,
                function: func,
                value: parseInt(val)
            });
        }
    });
    return mappings;
}

/**
 * @brief 发送测试指令
 */
async function sendPeripheralTest() {
    const activeItem = document.querySelector('.peripheral-item.active');
    const target = activeItem ? activeItem.getAttribute('data-peripheral') : 'dmx512';
    const commandEl = document.getElementById('test-command');

    if (!commandEl) return;

    const command = commandEl.value.trim();
    if (!command) {
        appendLog('ERROR', '请输入要发送的指令');
        return;
    }

    appendLog('SEND', `[${target.toUpperCase()}] -> ${command}`);

    try {
        const response = await apiAction('peripherals', 'send_test', {
                target: target,
                data: command
            });

        if (response && typeof response === 'object') {
            const result = Object.prototype.hasOwnProperty.call(response, 'reply')
                ? response.reply
                : 'OK';
            appendLog('RECV', `[${target.toUpperCase()}] <- ${JSON.stringify(result)}`);
        } else {
            throw new Error(response?.message || '设备未响应');
        }
    } catch (error) {
        appendLog('ERROR', `发送失败: ${error.message}`);
    }
}

/**
 * @brief 向测试日志添加记录
 */
function appendLog(type, message) {
    const logContainer = document.getElementById('peripheral-test-log');
    if (!logContainer) return;

    const entry = document.createElement('div');
    entry.className = 'log-entry';

    const time = new Date().toLocaleTimeString();

    let typeClass = '';
    switch (type) {
        case 'SEND': typeClass = 'log-type-send'; break;
        case 'RECV': typeClass = 'log-type-recv'; break;
        case 'ERROR': typeClass = 'log-type-error'; break;
    }

    entry.innerHTML = `
        <span class="log-time">[${time}]</span>
        <span class="${typeClass}">${type}:</span>
        <span class="log-msg">${message}</span>
    `;

    logContainer.appendChild(entry);
    logContainer.scrollTop = logContainer.scrollHeight;
}

/**
 * @brief 更新 DMX 模式 UI 显示
 */
function updateDmxModeUI(isManual) {
    const btn = document.getElementById('dmx-mode-toggle-btn');
    if (!btn) return;

    if (isManual) {
        btn.innerHTML = '🚧 模拟调试';
        btn.classList.add('is-manual');
        btn.style.borderColor = 'rgba(245, 158, 11, 0.4)';
        btn.style.color = '#f59e0b';
    } else {
        btn.innerHTML = '📡 实时数据';
        btn.classList.remove('is-manual');
        btn.style.borderColor = 'rgba(6, 182, 212, 0.3)';
        btn.style.color = '#06b6d4';
    }
}

// ============================================================
// UDP / TCP 功能列表（与串口共用数据源，独立渲染到各自容器）
// ============================================================

/**
 * @brief 初始化 UDP 或 TCP 外设的搜索、图层过滤、工作区交互
 * 示例/字段：@param {string} type 'udp' | 'tcp'
 */
function initNetworkPeripheralFeatures(type) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;

    // 搜索框（墙板布局：在 .functions-col 的 .simple-search-bar 里）
    const searchInput = section.querySelector('.functions-col .simple-search-bar input');
    if (searchInput) {
        searchInput.addEventListener('input', () => filterNetworkFunctions(type));
    }

    // 图层过滤按钮
    const layerBtns = section.querySelectorAll('.functions-col .layer-filter-btn');
    layerBtns.forEach(btn => {
        btn.addEventListener('click', () => {
            layerBtns.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            filterNetworkFunctions(type);
        });
    });

    // 触发列表搜索（左侧栏）
    const triggerSearch = document.getElementById(`${type}-trigger-search`);
    if (triggerSearch) {
        triggerSearch.addEventListener('input', () => filterNetworkTriggerList(type));
    }

    // 添加触发按钮
    const addTriggerBtn = document.getElementById(`add-${type}-trigger-btn`);
    addTriggerBtn?.addEventListener('click', async () => {
        await addNetworkTrigger(type);
    });

    // 清空触发按钮
    const clearBtn = document.getElementById(`${type}-clear-templates-btn`);
    clearBtn?.addEventListener('click', async () => {
        const confirmed = await showConfirm(`确定要清空所有 ${type.toUpperCase()} 触发指令吗？此操作不可恢复。`, '清空触发');
        if (!confirmed) return;
        clearNetworkTriggers(type);
        addToCommandLog('清空触发', 'success', `已清空 ${type.toUpperCase()} 触发指令`);
    });

    // 保存功能配置按钮 → 立即提交工作区 + 持久化整个外设配置
    const saveFnBtn = document.getElementById(`save-${type}-template-functions-btn`);
    saveFnBtn?.addEventListener('click', async () => {
        commit工作区ToActiveTrigger(type);
        await savePeripheralConfig(type);
    });
}

/**
 * @brief 过滤 UDP/TCP 功能列表（搜索 + 图层）
 */
function filterNetworkFunctions(type) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;

    const searchInput = section.querySelector('.functions-col .simple-search-bar input');
    const activeLayerBtn = section.querySelector('.functions-col .layer-filter-btn.active');
    const keyword = searchInput ? searchInput.value.trim().toLowerCase() : '';
    const selectedLayer = activeLayerBtn ? activeLayerBtn.dataset.layer : 'all';

    const buttons = section.querySelectorAll('.functions-col .func-item-btn');
    const categoryContainers = new Set();

    buttons.forEach(btn => {
        const text = btn.textContent.toLowerCase();
        const matchesKeyword = !keyword || text.includes(keyword);
        const matchesLayer = matchesFunctionLayer(btn, selectedLayer);
        btn.style.display = (matchesKeyword && matchesLayer) ? '' : 'none';
        const cat = btn.closest('.function-category');
        if (cat) categoryContainers.add(cat);
    });

    categoryContainers.forEach(cat => {
        const visible = Array.from(cat.querySelectorAll('.func-item-btn')).some(b => b.style.display !== 'none');
        cat.style.display = visible ? '' : 'none';
    });
}

/**
 * @brief 过滤 UDP/TCP 触发列表（搜索）
 */
function filterNetworkTriggerList(type) {
    const triggerSearch = document.getElementById(`${type}-trigger-search`);
    const keyword = triggerSearch ? triggerSearch.value.trim().toLowerCase() : '';
    const list = document.getElementById(`${type}-trigger-list`);
    if (!list) return;
    list.querySelectorAll('.template-item').forEach(item => {
        const code = (item.dataset.templateCode || '').toLowerCase();
        const name = (item.dataset.templateName || '').toLowerCase();
        const visible = !keyword || code.includes(keyword) || name.includes(keyword);
        item.style.display = visible ? '' : 'none';
    });
}

/**
 * @brief 加载 UDP/TCP 功能列表（与串口共用数据源）
 */
async function loadNetworkFunctions(type) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;
    const container = section.querySelector('.functions-col .function-buttons-grid');
    if (!container) return;

    try {
        const functions = await loadCommandsFromCommandList();
        const templates = await loadLayerTemplates();
        const playlists = await loadPlaylists();
        const allFunctions = [buildForwardFunctionEntry(), ...functions, ...templates, ...playlists];

        if (allFunctions.length > 0) {
            renderNetworkFunctions(type, allFunctions);
        } else {
            showEmptyState(container, 'settings', '暂无功能数据');
        }
    } catch (error) {
        showErrorState(container, '加载功能列表失败', error.message || '网络错误');
    }
}

/**
 * @brief 渲染 UDP/TCP 功能列表到对应容器
 */
function renderNetworkFunctions(type, functions) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;
    const container = section.querySelector('.functions-col .function-buttons-grid');
    if (!container) return;

    container.innerHTML = '';

    if (!Array.isArray(functions) || functions.length === 0) {
        showEmptyState(container, 'settings', '暂无功能数据');
        return;
    }

    // 按分类分组
    const grouped = {};
    functions.forEach(func => {
        const cat = func.category || '其他';
        if (!grouped[cat]) grouped[cat] = [];
        grouped[cat].push(func);
    });

    Object.keys(grouped).sort().forEach(category => {
        const catDiv = document.createElement('div');
        catDiv.className = 'function-category';
        catDiv.style.marginBottom = '16px';

        const title = document.createElement('div');
        title.className = 'function-category-title';
        title.textContent = category;
        title.style.cssText = 'font-size:12px;font-weight:600;color:#64748b;margin-bottom:8px;text-transform:uppercase;letter-spacing:0.05em;';
        catDiv.appendChild(title);

        const row = document.createElement('div');
        row.className = 'function-buttons-row';
        row.style.cssText = 'display:flex;flex-wrap:wrap;gap:6px;';

        grouped[category].forEach(func => {
            const btn = document.createElement('button');
            btn.className = 'func-item-btn';
            btn.textContent = func.name || func.action || '未知功能';
            btn.title = func.description || func.name || '';
            btn.dataset.functionId = func.id || func.action || '';
            btn.dataset.action = func.action || func.id || '';
            btn.dataset.code = func.code !== undefined ? func.code : '';
            btn.dataset.category = category;
            if (func.layerId) btn.dataset.layerId = String(func.layerId);
            // 携带模板/播放列表元数据，添加到工作区时可还原参数
            if (func.isTemplate) btn.dataset.isTemplate = 'true';
            if (func.isDefault) btn.dataset.isDefault = 'true';
            if (func.scene_name) btn.dataset.sceneName = func.scene_name;
            if (func.isPlaylist) btn.dataset.isPlaylist = 'true';
            if (func.playlistId) btn.dataset.playlistId = String(func.playlistId);
            if (func.playlistName) btn.dataset.playlistName = func.playlistName;
            if (func.targetLayerId) btn.dataset.targetLayerId = String(func.targetLayerId);

            // 点击功能按钮 → 加入到当前选中触发的工作区
            btn.addEventListener('click', () => addFunctionToNetwork工作区(type, btn));
            row.appendChild(btn);
        });

        catDiv.appendChild(row);
        container.appendChild(catDiv);
    });

    container.style.cssText = 'overflow-y:auto;overflow-x:hidden;flex:1;min-height:0;';
}

// ============================================================
//   UDP / TCP 触发墙板 —— 本地状态管理 + 工作区交互
// ============================================================

/**
 * @brief 把功能按钮添加到当前选中触发的工作区，并同步到本地状态
 */
function addFunctionToNetwork工作区(type, btn) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;
    const workspaceCanvas = section.querySelector('.workspace-canvas');
    if (!workspaceCanvas) return;

    // 必须先选中一个触发
    const activeCode = networkActiveTriggerCode[type];
    const trigger = activeCode ? networkTriggers[type].find(t => t.code === activeCode) : null;
    if (!trigger) {
        addToCommandLog('添加功能', 'warning', '请先在左侧选择或添加一个触发');
        return;
    }

    const functionId = btn.dataset.functionId || '';
    const functionName = btn.textContent.trim();

    // 防重复
    const existing = functionId !== FORWARD_FUNCTION_ID
        ? workspaceCanvas.querySelector(`.function-chip[data-function-id="${CSS.escape(functionId)}"]`)
        : null;
    if (existing) {
        addToCommandLog('添加功能', 'warning', `"${functionName}" 已存在`);
        return;
    }

    workspaceCanvas.querySelector('.workspace-empty-state')?.remove();

    const chip = document.createElement('div');
    chip.className = 'function-chip';
    chip.dataset.functionId = functionId;
    chip.dataset.action = btn.dataset.action || '';
    chip.dataset.functionCode = btn.dataset.code || '';
    if (btn.dataset.layerId) chip.dataset.layerId = btn.dataset.layerId;
    if (btn.dataset.isTemplate) chip.dataset.isTemplate = btn.dataset.isTemplate;
    if (btn.dataset.isDefault) chip.dataset.isDefault = btn.dataset.isDefault;
    if (btn.dataset.sceneName) chip.dataset.sceneName = btn.dataset.sceneName;
    if (btn.dataset.isPlaylist) chip.dataset.isPlaylist = btn.dataset.isPlaylist;
    if (btn.dataset.playlistId) chip.dataset.playlistId = btn.dataset.playlistId;
    if (btn.dataset.playlistName) chip.dataset.playlistName = btn.dataset.playlistName;
    if (btn.dataset.targetLayerId) chip.dataset.targetLayerId = btn.dataset.targetLayerId;

    chip.innerHTML = `
        <div class="function-chip-main">
            <span>${functionName}</span>
            ${functionId === FORWARD_FUNCTION_ID ? '<small class="function-chip-subtitle"></small>' : ''}
        </div>
        <div class="chip-actions">
            <button class="move-btn move-up" title="上移">▲</button>
            <button class="move-btn move-down" title="下移">▼</button>
            <button class="close-btn" title="删除">×</button>
        </div>
    `;
    workspaceCanvas.appendChild(chip);

    bindNetworkChipActions(type, chip);

    if (functionId === FORWARD_FUNCTION_ID) {
        showForwardConfigDialog({
            default_source_hex: trigger.trigger || ''
        }).then(config => {
            if (!config) {
                chip.remove();
                commit工作区ToActiveTrigger(type);
                if (!workspaceCanvas.querySelector('.function-chip')) {
                    const empty = document.createElement('div');
                    empty.className = 'workspace-empty-state';
                    empty.textContent = '从右侧选择功能，点击即可添加到此处';
                    workspaceCanvas.appendChild(empty);
                }
                return;
            }
            applyForwardConfigToChip(chip, config);
            commit工作区ToActiveTrigger(type);
            scheduleNetworkConfigAutosave(type);
        });
    }

    // 同步到本地状态
    commit工作区ToActiveTrigger(type);

    const saveBtn = document.getElementById(`save-${type}-template-functions-btn`);
    if (saveBtn) saveBtn.classList.remove('hidden');

    addToCommandLog('添加功能', 'success', `已添加 ${functionName}`);
    if (functionId !== FORWARD_FUNCTION_ID) {
        scheduleNetworkConfigAutosave(type);
    }
}

/**
 * @brief 给触发工作区的功能 chip 绑定 删除/上移/下移 事件，并在变更后同步到本地状态
 */
function bindNetworkChipActions(type, chip) {
    const workspaceCanvas = chip.closest('.workspace-canvas');
    chip.querySelector('.close-btn')?.addEventListener('click', () => {
        chip.remove();
        commit工作区ToActiveTrigger(type);
        scheduleNetworkConfigAutosave(type);
        if (workspaceCanvas && !workspaceCanvas.querySelector('.function-chip')) {
            const empty = document.createElement('div');
            empty.className = 'workspace-empty-state';
            empty.textContent = '从右侧选择功能，点击即可添加到此处';
            workspaceCanvas.appendChild(empty);
        }
    });
    chip.querySelector('.move-up')?.addEventListener('click', (e) => {
        e.stopPropagation();
        moveFunctionChip(chip, 'up');
        commit工作区ToActiveTrigger(type);
        scheduleNetworkConfigAutosave(type);
    });
    chip.querySelector('.move-down')?.addEventListener('click', (e) => {
        e.stopPropagation();
        moveFunctionChip(chip, 'down');
        commit工作区ToActiveTrigger(type);
        scheduleNetworkConfigAutosave(type);
    });
    if (isForwardChip(chip)) {
        chip.addEventListener('dblclick', async () => {
            const activeCode = networkActiveTriggerCode[type];
            const trigger = activeCode ? networkTriggers[type].find(t => t.code === activeCode) : null;
            const config = await showForwardConfigDialog({
                ...getForwardConfigFromChip(chip),
                default_source_hex: trigger?.trigger || ''
            });
            if (!config) return;
            applyForwardConfigToChip(chip, config);
            commit工作区ToActiveTrigger(type);
            scheduleNetworkConfigAutosave(type);
        });
    }
}

/**
 * @brief 把当前 chip 提取为可序列化的 function 数据
 */
function chipToFunctionData(chip) {
    const functionId = chip.dataset.functionId || '';
    const functionName = chip.querySelector('span')?.textContent.trim() || '';
    let action = chip.dataset.action || '';
    let functionCode = parseInt(chip.dataset.functionCode || '0', 10) || 0;
    if (!functionId || !functionName) return null;

    if (chip.dataset.isPlaylist) {
        action = 'play';
        functionCode = 0x02;
    }

    const fd = { function_id: functionId, function_name: functionName, action, function_code: functionCode };
    if (chip.dataset.layerId) {
        const lid = parseInt(chip.dataset.layerId, 10);
        if (!isNaN(lid)) fd.layerId = lid;
    }
        if (chip.dataset.isTemplate) {
            fd.isTemplate = true;
            if (chip.dataset.isDefault) fd.isDefault = true;
            else if (chip.dataset.sceneName) {
                fd.scene_name = chip.dataset.sceneName;
            }
        }
    if (chip.dataset.isPlaylist) {
        fd.isPlaylist = true;
        fd.playlistId = chip.dataset.playlistId || '';
        fd.playlistName = chip.dataset.playlistName || '';
        const playlistLayerId = parseInt(
            chip.dataset.layerId || chip.dataset.targetLayerId || '1',
            10
        ) || 1;
        fd.layerId = playlistLayerId;
        fd.targetLayerId = playlistLayerId;
    }
    if (isForwardChip(chip)) {
        Object.assign(fd, getForwardConfigFromChip(chip));
    }
    return fd;
}

/**
 * @brief 把当前工作区的 chip 序列同步到当前选中触发的 functions 数组
 */
function commit工作区ToActiveTrigger(type) {
    const activeCode = networkActiveTriggerCode[type];
    if (!activeCode) return;
    const trigger = networkTriggers[type].find(t => t.code === activeCode);
    if (!trigger) return;

    const section = document.getElementById(`config-${type}`);
    const workspaceCanvas = section?.querySelector('.workspace-canvas');
    if (!workspaceCanvas) return;

    const chips = workspaceCanvas.querySelectorAll('.function-chip');
    const seen = new Set();
    const functions = [];
    chips.forEach(chip => {
        const fd = chipToFunctionData(chip);
        if (!fd) return;
        const identityKey = fd.function_id === FORWARD_FUNCTION_ID
            ? forwardFunctionIdentity(fd)
            : fd.function_id;
        if (seen.has(identityKey)) return;
        seen.add(identityKey);
        functions.push(fd);
    });
    trigger.functions = functions;
}

/**
 * @brief 添加一条触发指令（手动新增模板）
 */
function ensureNetworkTriggerDialog() {
    let dialog = document.getElementById('network-trigger-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'network-trigger-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content">
                <h3>添加触发</h3>
                <div class="dialog-body">
                    <div style="display: flex; flex-direction: column; gap: 8px; margin-bottom: 12px;">
                        <label for="network-trigger-input" style="color: var(--color-text-main); font-size: 13px;">触发命令</label>
                        <input id="network-trigger-input" class="form-control" type="text" placeholder="外部设备发送的内容">
                    </div>
                    <div style="display: flex; flex-direction: column; gap: 8px;">
                        <label for="network-trigger-name-input" style="color: var(--color-text-main); font-size: 13px;">名称</label>
                        <input id="network-trigger-name-input" class="form-control" type="text" placeholder="可选，不填则使用触发命令">
                    </div>
                </div>
                <div class="dialog-footer">
                    <button id="network-trigger-cancel-btn" class="btn">取消</button>
                    <button id="network-trigger-ok-btn" class="btn primary">添加</button>
                </div>
            </div>
        `;
        document.body.appendChild(dialog);
    }
    return dialog;
}

function showNetworkTriggerDialog(type) {
    return new Promise((resolve) => {
        const dialog = ensureNetworkTriggerDialog();
        const title = dialog.querySelector('h3');
        const triggerInput = dialog.querySelector('#network-trigger-input');
        const nameInput = dialog.querySelector('#network-trigger-name-input');
        const okBtn = dialog.querySelector('#network-trigger-ok-btn');
        const cancelBtn = dialog.querySelector('#network-trigger-cancel-btn');
        let settled = false;

        title.textContent = `添加 ${type.toUpperCase()} 触发`;
        triggerInput.value = '';
        nameInput.value = '';
        nameInput.placeholder = '可选，不填则使用触发命令';

        const finish = (value) => {
            if (settled) return;
            settled = true;
            dialog.classList.remove('active');
            resolve(value);
        };

        okBtn.onclick = () => {
            const rawTrigger = (triggerInput.value || '').trim();
            if (!rawTrigger) {
                addToCommandLog('添加触发', 'warning', '触发命令不能为空');
                triggerInput.focus();
                return;
            }
            const normalized = normalizeHexInput(rawTrigger);
            const triggerStr = normalized.ok ? normalized.value : rawTrigger;
            const exists = networkTriggers[type].some(t => t.trigger === triggerStr);
            if (exists) {
                addToCommandLog('添加触发', 'warning', `触发 "${triggerStr}" 已存在`);
                triggerInput.focus();
                triggerInput.select();
                return;
            }
            const name = (nameInput.value || '').trim() || triggerStr;
            finish({ trigger: triggerStr, name });
        };

        cancelBtn.onclick = () => finish(null);
        dialog.onclick = (event) => {
            if (event.target === dialog) {
                finish(null);
            }
        };
        triggerInput.oninput = () => {
            const rawValue = (triggerInput.value || '').trim();
            const normalized = normalizeHexInput(rawValue);
            const value = normalized.ok ? normalized.value : rawValue;
            nameInput.placeholder = value ? `可选，默认：${value}` : '可选，不填则使用触发命令';
        };
        triggerInput.onkeydown = nameInput.onkeydown = (event) => {
            if (event.key === 'Enter') {
                okBtn.click();
            } else if (event.key === 'Escape') {
                cancelBtn.click();
            }
        };

        dialog.classList.add('active');
        setTimeout(() => triggerInput.focus(), 0);
    });
}

async function addNetworkTrigger(type) {
    const input = await showNetworkTriggerDialog(type);
    if (!input) return;

    const triggerStr = (input.trigger || '').trim();
    if (!triggerStr) {
        addToCommandLog('添加触发', 'warning', '触发字符串不能为空');
        return;
    }

    // 防重复
    const exists = networkTriggers[type].some(t => t.trigger === triggerStr);
    if (exists) {
        addToCommandLog('添加触发', 'warning', `触发 "${triggerStr}" 已存在`);
        return;
    }

    const name = (input.name || '').trim() || triggerStr;

    const entry = {
        code: triggerStr,        // 与触发字符串相同，作为本地稳定 ID
        trigger: triggerStr,
        name,
        functions: []
    };
    networkTriggers[type].push(entry);
    networkActiveTriggerCode[type] = entry.code;

    renderNetworkTriggerList(type);
    addToCommandLog('添加触发', 'success', `已添加: ${triggerStr}`);
    scheduleNetworkConfigAutosave(type);
}

/**
 * @brief 清空所有触发
 */
function clearNetworkTriggers(type) {
    networkTriggers[type] = [];
    networkActiveTriggerCode[type] = '';
    renderNetworkTriggerList(type);
    scheduleNetworkConfigAutosave(type);
}

/**
 * @brief 重命名触发
 */
async function renameNetworkTrigger(type, code) {
    const trigger = networkTriggers[type].find(t => t.code === code);
    if (!trigger) return;
    const newNameRaw = await showPrompt(`修改名称（${trigger.trigger}）`, '重命名', trigger.name || '');
    if (newNameRaw === null) return;
    const newName = (newNameRaw || '').trim();
    if (!newName) {
        addToCommandLog('重命名', 'warning', '名称不能为空');
        return;
    }
    trigger.name = newName;
    renderNetworkTriggerList(type);
    addToCommandLog('重命名', 'success', `已更新: ${newName}`);
    scheduleNetworkConfigAutosave(type);
}

/**
 * @brief 删除触发
 */
async function deleteNetworkTrigger(type, code) {
    const trigger = networkTriggers[type].find(t => t.code === code);
    if (!trigger) return;
    const confirmed = await showConfirm(`确定要删除触发 "${trigger.name || trigger.trigger}" 吗？`);
    if (!confirmed) return;
    networkTriggers[type] = networkTriggers[type].filter(t => t.code !== code);
    if (networkActiveTriggerCode[type] === code) {
        networkActiveTriggerCode[type] = '';
    }
    renderNetworkTriggerList(type);
    addToCommandLog('删除触发', 'success', `已删除: ${trigger.trigger}`);
    scheduleNetworkConfigAutosave(type);
}

/**
 * @brief 序列化本地触发列表为唯一 mappings 格式（保存到后端）
 */
function harvestNetworkTriggers(type) {
    return networkTriggers[type].map(t => {
        const triggerValue = getEffectiveNetworkTriggerValue(t);
        if (!triggerValue) return null;
        return {
            trigger: triggerValue,
            name: t.name || triggerValue,
            functions: Array.isArray(t.functions) ? t.functions : []
        };
    }).filter(Boolean);
}

/**
 * @brief 按唯一 { trigger, name, functions } 格式反序列化后端 mappings
 */
function deserializeNetworkTriggers(type, mappings) {
    networkTriggers[type] = [];
    networkActiveTriggerCode[type] = '';
    if (!Array.isArray(mappings)) return;

    mappings.forEach(m => {
        if (!m || !Array.isArray(m.functions)) return;
        const triggerStr = getEffectiveNetworkTriggerValue(m);
        if (!triggerStr) return;

        networkTriggers[type].push({
            code: triggerStr,
            trigger: triggerStr,
            name: m.name || triggerStr,
            functions: m.functions
        });
    });

    if (networkTriggers[type].length > 0) {
        networkActiveTriggerCode[type] = networkTriggers[type][0].code;
    }
}

/**
 * @brief 渲染触发列表（左侧栏） + 同步右上角 badge + 工作区
 */
function renderNetworkTriggerList(type) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;
    const list = document.getElementById(`${type}-trigger-list`);
    if (!list) return;

    const triggers = networkTriggers[type] || [];
    list.innerHTML = '';

    if (triggers.length === 0) {
        showEmptyState(list, 'clipboard', '暂无触发指令，点击"+ 添加触发"创建');
        // 工作区清空
        const workspaceCanvas = section.querySelector('.workspace-canvas');
        if (workspaceCanvas) {
            workspaceCanvas.innerHTML = '';
            const empty = document.createElement('div');
            empty.className = 'workspace-empty-state';
            empty.textContent = '从右侧选择功能，点击即可添加到此处';
            workspaceCanvas.appendChild(empty);
        }
        const badge = section.querySelector('.workspace-header .badge-tag');
        if (badge) badge.textContent = '未选择触发';
        const saveBtn = document.getElementById(`save-${type}-template-functions-btn`);
        if (saveBtn) saveBtn.classList.add('hidden');
        return;
    }

    // 如果没有有效的 active，则默认选中第一项
    let activeCode = networkActiveTriggerCode[type];
    if (!activeCode || !triggers.some(t => t.code === activeCode)) {
        activeCode = triggers[0].code;
        networkActiveTriggerCode[type] = activeCode;
    }

    triggers.forEach(t => {
        const item = document.createElement('div');
        item.className = 'template-item';
        item.dataset.templateCode = t.code;
        item.dataset.templateName = t.name || t.trigger;

        if (t.code === activeCode) item.classList.add('bg-active');

        const display = (t.trigger || '').toUpperCase();
        item.innerHTML = `
            <span class="template-cell template-cell-code" title="${t.trigger}">${display}</span>
            <span class="template-cell template-cell-name">
                <span class="template-name-text">${t.name || t.trigger}</span>
                <button type="button" class="template-rename-btn" title="修改名称">🖊</button>
            </span>
            <span class="template-cell template-cell-actions">
                <span class="template-delete-btn">删除</span>
            </span>
        `;

        item.addEventListener('click', (e) => {
            if (e.target.closest('.template-rename-btn, .template-delete-btn')) return;
            // 切换前先把当前工作区状态写回上一个触发
            commit工作区ToActiveTrigger(type);
            networkActiveTriggerCode[type] = t.code;
            section.querySelectorAll('.template-item').forEach(i => i.classList.remove('bg-active'));
            item.classList.add('bg-active');
            const badge = section.querySelector('.workspace-header .badge-tag');
            if (badge) badge.textContent = t.name || t.trigger;
            renderActiveNetworkTrigger工作区(type);
        });

        item.querySelector('.template-rename-btn')?.addEventListener('click', (e) => {
            e.stopPropagation();
            renameNetworkTrigger(type, t.code);
        });
        item.querySelector('.template-delete-btn')?.addEventListener('click', (e) => {
            e.stopPropagation();
            deleteNetworkTrigger(type, t.code);
        });

        list.appendChild(item);
    });

    // 同步右上角 badge + 工作区
    const activeTrigger = triggers.find(t => t.code === activeCode);
    const badge = section.querySelector('.workspace-header .badge-tag');
    if (badge && activeTrigger) badge.textContent = activeTrigger.name || activeTrigger.trigger;
    renderActiveNetworkTrigger工作区(type);
}

/**
 * @brief 把当前选中触发的 functions 渲染到工作区
 */
function renderActiveNetworkTrigger工作区(type) {
    const section = document.getElementById(`config-${type}`);
    if (!section) return;
    const workspaceCanvas = section.querySelector('.workspace-canvas');
    if (!workspaceCanvas) return;

    workspaceCanvas.innerHTML = '';
    const activeCode = networkActiveTriggerCode[type];
    const trigger = activeCode ? networkTriggers[type].find(t => t.code === activeCode) : null;
    const saveBtn = document.getElementById(`save-${type}-template-functions-btn`);

    if (!trigger) {
        const empty = document.createElement('div');
        empty.className = 'workspace-empty-state';
        empty.textContent = '请先在左侧选择或添加触发';
        workspaceCanvas.appendChild(empty);
        if (saveBtn) saveBtn.classList.add('hidden');
        return;
    }

    if (saveBtn) saveBtn.classList.remove('hidden');

    if (!Array.isArray(trigger.functions) || trigger.functions.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'workspace-empty-state';
        empty.textContent = '从右侧选择功能，点击即可添加到此处';
        workspaceCanvas.appendChild(empty);
        return;
    }

    trigger.functions.forEach(func => {
        const chip = document.createElement('div');
        chip.className = 'function-chip';
        chip.dataset.functionId = func.function_id || '';
        chip.dataset.action = func.action || '';
        chip.dataset.functionCode = func.function_code || '';
        if (isForwardFunctionLike(func)) {
            chip.dataset.functionId = FORWARD_FUNCTION_ID;
            chip.dataset.action = 'forward_payload';
        }
        if (func.layerId) chip.dataset.layerId = String(func.layerId);
        if (func.isTemplate) chip.dataset.isTemplate = 'true';
        if (func.isDefault) chip.dataset.isDefault = 'true';
        if (func.scene_name) chip.dataset.sceneName = func.scene_name;
        if (func.isPlaylist) chip.dataset.isPlaylist = 'true';
        if (func.playlistId) chip.dataset.playlistId = String(func.playlistId);
        if (func.playlistName) chip.dataset.playlistName = func.playlistName;
        if (func.targetLayerId) chip.dataset.targetLayerId = String(func.targetLayerId);
        if (func.layerId || func.targetLayerId) {
            chip.dataset.layerId = String(func.layerId || func.targetLayerId);
        }

        chip.innerHTML = `
            <div class="function-chip-main">
                <span>${func.function_name || ''}</span>
                ${isForwardFunctionLike(func) ? '<small class="function-chip-subtitle"></small>' : ''}
            </div>
            <div class="chip-actions">
                <button class="move-btn move-up" title="上移">▲</button>
                <button class="move-btn move-down" title="下移">▼</button>
                <button class="close-btn" title="删除">×</button>
            </div>
        `;
        if (isForwardFunctionLike(func)) {
            applyForwardConfigToChip(chip, func);
        }
        workspaceCanvas.appendChild(chip);
        bindNetworkChipActions(type, chip);
    });
}

/**
 * @brief 兼容入口：被 loadPeripheralConfigs 调用，渲染当前类型的触发列表
 */
async function loadNetworkTemplates(type) {
    renderNetworkTriggerList(type);
}
