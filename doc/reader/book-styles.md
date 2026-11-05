# Book styles

## Experimental book normalization

Settings > Styling includes Book normalization (experimental). Its modal alert offers Recommended, Full and Custom presets with checkboxes for each rule. Recommended preserves the existing defaults: use Kobalt page margins and reduce chapter-start spacing, with opening-letter normalization, paragraph-gap removal and first-line indentation off. Full enables every rule. Changing a checkbox selects Custom. Unchecking every rule in Custom turns normalization off. The rules apply to all books. Cancel discards modal edits; Done keeps them in Settings. Closing Settings saves and applies the choices.

When selected, the paragraph rules give recognized ordinary paragraphs a 1.2 em first-line indent and remove their bottom gaps. Text-initial normalization is independent of these paragraph rules. Recognized text initials and CSS first letters lose their enlarged size and float wherever they appear, including after a subtitle, in short paragraphs and in quotations. Characters and image-based initials stay unchanged. Explicit paragraph spacing and indentation settings take precedence. The paragraph rules leave headings, quotations, verse, tables, right-aligned text, hanging indents and paragraphs with leading gaps unchanged. Short or uncertain paragraphs also keep their spacing and indentation. Recognition does not cover every publisher convention.

Kobo spans and their IDs stay intact, including in kepubify conversions. The original EPUB is never rewritten. Turning the option off restores the current Kobalt styling. Changing the mode reflows pages while retaining the source reading anchor.


Book normalization includes independent Use Kobalt page margins and Reduce chapter-start spacing rules. The first overrides HTML and body margins and padding so the Layout margins apply. The second removes margins at forced page breaks. Space above illustrations at chapter starts is preserved. Both rules are on in Recommended and Full. Turn either off in Custom to keep that publisher styling. The current book reflows without losing its reading position. Text and Layout controls still apply. CSS `@page` rules remain unsupported by the renderer.

## Custom book styles

`userstyle.css` adds your own CSS to every book. Use it for changes that the Text and Layout panels do not offer, such as paragraph indentation or heading alignment. It changes book content only, not the reader interface.

### Where the file goes

Kobalt does not create the file or ship a default. Create it as a plain text file:

| Platform | Location |
|---|---|
| Kobo | `.adds/kobalt/userstyle.css` on the Kobo drive |
| macOS development app | `~/Library/Application Support/Kobalt/Kobalt/config/userstyle.css`, beside `reader-settings.json` |
| Android | The app-private `config/` directory. There is no in-app file picker or editor. |

Updates keep the file. It is not part of the asset manifest, so editing or deleting it does not affect installation checks.

### When changes apply

Kobalt reads the file each time a book opens. After an edit, close the book and open it again. A changed file also discards cached layouts that were made with other CSS. Appearance previews use the same CSS as the open book. To stop using custom CSS, delete the file or leave it empty.

### How your rules combine with the book's

Your rules go into Kobalt's base stylesheet, after Kobalt's built-in styles and Text settings. CREngine applies them in this order:

1. Ordinary rules in your file rank below the book's own styles. They apply only where the publisher sets nothing.
2. Rules marked `!important` rank above ordinary publisher styles. Use a selector that is at least as specific as the book's.
3. Some Text and Layout settings are already `!important`: line spacing, page margins while Reduce publisher spacing is on, alignment when it is not Publisher, hyphenation when it is On or Off, and the font family when a reading font other than Publisher Fonts is chosen. Line spacing is set on each text block, including table cells and captions, so it follows that block's own text size. To change those, your rule needs `!important` and enough specificity too. Alignment is set on marked text blocks with an attribute selector, so `p { text-align: left !important; }` is not specific enough; set Alignment to Publisher instead, or use a selector such as `p[data-kobalt-prose]`.

When a reading font is chosen, it replaces every font except code and MathML unless your CSS sets the font itself.

### Examples

```css
/* Remove paragraph indentation. */
p { text-indent: 0 !important; }

/* Add space between paragraphs. */
p { margin-top: 0.5em !important; margin-bottom: 0.5em !important; }

/* Centre chapter headings. */
h1, h2 { text-align: center !important; }
```

### Limits

The file must be at most 256 KiB and must not contain NUL bytes. Kobalt ignores a missing, unreadable or oversized file, and the book still opens. Rules use the CSS that CREngine supports. CREngine handles CSS syntax errors itself; they never cause an installation error. Font fallbacks stay built into the app and cannot be changed here.
