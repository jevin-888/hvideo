import { apiGet, apiPost, apiPut, apiDelete } from '../core/api.js';
import { roundNumbersInObject } from '../utils/configUtils.js';
import { addToCommandLog } from '../core/commandLog.js';
import { clearContainer } from '../utils/domHelpers.js';
import { toggleButtonGroupActive } from '../components/buttonGroups.js';
import { showConfirm } from '../components/toast.js';

// 全局变量，用于跟踪当前选中的场景
let selectedScene = null;

// 初始化场景模板列表
export function initializeSceneTemplates() {
    updateSceneTemplates();
    initializeDefaultConfigEditor(); // 新增：初始化 config.json 编辑器
    initializeSceneTabs();
    initializeSceneEditor();
}

// 初始化系统核心配置编辑器 (config.json)
function initializeDefaultConfigEditor() {
    const loadBtn = document.getElementById('load-config-json-btn');
    const saveBtn = document.getElementById('save-config-json-btn');
    const editor = document.getElementById('config-json-editor');

    if (loadBtn) {
        loadBtn.addEventListener('click', async () => {
            const data = await apiGet('/config.json');
            if (data) {
                editor.value = JSON.stringify(data, null, 4);
                addToCommandLog('读取系统配置', 'info', '成功从服务器读取 config.json');
            } else {
                addToCommandLog('读取系统配置', 'error', '无法从服务器读取 config.json');
            }
        });
    }

    if (saveBtn) {
        saveBtn.addEventListener('click', async () => {
            const content = editor.value.trim();
            if (!content) {
                window.hsUtils.showNotification('请输入 JSON 内容', 'warning');
                return;
            }

            try {
                const json = JSON.parse(content);
                if (typeof json !== 'object' || Array.isArray(json)) {
                    window.hsUtils.showNotification('config.json 必须是一个 JSON 对象', 'error');
                    return;
                }

                const payload = roundNumbersInObject(json);
                const result = await apiPost('/config.json', payload);

                // API 成功时返回业务 data，失败时返回 null。
                if (result != null) {
                    window.hsUtils.showNotification('系统配置保存成功，建议重启软件', 'success');
                    addToCommandLog('保存系统配置', 'success', 'config.json 已更新到服务器');
                } else {
                    const msg = (result && result.message) || '保存失败';
                    window.hsUtils.showNotification(msg, 'error');
                }
            } catch (e) {
                window.hsUtils.showNotification('JSON 格式错误: ' + e.message, 'error');
            }
        });
    }
}

// 初始化场景编辑器按钮
function initializeSceneEditor() {
    const refreshBtn = document.getElementById('refresh-scene-btn');
    const saveBtn = document.getElementById('save-scene-btn');

    if (refreshBtn) {
        refreshBtn.addEventListener('click', () => {
            if (selectedScene) {
                showSceneDetails(selectedScene);
            } else {
                window.hsUtils.showNotification('请先从左侧选择一个场景', 'warning');
            }
        });
    }

    if (saveBtn) {
        saveBtn.addEventListener('click', async () => {
            if (!selectedScene) {
                window.hsUtils.showNotification('没有选中的场景', 'warning');
                return;
            }

            const editor = document.getElementById('scene-json-editor');
            const content = editor.value.trim();

            if (!content) {
                window.hsUtils.showNotification('场景内容不能为空', 'warning');
                return;
            }

            try {
                const jsonContent = JSON.parse(content);
                const encodedSceneName = encodeURIComponent(selectedScene);
                const result = await apiPut(`/scenes/${encodedSceneName}`, {
                    content: jsonContent
                });

                if (result != null) {
                    window.hsUtils.showNotification('场景保存成功', 'success');
                    addToCommandLog('保存场景模板', 'success', `场景 ${selectedScene} 已更新`);
                } else {
                    const msg = (result && result.message) || '保存失败';
                    window.hsUtils.showNotification(msg, 'error');
                }
            } catch (e) {
                window.hsUtils.showNotification('JSON 格式错误: ' + e.message, 'error');
            }
        });
    }
}

