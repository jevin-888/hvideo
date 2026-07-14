// 节目列表模块
import { apiGet, apiPost, apiPut, apiDelete, apiAction } from '../core/api.js';
import { addToCommandLog } from '../core/commandLog.js';
import { handleApiOperation } from '../utils/apiHelpers.js';
import { getButtonGroupValue, updateButtonGroupState, setImageOnlyLoopOptionVisible } from '../components/buttonGroups.js';
import { showConfirm, showWarning, escapeHtml } from '../components/toast.js';
import { showPlaylistSettingsDialog, showCreatePlaylistDialog, hideDialog } from '../components/dialogs.js';
import { extractFileName, getFolderFromPath } from '../utils/pathHelpers.js';
import { clearContainer } from '../utils/domHelpers.js';
import { setPlaylistMode, playVideo } from '../pages/videoControl.js?v=2.98';
import { openVideoPreview } from './videoPreview.js';

// 全局变量 - 播放列表状态集中管理
let currentPlaylistId = null;
let currentPlaylistName = '';
let playlistsCache = []; // 播放列表数据缓存，用于下拉框
const PLAYLIST_DMX_STEP = 10;
const IMAGE_FIXED_LOOP_MODE = 4;

function normalizePlaylistDmxId(value) {
    const numericValue = Math.max(0, Math.min(255, parseInt(value, 10) || 0));
    if (numericValue === 0) return 0;
    return Math.min(255, (Math.floor((numericValue - 1) / PLAYLIST_DMX_STEP) + 1) * PLAYLIST_DMX_STEP);
}

function findNextAvailablePlaylistDmxId(playlists = playlistsCache) {
    const used = new Set(
        (Array.isArray(playlists) ? playlists : [])
            .map(playlist => normalizePlaylistDmxId(playlist?.dmxId || 0))
            .filter(dmxId => dmxId > 0)
    );
    for (let dmxId = PLAYLIST_DMX_STEP; dmxId <= 255; dmxId += PLAYLIST_DMX_STEP) {
        if (!used.has(dmxId)) return dmxId;
    }
    return 0;
}

async function refreshCreatePlaylistDmxId() {
    const dmxIdInput = document.getElementById('new-playlist-dmx-id');
    if (!dmxIdInput) return;

    try {
        const response = await apiGet('/playlists');
        if (Array.isArray(response)) {
            playlistsCache = response;
        }
    } catch (error) {
        // 使用当前缓存兜底，不阻断创建弹窗。
    }

    dmxIdInput.value = String(findNextAvailablePlaylistDmxId());
}

// 获取当前播放列表ID（供其他模块使用）
export function getCurrentPlaylistId() {
    return currentPlaylistId;
}

// 获取当前播放列表名称（供其他模块使用）
function getCurrentPlaylistName() {
    return currentPlaylistName;
}

// 获取播放列表绑定的图层（返回 null 而不是硬编码默认值，让调用方处理）
export function getPlaylistTargetLayer(playlistId) {
    const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
    return playlist?.targetLayerId || null;
}

