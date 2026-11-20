/**
 * @file login.js（文件名）
 * @brief 登录页面模块
 *
 * 本模块提供用户登录功能，包括：
 * - 登录表单处理
 * - 认证令牌管理
 * - 登录状态检查
 */

import '/shared/interactionGuards.js';
import { parseApiResponse } from '../../shared/apiResponseParser.js';
import { getAuthToken, clearAuthToken, saveAuthToken, isLoggedIn, validateSession } from '/shared/authSession.js';

async function readApiResponse(response) {
    const text = await response.text();
    if (!text) throw new Error('后端返回空响应，不符合统一 API contract');
    let json;
    try {
        json = JSON.parse(text);
    } catch (_) {
        throw new Error('后端返回非 JSON 响应，不符合统一 API contract');
    }
    const parsed = parseApiResponse(json);
    if (!response.ok || !parsed.ok) {
        throw new Error(parsed.error?.message || `HTTP ${response.status}`);
    }
    return parsed.data;
}

/**
 * @brief 登录函数
 */
async function login(username, password, loginType) {
    try {
        const response = await fetch('/api/v1/auth/login', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ username, password, loginType })
        });
        
        const data = await readApiResponse(response);
        
        if (data && data.token) {
            saveAuthToken(data.token, data.expiresIn || 2 * 60 * 60 * 1000);
            return { success: true };
        } else {
            return { success: false, message: '登录响应缺少 token' };
        }
    } catch (error) {
        console.error('Login error:', error);
        return { success: false, message: error.message || '网络错误，请稍后重试' };
    }
}

/**
 * @brief 登出函数
 */
async function logout() {
    try {
        const token = getAuthToken();
        if (token) {
            const response = await fetch('/api/v1/auth/logout', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Authorization': `Bearer ${token}`
                }
            });
            await readApiResponse(response);
        }
    } catch (error) {
        console.error('Logout error:', error);
    } finally {
        clearAuthToken();
        window.location.href = '/login.html';
    }
}

/**
 * @brief 初始化登录页面
 */
function initLoginPage() {
    const destination = new URLSearchParams(window.location.search).get('next') === 'factory-serial'
        ? '/factory-serial.html' : '/index.html';
    const loginForm = document.getElementById('login-form');
    const loginButton = document.getElementById('login-button');
    const errorMessage = document.getElementById('error-message');
    const loginTypeSelect = document.getElementById('login-type');
    if (!loginForm) return;
    const loginTypeKey = 'hvideo_login_type';
    if (loginTypeSelect) {
        try {
            const savedType = localStorage.getItem(loginTypeKey);
            loginTypeSelect.value = savedType === 'local' ? 'local' : 'cloud';
        } catch (_) { /* 浏览器禁用存储时仍可正常选择登录方式。 */ }
    }
    function updateLoginHint() {
        const cloud = loginTypeSelect?.value === 'cloud';
        document.getElementById('login-subtitle').textContent = cloud
            ? '使用云端代理商账号和密码登录'
            : '请登录以继续';
    }
    loginTypeSelect?.addEventListener('change', () => {
        try {
            localStorage.setItem(loginTypeKey, loginTypeSelect.value === 'cloud' ? 'cloud' : 'local');
        } catch (_) { /* 偏好保存失败不阻止登录。 */ }
        const usernameInput = document.getElementById('username');
        if (loginTypeSelect.value === 'local' && !usernameInput.value) usernameInput.value = 'admin';
        if (loginTypeSelect.value === 'cloud' && usernameInput.value === 'admin') usernameInput.value = '';
        document.getElementById('password').value = '';
        errorMessage.classList.remove('show');
        updateLoginHint();
    });

    updateLoginHint();
    
    async function loadLoginConfig() {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 5000);
        try {
            const response = await fetch('/api/v1/auth/config', {
                cache: 'no-store', signal: controller.signal
            });
            const data = await readApiResponse(response);
            if (typeof data?.localPasswordConfigured !== 'boolean') {
                throw new Error('登录配置无效，请刷新页面重试');
            }
            updateLoginHint();
            const usernameInput = document.getElementById('username');
            if (loginTypeSelect?.value !== 'cloud' && !usernameInput.value) usernameInput.value = 'admin';
        } finally {
            clearTimeout(timeout);
        }
    }

    let retryTimer = null;
    let autoLoginAttempted = false;
    async function prepareLogin() {
        loginButton.disabled = true;
        try {
            if (await validateSession()) {
                window.location.replace(destination);
                return;
            }
            await loadLoginConfig();
            errorMessage.classList.remove('show');
            loginButton.disabled = false;
            if (!autoLoginAttempted && new URLSearchParams(window.location.search).get('terminalAutoLogin') === '1') {
                autoLoginAttempted = true;
                const usernameInput = document.getElementById('username');
                const passwordInput = document.getElementById('password');
                if (loginTypeSelect) loginTypeSelect.value = 'local';
                if (usernameInput) usernameInput.value = 'admin';
                if (passwordInput) passwordInput.value = 'admin*';
                loginForm.requestSubmit();
            }
        } catch (_) {
            showError('设备暂未连接，正在重试...');
            retryTimer = setTimeout(prepareLogin, 3000);
        }
    }
    window.addEventListener('pagehide', () => clearTimeout(retryTimer), { once: true });
    void prepareLogin();
    
    loginForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        if (loginButton.disabled) return;
        
        const username = document.getElementById('username').value.trim();
        const password = document.getElementById('password').value;
        
        if (!username || !password) {
            showError('请输入用户名和密码');
            return;
        }
        
        // 禁用按钮，显示加载状态
        loginButton.disabled = true;
        loginButton.innerHTML = '<span class="loading"></span>登录中...';
        errorMessage.classList.remove('show');
        
        const result = await login(username, password, loginTypeSelect?.value === 'cloud' ? 'cloud' : 'local');
        
        // 恢复按钮状态
        loginButton.disabled = false;
        loginButton.innerHTML = '登录';
        
        if (result.success) {
            // 登录成功，跳转到主页
            window.location.href = destination;
        } else {
            showError(result.message || '登录失败');
        }
    });
    
    function showError(message) {
        errorMessage.textContent = message;
        errorMessage.classList.add('show');
    }
    
}

// 导出函数供其他模块使用
export { getAuthToken, clearAuthToken, isLoggedIn, logout };

// 如果当前是登录页面，初始化登录逻辑
if (window.location.pathname.endsWith('/login.html') || window.location.pathname === '/login.html') {
    document.addEventListener('DOMContentLoaded', initLoginPage);
}
