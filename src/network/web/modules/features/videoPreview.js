// 视频预览窗口功能
import { addToCommandLog } from '../core/commandLog.js';
import { showError } from '../components/toast.js';
import { getCurrentPlaylistId } from './playlist.js';

// 预览窗口相关变量
let previewWindow = null;
let previewSwitchListener = null;

/**
 * 打开视频预览窗口（MJPEG流）
 * @param {string} filePath 视频文件路径
 * @param {string} fileName 视频文件名
 * @param {number} [layerId] 图层 ID，用于预览流显示对应图层（与播放列表项图层一致）
 */
export async function openVideoPreview(filePath, fileName, layerId) {
    // 检查预览开关状态
    const previewSwitch = document.getElementById('video-preview-switch');
    const isPreviewEnabled = previewSwitch && previewSwitch.checked;

    if (!isPreviewEnabled) {
        showError('预览功能已关闭，请先打开预览开关');
        return;
    }

    // 如果已有窗口，先关闭
    if (previewWindow) {
        closeVideoPreview();
    }

    // 获取当前播放列表ID
    const playlistId = getCurrentPlaylistId();
    if (!playlistId) {
        showError('请先选择播放列表');
        return;
    }

    // 创建预览窗口
    previewWindow = document.createElement('div');
    previewWindow.className = 'video-preview-window';
    previewWindow.style.cssText = `
        position: fixed;
        bottom: 20px;
        right: 20px;
        width: 320px;
        height: auto;
        aspect-ratio: 16 / 9;
        margin-top: 30px; /* 标题栏预留 */
        background: #000;
        border: 1px solid #444;
        border-radius: 8px;
        box-shadow: 0 10px 30px rgba(0,0,0,0.6);
        z-index: 10000;
        overflow: hidden;
        display: flex;
        flex-direction: column;
    `;

    // 创建标题栏
    const titleBar = document.createElement('div');
    titleBar.style.cssText = `
        background: #222;
        color: #ddd;
        padding: 5px 10px;
        font-size: 12px;
        display: flex;
        justify-content: space-between;
        align-items: center;
        cursor: move;
        height: 30px;
        flex-shrink: 0;
    `;
    titleBar.textContent = `预览: ${fileName}`;

    // 关闭按钮
    const closeBtn = document.createElement('button');
    closeBtn.textContent = '×';
    closeBtn.style.cssText = `
        background: transparent;
        border: none;
        color: #fff;
        font-size: 24px;
        cursor: pointer;
        padding: 0;
        width: 30px;
        height: 30px;
        line-height: 1;
    `;
    closeBtn.onclick = closeVideoPreview;
    titleBar.appendChild(closeBtn);

    // 创建MJPEG图像元素
    const img = document.createElement('img');
    img.style.cssText = `
        width: 100%;
        flex: 1;
        object-fit: fill;
        background: #000;
        display: block;
    `;
    // 在URL中添加预览开关状态参数（后端可以检查）；传入 layerId 使预览流显示当前播放的图层
    const previewEnabled = isPreviewEnabled ? '1' : '0';
    let previewUrl = `${window.apiBaseUrl}/materials/preview_stream?playlistId=${encodeURIComponent(playlistId)}&enabled=${previewEnabled}&t=${Date.now()}`;
    if (layerId != null && layerId >= 1 && layerId <= 4) {
        previewUrl += `&layerId=${layerId}`;
    }
    img.src = previewUrl;

    // 监听图片加载错误（可能是预览开关关闭导致）
    img.onerror = function () {
        const previewSwitch = document.getElementById('video-preview-switch');
        if (previewSwitch && !previewSwitch.checked) {
            closeVideoPreview();
            showError('预览已关闭');
        }
    };

    previewWindow.appendChild(titleBar);
    previewWindow.appendChild(img);
    document.body.appendChild(previewWindow);

    // 使窗口可拖动
    makeWindowDraggable(previewWindow, titleBar);

    // 监听预览开关变化，如果关闭则关闭预览窗口
    if (!previewSwitchListener) {
        previewSwitchListener = function () {
            const previewSwitch = document.getElementById('video-preview-switch');
            if (previewSwitch && !previewSwitch.checked && previewWindow) {
                closeVideoPreview();
            }
        };
        const previewSwitch = document.getElementById('video-preview-switch');
        if (previewSwitch) {
            previewSwitch.addEventListener('change', previewSwitchListener);
        }
    }

    addToCommandLog('预览窗口', 'success', `已打开预览窗口: ${fileName}`);
}

/**
 * 关闭视频预览窗口
 */
function closeVideoPreview() {
    if (previewWindow) {
        // 在移除窗口前，先查找并断开图片连接
        const img = previewWindow.querySelector('img');
        if (img) {
            img.src = '';
            img.onerror = null;
        }

        previewWindow.remove();
        previewWindow = null;
    }

    // 移除预览开关监听器
    if (previewSwitchListener) {
        const previewSwitch = document.getElementById('video-preview-switch');
        if (previewSwitch) {
            previewSwitch.removeEventListener('change', previewSwitchListener);
        }
        previewSwitchListener = null;
    }
}

/**
 * 使窗口可拖动
 */
function makeWindowDraggable(window, handle) {
    let isDragging = false;
    let currentX;
    let currentY;
    let initialX;
    let initialY;

    handle.addEventListener('mousedown', function (e) {
        initialX = e.clientX - window.offsetLeft;
        initialY = e.clientY - window.offsetTop;
        isDragging = true;
    });

    document.addEventListener('mousemove', function (e) {
        if (isDragging) {
            e.preventDefault();
            currentX = e.clientX - initialX;
            currentY = e.clientY - initialY;
            window.style.left = currentX + 'px';
            window.style.top = currentY + 'px';
            window.style.right = 'auto';
            window.style.bottom = 'auto';
        }
    });

    document.addEventListener('mouseup', function () {
        isDragging = false;
    });
}
