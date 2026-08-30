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

  /* ---- H.264 fMP4 / MSE 播放器（默认 /video/stream） ----
     单条 fetch 喂 MediaSource：init 段(ftyp/moov) + 逐片段(moof/mdat) append；
     'moof' 签名计数喂 FPS 徽标（近似，仅显示用）。失败 1s 节流重连，
     服务端断流（会话轮转）立即重连；1s 看门狗防缓冲堆积与假死 */
  var mseMs = null, mseSb = null, mseAbort = null, mseReader = null;
  var mseRunning = false, mseUrl = '', mseLastData = 0, mseLastFail = 0;
  var mseTail = new Uint8Array(3);   /* 跨块 'moof' 签名续接尾巴 */

  function mseSupported() {
    if (!window.MediaSource || !MediaSource.isTypeSupported) return false;
    var codes = ['avc1.64001f', 'avc1.4d401f', 'avc1.42e01f'];
    for (var i = 0; i < codes.length; i++)
      if (MediaSource.isTypeSupported('video/mp4; codecs="' + codes[i] + '"')) return true;
    return false;
  }

  function mseStop() {
    if (mseReader) { try { mseReader.cancel(); } catch(e) {} mseReader = null; }
    if (mseAbort) { mseAbort.abort(); mseAbort = null; }
    if (mseMs) {
      try {
        if (mseSb && mseMs.readyState === 'open') mseMs.removeSourceBuffer(mseSb);
      } catch(e) {}
      mseMs = null;
    }
    mseSb = null;
    mseRunning = false;
    mseUrl = '';
    mseTail[0] = mseTail[1] = mseTail[2] = 0;
    var vid = document.getElementById('stream_video');
    if (vid) {
      if (vid.src && vid.src.indexOf('blob:') === 0) URL.revokeObjectURL(vid.src);
      vid.removeAttribute('src');
      vid.load();                          /* 清空播放器（readyState 归零） */
    }
  }

  function mseAppend(sb, chunk, tries) {
    if (!mseRunning || mseSb !== sb) return;
    if (sb.updating) {                     /* appendBuffer 未完成：稍后重试（16ms 步进，时序 FIFO） */
      if (tries > 50) { mseLastFail = Date.now(); mseStop(); return; }
      setTimeout(function() { mseAppend(sb, chunk, tries + 1); }, 16);
      return;
    }
    try {
      sb.appendBuffer(chunk);
    } catch(e) {                           /* QuotaExceeded/NotSupported 等 */
      mseLastFail = Date.now();
      mseStop();
    }
  }

  function mseScanMoof(chunk) {
    var buf = new Uint8Array(mseTail.length + chunk.length);
    buf.set(mseTail, 0);
    buf.set(chunk, mseTail.length);
    for (var i = 0; i + 3 < buf.length; i++) {
      if (buf[i] === 0x6d && buf[i+1] === 0x6f && buf[i+2] === 0x6f && buf[i+3] === 0x66) {
        fpsCnt++;
        i += 3;
      }
    }
    mseTail.set(buf.subarray(buf.length - 3));
  }

  function mseStart(url) {
    if (mseUrl === url && mseRunning) return;        /* 同一流已在播放 */
    if (Date.now() - mseLastFail < 1000) return;     /* 失败 1s 节流（update 200ms 轮询） */
    mseStop();
    mseUrl = url;
    mseRunning = true;
    mseLastData = Date.now();
    resetFps();
    var ac = new AbortController();
    mseAbort = ac;

    fetch(url, {signal: ac.signal}).then(function(r) {
      if (!r.ok || !r.body) {                        /* 503（无编码会话）：节流重试 */
        mseLastFail = Date.now();
        mseStop();
        return;
      }
      var codec = r.headers.get('X-Codec') || 'avc1.4d401f';
      var ms = new MediaSource();
      mseMs = ms;
      var vid = document.getElementById('stream_video');
      vid.src = URL.createObjectURL(ms);
      var rd = r.body.getReader();
      mseReader = rd;

      ms.addEventListener('sourceopen', function() {
        if (mseMs !== ms) return;                    /* 期间已被 mseStop：丢弃 */
        var sb;
        try {
          sb = ms.addSourceBuffer('video/mp4; codecs="' + codec + '"');
        } catch(e) {
          mseLastFail = Date.now();
          mseStop();
          return;
        }
        mseSb = sb;
        sb.addEventListener('error', function() { mseStop(); });
        var p = vid.play();
        if (p && p.catch) p.catch(function(){});     /* muted+autoplay 正常应成功 */

        (function pump() {
          rd.read().then(function(c) {
            if (c.done) { mseStop(); return; }       /* 服务端断连（会话轮转）：立即重连 */
            mseLastData = Date.now();
            mseScanMoof(c.value);
            mseAppend(sb, c.value, 0);
            pump();
          }).catch(function() { mseStop(); });
        })();
      });
    }).catch(function() {                            /* 网络失败/中断：节流重连 */
      mseLastFail = Date.now();
      mseStop();
    });
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
      /* 视频流：默认走 H.264 fMP4/MSE（~2Mbps，弱网不卡顿）；
         显式配置 URL 或浏览器不支持 MSE 时回退原 img/video 管线 */
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
        var useMse = !explicit && mseSupported();
        if (!explicit && !useMse) streamUrl = '/video/mjpeg_ai';   /* 无 MSE：回退画框流 */
        if (useMse) {
          /* MSE 路径：录制编码扇出（无会话 503 节流重试，会话轮转断流自动重连） */
          imgEl = null;
          streamStop();
          img.style.display = 'none';
          vid.style.display = '';
          mseStart('/video/stream');        /* 每次轮询都调用：内部幂等 + 失败节流重连 */
        } else if (streamUrl) {
          mseStop();
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

  /* MSE 延迟看门狗：缓冲超前 >3s（播放落后于实时）或 5s 无数据 → 重连接合最新 IDR。
     页面隐藏时跳过：后台标签浏览器暂停视频解码，currentTime 不再推进而数据仍在
     入缓冲，缓冲超前必然突破 3s，会每秒误杀重连一次（WebRTC 版曾在 journal 里
     刷出成片的短命会话，同一个坑）。隐藏期间顺带把无数据计时基准推后，
     切回前台时从当前时刻重新计。 */
  setInterval(function() {
    if (!mseRunning || !mseSb || !mseMs) return;
    if (document.hidden) { mseLastData = Date.now(); return; }
    var vid = document.getElementById('stream_video');
    try {
      var b = mseSb.buffered;
      if (b.length > 0 && b.end(b.length - 1) - vid.currentTime > 3) { mseStop(); return; }
    } catch(e) {}
    if (vid.readyState === 0 && Date.now() - mseLastData > 5000) mseStop();
  }, 1000);

  window.reboot = function() { if (!confirm('确定要重启设备吗？')) return; authFetch('/api/reboot').catch(function(){}); };
  window.shutdown = function() { if (!confirm('确定要关闭设备吗？')) return; authFetch('/api/shutdown').catch(function(){}); };
})();
