/** Markdown output with turndown: ATX headings, fenced code, "-" bullets, pipe tables. */
import TurndownService from 'turndown';
import type { Article, BuildOptions, OutputFile } from '../types';
import { sanitizeFilename } from './filename';
import { renderBodyHtml } from './html';
import { estimateReading } from './reading';
import { collapseWs, coverLines, sourceNoteText } from './util';

function cellText(cell: Element): string {
  return collapseWs(cell.textContent ?? '').replace(/\|/g, '\\|');
}

function makeTurndown(): TurndownService {
  const td = new TurndownService({
    headingStyle: 'atx',
    codeBlockStyle: 'fenced',
    bulletListMarker: '-',
    hr: '---',
    emDelimiter: '*',
    strongDelimiter: '**',
  });
  td.addRule('strike', {
    filter: ['del', 's'],
    replacement: (content) => `~~${content}~~`,
  });
  td.addRule('table', {
    filter: 'table',
    replacement: (_content, node) => {
      const table = node as HTMLTableElement;
      const rows = Array.from(table.querySelectorAll('tr'));
      if (rows.length === 0) return '';
      const grid = rows.map((r) => Array.from(r.children).map((c) => cellText(c)));
      const cols = Math.max(...grid.map((r) => r.length), 1);
      const line = (cells: string[]) =>
        `| ${Array.from({ length: cols }, (_, i) => cells[i] ?? '').join(' | ')} |`;
      const out = [line(grid[0]), `|${' --- |'.repeat(cols)}`, ...grid.slice(1).map(line)];
      const cap = table.querySelector('caption');
      const capText = cap ? `*${collapseWs(cap.textContent ?? '')}*\n\n` : '';
      return `\n\n${capText}${out.join('\n')}\n\n`;
    },
  });
  // A <pre> without <code> is still a code block.
  td.addRule('pre-plain', {
    filter: (node) => node.nodeName === 'PRE' && !node.querySelector('code'),
    replacement: (_content, node) => {
      const text = (node.textContent ?? '').replace(/\n+$/, '');
      return `\n\n\`\`\`\n${text}\n\`\`\`\n\n`;
    },
  });
  td.addRule('img-alt', {
    filter: (node) => (node as Element).classList?.contains('img-alt') ?? false,
    replacement: (content) => `\n\n*[Image: ${content.trim()}]*\n\n`,
  });
  return td;
}

export function buildMarkdown(article: Article, opts: BuildOptions): OutputFile {
  const title = collapseWs(article.title ?? '') || 'Untitled';
  const parts: string[] = [`# ${title.replace(/^#+\s*/, '')}`];
  if (opts.includeCover) {
    const lines = coverLines(article).map((l) => `**${l.label}:** ${l.value}`);
    if (lines.length) parts.push(lines.join('  \n'));
  }
  const container = renderBodyHtml(article, opts);
  const body = makeTurndown().turndown(container as HTMLElement).trim();
  if (body) parts.push(body);
  if (opts.includeSourceNote) parts.push(`---\n\n*${sourceNoteText(article)}*`);
  const md = `${parts.join('\n\n')}\n`;
  return {
    name: `${sanitizeFilename(article.title)}.md`,
    mime: 'text/markdown;charset=utf-8',
    blob: new Blob([md], { type: 'text/markdown;charset=utf-8' }),
    format: 'md',
    estimatedPages: estimateReading(article, opts.style).pages,
  };
}
