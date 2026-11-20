import { createApiClient } from '/shared/apiClient.js';
import { handleUnauthorized, isAuthRedirecting } from '/shared/authSession.js';

const client = createApiClient({
    getAuthToken: () => localStorage.getItem('hvideo_auth_token'),
    handleUnauthorized,
    isAuthRedirecting
});

export const {
    getLastApiError, apiRequest, apiGet, apiPost, apiPut, apiDelete, apiAction, sendLayerCommand
} = client;
