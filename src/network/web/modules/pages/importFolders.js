// 导入文件夹功能模块
import { apiGet, apiPost, getLastApiError } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { showSuccess, showError, showWarning, showInfo, showConfirm } from '../components/toast.js';

let selectedFiles = [];
let isImporting = false;
let dataRootPath = '当前数据根目录';

/**
 * 初始化导入文件夹功能
 */
export function initializeImportFolders() {
    const importBtn = document.getElementById('import-folder-btn');
    const fileInput = document.getElementById('import-folder-input');
    const executeBtn = document.getElementById('import-execute-btn');
    const cancelBtn = document.getElementById('import-cancel-btn');

    if (importBtn && fileInput) {
        importBtn.addEventListener('click', async () => {
            const confirmed = await showConfirm(
                `请选择本地文件夹，系统会读取该文件夹内的文件用于导入到设备 ${dataRootPath} 目录。浏览器可能会继续显示一次安全确认，这是浏览器限制。`,
                '导入文件夹'
            );
            if (!confirmed) return;
            fileInput.click();
        });
        fileInput.addEventListener('change', handleFolderSelect);
    }

    if (executeBtn) {
        executeBtn.addEventListener('click', startImport);
    }

    if (cancelBtn) {
        cancelBtn.addEventListener('click', cancelImport);
    }

    syncImportDataRoot();
}

async function syncImportDataRoot() {
    try {
        const result = await apiGet('/system/data-root');
        const root = result?.root;
        if (root) {
            dataRootPath = root.replace(/\/+$/, '');
        }
    } catch (error) {
        dataRootPath = '当前数据根目录';
    }

    const desc = document.getElementById('import-folder-root-desc');
    if (desc) {
        desc.textContent = `选择本地文件夹，将其内容导入到设备的 ${dataRootPath}/ 目录下。适用于批量导入视频、图片、字体等资源。`;
    }

    const help = document.getElementById('import-target-root-help');
    if (help) {
        help.textContent = `输入目标子目录名称（相对于 ${dataRootPath}/），留空则导入到根目录`;
    }
}

function isSafeRelativePath(path) {
    return !!path &&
        !path.includes('..') &&
        !path.startsWith('/') &&
        !path.startsWith('\\');
}

/**
 * 处理文件夹选择
 */
function handleFolderSelect(event) {
    const files = Array.from(event.target.files);
    if (files.length === 0) return;

    selectedFiles = files;
    
    // 显示预览
    const preview = document.getElementById('import-folder-preview');
    const info = document.getElementById('import-folder-info');
    const fileCount = document.getElementById('import-file-count');
    const fileContainer = document.getElementById('import-file-list-container');
    const executeBtn = document.getElementById('import-execute-btn');

    if (preview && info) {
        info.style.display = 'none';
        preview.style.display = 'block';
    }

    if (fileCount) {
        fileCount.textContent = files.length;
    }

    // 显示文件列表（最多显示100个）
    if (fileContainer) {
        fileContainer.innerHTML = '';
        const displayFiles = files.slice(0, 100);
        displayFiles.forEach((file, index) => {
            const fileItem = document.createElement('div');
            fileItem.className = 'file-item';
            fileItem.innerHTML = `
                <span class="file-name" title="${file.webkitRelativePath || file.name}">${file.webkitRelativePath || file.name}</span>
                <span class="file-size">${formatFileSize(file.size)}</span>
            `;
            fileContainer.appendChild(fileItem);
        });

        if (files.length > 100) {
            const moreItem = document.createElement('div');
            moreItem.className = 'file-item more-files';
            moreItem.textContent = `... 还有 ${files.length - 100} 个文件`;
            fileContainer.appendChild(moreItem);
        }
    }

    // 启用导入按钮
    if (executeBtn) {
        executeBtn.disabled = false;
    }

    addToCommandLog('选择文件夹', 'info', `已选择 ${files.length} 个文件`);
    showInfo(`已选择 ${files.length} 个文件`);
}

/**
 * 开始导入
 */
