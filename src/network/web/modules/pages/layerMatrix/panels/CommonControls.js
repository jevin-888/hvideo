/** 主图层、切片与移动端共用的数值控件。 */
export function createNumControl(id, val, min = 0, max = null, step = 1, options = {}) {
    const wrapperClass = options.wrapperClass || '';
    const inputClass = options.inputClass || '';
    const disabled = options.disabled ? 'disabled' : '';
    const maxAttribute = max !== null ? `max="${max}"` : '';
    return `<div class="number-input-wrapper horizontal layer-param-input ${wrapperClass}">
        <button type="button" class="number-btn minus" data-target="${id}" aria-label="−" ${disabled}>-</button>
        <input type="number" id="${id}" min="${min}" ${maxAttribute} step="${step}" value="${val}" data-default="${val}" class="form-control number-input ${inputClass}" ${disabled}>
        <button type="button" class="number-btn plus" data-target="${id}" aria-label="+" ${disabled}>+</button>
        <button type="button" class="number-btn number-reset" data-target="${id}" title="恢复默认" aria-label="恢复默认" ${disabled}>↻</button>
    </div>`;
}

export const createNumInput = (id, label, val, min = 0, max = null, step = 1, colSpan = 1, options = {}) => `
    <div ${options.itemId ? `id="${options.itemId}"` : ''} class="setting-item ${options.itemClass || ''}" style="grid-column: span ${colSpan}; min-width: 0;${options.display !== undefined ? ` display: ${options.display};` : ''}">
        <label for="${id}">${label}:</label>
        ${createNumControl(id, val, min, max, step, options)}
    </div>
`;

const numberBindings = new WeakMap();

function numberValue(value, fallback) {
    const parsed = Number.parseFloat(value);
    return Number.isFinite(parsed) ? parsed : fallback;
}

function limitsFor(input) {
    const step = numberValue(input.step, 1);
    const [, decimals = '', exponent = '0'] = String(step).match(/(?:\.(\d+))?(?:e([+-]?\d+))?$/i) || [];
    return {
        min: numberValue(input.min, -Infinity),
        max: numberValue(input.max, Infinity),
        step: step > 0 ? step : 1,
        precision: Math.min(12, Math.max(0, decimals.length - Number(exponent)))
    };
}

function boundedValue(value, limits) {
    return Math.max(limits.min, Math.min(limits.max, Number(value.toFixed(limits.precision))));
}

function renderNumberValue(input) {
    const track = input.closest('.number-input-wrapper');
    if (!track) return;
    const limits = limitsFor(input);
    const { min, max } = limits;
    const value = numberValue(input.value, 0);
    let ratio;
    if (Number.isFinite(min) && Number.isFinite(max) && max > min) {
        ratio = (value - min) / (max - min);
    } else {
        const scale = Math.max(1, limits.step * 100);
        ratio = 0.5 + Math.atan(value / scale) / Math.PI;
    }
    track.style.setProperty('--numeric-drag-level', `${Math.max(0, Math.min(1, ratio)) * 100}%`);
}

function publishValue(input, binding, value, action, commit = true) {
    input.value = String(value);
    renderNumberValue(input);
    binding.onChange?.(input.id, value, action);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    if (commit) input.dispatchEvent(new Event('change', { bubbles: true }));
}

