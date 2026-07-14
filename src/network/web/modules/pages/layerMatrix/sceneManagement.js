// 场景管理模块
import { apiGet, apiDelete } from '../../core/api.js';
import { sendSceneCommand } from '../../core/commandHelper.js';
import { addToCommandLog } from '../../core/commandLog.js';
import { showPrompt, showSuccess, showError, showConfirm } from '../../components/toast.js';
import { handleApiOperation } from '../../utils/apiHelpers.js';

let currentLoadedSceneName = '默认配置';

export function isDefaultSceneName(sceneName) {
    return !sceneName || sceneName === '默认配置' || sceneName === 'default' || sceneName === 'default.json';
}

function normalizeSceneName(sceneName) {
    if (!sceneName || typeof sceneName !== 'string') return '默认配置';
    if (isDefaultSceneName(sceneName)) return '默认配置';
    return sceneName.endsWith('.json') ? sceneName.slice(0, -5) : sceneName;
}

export function getCurrentSceneName() {
    return normalizeSceneName(currentLoadedSceneName);
}

export function setCurrentSceneName(sceneName) {
    currentLoadedSceneName = normalizeSceneName(sceneName);
}

export async function syncCurrentSceneFromServer() {
    try {
        const response = await apiGet('/scenes/current');
        const sceneName = response?.current_scene || response?.scene_name || response?.name;
        if (sceneName) {
            currentLoadedSceneName = normalizeSceneName(sceneName);
        }
    } catch (error) {
        console.warn('syncCurrentSceneFromServer failed:', error);
    }
    return getCurrentSceneName();
}

export async function resolveSceneNameForSave(dropdownSceneName = '') {
    const loadedScene = await syncCurrentSceneFromServer();
    return normalizeSceneName(loadedScene || dropdownSceneName);
}

/**
 * 刷新场景下拉菜单
 */
export async function refreshSceneDropdown(selectId) {
    const select = document.getElementById(selectId);
    if (!select) {
        console.error('refreshSceneDropdown: Select element not found', selectId);
        return;
    }

    addToCommandLog('场景管理', 'info', '正在刷新场景列表...');
    const serverSceneName = await syncCurrentSceneFromServer();

    // Function to 渲染 HTML directly (Bypassing Option constructor)
    const renderHTML = (list) => {
        const currentVal = normalizeSceneName(currentLoadedSceneName || select.value || serverSceneName);
        const safeList = Array.isArray(list) ? list : [];

        // 默认选中"默认配置"，不显示"选择模板..."占位符
        const defaultSelected = isDefaultSceneName(currentVal);
        let html = `<option value="默认配置" ${defaultSelected ? 'selected' : ''}>默认配置</option>`;

        safeList.forEach(scene => {
            const normalizedScene = normalizeSceneName(scene);
            if (isDefaultSceneName(normalizedScene)) return;
            const selected = (normalizedScene === currentVal) ? 'selected' : '';
            html += `<option value="${normalizedScene}" ${selected}>${normalizedScene}</option>`;
        });

        select.innerHTML = html;
        select.dispatchEvent(new CustomEvent('scene-options-updated', { bubbles: true }));

    };

    // 1. Initial 渲染 (HTML injection)
    renderHTML([]);

    try {
        const response = await apiGet('/scenes');

        const scenes = Array.isArray(response) ? response : [];

        // 2. Final 渲染
        renderHTML(scenes);

        const count = scenes.length;
        if (count > 0) {
            addToCommandLog('场景管理', 'success', `场景列表已更新 (${count} 个)`);
        } else {
            addToCommandLog('场景管理', 'success', '场景列表已更新 (无自定义场景)');
        }

    } catch (error) {
        console.error('Failed to load scenes:', error);
        addToCommandLog('场景管理', 'error', '获取场景列表失败');
    }
}

/**
 * 加载默认模板
 */
export async function loadDefaultScene(selectedLayer, selectedSlice, updateLayerMatrix) {
    addToCommandLog('场景管理', 'info', '正在加载默认模板...');

    const success = await handleApiOperation(
        sendSceneCommand('load_default'),
        '场景管理',
        '默认模板已加载',
        '加载默认模板失败',
        async (result) => {
            currentLoadedSceneName = '默认配置';
            const sceneDropdown = document.getElementById('select-template-dropdown');
            if (sceneDropdown) sceneDropdown.value = '默认配置';

            selectedLayer = null;
            selectedSlice = null;

            await updateLayerMatrix();

            const propertiesPanel = document.querySelector('.properties-panel');
            if (propertiesPanel) {
                propertiesPanel.innerHTML = '<div class="property-placeholder">选择图层查看属性</div>';
            }

            const sliceInfo = document.querySelector('.slice-info');
            if (sliceInfo) {
                sliceInfo.innerHTML = '';
            }
        }
    );
}

/**
 * 保存当前配置
 */
