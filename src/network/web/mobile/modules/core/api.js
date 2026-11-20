import { createApiClient } from '/shared/apiClient.js';

export const {
    getLastApiError, apiRequest, apiGet, apiPost, apiPut, apiDelete,
    apiAction, apiActionOrThrow, sendLayerCommand
} = createApiClient();
