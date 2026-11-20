import { SHAPE_TYPES } from '../utils.js';
import { createNumInput } from './CommonControls.js';

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
    const label = shapeType === 3 ? '圆角半径' : shapeType === 4 ? '角的数量' : shapeType === 8 ? '花瓣数量' : '参数';
    const min = shapeType === 3 ? '0.0' : (shapeType === 4 || shapeType === 8) ? '3' : '0.0';
    const max = shapeType === 3 ? '0.5' : (shapeType === 4 || shapeType === 8) ? '8' : '1.0';
    const step = shapeType === 3 ? '0.01' : (shapeType === 4 || shapeType === 8) ? '1' : '0.01';

    return createNumInput(`${prefix}-shape-param`, label, shapeParam, min, max, step, 1, {
        itemId: `${prefix}-shape-param-item`,
        display: isVisible ? displayType : 'none'
    });
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