export async function saveDefaultScene(layers = []) {
    const sceneDropdown = document.getElementById('select-template-dropdown');
    const currentScene = await resolveSceneNameForSave(sceneDropdown?.value || '');
    if (!isDefaultSceneName(currentScene)) {
        return saveSceneByName(currentScene);
    }

    addToCommandLog('场景管理', 'info', '[保存步骤1] 用户点击保存按钮，开始保存配置...');

    try {
        const result = await sendSceneCommand('save_current');
        addToCommandLog('场景管理', 'info', `[保存步骤3] save_current 响应: ${JSON.stringify(result)}`);

        if (!result || typeof result !== 'object' || Array.isArray(result)) {
            const errorMsg = '保存失败：后端未返回保存结果';
            addToCommandLog('场景管理', 'error', `[保存步骤5-失败] ${errorMsg}`);
            showError(errorMsg);
            return false;
        }

        // 检查是否有config_path
        const configPath = result.configPath || result.config_path || '';
        if (!configPath) {
            addToCommandLog('场景管理', 'warning', `[保存步骤5-警告] 响应中缺少configPath字段`);
            // 即使没有config_path，如果成功也认为保存成功
            const successMsg = '配置已保存';
            addToCommandLog('场景管理', 'success', `[保存步骤5-成功] ${successMsg}`);
            showSuccess(successMsg);
            return true;
        }

        const successMsg = '配置已保存';
        addToCommandLog('场景管理', 'success', `[保存步骤5-成功] ${successMsg}: ${configPath}`);
        showSuccess(successMsg);
        return true;

    } catch (error) {
        const errorMsg = `保存失败：${error.message || '未知错误'}`;
        addToCommandLog('场景管理', 'error', `[保存步骤3-异常] API调用异常: ${error.message || '未知错误'}`);
        addToCommandLog('场景管理', 'error', `[保存步骤4] 错误堆栈: ${error.stack || '无堆栈信息'}`);
        showError(errorMsg);
        return false;
    }
}

/**
 * 显示场景选择器
 */
export async function showSceneSelector(selectedLayer, selectedSlice, updateLayerMatrix) {
    try {
        const response = await apiGet('/scenes');
        let scenes = Array.isArray(response) ? response : [];

        // 确保默认配置排在最前
        scenes = scenes.filter(s => s !== '默认配置' && s !== 'default' && s !== 'default.json');
        scenes.unshift('默认配置');

        if (scenes.length === 0) {
            addToCommandLog('场景管理', 'info', '暂无保存的场景');
            return;
        }

        const dialog = document.createElement('div');
        dialog.className = 'scene-dialog';
        dialog.innerHTML = `
            <div class="scene-dialog-content" style="width: 320px; padding: 0; background: #1e1e2d; border: 1px solid rgba(255,255,255,0.1); border-radius: 8px; overflow: hidden; box-shadow: 0 10px 40px rgba(0,0,0,0.6);">
                <div style="padding: 12px 15px; background: rgba(255,255,255,0.05); border-bottom: 1px solid rgba(255,255,255,0.1); display: flex; justify-content: space-between; align-items: center;">
                    <h3 style="margin: 0; font-size: 14px; font-weight: 600; color: #fff;">切换场景</h3>
                    <button class="close-dialog" style="background: none; border: none; color: #888; cursor: pointer; font-size: 18px; padding: 0; display: flex;">&times;</button>
                </div>
                <div class="scene-list" style="max-height: 400px; overflow-y: auto; padding: 5px;">
                    ${scenes.map(scene => {
            const isDefault = scene === '默认配置' || scene === 'default' || scene === 'default.json';
            return `
                        <div class="scene-item dropdown-item" data-scene="${scene}" style="
                            position: relative; 
                            padding: 10px 12px; 
                            margin-bottom: 2px;
                            border-radius: 4px;
                            cursor: pointer; 
                            color: #ddd; 
                            display: flex; 
                            align-items: center; 
                            transition: all 0.2s;
                        ">
                            <span class="scene-item-icon" style="margin-right: 10px; opacity: 0.6; font-size: 14px;">📄</span>
                            <span class="scene-name" style="flex: 1; font-size: 13px; font-weight: 500;">${scene}</span>
                            ${!isDefault ?
                    `<button class="delete-scene-icon" title="删除" style="
                                    width: 28px; 
                                    height: 28px; 
                                    border: none; 
                                    background: rgba(255, 77, 79, 0.15); 
                                    color: #ff4d4f; 
                                    font-size: 20px; 
                                    font-weight: bold;
                                    line-height: 1;
                                    cursor: pointer; 
                                    display: flex !important; 
                                    align-items: center; 
                                    justify-content: center;
                                    opacity: 1 !important;
                                    transition: all 0.2s;
                                    border-radius: 4px;
                                    flex-shrink: 0;
                                ">&times;</button>` : ''
                }
                        </div>
                    `;
        }).join('')}
                </div>
            </div>
        `;

        document.body.appendChild(dialog);

        // 说明：添加内联样式以支持交互
        const style = document.createElement('style');
        style.innerHTML = `
            .scene-dialog {
                position: fixed;
                top: 0; left: 0; right: 0; bottom: 0;
                background: rgba(0,0,0,0.4);
                display: flex;
                align-items: center;
                justify-content: center;
                z-index: 9999;
                backdrop-filter: blur(2px);
                animation: fadeIn 0.15s ease-out;
            }
            @keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }
            
            .dropdown-item:hover {
                background-color: rgba(102, 126, 234, 0.2); 
                color: #fff;
            }
            .dropdown-item:hover .scene-item-icon {
                opacity: 1;
            }
            .dropdown-item:hover .delete-scene-icon {
                opacity: 1 !important;
                background: rgba(255, 77, 79, 0.25) !important;
            }
            .delete-scene-icon {
                opacity: 1 !important;
                display: flex !important;
            }
            .delete-scene-icon:hover {
                background-color: rgba(255, 77, 79, 0.35) !important;
                opacity: 1 !important;
                transform: scale(1.15);
            }
            .close-dialog:hover {
                color: #fff !important;
            }
        `;
        dialog.appendChild(style);

        // 说明：绑定行点击以加载场景
        dialog.querySelectorAll('.scene-item').forEach(item => {
            item.addEventListener('click', async (e) => {
                const sceneName = item.dataset.scene;
                await loadScene(sceneName, selectedLayer, selectedSlice, updateLayerMatrix);
                dialog.remove(); // 关闭 after loading
            });
        });

        // 说明：绑定删除按钮并阻止事件冒泡
        dialog.querySelectorAll('.delete-scene-icon').forEach(btn => {
            btn.addEventListener('click', async (e) => {
                e.stopPropagation(); // 说明：避免触发行点击
                e.preventDefault();

                if (!await showConfirm('确定要删除该场景吗？', '删除场景')) return;

                const item = e.target.closest('.scene-item');
                const sceneName = item.dataset.scene;

                await deleteScene(sceneName);
                item.remove();
            });
        });

        // 关闭 button
        dialog.querySelector('.close-dialog').addEventListener('click', () => dialog.remove());

        // 关闭 on clicking outside the content area
        dialog.addEventListener('click', (e) => {
            if (e.target === dialog) dialog.remove();
        });

    } catch (error) {
        addToCommandLog('场景管理', 'error', `获取场景列表失败: ${error.message}`);
    }
}

