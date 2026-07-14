// 矩阵配置模块
import { apiPost, getLastApiError, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { showSuccess, showError } from '../components/toast.js';

// 当前映射配置
// 矩阵配置负责：幕布布局、幕布分辨率、输出分辨率、输出布局、输出映射、旋转角度
let currentMappingConfig = {
    canvas: { width: 0, height: 0, gridCols: 0, gridRows: 0 },
    output: { width: 0, height: 0, gridCols: 0, gridRows: 0 },
    mappings: []
};

// 当前打开的下拉菜单
let currentDropdown = null;

// 标记事件监听器是否已绑定，避免重复绑定
let eventListenersBound = false;
let mappingLoadPromise = null;
const MATRIX_SAVE_TIMEOUT_MS = 60000;
const MAX_MAPPING_GRID_DIMENSION = 64;
const MAX_MAPPING_GRID_CELLS = 144;
let licensedInputChannelCount = 0;

function formatLayout(rows, cols) {
    // UI/API 布局文字统一按行×列(rows×cols)显示，12×1 表示 12 行 1 列。
    return `${rows}×${cols}`;
}

function toInteger(value, defaultValue = 0) {
    const num = Number(value);
    return Number.isFinite(num) ? Math.floor(num) : defaultValue;
}

function computeAllowedInputLayouts(maxChannels) {
    const limit = toInteger(maxChannels, 0);
    if (limit <= 0) return [];
    const layouts = [];
    for (let rows = 1; rows <= limit; rows++) {
        for (let cols = 1; cols <= limit; cols++) {
            if (rows * cols > limit) continue;
            layouts.push({ rows, cols, area: rows * cols, label: `${rows}x${cols}` });
        }
    }
    layouts.sort((a, b) => {
        if (a.area !== b.area) return a.area - b.area;
        if (a.rows !== b.rows) return a.rows - b.rows;
        return a.cols - b.cols;
    });
    return layouts;
}

function syncLayoutModeFromGrid() {
    const layoutModeSelect = document.getElementById('flexible-canvas-layout-mode');
    if (!layoutModeSelect) return;
    const rows = toInteger(currentMappingConfig.canvas.gridRows, 0);
    const cols = toInteger(currentMappingConfig.canvas.gridCols, 0);
    const expectedValue = rows > 0 && cols > 0 ? `${rows}x${cols}` : '';
    if (expectedValue && Array.from(layoutModeSelect.options).some((option) => option.value === expectedValue)) {
        layoutModeSelect.value = expectedValue;
    } else {
        layoutModeSelect.value = '';
    }
}

function updateAllowedLayoutModes() {
    const layoutModeSelect = document.getElementById('flexible-canvas-layout-mode');
    if (!layoutModeSelect) return;

    const layouts = computeAllowedInputLayouts(licensedInputChannelCount);
    const currentRows = toInteger(currentMappingConfig.canvas.gridRows, 0);
    const currentCols = toInteger(currentMappingConfig.canvas.gridCols, 0);
    const currentValue = currentRows > 0 && currentCols > 0 ? `${currentRows}x${currentCols}` : '';
    if (layouts.length === 0) {
        layoutModeSelect.innerHTML = '<option value="">不限制</option>';
        layoutModeSelect.disabled = true;
        return;
    }

    layoutModeSelect.disabled = false;
    const options = ['<option value="">请选择</option>'];
    layouts.forEach((layout) => {
        options.push(`<option value="${layout.label}">${layout.label} ${layout.area}通道</option>`);
    });
    if (currentValue && !layouts.some((layout) => layout.label === currentValue)) {
        options.push(`<option value="${currentValue}">${currentValue} 当前配置 超出授权</option>`);
    }
    layoutModeSelect.innerHTML = options.join('');
    syncLayoutModeFromGrid();
}

function enforceLicensedInputLayoutOrThrow(rows, cols) {
    const limit = toInteger(licensedInputChannelCount, 0);
    if (limit > 0 && rows * cols > limit) {
        throw new Error(`当前授权输入通道数为 ${limit}，输入布局最多使用 ${limit} 个通道`);
    }
}

function validateLayoutSizeOrThrow(rows, cols, label) {
    if (rows <= 0 || cols <= 0) return;
    if (rows > MAX_MAPPING_GRID_DIMENSION || cols > MAX_MAPPING_GRID_DIMENSION) {
        throw new Error(`${label}单边最大支持 ${MAX_MAPPING_GRID_DIMENSION}`);
    }
    if (rows * cols > MAX_MAPPING_GRID_CELLS) {
        throw new Error(`${label}最多支持 ${MAX_MAPPING_GRID_CELLS} 个单元`);
    }
}

export function setLicensedInputChannelCount(count) {
    licensedInputChannelCount = Math.max(0, toInteger(count, 0));
    updateAllowedLayoutModes();
}

function normalizeMappings(mappings) {
    if (!Array.isArray(mappings)) return [];
    return mappings.map(m => {
        const inId = toInteger(m.in_id ?? m.inputRegionId, 0);
        const rawOutIdx = toInteger(m.out_idx ?? m.outputIndex, -1);
        const enabled = m.enabled !== false && rawOutIdx >= 0;
        return {
            in_id: inId,
            out_idx: enabled ? rawOutIdx : -1,
            enabled
        };
    }).sort((a, b) => a.in_id - b.in_id);
}

function buildMappingsForSave(existingMappings, inputCount, outputCount) {
    const byInput = new Map();
    const usedOutputs = new Set();
    const source = Array.isArray(existingMappings) ? existingMappings : [];

    source.forEach(m => {
        const inputRegionId = toInteger(m.inputRegionId ?? m.in_id, 0);
        if (inputRegionId < 1 || inputRegionId > inputCount) return;

        const rawOutputIndex = toInteger(m.outputIndex ?? m.out_idx, -1);
        const canUseOutput = rawOutputIndex >= 0 &&
            rawOutputIndex < outputCount &&
            !usedOutputs.has(rawOutputIndex) &&
            m.enabled !== false;
        const outputIndex = canUseOutput ? rawOutputIndex : -1;
        if (outputIndex >= 0) usedOutputs.add(outputIndex);

        byInput.set(inputRegionId, {
            inputRegionId,
            outputIndex,
            enabled: outputIndex >= 0
        });
    });

    for (let inputRegionId = 1; inputRegionId <= inputCount; inputRegionId++) {
        if (byInput.has(inputRegionId)) continue;

        let outputIndex = -1;
        for (let candidate = 0; candidate < outputCount; candidate++) {
            if (!usedOutputs.has(candidate)) {
                outputIndex = candidate;
                usedOutputs.add(candidate);
                break;
            }
        }

        byInput.set(inputRegionId, {
            inputRegionId,
            outputIndex,
            enabled: outputIndex >= 0
        });
    }

    return Array.from(byInput.values()).sort((a, b) => a.inputRegionId - b.inputRegionId);
}

function responseMatchesConfig(responseData, config) {
    const integerFields = [
        ['canvas_in_width', '输入宽度'],
        ['canvas_in_height', '输入高度'],
        ['layout_in_rows', '输入布局行数'],
        ['layout_in_cols', '输入布局列数'],
        ['canvas_out_width', '输出宽度'],
        ['canvas_out_height', '输出高度'],
        ['layout_out_rows', '输出布局行数'],
        ['layout_out_cols', '输出布局列数'],
        ['split_direction', '分割方向']
    ];

    for (const [field, label] of integerFields) {
        if (toInteger(responseData?.[field], NaN) !== toInteger(config[field], NaN)) {
            return {
                ok: false,
                message: `${label}返回${responseData?.[field]}，期望${config[field]}`
            };
        }
    }

    if (Math.abs(Number(responseData?.rotation_angle || 0) - Number(config.rotation_angle || 0)) > 0.01) {
        return {
            ok: false,
            message: `旋转角度返回${responseData?.rotation_angle}，期望${config.rotation_angle}`
        };
    }

    const expectedMappings = normalizeMappings(config.mappings);
    const actualMappings = normalizeMappings(responseData?.mappings);
    if (expectedMappings.length !== actualMappings.length) {
        return {
            ok: false,
            message: `映射数量返回${actualMappings.length}，期望${expectedMappings.length}`
        };
    }

    for (let i = 0; i < expectedMappings.length; i++) {
        const expected = expectedMappings[i];
        const actual = actualMappings[i];
        if (expected.in_id !== actual.in_id ||
            expected.out_idx !== actual.out_idx ||
            expected.enabled !== actual.enabled) {
            return {
                ok: false,
                message: `输入${expected.in_id}映射返回${actual.out_idx}，期望${expected.out_idx}`
            };
        }
    }

    return { ok: true, message: '' };
}

/**
 * 初始化矩阵配置
 */
export function initializeFlexibleMapping() {
    setupEventListeners();
}

/**
 * 设置事件监听器
 */
function setupEventListeners() {
    // 如果已经绑定过，先移除旧的事件监听器（避免重复绑定）
    if (eventListenersBound) {
        return;
    }

    // 先绑定保存和重置按钮（使用全局查找，不依赖容器）
    const applyBtn = document.getElementById('apply-flexible-mapping');
    if (applyBtn) {
        const newApplyBtn = applyBtn.cloneNode(true);
        applyBtn.parentNode.replaceChild(newApplyBtn, applyBtn);
        newApplyBtn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            applyFlexibleMapping();
        });
    }

    // 重置按钮
    const resetBtn = document.getElementById('reset-flexible-mapping');
    if (resetBtn) {
        const newResetBtn = resetBtn.cloneNode(true);
        resetBtn.parentNode.replaceChild(newResetBtn, resetBtn);
        newResetBtn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            resetFlexibleMapping();
        });
    }

    // 使用 querySelector 查找矩阵配置区域的元素（避免与其他页面的同名元素冲突）
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) {
        eventListenersBound = true;
        return;
    }

    // 输入布局和分辨率变化
    const canvasWidthInput = flexibleMappingContainer.querySelector('#flexible-canvas-width');
    const canvasHeightInput = flexibleMappingContainer.querySelector('#flexible-canvas-height');
    const canvasColsInput = flexibleMappingContainer.querySelector('#flexible-canvas-cols');
    const canvasRowsInput = flexibleMappingContainer.querySelector('#flexible-canvas-rows');
    const layoutModeSelect = flexibleMappingContainer.querySelector('#flexible-canvas-layout-mode');
    if (canvasWidthInput) {
        canvasWidthInput.addEventListener('change', () => {
            currentMappingConfig.canvas.width = parseInt(canvasWidthInput.value, 10) || 0;
            updateSectionTitles();
        });
    }
    if (canvasHeightInput) {
        canvasHeightInput.addEventListener('change', () => {
            currentMappingConfig.canvas.height = parseInt(canvasHeightInput.value, 10) || 0;
            updateSectionTitles();
        });
    }
    if (canvasColsInput) {
        canvasColsInput.addEventListener('change', () => updateInputGrid());
    }
    if (canvasRowsInput) {
        canvasRowsInput.addEventListener('change', () => updateInputGrid());
    }
    if (layoutModeSelect) {
        layoutModeSelect.addEventListener('change', () => {
            const value = layoutModeSelect.value || '';
            if (!value.includes('x')) return;
            const parts = value.split('x').map((item) => parseInt(item, 10));
            if (parts.length !== 2 || !parts[0] || !parts[1]) return;
            if (canvasRowsInput) canvasRowsInput.value = parts[0];
            if (canvasColsInput) canvasColsInput.value = parts[1];
            updateInputGrid();
        });
    }

    // 输出布局和分辨率变化
    const outputWidthInput = flexibleMappingContainer.querySelector('#flexible-output-width');
    const outputHeightInput = flexibleMappingContainer.querySelector('#flexible-output-height');
    const outputColsInput = flexibleMappingContainer.querySelector('#flexible-output-cols');
    const outputRowsInput = flexibleMappingContainer.querySelector('#flexible-output-rows');
    if (outputWidthInput) {
        outputWidthInput.addEventListener('change', () => {
            currentMappingConfig.output.width = parseInt(outputWidthInput.value, 10) || 0;
            renderOutputGrid();
        });
    }
    if (outputHeightInput) {
        outputHeightInput.addEventListener('change', () => {
            currentMappingConfig.output.height = parseInt(outputHeightInput.value, 10) || 0;
            renderOutputGrid();
        });
    }
    if (outputColsInput) {
        outputColsInput.addEventListener('change', () => updateOutputGrid());
    }
    if (outputRowsInput) {
        outputRowsInput.addEventListener('change', () => updateOutputGrid());
    }

    // 标记事件监听器已绑定
    eventListenersBound = true;
}

