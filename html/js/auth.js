/* auth.js — 前端统一 Basic Auth 封装。
 *
 * 后端对写接口（删除/保存/重启/关机/上传等）强制 root 账号 Basic Auth，
 * 单纯依赖浏览器缓存凭据不可靠（fetch 请求 401 时不弹登录框）。
 * 这里把凭据存入 sessionStorage，所有写请求显式携带 Authorization 头，
 * 401 时提示输入账号密码并自动重试一次。
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

  /* 弹出登录框（root 账号），返回 Basic 凭据；取消返回 null */
  function promptLogin() {
    var u = prompt('需要管理员权限\n请输入用户名（默认 root）：');
    if (!u) return null;
    var p = prompt('请输入 ' + u + ' 的密码：');
    if (p === null) return null;
    var cred = '';
    try { cred = btoa(u + ':' + p); } catch(e) { return null; }
    set(cred);
    return cred;
  }

  var loginBusy = false;   /* 是否已有请求正在弹窗等待输入 */
  var waiters = [];        /* 等待同一登录流程的请求回调 */

  /* 取凭据：缓存有直接用；无缓存时只让第一个请求弹窗，其余等待其结果。
     promptLogin 是同步的（prompt 阻塞），无需 Promise 包装；
     try/catch 保证 loginBusy 一定复位，异常时等待者也能拿到 null。 */
  function getCred() {
    var cred = get();
    if (cred) return Promise.resolve(cred);
    if (loginBusy) {
      return new Promise(function(resolve) { waiters.push(resolve); });
    }
    loginBusy = true;
    var c;
    try {
      c = promptLogin();
    } catch (e) {
      c = null;
    }
    while (waiters.length) waiters.shift()(c);
    loginBusy = false;
    return Promise.resolve(c);
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
      clear();
      cred = await getCred();   /* 并发 401 也合并为一次登录 */
      if (!cred) throw new Error('auth cancelled');
      h['Authorization'] = 'Basic ' + cred;
      r = await fetch(url, opts);
    }
    return r;
  }

  window.authFetch = authFetch;
})();
