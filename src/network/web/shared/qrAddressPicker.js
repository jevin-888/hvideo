function getPickerParts(input) {
    const picker = input?.closest('[data-qr-address-picker]');
    if (!picker) return null;
    return {
        picker,
        menu: picker.querySelector('[data-qr-address-menu]'),
        toggle: picker.querySelector('[data-qr-address-toggle]')
    };
}

export function renderQrAddressOptions(input, addresses) {
    const parts = getPickerParts(input);
    if (!parts?.menu) return;

    parts.menu.replaceChildren(...addresses.map(address => {
        const option = document.createElement('button');
        option.type = 'button';
        option.className = 'qr-address-option';
        option.setAttribute('role', 'option');
        option.dataset.value = address;
        option.textContent = address;
        return option;
    }));
}

export function setupQrAddressPicker(input, addressesReady, registerCleanup) {
    const parts = getPickerParts(input);
    if (!parts?.menu || !parts.toggle) return;

    const closeMenu = () => {
        parts.menu.hidden = true;
        parts.toggle.setAttribute('aria-expanded', 'false');
    };
    const openMenu = () => {
        parts.menu.hidden = false;
        parts.toggle.setAttribute('aria-expanded', 'true');
    };
    const toggleHandler = event => {
        event.preventDefault();
        if (parts.menu.hidden) openMenu();
        else closeMenu();
    };
    const optionHandler = event => {
        const option = event.target.closest('.qr-address-option');
        if (!option || !parts.menu.contains(option)) return;
        input.value = option.dataset.value || '';
        input.dispatchEvent(new Event('input', { bubbles: true }));
        input.dispatchEvent(new Event('change', { bubbles: true }));
        closeMenu();
        input.focus();
    };
    const outsideHandler = event => {
        if (!parts.picker.contains(event.target)) closeMenu();
    };
    parts.toggle.addEventListener('click', toggleHandler);
    parts.menu.addEventListener('click', optionHandler);
    document.addEventListener('pointerdown', outsideHandler);
    Promise.resolve(addressesReady).then(() => {
        if (!parts.menu.children.length) closeMenu();
    });

    registerCleanup?.(() => {
        parts.toggle.removeEventListener('click', toggleHandler);
        parts.menu.removeEventListener('click', optionHandler);
        document.removeEventListener('pointerdown', outsideHandler);
        closeMenu();
    });
}