/**
 * 加载矩阵配置
 */
export async function loadFlexibleMapping() {
    if (mappingLoadPromise) return mappingLoadPromise;
    mappingLoadPromise = doLoadFlexibleMapping().finally(() => {
        mappingLoadPromise = null;
    });
    return mappingLoadPromise;
}

async function doLoadFlexibleMapping() {
    try {
        const regionData = await apiAction('regions', 'get_region_config', {});

        if (regionData && typeof regionData === 'object' && !Array.isArray(regionData)) {
            applyRegionDataToCurrentConfig(regionData);
            updateInputFields();
            renderAll();
        } else {
            generateDefaultMappings();
            updateInputFields();
            renderAll();
        }
    } catch (error) {
        showError('加载矩阵配置失败');
    }
}

function applyRegionDataToCurrentConfig(regionData) {
    if (!regionData || typeof regionData !== 'object') return;

    const safeParseInt = (value, defaultValue) => {
        if (value === undefined || value === null || value === '') return defaultValue;
        const num = Number(value);
        return (Number.isNaN(num) || num <= 0) ? defaultValue : Math.floor(num);
    };

    const applyPositive = (target, key, value) => {
        if (value !== undefined && Number(value) > 0) {
            target[key] = safeParseInt(value, target[key]);
        }
    };

    applyPositive(currentMappingConfig.canvas, 'width', regionData.canvas_in_width);
    applyPositive(currentMappingConfig.canvas, 'height', regionData.canvas_in_height);
    applyPositive(currentMappingConfig.canvas, 'gridCols', regionData.layout_in_cols);
    applyPositive(currentMappingConfig.canvas, 'gridRows', regionData.layout_in_rows);
    applyPositive(currentMappingConfig.output, 'width', regionData.canvas_out_width);
    applyPositive(currentMappingConfig.output, 'height', regionData.canvas_out_height);
    applyPositive(currentMappingConfig.output, 'gridCols', regionData.layout_out_cols);
    applyPositive(currentMappingConfig.output, 'gridRows', regionData.layout_out_rows);

    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    const rotationInput = flexibleMappingContainer?.querySelector('#flexible-canvas-rotation');
    if (rotationInput && regionData.rotation_angle !== undefined) {
        rotationInput.value = String(Math.min(360, Math.max(0, Math.round(Number(regionData.rotation_angle) || 0))));
    }

    if (Array.isArray(regionData.mappings) && regionData.mappings.length > 0) {
        currentMappingConfig.mappings = regionData.mappings.map(m => {
            const outputIndex = m.out_idx !== undefined ? parseInt(m.out_idx, 10) : -1;
            const enabled = m.enabled !== undefined ? Boolean(m.enabled) : outputIndex >= 0;
            return {
                inputRegionId: parseInt(m.in_id || 0, 10),
                outputIndex: enabled ? outputIndex : -1,
                enabled
            };
        });
    } else {
        generateDefaultMappings();
    }
    syncLayoutModeFromGrid();
}

