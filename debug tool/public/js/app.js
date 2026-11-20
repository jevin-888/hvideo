(() => {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const invoke = window.__TAURI__?.tauri?.invoke;
  const listen = window.__TAURI__?.event?.listen;
  let devices = [];
  let selected = null;
  let scanning = false;
  let opening = false;
  let scanned = false;
  let page = 1;
  let lastDevice = null;
  let logoClicks = 0;
  let serialUnlockPrompted = false;
  let serialUnlockResolve;
  let deviceViewUrl = '';
  let returningFromDebug = false;
  const keyOf = (device) => `${device.ip}:${device.ports.http}`;
  const nameOf = (device) => device.device_name || 'HVIDEO 播放设备';

  function message(id, text, error = false) {
    $(id).textContent = text;
    $(id).classList.toggle('error', error);
  }

  function renderSelection() {
    $('no-selection').hidden = Boolean(selected);
    $('selection-details').hidden = !selected;
    $('login-button').disabled = !selected || opening || !invoke;
    $('serial-button').disabled = !selected || opening || !invoke;
    $('serial-button').title = selected ? `为 ${selected.ip} 写入出厂序列号` : '请先选择设备';
    $('login-button').replaceChildren(document.createTextNode(opening ? '正在连接…' : '进入调试 '));
    const arrow = document.createElement('span');
    arrow.textContent = '→';
    arrow.setAttribute('aria-hidden', 'true');
    $('login-button').append(arrow);
    $('manual-button').disabled = opening;
    $('selection-hint').textContent = selected ? selected.manual ? '手动指定地址，连接时检查设备' : '确认设备后，自动登录并进入调试' : '先选择设备，再进入调试';
    if (selected) {
      $('selected-name').textContent = nameOf(selected);
      $('selected-address').textContent = selected.ip;
      $('selected-model').textContent = selected.model || '—';
      $('selected-serial').textContent = selected.serial || '—';
      $('selected-mac').textContent = selected.mac || '—';
    }
    for (const row of $('device-list').children) {
      const active = selected && row.dataset.key === keyOf(selected);
      row.classList.toggle('selected', Boolean(active));
      row.querySelector('button').setAttribute('aria-pressed', String(Boolean(active)));
      row.querySelector('button').disabled = opening;
    }
  }

  function selectDevice(device) {
    if (opening) return;
    selected = device;
    message('connect-status', '');
    renderSelection();
  }

  function renderDevices() {
    const query = $('device-filter').value.trim().toLocaleLowerCase();
    const model = $('model-filter').value;
    const filtered = devices.filter((device) => (!model || device.model === model) &&
      [device.device_name, device.ip, device.model, device.serial, device.mac]
        .some((value) => String(value || '').toLocaleLowerCase().includes(query)));
    const ipCompare = (a, b) => {
      const left = a.ip.split('.').map(Number);
      const right = b.ip.split('.').map(Number);
      for (let i = 0; i < 4; i += 1) if (left[i] !== right[i]) return left[i] - right[i];
      return a.ports.http - b.ports.http;
    };
    filtered.sort($('device-sort').value === 'name'
      ? (a, b) => nameOf(a).localeCompare(nameOf(b), 'zh-CN', { numeric: true }) || ipCompare(a, b)
      : ipCompare);
    const pageSize = Number($('page-size').value);
    const pages = Math.max(1, Math.ceil(filtered.length / pageSize));
    page = Math.max(1, Math.min(page, pages));
    const start = (page - 1) * pageSize;
    const batch = filtered.slice(start, start + pageSize);
    $('device-count').textContent = String(devices.length);
    $('result-count').textContent = filtered.length
      ? `${start + 1}–${start + batch.length} / ${filtered.length} 台${filtered.length !== devices.length ? `（全部 ${devices.length} 台）` : ''}`
      : devices.length ? `0 台匹配（全部 ${devices.length} 台）` : '共 0 台设备';
    $('page-indicator').textContent = `${page} / ${pages}`;
    $('previous-page').disabled = page === 1;
    $('next-page').disabled = page === pages;
    $('device-list').replaceChildren();
    $('empty-state').hidden = batch.length > 0;
    if (!batch.length) {
      $('empty-state').querySelector('h2').textContent = scanning ? '正在发现设备' : devices.length ? '没有匹配的设备' : scanned ? '未发现设备' : '等待扫描设备';
      $('empty-state').querySelector('p').textContent = scanning ? '发现后立即显示，请保持电脑与设备处于同一网络。' : devices.length ? '试试其他关键词，或将型号切换为「全部型号」。' : '请检查网卡与设备网络，重新扫描；也可使用右上角「手动连接」。';
    }
    const fragment = document.createDocumentFragment();
    for (const device of batch) {
      const row = document.createElement('tr');
      row.dataset.key = keyOf(device);
      const nameCell = document.createElement('td');
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'device-select';
      button.setAttribute('aria-label', `选择 ${nameOf(device)}，${device.ip}`);
      const icon = document.createElement('span');
      icon.className = 'device-icon';
      icon.textContent = 'H';
      icon.setAttribute('aria-hidden', 'true');
      const title = document.createElement('span');
      title.className = 'device-title';
      const name = document.createElement('strong');
      name.textContent = nameOf(device);
      name.title = nameOf(device);
      const caption = document.createElement('small');
      caption.textContent = 'HVIDEO 播放设备';
      title.append(name, caption);
      button.append(icon, title);
      nameCell.append(button);
      const ipCell = document.createElement('td');
      ipCell.className = 'ip-text';
      ipCell.textContent = device.ip;
      ipCell.title = device.ip;
      const modelCell = document.createElement('td');
      const modelText = document.createElement('span');
      modelText.className = 'model-text';
      modelText.textContent = device.model || '—';
      modelText.title = device.model || '设备未上报型号';
      modelCell.append(modelText);
      const choice = document.createElement('td');
      choice.className = 'choice';
      const circle = document.createElement('span');
      circle.className = 'selection-circle';
      circle.textContent = '✓';
      circle.setAttribute('aria-hidden', 'true');
      choice.append(circle);
      row.append(nameCell, ipCell, modelCell, choice);
      row.addEventListener('click', () => selectDevice(device));
      fragment.append(row);
    }
    $('device-list').append(fragment);
    renderSelection();
  }

  async function refreshInterfaces() {
    const current = $('network-interface').value;
    const interfaces = await invoke('get_network_interfaces');
    $('network-interface').replaceChildren(new Option('全部网卡', ''));
    for (const network of interfaces) $('network-interface').add(new Option(`${network.name} · ${network.ip}`, network.ip));
    if (interfaces.some((network) => network.ip === current)) $('network-interface').value = current;
    if (!interfaces.length) throw new Error('没有可用的 IPv4 网卡，请连接网络后重新扫描。');
  }

  function updateModels() {
    const current = $('model-filter').value;
    $('model-filter').replaceChildren(new Option('全部型号', ''));
    const models = [...new Set(devices.map((device) => device.model).filter(Boolean))].sort();
    for (const model of models) $('model-filter').add(new Option(model, model));
    if (models.includes(current)) $('model-filter').value = current;
  }

  async function scanDevices() {
    if (scanning || opening || !invoke) return;
    scanning = true;
    const previousKey = selected && !selected.manual ? keyOf(selected) : null;
    devices = [];
    if (!selected?.manual) selected = null;
    page = 1;
    const scanId = crypto.randomUUID();
    const found = new Map();
    let unlisten;
    let renderTimer;
    let accepting = true;
    $('scan-button').disabled = true;
    $('scan-button').textContent = '扫描中…';
    $('network-interface').disabled = true;
    $('device-table').setAttribute('aria-busy', 'true');
    $('scan-warnings').hidden = true;
    message('status', '正在发现设备，收到响应即可连接…');
    renderDevices();
    try {
      await refreshInterfaces();
      if (listen) {
        try {
          unlisten = await listen('discovery-device', ({ payload }) => {
            if (!accepting || payload.scan_id !== scanId) return;
            const device = payload.device;
            found.set(keyOf(device), device);
            devices = [...found.values()];
            if ((!selected && keyOf(device) === previousKey) ||
                (selected && !selected.manual && keyOf(device) === keyOf(selected))) selected = device;
            if (!renderTimer) renderTimer = setTimeout(() => {
              renderTimer = null;
              updateModels();
              renderDevices();
              message('status', `已发现 ${devices.length} 台，可立即连接；继续查找其他设备…`);
            }, 50);
          });
        } catch (_) { /* 事件订阅不可用时仍使用扫描完成后的完整结果。 */ }
      }
      const result = await invoke('scan_devices', { interfaceIp: $('network-interface').value || null, scanId });
      devices = result.devices;
      if (!selected && previousKey) selected = devices.find((device) => keyOf(device) === previousKey) || null;
      if (selected && !selected.manual) {
        const previous = selected;
        selected = devices.find((device) => keyOf(device) === keyOf(previous)) || null;
        if (!selected) message('connect-status', '本次未发现原选设备，请重新选择。');
      }
      $('scan-time').textContent = `${new Date().toLocaleTimeString('zh-CN')} 更新`;
      message('status', devices.length ? `已发现 ${devices.length} 台设备，点击列表选择。` : '未收到设备响应，请检查网络或手动连接。');
      $('warning-detail').textContent = result.warnings.join('\n');
      $('scan-warnings').hidden = !result.warnings.length;
    } catch (error) {
      message('status', `扫描失败：${error.message || error}`, true);
    } finally {
      accepting = false;
      clearTimeout(renderTimer);
      if (unlisten) unlisten();
      scanned = true;
      scanning = false;
      $('scan-button').disabled = opening;
      $('scan-button').textContent = '↻ 重新扫描';
      $('network-interface').disabled = false;
      $('device-table').setAttribute('aria-busy', 'false');
      updateModels();
      renderDevices();
    }
  }

  async function openSelectedDevice(factorySerial = false) {
    if (opening || !selected || !invoke) return;
    const { ip, ports } = selected;
    const port = ports.http;
    opening = true;
    $('scan-button').disabled = true;
    renderSelection();
    message('connect-status', `正在连接 ${ip}…`);
    try {
      const result = await invoke(factorySerial ? 'open_serial_window' : 'open_debug_window', { ip, port });
      if (!factorySerial) {
        deviceViewUrl = result;
        await invoke('set_debug_window_chrome', { hidden: true });
        $('device-view-back').hidden = false;
        $('app').hidden = true;
        $('device-view').hidden = false;
        $('device-frame').src = deviceViewUrl;
        document.title = `HVIDEO · 设备调试 · ${ip}`;
        $('device-view-back').focus();
      }
      message('connect-status', factorySerial ? '已打开写号窗口；需设备安装支持写号的新版播放器。' : '已在当前窗口打开设备调试页面。');
      lastDevice = { ip, port };
      try { localStorage.setItem('hsvj-debug-last-device', JSON.stringify(lastDevice)); } catch (_) { /* 存储不可用不影响连接。 */ }
    } catch (error) {
      const detail = String(error.message || error).replaceAll(`${ip}:${port}`, ip);
      message('connect-status', `连接失败：${detail}`, true);
    } finally {
      opening = false;
      $('scan-button').disabled = scanning;
      renderSelection();
    }
  }

  async function returnToDeviceList() {
    if (returningFromDebug) return;
    returningFromDebug = true;
    try {
      if (invoke) {
        await Promise.allSettled([
          invoke('stop_debug_reconnect'),
          invoke('set_debug_window_chrome', { hidden: false }),
        ]);
      }
      deviceViewUrl = '';
      $('device-frame').src = 'about:blank';
      $('device-view').hidden = true;
      $('app').hidden = false;
      document.title = 'HVIDEO · 连接设备';
      $('login-button').focus();
    } finally {
      returningFromDebug = false;
    }
  }
  $('device-view-back').addEventListener('click', returnToDeviceList);
  window.addEventListener('message', (event) => {
    if (!deviceViewUrl || event.source !== $('device-frame').contentWindow ||
        event.origin !== new URL(deviceViewUrl).origin) return;
    if (event.data?.type === 'hvideo-debug-navigation-request') {
      event.source.postMessage({ type: 'hvideo-debug-navigation-attach' }, event.origin);
    } else if (event.data?.type === 'hvideo-debug-home-ready') {
      $('device-view-back').hidden = true;
    } else if (event.data?.type === 'hvideo-debug-home') {
      void returnToDeviceList();
    }
  });
  $('device-frame').addEventListener('load', () => {
    if (!deviceViewUrl) return;
    // The fallback remains available on login pages and connection error pages.
    $('device-view-back').hidden = false;
    $('device-frame').contentWindow.postMessage({ type: 'hvideo-debug-navigation-attach' }, new URL(deviceViewUrl).origin);
  });
  window.addEventListener('debug-device-recovered', (event) => {
    if (!deviceViewUrl || event.detail !== new URL(deviceViewUrl).origin) return;
    $('device-view-back').hidden = false;
    $('device-frame').src = deviceViewUrl;
  });

  function resetPage() {
    page = 1;
    $('device-table').scrollTop = 0;
    renderDevices();
  }
  function requestSerialUnlock() {
    return new Promise((resolve) => {
      serialUnlockResolve = resolve;
      $('serial-unlock-password').value = '';
      message('serial-unlock-error', '');
      $('serial-unlock-dialog').showModal();
      $('serial-unlock-password').focus();
    });
  }
  function closeSerialUnlock(value) {
    if (!serialUnlockResolve) return;
    $('serial-unlock-dialog').close();
    const resolve = serialUnlockResolve;
    serialUnlockResolve = null;
    resolve(value);
  }
  $('serial-unlock-form').addEventListener('submit', (event) => {
    event.preventDefault();
    if ($('serial-unlock-password').value === '989898') closeSerialUnlock(true);
    else message('serial-unlock-error', '密码错误，请重新输入。', true);
  });
  $('close-serial-unlock').addEventListener('click', () => closeSerialUnlock(false));
  $('cancel-serial-unlock').addEventListener('click', () => closeSerialUnlock(false));
  $('serial-unlock-dialog').addEventListener('cancel', (event) => { event.preventDefault(); closeSerialUnlock(false); });
  $('brand-logo').addEventListener('click', async () => {
    if (logoClicks < 5) logoClicks += 1;
    if (logoClicks === 5 && !serialUnlockPrompted) {
      serialUnlockPrompted = true;
      if (await requestSerialUnlock()) {
        $('serial-button').hidden = false;
      } else {
        logoClicks = 0;
        serialUnlockPrompted = false;
        message('status', '密码错误，写号按钮未开启。', true);
      }
    }
  });
  $('scan-button').addEventListener('click', () => void scanDevices());
  $('login-button').addEventListener('click', () => void openSelectedDevice());
  $('serial-button').addEventListener('click', () => void openSelectedDevice(true));
  $('app-update-button').addEventListener('click', () => window.HvideoAppUpdate.open(devices, selected));
  $('system-update-button').addEventListener('click', () => window.HvideoSystemUpdate.open(devices, selected));
  $('device-filter').addEventListener('input', resetPage);
  for (const id of ['model-filter', 'device-sort', 'page-size']) $(id).addEventListener('change', resetPage);
  $('previous-page').addEventListener('click', () => { page -= 1; $('device-table').scrollTop = 0; renderDevices(); });
  $('next-page').addEventListener('click', () => { page += 1; $('device-table').scrollTop = 0; renderDevices(); });
  $('manual-button').addEventListener('click', () => { message('manual-error', ''); $('manual-dialog').showModal(); });
  $('close-manual').addEventListener('click', () => $('manual-dialog').close());
  $('connect-form').addEventListener('submit', (event) => {
    event.preventDefault();
    if (opening) return;
    const ip = $('device-ip').value.trim();
    const parts = ip.split('.');
    if (parts.length !== 4 || parts.some((part) => !/^(0|[1-9]\d{0,2})$/.test(part) || Number(part) > 255)
      || ip === '0.0.0.0' || ip === '255.255.255.255' || (Number(parts[0]) >= 224 && Number(parts[0]) <= 239)) {
      message('manual-error', '请输入有效的 IPv4 地址。', true);
      return;
    }
    const port = lastDevice?.ip === ip ? lastDevice.port : 8080;
    selected = devices.find((device) => device.ip === ip)
      || { ip, ports: { http: port }, device_name: '手动连接设备', manual: true };
    message('connect-status', '');
    $('manual-dialog').close();
    renderSelection();
  });
  try {
    const saved = JSON.parse(localStorage.getItem('hsvj-debug-last-device'));
    if (saved && typeof saved.ip === 'string' && Number.isInteger(saved.port) && saved.port > 0 && saved.port <= 65535) {
      $('device-ip').value = saved.ip;
      lastDevice = saved;
    }
  } catch (_) { /* 忽略不可读取的连接偏好。 */ }
  renderDevices();
  $('app-update-button').disabled = !invoke;
  $('system-update-button').disabled = !invoke;
  if (!invoke) {
    message('status', '请使用桌面客户端扫描和连接设备。', true);
    $('scan-button').disabled = true;
  } else {
    void scanDevices();
  }
})();
