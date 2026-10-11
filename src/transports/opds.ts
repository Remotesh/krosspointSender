/**
 * OPDS package. send() builds a ZIP with catalog.xml (an OPDS 1.2 Atom
 * acquisition feed) and the book, then downloads it. Unzip it on any static
 * web server (with HTTP Basic auth if you like). The reader then browses the
 * catalog and downloads the book.
 */
import JSZip from 'jszip';
import {
  TransferError,
  type ConnectionTestResult,
  type DeviceProfile,
  type OutputFile,
  type TransferProgress,
  type TransferResult,
  type TransportAdapter,
} from '../types';
import { runSend, safeEmitter } from './common';
import { downloadBlob } from './download';

export interface OpdsEntry {
  title: string;
  author?: string;
  fileName: string;
  bytes: number;
  /** ISO date. */
  updated: string;
  /** BCP47. Default "en". */
  language?: string;
  /** Acquisition MIME type. Default "application/epub+zip". */
  mime?: string;
}

const NS_ATOM = 'http://www.w3.org/2005/Atom';
const NS_DC = 'http://purl.org/dc/terms/';
const NS_OPDS = 'http://opds-spec.org/2010/catalog';
const ACQ_KIND = 'application/atom+xml;profile=opds-catalog;kind=acquisition';

/** Escapes text for XML. Removes characters that XML 1.0 does not allow. */
export function xmlEscape(s: string): string {
  return String(s ?? '')
    // eslint-disable-next-line no-control-regex
    .replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F￾￿]/g, '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&apos;');
}

function isoDate(s: string | undefined): string {
  const d = s ? new Date(s) : new Date();
  return Number.isNaN(d.getTime()) ? new Date().toISOString() : d.toISOString();
}

function joinHref(baseUrl: string, fileName: string): string {
  const enc = encodeURIComponent(fileName);
  if (!baseUrl) return enc;
  return baseUrl.endsWith('/') ? `${baseUrl}${enc}` : `${baseUrl}/${enc}`;
}

/** Builds an OPDS 1.2 Atom acquisition feed. baseUrl may be '' for relative links. */
export function buildOpdsCatalog(entries: OpdsEntry[], baseUrl: string): string {
  const updated = entries.reduce<string>((latest, e) => {
    const d = isoDate(e.updated);
    return d > latest ? d : latest;
  }, entries.length > 0 ? isoDate(entries[0].updated) : new Date().toISOString());
  const selfHref = joinHref(baseUrl, 'catalog.xml');

  const lines: string[] = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    `<feed xmlns="${NS_ATOM}" xmlns:dc="${NS_DC}" xmlns:opds="${NS_OPDS}">`,
    '  <id>urn:krosspoint:catalog</id>',
    '  <title>k-rosspointSender</title>',
    `  <updated>${updated}</updated>`,
    '  <author><name>k-rosspointSender</name></author>',
    `  <link rel="self" href="${xmlEscape(selfHref)}" type="${ACQ_KIND}"/>`,
    `  <link rel="start" href="${xmlEscape(selfHref)}" type="${ACQ_KIND}"/>`,
  ];
  for (const e of entries) {
    const href = joinHref(baseUrl, e.fileName);
    lines.push(
      '  <entry>',
      `    <id>urn:krosspoint:book:${xmlEscape(encodeURIComponent(e.fileName))}</id>`,
      `    <title>${xmlEscape(e.title || e.fileName)}</title>`,
      `    <author><name>${xmlEscape(e.author || 'Unknown')}</name></author>`,
      `    <updated>${isoDate(e.updated)}</updated>`,
      `    <dc:language>${xmlEscape(e.language || 'en')}</dc:language>`,
      `    <dc:extent>${Math.max(0, Math.round(e.bytes))} bytes</dc:extent>`,
      `    <link rel="http://opds-spec.org/acquisition" href="${xmlEscape(href)}" type="${xmlEscape(e.mime || 'application/epub+zip')}" length="${Math.max(0, Math.round(e.bytes))}"/>`,
      '  </entry>',
    );
  }
  lines.push('</feed>', '');
  return lines.join('\n');
}

function baseName(name: string): string {
  const i = name.lastIndexOf('.');
  return i > 0 ? name.slice(0, i) : name;
}

function titleFromFile(name: string): string {
  return baseName(name).replace(/[_-]+/g, ' ').trim() || name;
}

/** Builds the OPDS ZIP (catalog.xml + the book). */
export async function buildOpdsPackage(
  file: OutputFile,
  meta: { title?: string; author?: string; language?: string } = {},
): Promise<Blob> {
  const zip = new JSZip();
  const catalog = buildOpdsCatalog(
    [
      {
        title: meta.title || titleFromFile(file.name),
        author: meta.author,
        fileName: file.name,
        bytes: file.blob.size,
        updated: new Date().toISOString(),
        language: meta.language,
        mime: file.mime || 'application/epub+zip',
      },
    ],
    '',
  );
  zip.file('catalog.xml', catalog);
  zip.file(file.name, file.blob);
  return zip.generateAsync({ type: 'blob', mimeType: 'application/zip' });
}

export class OpdsTransport implements TransportAdapter {
  readonly method = 'opds' as const;

  isSupported(): boolean {
    return true;
  }

  limitations(): string[] {
    return [
      'This saves a ZIP with catalog.xml and the book. It does not send anything.',
      'Unzip it on a web server in your network. Add the catalog URL to the reader as an OPDS source.',
    ];
  }

  async testConnection(_profile: DeviceProfile): Promise<ConnectionTestResult> {
    return { ok: true, latencyMs: 0, info: { method: 'opds' } };
  }

  send(
    _profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    return runSend(file, onProgress, undefined, async () => {
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
      emit({ state: 'connecting', message: 'Building the OPDS catalog' });
      const blob = await buildOpdsPackage(file);
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
      const zipName = `${baseName(file.name)}-opds.zip`;
      emit({ state: 'uploading', fraction: 1, message: `Downloading ${zipName}` });
      downloadBlob(blob, zipName);
      return zipName;
    });
  }
}

export const opdsTransport = new OpdsTransport();
