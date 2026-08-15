/* config.js — 配置页面逻辑 */
(function() {

  var curCan = 0;      /* 当前选中的 CAN 通道索引 */
  var canNames = [];   /* 通道名列表（由 /api/config 动态读取） */
  var cansCfg = {};    /* 各通道配置缓存 { can0:{...}, ... } */

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
      r.innerHTML = '<input type="text" value="' + f.id + '" style="width:80px">'
        + '<span style="color:#484f58;font-size:12px">:</span>'
        + '<input type="text" value="' + f.mask + '" style="width:80px">'
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

  /* 页面加载时扫描可用摄像头 */
  async function loadDevices() {
    var sel = document.getElementById('vd');
    try {
      var r = await fetch('/api/video/devices');
      if (!r.ok) return;
      var devs = await r.json();
      sel.innerHTML = '';
      for (var i = 0; i < devs.length; i++)
        sel.innerHTML += '<option value="' + devs[i].path + '">' + devs[i].path + ' - ' + devs[i].card + '</option>';
      if (devs.length === 0)
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
    r.innerHTML = '<input type="text" placeholder="ID (hex)" style="width:80px">'
      + '<span style="color:#484f58;font-size:12px">:</span>'
      + '<input type="text" placeholder="Mask (hex)" style="width:80px">'
      + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
    el.appendChild(r);
  };

  async function loadCfg() {
    try {
      var cfg = await (await fetch('/api/config')).json();
      document.getElementById('tp').value = cfg.tcp_port || '';
      document.getElementById('mc').value = cfg.max_clients || '16';
      document.getElementById('vd').value = cfg.video_device || '/dev/video0';
      /* 自动查询摄像头参数并同步下拉框 */
      if (cfg.video_device) setTimeout(queryCaps, 500);
      document.getElementById('vw').value = cfg.video_width || 640;
      document.getElementById('vh').value = cfg.video_height || 480;

      canNames = Object.keys(cfg.cans || {});
      if (canNames.length === 0) canNames = ['can0', 'can1'];

      /* 动态生成通道下拉框 */
      var sel = document.getElementById('canSel');
      sel.innerHTML = '';
      for (var i = 0; i < canNames.length; i++)
        sel.innerHTML += '<option value="' + i + '">' + canNames[i] + '</option>';
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

  document.getElementById('f').addEventListener('submit', async function(e) {
    e.preventDefault();
    collectCan();

    var d = {};
    for (var i = 0; i < canNames.length; i++) {
      var name = canNames[i];
      var f = cansCfg[name] || {bitrate: '', fd: 'off', dbitrate: '', up: 'off', filters: []};
      d['ifname' + i] = name;
      d['bitrate' + i] = f.bitrate;
      d['fd' + i] = f.fd;
      d['dbitrate' + i] = f.dbitrate;
      d['up' + i] = f.up;
      var filters = f.filters || [];
      for (var k = 0; k < filters.length; k++) {
        d['filter_id_' + i + '_' + k] = filters[k].id;
        d['filter_mask_' + i + '_' + k] = filters[k].mask;
      }
    }

    d['tcp_port'] = document.getElementById('tp').value;
    d['max_clients'] = document.getElementById('mc').value;
    d['video_device'] = document.getElementById('vd').value;
    d['video_width'] = document.getElementById('vw').value;
    d['video_height'] = document.getElementById('vh').value;

    try {
      var r = await fetch('/api/config', {method: 'POST', body: new URLSearchParams(d)});
      show(r.ok ? 'ok' : 'err', r.ok ? '✓ 已保存，立即生效' : '失败(' + r.status + ')');
      if (r.ok) setTimeout(function() { document.getElementById('msg').className = ''; }, 3000);
    } catch(e) { show('err', '请求失败'); }
  });

  window.reboot = async function() {
    if (!confirm('确定要重启设备吗？')) return;
    try { await fetch('/api/reboot'); } catch(e) {}
  };

  window.shutdown = async function() {
    if (!confirm('确定要关闭设备吗？')) return;
    try { await fetch('/api/shutdown'); } catch(e) {}
  };

  /* 先扫描设备列表，再加载配置 */
  loadDevices().then(function() { loadCfg(); });
})();