// 初始化场景页面内部标签页
function initializeSceneTabs() {
    const tabs = document.querySelectorAll('[data-scene-tab]');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const targetTabId = tab.dataset.sceneTab;

            // 切换按钮状态
            tabs.forEach(t => t.classList.remove('active'));
            tab.classList.add('active');

            // 切换内容显隐
            document.querySelectorAll('.scene-tab').forEach(content => {
                content.classList.remove('active');
            });
            const targetContent = document.getElementById(targetTabId);
            if (targetContent) {
                targetContent.classList.add('active');
            }
        });
    });
}

// 更新场景模板列表
export async function updateSceneTemplates() {
    const scenes = await apiGet('/scenes');
    if (scenes) {
        const container = document.getElementById('template-list-container');
        if (!container) return;
        clearContainer(container);

        scenes.forEach(scene => {
            const sceneItem = document.createElement('div');
            sceneItem.className = 'template-item';
            sceneItem.dataset.sceneName = scene;
            
            // 判断是否为"默认配置"（不可删除）
            const isDefaultConfig = scene === '默认配置';
            
            sceneItem.innerHTML = `
                <div class="template-name">${scene}</div>
                <div class="template-meta">
                    <span>模板</span>
                    ${!isDefaultConfig ? '<button class="delete-scene-btn" title="删除场景">×</button>' : ''}
                </div>
            `;

            // 点击场景项选择场景
            sceneItem.addEventListener('click', function (e) {
                // 如果点击的是删除按钮，不触发选择
                if (e.target.classList.contains('delete-scene-btn')) {
                    return;
                }
                selectScene(scene);
            });

            // 添加删除按钮事件（如果不是默认配置）
            if (!isDefaultConfig) {
                const deleteBtn = sceneItem.querySelector('.delete-scene-btn');
                if (deleteBtn) {
                    deleteBtn.addEventListener('click', async function (e) {
                        e.stopPropagation(); // 阻止事件冒泡到父元素
                        
                        if (await showConfirm(`确定要删除场景模板 "${scene}" 吗？`, '删除场景模板')) {
                            const encodedSceneName = encodeURIComponent(scene);
                            const result = await apiDelete(`/scenes/${encodedSceneName}`);
                            
                            if (result != null) {
                                window.hsUtils.showNotification(`场景 "${scene}" 已删除`, 'success');
                                addToCommandLog('删除场景模板', 'success', `已删除场景: ${scene}`);
                                
                                // 如果删除的是当前选中的场景，清空选择
                                if (selectedScene === scene) {
                                    selectedScene = null;
                                    const editor = document.getElementById('scene-json-editor');
                                    const label = document.getElementById('current-scene-name-label');
                                    if (editor) editor.value = '';
                                    if (label) label.textContent = '未选择场景';
                                }
                                
                                // 刷新场景列表
                                updateSceneTemplates();
                            } else {
                                const msg = (result && result.message) || '删除失败';
                                window.hsUtils.showNotification(msg, 'error');
                                addToCommandLog('删除场景模板', 'error', `删除场景失败: ${scene}`);
                            }
                        }
                    });
                }
            }

            container.appendChild(sceneItem);
        });
    }
}

// 选择场景模板
export function selectScene(sceneName) {
    // 使用工具函数更新选中状态
    const sceneItem = Array.from(document.querySelectorAll('.template-item'))
        .find(item => item.dataset.sceneName === sceneName);
    if (sceneItem) {
        toggleButtonGroupActive('.template-item', sceneItem);
        selectedScene = sceneName;
        showSceneDetails(sceneName);
        addToCommandLog('选择场景模板', 'info', `选择场景模板: ${sceneName}`);
    }
}

// 显示场景详情
async function showSceneDetails(sceneName) {
    const editor = document.getElementById('scene-json-editor');
    const label = document.getElementById('current-scene-name-label');

    if (label) label.textContent = `正在编辑: ${sceneName}`;

    const encodedSceneName = encodeURIComponent(sceneName);
    const scene = await apiGet(`/scenes/${encodedSceneName}`);
    if (scene) {
        if (editor) {
            editor.value = JSON.stringify(scene, null, 4);
        }
    } else {
        if (editor) editor.value = '无法加载场景详情';
    }
}