// 初始化节目列表
export function initializePlaylist() {
    // 绑定新建列表按钮事件
    const createPlaylistBtn = document.getElementById('create-playlist-btn');
    if (createPlaylistBtn) {
        createPlaylistBtn.addEventListener('click', async () => {
            // 清空之前的输入
            const nameInput = document.getElementById('new-playlist-name');
            if (nameInput) nameInput.value = '';
            await refreshCreatePlaylistLayerOptions();
            await refreshCreatePlaylistDmxId();
            // 重置循环模式为默认值（循环全部）
            updateButtonGroupState('new-playlist-loop-mode', '0');
            const targetLayerSelect = document.getElementById('new-playlist-target-layer');
            syncPlaylistLoopModeVisibility('new-playlist-loop-mode', targetLayerSelect?.value || 1);
            showCreatePlaylistDialog();
        });
    }

    const targetLayerSelect = document.getElementById('new-playlist-target-layer');
    if (targetLayerSelect) {
        targetLayerSelect.addEventListener('change', () => {
            syncPlaylistLoopModeVisibility('new-playlist-loop-mode', targetLayerSelect.value);
        });
    }

    // 绑定创建对话框确认按钮
    const confirmCreateBtn = document.getElementById('confirm-create-playlist-btn');
    if (confirmCreateBtn) {
        confirmCreateBtn.addEventListener('click', async () => {
            const nameInput = document.getElementById('new-playlist-name');
            const targetLayerSelect = document.getElementById('new-playlist-target-layer');
            const dmxIdInput = document.getElementById('new-playlist-dmx-id');
            const name = nameInput ? nameInput.value : '';
            const targetLayerId = targetLayerSelect ? parseInt(targetLayerSelect.value) : 1;
            const loopMode = parseInt(getButtonGroupValue('new-playlist-loop-mode')) || 0;
            const dmxId = dmxIdInput ? normalizePlaylistDmxId(dmxIdInput.value) : 0;
            if (dmxIdInput) {
                dmxIdInput.value = String(dmxId);
            }

            if (!targetLayerId || Number.isNaN(targetLayerId)) {
                addToCommandLog('创建播放列表', 'error', '没有可用的授权播放图层');
                return;
            }

            if (name) {
                const createdPlaylist = await createPlaylist(name, targetLayerId, loopMode, dmxId);
                if (createdPlaylist && createdPlaylist.id) {
                    hideDialog('create-playlist-dialog');
                    await updatePlaylistList();
                    createdPlaylist.targetLayerId = targetLayerId;
                    selectPlaylist(createdPlaylist.id, createdPlaylist.name || name, createdPlaylist);
                }
            } else {
                addToCommandLog('创建播放列表', 'error', '请输入播放列表名称');
            }
        });
    }

    // 绑定创建对话框取消按钮
    const cancelCreateBtn = document.getElementById('cancel-create-playlist-btn');
    if (cancelCreateBtn) {
        cancelCreateBtn.addEventListener('click', () => {
            hideDialog('create-playlist-dialog');
        });
    }

    // 绑定添加视频下拉选择框事件
    const addItemSelect = document.getElementById('add-item-select');
    if (addItemSelect) {
        addItemSelect.addEventListener('change', async function () {
            const selectedPlaylistId = this.value;
            if (selectedPlaylistId) {
                // 从缓存中查找播放列表信息
                const selectedPlaylist = playlistsCache.find(p => String(p.id) === String(selectedPlaylistId));
                if (selectedPlaylist) {
                    // 选择该播放列表
                    selectPlaylist(selectedPlaylist.id, selectedPlaylist.name);
                    // 切换时自动播放
                    const targetLayer = selectedPlaylist.targetLayerId || 1;
                    await playPlaylist(selectedPlaylist.id, targetLayer);
                }
            }
        });
    }

    // 绑定清空播放列表按钮事件
    const clearPlaylistBtn = document.getElementById('delete-playlist-btn');
    if (clearPlaylistBtn) {
        clearPlaylistBtn.addEventListener('click', async () => {
            if (!currentPlaylistId) {
                addToCommandLog('清空播放列表', 'error', '请先选择一个播放列表');
                return;
            }

            const confirmed = await showConfirm(
                `确定要清空播放列表 "${currentPlaylistName}" 中的所有内容吗？`,
                '清空播放列表'
            );

            if (confirmed) {
                await clearPlaylist(currentPlaylistId);
                await updatePlaylistContent(currentPlaylistId);
            }
        });
    }

    // 绑定删除播放列表按钮事件
    const removePlaylistBtn = document.getElementById('remove-playlist-btn');
    if (removePlaylistBtn) {
        removePlaylistBtn.addEventListener('click', async () => {
            if (!currentPlaylistId) {
                addToCommandLog('删除播放列表', 'error', '请先选择一个播放列表');
                return;
            }

            const confirmed = await showConfirm(
                `确定要删除播放列表 "${currentPlaylistName}" 吗？此操作不可恢复！`,
                '删除播放列表'
            );

            if (confirmed) {
                const success = await deletePlaylist(currentPlaylistId);
                if (success) {
                    // 重置当前选择
                    currentPlaylistId = null;
                    currentPlaylistName = '';
                    // 隐藏数量徽章
                    updatePlaylistItemCount(0);
                    // 刷新播放列表
                    await updatePlaylistList();
                    // 使用工具函数清空显示
                    const container = document.getElementById('playlist-items');
                    if (container) {
                        clearContainer(container);
                    }
                }
            }
        });
    }

    // 绑定播放播放列表按钮事件
    const playPlaylistBtn = document.getElementById('play-playlist-btn');
    if (playPlaylistBtn) {
        playPlaylistBtn.addEventListener('click', async () => {
            if (!currentPlaylistId) {
                addToCommandLog('播放播放列表', 'error', '请先选择一个播放列表');
                return;
            }
            // 默认使用播放列表绑定的图层播放
            const playlist = playlistsCache.find(p => String(p.id) === String(currentPlaylistId));
            const targetLayer = playlist?.targetLayerId || 1;
            await playPlaylist(currentPlaylistId, targetLayer);
        });
    }

    // 绑定设为默认按钮事件
    const setDefaultBtn = document.getElementById('set-default-playlist-btn');
    if (setDefaultBtn) {
        setDefaultBtn.addEventListener('click', async () => {
            if (!currentPlaylistId) {
                addToCommandLog('设为默认', 'error', '请先选择一个播放列表');
                return;
            }
            await setDefaultPlaylist(currentPlaylistId);
        });
    }

    // 绑定设置按钮事件
    const settingsBtn = document.getElementById('playlist-settings-btn');
    if (settingsBtn) {
        settingsBtn.addEventListener('click', async () => {
            if (!currentPlaylistId) {
                addToCommandLog('播放列表设置', 'error', '请先选择一个播放列表');
                return;
            }
            await openPlaylistSettings(currentPlaylistId);
        });
    }

    // 绑定设置保存按钮
    const confirmSettingsBtn = document.getElementById('confirm-playlist-settings-btn');
    if (confirmSettingsBtn) {
        confirmSettingsBtn.addEventListener('click', async () => {
            await savePlaylistSettings(currentPlaylistId);
            hideDialog('playlist-settings-dialog');
        });
    }

    // 绑定设置取消按钮
    const cancelSettingsBtn = document.getElementById('cancel-playlist-settings-btn');
    if (cancelSettingsBtn) {
        cancelSettingsBtn.addEventListener('click', () => {
            hideDialog('playlist-settings-dialog');
        });
    }

    // 初始化播放列表
    updatePlaylistList();

    // 监听播放列表切换事件（从串口命令触发，通过自定义事件）
    window.addEventListener('playlistSwitch', (e) => {
        const playlistId = e.detail?.playlistId;
        if (playlistId) {
            // 从缓存中查找播放列表信息
            const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
            if (playlist) {
                selectPlaylist(playlist.id, playlist.name);
                addToCommandLog('播放列表切换', 'success', `已切换到播放列表: ${playlist.name}`);
            } else {
                // 如果缓存中没有，重新加载播放列表
                updatePlaylistList().then(() => {
                    const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
                    if (playlist) {
                        selectPlaylist(playlist.id, playlist.name);
                        addToCommandLog('播放列表切换', 'success', `已切换到播放列表: ${playlist.name}`);
                    } else {
                        addToCommandLog('播放列表切换', 'warning', `播放列表 ${playlistId} 不存在`);
                    }
                });
            }
        }
    });
}

