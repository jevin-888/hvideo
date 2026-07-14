/**
 * @file toast.js（文件名）
 * @brief 提示框和弹窗工具模块
 * 
 * 本模块提供统一的提示框和弹窗功能，包括：
 * - Toast提示框（成功、错误、警告、信息）
 * - 确认对话框
 * - 进度提示
 */

import { parseApiResponse } from '../utils/apiResponseParser.js';

/**
 * @brief 创建提示框容器（如果不存在）
 * @returns {HTMLElement} 提示框容器元素
 */
function ensureToastContainer() {
    let container = document.getElementById('toast-container');
    if (!container) {
        container = document.createElement('div');
        container.id = 'toast-container';
        container.className = 'toast-container';
        document.body.appendChild(container);
    }
    return container;
}

// 创建确认对话框容器（如果不存在）
function ensureConfirmDialog() {
    let dialog = document.getElementById('confirm-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'confirm-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content">
                <h3>确认操作</h3>
                <div class="dialog-body">
                    <p id="confirm-message"></p>
                    <div id="confirm-progress" style="display: none; margin-top: 16px;">
                        <div style="display: flex; align-items: center; gap: 12px;">
                            <div class="spinner" style="width: 20px; height: 20px; border: 2px solid rgba(255,255,255,0.2); border-top-color: #667eea; border-radius: 50%; animation: spin 0.8s linear infinite;"></div>
                            <span id="confirm-progress-text" style="color: var(--color-text-main); font-size: 14px;">正在删除...</span>
                        </div>
                    </div>
                </div>
                <div class="dialog-footer">
                    <button id="confirm-cancel-btn" class="btn">取消</button>
                    <button id="confirm-ok-btn" class="btn primary">确定</button>
                </div>
            </div>
        `;
        // 添加旋转动画
        if (!document.getElementById('dialog-spinner-style')) {
            const style = document.createElement('style');
            style.id = 'dialog-spinner-style';
            style.textContent = `
                @keyframes spin {
                    to { transform: rotate(360deg); }
                }
            `;
            document.head.appendChild(style);
        }
        document.body.appendChild(dialog);

        // 绑定事件
        dialog.querySelector('#confirm-cancel-btn').addEventListener('click', () => {
            // 如果正在显示进度，不关闭对话框
            const progressEl = dialog.querySelector('#confirm-progress');
            if (progressEl && progressEl.style.display === 'block') {
                return;
            }
            // 先保存回调引用，因为 hideConfirmDialog 会清除它们
            const resolveCallback = dialog._resolve;
            hideConfirmDialog();
            if (resolveCallback) {
                resolveCallback(false);
            }
        });

        dialog.querySelector('#confirm-ok-btn').addEventListener('click', () => {
            // 如果设置了需要显示进度，不立即关闭对话框
            if (dialog._showProgress) {
                // 只resolve，不关闭对话框，由调用者控制关闭
                if (dialog._resolve) {
                    dialog._resolve(true);
                    // 注意：这里不清除resolve，以便后续可以继续使用
                }
                return;
            }
            // 先保存回调引用，因为 hideConfirmDialog 会清除它们
            const resolveCallback = dialog._resolve;
            hideConfirmDialog();
            if (resolveCallback) {
                resolveCallback(true);
            }
        });

        // 点击遮罩层关闭（如果不在显示进度）
        dialog.addEventListener('click', (e) => {
            if (e.target === dialog) {
                const progressEl = dialog.querySelector('#confirm-progress');
                if (progressEl && progressEl.style.display === 'block') {
                    return; // 显示进度时不允许关闭
                }
                // 先保存回调引用，因为 hideConfirmDialog 会清除它们
                const resolveCallback = dialog._resolve;
                hideConfirmDialog();
                if (resolveCallback) {
                    resolveCallback(false);
                }
            }
        });
    }
    return dialog;
}

/**
 * 显示提示消息（Toast）
 * @param {string} message - 提示消息
 * @param {string} type - 类型：'success', 'error', 'warning', 'info'
 * @param {number} duration - 显示时长（毫秒），默认3000
 */
export function showToast(message, type = 'info', duration = 3000) {
    const container = ensureToastContainer();

    // 如果已有Toast正在显示，先关闭它
    const existingToasts = container.querySelectorAll('.toast');
    existingToasts.forEach(existingToast => {
        hideToast(existingToast);
    });

    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;

    const toastIconSvg = {
        success: '<svg class="toast-icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>',
        error: '<svg class="toast-icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>',
        warning: '<svg class="toast-icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>',
        info: '<svg class="toast-icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>'
    };

    toast.innerHTML = `
        <span class="toast-icon">${toastIconSvg[type] || toastIconSvg.info}</span>
        <span class="toast-message">${escapeHtml(message)}</span>
    `;

    container.appendChild(toast);

    // 触发动画
    requestAnimationFrame(() => {
        toast.classList.add('show');
    });

    // 自动关闭
    const timer = setTimeout(() => {
        hideToast(toast);
    }, duration);

    // 点击关闭
    toast.addEventListener('click', () => {
        clearTimeout(timer);
        hideToast(toast);
    });

    return toast;
}

/**
 * 隐藏提示消息
 */
function hideToast(toast) {
    toast.classList.remove('show');
    toast.classList.add('hide');
    setTimeout(() => {
        if (toast.parentNode) {
            toast.parentNode.removeChild(toast);
        }
    }, 300);
}

/**
 * 显示确认对话框（替换confirm）
 * @param {string} message - 确认消息
 * @param {string} title - 对话框标题，默认"确认操作"
 * @param {boolean} showProgress - 是否在确认后显示进度（不立即关闭对话框）
 * @returns {Promise<boolean>} - 用户点击确定返回true，取消返回false
 */
export function showConfirm(message, title = '确认操作', showProgress = false) {
    return new Promise((resolve, reject) => {
        const dialog = ensureConfirmDialog();
        dialog.querySelector('#confirm-message').textContent = message;
        dialog.querySelector('h3').textContent = title;

        dialog._resolve = resolve;
        dialog._reject = reject;
        dialog._showProgress = showProgress; // 标记是否需要显示进度

        // 确保进度区域和按钮状态正确
        const progressEl = dialog.querySelector('#confirm-progress');
        const footer = dialog.querySelector('.dialog-footer');
        if (progressEl) {
            progressEl.style.display = 'none';
        }
        if (footer) {
            footer.style.display = 'flex';
        }

        dialog.classList.add('active');
    });
}

// 创建输入对话框容器（如果不存在）
function ensurePromptDialog() {
    let dialog = document.getElementById('prompt-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'prompt-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content">
                <h3>输入</h3>
                <div class="dialog-body">
                    <p id="prompt-message"></p>
                    <div class="dialog-input-row">
                        <input id="prompt-input" class="form-control" type="text" />
                    </div>
                </div>
                <div class="dialog-footer">
                    <button id="prompt-cancel-btn" class="btn">取消</button>
                    <button id="prompt-ok-btn" class="btn primary">确定</button>
                </div>
            </div>
        `;
        document.body.appendChild(dialog);

        dialog.querySelector('#prompt-cancel-btn').addEventListener('click', () => {
            const resolveCallback = dialog._resolve;
            hidePromptDialog();
            if (resolveCallback) {
                resolveCallback(null);
            }
        });

        dialog.querySelector('#prompt-ok-btn').addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            const input = dialog.querySelector('#prompt-input');
            const value = input ? String(input.value) : '';
            const resolveCallback = dialog._resolve;
            if (resolveCallback) {
                resolveCallback(value);
            }
            hidePromptDialog();
        });

        // 仅在按下和抬起都发生在遮罩层上时关闭，避免输入框拖拽选中文本时误关弹窗
        dialog.addEventListener('mousedown', (e) => {
            dialog._mouseDownOnBackdrop = (e.target === dialog);
        });

        dialog.addEventListener('click', (e) => {
            if (e.target === dialog && dialog._mouseDownOnBackdrop && dialog._dismissOnBackdrop !== false) {
                const resolveCallback = dialog._resolve;
                hidePromptDialog();
                if (resolveCallback) {
                    resolveCallback(null);
                }
            }
            dialog._mouseDownOnBackdrop = false;
        });
    }
    return dialog;
}