/**
 * 生成默认映射（顺序映射）
 */
function generateDefaultMappings() {
    const gridCols = currentMappingConfig.canvas.gridCols || 0;
    const gridRows = currentMappingConfig.canvas.gridRows || 0;
    const outputCols = currentMappingConfig.output.gridCols || 0;
    const outputRows = currentMappingConfig.output.gridRows || 0;

    const inputCount = gridCols * gridRows;
    const outputCount = outputCols * outputRows;

    currentMappingConfig.mappings = [];
    for (let i = 1; i <= inputCount; i++) {
        const outputIndex = i <= outputCount ? i - 1 : -1;
        currentMappingConfig.mappings.push({
            inputRegionId: i,
            outputIndex: outputIndex,
            enabled: outputIndex >= 0
        });
    }
}

/**
 * 更新区域标题显示
 */
function updateSectionTitles() {
    const inputTitle = document.getElementById('input-section-title');
    const outputTitle = document.getElementById('output-section-title');

    if (inputTitle) {
        inputTitle.textContent = '输入';
    }

    if (outputTitle) {
        outputTitle.textContent = '输出';
    }
}

/**
 * 更新输入字段
 */
function updateInputFields() {
    // 在矩阵配置区域内查找元素（避免与其他页面的同名元素冲突）
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) return;

    const canvasWidthInput = flexibleMappingContainer.querySelector('#flexible-canvas-width');
    const canvasHeightInput = flexibleMappingContainer.querySelector('#flexible-canvas-height');
    const canvasColsInput = flexibleMappingContainer.querySelector('#flexible-canvas-cols');
    const canvasRowsInput = flexibleMappingContainer.querySelector('#flexible-canvas-rows');
    const outputWidthInput = flexibleMappingContainer.querySelector('#flexible-output-width');
    const outputHeightInput = flexibleMappingContainer.querySelector('#flexible-output-height');
    const outputColsInput = flexibleMappingContainer.querySelector('#flexible-output-cols');
    const outputRowsInput = flexibleMappingContainer.querySelector('#flexible-output-rows');
    const rotationInput = flexibleMappingContainer.querySelector('#flexible-canvas-rotation');

    // 零硬编码：未配置时为 0，须用户配置矩阵
    const canvasWidth = currentMappingConfig.canvas.width > 0 ? currentMappingConfig.canvas.width : 0;
    const canvasHeight = currentMappingConfig.canvas.height > 0 ? currentMappingConfig.canvas.height : 0;
    const canvasCols = currentMappingConfig.canvas.gridCols > 0 ? currentMappingConfig.canvas.gridCols : 0;
    const canvasRows = currentMappingConfig.canvas.gridRows > 0 ? currentMappingConfig.canvas.gridRows : 0;
    const outputWidth = currentMappingConfig.output.width > 0 ? currentMappingConfig.output.width : 0;
    const outputHeight = currentMappingConfig.output.height > 0 ? currentMappingConfig.output.height : 0;
    const outputCols = currentMappingConfig.output.gridCols > 0 ? currentMappingConfig.output.gridCols : 0;
    const outputRows = currentMappingConfig.output.gridRows > 0 ? currentMappingConfig.output.gridRows : 0;

    if (canvasWidthInput) canvasWidthInput.value = canvasWidth;
    if (canvasHeightInput) canvasHeightInput.value = canvasHeight;
    if (canvasColsInput) canvasColsInput.value = canvasCols;
    if (canvasRowsInput) canvasRowsInput.value = canvasRows;
    if (outputWidthInput) {
        outputWidthInput.value = outputWidth;
        outputWidthInput.readOnly = false;
        outputWidthInput.style.backgroundColor = '';
        outputWidthInput.title = '总输出分辨率宽度（渲染目标），须与后端 config.json 一致';
    }
    if (outputHeightInput) {
        outputHeightInput.value = outputHeight;
        outputHeightInput.readOnly = false;
        outputHeightInput.style.backgroundColor = '';
        outputHeightInput.title = '总输出分辨率高度（渲染目标），须与后端 config.json 一致';
    }
    if (outputColsInput) outputColsInput.value = outputCols;
    if (outputRowsInput) outputRowsInput.value = outputRows;

    // 更新标题显示
    updateSectionTitles();
    syncLayoutModeFromGrid();
}