function bindNumberDrag(track, binding) {
    if (!track) return;
    let press = null;
    let suppressClick = false;

    track.addEventListener('pointerdown', event => {
        const input = track.querySelector('input[type="number"]');
        binding.input = input;
        if (event.button !== 0 || !input || input.disabled || input.readOnly || event.target.closest('button')) return;
        press = { pointerId: event.pointerId, x: event.clientX, y: event.clientY, value: numberValue(input.value, 0), dragging: false, changed: false };
    });
    track.addEventListener('pointermove', event => {
        if (!press || event.pointerId !== press.pointerId) return;
        const input = track.querySelector('input[type="number"]');
        binding.input = input;
        if (!input) return;
        const dx = event.clientX - press.x;
        const dy = event.clientY - press.y;
        if (!press.dragging) {
            if (Math.abs(dy) > Math.abs(dx) && Math.abs(dy) > 6) {
                press = null;
                return;
            }
            if (Math.abs(dx) < 6 || Math.abs(dx) <= Math.abs(dy)) return;
            press.dragging = true;
            suppressClick = true;
            input.blur();
            track.classList.add('is-horizontal-dragging');
            track.setPointerCapture(event.pointerId);
        }
        const limits = limitsFor(input);
        const width = Math.max(1, input.getBoundingClientRect().width);
        const span = limits.max - limits.min;
        const delta = Number.isFinite(span) && span > 0 ? dx * span / width : dx * limits.step / 4;
        const value = boundedValue(press.value + Math.round(delta / limits.step) * limits.step, limits);
        if (value !== numberValue(input.value, 0)) {
            press.changed = true;
            publishValue(input, binding, value, 'drag', false);
        }
        event.preventDefault();
    });
    const finish = event => {
        if (!press || event.pointerId !== press.pointerId) return;
        const finished = press;
        press = null;
        track.classList.remove('is-horizontal-dragging');
        if (track.hasPointerCapture(event.pointerId)) track.releasePointerCapture(event.pointerId);
        if (finished.changed) track.querySelector('input[type="number"]')?.dispatchEvent(new Event('change', { bubbles: true }));
        if (finished.dragging) {
            event.preventDefault();
            setTimeout(() => { suppressClick = false; }, 0);
        }
    };
    track.addEventListener('pointerup', finish);
    track.addEventListener('pointercancel', finish);
    track.addEventListener('lostpointercapture', finish);
    track.addEventListener('pointerleave', () => { if (!press?.dragging) press = null; });
    track.addEventListener('click', event => {
        if (!suppressClick) return;
        event.preventDefault();
        event.stopPropagation();
    }, true);
}

/** 重复初始化只更新回调；排除的区域由各自业务监听器负责初始化。 */
export function setupNumberInputButtons(container, onChange, { excludeSelector } = {}) {
    if (!container) return;
    container.querySelectorAll('.number-input-wrapper input[type="number"]').forEach(input => {
        if (excludeSelector && input.closest(excludeSelector)) return;
        const host = input.closest('.number-input-wrapper');
        if (!host) return;
        input.classList.add('number-input-drag-value');
        const track = input.closest('.number-input-wrapper');
        if (!track) return;
        track.classList.add('numeric-drag-field');
        let binding = numberBindings.get(host);
        if (binding) {
            binding.input = input;
            if (onChange !== undefined) binding.onChange = onChange;
            input.addEventListener('input', () => renderNumberValue(input));
            input.addEventListener('change', () => renderNumberValue(input));
            renderNumberValue(input);
            return;
        }
        binding = { input, onChange };
        numberBindings.set(host, binding);
        host.querySelectorAll('.number-btn').forEach(button => {
            button.addEventListener('click', () => {
                const currentInput = host.querySelector('input[type="number"]');
                if (!currentInput) return;
                binding.input = currentInput;
                if (currentInput.disabled || currentInput.readOnly) return;
                if (button.classList.contains('number-reset')) {
                    const value = numberValue(currentInput.dataset.default, null);
                    if (value !== null) publishValue(currentInput, binding, value, 'reset');
                    return;
                }
                const action = button.classList.contains('plus') ? 'plus' : 'minus';
                const limits = limitsFor(currentInput);
                const value = boundedValue(numberValue(currentInput.value, 0) + (action === 'plus' ? limits.step : -limits.step), limits);
                publishValue(currentInput, binding, value, action);
            });
        });
        host.addEventListener('input', event => {
            if (event.target.matches('input[type="number"]')) {
                binding.input = event.target;
                renderNumberValue(event.target);
            }
        });
        host.addEventListener('change', event => {
            if (event.target.matches('input[type="number"]')) {
                binding.input = event.target;
                renderNumberValue(event.target);
            }
        });
        bindNumberDrag(track, binding);
        renderNumberValue(input);
    });
}
