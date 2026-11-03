# Full-text search over sessions

Agreed with the desktop client (session-7b7ac6d9). This supersedes the README
line that said search would be `rg` on demand and never an index: the owner
asked for FTS, and the owner outranks the design note.

## Method

`session.search_text {query, session?, limit?, before?}` →
`{hits: [{session, seq, role, text, snippet, score, ts}], next, total}`

- **Indexed text**: `session.prompted` (role `user`) and merged `text.delta`
  (role `assistant`). Tool events are noise and stay out; `role` also admits
  `system` for the rules and time lines.
- **`limit`** defaults to 50, capped at 200. **`before`** is a `seq` cursor: hits
  strictly below it, so paging does not depend on offsets.
- **`snippet`** is a bounded excerpt around the match, not the whole message.
- **`next`** is the cursor for the following page, absent at the end; **`total`**
  is the match count for the query, not for the page.
- `session.search` keeps its name/id meaning. The new method is additive, so no
  protocol bump.

## Storage

An FTS5 table in the existing store, not a second database:

```
CREATE VIRTUAL TABLE search USING fts5(text, session UNINDEXED, seq UNINDEXED,
                                       role UNINDEXED, ts UNINDEXED);
```

Rows are written from `wd_session_emit` — the one place every durable event
passes — so the index cannot drift from the log. Reindexing an existing state
directory is a one-shot migration that walks the events table; it must be
idempotent and must not run while a turn is writing.

## What it must not do

- No reindex on every query.
- No unbounded scan: `limit` and the `before` cursor are the only paging.
- No indexing of tool payloads, which would put model-written text into the
  searchable corpus.

## Gate

A session with three known messages, then: a query matching one returns one hit
with the right role and a snippet; a query matching nothing returns an empty
list and `total: 0`; `before` pages backwards without repeating a hit; `limit`
above 200 clamps instead of scanning.

None of this is built yet. The method is specified here so the client can wire
its search box against a fixed shape, and so the next session does not
re-derive the indexing rules.
