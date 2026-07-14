/**
 * 创建数字输入框UI组件
 */
export const createNumInput = (id, label, val, min = 0, max = null, step = 1, colSpan = 1) => `
    <div class="setting-item" style="grid-column: span ${colSpan}; min-width: 0;">
        <label for="${id}">${label}:</label>
        <div class="number-input-wrapper horizontal layer-param-input">
            <button type="button" class="number-btn minus" data-target="${id}">-</button>
            <input type="number" id="${id}" min="${min}" ${max !== null ? `max="${max}"` : ''} step="${step}" value="${val}" data-default="${val}" class="form-control number-input">
            <button type="button" class="number-btn plus" data-target="${id}">+</button>
            <button type="button" class="number-btn number-reset" data-target="${id}" title="恢复默认">↻</button>
        </div>
    </div>
`;

/**
 * 设置数字输入框的加减按钮监听器
 * @param {HTMLElement} container - 包含按钮的容器元素
 * @param {Function} onChange - 值改变时的回调函数 (targetId, newValue)
 */
export function setupNumberInputButtons(container, onChange) {
    if (!container) return;

    container.querySelectorAll('.number-btn').forEach(btn => {
        btn.addEventListener('click', function () {
            const targetId = this.dataset.target;
            const input = document.getElementById(targetId);
            if (!input) return;

            if (this.classList.contains('number-reset')) {
                const defaultVal = input.dataset.default;
                if (defaultVal !== undefined && defaultVal !== '') {
                    input.value = defaultVal;
                    if (onChange) onChange(targetId, parseFloat(defaultVal), 'reset');
                    input.dispatchEvent(new Event('input', { bubbles: true }));
                    input.dispatchEvent(new Event('change', { bubbles: true }));
                }
                return;
            }

            const getFloatValue = (val, defaultVal = 0) => {
                const parsed = parseFloat(val);
                return isNaN(parsed) ? defaultVal : parsed;
            };

            const currentValue = getFloatValue(input.value, 0);
            const min = getFloatValue(input.min, 0);
            const max = input.max !== undefined && input.max !== '' ? getFloatValue(input.max, null) : null;
            const step = getFloatValue(input.step, 1);

            const getDecimalPlaces = (num) => {
                const str = String(num);
                const dotIndex = str.indexOf('.');
                return dotIndex === -1 ? 0 : str.length - dotIndex - 1;
            };
            const precision = getDecimalPlaces(step);

            let newValue;
            if (this.classList.contains('plus')) {
                newValue = currentValue + step;
                newValue = parseFloat(newValue.toFixed(precision));
                if (max !== null && newValue > max) newValue = max;
            } else if (this.classList.contains('minus')) {
                newValue = currentValue - step;
                newValue = parseFloat(newValue.toFixed(precision));
                if (newValue < min) newValue = min;
            }

            input.value = newValue;

            let action = '';
            if (this.classList.contains('plus')) {
                action = 'plus';
            } else if (this.classList.contains('minus')) {
                action = 'minus';
            }

            if (onChange) onChange(targetId, newValue, action);

            input.dispatchEvent(new Event('input', { bubbles: true }));
            input.dispatchEvent(new Event('change', { bubbles: true }));
        });
    });
}
