import { parseApiResponse } from './apiResponseParser.js';

const TOKEN_KEY = 'hvideo_auth_token';
const EXPIRY_KEY = 'hvideo_auth_token_expiry';
let redirecting = false;

export function clearAuthToken() {
    localStorage.removeItem(TOKEN_KEY);
    localStorage.removeItem(EXPIRY_KEY);
}

export function getAuthToken() {
    const token = localStorage.getItem(TOKEN_KEY);
    const expiry = Number(localStorage.getItem(EXPIRY_KEY));
    if (!token || !Number.isFinite(expiry) || expiry <= Date.now()) {
        clearAuthToken();
        return null;
    }
    return token;
}

export function saveAuthToken(token, expiresIn = 2 * 60 * 60 * 1000) {
    localStorage.setItem(TOKEN_KEY, token);
    localStorage.setItem(EXPIRY_KEY, String(Date.now() + expiresIn));
}

export function isLoggedIn() {
    return getAuthToken() !== null;
}

export function isAuthRedirecting() {
    return redirecting;
}

export function handleUnauthorized(response, requestToken) {
    if (response.status !== 401) return false;
    // A late response for an older session must not invalidate a newer login.
    if (localStorage.getItem(TOKEN_KEY) !== requestToken) return true;
    if (!redirecting) {
        redirecting = true;
        clearAuthToken();
        window.dispatchEvent(new Event('authSessionExpired'));
        window.location.replace('/login.html');
    }
    return true;
}

export async function validateSession() {
    const token = getAuthToken();
    if (!token) return false;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 5000);
    try {
        const response = await fetch('/api/v1/auth/validate', {
            headers: { Authorization: `Bearer ${token}` },
            cache: 'no-store',
            signal: controller.signal
        });
        if (response.status === 401) {
            if (localStorage.getItem(TOKEN_KEY) === token) clearAuthToken();
            return false;
        }
        const parsed = parseApiResponse(await response.json());
        if (!response.ok || !parsed.ok) throw new Error('Session validation unavailable');
        return parsed.data?.valid === true && getAuthToken() === token;
    } finally {
        clearTimeout(timeout);
    }
}