async function refreshCreatePlaylistLayerOptions() {
    const targetLayerSelect = document.getElementById('new-playlist-target-layer');
    const confirmCreateBtn = document.getElementById('confirm-create-playlist-btn');
    if (!targetLayerSelect) return;

    targetLayerSelect.innerHTML = '';

    try {
        const [layers, config] = await Promise.all([
            apiGet('/layers/authorized'),
            apiGet('/config.json')
        ]);
        const vodMode = parseInt(config?.vodMode ?? (config?.enableVod ? 2 : 0), 10) || 0;
        const reservedLayerIds = new Set();
        if (vodMode > 0 || config?.enableVod === true) {
            reservedLayerIds.add(1);
        }
        const playlistLayerIds = new Set([1, 2, 3, 4, 60]);
        const playableLayers = Array.isArray(layers)
            ? layers.filter(layer => layer && playlistLayerIds.has(Number(layer.id)) && !reservedLayerIds.has(Number(layer.id)))
            : [];

        playableLayers.forEach((layer, index) => {
            const option = document.createElement('option');
            option.value = String(layer.id);
            option.textContent = layer.type === 'image' ? `图层 ${layer.id} (图片)` : `图层 ${layer.id} (视频)`;
            if (index === 0) option.selected = true;
            targetLayerSelect.appendChild(option);
        });

        if (confirmCreateBtn) {
            confirmCreateBtn.disabled = playableLayers.length === 0;
        }

        if (playableLayers.length === 0) {
            const option = document.createElement('option');
            option.value = '';
            option.textContent = '无可用授权播放图层';
            targetLayerSelect.appendChild(option);
            addToCommandLog('创建播放列表', 'warning', '没有可用的授权播放图层');
        }
    } catch (error) {
        if (confirmCreateBtn) confirmCreateBtn.disabled = true;
        const option = document.createElement('option');
        option.value = '';
        option.textContent = '读取授权图层失败';
        targetLayerSelect.appendChild(option);
        addToCommandLog('创建播放列表', 'error', error.message || '读取授权图层失败');
    }
}

