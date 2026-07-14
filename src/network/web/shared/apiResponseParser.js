/**
 * Parse the single public HTTP API response contract.
 *
 * Wire format (and no other top-level fields):
 *   { ok: true,  data: any,  error: null }
 *   { ok: false, data: null, error: { code: string, message: string } }
 */
function invalidApiResponse() {
    const message = '后端返回不符合统一 API contract';
    return {
        ok: false,
        data: null,
        error: {
            code: 'INVALID_API_RESPONSE',
            message
        },
        message
    };
}

function hasExactKeys(value, expectedKeys) {
    const keys = Object.keys(value);
    return keys.length === expectedKeys.length && expectedKeys.every(key => keys.includes(key));
}

export function parseApiResponse(response) {
    if (!response || typeof response !== 'object' || Array.isArray(response)) {
        return invalidApiResponse();
    }

    if (!hasExactKeys(response, ['ok', 'data', 'error']) || typeof response.ok !== 'boolean') {
        return invalidApiResponse();
    }

    if (response.ok) {
        if (response.error !== null) {
            return invalidApiResponse();
        }
        return {
            ok: true,
            data: response.data,
            error: null,
            message: ''
        };
    }

    if (
        response.data !== null ||
        !response.error ||
        typeof response.error !== 'object' ||
        Array.isArray(response.error) ||
        !hasExactKeys(response.error, ['code', 'message']) ||
        typeof response.error.code !== 'string' ||
        response.error.code.length === 0 ||
        typeof response.error.message !== 'string'
    ) {
        return invalidApiResponse();
    }

    return {
        ok: false,
        data: null,
        error: {
            code: response.error.code,
            message: response.error.message
        },
        message: response.error.message
    };
}
