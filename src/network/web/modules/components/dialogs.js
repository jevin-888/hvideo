// 对话框管理模块

// 初始化对话框
export function initializeDialogs() {
    // 绑定对话框关闭按钮事件
    document.querySelectorAll('.dialog').forEach(dialog => {
        // 点击遮罩层关闭对话框
        dialog.addEventListener('click', function (e) {
            if (e.target === this) {
                hideDialog(this.id);
            }
        });
    });

    // 绑定取消按钮事件
    const cancelButtons = document.querySelectorAll('[id^="cancel-"]');
    cancelButtons.forEach(btn => {
        btn.addEventListener('click', hideAllDialogs);
    });
}

// 显示指定对话框
function showDialog(dialogId) {
    const dialog = document.getElementById(dialogId);
    if (dialog) {
        dialog.classList.add('active');
    }
}

// 隐藏指定对话框
export function hideDialog(dialogId) {
    const dialog = document.getElementById(dialogId);
    if (dialog) {
        dialog.classList.remove('active');
    }
}

// 显示保存场景对话框
function showSaveSceneDialog() {
    showDialog('save-scene-dialog');
}

// 显示加载场景对话框
function showLoadSceneDialog() {
    showDialog('load-scene-dialog');
}

// 显示删除场景对话框
function showDeleteSceneDialog() {
    showDialog('delete-scene-dialog');
}

// 显示播放列表设置对话框
export function showPlaylistSettingsDialog() {
    showDialog('playlist-settings-dialog');
}

// 显示新建播放列表对话框
export function showCreatePlaylistDialog() {
    showDialog('create-playlist-dialog');
}

// 隐藏所有对话框
function hideAllDialogs() {
    document.querySelectorAll('.dialog').forEach(dialog => {
        dialog.classList.remove('active');
    });
}