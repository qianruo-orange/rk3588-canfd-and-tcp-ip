/* config.js — 配置页面逻辑 */
(function() {

  var curCan = 0;      /* 当前选中的 CAN 通道索引 */
  var canNames = [];   /* 通道名列表（由 /api/config 动态读取） */
  var cansCfg = {};    /* 各通道配置缓存 { can0:{...}, ... } */
  var canFD = {};      /* 系统接口 CAN FD 支持映射 { can0: 1, ... } */

  function curName() { return canNames[curCan] || ('can' + curCan); }

  function chkFd() {
    document.getElementById('dbr').style.display = document.getElementById('fd').value === 'on' ? 'flex' : 'none';
  }

  function show(t, c) {
    var m = document.getElementById('msg');
    m.className = t;
    m.textContent = c;
  }

  function setSwitch(up) {
    document.getElementById('st').textContent = up ? '\ud83d\udfe2' : '\ud83d\udd34';
    document.getElementById('tg').checked = up;
    document.getElementById('cfg').style.display = up ? 'block' : 'none';
    document.getElementById('hint').style.display = up ? 'none' : 'block';
  }

  function setFilters(filters) {
    var el = document.getElementById('fil');
    el.innerHTML = '';
    if (!filters) return;
    filters.forEach(function(f) {
      var r = document.createElement('div');
      r.className = 'filter-row';
      r.innerHTML = '<input type="text" value="' + f.id + '" placeholder="ID (hex)">'
        + '<input type="text" value="' + f.mask + '" placeholder="Mask (hex)">'
        + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
      el.appendChild(r);
    });
  }

  /* 把当前表单内容写入 cansCfg[curName()] */
  function collectCan() {
    var f = cansCfg[curName()] || (cansCfg[curName()] = {});
    f.bitrate = document.getElementById('br').value;
    f.fd = document.getElementById('fd').value;
    f.dbitrate = document.getElementById('db').value;
    f.up = document.getElementById('tg').checked ? 'on' : 'off';
    f.filters = [];
    var rows = document.getElementById('fil').children;
    for (var k = 0; k < rows.length; k++) {
      var inputs = rows[k].querySelectorAll('input');
      if (inputs.length >= 2) f.filters.push({id: inputs[0].value, mask: inputs[1].value});
    }
  }

  /* 用 cansCfg[curName()] 渲染当前通道 */
  function renderCan() {
    var f = cansCfg[curName()] || {};
    document.getElementById('br').value = f.bitrate || '';
    document.getElementById('fd').value = f.fd || 'off';
    document.getElementById('db').value = f.dbitrate || '';
    setSwitch(f.up === 'on');
    chkFd();
    setFilters(f.filters || []);
    document.getElementById('dbc').value = '';
  }

  window.onCanSel = function() {
    collectCan();
    curCan = parseInt(document.getElementById('canSel').value, 10) || 0;
    renderCan();
  };

  window.toggleCan = async function(on) {
    var n = curName();
    var tg = document.getElementById('tg');
    tg.disabled = true;
    try {
      await fetch('/api/can/toggle', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ifname=' + n + '&action=' + (on ? 'up' : 'down')
      });
    } catch(e) {}
    setSwitch(on);
    cansCfg[n] = cansCfg[n] || {};
    cansCfg[n].up = on ? 'on' : 'off';
    tg.disabled = false;
  };

  window.chkFd = chkFd;

  window.uploadDbc = async function() {
    var input = document.getElementById('dbc');
    var file = input.files && input.files[0];
    if (!file) { show('err', '请先选择 DBC 文件'); return; }
    var n = curName();
    try {
      var r = await fetch('/api/can/dbc?ifname=' + n, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream'},
        body: file
      });
      var j = r.ok ? await r.json() : null;
      if (j && j.result === 'ok') {
        show('ok', '✓ ' + n + ' DBC 已上传（' + j.messages + ' 报文 / ' + j.signals + ' 信号）');
      } else {
        show('err', '上传失败(' + r.status + ')');
      }
    } catch(e) { show('err', '上传失败'); }
    setTimeout(function() { document.getElementById('msg').className = ''; }, 3000);
  };

  var capsData = null;
  var camDevs = [];   /* 系统枚举到的摄像头列表 [{path,card},...] */

  /* 页面加载时扫描可用摄像头 */
  async function loadDevices() {
    var sel = document.getElementById('vd');
    try {
      var r = await fetch('/api/video/devices');
      if (!r.ok) return;
      camDevs = await r.json();
      sel.innerHTML = '';
      for (var i = 0; i < camDevs.length; i++)
        sel.innerHTML += '<option value="' + camDevs[i].path + '">' + camDevs[i].path + ' - ' + camDevs[i].card + '</option>';
      if (camDevs.length === 0)
        sel.innerHTML = '<option value="">无可用摄像头</option>';
    } catch(e) { sel.innerHTML = '<option value="">扫描失败</option>'; }
  }

  window.queryCaps = async function() {
    var dev = document.getElementById('vd').value;
    var sel = document.getElementById('vr');
    if (!dev) { sel.innerHTML = '<option value="">-- 请选择设备 --</option>'; return; }
    sel.innerHTML = '<option value="">查询中...</option>';
    try {
      var r = await fetch('/api/video/caps?device=' + encodeURIComponent(dev));
      if (!r.ok) { sel.innerHTML = '<option value="">设备不可用</option>'; return; }
      capsData = await r.json();
      sel.innerHTML = '';

      var curW = parseInt(document.getElementById('vw').value) || 0;
      var curH = parseInt(document.getElementById('vh').value) || 0;
      var matchVal = '';

      for (var i = 0; i < capsData.formats.length; i++) {
        var f = capsData.formats[i];
        for (var j = 0; j < f.sizes.length; j++) {
          var s = f.sizes[j];
          var val = i + '_' + j;
          var txt = f.fmt + '  ' + s.w + '×' + s.h + (s.label ? ' (' + s.label + ')' : '');
          sel.innerHTML += '<option value="' + val + '">' + txt + '</option>';
          if (s.w === curW && s.h === curH && !matchVal) matchVal = val;
        }
      }

      if (matchVal) sel.value = matchVal;
      else if (sel.options.length > 0) sel.selectedIndex = 0;
      onResChange();
    } catch(e) { sel.innerHTML = '<option value="">查询失败</option>'; }
  };

  window.onResChange = function() {
    var sel = document.getElementById('vr');
    var val = sel.value;
    if (!val || !capsData) return;
    var parts = val.split('_');
    var fi = parseInt(parts[0]), ri = parseInt(parts[1]);
    var s = capsData.formats[fi].sizes[ri];
    document.getElementById('vw').value = s.w;
    document.getElementById('vh').value = s.h;
  };

  window.addFilter = function() {
    var el = document.getElementById('fil');
    if (el.children.length >= 16) return;
    var r = document.createElement('div');
    r.className = 'filter-row';
    r.innerHTML = '<input type="text" placeholder="ID (hex)">'
      + '<input type="text" placeholder="Mask (hex)">'
      + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
    el.appendChild(r);
  };

  /* 加载网络接口列表，填充"绑定网卡"下拉框 */
  async function loadIfaces() {
    var sel = document.getElementById('bind');
    sel.innerHTML = '<option value="">所有网卡</option>';
    try {
      var r = await fetch('/api/network/ifaces');
      if (!r.ok) return;
      var list = await r.json();
      for (var i = 0; i < list.length; i++)
        sel.innerHTML += '<option value="' + list[i] + '">' + list[i] + '</option>';
    } catch(e) {}
  }

  /* 枚举系统全部 CAN 接口及其 CAN FD 支持情况 */
  async function loadCanIfaces() {
    try {
      var r = await fetch('/api/can/ifaces');
      if (!r.ok) return;
      var list = await r.json();
      canFD = {};
      for (var i = 0; i < list.length; i++) canFD[list[i].name] = list[i].fd;
    } catch(e) {}
  }

  async function loadCfg() {
    try {
      var cfg = await (await fetch('/api/config')).json();
      document.getElementById('tp').value = cfg.tcp_port || '';
      document.getElementById('mc').value = cfg.max_clients || '';
      document.getElementById('bind').value = cfg.tcp_bind || '';

      /* 视频设备：配置值必须在系统枚举列表中才使用，否则选择第一个真实设备；
         没有摄像头时保持为空 */
      var vd = document.getElementById('vd');
      var cfgDev = cfg.video_device || '';
      var devInList = false;
      for (var di = 0; di < camDevs.length; di++)
        if (camDevs[di].path === cfgDev) { devInList = true; break; }
      vd.value = devInList ? cfgDev : (camDevs.length ? camDevs[0].path : '');
      if (vd.value) setTimeout(queryCaps, 500);

      /* 分辨率由设备实际能力（/api/video/caps）决定：
         配置中有有效值则匹配选中，否则默认取第一个支持的分辨率 */
      document.getElementById('vw').value = cfg.video_width > 0 ? cfg.video_width : '';
      document.getElementById('vh').value = cfg.video_height > 0 ? cfg.video_height : '';

      canNames = Object.keys(cfg.cans || {});

      /* 与系统实际枚举的接口合并：枚举接口优先展示，配置中已存在但未枚举到的保留。
         系统无 CAN 接口且配置中也没有通道时，canNames 保持为空，下方显示"无可用"提示。 */
      var sysNames = Object.keys(canFD);
      if (sysNames.length > 0) {
        var merged = sysNames.slice();
        for (var ci = 0; ci < canNames.length; ci++)
          if (merged.indexOf(canNames[ci]) < 0) merged.push(canNames[ci]);
        canNames = merged;
      }

      /* 动态生成通道下拉框（标注 CAN FD 支持） */
      var sel = document.getElementById('canSel');
      sel.innerHTML = '';
      if (canNames.length === 0) {
        sel.innerHTML = '<option value="0">无可用 CAN 接口</option>';
        document.getElementById('cfg').style.display = 'none';
        document.getElementById('hint').style.display = 'block';
        document.getElementById('hint').textContent = '系统未检测到 CAN 设备';
      } else {
        for (var i = 0; i < canNames.length; i++) {
          var nm = canNames[i];
          var tag = '';
          if (canFD[nm] === 1) tag = ' (CAN-FD)';
          else if (canFD[nm] === 0) tag = ' (无 CAN-FD)';
          sel.innerHTML += '<option value="' + i + '">' + nm + tag + '</option>';
        }
        document.getElementById('hint').textContent = '接口已关闭，开启后可配置';
      }
      if (curCan >= canNames.length) curCan = 0;
      sel.value = String(curCan);

      cansCfg = {};
      for (var i = 0; i < canNames.length; i++) {
        var f = (cfg.cans || {})[canNames[i]] || {};
        cansCfg[canNames[i]] = {
          bitrate: f.bitrate || '',
          fd: f.fd || 'off',
          dbitrate: f.dbitrate || '',
          up: f.up === 'on' ? 'on' : 'off',
          filters: f.filters || []
        };
      }
      renderCan();
    } catch(e) {}
  }
  window.loadCfg = loadCfg;

  /* 提交一个模块的配置（target 指定模块，后端只应用该模块并重启对应模块） */
  async function postConfig(d) {
    try {
      var r = await fetch('/api/config', {method: 'POST', body: new URLSearchParams(d)});
      show(r.ok ? 'ok' : 'err', r.ok ? '✓ 已保存，模块已重启生效' : '失败(' + r.status + ')');
      if (r.ok) setTimeout(function() { document.getElementById('msg').className = ''; }, 3000);
    } catch(e) { show('err', '请求失败'); }
  }

  /* 保存当前 CAN 通道配置并重新配置该通道 */
  window.saveCan = async function() {
    collectCan();
    var name = canNames[curCan];
    if (!name) { show('err', '无可用 CAN 接口'); return; }
    var f = cansCfg[name] || {};
    var d = {target: 'can'};
    d['ifname' + curCan] = name;
    d['bitrate' + curCan] = f.bitrate;
    d['fd' + curCan] = f.fd;
    d['dbitrate' + curCan] = f.dbitrate;
    d['up' + curCan] = f.up;
    var filters = f.filters || [];
    for (var k = 0; k < filters.length; k++) {
      d['filter_id_' + curCan + '_' + k] = filters[k].id;
      d['filter_mask_' + curCan + '_' + k] = filters[k].mask;
    }
    await postConfig(d);
  };

  /* 保存网络配置并重启 TCP 监听 */
  window.saveNet = async function() {
    await postConfig({
      target: 'net',
      tcp_port: document.getElementById('tp').value,
      max_clients: document.getElementById('mc').value,
      tcp_bind: document.getElementById('bind').value
    });
  };

  /* 保存视频配置并重启视频流 */
  window.saveVideo = async function() {
    await postConfig({
      target: 'video',
      video_device: document.getElementById('vd').value,
      video_width: document.getElementById('vw').value,
      video_height: document.getElementById('vh').value
    });
  };

  window.reboot = async function() {
    if (!confirm('确定要重启设备吗？')) return;
    try { await fetch('/api/reboot'); } catch(e) {}
  };

  window.shutdown = async function() {
    if (!confirm('确定要关闭设备吗？')) return;
    try { await fetch('/api/shutdown'); } catch(e) {}
  };

  /* 先扫描设备列表、CAN 接口与网卡列表，再加载配置 */
  loadDevices().then(loadCanIfaces).then(loadIfaces).then(loadCfg);
})();
