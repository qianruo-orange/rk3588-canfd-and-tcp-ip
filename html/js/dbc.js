/* dbc.js — 分别轮询接收 / 发送方向的 DBC 解析结果并渲染 */
(function() {
  function esc(s) {
    return String(s).replace(/[&<>"]/g, function(c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }

  /* 为某一方向（接收/发送）创建一套轮询 + 过滤 + 渲染视图 */
  function makeView(rowsId, metaId, channelId, apiUrl) {
    var latest = [];
    var tbody = document.getElementById(rowsId);
    var meta = document.getElementById(metaId);
    var sel = document.getElementById(channelId);

    function render() {
      var ifname = sel.value;   // '' = 全部
      var arr = ifname ? latest.filter(function(e) { return e.ifname === ifname; }) : latest;

      if (!arr.length) {
        tbody.innerHTML = '<tr><td colspan="5" class="empty">暂无解析数据（请确认已配置 dbc_path 且总线上有匹配报文）</td></tr>';
        meta.textContent = '';
        return;
      }

      var h = '';
      for (var i = 0; i < arr.length; i++) {
        var e = arr[i];
        h += '<tr>'
          + '<td>' + (i + 1) + '</td>'
          + '<td class="mono">' + esc(e.ifname) + '</td>'
          + '<td class="mono id">' + esc(e.id) + '</td>'
          + '<td class="name">' + esc(e.name) + '</td>'
          + '<td class="mono sig">' + esc(e.text) + '</td>'
          + '</tr>';
      }
      tbody.innerHTML = h;
      meta.textContent = '共 ' + arr.length + ' 条记录，每 0.5s 自动刷新';
    }

    function refreshChannels() {
      var cur = sel.value;
      var set = {};
      latest.forEach(function(e) { set[e.ifname] = 1; });
      var names = Object.keys(set).sort();

      var h = '<option value="">全部</option>';
      names.forEach(function(n) {
        h += '<option value="' + esc(n) + '">' + esc(n) + '</option>';
      });
      sel.innerHTML = h;
      if (names.indexOf(cur) >= 0) sel.value = cur;   // 尽量保留当前选择
    }

    async function update() {
      try {
        var r = await fetch(apiUrl);
        var arr = await r.json();
        latest = Array.isArray(arr) ? arr : [];
        refreshChannels();
        render();
      } catch (e) {
        tbody.innerHTML = '<tr><td colspan="5" class="empty">加载失败</td></tr>';
        meta.textContent = '';
      }
    }

    sel.addEventListener('change', render);
    update();
    setInterval(update, 500);
  }

  makeView('rows', 'meta', 'channel', '/api/can/decoded');
  makeView('rows-tx', 'meta-tx', 'channel-tx', '/api/can/decoded/tx');
})();
