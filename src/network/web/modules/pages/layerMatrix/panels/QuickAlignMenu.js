// 画布右键"快速选择"菜单
// 对选中图层做快速对齐 / 1-N 分位定位。完全自治：仅依赖传入的 layer / canvasConfig，
// 由调用方在 onApply 回调里处理 drawCanvas / updatePropertyInputs / syncLayerToServer。

const MENU_ID = 'layer-quick-align-menu';

// 菜单项定义：
//   kind: 'pos'  → 只改位置；axis='x' 改 x，'y' 改 y。
//                  anchor: 'start'=紧贴左/上, 'end'=紧贴右/下, 'center'=居中。
//   kind: 'size' → 只改尺寸；axis='x' 改宽度，'y' 改高度。
//                  newW = W * fraction（高度不变）；newH = H * fraction（宽度不变）。
//                  此类操作不改位置，仅夹紧避免越界（如有必要再回缩 x/y）。
const ITEMS = [
    { label: '左对齐', kind: 'pos',  axis: 'x', anchor: 'start' },
    { label: '右对齐', kind: 'pos',  axis: 'x', anchor: 'end' },
    { label: '居中',   kind: 'pos',  axis: 'x', anchor: 'center' },
    { label: '上对齐', kind: 'pos',  axis: 'y', anchor: 'start' },
    { label: '下对齐', kind: 'pos',  axis: 'y', anchor: 'end' },
    { label: '1/2 (左右)', kind: 'size', axis: 'x', fraction: 1 / 2 },
    { label: '1/3 (左右)', kind: 'size', axis: 'x', fraction: 1 / 3 },
    { label: '1/4 (左右)', kind: 'size', axis: 'x', fraction: 1 / 4 },
    { label: '2/3 (左右)', kind: 'size', axis: 'x', fraction: 2 / 3 },
    { label: '2/4 (左右)', kind: 'size', axis: 'x', fraction: 2 / 4 },
    { label: '3/4 (左右)', kind: 'size', axis: 'x', fraction: 3 / 4 },
    { label: '1/2 (上下)', kind: 'size', axis: 'y', fraction: 1 / 2 },
    { label: '1/3 (上下)', kind: 'size', axis: 'y', fraction: 1 / 3 },
    { label: '1/4 (上下)', kind: 'size', axis: 'y', fraction: 1 / 4 },
    { label: '2/3 (上下)', kind: 'size', axis: 'y', fraction: 2 / 3 },
    { label: '2/4 (上下)', kind: 'size', axis: 'y', fraction: 2 / 4 },
    { label: '3/4 (上下)', kind: 'size', axis: 'y', fraction: 3 / 4 }
];

/**
 * 计算应用某菜单项后图层的新 position / size。
 * 不修改 layer 本体，返回 { position:{x,y}, size:{宽度,高度} }（系统坐标，整数）。
 *
 *  - kind='pos'  : 只改对应轴的坐标；size 原样返回。
 *  - kind='size' : 只改对应轴的尺寸 = 画布尺寸 × fraction；position 保持，
 *                  最后统一做一次"贴边夹紧"，避免越界。
 */
export function computeQuickAlign(layer, canvasConfig, item) {
    const W = canvasConfig.width || 0;
    const H = canvasConfig.height || 0;
    let w = (layer.size && layer.size.width) || 0;
    let h = (layer.size && layer.size.height) || 0;
    let x = (layer.position && layer.position.x) || 0;
    let y = (layer.position && layer.position.y) || 0;

    if (item.kind === 'size') {
        if (item.axis === 'x') {
            w = Math.max(1, Math.round(W * item.fraction));
        } else {
            h = Math.max(1, Math.round(H * item.fraction));
        }
    } else { // 说明：'pos'
        if (item.axis === 'x') {
            if (item.anchor === 'start')       x = 0;
            else if (item.anchor === 'end')    x = W - w;
            else /* 居中 */                  x = Math.round((W - w) / 2);
        } else {
            if (item.anchor === 'start')       y = 0;
            else if (item.anchor === 'end')    y = H - h;
            else /* 居中 */                  y = Math.round((H - h) / 2);
        }
    }

    // 夹紧到画布范围内（避免负值或越界；尺寸变化后可能需要回缩位置）
    if (W > 0) x = Math.max(0, Math.min(x, Math.max(0, W - w)));
    if (H > 0) y = Math.max(0, Math.min(y, Math.max(0, H - h)));

    return {
        position: { x: Math.round(x), y: Math.round(y) },
        size:     { width: Math.round(w), height: Math.round(h) }
    };
}