// 更新播放列表列表
export async function updatePlaylistList() {
    try {
        const response = await apiGet('/playlists');

        // apiGet 已解包统一 envelope；该端点的 data 固定为数组。
        const playlists = Array.isArray(response) ? response : null;

        // 更新缓存
        playlistsCache = playlists || [];

        // 更新下拉选择框
        updatePlaylistSelect(playlists);
    } catch (error) {
        // 更新缓存为空
        playlistsCache = [];
        // 即使出错也尝试更新下拉框（可能为空）
        updatePlaylistSelect(null);
    }
}

// 更新播放列表下拉选择框
function updatePlaylistSelect(playlists) {
    const select = document.getElementById('add-item-select');
    if (!select) {
        return;
    }

    // 保存当前选中的值
    const currentValue = select.value;

    clearContainer(select);

    if (playlists && Array.isArray(playlists) && playlists.length > 0) {
        // 添加提示选项（但不选中，因为我们会默认选中第一个播放列表）
        const placeholderOption = document.createElement('option');
        placeholderOption.value = '';
        placeholderOption.textContent = '请选择播放列表';
        placeholderOption.disabled = true;
        select.appendChild(placeholderOption);

        // 添加播放列表选项
        playlists.forEach(playlist => {
            const option = document.createElement('option');
            option.value = String(playlist.id);
            // 如果是默认播放列表，添加星号标记
            option.textContent = playlist.isDefault ? `★ ${playlist.name}` : playlist.name;
            select.appendChild(option);
        });

        // 如果有之前选中的值且仍然存在，恢复它；否则默认选中第一个播放列表
        let selectedPlaylistId = null;
        if (currentValue) {
            const optionExists = Array.from(select.options).some(opt => opt.value === currentValue);
            if (optionExists) {
                select.value = currentValue;
                selectedPlaylistId = currentValue;
            } else {
                // 如果之前的值不存在了，默认选中第一个播放列表
                if (playlists.length > 0) {
                    select.value = String(playlists[0].id);
                    selectedPlaylistId = String(playlists[0].id);
                }
            }
        } else {
            // 没有之前的值，默认选中第一个播放列表
            if (playlists.length > 0) {
                select.value = String(playlists[0].id);
                selectedPlaylistId = String(playlists[0].id);
            }
        }

        // 如果默认选中了第一个播放列表且当前没有选中的播放列表，自动选择它
        if (selectedPlaylistId && !currentPlaylistId) {
            const selectedPlaylist = playlists.find(p => String(p.id) === selectedPlaylistId);
            if (selectedPlaylist) {
                selectPlaylist(selectedPlaylist.id, selectedPlaylist.name);
            }
        }
    } else {
        // 没有播放列表时，只显示提示选项
        const placeholderOption = document.createElement('option');
        placeholderOption.value = '';
        placeholderOption.textContent = '请选择播放列表';
        placeholderOption.disabled = true;
        placeholderOption.selected = true;
        select.appendChild(placeholderOption);
    }
}

