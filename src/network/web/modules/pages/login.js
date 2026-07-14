/**
 * @file login.js（文件名）
 * @brief 登录页面模块
 *
 * 本模块提供用户登录功能，包括：
 * - 登录表单处理
 * - 认证令牌管理
 * - 登录状态检查
 */

import '../core/interactionGuards.js';
import { parseApiResponse } from '../../shared/apiResponseParser.js';

const AUTH_TOKEN_KEY = 'hvideo_auth_token';
const AUTH_TOKEN_EXPIRY_KEY = 'hvideo_auth_token_expiry';

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
 * @brief 保存认证令牌
 */
function saveAuthToken(token, expiresIn = 2 * 60 * 60 * 1000) {
    const expiry = Date.now() + expiresIn;
    localStorage.setItem(AUTH_TOKEN_KEY, token);
    localStorage.setItem(AUTH_TOKEN_EXPIRY_KEY, expiry.toString());
}

/**
 * @brief 获取认证令牌
 */
function getAuthToken() {
    const token = localStorage.getItem(AUTH_TOKEN_KEY);
    const expiry = localStorage.getItem(AUTH_TOKEN_EXPIRY_KEY);
    
    if (!token || !expiry) {
        return null;
    }
    
    // 检查令牌是否过期
    if (Date.now() > parseInt(expiry)) {
        clearAuthToken();
        return null;
    }
    
    return token;
}

/**
 * @brief 清除认证令牌
 */
function clearAuthToken() {
    localStorage.removeItem(AUTH_TOKEN_KEY);
    localStorage.removeItem(AUTH_TOKEN_EXPIRY_KEY);
}

/**
 * @brief 检查是否已登录
 */
function isLoggedIn() {
    return getAuthToken() !== null;
}

/**
 * @brief 登录函数
 */
async function login(username, password) {
    try {
        const response = await fetch('/api/v1/auth/login', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ username, password })
        });
        
        const data = await readApiResponse(response);
        
        if (data && data.token) {
            saveAuthToken(data.token, data.expiresIn || 2 * 60 * 60 * 1000);
            // 跳转到主页
            window.location.href = '/index.html';
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
    const loginForm = document.getElementById('login-form');
    const loginButton = document.getElementById('login-button');
    const errorMessage = document.getElementById('error-message');
    
    if (!loginForm) return;
    
    // 如果已登录，跳转到主页
    if (isLoggedIn()) {
        window.location.href = '/index.html';
        return;
    }
    
    loginForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        
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
        
        const result = await login(username, password);
        
        // 恢复按钮状态
        loginButton.disabled = false;
        loginButton.innerHTML = '登录';
        
        if (result.success) {
            // 登录成功，跳转到主页
            window.location.href = '/index.html';
        } else {
            showError(result.message || '登录失败');
        }
    });
    
    function showError(message) {
        errorMessage.textContent = message;
        errorMessage.classList.add('show');
    }
    
    // 支持回车键提交
    document.getElementById('password').addEventListener('keypress', (e) => {
        if (e.key === 'Enter') {
            loginForm.dispatchEvent(new Event('submit'));
        }
    });
}

// 导出函数供其他模块使用
export { getAuthToken, clearAuthToken, isLoggedIn, logout };

// 如果当前是登录页面，初始化登录逻辑
if (window.location.pathname.endsWith('/login.html') || window.location.pathname === '/login.html') {
    document.addEventListener('DOMContentLoaded', initLoginPage);
}