/**
 * 渲染所有布局单元
 */
function renderAll() {
    updateSectionTitles();
    renderInputGrid();
    renderOutputGrid();
}

/**
 * 渲染输入布局
 */
function renderInputGrid() {
    // 在矩阵配置区域内查找元素
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) return;

    const grid = flexibleMappingContainer.querySelector('#input-grid');
    if (!grid) return;

    const gridCols = currentMappingConfig.canvas.gridCols || 0;
    const gridRows = currentMappingConfig.canvas.gridRows || 0;

    if (gridCols <= 0 || gridRows <= 0) {
        grid.innerHTML = '<div class="mapping-placeholder">请先配置幕布布局（行×列）</div>';
        return;
    }
    const totalCells = gridCols * gridRows;
    if (totalCells > MAX_MAPPING_GRID_CELLS) {
        grid.innerHTML = `<div class="mapping-placeholder">输入布局共 ${totalCells} 个单元，超过预览上限 ${MAX_MAPPING_GRID_CELLS}</div>`;
        return;
    }

    grid.innerHTML = '';
    
    // 当列数较多时（>8列），使用固定最小宽度让布局可以水平滚动
    const MIN_CELL_WIDTH = 70; // 最小单元格宽度（像素）
    if (gridCols > 8) {
        grid.style.gridTemplateColumns = `repeat(${gridCols}, minmax(${MIN_CELL_WIDTH}px, 1fr))`;
        grid.style.minWidth = `${gridCols * (MIN_CELL_WIDTH + 4)}px`; // 加上间隔
    } else {
        grid.style.gridTemplateColumns = `repeat(${gridCols}, 1fr)`;
        grid.style.minWidth = '';
    }
    grid.style.gridTemplateRows = `repeat(${gridRows}, auto)`;
    grid.style.gap = '4px';

    const mappingsByInput = new Map(
        currentMappingConfig.mappings.map(m => [m.inputRegionId, m])
    );
    const fragment = document.createDocumentFragment();
    for (let row = 0; row < gridRows; row++) {
        for (let col = 0; col < gridCols; col++) {
            const regionId = row * gridCols + col + 1;
            const mapping = mappingsByInput.get(regionId);
            const cell = createInputCell(regionId, mapping);
            fragment.appendChild(cell);
        }
    }
    grid.appendChild(fragment);
}

/**
 * 创建输入布局单元格
 */
function createInputCell(regionId, mapping) {
    const cell = document.createElement('div');
    const isEnabled = mapping && mapping.enabled && mapping.outputIndex !== undefined && mapping.outputIndex >= 0;
    cell.className = `mapping-cell input-cell ${isEnabled ? 'enabled' : 'disabled'}`;
    cell.dataset.regionId = regionId;
    cell.style.position = 'relative';

    // 固定使用16:9的宽高比显示预览单元格
    cell.style.aspectRatio = '16 / 9';

    // 显示映射关系：{regionId}|{outputIndex+1} 格式
    const label = document.createElement('div');
    label.className = 'cell-label';
    if (isEnabled) {
        label.textContent = `${regionId}|${mapping.outputIndex + 1}`;
    } else {
        label.textContent = `${regionId}`;
    }
    
    // 根据格子数量动态调整字体大小
    const totalCells = (currentMappingConfig.canvas.gridCols || 1) * (currentMappingConfig.canvas.gridRows || 1);
    let fontSize = '14px';
    if (totalCells > 100) {
        fontSize = '10px';
    } else if (totalCells > 64) {
        fontSize = '11px';
    } else if (totalCells > 36) {
        fontSize = '12px';
    }
    
    label.style.fontSize = fontSize;
    label.style.fontWeight = 'bold';
    label.style.color = isEnabled ? '#fff' : '#888';
    label.style.textAlign = 'center';
    label.style.lineHeight = '1.2';

    // 下拉箭头图标
    const dropdownIcon = document.createElement('div');
    dropdownIcon.className = 'dropdown-icon';
    dropdownIcon.innerHTML = '▼';
    dropdownIcon.style.position = 'absolute';
    dropdownIcon.style.top = '4px';
    dropdownIcon.style.right = '4px';
    dropdownIcon.style.fontSize = '10px';
    dropdownIcon.style.color = 'rgba(255, 255, 255, 0.6)';
    dropdownIcon.style.pointerEvents = 'none';

    cell.appendChild(label);
    cell.appendChild(dropdownIcon);

    // 点击显示下拉菜单
    cell.addEventListener('click', (e) => {
        e.stopPropagation();
        showOutputDropdown(regionId, cell, mapping ? mapping.outputIndex : -1);
    });

    return cell;
}