// 外部关闭事件集合：在触摸屏 / pointer-only 场景下，浏览器可能不再派发
// mousedown，只发 pointerdown / touchstart，所以全部都要听，并放在捕获阶段
// 以早于 canvas 自身的 pointerdown 处理。
const OUTSIDE_EVENTS = ['pointerdown', 'mousedown', 'touchstart', 'wheel'];
let outsideHandler = null;
let escHandler = null;
let windowBlurHandler = null;

/**
 * 关闭菜单（无副作用，多次调用安全）
 */
export function hideQuickAlignMenu() {
    const el = document.getElementById(MENU_ID);
    if (el && el.parentNode) el.parentNode.removeChild(el);
    if (outsideHandler) {
        OUTSIDE_EVENTS.forEach(ev =>
            document.removeEventListener(ev, outsideHandler, true)
        );
        outsideHandler = null;
    }
    if (escHandler) {
        document.removeEventListener('keydown', escHandler, true);
        escHandler = null;
    }
    if (windowBlurHandler) {
        window.removeEventListener('blur', windowBlurHandler);
        window.removeEventListener('resize', windowBlurHandler);
        windowBlurHandler = null;
    }
}

/**
 * 显示菜单
 * 示例/字段：@param {Object} cfg
 *   clientX, clientY  鼠标视口坐标（用于定位菜单）
 *   layer             目标图层（不会被本模块修改）
 *   canvasConfig      画布配置（提供 宽度/高度）
 *   onApply           function(newPosition, item) — 用户点选后回调，由调用方落地
 */
export function showQuickAlignMenu(cfg) {
    hideQuickAlignMenu(); // 防止叠加

    const { clientX, clientY, layer, canvasConfig, onApply } = cfg;
    if (!layer || !canvasConfig || !canvasConfig.width || !canvasConfig.height) return;

    const menu = document.createElement('div');
    menu.id = MENU_ID;
    menu.className = 'layer-quick-align-menu';
    Object.assign(menu.style, {
        position: 'fixed',
        zIndex: '99999',
        minWidth: '120px',
        padding: '4px 0',
        background: '#252526',
        color: '#e8e8e8',
        border: '1px solid #3c3c3c',
        borderRadius: '4px',
        boxShadow: '0 4px 14px rgba(0,0,0,0.45)',
        font: '13px/1.4 system-ui, "Microsoft YaHei", sans-serif',
        userSelect: 'none'
    });

    const header = document.createElement('div');
    header.textContent = '快速选择';
    Object.assign(header.style, {
        padding: '6px 14px 6px',
        color: '#9da0a4',
        fontSize: '12px',
        borderBottom: '1px solid #3c3c3c',
        marginBottom: '4px'
    });
    menu.appendChild(header);

    ITEMS.forEach(item => {
        const row = document.createElement('div');
        row.textContent = item.label;
        Object.assign(row.style, {
            padding: '5px 14px',
            cursor: 'pointer',
            whiteSpace: 'nowrap'
        });
        row.addEventListener('mouseenter', () => { row.style.background = '#094771'; });
        row.addEventListener('mouseleave', () => { row.style.background = ''; });
        row.addEventListener('click', (ev) => {
            ev.stopPropagation();
            try {
                const newPos = computeQuickAlign(layer, canvasConfig, item);
                if (typeof onApply === 'function') onApply(newPos, item);
            } finally {
                hideQuickAlignMenu();
            }
        });
        menu.appendChild(row);
    });

    document.body.appendChild(menu);

    // 视口边界回弹
    const rect = menu.getBoundingClientRect();
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    let left = clientX;
    let top = clientY;
    if (left + rect.width > vw - 4) left = Math.max(4, vw - rect.width - 4);
    if (top + rect.height > vh - 4) top = Math.max(4, vh - rect.height - 4);
    menu.style.left = left + 'px';
    menu.style.top = top + 'px';

    // 点击菜单外 / 按 Esc / 失焦 / 视口变化 关闭
    outsideHandler = (e) => {
        // touchstart 的 target 是触摸目标；pointerdown / mousedown 的 target 同理
        const t = e.target;
        if (t && menu.contains(t)) return;
        hideQuickAlignMenu();
    };
    escHandler = (e) => {
        if (e.key === 'Escape') hideQuickAlignMenu();
    };
    windowBlurHandler = () => hideQuickAlignMenu();

    // 用 capture 确保比 canvas 自身的 pointerdown / 自身派发的 mousedown 都早触发；
    // 同时监听多种事件以兼容触摸屏 / pointer-only 设备（部分浏览器不再补发 mousedown）
    OUTSIDE_EVENTS.forEach(ev =>
        document.addEventListener(ev, outsideHandler, true)
    );
    document.addEventListener('keydown', escHandler, true);
    window.addEventListener('blur', windowBlurHandler);
    window.addEventListener('resize', windowBlurHandler);
}