/**
 * 加载场景
 */
export async function loadScene(sceneName, selectedLayer, selectedSlice, updateLayerMatrix) {
    sceneName = normalizeSceneName(sceneName);
    addToCommandLog('场景管理', 'info', `正在加载场景: ${sceneName}...`);

    await handleApiOperation(
        sendSceneCommand('switch_scene', { scene_name: sceneName }),
        '场景管理',
        `已加载场景: ${sceneName}`,
        '加载场景失败',
        async (result) => {
            document.querySelector('.scene-dialog')?.remove();
            currentLoadedSceneName = sceneName;
            const sceneDropdown = document.getElementById('select-template-dropdown');
            if (sceneDropdown) sceneDropdown.value = sceneName;

            selectedLayer = null;
            selectedSlice = null;

            await updateLayerMatrix();

            const propertiesPanel = document.querySelector('.properties-panel');
            if (propertiesPanel) {
                propertiesPanel.innerHTML = '<div class="property-placeholder">选择图层查看属性</div>';
            }

            const sliceInfo = document.querySelector('.slice-info');
            if (sliceInfo) {
                sliceInfo.innerHTML = '';
            }
        }
    );
}

/**
 * 删除场景
 */
export async function deleteScene(sceneName) {
    // 对场景名称进行 URL 编码，确保中文字符正确处理
    const encodedSceneName = encodeURIComponent(sceneName);
    await handleApiOperation(
        apiDelete(`/scenes/${encodedSceneName}`),
        '场景管理',
        `已删除场景: ${sceneName}`,
        '删除场景失败'
    );
}

/**
 * 保存当前场景
 */
export async function saveCurrentScene(layers) {
    const sceneName = await showPrompt('请输入场景名称:', '保存场景', '场景1');
    if (!sceneName) return null;

    const saved = await saveSceneByName(sceneName);
    return saved ? sceneName : null;
}

async function saveSceneByName(sceneName) {
    sceneName = normalizeSceneName(sceneName);
    try {
        addToCommandLog('场景管理', 'info', `正在保存场景: ${sceneName}...`);
        const saved = await handleApiOperation(
            sendSceneCommand('save_as', { scene_name: sceneName }),
            '场景管理',
            `已保存场景: ${sceneName}`,
            '保存场景失败'
        );
        if (saved) {
            currentLoadedSceneName = sceneName;
            const sceneDropdown = document.getElementById('select-template-dropdown');
            if (sceneDropdown) sceneDropdown.value = sceneName;
        }
        return saved;
    } catch (error) {
        addToCommandLog('场景管理', 'error', `保存场景失败: ${error.message}`);
        return false;
    }
}