// 选择播放列表
export function selectPlaylist(playlistId, playlistName) {
    currentPlaylistId = playlistId;
    currentPlaylistName = playlistName;

    if (!playlistsCache.some(p => String(p.id) === String(playlistId))) {
        const createdPlaylist = arguments.length > 2 ? arguments[2] : null;
        playlistsCache.push({
            id: playlistId,
            name: playlistName,
            targetLayerId: createdPlaylist?.targetLayerId || null
        });
    }

    // 同步更新下拉框选中状态
    const addItemSelect = document.getElementById('add-item-select');
    if (addItemSelect) {
        const playlistValue = String(playlistId);
        const optionExists = Array.from(addItemSelect.options).some(opt => opt.value === playlistValue);
        if (!optionExists) {
            const option = document.createElement('option');
            option.value = playlistValue;
            option.textContent = playlistName;
            addItemSelect.appendChild(option);
        }
        addItemSelect.value = String(playlistId);
    }

    // 更新播放列表内容
    updatePlaylistContent(playlistId);
    addToCommandLog('选择播放列表', 'info', `选择播放列表: ${playlistName}`);

    // 发送自定义事件，通知其他模块（如视频控制）同步图层选择
    const targetLayerId = getPlaylistTargetLayer(playlistId);
    window.dispatchEvent(new CustomEvent('playlistSelected', {
        detail: { playlistId, playlistName, targetLayerId }
    }));
}

// 取消选择播放列表
export function deselectPlaylist() {
    currentPlaylistId = null;
    currentPlaylistName = '';

    // 同步更新下拉框状态
    const addItemSelect = document.getElementById('add-item-select');
    if (addItemSelect) {
        addItemSelect.value = '';
    }
}

// 更新播放列表素材数量统计
function updatePlaylistItemCount(count) {
    const countBadge = document.getElementById('playlist-item-count');
    if (countBadge) {
        if (count > 0 && currentPlaylistId) {
            countBadge.textContent = count;
            countBadge.style.display = 'flex';
        } else {
            countBadge.style.display = 'none';
        }
    }
}

// 路径处理函数已移至 pathHelpers.js，直接使用导入的函数

/** 图片播放列表使用的图层 ID，与 videoControl 一致 */
const IMAGE_PLAYLIST_LAYER_ID = 60;
const FUSION_BACKGROUND_FOLDER = 'gb_fusion';

const DEFAULT_SOFT_BLOCK_MESSAGE = '当前设备不能同时播放两个4K视频';

function isFusionBackgroundImagePath(filePath) {
    const lower = String(filePath || '').replace(/\\/g, '/').toLowerCase();
    return lower.includes(`/image/${FUSION_BACKGROUND_FOLDER}/`) ||
        lower.startsWith(`image/${FUSION_BACKGROUND_FOLDER}/`) ||
        lower.startsWith(`${FUSION_BACKGROUND_FOLDER}/`);
}

function isSoftBlockedPlaybackResult(result) {
    return result && typeof result === 'object' &&
        (result.state === 'blocked' || result.softBlocked === true || result.notice === true);
}

function getPlaybackResultMessage(result, fallback = DEFAULT_SOFT_BLOCK_MESSAGE) {
    if (result && typeof result === 'object' &&
        typeof result.message === 'string' &&
        result.message.trim()) {
        return result.message;
    }
    return fallback;
}

/**
 * 预览播放列表中的单条素材（使用当前播放列表的图层；图片在图层 60 加载）
 * @param {string} itemPath - 素材路径
 * @param {string} 播放列表Id - 当前播放列表 ID，用于确定目标图层
 * @param {number} itemLayerId - 该项的图层（仅用于判断是否为图片 60）
 */
async function previewPlaylistItem(itemPath, playlistId, itemLayerId) {
    try {
        if (itemLayerId === IMAGE_PLAYLIST_LAYER_ID) {
            if (isFusionBackgroundImagePath(itemPath)) {
                const message = 'gb_fusion 是融合背景目录，不发送到播放图层';
                showWarning(message);
                addToCommandLog('预览图片', 'warning', message);
                return false;
            }

            // 提取相对于 Image/ 目录的相对路径，支持子目录（如 gb_fusion/19.jpg）
            let imageFile = extractFileName(itemPath);
            const imageDir = '/Image/';
            const imageDirIdx = itemPath.indexOf(imageDir);
            if (imageDirIdx !== -1) {
                imageFile = itemPath.substring(imageDirIdx + imageDir.length);
            }
            const result = await apiAction('rendering', 'loadImage', { layerId: IMAGE_PLAYLIST_LAYER_ID, image_file: imageFile });
            if (result === null || result === undefined) {
                throw new Error('图片预览失败: 请求失败');
            }
            addToCommandLog('预览图片', 'success', extractFileName(itemPath));
            return true;
        } else {
            const payload = {
                path: itemPath,
                loop: 0,
                playback_rate: 1.0
            };
            if (playlistId) {
                payload.playlistId = playlistId;
            }
            const playResult = await apiPost('/video/play', payload);
            if (playResult === null || playResult === undefined) {
                throw new Error('预览失败: 请求失败');
            }
            if (isSoftBlockedPlaybackResult(playResult)) {
                const blockedMsg = getPlaybackResultMessage(playResult);
                showWarning(`预览受限：${blockedMsg}`);
                addToCommandLog('预览', 'warning', blockedMsg);
                return false;
            }
            if (playResult == null) {
                throw new Error('预览失败');
            }
            const targetLayer = getPlaylistTargetLayer(playlistId) || 1;
            addToCommandLog('预览', 'success', `图层${targetLayer}: ${extractFileName(itemPath)}`);
            return true;
        }
    } catch (e) {
        addToCommandLog('预览', 'error', e.message || '预览失败');
        return false;
    }
}

