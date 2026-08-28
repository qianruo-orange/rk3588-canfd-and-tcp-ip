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
      await authFetch('/api/can/toggle', {
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
      var r = await authFetch('/api/can/dbc?ifname=' + n, {
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
  var cfgVideoFps = 0;   /* 配置中的帧率（0=自动），页面加载时用于回选合并下拉框 */
  var selVidFps = 0;     /* 当前选中项（格式/分辨率/帧率合一）的帧率，保存时提交 */

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

      /* 格式 / 分辨率 / 帧率合并为一个下拉框：每项 = fmt + 尺寸 + @fps */
      var curW = parseInt(document.getElementById('vw').value) || 0;
      var curH = parseInt(document.getElementById('vh').value) || 0;
      var matchVal = '';   /* 尺寸 + 帧率都匹配 */
      var matchSize = '';  /* 仅尺寸匹配（配置帧率为 0 时用） */

      for (var i = 0; i < capsData.formats.length; i++) {
        var f = capsData.formats[i];
        for (var j = 0; j < f.sizes.length; j++) {
          var s = f.sizes[j];
          var fps = s.fps && s.fps.length ? s.fps : [60,50,30,25,20,15,10,5];
          for (var k = 0; k < fps.length; k++) {
            var val = i + '_' + j + '_' + fps[k];
            var txt = f.fmt + '  ' + s.w + '×' + s.h + (s.label ? ' (' + s.label + ')' : '') + ' @ ' + fps[k] + ' FPS';
            sel.innerHTML += '<option value="' + val + '">' + txt + '</option>';
            if (s.w === curW && s.h === curH) {
              if (!matchSize) matchSize = val;
              if (cfgVideoFps > 0 && fps[k] === cfgVideoFps) matchVal = val;
            }
          }
        }
      }

      if (matchVal) sel.value = matchVal;
      else if (matchSize) sel.value = matchSize;
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
    selVidFps = parseInt(parts[2], 10) || 0;   /* 帧率随选项一起保存 */
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
      cfgVideoFps = cfg.video_fps || 0;

      /* AI 检测配置（必要流程；线程数下拉框为 3 的倍数；置信度/NMS 显示为百分比） */
      var el = document.getElementById('ai_th');
      if (el) el.value = String(cfg.ai_threads || 3);
      document.getElementById('ai_cf').value = Math.round((cfg.ai_conf || 0.25) * 100);
      document.getElementById('ai_nm').value = Math.round((cfg.ai_nms || 0.45) * 100);
      document.getElementById('ai_iv').value = cfg.ai_interval_ms || 10;
      /* IP 设置：IP 即 TCP 绑定网卡的 IP；回填保存配置 + 当前运行时地址 */
      document.getElementById('ip_mode').value = cfg.ip_mode || '';
      document.getElementById('ip_a').value = cfg.ip_addr || '';
      document.getElementById('ip_m').value = cfg.ip_mask || '';
      document.getElementById('ip_g').value = cfg.ip_gw || '';
      var cur = '当前 ' + (cfg.ip_cur_if || '') + '  ';
      cur += cfg.ip_cur_addr ? ('IP ' + cfg.ip_cur_addr) : '无 IP';
      if (cfg.ip_cur_mask) cur += ' / ' + cfg.ip_cur_mask;
      if (cfg.ip_cur_gw) cur += ' 网关 ' + cfg.ip_cur_gw;
      document.getElementById('ip_cur').textContent = cur;
      chkIpMode();

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
      var r = await authFetch('/api/config', {method: 'POST', body: new URLSearchParams(d)});
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

  /* 保存网络设置：TCP 数据通道参数 + 该网卡的 IP 配置（IP 即 TCP 绑定网卡的 IP） */
  window.saveNet = async function() {
    var mode = document.getElementById('ip_mode').value;
    var ifn = document.getElementById('bind').value;
    if (mode !== '') {
      if (!ifn) { show('err', '配置 IP 需要先选择绑定网卡'); return; }
      if (mode === 'static') {
        var a = document.getElementById('ip_a').value.trim();
        var m = document.getElementById('ip_m').value.trim();
        if (!a || !m) { show('err', '静态模式需要填写 IP 地址与子网掩码'); return; }
        if (!confirm('将把 ' + ifn + ' 设为静态 IP ' + a + '/' + m
          + (document.getElementById('ip_g').value ? '，网关 ' + document.getElementById('ip_g').value : '')
          + '。\n若当前页面通过该网卡访问，连接会断开，请用新 IP 重新访问。')) return;
      } else if (mode === 'dhcp') {
        if (!confirm('将把 ' + ifn + ' 切换到 DHCP 自动获取。\nIP 变化后请用新地址重新访问。')) return;
      }
    }
    /* 1) TCP 服务参数 */
    await postConfig({
      target: 'net',
      tcp_port: document.getElementById('tp').value,
      max_clients: document.getElementById('mc').value,
      tcp_bind: ifn
    });
    /* 2) IP 设置（mode 为空 = 关闭不管理，仍保存字段） */
    await postConfig({
      target: 'ip',
      ip_ifname: ifn,
      ip_mode: mode,
      ip_addr: document.getElementById('ip_a').value.trim(),
      ip_mask: document.getElementById('ip_m').value.trim(),
      ip_gw: document.getElementById('ip_g').value.trim()
    });
  };

  /* 按 IP 模式显隐静态地址输入行 */
  window.chkIpMode = function() {
    var mode = document.getElementById('ip_mode').value;
    var show = mode === 'static';
    document.getElementById('ip_a_row').style.display = show ? 'flex' : 'none';
    document.getElementById('ip_m_row').style.display = show ? 'flex' : 'none';
    document.getElementById('ip_g_row').style.display = show ? 'flex' : 'none';
  };

  /* 保存视频配置并重启视频流 */
  window.saveVideo = async function() {
    await postConfig({
      target: 'video',
      video_device: document.getElementById('vd').value,
      video_width: document.getElementById('vw').value,
      video_height: document.getElementById('vh').value,
      video_fps: selVidFps
    });
  };

  /* 保存 AI 配置并热重载推理池（线程数取 3 的倍数） */
  window.saveAi = async function() {
    var th = parseInt(document.getElementById('ai_th').value, 10);
    if (!th || th % 3 !== 0) th = 3;
    await postConfig({
      target: 'ai',
      ai_threads: th,
      ai_conf: document.getElementById('ai_cf').value,
      ai_nms: document.getElementById('ai_nm').value,
      ai_interval_ms: document.getElementById('ai_iv').value
    });
    setTimeout(loadCfg, 1200);   /* 重载完成后刷新显示 */
  };

  /* 上传 AI 文件（模型 .rknn / 类别标签 .names）并热重载 */
  window.uploadAi = async function(type) {
    var input = document.getElementById(type === 'model' ? 'ai_model_f' : 'ai_names_f');
    var file = input.files && input.files[0];
    if (!file) { show('err', '请先选择' + (type === 'model' ? '模型' : '类别标签') + '文件'); return; }
    try {
      var r = await authFetch('/api/ai/upload?type=' + type, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream'},
        body: file
      });
      var j = r.ok ? await r.json() : null;
      if (j && j.result === 'ok') {
        var extra = type === 'names' ? ('（' + j.classes + ' 个类别）') : '';
        show('ok', '✓ ' + j.path + ' 已上传并热生效' + extra);
        setTimeout(loadCfg, 800);
      } else if (j) {
        show('err', '上传失败：' + (j.result || ('HTTP ' + r.status)));
      } else {
        show('err', '上传失败(' + r.status + ')');
      }
    } catch(e) { show('err', '上传失败'); }
    input.value = '';
    setTimeout(function() { document.getElementById('msg').className = ''; }, 4000);
  };

  /* 导出配置：只导出 config 文件夹——后端打包整个 config 目录
     （config.txt / 模型 / DBC / 标签），authFetch 带登录重试，
     成功转 blob 触发浏览器下载 */
  window.exportCfg = async function() {
    try {
      var pr = await authFetch('/api/config/export');
      if (!pr.ok) throw new Error('HTTP ' + pr.status);
      var pb = await pr.blob();
      var pa = document.createElement('a');
      pa.href = URL.createObjectURL(pb);
      pa.download = 'config_pack.tar.gz';
      pa.click();
      setTimeout(function() { URL.revokeObjectURL(pa.href); }, 1000);
      show('ok', '✓ 配置已导出（config 文件夹）');
    } catch(e) {
      show('err', '配置导出失败（' + e.message + '）');
    }
  };

  window.reboot = async function() {
    if (!confirm('确定要重启设备吗？')) return;
    try { await authFetch('/api/reboot'); } catch(e) {}
  };

  window.shutdown = async function() {
    if (!confirm('确定要关闭设备吗？')) return;
    try { await authFetch('/api/shutdown'); } catch(e) {}
  };

  /* 先扫描设备列表、CAN 接口与网卡列表，再加载配置 */
  loadDevices().then(loadCanIfaces).then(loadIfaces).then(loadCfg);
})();
