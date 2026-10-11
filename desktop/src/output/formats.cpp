// HTML, TXT, Markdown and ZIP outputs (port of converters/html.ts,
// text.ts, markdown.ts, zip.ts).
#include "output/internal.h"

#include <nlohmann/json.hpp>

namespace ks::output {

using namespace detail;

namespace {

// Extra rules for a desktop browser window. The EPUB does not get these.
const char* kScreenCss =
    "\n@media screen and (min-width: 40em) {\n"
    "  body { max-width: 36em; margin-left: auto; margin-right: auto; padding: 1em; }\n"
    "}\n";

OutputFile textFile(const std::string& name, const std::string& mime, std::string bytes, OutputFormat f, int pages) {
    OutputFile out;
    out.name = name;
    out.mime = mime;
    out.bytes = std::move(bytes);
    out.format = f;
    out.estimatedPages = pages;
    return out;
}

std::string titleOf(const Article& a) {
    std::string t = collapseWs(a.title);
    return t.empty() ? "Untitled" : t;
}

std::string underline(const std::string& t, char c) { return std::string(std::max<size_t>(3, cpLength(t)), c); }

// A "</style" inside custom CSS would end the element. Escape it.
std::string escapeStyleEnd(const std::string& css) {
    std::string out;
    for (size_t i = 0; i < css.size(); ++i) {
        if (css[i] == '<' && i + 7 <= css.size() && css[i + 1] == '/' && toLower(css.substr(i + 2, 5)) == "style") {
            out += "<\\/";
            ++i;  // skip '/'
            continue;
        }
        out += css[i];
    }
    return out;
}

} // namespace

OutputFile buildHtml(const Article& a, const BuildOptions& o) {
    const std::string title = titleOf(a);
    const std::string lang = langOf(a);

    Doc doc(xmlNewDoc(BAD_CAST "1.0"));
    xmlNodePtr body = xmlNewDocNode(doc.get(), nullptr, BAD_CAST "body", nullptr);
    xmlDocSetRootElement(doc.get(), body);
    if (o.includeCover) {
        xmlNodePtr cover = addEl(body, nullptr, "header", "cover");
        appendTitleBlock(cover, nullptr, title, coverLines(a));
        addEl(body, nullptr, "hr");
    } else {
        appendTitleBlock(body, nullptr, title, {});
    }
    xmlNodePtr main = addEl(body, nullptr, "main");
    renderBodyInto(a, o, main);
    if (o.includeSourceNote) appendSourceNote(body, nullptr, sourceNoteText(a));

    std::string html = "<!DOCTYPE html>\n<html lang=\"" + escapeXml(lang) + "\"><head><meta charset=\"utf-8\"><title>" +
                       escapeXml(title) +
                       "</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                       "<meta name=\"generator\" content=\"K-rosspointSender\"><style>" +
                       escapeStyleEnd(epubCss(o.style) + kScreenCss) + "</style></head>";
    serializeHtml(body, html);
    html += "</html>\n";
    return textFile(sanitizeFilename(a.title) + ".html", "text/html;charset=utf-8", std::move(html), OutputFormat::Html,
                    estimateReading(a, o.style).pages);
}

OutputFile buildTxt(const Article& a, const BuildOptions& o) {
    const std::string title = titleOf(a);
    std::vector<std::string> parts = {title + "\n" + underline(title, '=')};
    if (o.includeCover) {
        std::string lines;
        for (const Line& l : coverLines(a)) lines += (lines.empty() ? "" : "\n") + l.label + ": " + l.value;
        if (!lines.empty()) parts.push_back(lines);
    }
    // Keep every <img> as an element, so the text shows "[Image: alt]".
    CopyContext ctx;
    ctx.image = [](const std::string& src, const std::string&) -> std::optional<std::string> {
        return src.empty() ? std::string("image") : src;
    };
    ctx.href = defaultHref;
    Rendered r = renderBody(a, o, &ctx);
    std::string body = htmlToText(r.container, o.style.keepLinks, o.includeImages);
    if (!body.empty()) parts.push_back(body);
    if (o.includeSourceNote) parts.push_back("* * *\n\n" + sourceNoteText(a));
    std::string text;
    for (size_t i = 0; i < parts.size(); ++i) text += (i ? "\n\n" : "") + parts[i];
    text += "\n";
    return textFile(sanitizeFilename(a.title) + ".txt", "text/plain;charset=utf-8", std::move(text), OutputFormat::Txt,
                    estimateReading(a, o.style).pages);
}

OutputFile buildMarkdown(const Article& a, const BuildOptions& o) {
    std::string title = titleOf(a);
    // Remove a leading "#" run, so the heading stays one heading.
    size_t k = 0;
    while (k < title.size() && title[k] == '#') ++k;
    if (k) title = jsTrim(title.substr(k));
    std::vector<std::string> parts = {"# " + title};
    if (o.includeCover) {
        std::string lines;
        for (const Line& l : coverLines(a)) lines += (lines.empty() ? "" : "  \n") + ("**" + l.label + ":** " + l.value);
        if (!lines.empty()) parts.push_back(lines);
    }
    Rendered r = renderBody(a, o);
    std::string body = jsTrim(htmlToMarkdown(r.container));
    if (!body.empty()) parts.push_back(body);
    if (o.includeSourceNote) parts.push_back("---\n\n*" + sourceNoteText(a) + "*");
    std::string md;
    for (size_t i = 0; i < parts.size(); ++i) md += (i ? "\n\n" : "") + parts[i];
    md += "\n";
    return textFile(sanitizeFilename(a.title) + ".md", "text/markdown;charset=utf-8", std::move(md), OutputFormat::Md,
                    estimateReading(a, o.style).pages);
}

std::string detail::articleJson(const Article& a) {
    nlohmann::ordered_json j;
    j["id"] = a.id;
    j["title"] = a.title;
    // Optional fields: left out when empty, like JSON.stringify() with undefined.
    auto opt = [&](const char* key, const std::string& v) {
        if (!v.empty()) j[key] = v;
    };
    opt("author", a.author);
    opt("publishedAt", a.publishedAt);
    j["capturedAt"] = a.capturedAt;
    opt("sourceUrl", a.sourceUrl);
    opt("siteName", a.siteName);
    opt("excerpt", a.excerpt);
    opt("language", a.language);
    j["tags"] = a.tags;
    j["bodyHtml"] = a.bodyHtml;
    nlohmann::ordered_json toc = nlohmann::ordered_json::array();
    for (const TocEntry& t : a.toc) toc.push_back({{"id", t.id}, {"level", t.level}, {"text", t.text}});
    j["toc"] = toc;
    nlohmann::ordered_json imgs = nlohmann::ordered_json::array();
    for (const ArticleImage& im : a.images) {
        nlohmann::ordered_json e;
        e["src"] = im.src;
        if (!im.alt.empty()) e["alt"] = im.alt;
        imgs.push_back(e);
    }
    j["images"] = imgs;
    j["wordCount"] = a.wordCount;
    j["inputKind"] = toString(a.inputKind);
    return j.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

OutputFile buildZip(const Article& a, const BuildOptions& o, const ImageMap& images) {
    const std::string base = sanitizeFilename(a.title);
    int pages = 1;
    const std::string epub = buildEpubBytes(a, o, images, &pages);
    const std::string html = buildHtml(a, o).bytes;
    const std::string txt = buildTxt(a, o).bytes;
    const std::string md = buildMarkdown(a, o).bytes;
    const std::string json = articleJson(a);
    std::vector<ZipEntry> e = {
        {base + ".epub", &epub, true},
        {base + ".html", &html, false},
        {base + ".txt", &txt, false},
        {base + ".md", &md, false},
        {"article.json", &json, false},
    };
    return textFile(base + ".zip", "application/zip", zipPack(e), OutputFormat::Zip, pages);
}

} // namespace ks::output
