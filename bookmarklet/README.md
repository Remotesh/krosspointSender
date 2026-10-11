# K-rosspointSender bookmarklet

Most sites block the app from reading them (CORS). The bookmarklet runs on
the article page itself, so the page cannot block it.

## Build

```
node bookmarklet/build.mjs https://k-engine.org/misc/krosspoint/
```

The script writes `bookmarklet/bookmarklet.txt`. The file holds one
`javascript:` URL. Without an argument the app URL is `./`. A relative URL
works only when the article is on the same site as the app, so give the
absolute app URL for normal use.

## Install

1. Show the bookmarks bar (Ctrl+Shift+B).
2. Make a new bookmark. Name it "Send to X4".
3. Paste the content of `bookmarklet.txt` into the URL field.

## Use

1. Open an article in a normal tab.
2. Optional: select the part of the page that you want.
3. Click the bookmark.

The bookmarklet sends the page HTML, URL, title and date. When the data is
smaller than 1.5 MB, it opens the app with `#payload=<base64 JSON>`. The
app reads the payload on load. The data stays in the address hash. The
browser does not send the hash to a server.

When the page is bigger, the bookmarklet copies the JSON to the clipboard.
Open the app, select the Bookmarklet tab, and paste.

## Limits

- Some sites (strict Content Security Policy) block bookmarklets. Then save
  the page (Ctrl+S) and upload the `.html` file.
- A pop-up blocker can stop the new tab. The bookmarklet then opens the app
  in the same tab.
