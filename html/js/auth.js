/* auth.js — 前端统一 Basic Auth 封装。
 *
 * 后端对写接口（删除/保存/重启/关机/上传等）强制 root 账号 Basic Auth，
 * 单纯依赖浏览器缓存凭据不可靠（fetch 请求 401 时不弹登录框）。
 * 这里把凭据存入 sessionStorage，所有写请求显式携带 Authorization 头，
 * 401 时提示输入账号密码并自动重试一次。 */
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

  function credOrPrompt() {
    var cred = get();
    if (cred) return cred;
    return promptLogin();
  }

  /* 带认证的 fetch：先带缓存凭据请求，401 则要求登录后重试一次 */
  async function authFetch(url, opts) {
    opts = opts || {};
    var cred = credOrPrompt();
    if (!cred) throw new Error('auth cancelled');
    var h = opts.headers || {};
    h['Authorization'] = 'Basic ' + cred;
    opts.headers = h;
    var r = await fetch(url, opts);
    if (r.status === 401) {
      clear();
      cred = promptLogin();
      if (!cred) throw new Error('auth cancelled');
      h['Authorization'] = 'Basic ' + cred;
      r = await fetch(url, opts);
    }
    return r;
  }

  window.authFetch = authFetch;
})();