/**
 * 渲染输出布局
 */
function renderOutputGrid() {
    // 在矩阵配置区域内查找元素
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) return;

    const grid = flexibleMappingContainer.querySelector('#output-grid');
    if (!grid) return;

    const gridCols = currentMappingConfig.output.gridCols || 0;
    const gridRows = currentMappingConfig.output.gridRows || 0;

    if (gridCols <= 0 || gridRows <= 0) {
        grid.innerHTML = '<div class="mapping-placeholder">请先配置输出布局（行×列）</div>';
        return;
    }
    const totalCells = gridCols * gridRows;
    if (totalCells > MAX_MAPPING_GRID_CELLS) {
        grid.innerHTML = `<div class="mapping-placeholder">输出布局共 ${totalCells} 个单元，超过预览上限 ${MAX_MAPPING_GRID_CELLS}</div>`;
        return;
    }

    grid.innerHTML = '';
    
    // 当列数较多时（>8列），使用固定最小宽度让布局可以水平滚动
    const MIN_CELL_WIDTH = 70;
    if (gridCols > 8) {
        grid.style.gridTemplateColumns = `repeat(${gridCols}, minmax(${MIN_CELL_WIDTH}px, 1fr))`;
        grid.style.minWidth = `${gridCols * (MIN_CELL_WIDTH + 4)}px`;
    } else {
        grid.style.gridTemplateColumns = `repeat(${gridCols}, 1fr)`;
        grid.style.minWidth = '';
    }
    grid.style.gridTemplateRows = `repeat(${gridRows}, auto)`;
    grid.style.gap = '4px';

    const mappingsByOutput = new Map();
    currentMappingConfig.mappings.forEach(m => {
        if (m.enabled && m.outputIndex >= 0) {
            mappingsByOutput.set(m.outputIndex, m);
        }
    });
    const fragment = document.createDocumentFragment();
    for (let row = 0; row < gridRows; row++) {
        for (let col = 0; col < gridCols; col++) {
            const outputIndex = row * gridCols + col;
            const mapping = mappingsByOutput.get(outputIndex);
            const cell = createOutputCell(outputIndex, mapping);
            fragment.appendChild(cell);
        }
    }
    grid.appendChild(fragment);
}

/**
 * 创建输出布局单元格
 */
function createOutputCell(outputIndex, mapping) {
    const cell = document.createElement('div');
    const isMapped = mapping && mapping.inputRegionId && mapping.enabled;
    cell.className = `mapping-cell output-cell ${isMapped ? 'mapped' : 'empty'}`;
    cell.dataset.outputIndex = outputIndex;

    // 固定使用16:9的宽高比显示预览单元格
    cell.style.aspectRatio = '16 / 9';

    // 主标签：显示输出位置编号和对应的输入区域
    const label = document.createElement('div');
    label.className = 'cell-label';
    if (isMapped) {
        label.textContent = `${outputIndex + 1} ←输入${mapping.inputRegionId}`;
    } else {
        label.textContent = `${outputIndex + 1}`;
    }
    
    // 根据格子数量动态调整字体大小
    const totalCells = (currentMappingConfig.output.gridCols || 1) * (currentMappingConfig.output.gridRows || 1);
    let fontSize = '14px';
    if (totalCells > 100) {
        fontSize = '9px';
    } else if (totalCells > 64) {
        fontSize = '10px';
    } else if (totalCells > 36) {
        fontSize = '11px';
    }
    
    label.style.fontSize = fontSize;
    label.style.fontWeight = 'bold';
    label.style.color = isMapped ? '#fff' : '#888';
    label.style.textAlign = 'center';
    label.style.lineHeight = '1.2';

    cell.appendChild(label);

    return cell;
}


/**
 * 显示输出下拉菜单
 */