function hidePromptDialog() {
    const dialog = document.getElementById('prompt-dialog');
    if (dialog) {
        dialog.classList.remove('active');
        dialog._resolve = null;
        dialog._reject = null;
        dialog._mouseDownOnBackdrop = false;
    }
}

/**
 * 显示输入对话框
 * @param {string} message - 提示语
 * @param {string} title - 标题
 * @param {string} defaultValue - 默认值
 * @param {string} inputType - 输入框类型
 * @param {object} options - 可选配置 { dismissOnBackdrop: true }
 * @returns {Promise<string|null>} 用户输入字符串；取消返回null
 */
export function showPrompt(message, title = '输入', defaultValue = '', inputType = 'text', options = {}) {
    return new Promise((resolve, reject) => {
        const dialog = ensurePromptDialog();
        dialog.querySelector('#prompt-message').textContent = message;
        dialog.querySelector('h3').textContent = title;
        const input = dialog.querySelector('#prompt-input');
        if (input) {
            input.type = inputType;
            input.value = defaultValue || '';
            // 打开时聚焦，但不全选
            setTimeout(() => {
                input.focus();
            }, 0);
        }
        dialog._resolve = resolve;
        dialog._reject = reject;
        dialog._dismissOnBackdrop = options.dismissOnBackdrop !== false;
        dialog.classList.add('active');
    });
}

