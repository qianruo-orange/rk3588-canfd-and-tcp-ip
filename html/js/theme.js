/* theme.js — 深/浅主题切换：data-theme + localStorage 持久化（默认深色） */
(function() {
  var t = 'dark';
  try { t = localStorage.getItem('theme') || 'dark'; } catch(e) {}
  document.documentElement.setAttribute('data-theme', t);
})();

function updateThemeBtn() {
  var b = document.getElementById('themeBtn');
  if (b) b.textContent = document.documentElement.getAttribute('data-theme') === 'light' ? '\u2600\ufe0f 主题' : '\ud83c\udf19 主题';
}

function toggleTheme() {
  var d = document.documentElement;
  var n = d.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
  d.setAttribute('data-theme', n);
  try { localStorage.setItem('theme', n); } catch(e) {}
  updateThemeBtn();
}

document.addEventListener('DOMContentLoaded', updateThemeBtn);
