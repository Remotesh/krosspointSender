/*
 * K-rosspointSender bookmarklet (readable source).
 * build.mjs makes the javascript: URL from this file.
 * Rules for this file: no line comments inside the code, and no "//" in strings.
 * The minifier keeps strings, but it removes block comments.
 *
 * What it does:
 * 1. It reads the page HTML, or the selected part of the page.
 * 2. It makes a payload: { v: 1, url, title, html, capturedAt }.
 * 3. When the base64 payload is smaller than 1.5 MB, it opens the app with
 *    "#payload=<base64>" in the address.
 * 4. Else it copies the JSON to the clipboard and tells the user to paste it.
 */
(function () {
  var APP = '__APP_URL__';
  var MAX = 1.5 * 1024 * 1024;
  var html = '';
  try {
    var sel = window.getSelection ? window.getSelection() : null;
    if (sel && sel.rangeCount > 0 && String(sel).trim().length > 200) {
      var box = document.createElement('div');
      for (var i = 0; i < sel.rangeCount; i++) {
        box.appendChild(sel.getRangeAt(i).cloneContents());
      }
      html = '<html><head><title></title></head><body><article>' + box.innerHTML + '</article></body></html>';
    }
  } catch (e) {
    html = '';
  }
  if (!html) {
    html = document.documentElement.outerHTML;
  }
  var payload = {
    v: 1,
    url: location.href,
    title: document.title,
    html: html,
    capturedAt: new Date().toISOString()
  };
  var json = JSON.stringify(payload);
  var b64 = '';
  try {
    var bytes = new TextEncoder().encode(json);
    var bin = '';
    var CHUNK = 32768;
    for (var j = 0; j < bytes.length; j += CHUNK) {
      bin += String.fromCharCode.apply(null, bytes.subarray(j, j + CHUNK));
    }
    b64 = btoa(bin);
  } catch (e) {
    b64 = '';
  }
  if (b64 && b64.length < MAX) {
    var target = APP + '#payload=' + encodeURIComponent(b64);
    var w = window.open(target, '_blank');
    if (!w) {
      location.href = target;
    }
    return;
  }
  var done = function () {
    alert('K-rosspointSender: the page is too big for a link. The page data is on your clipboard. Open K-rosspointSender, select the Bookmarklet tab, and paste it.');
    window.open(APP, '_blank');
  };
  var fail = function () {
    alert('K-rosspointSender: the page is too big, and the clipboard is not available. Save the page (Ctrl+S) and upload the .html file instead.');
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(json).then(done, fail);
  } else {
    fail();
  }
})();
