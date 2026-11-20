function decimalPlaces(value) {
    const text = String(value).toLowerCase();
    if (text.includes('e-')) {
        const [coefficient, exponent] = text.split('e-');
        const coefficientDecimals = coefficient.includes('.') ? coefficient.split('.')[1].length : 0;
        return Number(exponent) + coefficientDecimals;
    }
    return text.includes('.') ? text.split('.')[1].length : 0;
}

function normalizeValue(value, min, max, step) {
    const number = Number(value);
    const finite = Number.isFinite(number) ? number : min;
    const bounded = Math.max(min, Math.min(max, finite));
    if (!(step > 0)) return bounded;
    const snapped = min + Math.round((bounded - min) / step) * step;
    return Number(Math.max(min, Math.min(max, snapped)).toFixed(Math.min(6, decimalPlaces(step))));
}

function resolveDragHost(target) {
    if (!target) return null;
    if (target.classList?.contains('horizontal-drag-field')) return target;
    return target.closest?.('.horizontal-drag-field') || null;
}

export function renderHorizontalDragFill(target, value, options = {}) {
    const host = resolveDragHost(target);
    if (!host) return;
    const min = Number.isFinite(Number(options.min)) ? Number(options.min) : 0;
    const max = Number.isFinite(Number(options.max)) ? Number(options.max) : 1;
    const span = Math.max(Number.EPSILON, max - min);
    const number = Number(value);
    const finite = Number.isFinite(number) ? number : min;
    const ratio = Math.max(0, Math.min(1, (finite - min) / span));
    host.style.setProperty('--horizontal-drag-color', options.color || '245, 158, 11');
    host.style.setProperty('--horizontal-drag-level', `${ratio * 100}%`);
}

export function bindHorizontalDragControl(input, options = {}) {
    const host = options.host || resolveDragHost(input);
    if (!input || !host || host.dataset.horizontalDragBound) return;
    host.dataset.horizontalDragBound = 'true';

    const min = Number.isFinite(Number(options.min)) ? Number(options.min) : 0;
    const max = Number.isFinite(Number(options.max)) ? Number(options.max) : 1;
    const step = Number.isFinite(Number(options.step)) ? Math.abs(Number(options.step)) : 0;
    const longPressMs = Number.isFinite(Number(options.longPressMs)) ? Number(options.longPressMs) : 320;
    const moveTolerance = Number.isFinite(Number(options.moveTolerance)) ? Number(options.moveTolerance) : 8;
    let pendingPress = null;
    let dragPointerId = null;
    let lastDragValue = null;
    let suppressClick = false;

    const isDisabled = () => !!input.disabled || host.getAttribute('aria-disabled') === 'true';
    const renderValue = (value) => {
        renderHorizontalDragFill(host, value, {
            min,
            max,
            color: options.color
        });
        options.onRender?.(value);
    };
    const clearPendingPress = () => {
        if (pendingPress?.timer) clearTimeout(pendingPress.timer);
        pendingPress = null;
    };
    const valueFromPointer = (event) => {
        const rect = host.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left) / Math.max(1, rect.width)));
        return normalizeValue(min + ratio * (max - min), min, max, step);
    };
    const updateDrag = (event, final) => {
        if (dragPointerId !== event.pointerId) return;
        const value = valueFromPointer(event);
        lastDragValue = value;
        renderValue(value);
        options.onChange?.(value, { final, event });
        event.preventDefault();
    };
    const commitLastDragValue = (event) => {
        if (dragPointerId !== event.pointerId || lastDragValue === null) return;
        renderValue(lastDragValue);
        options.onChange?.(lastDragValue, { final: true, event });
        event.preventDefault();
    };
    const startDrag = (event = pendingPress?.event) => {
        const press = pendingPress;
        pendingPress = null;
        if (!press || isDisabled()) return;
        dragPointerId = press.pointerId;
        lastDragValue = null;
        suppressClick = true;
        input.blur?.();
        options.onStart?.(press.event);
        host.classList.add('is-horizontal-dragging');
        host.setPointerCapture?.(press.pointerId);
        updateDrag(event || press.event, false);
    };
    const finishDrag = (event, useLastValue = false) => {
        if (pendingPress?.pointerId === event.pointerId) clearPendingPress();
        if (dragPointerId !== event.pointerId) return;
        if (useLastValue) commitLastDragValue(event);
        else updateDrag(event, true);
        host.classList.remove('is-horizontal-dragging');
        if (!host.hasPointerCapture || host.hasPointerCapture(event.pointerId)) {
            host.releasePointerCapture?.(event.pointerId);
        }
        dragPointerId = null;
        lastDragValue = null;
        options.onEnd?.(event);
        setTimeout(() => {
            suppressClick = false;
        }, 0);
    };
    const finishFromOwner = (event) => {
        if (!event || (pendingPress?.pointerId !== event.pointerId && dragPointerId !== event.pointerId)) return;
        // Pointer capture normally routes these events to the host, but browsers
        // may deliver an early release/cancel on window after leaving the field.
        // Clear the long-press timer there so it cannot start a stale drag later.
        if (pendingPress?.pointerId === event.pointerId) clearPendingPress();
        if (dragPointerId === event.pointerId) finishDrag(event, event.type === 'pointercancel');
    };

    host.addEventListener('pointerdown', (event) => {
        if (event.button !== undefined && event.button !== 0) return;
        if (event.target?.closest?.('button') || isDisabled()) return;
        clearPendingPress();
        pendingPress = {
            pointerId: event.pointerId,
            x: event.clientX,
            y: event.clientY,
            event,
            timer: setTimeout(startDrag, longPressMs)
        };
    });
    host.addEventListener('pointermove', (event) => {
        if (dragPointerId === event.pointerId) {
            updateDrag(event, false);
            return;
        }
        if (!pendingPress || pendingPress.pointerId !== event.pointerId) return;
        if (Math.hypot(event.clientX - pendingPress.x, event.clientY - pendingPress.y) > moveTolerance) {
            startDrag(event);
        }
    });
    host.addEventListener('pointerup', (event) => finishDrag(event));
    host.addEventListener('pointercancel', (event) => finishDrag(event, true));
    host.addEventListener('pointerleave', (event) => {
        if (dragPointerId === null && pendingPress?.pointerId === event.pointerId) clearPendingPress();
    });
    host.addEventListener('contextmenu', (event) => {
        if (pendingPress || dragPointerId !== null) event.preventDefault();
    });
    host.addEventListener('click', (event) => {
        if (!suppressClick) return;
        event.preventDefault();
        event.stopPropagation();
    }, true);

    // Keep a window-level safety net for releases that occur outside the host
    // before pointer capture is established (notably touch/stylus cancel paths).
    if (typeof window !== 'undefined' && window.addEventListener) {
        window.addEventListener('pointerup', finishFromOwner);
        window.addEventListener('pointercancel', finishFromOwner);
    }

    const initialValue = normalizeValue(input.value, min, max, step);
    renderValue(initialValue);
}