// 更新播放列表内容
export async function updatePlaylistContent(playlistId) {
    const container = document.getElementById('playlist-items');
    clearContainer(container);

    // 如果没有选中播放列表，隐藏数量徽章
    if (!playlistId) {
        updatePlaylistItemCount(0);
        return;
    }

    // 获取所有图层的播放列表项（视频图层 1-4 + 图片图层 60）
    let allItems = [];
    const layerIds = [1, 2, 3, 4, 60];
    for (const layerId of layerIds) {
        const items = await apiGet(`/playlists/${playlistId}/items?layerId=${layerId}`);
        if (items && Array.isArray(items)) {
            items.forEach((item, index) => {
                allItems.push({
                    ...item,
                    layerId: layerId,
                    originalIndex: index
                });
            });
        }
    }

    // 按图层和索引排序
    allItems.sort((a, b) => {
        if (a.layerId !== b.layerId) {
            return a.layerId - b.layerId;
        }
        return a.originalIndex - b.originalIndex;
    });

    // 更新素材数量统计
    updatePlaylistItemCount(allItems.length);

    // 显示所有项
    allItems.forEach((item, globalIndex) => {
        const fileName = escapeHtml(extractFileName(item.path));
        const folderPath = escapeHtml(getFolderFromPath(item.path));
        const playlistItem = document.createElement('div');
        playlistItem.className = 'playlist-item-content';
        playlistItem.dataset.layerId = item.layerId;
        playlistItem.dataset.originalIndex = item.originalIndex;
        playlistItem.innerHTML = `
            <div class="playlist-item-name">${fileName}</div>
            <div class="playlist-item-second-row">
                <div class="playlist-item-folder">${folderPath || '根目录'}</div>
                <div class="playlist-item-preview-layer">
                    <button class="btn small preview-playlist-item" title="预览">预览</button>
                    <span class="badge layer-badge">图层${item.layerId}</span>
                </div>
                <div class="playlist-item-actions">
                    <button class="btn small delete-playlist-item" title="删除">×</button>
                </div>
            </div>
        `;

        // 预览按钮事件：使用当前播放列表的图层播放，预览窗口显示该图层
        const previewBtn = playlistItem.querySelector('.preview-playlist-item');
        previewBtn.addEventListener('click', async (e) => {
            e.stopPropagation();
            const previewSucceeded = await previewPlaylistItem(item.path, playlistId, item.layerId);
            if (!previewSucceeded) {
                return;
            }
            const previewSwitch = document.getElementById('video-preview-switch');
            if (previewSwitch && previewSwitch.checked && item.layerId !== IMAGE_PLAYLIST_LAYER_ID) {
                const targetLayer = getPlaylistTargetLayer(playlistId) || 1;
                openVideoPreview(item.path, extractFileName(item.path), targetLayer);
            }
        });

        // 删除按钮事件
        const deleteBtn = playlistItem.querySelector('.delete-playlist-item');
        deleteBtn.addEventListener('click', async (e) => {
            e.stopPropagation();
            await deletePlaylistItem(playlistId, item.originalIndex, item.layerId);
            await updatePlaylistContent(playlistId);
        });

        container.appendChild(playlistItem);
    });
}

