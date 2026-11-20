(() => {
  'use strict';
  const $ = id => document.getElementById(`system-update-${id}`);
  const invoke = window.__TAURI__?.tauri?.invoke;
  let candidates = [];
  let choices = new Set();
  let snapshot = { rom: null, romReady: false, task: null, busy: false };
  let ready = false;
  let pending = false;
  let timer = null;
  let generation = 0;

  const DEFAULT_HARDWARE = 'hw82';
  $('hardware').value = DEFAULT_HARDWARE;

  const running = () => snapshot.busy || snapshot.task?.status === 'running';
  const locked = () => pending || running();

  function message(text, error = false) {
    $('status').textContent = text;
    $('status').classList.toggle('error', error);
  }

  function controls() {
    const count = choices.size;
    $('count').textContent = `已选 ${count} 台 / 最多 50 台`;
    $('all').checked = candidates.length > 0 && count === candidates.length;
    $('all').indeterminate = count > 0 && count < candidates.length;
    $('fields').disabled = !ready || locked();
    $('start').disabled = !ready || locked() || !snapshot.romReady || count === 0 || count > 50;
    $('start').textContent = pending ? '正在处理…' : running() ? '升级中…' : '检查并启动';
    $('close').disabled = pending;
    $('refresh').disabled = pending;
    const rom = snapshot.rom;
    $('file-info').replaceChildren();
    if (rom) {
      const path = document.createElement('span');
      path.textContent = rom.sourcePath;
      const meta = document.createElement('small');
      meta.textContent = `v${rom.versionCode} · ${rom.hardware} · ${(rom.imageSize / 1024 / 1024).toFixed(1)} MB · ${rom.releaseId.slice(0, 12)}`;
      $('file-info').append(path, meta);
    } else {
      $('file-info').textContent = '尚未选择 ROM（请选择 update.img，自动匹配配套清单和签名）';
    }
    $('choose').disabled = !ready || locked();
    $('hardware').disabled = !ready || locked() || !snapshot.romReady;
  }

  function renderDevices() {
    $('devices').replaceChildren();
    if (!candidates.length) $('devices').textContent = '暂无设备，请先扫描局域网或手动连接设备。';
    for (const device of candidates) {
      const label = document.createElement('label');
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox'; checkbox.checked = choices.has(device.ip); checkbox.value = device.ip;
      checkbox.addEventListener('change', () => { if (checkbox.checked) choices.add(device.ip); else choices.delete(device.ip); controls(); });
      const caption = document.createElement('span');
      caption.textContent = `${device.name} · ${device.ip}${device.manual ? '（手动地址）' : ''}`;
      label.append(checkbox, caption); $('devices').append(label);
    }
    controls();
  }

  function renderTask() {
    $('results').replaceChildren();
    const task = snapshot.task;
    if (running() && task) { $('downgrade').checked = task.allowDowngrade; $('hardware').value = task.hardwareOverride || DEFAULT_HARDWARE; if (task.rom) snapshot.rom = task.rom; }
    const labels = { queued: '等待', connecting: '连接中', staging: '推送 ROM', checking: '校验中', arming: '启动 Recovery', rebooting: '等待设备恢复', verifying: '确认升级结果', succeeded: '升级成功', failed: '失败', unconfirmed: '结果待确认' };
    if (!task) { message(snapshot.rom ? '请选择目标设备，最多 50 台。' : '请先选择 ROM 包和目标设备，最多 50 台。'); controls(); return; }
    for (const device of task.devices) {
      const row = document.createElement('div');
      row.className = `update-result ${device.status === 'failed' ? 'failed' : device.status === 'succeeded' ? 'succeeded' : device.status === 'unconfirmed' ? 'warning' : ''}`;
      const title = document.createElement('strong');
      title.textContent = `${device.name} (${device.terminalIp}) · ${labels[device.status] || device.status}`;
      const detail = document.createElement('p');
      detail.textContent = device.status === 'failed' ? `失败原因：${device.message || '设备未返回错误详情，请检查设备连接和升级日志。'}` : device.message;
      row.append(title, detail); $('results').append(row);
    }
    const succeeded = task.devices.filter(device => device.status === 'succeeded').length;
    const failed = task.devices.filter(device => device.status === 'failed').length;
    const unconfirmed = task.devices.filter(device => device.status === 'unconfirmed').length;
    const waiting = task.devices.length - succeeded - failed - unconfirmed;
    const nextStep = !running() && !snapshot.romReady ? '。再次升级请重新选择 update.img。' : '';
    message(`${running() ? '正在升级并监控结果' : '任务已结束'} · 成功 ${succeeded} / 失败 ${failed} / 待确认 ${unconfirmed}${waiting ? ` / 进行中 ${waiting}` : ''} / 共 ${task.devices.length} 台${nextStep}`, !running() && failed > 0);
    controls();
  }

  function schedule(version) {
    clearTimeout(timer);
    if ($('dialog').open && (!ready || running())) timer = setTimeout(() => void refresh(version), 1000);
  }

  async function refresh(version = ++generation) {
    if (!invoke || pending) return;
    clearTimeout(timer);
    try {
      const result = await invoke('get_system_update_state');
      if (version !== generation || !$('dialog').open) return;
      snapshot = result; ready = true; renderTask(); schedule(version);
    } catch (error) {
      if (version !== generation || !$('dialog').open) return;
      ready = false; controls(); message(`暂时无法获取系统升级状态，正在自动重试：${error.message || error}`, true);
      schedule(version);
    }
  }

  window.HvideoSystemUpdate = {
    open(devices, selected) {
      if (!invoke) return;
      const unique = new Map();
      for (const device of [...devices, ...(selected ? [selected] : [])]) {
        unique.set(device.ip, { ip: device.ip, name: device.device_name || 'HVIDEO 播放设备', manual: device.manual });
      }
      candidates = [...unique.values()];
      choices = new Set([...choices].filter(ip => unique.has(ip)));
      if (!choices.size && selected) choices.add(selected.ip);
      ready = false; renderDevices();
      if (!$('dialog').open) $('dialog').showModal();
      message('正在读取系统升级任务…'); void refresh();
    },
  };

  $('all').addEventListener('change', () => { choices = $('all').checked ? new Set(candidates.map(device => device.ip)) : new Set(); renderDevices(); });
  $('choose').addEventListener('click', async () => {
    if ($('choose').disabled) return;
    pending = true; controls(); message('请选择 update.img，工具将匹配并校验配套清单和签名…');
    try {
      snapshot = await invoke('choose_system_rom');
      message(snapshot.romReady ? 'ROM 已准备，请核对目标设备后点击「检查并启动」。' : snapshot.rom ? '已保留上次 ROM 信息；再次升级请重新选择 update.img。' : '未选择 ROM。');
    } catch (error) {
      message(`ROM 准备失败：${error.message || error}`, true);
    } finally { pending = false; controls(); }
  });
  $('start').addEventListener('click', async () => {
    if ($('start').disabled) return;
    const version = ++generation;
    pending = true; controls(); clearTimeout(timer); message($('hardware').value ? '正在校验 ROM 并选择硬件标识，再检查设备…' : '正在检查 ADB 并启动播放器系统升级流程…');
    try {
      snapshot.task = await invoke('start_system_update', { targets: candidates.filter(device => choices.has(device.ip)).map(device => ({ name: device.name, terminalIp: device.ip })), allowDowngrade: $('downgrade').checked, hardwareOverride: $('hardware').value || null });
      snapshot.romReady = false; snapshot.busy = true; renderTask();
    } catch (error) {
      ready = false; message(`${error.message || error}。请点击「重新获取状态」确认是否已启动任务。`, true);
    } finally { pending = false; controls(); schedule(version); }
  });
  $('refresh').addEventListener('click', () => { ready = false; controls(); void refresh(); });
  $('hardware').addEventListener('change', () => { message(`硬件标识已选择 ${$('hardware').value}，启动时会自动选用对应标识文件。`); });
  $('close').addEventListener('click', () => { if (!pending) $('dialog').close(); });
  $('dialog').addEventListener('cancel', event => { if (pending) event.preventDefault(); });
  $('dialog').addEventListener('close', () => { clearTimeout(timer); generation += 1; document.getElementById('system-update-button').focus(); });
  window.addEventListener('system-update-close-blocked', () => { if (!$('dialog').open) $('dialog').showModal(); message('系统升级检查尚未结束，请等待任务完成后退出调试工具。', true); void refresh(); });
})();
