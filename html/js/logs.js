/* logs.js — 日志文件页面逻辑 */
(function() {

  function fmtSize(bytes) {
    var k = bytes / 1024;
    if (k >= 1024) return (k / 1024).toFixed(1) + ' MB';
    return k.toFixed(1) + ' KB';
  }

  async function load() {
    var tbody = document.getElementById('rows');
    tbody.innerHTML = '';
    try {
      var data = await (await fetch('/api/logs')).json();
      var logs = data.logs || [];
      if (logs.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4">暂无日志文件</td></tr>';
        return;
      }
      logs.forEach(function(f) {
        var tr = document.createElement('tr');

        var tdName = document.createElement('td');
        tdName.textContent = f.name;

        var tdSize = document.createElement('td');
        tdSize.textContent = fmtSize(f.size);

        var tdTime = document.createElement('td');
        tdTime.textContent = f.mtime;

        var tdOp = document.createElement('td');
        var dl = document.createElement('a');
        dl.className = 'dl';
        dl.href = '/logfile/' + encodeURIComponent(f.name);
        dl.textContent = '下载';
        var del = document.createElement('button');
        del.className = 'del';
        del.textContent = '删除';
        del.addEventListener('click', function() {
          fetch('/logfile/' + encodeURIComponent(f.name), {method: 'DELETE'})
            .then(load);
        });
        tdOp.appendChild(dl);
        tdOp.appendChild(del);

        tr.appendChild(tdName);
        tr.appendChild(tdSize);
        tr.appendChild(tdTime);
        tr.appendChild(tdOp);
        tbody.appendChild(tr);
      });
    } catch(e) {
      tbody.innerHTML = '<tr><td colspan="4">加载失败</td></tr>';
    }
  }

  load();
})();