// 创建播放列表
async function createPlaylist(name, targetLayerId = 1, loopMode = 0, dmxId = 0) {
    try {
        const result = await apiPost('/playlists', {
            name: name,
            target_layerId: targetLayerId,
            dmxId: normalizePlaylistDmxId(dmxId)
        });

        if (result && result.id) {
            addToCommandLog('创建播放列表', 'success', `成功创建播放列表: ${name}`);

            // 如果创建成功，设置循环模式
            try {
                const configBody = { loop: loopMode };
                if (isImagePlaylistLayer(targetLayerId) && loopMode === IMAGE_FIXED_LOOP_MODE) {
                    configBody.displayDuration = 0;
                }
                await apiPut(`/playlists/${result.id}/config`, configBody);
            } catch (error) {
                // 如果设置循环模式失败，只记录日志，不影响创建成功
                addToCommandLog('设置循环模式', 'warning', '创建播放列表成功，但设置循环模式失败');
            }
            return result;
        } else {
            addToCommandLog('创建播放列表', 'error', '创建播放列表失败');
            return null;
        }
    } catch (error) {
        addToCommandLog('创建播放列表', 'error', error.message || '创建播放列表失败');
        return null;
    }
}

// 删除播放列表项
async function deletePlaylistItem(playlistId, index, layerId = 1) {
    const url = `/playlists/${playlistId}/items/${index}${layerId ? `?layerId=${layerId}` : ''}`;
    await handleApiOperation(
        apiDelete(url),
        '删除播放列表项',
        '成功删除播放列表项',
        '删除播放列表项失败'
    );
}

// 播放播放列表
export async function playPlaylist(playlistId, layerId) {
    // 图片播放列表（图层 60）由视频控制模块的 playVideo 处理
    if (layerId === 60) {
        await playVideo();
        return;
    }
    await handleApiOperation(
        // 不传 index，让后端按播放模式决定起点；loop=3 会从上次记录位置的下一条开始。
        apiPost('/video/play', {
            playlistId: playlistId,
            layerId: layerId
        }),
        '播放播放列表',
        `成功播放播放列表: ${currentPlaylistName}`,
        '播放播放列表失败',
        () => {
            setPlaylistMode();
        }
    );
}

// 设置默认播放列表
async function setDefaultPlaylist(playlistId) {
    const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
    const targetLayerId = playlist?.targetLayerId || null;
    const success = await handleApiOperation(
        apiPut('/playlists/default', {
            playlistId: playlistId,
            targetLayerId: targetLayerId
        }),
        '设为默认',
        `已将 "${currentPlaylistName}" 设为当前图层默认播放列表`,
        '设置默认播放列表失败',
        async () => {
            // 刷新播放列表以更新默认标记显示
            await updatePlaylistList();
        }
    );
    return success;
}


// 清空播放列表
export async function clearPlaylist(playlistId) {
    // 获取所有图层（1-4）的播放列表项并删除
    let totalDeleted = 0;

    const layerIds = [1, 2, 3, 4, 60];
    for (const layerId of layerIds) {
        const items = await apiGet(`/playlists/${playlistId}/items?layerId=${layerId}`);
        if (items && Array.isArray(items) && items.length > 0) {
            for (let i = items.length - 1; i >= 0; i--) {
                const url = `/playlists/${playlistId}/items/${i}?layerId=${layerId}`;
                await apiDelete(url);
                totalDeleted++;
            }
        }
    }

    if (totalDeleted > 0) {
        addToCommandLog('清空播放列表', 'success', `成功清空播放列表，删除了 ${totalDeleted} 个项目`);
    } else {
        addToCommandLog('清空播放列表', 'info', `播放列表已为空`);
    }
}

// 删除播放列表
async function deletePlaylist(playlistId) {
    return await handleApiOperation(
        apiDelete(`/playlists/${playlistId}`),
        '删除播放列表',
        '成功删除播放列表',
        '删除播放列表失败'
    );
}

// 根据播放列表绑定图层显示/隐藏幻灯片参数区域（仅图片图层 60 显示）
function toggleSlideshowSection(layerId) {
    const slideshowSection = document.getElementById('playlist-slideshow-section');
    if (slideshowSection) {
        slideshowSection.style.display = parseInt(layerId, 10) === 60 ? 'block' : 'none';
    }
}

function isImagePlaylistLayer(targetLayerId) {
    return parseInt(targetLayerId, 10) === IMAGE_PLAYLIST_LAYER_ID;
}

