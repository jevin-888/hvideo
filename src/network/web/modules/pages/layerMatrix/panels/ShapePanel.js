import { SHAPE_TYPES } from '../utils.js';

/**
 * 生成图形类型选项HTML
 * @param {number} selectedValue - 当前选中的值
 * @returns {string} HTML字符串
 */
export function generateShapeTypeOptions(selectedValue) {
    return SHAPE_TYPES.map(shape => {
        // 确保 value 是数字类型，避免字符串比较问题
        const value = Number(shape.value);
        const isSelected = Number(selectedValue) === value;
        return `<option value="${value}" ${isSelected ? 'selected' : ''}>${shape.label}</option>`;
    }).join('');
}

/**
 * 生成图形参数配置区域的HTML
 */
export function generateShapeParamHTML(shapeType, shapeParam, prefix = 'layer', displayType = 'block') {
    const isVisible = (shapeType === 3 || shapeType === 4 || shapeType === 8);
    const label = shapeType === 3 ? '圆角半径:' : shapeType === 4 ? '角的数量:' : shapeType === 8 ? '花瓣数量:' : '参数:';
    const min = shapeType === 3 ? '0.0' : (shapeType === 4 || shapeType === 8) ? '3' : '0.0';
    const max = shapeType === 3 ? '0.5' : (shapeType === 4 || shapeType === 8) ? '8' : '1.0';
    const step = shapeType === 3 ? '0.01' : (shapeType === 4 || shapeType === 8) ? '1' : '0.01';

    return `
        <div id="${prefix}-shape-param-item" class="setting-item" style="grid-column: span 1; display: ${isVisible ? displayType : 'none'};">
            <label for="${prefix}-shape-param">${label}</label>
            <div class="number-input-wrapper horizontal layer-param-input">
                <button type="button" class="number-btn minus" data-target="${prefix}-shape-param">-</button>
                <input type="number" id="${prefix}-shape-param" 
                    min="${min}" max="${max}" step="${step}" 
                    value="${shapeParam}" data-default="${shapeParam}" class="form-control number-input">
                <button type="button" class="number-btn plus" data-target="${prefix}-shape-param">+</button>
                <button type="button" class="number-btn number-reset" data-target="${prefix}-shape-param" title="恢复默认">↻</button>
            </div>
        </div>
    `;
}

/**
 * 更新图形参数容器的显示状态和配置
 * @param {number} 形状类型 - 图形类型
 * @param {HTMLElement} paramItem - 参数容器元素
 * @param {string} prefix - ID前缀 (layer/slice)
 * @param {string} displayType - 显示模式
 */
export function updateShapeParamUI(shapeType, paramItem, prefix = 'layer', displayType = 'block') {
    if (!paramItem) return;

    const needsParam = (shapeType === 3 || shapeType === 4 || shapeType === 8);
    paramItem.style.display = needsParam ? displayType : 'none';

    if (needsParam) {
        const paramLabel = paramItem.querySelector(`label[for="${prefix}-shape-param"]`);
        const paramInput = paramItem.querySelector(`input#${prefix}-shape-param`);
        if (paramLabel && paramInput) {
            if (shapeType === 3) {
                paramLabel.textContent = '圆角半径:';
                paramInput.min = '0.0';
                paramInput.max = '0.5';
                paramInput.step = '0.01';
            } else if (shapeType === 4) {
                paramLabel.textContent = '角的数量:';
                paramInput.min = '3';
                paramInput.max = '8';
                paramInput.step = '1';
            } else if (shapeType === 8) {
                paramLabel.textContent = '花瓣数量:';
                paramInput.min = '3';
                paramInput.max = '8';
                paramInput.step = '1';
            }
        }
    }
}