/**
 * 隐藏确认对话框
 */
function hideConfirmDialog() {
    const dialog = document.getElementById('confirm-dialog');
    if (dialog) {
        dialog.classList.remove('active');
        // 重置进度显示
        const progressEl = dialog.querySelector('#confirm-progress');
        if (progressEl) {
            progressEl.style.display = 'none';
        }
        const footer = dialog.querySelector('.dialog-footer');
        if (footer) {
            footer.style.display = 'flex';
        }
        dialog._resolve = null;
        dialog._reject = null;
        dialog._showProgress = false; // 重置进度标志
    }
}

/**
 * 更新确认对话框的进度状态
 * @param {string} message - 进度消息
 * @param {boolean} showProgress - 是否显示进度
 */
export function updateConfirmProgress(message, showProgress = true) {
    const dialog = document.getElementById('confirm-dialog');
    if (dialog) {
        const progressEl = dialog.querySelector('#confirm-progress');
        const progressText = dialog.querySelector('#confirm-progress-text');
        const footer = dialog.querySelector('.dialog-footer');

        if (progressEl && progressText) {
            if (showProgress) {
                progressEl.style.display = 'block';
                progressText.textContent = message;
                // 隐藏按钮
                if (footer) {
                    footer.style.display = 'none';
                }
            } else {
                progressEl.style.display = 'none';
                // 显示按钮
                if (footer) {
                    footer.style.display = 'flex';
                }
                // 清空进度文本
                progressText.textContent = '';
            }
        }
    }
}

/**
 * 显示警告提示（快捷方法）
 */
export function showWarning(message, duration = 3000) {
    return showToast(message, 'warning', duration);
}

/**
 * 显示成功提示（快捷方法）
 */
export function showSuccess(message, duration = 3000) {
    return showToast(message, 'success', duration);
}

/**
 * 显示错误提示（快捷方法）
 */
export function showError(message, duration = 4000) {
    return showToast(message, 'error', duration);
}

/**
 * 显示信息提示（快捷方法）
 */
export function showInfo(message, duration = 3000) {
    return showToast(message, 'info', duration);
}

/**
 * HTML转义，防止XSS
 * @param {string} text - 需要转义的文本
 * @returns {string} 转义后的HTML安全字符串
 */
