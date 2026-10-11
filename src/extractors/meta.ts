/**
 * Read article metadata from <meta> tags, <time> elements and JSON-LD.
 */
import { normText } from './dom';

export interface PageMeta {
  title?: string;
  author?: string;
  publishedAt?: string;
  siteName?: string;
  excerpt?: string;
  language?: string;
  tags: string[];
}

/** Return an ISO date, or undefined when the input is not a date. */
export function toIsoDate(s: string | null | undefined): string | undefined {
  const v = normText(s);
  if (!v) return undefined;
  const t = Date.parse(v);
  if (Number.isNaN(t)) return undefined;
  return new Date(t).toISOString();
}

function metaContent(doc: Document, ...keys: string[]): string | undefined {
  for (const k of keys) {
    const el = doc.querySelector(`meta[property="${k}" i], meta[name="${k}" i], meta[itemprop="${k}" i]`);
    const v = normText(el?.getAttribute('content'));
    if (v) return v;
  }
  return undefined;
}

const ARTICLE_TYPES = /^(Article|NewsArticle|BlogPosting|Report|ScholarlyArticle|TechArticle|ReportageNewsArticle|AnalysisNewsArticle|OpinionNewsArticle|LiveBlogPosting|SocialMediaPosting|Review|Recipe|HowTo)$/;

type Json = Record<string, unknown>;

function typesOf(o: Json): string[] {
  const t = o['@type'];
  return (Array.isArray(t) ? t : [t]).filter((x): x is string => typeof x === 'string');
}

function nameOf(v: unknown): string | undefined {
  if (!v) return undefined;
  if (typeof v === 'string') return normText(v) || undefined;
  if (Array.isArray(v)) {
    const names = v.map(nameOf).filter((x): x is string => !!x);
    return names.length ? Array.from(new Set(names)).join(', ') : undefined;
  }
  if (typeof v === 'object') {
    const n = (v as Json).name;
    return typeof n === 'string' ? normText(n) || undefined : undefined;
  }
  return undefined;
}

/** Find the best article object in the JSON-LD blocks. */
export function readJsonLd(doc: Document): { headline?: string; author?: string; datePublished?: string; publisher?: string; description?: string; inLanguage?: string } {
  const found: Json[] = [];
  const visit = (v: unknown, depth: number) => {
    if (!v || depth > 6) return;
    if (Array.isArray(v)) v.forEach((x) => visit(x, depth + 1));
    else if (typeof v === 'object') {
      const o = v as Json;
      found.push(o);
      if (o['@graph']) visit(o['@graph'], depth + 1);
      if (o.mainEntity && typeof o.mainEntity === 'object') visit(o.mainEntity, depth + 1);
    }
  };
  for (const s of Array.from(doc.querySelectorAll('script[type="application/ld+json" i]'))) {
    try {
      visit(JSON.parse((s.textContent ?? '').trim().replace(/^<!--|-->$/g, '')), 0);
    } catch {
      /* Bad JSON-LD is common. Skip it. */
    }
  }
  const best =
    found.find((o) => typesOf(o).some((t) => ARTICLE_TYPES.test(t))) ??
    found.find((o) => typeof o.headline === 'string') ??
    found.find((o) => typesOf(o).includes('WebPage'));
  if (!best) return {};
  return {
    headline: typeof best.headline === 'string' ? normText(best.headline) : typeof best.name === 'string' ? normText(best.name) : undefined,
    author: nameOf(best.author) ?? nameOf(best.creator),
    datePublished: typeof best.datePublished === 'string' ? best.datePublished : typeof best.dateCreated === 'string' ? best.dateCreated : undefined,
    publisher: nameOf(best.publisher),
    description: typeof best.description === 'string' ? normText(best.description) : undefined,
    inLanguage: typeof best.inLanguage === 'string' ? best.inLanguage : undefined,
  };
}

/** Read metadata from the page. Call this before any change to the document. */
export function readMeta(doc: Document): PageMeta {
  const ld = readJsonLd(doc);
  const metaAuthor = metaContent(doc, 'author', 'article:author', 'dc.creator', 'dcterms.creator', 'parsely-author', 'sailthru.author', 'twitter:creator');
  const relAuthor = normText(doc.querySelector('[rel="author"], [itemprop="author"] [itemprop="name"], [itemprop="author"], .byline .author, .author-name')?.textContent);
  const authorFromMeta = metaAuthor && !/^https?:\/\//i.test(metaAuthor) && !metaAuthor.startsWith('@') ? metaAuthor : undefined;

  const timeEl =
    doc.querySelector('article time[datetime], time[pubdate][datetime], time[itemprop="datePublished"], time.published[datetime], time[datetime]');
  const published =
    toIsoDate(ld.datePublished) ??
    toIsoDate(metaContent(doc, 'article:published_time', 'og:published_time', 'datePublished', 'date', 'dc.date', 'dc.date.issued', 'dcterms.created', 'pubdate', 'publishdate', 'publish-date', 'sailthru.date', 'parsely-pub-date')) ??
    toIsoDate(timeEl?.getAttribute('datetime')) ??
    toIsoDate(doc.querySelector('[itemprop="datePublished"]')?.getAttribute('content'));

  const tags = Array.from(doc.querySelectorAll('meta[property="article:tag" i]'))
    .map((m) => normText(m.getAttribute('content')))
    .filter(Boolean)
    .slice(0, 10);

  const lang = normText(doc.documentElement.getAttribute('lang')) || metaContent(doc, 'og:locale', 'language', 'content-language') || ld.inLanguage;

  return {
    title: ld.headline || metaContent(doc, 'og:title', 'twitter:title', 'dc.title', 'parsely-title') || normText(doc.title) || undefined,
    author: ld.author ?? authorFromMeta ?? (relAuthor && relAuthor.length < 100 ? relAuthor.replace(/^by\s+/i, '') : undefined),
    publishedAt: published,
    siteName: metaContent(doc, 'og:site_name', 'application-name', 'apple-mobile-web-app-title') ?? ld.publisher,
    excerpt: metaContent(doc, 'og:description', 'description', 'twitter:description') ?? ld.description,
    language: lang ? lang.replace('_', '-') : undefined,
    tags: Array.from(new Set(tags)),
  };
}
