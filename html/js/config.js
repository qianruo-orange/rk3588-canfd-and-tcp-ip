/* config.js — 配置页面逻辑 */
(function() {

  function chkFd(i) {
    document.getElementById('dbr' + i).style.display = document.getElementById('fd' + i).value === 'on' ? 'flex' : 'none';
  }

  function show(t, c) {
    var m = document.getElementById('msg');
    m.className = t;
    m.textContent = c;
  }

  function setSwitch(i, up) {
    document.getElementById('st' + i).textContent = up ? '\ud83d\udfe2' : '\ud83d\udd34';
    document.getElementById('tg' + i).checked = up;
    document.getElementById('cfg' + i).style.display = up ? 'block' : 'none';
    document.getElementById('hint' + i).style.display = up ? 'none' : 'block';
  }

  window.toggleCan = async function(i, on) {
    var n = (i === 0) ? 'can0' : 'can1';
    var tg = document.getElementById('tg' + i);
    tg.disabled = true;
    try {
      await fetch('/api/can/toggle', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ifname=' + n + '&action=' + (on ? 'up' : 'down')
      });
    } catch(e) {}
    setSwitch(i, on);
    tg.disabled = false;
  };

  window.chkFd = chkFd;

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
  window.addFilter = function(canIdx) {
    var cnt = document.getElementById('fil' + canIdx).children.length;
    if (cnt >= 16) return;
    var r = document.createElement('div');
    r.className = 'filter-row';
    r.innerHTML = '<input type="text" placeholder="ID (hex)" style="width:80px">'
      + '<span style="color:#484f58;font-size:12px">:</span>'
      + '<input type="text" placeholder="Mask (hex)" style="width:80px">'
      + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
    document.getElementById('fil' + canIdx).appendChild(r);
  };

  function setFilters(canIdx, filters) {
    var el = document.getElementById('fil' + canIdx);
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

      var names = ['can0', 'can1'];
      for (var i = 0; i < 2; i++) {
        var f = (cfg.cans || {})[names[i]];
        if (!f) continue;
        document.getElementById('br' + i).value = f.bitrate || '';
        document.getElementById('fd' + i).value = f.fd || 'off';
        document.getElementById('db' + i).value = f.dbitrate || '';
        setSwitch(i, f.up === 'on');
        chkFd(i);
        setFilters(i, f.filters);
      }
    } catch(e) {}
  }
  window.loadCfg = loadCfg;

  document.getElementById('f').addEventListener('submit', async function(e) {
    e.preventDefault();
    var d = {};
    d['ifname0'] = 'can0';
    d['bitrate0'] = document.getElementById('br0').value;
    d['fd0'] = document.getElementById('fd0').value;
    d['dbitrate0'] = document.getElementById('db0').value;
    d['up0'] = document.getElementById('tg0').checked ? 'on' : 'off';
    d['ifname1'] = 'can1';
    d['bitrate1'] = document.getElementById('br1').value;
    d['fd1'] = document.getElementById('fd1').value;
    d['dbitrate1'] = document.getElementById('db1').value;
    d['up1'] = document.getElementById('tg1').checked ? 'on' : 'off';
    d['tcp_port'] = document.getElementById('tp').value;
    d['max_clients'] = document.getElementById('mc').value;
    d['video_device'] = document.getElementById('vd').value;
    d['video_width'] = document.getElementById('vw').value;
    d['video_height'] = document.getElementById('vh').value;

    for (var i = 0; i < 2; i++) {
      var rows = document.getElementById('fil' + i).children;
      for (var k = 0; k < rows.length; k++) {
        var inputs = rows[k].querySelectorAll('input');
        if (inputs.length >= 2) {
          d['filter_id_' + i + '_' + k] = inputs[0].value;
          d['filter_mask_' + i + '_' + k] = inputs[1].value;
        }
      }
    }

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
