/**
 * Bookmarklet tab. It shows an install link and a box to paste a payload.
 * The link is built at run time. It points back to this page.
 */
import type { BookmarkletPayload } from '../types';
import { parseBookmarkletPayload } from '../extractors';
import { errorMessage, field, h } from './dom';
import { toast } from './toast';

/** Max hash size before the capture script falls back to the clipboard. */
const HASH_LIMIT = 1_500_000;

/**
 * Minimal capture script. It reads the page, makes a payload and opens
 * the app with the payload in the hash. Large pages go to the clipboard.
 */
function captureScript(appUrl: string): string {
  return `(function(){var d=document,s=window.getSelection&&String(window.getSelection())?(function(){var r=window.getSelection().getRangeAt(0),c=d.createElement('div');c.appendChild(r.cloneContents());return '<html><head><title>'+d.title+'</title></head><body><article>'+c.innerHTML+'</article></body></html>';})():d.documentElement.outerHTML;var p={v:1,url:location.href,title:d.title,html:s,capturedAt:new Date().toISOString()};var j=JSON.stringify(p),b;try{b=btoa(unescape(encodeURIComponent(j)));}catch(e){b='';}var a=${JSON.stringify(appUrl)};if(b&&b.length<${HASH_LIMIT}){window.open(a+'#payload='+b,'_blank');}else{var done=function(){alert('k-rosspointSender: the page is large. The payload is on the clipboard. Paste it in the Bookmarklet tab.');window.open(a,'_blank');};if(navigator.clipboard){navigator.clipboard.writeText(j).then(done,function(){prompt('Copy this payload:',j);});}else{prompt('Copy this payload:',j);}}})();`;
}

/** Build the javascript: URL for the current app location. */
export function bookmarkletHref(): string {
  const appUrl = location.href.split('#')[0];
  return 'javascript:' + encodeURIComponent(captureScript(appUrl));
}

/** Render the bookmarklet tab. onPayload gets a parsed payload. */
export function renderBookmarkletPanel(onPayload: (p: BookmarkletPayload) => void): HTMLElement {
  const link = h(
    'a',
    {
      class: 'bookmarklet-link',
      href: bookmarkletHref(),
      title: 'Drag this link to your bookmarks bar',
      onclick: (e: Event) => {
        e.preventDefault();
        toast('Drag this link to your bookmarks bar. Then click it on an article page.', 'info');
      },
    },
    'Send to K-rosspoint',
  );
  const box = h('textarea', {
    rows: 6,
    placeholder: 'Paste a bookmarklet payload (JSON or base64) here',
    spellcheck: false,
  });
  const load = (): void => {
    const raw = box.value.trim();
    if (!raw) {
      toast('The payload box is empty.', 'warn');
      return;
    }
    try {
      onPayload(parseBookmarkletPayload(raw));
      box.value = '';
    } catch (err) {
      toast(`This payload is not valid: ${errorMessage(err)}`, 'error');
    }
  };
  const fileUrl = location.protocol === 'file:';
  return h(
    'div',
    { class: 'tab-body' },
    h(
      'p',
      { class: 'hint' },
      'The bookmarklet captures the page you read, with your login and cookies. ',
      'It then opens this app with the page. Nothing goes to a server.',
    ),
    h('p', { class: 'center' }, link),
    fileUrl
      ? h(
          'p',
          { class: 'hint warn' },
          'This app runs from a file:// URL. Some browsers do not let a web page open file:// links. ',
          'If the link does not open the app, use the paste box below.',
        )
      : null,
    field('Paste payload', box),
    h('button', { type: 'button', class: 'btn primary', onclick: load }, 'Load payload'),
  );
}