export function escapeHtml(text) {
    if (!text) return '';
    const map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;'
    };
    return String(text).replace(/[&<>"']/g, (char) => map[char]);
}

// 兼容旧的showNotification函数
export function showNotification(message, type = 'info') {
    showToast(message, type);
}

// ========== 目录选择器对话框 ==========

// 创建目录选择器对话框
function ensureDirectoryPickerDialog() {
    let dialog = document.getElementById('directory-picker-dialog');
    if (!dialog) {
        dialog = document.createElement('div');
        dialog.id = 'directory-picker-dialog';
        dialog.className = 'dialog';
        dialog.innerHTML = `
            <div class="dialog-content directory-picker-content">
                <h3 id="directory-picker-title">选择保存位置</h3>
                <div class="dialog-body directory-picker-body">
                    <div class="directory-picker-nav">
                        <button id="dir-back-btn" class="btn small" title="返回上级">
                            <span style="font-size: 16px;">⬆</span>
                        </button>
                        <div id="dir-current-path" class="dir-current-path">/</div>
                    </div>
                    <div id="directory-list" class="directory-list">
                        <div class="directory-loading">加载中...</div>
                    </div>
                    <div class="directory-picker-new">
                        <input type="text" id="new-dir-input" class="dialog-input" placeholder="新建文件夹名称" />
                        <button id="create-dir-btn" class="btn small primary">新建</button>
                    </div>
                </div>
                <div id="dir-progress" class="directory-progress" style="display: none;">
                    <div class="directory-progress-content">
                        <div class="spinner" style="width: 24px; height: 24px; border: 3px solid rgba(255,255,255,0.2); border-top-color: #667eea; border-radius: 50%; animation: spin 0.8s linear infinite;"></div>
                        <span id="dir-progress-text">正在下载...</span>
                    </div>
                </div>
                <div id="dir-footer" class="dialog-footer">
                    <button id="dir-cancel-btn" class="btn">取消</button>
                    <button id="dir-select-btn" class="btn primary">选择此目录</button>
                </div>
            </div>
        `;

        // 添加样式
        if (!document.getElementById('directory-picker-style')) {
            const style = document.createElement('style');
            style.id = 'directory-picker-style';
            style.textContent = `
                .directory-picker-content {
                    width: 90%;
                    max-width: 500px;
                    max-height: 80vh;
                }
                .directory-picker-body {
                    display: flex;
                    flex-direction: column;
                    gap: 12px;
                }
                .directory-picker-nav {
                    display: flex;
                    align-items: center;
                    gap: 8px;
                    padding: 8px;
                    background: rgba(255,255,255,0.05);
                    border-radius: 6px;
                }
                .dir-current-path {
                    flex: 1;
                    font-size: 13px;
                    color: var(--color-text-main);
                    overflow: hidden;
                    text-overflow: ellipsis;
                    white-space: nowrap;
                    font-family: monospace;
                }
                .directory-list {
                    min-height: 200px;
                    max-height: 300px;
                    overflow-y: auto;
                    border: 1px solid rgba(255,255,255,0.1);
                    border-radius: 6px;
                    background: rgba(0,0,0,0.2);
                }
                .directory-item {
                    display: flex;
                    align-items: center;
                    gap: 10px;
                    padding: 10px 12px;
                    cursor: pointer;
                    border-bottom: 1px solid rgba(255,255,255,0.05);
                    transition: background 0.15s;
                }
                .directory-item:last-child {
                    border-bottom: none;
                }
                .directory-item:hover {
                    background: rgba(255,255,255,0.1);
                }
                .directory-item-icon {
                    font-size: 20px;
                }
                .directory-item-name {
                    flex: 1;
                    font-size: 14px;
                    color: var(--color-text-main);
                    overflow: hidden;
                    text-overflow: ellipsis;
                    white-space: nowrap;
                }
                .directory-loading, .directory-empty {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    height: 100px;
                    color: var(--color-text-muted);
                    font-size: 14px;
                }
                .directory-picker-new {
                    display: flex;
                    gap: 8px;
                }
                .directory-picker-new input {
                    flex: 1;
                }
                .directory-progress {
                    padding: 20px;
                    text-align: center;
                }
                .directory-progress-content {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    gap: 12px;
                }
                #dir-progress-text {
                    color: var(--color-text-main);
                    font-size: 14px;
                }
            `;
            document.head.appendChild(style);
        }

        document.body.appendChild(dialog);

        // 绑定事件
        dialog.querySelector('#dir-cancel-btn').addEventListener('click', () => {
            hideDirectoryPickerDialog();
            if (dialog._reject) {
                dialog._reject(null);
            }
        });

        dialog.querySelector('#dir-select-btn').addEventListener('click', () => {
            const currentPath = dialog._currentPath || '/';
            // 不立即关闭，调用回调让调用者决定
            if (dialog._onSelect) {
                dialog._onSelect(currentPath);
            } else if (dialog._resolve) {
                hideDirectoryPickerDialog();
                dialog._resolve(currentPath);
            }
        });

        // 返回上级按钮
        dialog.querySelector('#dir-back-btn').addEventListener('click', () => {
            if (dialog._parentPath) {
                loadDirectoryList(dialog._parentPath);
            }
        });

        // 新建文件夹按钮
        dialog.querySelector('#create-dir-btn').addEventListener('click', async () => {
            const input = dialog.querySelector('#new-dir-input');
            const name = input.value.trim();
            if (!name) {
                showWarning('请输入文件夹名称');
                return;
            }

            // 检查名称是否包含非法字符
            if (name.includes('/') || name.includes('\\') || name.includes('..')) {
                showWarning('文件夹名称不能包含 / \\ 或 ..');
                return;
            }

            const newPath = (dialog._currentPath === '/' ? '' : dialog._currentPath) + '/' + name;

            try {
                const response = await fetch(window.apiBaseUrl + '/filesystem/mkdir', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ path: newPath })
                });

                const envelope = parseApiResponse(await response.json());
                if (response.ok && envelope.ok) {
                    input.value = '';
                    showSuccess('文件夹创建成功');
                    // 刷新目录列表并进入新目录
                    loadDirectoryList(newPath);
                } else {
                    showError(envelope.error?.message || '创建失败');
                }
            } catch (error) {
                showError('创建文件夹失败: ' + error.message);
            }
        });

        // 回车键创建文件夹
        dialog.querySelector('#new-dir-input').addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                dialog.querySelector('#create-dir-btn').click();
            }
        });

        // 点击遮罩层关闭
        dialog.addEventListener('click', (e) => {
            if (e.target === dialog) {
                hideDirectoryPickerDialog();
                if (dialog._reject) {
                    dialog._reject(null);
                }
            }
        });
    }
    return dialog;
}

