// SPDX-License-Identifier: GPL-3.0-or-later
/* monitor.js — 仪表盘数据刷新 */
(function() {
  var lastCpu = {user:0,sys:0,idle:0,total:0};
  var lastCores = [];
  var npuInited = false;

  /* 网络速度跟踪 */
  var lastNet = {eth0:{rx:0,tx:0,ts:0},wlan0:{rx:0,tx:0,ts:0}};
  var netInited = false;

  /* 视频流：单条 fetch 连接同时喂 <img>（blob URL 逐帧）并扫描 SOI(FF D8) 计数。
     旧方案是 <img src> 显示 + 独立 fetch 计数双路下载同一流（流量×2），
     浏览器/无线吃紧时会间歇掉速；单路方案显示与计数天然一致 */
  var fpsCnt = 0, fpsWin = Date.now(), fpsVal = 0;
  var streamUrl = '', streamAbort = null, streamRunning = false;
  var imgEl = null, lastBlobUrl = null;
  var imgBusy = false;      /* 上一帧还在解码：不覆盖 src（覆盖会取消其加载） */
  var pendingBlob = null;   /* 解码期间收到的最新帧，仅保留最后一张，解码完立即补显 */

  function resetFps() { fpsCnt = 0; fpsWin = Date.now(); fpsVal = 0; }

  /* 显示一帧：仅在上一次 onload/onerror 之后调用，上一帧 URL 此时已被完整消费，
     可安全回收；onload 前就 revoke 会中断在途解码，弱设备上永远看不到画面 */
  function showFrame(blob) {
    var u = URL.createObjectURL(blob);
    imgBusy = true;
    imgEl.onload = imgEl.onerror = function() {
      imgBusy = false;
      if (pendingBlob) {          /* 解码慢于码流时丢弃中间帧，只补最新一帧 */
        var pb = pendingBlob;
        pendingBlob = null;
        showFrame(pb);
      }
    };
    imgEl.src = u;
    if (lastBlobUrl) URL.revokeObjectURL(lastBlobUrl);
    lastBlobUrl = u;
  }

  function streamStop() {
    if (streamAbort) { streamAbort.abort(); streamAbort = null; }
    streamRunning = false;
    imgBusy = false;
    pendingBlob = null;
    if (lastBlobUrl) { URL.revokeObjectURL(lastBlobUrl); lastBlobUrl = null; }
  }

  function streamStart(url) {
    if (streamUrl === url && streamRunning) return;   /* 同一流已在播放 */
    streamStop();
    streamUrl = url;
    streamRunning = true;
    resetFps();
    var ac = new AbortController();
    streamAbort = ac;
    var buf = new Uint8Array(0);

    fetch(url, {signal: ac.signal}).then(function(r) {
      if (!r.ok || !r.body) { streamRunning = false; return; }
      var rd = r.body.getReader();
      (function pump() {
        rd.read().then(function(c) {
          if (c.done) { streamRunning = false; streamAbort = null; return; }
          var b = c.value;
          var nb = new Uint8Array(buf.length + b.length);
          nb.set(buf, 0); nb.set(b, buf.length);
          buf = nb;
          /* 扫 SOI(FF D8)…EOI(FF D9) 成帧（multipart 头被自然跳过；跨块帧头尾由缓冲续接） */
          var p = 0;
          while (p < buf.length - 1) {
            if (buf[p] === 0xFF && buf[p + 1] === 0xD8) {
              var q = p + 2;
              while (q < buf.length - 1 && !(buf[q] === 0xFF && buf[q + 1] === 0xD9)) q++;
              if (q >= buf.length - 1) break;          /* 帧未收全：等下一块 */
              if (imgEl) {
                fpsCnt++;                              /* 计数含被丢弃的帧（真实码流速率） */
                var blob = new Blob([buf.subarray(p, q + 2)], {type: 'image/jpeg'});
                if (imgBusy) pendingBlob = blob;       /* 解码忙：只留最新帧，防 src 覆盖风暴 */
                else showFrame(blob);
              }
              p = q + 2;
            } else p++;
          }
          /* 丢弃已消费数据，仅保留未完成帧的尾部 */
          if (p > 0) buf = buf.subarray(p);
          else if (buf.length > 65536) buf = buf.subarray(buf.length - 65536);  /* 无帧头垃圾上限 */
          pump();
        }).catch(function() { streamRunning = false; streamAbort = null; });  /* 流中断：下次 update 自动重连 */
      })();
    }).catch(function() { streamRunning = false; streamAbort = null; });
  }

  /* ---- H.265 WebRTC 播放器（WHEP，默认路径） ----
     POST SDP offer 到 /api/webrtc/whep（本服务反向代理 mediamtx），
     收 answer 后 RTCPeerConnection 直连 mediamtx 收流（媒体不经过本服务，
     同网段走 UDP，弱网自适应丢包）。
     rVFC 计数喂 FPS 徽标；失败 1s 节流重连；5s 无帧看门狗重连 */
  var wrPc = null, wrRunning = false, wrUrl = '', wrLastFail = 0, wrLastFrame = 0,
      wrGotFrame = false;

  function wrSupported() { return !!window.RTCPeerConnection; }

  function wrStop() {
    if (wrPc) { try { wrPc.close(); } catch(e) {} wrPc = null; }
    wrRunning = false;
    wrUrl = '';
    var vid = document.getElementById('stream_video');
    if (vid && vid.srcObject) {
      try { vid.srcObject.getTracks().forEach(function(t) { t.stop(); }); } catch(e) {}
      vid.srcObject = null;
    }
  }

  async function wrStart(url) {
    if (wrUrl === url && wrRunning) return;          /* 同一连接已建立 */
    if (Date.now() - wrLastFail < 1000) return;      /* 失败 1s 节流（update 200ms 轮询） */
    wrStop();
    wrUrl = url;
    wrRunning = true;
    wrLastFrame = Date.now();
    wrGotFrame = false;
    resetFps();
    var vid = document.getElementById('stream_video');
    try {
      var pc = new RTCPeerConnection();
      wrPc = pc;
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.onconnectionstatechange = function() {
        if (pc.connectionState === 'failed') { wrLastFail = Date.now(); wrStop(); }
        else if (pc.connectionState === 'disconnected') {
          /* ICE 短暂中断：留给 Chrome 自动恢复（iceRestart），5s 仍断则重连 */
          setTimeout(function() {
            if (wrPc === pc && pc.connectionState === 'disconnected') {
              wrLastFail = Date.now(); wrStop();
            }
          }, 5000);
        }
      };
      var cbStarted = false;
      pc.ontrack = function(ev) {
        if (vid.srcObject !== ev.streams[0]) vid.srcObject = ev.streams[0];
        /* FPS 徽标：rVFC 每呈现一帧回调一次（与 MJPEG 回退路径共用 fpsCnt）。
           回调链绑定本 pc：重连后 wrPc 换新，旧链自行终止——
           若只用全局 wrRunning 守卫，重连时它已被置回 true，旧链杀不掉，
           多条链叠加计数导致 FPS 虚高（如 29fps 流显示 72） */
        if (!cbStarted && vid.requestVideoFrameCallback) {
          cbStarted = true;
          var cb = function() {
            if (wrPc !== pc) return;
            fpsCnt++; wrLastFrame = Date.now(); wrGotFrame = true;
            vid.requestVideoFrameCallback(cb);
          };
          vid.requestVideoFrameCallback(cb);
        }
      };
      var offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      var r = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/sdp' },
        body: offer.sdp
      });
      if (!r.ok) { wrLastFail = Date.now(); wrStop(); return; }
      await pc.setRemoteDescription({ type: 'answer', sdp: await r.text() });
      var p = vid.play();
      if (p && p.catch) p.catch(function(){});       /* muted+autoplay 正常应成功 */
    } catch(e) {                                     /* 握手失败：节流重连 */
      wrLastFail = Date.now();
      wrStop();
    }
  }

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
      /* 视频流：默认走 H.265 WebRTC（弱网自适应、延迟低）；
         显式配置 URL 或浏览器不支持 WebRTC 时回退原 img/video 管线 */
      try {
        var streamUrl = window.STREAM_URL || '';
        var explicit = !!streamUrl;
        if (!explicit) {
          var r2 = await fetch('/api/config');
          if (r2.ok) {
            var cfg = await r2.json();
            if (cfg.stream_url || cfg.video_url) {
              streamUrl = cfg.stream_url || cfg.video_url;
              explicit = true;
            }
          }
        }
        var vid = document.getElementById('stream_video');
        var img = document.getElementById('stream_img');
        var useWebrtc = !explicit && wrSupported();
        if (!explicit && !useWebrtc) streamUrl = '/video/mjpeg_ai';   /* 无 WebRTC：回退画框流 */
        if (useWebrtc) {
          /* WebRTC 路径：WHEP 握手（本服务代理 mediamtx），断流/服务重启自动重连 */
          imgEl = null;
          streamStop();
          img.style.display = 'none';
          vid.style.display = '';
          wrStart('/api/webrtc/whep');      /* 每次轮询都调用：内部幂等 + 失败节流重连 */
        } else if (streamUrl) {
          wrStop();
          var useImg = streamUrl.indexOf('/video/mjpeg') === 0
                     || streamUrl.match(/\.mjpeg$|\.jpg$|\.jpeg$/i);
          if (useImg) {
            imgEl = img;                    /* 播放器逐帧喂 blob URL，显示与计数同一条连接 */
            img.style.display = '';
            vid.style.display = 'none';
            streamStart(streamUrl);         /* 每次轮询都调用：内部幂等，流中断时自动重连 */
          } else {
            imgEl = null;
            streamStop();
            if (!vid.src || vid.getAttribute('data-src') !== streamUrl) {
              vid.setAttribute('data-src', streamUrl);
              vid.src = streamUrl;
              vid.style.display = '';
              img.style.display = 'none';
              resetFps();
            }
          }
        }
      } catch(e) {}
    } catch(e) {}
    updateNet();
  }

  update();
  setInterval(update, 200);

  /* 每秒刷新视频流 FPS 徽标：窗口内无新帧则显示 --（流停滞） */
  setInterval(function() {
    var el = document.getElementById('video_fps');
    if (!el) return;
    var now = Date.now();
    if (now > fpsWin) {
      if (fpsCnt > 0) {
        fpsVal = fpsCnt * 1000 / (now - fpsWin);
        fpsCnt = 0; fpsWin = now;
      } else {
        fpsVal = 0;
      }
    }
    el.textContent = fpsVal > 0 ? Math.round(fpsVal) + ' FPS' : '-- FPS';
  }, 1000);

  /* WebRTC 看门狗：无呈现帧 → 重连（断流/服务重启自动恢复）。
     首帧宽限 12s：弱网下 握手+等关键帧+丢包恢复 常超 5s，过早重杀会
     反复从头协商，把起播时间越拖越长（journal 可见 4-6s 短命会话循环）；
     已出帧后仍用 5s 快速检测断流。页面隐藏时跳过：rVFC 不回调，
     否则后台标签每分钟都会误杀重连一次。 */
  setInterval(function() {
    if (document.hidden) { wrLastFrame = Date.now(); return; }
    if (wrRunning && Date.now() - wrLastFrame > (wrGotFrame ? 5000 : 12000)) {
      wrLastFail = Date.now();
      wrStop();
    }
  }, 1000);

  window.reboot = function() { if (!confirm('确定要重启设备吗？')) return; authFetch('/api/reboot').catch(function(){}); };
  window.shutdown = function() { if (!confirm('确定要关闭设备吗？')) return; authFetch('/api/shutdown').catch(function(){}); };
})();