function showOutputDropdown(regionId, cell, currentOutputIndex) {
    // 关闭已存在的下拉菜单
    if (currentDropdown) {
        currentDropdown.remove();
        currentDropdown = null;
    }

    const outputCount = (currentMappingConfig.output.gridCols || 0) * (currentMappingConfig.output.gridRows || 0);

    // 创建下拉菜单
    const dropdown = document.createElement('div');
    dropdown.className = 'mapping-dropdown';
    dropdown.style.position = 'fixed';
    dropdown.style.zIndex = '10000';
    dropdown.style.minWidth = '150px';

    // 获取单元格位置（使用 fixed 定位，相对于视口）
    const rect = cell.getBoundingClientRect();

    dropdown.style.left = `${rect.left}px`;
    dropdown.style.top = `${rect.bottom + 4}px`;

    // 添加"禁用输出"选项
    const disableOption = document.createElement('div');
    disableOption.className = 'dropdown-option';
    disableOption.textContent = '禁用输出';
    disableOption.style.padding = '8px 12px';
    disableOption.style.cursor = 'pointer';
    disableOption.style.borderBottom = '1px solid rgba(255, 255, 255, 0.1)';
    if (currentOutputIndex < 0) {
        disableOption.style.backgroundColor = 'rgba(102, 126, 234, 0.3)';
    }
    disableOption.addEventListener('click', () => {
        updateMapping(regionId, -1);
        dropdown.remove();
        currentDropdown = null;
    });
    disableOption.addEventListener('mouseenter', () => {
        if (currentOutputIndex >= 0) {
            disableOption.style.backgroundColor = 'rgba(255, 255, 255, 0.1)';
        }
    });
    disableOption.addEventListener('mouseleave', () => {
        if (currentOutputIndex >= 0) {
            disableOption.style.backgroundColor = 'transparent';
        }
    });
    dropdown.appendChild(disableOption);

    // 添加所有输出位置选项
    for (let i = 0; i < outputCount; i++) {
        const option = document.createElement('div');
        option.className = 'dropdown-option';
        option.textContent = `输出到屏幕${i + 1}`;
        option.style.padding = '8px 12px';
        option.style.cursor = 'pointer';
        if (i < outputCount - 1) {
            option.style.borderBottom = '1px solid rgba(255, 255, 255, 0.1)';
        }
        if (currentOutputIndex === i) {
            option.style.backgroundColor = 'rgba(102, 126, 234, 0.3)';
        }
        option.addEventListener('click', () => {
            updateMapping(regionId, i);
            dropdown.remove();
            currentDropdown = null;
        });
        option.addEventListener('mouseenter', () => {
            if (currentOutputIndex !== i) {
                option.style.backgroundColor = 'rgba(255, 255, 255, 0.1)';
            }
        });
        option.addEventListener('mouseleave', () => {
            if (currentOutputIndex !== i) {
                option.style.backgroundColor = 'transparent';
            }
        });
        dropdown.appendChild(option);
    }

    // 添加到 body，使用 fixed 定位
    document.body.appendChild(dropdown);
    currentDropdown = dropdown;

    // 确保下拉菜单在视口内可见
    const dropdownRect = dropdown.getBoundingClientRect();
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;

    // 如果超出右边界，调整位置
    if (dropdownRect.right > viewportWidth) {
        dropdown.style.left = `${Math.max(0, viewportWidth - dropdownRect.width - 10)}px`;
    }

    // 如果超出下边界，向上显示
    if (dropdownRect.bottom > viewportHeight) {
        dropdown.style.top = `${Math.max(0, rect.top - dropdownRect.height - 4)}px`;
    }

    // 点击外部关闭下拉菜单
    const closeDropdown = (e) => {
        if (!dropdown.contains(e.target) && e.target !== cell) {
            dropdown.remove();
            currentDropdown = null;
            document.removeEventListener('click', closeDropdown);
        }
    };
    setTimeout(() => {
        document.addEventListener('click', closeDropdown);
    }, 0);
}

/**
 * 更新映射关系
 */
function updateMapping(inputRegionId, outputIndex) {
    const mapping = currentMappingConfig.mappings.find(m => m.inputRegionId === inputRegionId);
    if (!mapping) return;

    if (outputIndex < 0) {
        // 禁用输出
        mapping.enabled = false;
        mapping.outputIndex = -1;
    } else {
        // 清除该输出位置的其他映射
        const existingMapping = currentMappingConfig.mappings.find(m => m.enabled && m.outputIndex === outputIndex && m.inputRegionId !== inputRegionId);
        if (existingMapping) {
            existingMapping.enabled = false;
            existingMapping.outputIndex = -1;
        }

        // 设置新映射
        mapping.enabled = true;
        mapping.outputIndex = outputIndex;
    }

    renderAll();
}

/**
 * 更新输入布局（当布局变化时）
 * 自动同步输出布局和映射关系
 */
function updateInputGrid() {
    // 在灵活映射配置区域内查找元素（避免与其他页面的同名元素冲突）
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) return;

    const rowsInput = flexibleMappingContainer.querySelector('#flexible-canvas-rows');
    const colsInput = flexibleMappingContainer.querySelector('#flexible-canvas-cols');
    const outputRowsInput = flexibleMappingContainer.querySelector('#flexible-output-rows');
    const outputColsInput = flexibleMappingContainer.querySelector('#flexible-output-cols');

    if (rowsInput && colsInput) {
        const newRows = parseInt(rowsInput.value, 10) || 0;
        const newCols = parseInt(colsInput.value, 10) || 0;
        try {
            if (newRows > 0 && newCols > 0) {
                validateLayoutSizeOrThrow(newRows, newCols, '输入布局');
                enforceLicensedInputLayoutOrThrow(newRows, newCols);
            }
        } catch (error) {
            showError(error.message || '输入布局超出授权通道数', 3000);
            rowsInput.value = currentMappingConfig.canvas.gridRows || 0;
            colsInput.value = currentMappingConfig.canvas.gridCols || 0;
            syncLayoutModeFromGrid();
            return;
        }

        if (newRows !== currentMappingConfig.canvas.gridRows ||
            newCols !== currentMappingConfig.canvas.gridCols) {
            const oldRows = currentMappingConfig.canvas.gridRows;
            const oldCols = currentMappingConfig.canvas.gridCols;

            currentMappingConfig.canvas.gridRows = newRows;
            currentMappingConfig.canvas.gridCols = newCols;

            if (outputRowsInput && outputColsInput) {
                const currentOutputRows = parseInt(outputRowsInput.value, 10) || currentMappingConfig.output.gridRows || 0;
                const currentOutputCols = parseInt(outputColsInput.value, 10) || currentMappingConfig.output.gridCols || 0;

                // 如果输出布局与旧输入布局相同，则同步到新输入布局
                // 否则保持当前输出布局不变
                if (currentOutputRows === oldRows && currentOutputCols === oldCols) {
                    currentMappingConfig.output.gridRows = newRows;
                    currentMappingConfig.output.gridCols = newCols;
                    outputRowsInput.value = newRows;
                    outputColsInput.value = newCols;
                }
            }

            // 重新生成映射（基于新的输入和输出布局）
            generateDefaultMappings();
            updateSectionTitles();
            // 只更新UI显示，不立即同步到后端（避免后端渲染线程崩溃）
            // 用户需要点击"保存"按钮才会同步到后端
            renderAll();
            syncLayoutModeFromGrid();
        }
    }
}

