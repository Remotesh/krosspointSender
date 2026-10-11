import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
const APP = process.argv[2]; const DEVPORT = process.argv[3]; const OUT = process.argv[4];
const FIX = new URL('../tests/fixtures/', import.meta.url).pathname.replace(/^/([A-Za-z]:)/, '$1');
fs.mkdirSync(OUT, { recursive: true });
let browser;
try { browser = await chromium.launch(); } catch { browser = await chromium.launch({ channel: 'msedge' }); }
const ctx = await browser.newContext({ acceptDownloads: true, viewport: { width: 1400, height: 900 } });
const page = await ctx.newPage();
const errors = []; page.on('pageerror', e => errors.push('PAGEERROR ' + e.message));
page.on('console', m => { if (m.type() === 'error') errors.push('CONSOLE ' + m.text()); });
const log = (...a) => console.log('[smoke]', ...a);
await page.goto(APP); await page.waitForTimeout(500);
log('title', await page.title());

// 1. Paste HTML (wikipedia fixture) -> preview
await page.getByRole('tab', { name: /paste html/i }).click();
await page.locator('textarea').first().fill(fs.readFileSync(FIX + 'wikipedia-sample.html', 'utf8'));
await page.locator('input[placeholder^="https://... (optional)"]').fill('https://en.wikipedia.org/wiki/Sample');
await page.getByRole('button', { name: 'Convert' }).first().click();
await page.waitForTimeout(800);
const paperText = await page.locator('.paper').innerText();
log('preview chars', paperText.length, '| infobox present:', paperText.toLowerCase().includes('infobox'));
log('stats:', (await page.locator('.preview-stats').innerText()).replace(/\s+/g, ' '));
await page.screenshot({ path: path.join(OUT, 'preview.png') });

// 2. Download EPUB
const [dl] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Download', exact: true }).click()]);
const epubPath = path.join(OUT, dl.suggestedFilename()); await dl.saveAs(epubPath);
log('downloaded', dl.suggestedFilename(), fs.statSync(epubPath).size, 'bytes');

// 3. File tab: upload article-blog.html
await page.getByRole('tab', { name: /^file/i }).click();
await page.locator('input[type=file]').setInputFiles(FIX + 'article-blog.html');
await page.waitForTimeout(800);
log('blog title in metadata:', await page.locator('input').filter({ hasText: '' }).first().inputValue().catch(() => '?'));
log('blog preview head:', (await page.locator('.paper').innerText()).slice(0, 120).replace(/\n/g, ' | '));

// 4. Live Wikipedia fetch
await page.getByRole('tab', { name: /wikipedia/i }).click();
await page.locator('input[placeholder*="Wikipedia"]').fill('E Ink');
await page.getByRole('button', { name: /fetch from wikipedia/i }).click();
await page.waitForTimeout(8000);
log('wiki status:', (await page.locator('.fetch-status').allInnerTexts()).join(' / '));

// 5. Live URL fetch (CORS-friendly: example.com has no ACAO? try a known CORS site)
await page.getByRole('tab', { name: /^url/i }).click();
await page.locator('input[placeholder="https://example.com/article"]').fill('https://example.com/');
await page.getByRole('button', { name: /^fetch$/i }).click();
await page.waitForTimeout(6000);
log('url status:', (await page.locator('.fetch-status').allInnerTexts()).join(' / '));

// 6. Device wizard -> fake device, test + send test epub + send
await page.getByRole('button', { name: /devices/i }).first().click(); await page.waitForTimeout(300);
await page.screenshot({ path: path.join(OUT, 'wizard.png') });
const dialog = page.locator('[role=dialog], .drawer').last();
await dialog.getByLabel('Name', { exact: true }).fill('Fake X4');
await dialog.getByLabel('Host').fill('127.0.0.1');
await dialog.getByLabel('Port').fill(String(DEVPORT));
const bodyTxt = await dialog.innerText(); log('wizard step1 text head:', bodyTxt.slice(0, 200).replace(/\n/g, ' | '));
await dialog.getByRole('button', { name: /next|save|continue/i }).first().click(); await page.waitForTimeout(300);
log('wizard step2:', (await dialog.innerText()).slice(0, 300).replace(/\n/g, ' | '));
const nextBtns = dialog.getByRole('button', { name: /next|continue/i });
if (await nextBtns.count()) { await nextBtns.first().click(); await page.waitForTimeout(300); }
const testBtn = dialog.getByRole('button', { name: /test connection/i });
if (await testBtn.count()) { await testBtn.first().click(); await page.waitForTimeout(2000); log('after test:', (await dialog.innerText()).slice(0, 400).replace(/\n/g, ' | ')); }
await page.screenshot({ path: path.join(OUT, 'wizard-test.png') });
const nb = dialog.getByRole('button', { name: /next|continue/i });
if (await nb.count()) { await nb.first().click(); await page.waitForTimeout(300); }
const sendTest = dialog.getByRole('button', { name: /send test/i });
if (await sendTest.count()) { await sendTest.first().click(); await page.waitForTimeout(3000); log('after send test:', (await dialog.innerText()).slice(0, 400).replace(/\n/g, ' | ')); }
await page.screenshot({ path: path.join(OUT, 'wizard-send.png') });
await page.keyboard.press('Escape'); await page.waitForTimeout(300);

// 7. Send current article via device panel
await page.getByRole('button', { name: 'Send', exact: true }).click(); await page.waitForTimeout(3000);
log('device panel:', (await page.locator('[aria-live]').allInnerTexts()).join(' / ').slice(0, 300));
await page.screenshot({ path: path.join(OUT, 'sent.png') });

// 8. Library
await page.getByRole('button', { name: /library/i }).first().click(); await page.waitForTimeout(400);
log('library:', (await page.locator('.drawer, [role=dialog]').last().innerText()).slice(0, 400).replace(/\n/g, ' | '));
await page.screenshot({ path: path.join(OUT, 'library.png') });
log('ERRORS:', errors.length ? errors : 'none');
await browser.close();