// 加载目录列表
async function loadDirectoryList(path) {
    const dialog = document.getElementById('directory-picker-dialog');
    if (!dialog) return;

    const listContainer = dialog.querySelector('#directory-list');
    const currentPathEl = dialog.querySelector('#dir-current-path');
    const backBtn = dialog.querySelector('#dir-back-btn');

    listContainer.innerHTML = '<div class="directory-loading">加载中...</div>';
    currentPathEl.textContent = path || '/';

    try {
        const response = await fetch(window.apiBaseUrl + '/filesystem/list?path=' + encodeURIComponent(path || '/'));
        const envelope = parseApiResponse(await response.json());

        if (!response.ok || !envelope.ok) {
            listContainer.innerHTML = '<div class="directory-empty">无法加载目录</div>';
            return;
        }

        const result = envelope.data;
        dialog._currentPath = result.path;
        dialog._parentPath = result.parentPath;
        currentPathEl.textContent = result.path;

        // 更新返回按钮状态
        backBtn.disabled = !result.parentPath;
        backBtn.style.opacity = result.parentPath ? '1' : '0.5';

        const directories = result.directories || [];

        if (directories.length === 0) {
            listContainer.innerHTML = '<div class="directory-empty">此目录下没有子文件夹</div>';
            return;
        }

        listContainer.innerHTML = '';
        directories.forEach(dir => {
            const item = document.createElement('div');
            item.className = 'directory-item';
            item.innerHTML = `
                <span class="directory-item-icon"><svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg></span>
                <span class="directory-item-name">${escapeHtml(dir.name)}</span>
                <span class="directory-item-arrow"><svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="9 18 15 12 9 6"/></svg></span>
            `;
            item.addEventListener('click', () => {
                loadDirectoryList(dir.path);
            });
            listContainer.appendChild(item);
        });

    } catch (error) {
        listContainer.innerHTML = '<div class="directory-empty">加载失败: ' + escapeHtml(error.message) + '</div>';
    }
}