async function startImport() {
    if (selectedFiles.length === 0 || isImporting) return;

    isImporting = true;
    const executeBtn = document.getElementById('import-execute-btn');
    const cancelBtn = document.getElementById('import-cancel-btn');
    const progressSection = document.getElementById('import-progress-section');
    const progressBar = document.getElementById('import-progress-bar');
    const progressText = document.getElementById('import-progress-text');
    const progressPercent = document.getElementById('import-progress-percent');
    const progressLog = document.getElementById('import-progress-log');
    const targetPath = document.getElementById('import-target-path')?.value?.trim() || '';

    if (targetPath && !isSafeRelativePath(targetPath)) {
        showError('目标子目录必须是 ROOT_PATH 下的相对路径，不能包含 .. 或绝对路径');
        isImporting = false;
        return;
    }

    if (executeBtn) executeBtn.disabled = true;
    if (cancelBtn) cancelBtn.disabled = true;
    if (progressSection) progressSection.style.display = 'block';
    if (progressLog) progressLog.innerHTML = '';

    const totalFiles = selectedFiles.length;
    let uploadedCount = 0;
    let failedCount = 0;

    addProgressLog(progressLog, `开始导入 ${totalFiles} 个文件到 ${dataRootPath}/${targetPath || '(根目录)'}`);

    // 按文件逐个上传
    for (let i = 0; i < selectedFiles.length; i++) {
        const file = selectedFiles[i];
        const relativePath = file.webkitRelativePath || file.name;
        
        // 计算目标路径
        let targetFilePath;
        if (targetPath) {
            // 移除文件夹前缀，只保留文件名
            const fileName = relativePath.includes('/') 
                ? relativePath.substring(relativePath.lastIndexOf('/') + 1)
                : relativePath;
            targetFilePath = `${targetPath}/${fileName}`;
        } else {
            targetFilePath = relativePath;
        }

        if (!isSafeRelativePath(targetFilePath)) {
            failedCount++;
            addProgressLog(progressLog, `✗ 跳过: ${relativePath} - 目标路径非法`, 'error');
            continue;
        }

        const percent = Math.round(((i + 1) / totalFiles) * 100);
        if (progressBar) progressBar.style.width = `${percent}%`;
        if (progressPercent) progressPercent.textContent = `${percent}%`;
        if (progressText) progressText.textContent = `正在上传 ${i + 1}/${totalFiles}`;

        addProgressLog(progressLog, `上传: ${relativePath}`);

        try {
            // 使用 FormData 上传文件
            const formData = new FormData();
            formData.append('file', file);
            formData.append('path', targetFilePath);

            const result = await apiPost('/system/upload-file', formData, {
                headers: {},
                isFormData: true,
                timeoutMs: 60000
            });

            if (result) {
                uploadedCount++;
                addProgressLog(progressLog, `✓ 成功: ${relativePath}`, 'success');
            } else {
                const apiError = getLastApiError();
                failedCount++;
                addProgressLog(progressLog, `✗ 失败: ${relativePath} - ${apiError?.message || '服务器未返回成功结果'}`, 'error');
            }
        } catch (error) {
            failedCount++;
            addProgressLog(progressLog, `✗ 错误: ${relativePath} - ${error.message}`, 'error');
            console.error(`上传失败: ${relativePath}`, error);
        }

        // 每上传10个文件更新一次日志
        if ((i + 1) % 10 === 0) {
            progressLog.scrollTop = progressLog.scrollHeight;
        }
    }

    // 完成
    const successMsg = `导入完成！成功: ${uploadedCount}, 失败: ${failedCount}`;
    addProgressLog(progressLog, successMsg, uploadedCount > 0 ? 'success' : 'error');
    
    if (progressText) progressText.textContent = '导入完成';
    if (progressBar) progressBar.style.width = '100%';
    if (progressPercent) progressPercent.textContent = '100%';

    addToCommandLog('导入文件夹', uploadedCount > 0 ? 'success' : 'error', successMsg);

    if (uploadedCount > 0) {
        if (failedCount > 0) {
            showWarning(`导入完成：成功 ${uploadedCount} 个，失败 ${failedCount} 个`);
        } else {
            showSuccess(`成功导入 ${uploadedCount} 个文件`);
        }
    } else {
        showError('所有文件导入失败');
    }

    isImporting = false;
    if (executeBtn) executeBtn.disabled = false;
    if (cancelBtn) cancelBtn.disabled = false;
}

/**
 * 取消导入
 */
function cancelImport() {
    if (isImporting) {
        isImporting = false;
        addToCommandLog('导入文件夹', 'warning', '用户取消导入');
        showWarning('已取消导入');
    }
    resetImportUI();
}

/**
 * 重置导入UI
 */
function resetImportUI() {
    selectedFiles = [];
    
    const preview = document.getElementById('import-folder-preview');
    const info = document.getElementById('import-folder-info');
    const fileInput = document.getElementById('import-folder-input');
    const progressSection = document.getElementById('import-progress-section');
    const executeBtn = document.getElementById('import-execute-btn');
    const cancelBtn = document.getElementById('import-cancel-btn');

    if (preview) preview.style.display = 'none';
    if (info) info.style.display = 'block';
    if (fileInput) fileInput.value = '';
    if (progressSection) progressSection.style.display = 'none';
    if (executeBtn) executeBtn.disabled = true;
    if (cancelBtn) cancelBtn.disabled = false;
}

/**
 * 添加进度日志
 */
function addProgressLog(container, message, type = 'info') {
    if (!container) return;
    
    const logEntry = document.createElement('div');
    logEntry.className = `log-entry log-${type}`;
    logEntry.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;
    container.appendChild(logEntry);
    container.scrollTop = container.scrollHeight;
}

/**
 * 格式化文件大小
 */
function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
}
