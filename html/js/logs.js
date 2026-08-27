/* logs.js — 日志与录像合并下载页面逻辑 */
(function() {

  function fmtSize(bytes) {
    var k = bytes / 1024;
    if (k >= 1024) return (k / 1024).toFixed(1) + ' MB';
    return k.toFixed(1) + ' KB';
  }

  /* 拆分 "日期/文件名" 两段式 name */
  function splitName(name) {
    var slash = name.indexOf('/');
    return slash >= 0 ? [name.slice(0, slash), name.slice(slash + 1)] : ['', name];
  }

  /* 通用表格行渲染：下载用 href（GET），删除用 op 回调 */
  function addRow(tbody, f, dlUrl, delFn) {
    var parts = splitName(f.name);
    var date = parts[0], file = parts[1];

    var tr = document.createElement('tr');

    var tdDate = document.createElement('td');
    tdDate.textContent = date;

    var tdName = document.createElement('td');
    tdName.textContent = file;

    var tdSize = document.createElement('td');
    tdSize.textContent = fmtSize(f.size);

    var tdTime = document.createElement('td');
    tdTime.textContent = f.mtime;

    var tdOp = document.createElement('td');
    var dl = document.createElement('a');
    dl.className = 'dl';
    dl.href = dlUrl + encodeURIComponent(f.name);
    dl.textContent = '下载';
    var del = document.createElement('button');
    del.className = 'del';
    del.textContent = '删除';
    del.addEventListener('click', delFn);
    tdOp.appendChild(dl);
    tdOp.appendChild(del);

    tr.appendChild(tdDate);
    tr.appendChild(tdName);
    tr.appendChild(tdSize);
    tr.appendChild(tdTime);
    tr.appendChild(tdOp);
    tbody.appendChild(tr);
  }

  /* ---- 日志列表 ---- */
  async function loadLogs() {
    var tbody = document.getElementById('rows_logs');
    tbody.innerHTML = '';
    try {
      var data = await (await fetch('/api/logs')).json();
      var logs = data.logs || [];
      if (logs.length === 0) {
        tbody.innerHTML = '<tr><td colspan="5">暂无日志文件</td></tr>';
        return;
      }
      logs.forEach(function(f) {
        addRow(tbody, f, '/logfile/', function() {
          authFetch('/logfile/' + encodeURIComponent(f.name), {method: 'DELETE'})
            .then(loadLogs);
        });
      });
    } catch(e) {
      tbody.innerHTML = '<tr><td colspan="5">加载失败</td></tr>';
    }
  }

  /* ---- 录像列表（按天分目录，自动录制持续生成，周期性刷新） ---- */
  async function loadRec() {
    var tbody = document.getElementById('rows_rec');
    tbody.innerHTML = '';
    try {
      var data = await (await fetch('/api/rec/list')).json();
      var files = data.files || [];
      if (files.length === 0) {
        tbody.innerHTML = '<tr><td colspan="5">暂无录像文件</td></tr>';
        return;
      }
      files.forEach(function(f) {
        addRow(tbody, f, '/recfile/', function() {
          if (!confirm('删除录像 ' + f.name + '？')) return;
          authFetch('/api/rec/delete', {method: 'POST', body: f.name})
            .then(function(r) { return r.ok ? loadRec() : alert('删除失败(' + r.status + ')'); })
            .catch(function() {});
        });
      });
    } catch(e) {
      tbody.innerHTML = '<tr><td colspan="5">加载失败</td></tr>';
    }
  }

  /* ---- Tab 切换 ---- */
  function switchTab(tab) {
    var isRec = tab === 'rec';
    document.getElementById('tbl_logs').style.display = isRec ? 'none' : '';
    document.getElementById('tbl_rec').style.display = isRec ? '' : 'none';
    document.getElementById('pack_btn').style.display = isRec ? 'none' : '';
    document.getElementById('pack_rec_btn').style.display = isRec ? '' : 'none';
    var btns = document.querySelectorAll('.tab-btn');
    for (var i = 0; i < btns.length; i++)
      btns[i].classList.toggle('tab-on', btns[i].getAttribute('data-tab') === tab);
  }
  window.switchTab = switchTab;

  loadLogs();
  loadRec();
  setInterval(loadRec, 5000);   /* 录像持续生成，低频刷新 */
})();
