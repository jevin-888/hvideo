(() => {
  'use strict';
  const $ = id => document.getElementById(`app-update-${id}`);
  const invoke = window.__TAURI__?.tauri?.invoke;
  let candidates = [];
  let networkCandidates = [];
  let mode = 'network';
  let choices = new Set();
  let snapshot = { apk: null, task: null, busy: false };
  let ready = false;
  let pending = false;
  let timer = null;
  let generation = 0;
  const running = () => snapshot.task?.status === 'running';
  const locked = () => pending || snapshot.busy || running();

  function message(text, error = false) {
    $('status').textContent = text;
    $('status').classList.toggle('error', error);
  }

  function controls() {
    const count = choices.size;
    const selectable = candidates.filter(device => !device.unavailable);
    $('count').textContent = `已选 ${count} 台 / 最多 50 台`;
    $('all').checked = selectable.length > 0 && count === selectable.length;
    $('all').indeterminate = count > 0 && count < selectable.length;
    $('fields').disabled = !ready || locked();
    $('usb-refresh').hidden = mode !== 'usb';
    $('all').disabled = selectable.length === 0;
    $('start').disabled = !ready || locked() || !snapshot.apk || count === 0 || count > 50;
    $('start').textContent = pending ? '正在处理…' : running() ? '安装中…' : '开始安装';
    $('close').disabled = pending;
    $('refresh').disabled = pending;
    const apk = snapshot.apk;
    $('file-info').textContent = apk
      ? `${apk.fileName} · ${(apk.size / 1024 / 1024).toFixed(1)} MB`
      : '尚未选择安装包（最大 300 MB）';
  }

  function renderDevices() {
    $('devices').replaceChildren();
    if (!candidates.length) {
      $('devices').textContent = mode === 'usb' ? '未发现 USB 设备，请检查数据线、USB 调试和 ADB 驱动，然后刷新。' : '暂无设备，请先扫描局域网或手动连接设备。';
    }
    for (const device of candidates) {
      const label = document.createElement('label');
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox';
      checkbox.disabled = Boolean(device.unavailable);
      checkbox.checked = choices.has(device.ip);
      checkbox.value = device.ip;
      checkbox.addEventListener('change', () => {
        if (checkbox.checked) choices.add(device.ip); else choices.delete(device.ip);
        controls();
      });
      const caption = document.createElement('span');
      caption.textContent = mode === 'usb'
        ? `${device.name} · ${device.ip} · ${device.stateLabel}`
        : `${device.name} · ${device.ip} · ${device.version || '版本未知'}${device.manual ? '（手动地址）' : ''}`;
      label.append(checkbox, caption);
      $('devices').append(label);
    }
    controls();
  }

  function renderTask() {
    $('results').replaceChildren();
    const task = snapshot.task;
    $('progress').hidden = !task;
    if (task) {
      const labels = { queued: '等待', preparing: '准备 ADB', connecting: '连接中', installing: '安装中', launching: '正在启动', succeeded: '成功', launch_failed: '已安装，未启动', failed: '失败' };
      let succeeded = 0;
      let failed = 0;
      let launchFailed = 0;
      for (const device of task.devices) {
        const row = document.createElement('div');
        row.className = `update-result ${device.status === 'failed' ? 'failed' : device.status === 'succeeded' ? 'succeeded' : device.status === 'launch_failed' ? 'warning' : ''}`;
        const title = document.createElement('strong');
        title.textContent = `${device.name} (${device.usbSerial ? `USB · ${device.usbSerial}` : device.terminalIp}) · ${labels[device.status] || device.status}`;
        const detail = document.createElement('p');
        detail.textContent = device.message;
        row.append(title, detail);
        $('results').append(row);
        if (device.status === 'succeeded') succeeded += 1;
        if (device.status === 'failed') failed += 1;
        if (device.status === 'launch_failed') launchFailed += 1;
      }
      $('progress').max = task.devices.length;
      $('progress').value = succeeded + failed + launchFailed;
      if (running()) $('downgrade').checked = task.allowDowngrade;
      message(`${task.fileName} · ${task.signingProfile ? '签名 ' + task.signingProfile : '保持原签名'} · ${task.allowDowngrade ? '允许降级' : '普通更新'} · ${running() ? '安装中，可关闭此窗口，请保持调试工具运行' : '任务已结束'} · 成功 ${succeeded} / 失败 ${failed}${launchFailed ? ` / 已安装未启动 ${launchFailed}` : ''} / 共 ${task.devices.length} 台`);
    } else {
      message('请选择 APK 和目标设备，最多 50 台，按顺序安装。');
    }
    controls();
  }

  function schedule(version) {
    clearTimeout(timer);
    if ($('dialog').open && (running() || snapshot.busy)) {
      timer = setTimeout(() => void refresh(version), 1500);
    }
  }

  async function refresh(version = ++generation) {
    if (!invoke || pending) return;
    clearTimeout(timer);
    try {
      const result = await invoke('get_app_update_state');
      if (version !== generation || !$('dialog').open) return;
      snapshot = result;
      ready = true;
      renderTask();
      schedule(version);
    } catch (error) {
      if (version !== generation) return;
      ready = false;
      controls();
      message(`无法获取安装状态：${error.message || error}。任务可能仍在执行，请重新获取状态。`, true);
    }
  }

  async function refreshUsb() {
    if (!ready || locked() || mode !== 'usb') return;
    pending = true;
    const version = ++generation;
    clearTimeout(timer);
    controls();
    message('正在读取 USB 设备…');
    try {
      const devices = await invoke('list_usb_update_devices');
      if (version !== generation) return;
      const labels = { device: '已连接', offline: '离线，请重新插拔', unauthorized: '未授权，请在设备上允许 USB 调试', recovery: 'Recovery 模式', sideload: 'Sideload 模式' };
      candidates = devices.map(device => ({ ip: device.serial, name: device.name, unavailable: device.state !== 'device', stateLabel: labels[device.state] || device.state }));
      const available = new Set(candidates.filter(device => !device.unavailable).map(device => device.ip));
      choices = new Set([...choices].filter(serial => available.has(serial)));
      message(`发现 ${candidates.length} 台 USB 设备，${available.size} 台可安装，请勾选目标设备。`);
    } catch (error) {
      candidates = []; choices.clear();
      message(`读取 USB 设备失败：${error.message || error}`, true);
    } finally {
      pending = false; renderDevices();
    }
  }

  window.HvideoAppUpdate = {
    open(devices, selected) {
      if (!invoke) return;
      const unique = new Map();
      for (const device of [...devices, ...(selected ? [selected] : [])]) {
        unique.set(device.ip, { ip: device.ip, name: device.device_name || 'HVIDEO 播放设备', version: device.version, manual: device.manual });
      }
      networkCandidates = [...unique.values()];
      candidates = mode === 'network' ? networkCandidates : [];
      choices = new Set([...choices].filter(ip => unique.has(ip)));
      if (mode === 'network' && !choices.size && selected) choices.add(selected.ip);
      ready = false;
      renderDevices();
      if (!$('dialog').open) $('dialog').showModal();
      message('正在读取安装任务…');
      void refresh().then(() => { if ($('dialog').open && mode === 'usb') void refreshUsb(); });
    },
  };

  $('connection').addEventListener('change', () => {
    mode = $('connection').value; choices.clear();
    candidates = mode === 'network' ? networkCandidates : [];
    renderDevices();
    if (mode === 'usb') void refreshUsb();
  });
  $('usb-refresh').addEventListener('click', () => void refreshUsb());
  $('all').addEventListener('change', () => {
    choices = $('all').checked ? new Set(candidates.filter(device => !device.unavailable).map(device => device.ip)) : new Set();
    renderDevices();
  });
  $('choose').addEventListener('click', async () => {
    if (!ready || locked()) return;
    pending = true;
    controls();
    message('请选择 APK，正在准备安装包…');
    try {
      snapshot = await invoke('choose_update_apk');
      message(snapshot.apk ? '安装包已准备，请核对目标设备后点击「开始安装」。' : '未选择安装包。');
    } catch (error) {
      message(`安装包准备失败：${error.message || error}`, true);
    } finally {
      pending = false;
      controls();
    }
  });
  $('start').addEventListener('click', async () => {
    if ($('start').disabled) return;
    pending = true;
    controls();
    clearTimeout(timer);
    message('正在检查 ADB 并创建安装任务…');
    try {
      snapshot.task = await invoke('start_app_update', {
        targets: candidates.filter(device => choices.has(device.ip)).map(device => mode === 'usb' ? { name: device.name, usbSerial: device.ip } : { name: device.name, terminalIp: device.ip }),
        allowDowngrade: $('downgrade').checked,
        signingProfile: $('signing').value,
      });
      snapshot.apk = null;
      snapshot.busy = true;
      renderTask();
      schedule(generation);
    } catch (error) {
      ready = false;
      message(`${error.message || error}。请点击「重新获取状态」确认是否已启动任务。`, true);
    } finally {
      pending = false;
      controls();
    }
  });
  $('refresh').addEventListener('click', () => {
    ready = false;
    controls();
    void refresh();
  });
  $('close').addEventListener('click', () => { if (!pending) $('dialog').close(); });
  $('dialog').addEventListener('cancel', event => { if (pending) event.preventDefault(); });
  $('dialog').addEventListener('close', () => {
    clearTimeout(timer);
    generation += 1;
    document.getElementById('app-update-button').focus();
  });
  window.addEventListener('app-update-close-blocked', () => {
    if (!$('dialog').open) $('dialog').showModal();
    message('APP 安装包准备或安装尚未结束，请等待任务完成后退出调试工具。', true);
    void refresh();
  });
})();
