const LEGACY_LABELED_ADDRESS_PREFIX = /^\s*\d+\s*[.、]/;
const HTTP_ADDRESS = /https?:\/\/\S+/i;

export function normalizeQrAddressValue(value) {
    const text = String(value ?? '').trim();
    if (!LEGACY_LABELED_ADDRESS_PREFIX.test(text)) return text;

    const match = text.match(HTTP_ADDRESS);
    return match ? match[0].replace(/[)）]+$/, '') : text;
}

export function buildQrAddressOptions(ip, onlineVodEnabled, onlineVodHost) {
    const host = String(ip || '').trim() || '127.0.0.1';
    const addresses = [`http://${host}:8081/`];
    const vodHost = String(onlineVodHost || '').trim();
    if (onlineVodEnabled && vodHost) {
        addresses.push(`http://${vodHost}:9898/vod_song/mobile?${host}`);
    }
    return addresses;
}
