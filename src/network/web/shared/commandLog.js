// 命令日志模块

const MAX_COMMAND_LOG_ENTRIES = 100;

// 保留最近一段命令记录，避免连续串口收发时后续帧覆盖 RX/TX 证据。
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

    logContainer.appendChild(logEntry);
    while (logContainer.children.length > MAX_COMMAND_LOG_ENTRIES) {
        logContainer.removeChild(logContainer.firstElementChild);
    }
    logContainer.scrollTop = logContainer.scrollHeight;
}
