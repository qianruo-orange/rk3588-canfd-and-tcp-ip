/* monitor.js — 仪表盘数据刷新 */
(function() {
  var lastCpu = {user:0,sys:0,idle:0,total:0};
  var lastCores = [];
  var npuInited = false;

  /* ---- 网络录像（默认自动开启，仅显示状态） ---- */
  var rec = { recording:false, startTs:0, frames:0, bytes:0, fps:0 };

  function fmtBytes(n) {
    if (n < 1024) return n + 'B';
    if (n < 1024*1024) return (n/1024).toFixed(1) + 'KB';
    return (n/1024/1024).toFixed(2) + 'MB';
  }
  function fmtDur(ms) {
    var s = Math.floor(ms/1000);
    var m = Math.floor(s/60), h = Math.floor(m/60);
    return (h>0 ? h+':' : '') + ('0'+ (m%60)).slice(-2) + ':' + ('0'+(s%60)).slice(-2);
  }

  async function updateRec() {
    try {
      var r = await fetch('/api/rec/status');
      if (!r.ok) return;
      var d = await r.json();
      rec.recording = !!d.recording;
      rec.frames = d.frames || 0;
      rec.bytes = d.bytes || 0;
      rec.fps = d.fps || 0;
      if (rec.recording && !rec.startTs) rec.startTs = Date.now();
      if (!rec.recording) rec.startTs = 0;

      var st = document.getElementById('rec_status');
      if (!st) return;
      if (rec.recording) {
        st.textContent = '● REC ' + fmtDur(Date.now() - rec.startTs)
          + ' · ' + fmtBytes(rec.bytes);
        st.className = 'rec-status rec-live';
      } else {
        st.textContent = rec.frames > 0 ? '已停止' : '';
        st.className = 'rec-status';
      }
    } catch(e) {}
  }

  /* 录像文件列表已合并到日志页（/logs），此处不再渲染 */

  /* 网络速度跟踪 */
  var lastNet = {eth0:{rx:0,tx:0,ts:0},wlan0:{rx:0,tx:0,ts:0}};
  var netInited = false;

  function barColor(p) { return p < 60 ? 'green' : p < 85 ? 'yellow' : 'red'; }

  function fmtSpeed(bytesPerSec) {
    if (bytesPerSec < 0) return '--';
    if (bytesPerSec < 1024) return bytesPerSec.toFixed(0);
    if (bytesPerSec < 1024 * 1024) return (bytesPerSec / 1024).toFixed(1);
    return (bytesPerSec / 1024 / 1024).toFixed(1);
  }
  function fmtUnit(bytesPerSec) {
    if (bytesPerSec < 0) return '';
    if (bytesPerSec < 1024) return 'B/s';
    if (bytesPerSec < 1024 * 1024) return 'KB/s';
    return 'MB/s';
  }

  function initNetUI() {
    if (netInited) return;
    var h = '';
    var ifaces = [{id:'eth0',name:'有线'},{id:'wlan0',name:'无线'}];
    for (var i = 0; i < ifaces.length; i++) {
      h += '<div class="net-iface"><h4>' + ifaces[i].name
        + '<span class="if-status" id="ns_' + ifaces[i].id + '_st">--</span></h4>';
      h += '<div class="net-row"><span class="net-dir">↓</span>'
        + '<span class="net-val" id="ns_' + ifaces[i].id + '_rx">--</span>'
        + '<span class="net-unit" id="ns_' + ifaces[i].id + '_rx_u"></span></div>';
      h += '<div class="net-row"><span class="net-dir">↑</span>'
        + '<span class="net-val" id="ns_' + ifaces[i].id + '_tx">--</span>'
        + '<span class="net-unit" id="ns_' + ifaces[i].id + '_tx_u"></span></div>';
      h += '</div>';
    }
    document.getElementById('net_speed').innerHTML = h;
    netInited = true;
  }

  async function updateNet() {
    try {
      var r = await fetch('/api/network');
      var d = await r.json();
      initNetUI();
      var now = Date.now();

      ['eth0','wlan0'].forEach(function(id) {
        var iface = d[id];
        var prev = lastNet[id];
        var active = iface && (iface.rx > 0 || iface.tx > 0);

        /* 状态 */
        var stEl = document.getElementById('ns_' + id + '_st');
        stEl.textContent = active ? '已连接' : '未连接';
        stEl.className = 'if-status ' + (active ? 'if-up' : 'if-down');

        if (active && prev.ts > 0) {
          var dt = (now - prev.ts) / 1000;
          if (dt > 0) {
            var rxSpeed = (iface.rx - prev.rx) / dt;
            var txSpeed = (iface.tx - prev.tx) / dt;
            document.getElementById('ns_' + id + '_rx').textContent = fmtSpeed(rxSpeed);
            document.getElementById('ns_' + id + '_rx_u').textContent = fmtUnit(rxSpeed);
            document.getElementById('ns_' + id + '_tx').textContent = fmtSpeed(txSpeed);
            document.getElementById('ns_' + id + '_tx_u').textContent = fmtUnit(txSpeed);
          }
        }

        prev.rx = iface ? iface.rx : 0;
        prev.tx = iface ? iface.tx : 0;
        prev.ts = now;
      });
    } catch(e) {}
  }

  function initCoreBars(id, names) {
    if (npuInited) return;
    var h = '';
    for (var i = 0; i < names.length; i++)
      h += '<div><div class="core-label"><span>' + names[i] + '</span><span id="' + id + '_pct' + i + '">0%</span></div><div class="core-bar"><div id="' + id + '_bar' + i + '" style="width:0%"></div></div></div>';
    document.getElementById(id).innerHTML = h;
  }

  function fmtTime() {
    var d = new Date(), wk = ['日','一','二','三','四','五','六'];
    var y = d.getFullYear();
    var m = ('0' + (d.getMonth() + 1)).slice(-2);
    var day = ('0' + d.getDate()).slice(-2);
    var h = ('0' + d.getHours()).slice(-2);
    var min = ('0' + d.getMinutes()).slice(-2);
    return y + '_' + m + '_' + day + ' 星期' + wk[d.getDay()] + ' ' + h + ':' + min;
  }

  async function update() {
    try {
      var r = await fetch('/api/system');
      var d = await r.json();
      document.getElementById('temp').textContent = d.temp + '°C';

      /* 总体 CPU */
      var cpuPct = 0;
      if (lastCpu.total > 0) {
        var du = d.cpu_user + d.cpu_sys - lastCpu.user - lastCpu.sys;
        var dt = d.cpu_total - lastCpu.total;
        cpuPct = dt > 0 ? Math.round(du * 100 / dt) : 0;
      }
      lastCpu = {user:d.cpu_user, sys:d.cpu_sys, idle:d.cpu_idle, total:d.cpu_total};
      document.getElementById('cpu_val').textContent = cpuPct + '%';
      var cb = document.getElementById('cpu_bar');
      cb.style.width = cpuPct + '%'; cb.className = barColor(cpuPct);

      /* CPU 逐核 */
      if (d.cpu_cores) {
        initCoreBars('cpu_cores', ['Core0','Core1','Core2','Core3','Core4','Core5','Core6','Core7']);
        if (!lastCores.length) lastCores = d.cpu_cores.map(function(c) { return {u:c.u,s:c.s,i:c.i}; });
        for (var i = 0; i < 8; i++) {
          var c = d.cpu_cores[i], l = lastCores[i];
          var du2 = c.u + c.s - l.u - l.s, di = c.i - l.i, dt2 = du2 + di;
          var p = dt2 > 0 ? Math.round(du2 * 100 / dt2) : 0;
          document.getElementById('cpu_cores_pct' + i).textContent = p + '%';
          var b = document.getElementById('cpu_cores_bar' + i);
          b.style.width = p + '%'; b.className = barColor(p);
        }
        lastCores = d.cpu_cores.map(function(c) { return {u:c.u,s:c.s,i:c.i}; });
      }

      /* NPU 逐核 */
      initCoreBars('npu_cores', ['NPU0','NPU1','NPU2']);
      for (var i = 0; i < 3; i++) {
        var p2 = d.npu_cores[i];
        document.getElementById('npu_cores_pct' + i).textContent = (p2 >= 0 ? p2 + '%' : 'N/A');
        var b2 = document.getElementById('npu_cores_bar' + i);
        b2.style.width = (p2 >= 0 ? p2 : 0) + '%'; b2.className = 'blue';
      }
      npuInited = true;

      document.getElementById('mem_val').textContent = d.mem_pct + '%';
      document.getElementById('mem_total').textContent = (d.mem_total/1024/1024).toFixed(1) + 'G';
      document.getElementById('mem_used').textContent = ((d.mem_total - d.mem_avail)/1024/1024).toFixed(1) + 'G';
      var mb = document.getElementById('mem_bar');
      mb.style.width = d.mem_pct + '%'; mb.className = d.mem_pct < 60 ? 'green' : d.mem_pct < 85 ? 'yellow' : 'red';

      document.getElementById('gpu_val').textContent = (d.gpu >= 0 ? d.gpu + '%' : 'N/A');
      var gb = document.getElementById('gpu_bar');
      gb.style.width = (d.gpu >= 0 ? d.gpu : 0) + '%'; gb.className = 'blue';

      var dp = Math.round(100 - d.disk_avail * 100 / d.disk_total);
      document.getElementById('disk_val').textContent = dp + '%';
      document.getElementById('disk_total').textContent = (d.disk_total > 1024 ? (d.disk_total/1024).toFixed(1) + 'G' : d.disk_total + 'M');
      document.getElementById('disk_used').textContent = (d.disk_total - d.disk_avail > 1024 ? ((d.disk_total - d.disk_avail)/1024).toFixed(1) + 'G' : (d.disk_total - d.disk_avail) + 'M');
      var db = document.getElementById('disk_bar');
      db.style.width = dp + '%'; db.className = barColor(dp);

      document.getElementById('datetime').textContent = fmtTime();
      /* 视频流尝试自动加载配置的 URL（如果存在） */
      try {
        var streamUrl = window.STREAM_URL || '/video/mjpeg';
        if (!streamUrl || streamUrl === '/video/mjpeg') {
          var r2 = await fetch('/api/config');
          if (r2.ok) {
            var cfg = await r2.json();
            streamUrl = cfg.stream_url || cfg.video_url || streamUrl;
          }
        }
        if (streamUrl) {
          var vid = document.getElementById('stream_video');
          var img = document.getElementById('stream_img');
          var useImg = streamUrl.indexOf('/video/mjpeg') === 0
                     || streamUrl.match(/\.mjpeg$|\.jpg$|\.jpeg$/i);
          if (useImg) {
            if (!img.src || img.getAttribute('data-src') !== streamUrl) {
              img.setAttribute('data-src', streamUrl);
              img.src = streamUrl;
              img.style.display = '';
              vid.style.display = 'none';
            }
          } else {
            if (!vid.src || vid.getAttribute('data-src') !== streamUrl) {
              vid.setAttribute('data-src', streamUrl);
              vid.src = streamUrl;
              vid.style.display = '';
              img.style.display = 'none';
            }
          }
        }
      } catch(e) {}
    } catch(e) {}
    updateNet();
    updateRec();
  }

  update();
  setInterval(update, 200);
})();