/**
 * 更新输出布局（当布局变化时）
 */
function updateOutputGrid() {
    // 在灵活映射配置区域内查找元素（避免与其他页面的同名元素冲突）
    const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
    if (!flexibleMappingContainer) return;

    const rowsInput = flexibleMappingContainer.querySelector('#flexible-output-rows');
    const colsInput = flexibleMappingContainer.querySelector('#flexible-output-cols');

    if (rowsInput && colsInput) {
        const newRows = parseInt(rowsInput.value, 10) || 0;
        const newCols = parseInt(colsInput.value, 10) || 0;
        try {
            validateLayoutSizeOrThrow(newRows, newCols, '输出布局');
        } catch (error) {
            showError(error.message || '输出布局超出支持范围', 3000);
            rowsInput.value = currentMappingConfig.output.gridRows || 0;
            colsInput.value = currentMappingConfig.output.gridCols || 0;
            return;
        }

        if (newRows !== currentMappingConfig.output.gridRows ||
            newCols !== currentMappingConfig.output.gridCols) {
            currentMappingConfig.output.gridRows = newRows;
            currentMappingConfig.output.gridCols = newCols;

            // 清除超出范围的映射
            const maxOutputIndex = newCols * newRows - 1;
            currentMappingConfig.mappings.forEach(m => {
                if (m.outputIndex > maxOutputIndex) {
                    m.enabled = false;
                    m.outputIndex = -1;
                }
            });

            updateSectionTitles();
            renderAll();
        }
    }
}

/**
 * 应用矩阵配置
 */
