// 特效图层参数 HTML
export function generateEffectHtml(layer) {
    const effectId = layer.effect_id || '';
    const effectParams = layer.effect_params || '';
    return `
        <div class="setting-section">
            <div class="setting-item" style="grid-column: span 4;">
                <label for="effect-id">特效ID:</label>
                <input type="text" id="effect-id" class="form-control" value="${effectId}" placeholder="如: blur, mosaic">
            </div>
            <div class="setting-item" style="grid-column: 1 / -1;">
                <label for="effect-params">特效参数 (JSON):</label>
                <input type="text" id="effect-params" class="form-control" value="${effectParams}" placeholder='{"intensity": 0.5}'>
            </div>
        </div>
    `;
}
