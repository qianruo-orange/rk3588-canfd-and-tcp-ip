// SPDX-License-Identifier: GPL-3.0-or-later
/* auth.js — 前端统一 Basic Auth 封装。
 *
 * 后端对写接口（删除/保存/重启/关机/上传等）强制 root 账号 Basic Auth，
 * 单纯依赖浏览器缓存凭据不可靠（fetch 请求 401 时不弹登录框）。
 * 这里把凭据存入 sessionStorage，所有写请求显式携带 Authorization 头，
 * 401 时提示输入账号密码并自动重试一次。
 *
 * 登录框为自绘模态框：用户名、密码同框输入，替代原生 prompt 的两步询问；
 * 样式复用 common.css 的主题变量，深浅色主题自适应。
 *
 * 并发去重：批量删除等场景会同时发出多个写请求，若都触发 401/无凭据，
 * 只弹一次登录框，其余请求等待同一登录流程完成后自动重试。 */
(function() {
  var KEY = 'rk_basic_auth';

  function get() {
    try { return sessionStorage.getItem(KEY) || ''; } catch(e) { return ''; }
  }
  function set(v) {
    try { sessionStorage.setItem(KEY, v); } catch(e) {}
  }
  function clear() {
    try { sessionStorage.removeItem(KEY); } catch(e) {}
  }

  /* ---- 登录模态框（懒创建，全局复用一份 DOM） ---- */
  var loginBox = null;      /* 模态框 DOM 与控件引用 */
  var loginResolve = null;  /* 当前等待输入的 resolve（同时只开一个框） */
  var lastUser = 'root';    /* 记住上次输入的用户名，重试时预填 */

  /* 注入一次模态框样式（沿用 common.css 变量，深浅色自适应） */
  function ensureLoginStyle() {
    if (document.getElementById('auth-box-style')) return;
    var s = document.createElement('style');
    s.id = 'auth-box-style';
    s.textContent =
      '#auth-overlay{position:fixed;inset:0;z-index:1000;display:none;align-items:center;justify-content:center;background:rgba(4,8,20,.45);backdrop-filter:blur(4px);-webkit-backdrop-filter:blur(4px)}' +
      '#auth-box{width:min(340px,calc(100vw - 48px));background:var(--card-bg);border:1px solid var(--card-border2);border-radius:18px;padding:22px;backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);box-shadow:var(--shadow);color:var(--text)}' +
      '#auth-box h3{font-size:15px;color:var(--text-strong);margin-bottom:4px}' +
      '#auth-box .auth-sub{font-size:12px;color:var(--muted);margin-bottom:6px}' +
      '#auth-box label{display:block;font-size:12px;color:var(--muted);margin:10px 0 4px}' +
      '#auth-box input{width:100%;padding:8px 10px;border-radius:10px;border:1px solid var(--card-border2);background:var(--input-bg);color:var(--text-strong);font-size:14px;outline:none}' +
      '#auth-box input:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--glow)}' +
      '#auth-box .auth-err{display:none;font-size:12px;color:#e86878;margin-top:10px}' +
      '#auth-box .auth-err.show{display:block}' +
      '#auth-box .auth-btns{display:flex;gap:10px;justify-content:flex-end;margin-top:16px}' +
      '#auth-box button{padding:7px 16px;border-radius:10px;border:1px solid var(--card-border2);background:var(--btn-bg);color:var(--accent);cursor:pointer;font-size:13px;font-weight:600;transition:background .2s}' +
      '#auth-box button:hover{background:var(--hover)}' +
      '#auth-box button.primary{background:var(--accent);border-color:transparent;color:#0b1021}' +
      'html[data-theme="light"] #auth-box button.primary{color:#ffffff}';
    document.head.appendChild(s);
  }

  function ensureLoginBox() {
    if (loginBox) return loginBox;
    ensureLoginStyle();
    var overlay = document.createElement('div');
    overlay.id = 'auth-overlay';
    overlay.innerHTML =
      '<div id="auth-box">' +
      '<h3>需要管理员权限</h3>' +
      '<div class="auth-sub">请输入 root 或 sudo 组成员账号</div>' +
      '<label for="auth-user">用户名</label>' +
      '<input id="auth-user" autocomplete="username" spellcheck="false">' +
      '<label for="auth-pass">密码</label>' +
      '<input id="auth-pass" type="password" autocomplete="current-password">' +
      '<div class="auth-err" id="auth-err"></div>' +
      '<div class="auth-btns">' +
      '<button type="button" id="auth-cancel">取消</button>' +
      '<button type="button" id="auth-ok" class="primary">确定</button>' +
      '</div></div>';
    (document.body || document.documentElement).appendChild(overlay);
    var user = overlay.querySelector('#auth-user');
    var pass = overlay.querySelector('#auth-pass');
    var err = overlay.querySelector('#auth-err');

    function closeBox(result) {
      overlay.style.display = 'none';
      document.removeEventListener('keydown', onKey);
      var r = loginResolve;
      loginResolve = null;
      if (r) r(result);
    }
    function showErr(msg) {
      err.textContent = msg;
      err.classList.add('show');
    }
    function submit() {
      var u = user.value.trim();
      if (!u) { showErr('请输入用户名'); user.focus(); return; }
      var cred = '';
      try { cred = btoa(u + ':' + pass.value); } catch(e) { showErr('用户名或密码含无法编码的字符'); return; }
      lastUser = u;
      set(cred);
      closeBox(cred);
    }
    function onKey(e) {
      if (e.key === 'Escape') { e.preventDefault(); closeBox(null); }
      else if (e.key === 'Enter') {
        if (e.target === user) { e.preventDefault(); pass.focus(); }
        else if (e.target === pass) { e.preventDefault(); submit(); }
      }
    }
    overlay.querySelector('#auth-ok').addEventListener('click', submit);
    overlay.querySelector('#auth-cancel').addEventListener('click', function() { closeBox(null); });
    overlay.addEventListener('click', function(e) { if (e.target === overlay) closeBox(null); });

    loginBox = { overlay: overlay, user: user, pass: pass, err: err, onKey: onKey };
    return loginBox;
  }

  /* 弹出登录框（管理员账号：root 或 sudo 组成员），返回 Basic 凭据；取消返回 null。
     errMsg 非空时在框内显示错误提示（401 后重试场景）。 */
  function promptLogin(errMsg) {
    var box = ensureLoginBox();
    box.err.textContent = errMsg || '';
    box.err.classList.toggle('show', !!errMsg);
    box.user.value = lastUser;
    box.pass.value = '';
    box.overlay.style.display = 'flex';
    document.addEventListener('keydown', box.onKey);
    (errMsg ? box.pass : box.user).focus();
    return new Promise(function(resolve) { loginResolve = resolve; });
  }

  var loginBusy = false;   /* 是否已有请求正在弹窗等待输入 */
  var waiters = [];        /* 等待同一登录流程的请求回调 */

  /* 取凭据：缓存有直接用；无缓存时只让第一个请求弹窗，其余等待其结果。
     登录框是异步的，用 then 的成败分支保证 loginBusy 一定复位，
     异常时等待者也能拿到 null。 */
  function getCred(errMsg) {
    var cred = get();
    if (cred) return Promise.resolve(cred);
    if (loginBusy) {
      return new Promise(function(resolve) { waiters.push(resolve); });
    }
    loginBusy = true;
    return promptLogin(errMsg).then(function(c) {
      while (waiters.length) waiters.shift()(c);
      loginBusy = false;
      return c;
    }, function() {
      while (waiters.length) waiters.shift()(null);
      loginBusy = false;
      return null;
    });
  }

  /* 带认证的 fetch：先带缓存凭据请求，401 则要求登录后重试一次 */
  async function authFetch(url, opts) {
    opts = opts || {};
    var cred = await getCred();
    if (!cred) throw new Error('auth cancelled');
    var h = opts.headers || {};
    h['Authorization'] = 'Basic ' + cred;
    opts.headers = h;
    var r = await fetch(url, opts);
    if (r.status === 401) {
      /* 仅当存储里仍是本请求用过的那份凭据时才清除：
         若并发请求已登录并写入新凭据，直接复用，避免连环弹窗 */
      if (get() === cred) clear();
      cred = await getCred('用户名或密码错误，请重试');   /* 并发 401 也合并为一次登录 */
      if (!cred) throw new Error('auth cancelled');
      h['Authorization'] = 'Basic ' + cred;
      r = await fetch(url, opts);
    }
    return r;
  }

  window.authFetch = authFetch;
})();
