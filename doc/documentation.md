# Documentation guide

These rules keep Kobalt's Markdown accurate and easy to scan, and stop docs from growing by appending.

## Where docs live

| Path | Owns |
| --- | --- |
| `README.md` | End-user overview and installation. The user owns this file. Do not edit it unless asked. |
| `AGENTS.md` | Cross-cutting rules, safety rules and the map of the repository and docs. Keep it short. Put detailed flags and procedures in `doc/`. |
| `doc/guide.md` | Plain-language guide for users: how Kobalt works, sync, battery, Wi-Fi and limits. No internal names or build details. |
| `doc/building.md` | Build prerequisites, CMake presets, SDKs, Kobo, Android and desktop builds, and formatting. No test inventories or viewer manuals. |
| `doc/reader.md` and `doc/reader/<area>.md` | Reader behavior, controls and feature rules, by area. |
| `doc/storage.md` and `doc/storage/<area>.md` | Saved-data formats, settings and book-state keys, SQLite and journal rules, migrations and caches. |
| `doc/validation.md` and `doc/validation/<area>.md` | Validation commands, guards, fixtures, switches, diagnostics and known gaps, by area. |
| `doc/engines.md` and `doc/engines/` | Blaze architecture and Hybrid performance rules. Mark plans from before Blaze as historical. |
| `doc/release-evaluation.md`, `doc/screen-review.md`, `doc/device-benchmarks.md` | The evaluation harness, the screen review tool and real-device benchmarks. |

All Markdown stays in `doc/`, except the root `README.md`, `AGENTS.md` and `src/engine/blaze/UPSTREAM.md`. `UPSTREAM.md` explains Blaze's changes to CREngine maintainers, who read it without the rest of Kobalt's docs. It stays with the engine source so that it is part of the fork.

When most tasks need only one part of a large doc, split it: `doc/<topic>.md` keeps the index and the core rules, and `doc/<topic>/<area>.md` holds each area.

## Updating docs

- Edit in place. Put a new setting, field or rule in the section or table where it belongs. Do not add notes above a file's introduction or its `Label` line, and do not add unrelated items to the end of another section.
- Do not add entries that say nothing changed. If a change leaves storage keys, schemas and cache policy as they are, `doc/storage/` gets no new text.
- In `doc/validation/<area>.md`, keep the `Label` line at the top. Record only what the test code does not show: the area's scope, environment variables and capture switches, diagnostic events, firmware notes and known gaps. Describe what one test checks in a one-line comment above that test.
- Do not record fixed totals of tests, cases, assertions or screenshots. `ctest --preset mac -N` gives the current inventory. Keep meaningful fixture sizes, limits and measured durations.
- Describe what the code does now, not release stages. Avoid text such as "comes in beta 7". Say when a feature is hidden or gated. Name an old release only when it marks a migration or cache boundary, such as importing Beta 3 JSON or the beta 7 cache token.
- Keep one source of truth. Update a command, preset or procedure in the doc that owns it, and link to it from other files instead of copying it.
- Do not copy values that change often, such as cache versions. Name the file that holds them.

## Format

- Use descriptive headings where they help. A short paragraph can introduce a feature. Use lists by default for feature behavior, settings, defaults, limits and exceptions.
- Start bullets with the feature or setting name readers would search for. Keep directly related conditions and exceptions together. Give independent facts their own bullets and break long items into subitems.
- Use numbered lists when order matters. Use tables for comparable entries that share the same fields, such as settings keys, command options and presets.
- Keep paragraphs short, under about 100 words. Use prose for brief introductions and explanations that need a narrative.
- Do not start a bullet with a bold label and a colon. Write the rule as a short sentence.
- Use plain words, not shorthand such as "max", "incl." or "&". Do not use em dashes.
- Keep headings stable. Do not put code names in parentheses in a heading that is a link target. Write `## Saved words`, not ``## Saved words (`saved_words`)``.
- After moving, splitting or renaming a doc or heading, update every link and anchor that points to it, including in `AGENTS.md` and index tables. Check that no file is left without a link.