async function applyFlexibleMapping() {
    try {
        const flexibleMappingContainer = document.querySelector('.flexible-mapping-container');
        if (!flexibleMappingContainer) {
            const errorMsg = '矩阵配置区域未加载，请刷新页面重试';
            showError(errorMsg, 3000);
            addToCommandLog('配置失败', 'error', errorMsg);
            return;
        }

        const getElement = (id) => flexibleMappingContainer.querySelector(`#${id}`);

        const canvasWidthInput = getElement('flexible-canvas-width');
        const canvasHeightInput = getElement('flexible-canvas-height');
        const canvasRowsInput = getElement('flexible-canvas-rows');
        const canvasColsInput = getElement('flexible-canvas-cols');
        // 输出分辨率和输出布局是矩阵配置的一部分
        const outputWidthInput = getElement('flexible-output-width');
        const outputHeightInput = getElement('flexible-output-height');
        const outputRowsInput = getElement('flexible-output-rows');
        const outputColsInput = getElement('flexible-output-cols');
        // 旋转角度是矩阵配置的一部分
        const rotationInput = getElement('flexible-canvas-rotation');

        // 验证必要的输入框是否存在（矩阵配置含输出分辨率与布局）
        if (!canvasWidthInput || !canvasHeightInput || !canvasColsInput || !canvasRowsInput ||
            !outputWidthInput || !outputHeightInput || !outputColsInput || !outputRowsInput) {
            const errorMsg = '找不到配置输入框，请刷新页面重试';
            showError(errorMsg, 3000);
            addToCommandLog('配置失败', 'error', errorMsg);
            return;
        }

        // 先从输入框读取最新值并更新配置
        // 注意：必须从输入框读取，因为用户可能修改了值但还没有触发change事件
        // 使用严格的验证，确保值有效
        const readRequiredInteger = (input, fieldName, minValue = 1) => {
            if (!input || !input.value || input.value.trim() === '') {
                throw new Error(`${fieldName}不能为空`);
            }
            const val = Math.floor(Number(input.value));
            if (isNaN(val) || val < minValue) {
                throw new Error(`${fieldName}必须是大于等于${minValue}的整数`);
            }
            return val;
        };

        const readOptionalNumber = (input, defaultValue = 0) => {
            if (!input || !input.value || input.value.trim() === '') {
                return defaultValue;
            }
            const val = Number(input.value);
            return isNaN(val) ? defaultValue : val;
        };

        // 读取并验证必需的配置值（矩阵配置包括输出参数和旋转角度）
        let canvasWidth, canvasHeight, canvasCols, canvasRows;
        let outputWidth, outputHeight, outputCols, outputRows;

        try {
            canvasWidth = readRequiredInteger(canvasWidthInput, '幕布分辨率宽度');
            canvasHeight = readRequiredInteger(canvasHeightInput, '幕布分辨率高度');
            canvasRows = readRequiredInteger(canvasRowsInput, '幕布布局行数');
            canvasCols = readRequiredInteger(canvasColsInput, '幕布布局列数');
            validateLayoutSizeOrThrow(canvasRows, canvasCols, '输入布局');
            enforceLicensedInputLayoutOrThrow(canvasRows, canvasCols);
            outputWidth = readRequiredInteger(outputWidthInput, '输出分辨率宽度');
            outputHeight = readRequiredInteger(outputHeightInput, '输出分辨率高度');
            outputRows = readRequiredInteger(outputRowsInput, '输出布局行数');
            outputCols = readRequiredInteger(outputColsInput, '输出布局列数');
            validateLayoutSizeOrThrow(outputRows, outputCols, '输出布局');
        } catch (error) {
            const errorMsg = error.message || '配置参数验证失败';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }

        // 读取旋转角度（矩阵配置的一部分，0/90/180/270 或 0-360 取整）
        const rawRotation = readOptionalNumber(rotationInput, 0);
        const rotationAngle = Math.min(360, Math.max(0, Math.round(Number(rawRotation))));

        // 更新当前配置
        currentMappingConfig.canvas.width = canvasWidth;
        currentMappingConfig.canvas.height = canvasHeight;
        currentMappingConfig.canvas.gridRows = canvasRows;
        currentMappingConfig.canvas.gridCols = canvasCols;
        currentMappingConfig.output.width = outputWidth;
        currentMappingConfig.output.height = outputHeight;
        currentMappingConfig.output.gridRows = outputRows;
        currentMappingConfig.output.gridCols = outputCols;

        const totalInputRegions = canvasCols * canvasRows;
        const totalOutputRegions = outputCols * outputRows;
        if (canvasWidth % canvasCols !== 0 || canvasHeight % canvasRows !== 0) {
            const errorMsg = '幕布分辨率必须能被幕布布局整除';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }
        if (outputWidth % outputCols !== 0 || outputHeight % outputRows !== 0) {
            const errorMsg = '输出分辨率必须能被输出布局整除';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }
        const mappingsToSave = buildMappingsForSave(
            currentMappingConfig.mappings,
            totalInputRegions,
            totalOutputRegions
        );
        currentMappingConfig.mappings = mappingsToSave;

        // split_direction: 0=水平分割, 1=垂直分割
        // 默认使用水平分割（从左到右排列），如果需要垂直分割，需要用户配置
        // 这里先使用水平分割，后续如果需要可以添加UI让用户选择
        const splitDirection = 0; // 默认水平分割

        // 使用已验证的值构建请求数据（矩阵配置包括输出参数和旋转角度）
        const config = {
            canvas_in_width: canvasWidth,
            canvas_in_height: canvasHeight,
            layout_in_cols: canvasCols,
            layout_in_rows: canvasRows,
            canvas_out_width: outputWidth,
            canvas_out_height: outputHeight,
            layout_out_cols: outputCols,
            layout_out_rows: outputRows,
            rotation_angle: rotationAngle,
            split_direction: splitDirection,
            mappings: mappingsToSave.map(m => ({
                in_id: m.inputRegionId,
                out_idx: m.enabled ? m.outputIndex : -1,
                enabled: Boolean(m.enabled && m.outputIndex >= 0)
            }))
        };

        // 验证配置
        if (config.canvas_in_width <= 0 || config.canvas_in_height <= 0) {
            const errorMsg = '幕布分辨率必须大于0';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }

        if (config.layout_in_cols <= 0 || config.layout_in_rows <= 0) {
            const errorMsg = '幕布布局必须大于0';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }

        // 验证输出参数（矩阵配置的一部分）
        if (config.canvas_out_width <= 0 || config.canvas_out_height <= 0) {
            const errorMsg = '输出分辨率必须大于0';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }

        if (config.layout_out_cols <= 0 || config.layout_out_rows <= 0) {
            const errorMsg = '输出布局必须大于0';
            showError(errorMsg, 3000);
            addToCommandLog('配置错误', 'error', errorMsg);
            return;
        }

        // 后端会按同一事务语义完成运行时应用与 config.json 持久化。
        addToCommandLog(
            '配置保存',
            'info',
            `提交矩阵: 输入${canvasWidth}×${canvasHeight} 布局${formatLayout(canvasRows, canvasCols)}，输出${outputWidth}×${outputHeight} 布局${formatLayout(outputRows, outputCols)}`
        );
        const response = await apiAction('regions', 'set_flexible_mapping', config, MATRIX_SAVE_TIMEOUT_MS);

        if (response === null || response === undefined) {
            const err = getLastApiError();
            const msg = err?.reason === 'timeout'
                ? '保存请求超时，后端可能仍在重建画布；请刷新读取后端状态，不会自动重复提交'
                : ((err && err.message) ? err.message : '服务器无响应，请检查网络连接');
            showError(`保存失败：${msg}`, 5000);
            addToCommandLog('配置失败', 'error', msg);
            return;
        }

        const responseData = response;
        const hasBackendConfirmation =
            responseData && typeof responseData === 'object' &&
            responseData.saved === true &&
            responseData.applied === true;
        const matchResult = hasBackendConfirmation
            ? responseMatchesConfig(responseData, config)
            : { ok: false, message: '' };
        const matchesSubmittedConfig = hasBackendConfirmation && matchResult.ok;

        if (matchesSubmittedConfig) {
            const successMessage = `矩阵配置已保存并生效: 输入布局${formatLayout(responseData.layout_in_rows, responseData.layout_in_cols)}，输出布局${formatLayout(responseData.layout_out_rows, responseData.layout_out_cols)}`;

            // 显示明显的成功提示
            showSuccess(successMessage, 3000);

            addToCommandLog('配置成功', 'success', successMessage);

            // 使用后端已保存并应用的配置回填界面，避免前后端状态漂移。
            applyRegionDataToCurrentConfig(responseData);

            // 确保UI与保存的配置一致（重新渲染）
            updateInputFields();
            renderAll();

            await loadFlexibleMapping();
            window.dispatchEvent(new CustomEvent('hsvj:flexible-mapping-updated', {
                detail: { config: responseData }
            }));
            if (window.hsFusion?.getInited?.() && window.hsFusion?.refreshLayout) {
                void window.hsFusion.refreshLayout({ preserveActive: true });
            }
        } else {
            const backendError = responseData?.message || '后端未确认保存并生效';
            const errorMsg = hasBackendConfirmation && !matchResult.ok
                ? `后端返回配置与提交不一致: ${matchResult.message}`
                : backendError;

            // 显示明显的错误提示
            showError(`保存失败: ${errorMsg}`, 4000);

            addToCommandLog('配置失败', 'error', `保存失败: ${errorMsg}`);
        }
    } catch (error) {
        const errorMessage = `保存异常: ${error.message}`;

        // 显示明显的错误提示
        showError(errorMessage, 4000);

        addToCommandLog('配置失败', 'error', errorMessage);
    }
}

/**
 * 重置矩阵配置
 * 从配置文件重新加载，而不是使用硬编码的默认值
 */
async function resetFlexibleMapping() {
    try {
        addToCommandLog('重置配置', 'info', '正在从配置文件重新加载...');

        // 从后端重新加载配置
        await loadFlexibleMapping();

        addToCommandLog('重置配置', 'success', '矩阵配置已从配置文件重新加载');
    } catch (error) {
        addToCommandLog('重置配置', 'error', '重新加载配置文件失败: ' + error.message);
    }
}
