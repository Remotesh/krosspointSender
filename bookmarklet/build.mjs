#!/usr/bin/env node
/*
 * Build the bookmarklet.
 * Usage: node bookmarklet/build.mjs [appUrl]
 * appUrl is the address of the k-rosspointSender app, for example
 * https://k-engine.org/misc/krosspoint/ . The default is "./".
 * Output: bookmarklet/bookmarklet.txt (one javascript: URL).
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

/** Remove comments and collapse whitespace. Strings stay as they are. */
export function minify(src) {
  let out = '';
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    const next = src[i + 1];
    if (c === "'" || c === '"' || c === '`') {
      // Copy a string literal.
      let j = i + 1;
      while (j < src.length && src[j] !== c) j += src[j] === '\\' ? 2 : 1;
      out += src.slice(i, j + 1);
      i = j + 1;
    } else if (c === '/' && next === '*') {
      const end = src.indexOf('*/', i + 2);
      i = end < 0 ? src.length : end + 2;
      out += ' ';
    } else if (c === '/' && next === '/') {
      const end = src.indexOf('\n', i);
      i = end < 0 ? src.length : end;
    } else if (/\s/.test(c)) {
      let j = i;
      while (j < src.length && /\s/.test(src[j])) j++;
      const prev = out[out.length - 1] ?? '';
      const after = src[j] ?? '';
      // Keep one space only between two word characters.
      if (/[\w$]/.test(prev) && /[\w$]/.test(after)) out += ' ';
      i = j;
    } else {
      out += c;
      i += 1;
    }
  }
  return out.trim();
}

/** Return the minified code with the app URL filled in. */
export function bookmarkletCode(appUrl = './') {
  const src = readFileSync(join(here, 'bookmarklet.js'), 'utf8');
  const safe = appUrl.replace(/\\/g, '\\\\').replace(/'/g, "\\'");
  return minify(src).replace(/__APP_URL__/g, () => safe);
}

/** Return the javascript: URL for an app URL. */
export function buildBookmarklet(appUrl = './') {
  return 'javascript:' + encodeURIComponent(bookmarkletCode(appUrl));
}

// Run only from the command line, not on import.
if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  const appUrl = process.argv[2] || './';
  const url = buildBookmarklet(appUrl);
  writeFileSync(join(here, 'bookmarklet.txt'), url + '\n', 'utf8');
  console.log(`Wrote bookmarklet/bookmarklet.txt (${url.length} chars) for app URL ${appUrl}`);
}
