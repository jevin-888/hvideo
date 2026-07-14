// 命令日志模块

// 初始化命令日志
export function initializeCommandLog() {
    // 命令日志初始化逻辑
}

// 添加日志条目（只保留一行，始终显示最新）
export function addToCommandLog(action, type, message) {
    const logContainer = document.getElementById('command-log');
    if (!logContainer) return;

    const logEntry = document.createElement('div');
    logEntry.className = `log-entry ${type}`;

    const timeEl = document.createElement('span');
    timeEl.className = 'log-time';
    timeEl.textContent = new Date().toLocaleTimeString();

    const actionEl = document.createElement('span');
    actionEl.className = 'log-action';
    actionEl.textContent = action ?? '';

    const messageEl = document.createElement('span');
    messageEl.className = 'log-message';
    messageEl.textContent = message ?? '';

    logEntry.append(timeEl, actionEl, messageEl);

    logContainer.innerHTML = '';
    logContainer.appendChild(logEntry);
    logContainer.scrollTop = logContainer.scrollHeight;
}
