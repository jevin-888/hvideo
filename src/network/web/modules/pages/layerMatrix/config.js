// 配置管理模块
import { apiPost, apiAction } from '../../core/api.js';
import { addToCommandLog } from '../../core/commandLog.js';

/**
 * 加载系统配置
 */
export async function loadSystemConfig(canvasConfig, updateCanvasSizeByLayout) {
    try {
        // 获取区域配置（幕布分辨率和布局）

        // apiAction 严格解析唯一 envelope，并直接返回其中的业务 data。
        const regionResponse = await apiAction('regions', 'get_region_config', {});

        // regionResponse 即为业务数据对象（含 canvas_in_宽度 / layout_in_cols 等字段）
        const regionData = (regionResponse && typeof regionResponse === 'object' && !Array.isArray(regionResponse))
            ? regionResponse
            : null;

        if (regionData) {
            if (regionData.canvas_in_width === undefined || regionData.canvas_in_height === undefined) {
                addToCommandLog('配置错误', 'error', 'config.json缺少canvas_in_width或canvas_in_height字段');
                return;
            }

            const width = parseInt(regionData.canvas_in_width) || 0;
            const height = parseInt(regionData.canvas_in_height) || 0;
            const cols = parseInt(regionData.layout_in_cols) || 0;
            const rows = parseInt(regionData.layout_in_rows) || 0;
            const regionWidth = parseInt(regionData.tile_in_width) || 0;
            const regionHeight = parseInt(regionData.tile_in_height) || 0;

            if (width <= 0 || height <= 0 || cols <= 0 || rows <= 0) {
                canvasConfig.loaded = false;
                addToCommandLog('配置错误', 'error', 'config.json中分辨率或布局参数无效');
                return;
            }

            canvasConfig.width = width;
            canvasConfig.height = height;
            canvasConfig.cols = cols;
            canvasConfig.rows = rows;
            canvasConfig.regionWidth = regionWidth;
            canvasConfig.regionHeight = regionHeight;
            canvasConfig.loaded = true;

            // 更新输入框
            const widthInput = document.getElementById('canvas-width-input');
            const heightInput = document.getElementById('canvas-height-input');
            const colsInput = document.getElementById('canvas-cols-input');
            const rowsInput = document.getElementById('canvas-rows-input');
            if (widthInput) widthInput.value = width;
            if (heightInput) heightInput.value = height;
            if (colsInput) colsInput.value = cols;
            if (rowsInput) rowsInput.value = rows;

            // 获取并设置旋转角度（矩阵配置的一部分）
            const canvasRotationInput = document.getElementById('canvas-rotation');
            if (canvasRotationInput && regionData.rotation_angle !== undefined) {
                const rotationValue = Math.round(parseFloat(regionData.rotation_angle) || 0);
                const normalizedValue = [0, 90, 180, 270].includes(rotationValue) ? rotationValue : 0;
                canvasRotationInput.value = normalizedValue.toString();
            }
            // 页面可见时更新画布尺寸
            const layerMatrixPage = document.getElementById('layer-matrix-page');
            if (layerMatrixPage && layerMatrixPage.classList.contains('active')) {
                await updateCanvasSizeByLayout();
            }
        } else {
            addToCommandLog('配置错误', 'error', 'config.json配置响应格式不正确或数据为空');
        }
    } catch (error) {
        addToCommandLog('配置加载失败', 'error', '无法从后端加载config.json: ' + error.message);
    }
}