function syncPlaylistLoopModeVisibility(groupId, targetLayerId, currentLoopMode = null) {
    const imageOnlyVisible = isImagePlaylistLayer(targetLayerId);
    setImageOnlyLoopOptionVisible(groupId, imageOnlyVisible);

    if (!imageOnlyVisible && currentLoopMode === IMAGE_FIXED_LOOP_MODE) {
        updateButtonGroupState(groupId, '0');
    }
}

// 打开播放列表设置
async function openPlaylistSettings(playlistId) {
    try {
        const config = await apiGet(`/playlists/${playlistId}/config`);
        if (config) {
            updateButtonGroupState('playlist-loop-mode', String(config.loop || 0));

            const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
            const targetLayerId = playlist?.targetLayerId || 1;
            const dmxIdInput = document.getElementById('playlist-dmx-id');
            if (dmxIdInput && playlist) {
                dmxIdInput.value = String(playlist.dmxId || 0);
            }

            // 幻灯片参数（图片列表）
            const displayDurationEl = document.getElementById('playlist-display-duration');
            const fadeInEl = document.getElementById('playlist-fade-in-time');
            const fadeOutEl = document.getElementById('playlist-fade-out-time');
            if (displayDurationEl) displayDurationEl.value = config.displayDuration ?? 3;
            if (fadeInEl) fadeInEl.value = config.fadeInTime ?? 0.5;
            if (fadeOutEl) fadeOutEl.value = config.fadeOutTime ?? 0.5;

            syncPlaylistLoopModeVisibility('playlist-loop-mode', targetLayerId, config.loop || 0);
            if (isImagePlaylistLayer(targetLayerId) && config.loop === IMAGE_FIXED_LOOP_MODE && displayDurationEl) {
                displayDurationEl.value = '0';
            }

            toggleSlideshowSection(targetLayerId);

            showPlaylistSettingsDialog();
        }
    } catch (error) {
        addToCommandLog('播放列表设置', 'error', '获取播放列表设置失败');
    }
}

// 保存播放列表设置
async function savePlaylistSettings(playlistId) {
    let loopMode = parseInt(getButtonGroupValue('playlist-loop-mode'));
    const playlist = playlistsCache.find(p => String(p.id) === String(playlistId));
    const targetLayerId = playlist?.targetLayerId || 1;
    if (!isImagePlaylistLayer(targetLayerId) && loopMode === IMAGE_FIXED_LOOP_MODE) {
        loopMode = 0;
    }
    const dmxIdInput = document.getElementById('playlist-dmx-id');
    const dmxId = dmxIdInput ? normalizePlaylistDmxId(dmxIdInput.value) : 0;
    if (dmxIdInput) {
        dmxIdInput.value = String(dmxId);
    }

    const body = {
        loop: loopMode,
        dmxId
    };
    if (targetLayerId === IMAGE_PLAYLIST_LAYER_ID) {
        const displayDurationEl = document.getElementById('playlist-display-duration');
        const fadeInEl = document.getElementById('playlist-fade-in-time');
        const fadeOutEl = document.getElementById('playlist-fade-out-time');
        const rawDuration = displayDurationEl ? parseFloat(displayDurationEl.value) : 3;
        body.displayDuration = loopMode === IMAGE_FIXED_LOOP_MODE ? 0 : (isNaN(rawDuration) ? 3 : rawDuration);
        body.fadeInTime = fadeInEl ? parseFloat(fadeInEl.value) || 0.5 : 0.5;
        body.fadeOutTime = fadeOutEl ? parseFloat(fadeOutEl.value) || 0.5 : 0.5;
    }

    try {
        await handleApiOperation(
            apiPut(`/playlists/${playlistId}/config`, body),
            '播放列表设置',
            '播放列表设置已保存',
            '保存播放列表设置失败',
            async () => {
                if (playlist) {
                    playlist.dmxId = dmxId;
                }
                syncPlaylistLoopModeVisibility('playlist-loop-mode', targetLayerId, loopMode);
                hideDialog('playlist-settings-dialog');
                await updatePlaylistList(); // 刷新列表显示图层ID
            }
        );
    } catch (error) {
        addToCommandLog('播放列表设置', 'error', '保存播放列表设置失败');
    }
}