/**
 * 显示目录选择器对话框
 * @param {string} title - 对话框标题
 * @param {string} initialPath - 初始目录路径
 * @returns {Promise<string|null>} - 用户选择的目录路径，取消返回null
 */
function showDirectoryPicker(title = '选择保存位置', initialPath = '/') {
    return new Promise((resolve, reject) => {
        const dialog = ensureDirectoryPickerDialog();
        dialog.querySelector('#directory-picker-title').textContent = title;

        dialog._resolve = resolve;
        dialog._reject = reject;
        dialog._currentPath = initialPath;
        dialog._parentPath = null;

        dialog.classList.add('active');

        // 加载目录列表
        loadDirectoryList(initialPath);
    });
}

/**
 * 隐藏目录选择器对话框
 */
function hideDirectoryPickerDialog() {
    const dialog = document.getElementById('directory-picker-dialog');
    if (dialog) {
        dialog.classList.remove('active');
        dialog._resolve = null;
        dialog._reject = null;
        dialog._onSelect = null;
        dialog._currentPath = '/';
        dialog._parentPath = null;
        // 重置显示状态
        const body = dialog.querySelector('.directory-picker-body');
        const footer = dialog.querySelector('#dir-footer');
        const progress = dialog.querySelector('#dir-progress');
        if (body) body.style.display = 'flex';
        if (footer) footer.style.display = 'flex';
        if (progress) progress.style.display = 'none';
    }
}

/**
 * 显示目录选择器的下载进度
 * @param {string} message - 进度消息
 */
function showDirectoryPickerProgress(message) {
    const dialog = document.getElementById('directory-picker-dialog');
    if (dialog) {
        const body = dialog.querySelector('.directory-picker-body');
        const footer = dialog.querySelector('#dir-footer');
        const progress = dialog.querySelector('#dir-progress');
        const progressText = dialog.querySelector('#dir-progress-text');

        if (body) body.style.display = 'none';
        if (footer) footer.style.display = 'none';
        if (progress) progress.style.display = 'block';
        if (progressText) progressText.textContent = message;
    }
}

/**
 * 显示目录选择器对话框（带回调版本，用于下载等需要显示进度的场景）
 * @param {string} title - 对话框标题
 * @param {string} initialPath - 初始目录路径
 * @param {Function} onSelect - 选择目录后的回调函数，接收路径参数
 */
function showDirectoryPickerWithCallback(title, initialPath, onSelect) {
    const dialog = ensureDirectoryPickerDialog();
    dialog.querySelector('#directory-picker-title').textContent = title;

    dialog._resolve = null;
    dialog._reject = null;
    dialog._onSelect = onSelect;
    dialog._currentPath = initialPath;
    dialog._parentPath = null;

    // 重置显示状态
    const body = dialog.querySelector('.directory-picker-body');
    const footer = dialog.querySelector('#dir-footer');
    const progress = dialog.querySelector('#dir-progress');
    if (body) body.style.display = 'flex';
    if (footer) footer.style.display = 'flex';
    if (progress) progress.style.display = 'none';

    dialog.classList.add('active');

    // 加载目录列表
    loadDirectoryList(initialPath);
}

