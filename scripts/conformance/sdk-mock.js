// A stand-in for TelegramWidget per docs/sdk-v1.md, injected only when build.ts
// renders the client-SDK group (visual/sdk) natively for its reference pictures:
// the widgets then show their "SDK present" state. It checks arguments as the
// contract says and does nothing else (no dialogs, no haptics). The palette is a
// reference palette for the pictures, not any client's.
(function () {
  var P = {
    light: { bg_color: '#ffffff', text_color: '#000000', hint_color: '#707579', link_color: '#3390ec', button_color: '#3390ec', button_text_color: '#ffffff', secondary_bg_color: '#f4f4f5', accent_text_color: '#3390ec', subtitle_text_color: '#707579', destructive_text_color: '#e53935', section_bg_color: '#ffffff', section_header_text_color: '#3390ec', section_separator_color: '#e7e7e7', header_bg_color: '#ffffff', bottom_bar_bg_color: '#ffffff' },
    dark: { bg_color: '#212121', text_color: '#ffffff', hint_color: '#aaaaaa', link_color: '#8774e1', button_color: '#8774e1', button_text_color: '#ffffff', secondary_bg_color: '#181818', accent_text_color: '#8774e1', subtitle_text_color: '#aaaaaa', destructive_text_color: '#ff595a', section_bg_color: '#212121', section_header_text_color: '#8774e1', section_separator_color: '#3d3d3d', header_bg_color: '#212121', bottom_bar_bg_color: '#212121' },
  };
  var scheme = matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  var params = Object.freeze(P[scheme]);
  for (var k in params) document.documentElement.style.setProperty('--tg-theme-' + k.replace(/_/g, '-'), params[k]);
  document.documentElement.style.setProperty('--tg-color-scheme', scheme);
  // The pictures show the widget on the bubble's background, as in a chat.
  document.documentElement.style.background = params.bg_color;
  var TG = /^(t\.me|telegram\.me|telegram\.dog)$/i;
  function url(u, telegram) {
    if (typeof u !== 'string' || u.length > 256) throw new TypeError('URL must be a string of at most 256 characters');
    var m = /^https:\/\/([^/?#@]+)(?:[/?#]|$)/i.exec(u);
    if (!m || /@/.test(u.split(/[/?#]/)[2] || '')) throw new TypeError('URL must be an absolute https URL without user info');
    if (TG.test(m[1]) !== telegram) throw new TypeError(telegram ? 'openTelegramLink takes t.me links' : 'openLink does not take t.me links');
  }
  var H = {};
  var check = function (v, list) { if (list.indexOf(v) < 0) throw new TypeError('unknown haptic ' + v); return H; };
  H.impactOccurred = function (s) { return check(s, ['light', 'medium', 'heavy', 'rigid', 'soft']); };
  H.notificationOccurred = function (t) { return check(t, ['error', 'success', 'warning']); };
  H.selectionChanged = function () { return H; };
  window.TelegramWidget = {
    profile: 1, isProfileAtLeast: function (n) { return typeof n === 'number' && n <= 1; },
    colorScheme: scheme, theme: scheme, themeParams: params, isActive: true,
    onEvent: function () {}, offEvent: function () {}, onTheme: function (cb) { if (typeof cb === 'function') cb(scheme); }, ready: function () {},
    openLink: function (u) { url(u, false); }, openTelegramLink: function (u) { url(u, true); },
    HapticFeedback: H,
  };
})();
