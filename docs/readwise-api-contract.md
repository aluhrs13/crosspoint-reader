# Readwise Reader API Contract

The bounded contract CrossPoint relies on when talking to the Readwise Reader API. This document exists so the persisted schemas, sync engine, and UI designed in later phases are built against measured behaviour rather than the published documentation, which this exercise found to be wrong in several places that matter.

Everything marked **verified** was observed against a live Reader account on 2026-08-05 using [scripts/readwise_probe.py](../scripts/readwise_probe.py). Everything marked *documented-only* comes from `readwise.io/reader_api` and has not been exercised here. Fixtures capturing the verified responses live in [test/readwise_contract/fixtures/](../test/readwise_contract/fixtures/) and are guarded by [test/readwise_contract/ReadwiseContractTest.cpp](../test/readwise_contract/ReadwiseContractTest.cpp).

## Headline findings

Four results change what the feature can be. Read these before the reference sections.

1. **`reading_progress` cannot be written.** `PATCH /update/` accepts it, answers `200`, and silently discards it. The device can read a reading position down from Readwise but cannot push one up. Verified against both an empty document and a 3,159-word article, with `title`, `seen`, and `location` writes succeeding in the same sequence as controls.
2. **There are no deletion tombstones.** A deleted document simply disappears. An `updatedAfter` window spanning the deletion does not mention it, and fetching it by `id` returns `count: 0` rather than `404`. Incremental sync structurally cannot observe deletions, so they are only discoverable by a full sweep of the synced locations.
3. **A single article body is 88 KB.** That is roughly a quarter of the device's entire RAM for one field of one document, and a full metadata page is 135 KB. Nothing may be buffered whole.
4. **Today's `StreamingJsonParser` silently drops any string over 512 bytes.** It does not truncate and does not error — the value callback is suppressed. This already affects `summary` (observed to 2,093 bytes), not just `html_content`.

A fifth is worth knowing but does not constrain the MVP: **`count` saturates at 10,000** rather than reporting a true total, so it can never be used to size a sweep or detect completion.

## Authentication

**Verified.** Token in an `Authorization` header, validated against a v2 endpoint:

```http
GET /api/v2/auth/ HTTP/1.1
Host: readwise.io
Authorization: Token <access-token>
```

| Outcome | Status | Body |
|---|---|---|
| Valid token | `204` | empty |
| Invalid token | `401` | `{"detail": "Invalid token."}` |

Users obtain a token from `readwise.io/access_token`. It is a long opaque string with no expiry or refresh mechanism, so the device stores it indefinitely.

**Storage.** Follow the existing credential precedent rather than inventing one: a dedicated `PersistableStore` singleton writing its own file under `/.crosspoint/`, with the token obfuscated at rest via `obfuscation::obfuscateToBase64` from [lib/Serialization/ObfuscationUtils.h](../lib/Serialization/ObfuscationUtils.h). [lib/KOReaderSync/KOReaderCredentialStore.h](../lib/KOReaderSync/KOReaderCredentialStore.h) is the model, including its `cfgVersion` migration hook. Note that the obfuscation is device-bound, not cryptographic.

## Transport

Base URL is `https://readwise.io/api/v3/`, with the auth check on `/api/v2/`. The service sits behind Cloudflare.

Two response properties constrain the client:

- **`Transfer-Encoding: chunked`** — list responses carry no `Content-Length`. `SecureHttpClient::hasContentLength()` will be false, so the client cannot pre-size a buffer or drive a progress bar from a content length. It must consume until the stream ends.
- **`Connection: close`** — no keep-alive, so each request pays a full TLS handshake. At the 20 req/min list ceiling this is the dominant cost of a sweep.

**TLS posture (accepted risk, reaffirmed).** The Readwise client calls `setInsecure()`, matching every existing `SecureHttpClient` call site in [lib/KOReaderSync/KOReaderSyncClient.cpp](../lib/KOReaderSync/KOReaderSyncClient.cpp).

Correction to the original phase-1 record: this document previously stated that no certificate-verification path existed for the wolfSSL backend. That was wrong — it was inferred while the `freeink-sdk` submodule was not checked out. `SecureHttpClient::setCACert(rootPem)` does exist and verifies against a single PEM root. The decision to ship with `setInsecure()` was reaffirmed with that fact known: pinning roots would tie sync availability to Cloudflare's CA rotation, and the first live handshake validation could only happen on hardware. Pinned-root verification remains an available follow-up.

A Readwise token therefore crosses an unverified TLS session and is interceptable by an active MITM on the local network. It is recorded here so the decision is explicit rather than accidental.

**Heap gating.** Every request must be gated on free heap before the handshake, following `MIN_FREE_FOR_TLS` / `MIN_BLOCK_FOR_TLS` in [lib/KOReaderSync/KOReaderSyncClient.cpp](../lib/KOReaderSync/KOReaderSyncClient.cpp), returning a low-memory result rather than attempting the connection.

## Listing documents

```http
GET /api/v3/list/?location=later&limit=100&updatedAfter=2026-08-01T00:00:00Z
```

**Rate limit: 20 requests/minute — verified.**

### Query parameters

| Parameter | Type | Status | Notes |
|---|---|---|---|
| `id` | string | verified | Exact document id. Returns `count: 0` for an unknown or deleted id, never `404`. |
| `updatedAfter` | ISO 8601 | verified | See [Incremental sync](#incremental-sync). |
| `location` | string | verified | See the location values below. |
| `category` | string | *documented-only* | |
| `tag` | string | *documented-only* | Up to 5. |
| `limit` | integer | verified | 1–100, default 100. |
| `pageCursor` | string | verified | Opaque; see [Pagination](#pagination). |
| `withHtmlContent` | boolean | verified | Adds `html_content`. Costs ~88 KB on a long article. |
| `withRawSourceUrl` | boolean | *documented-only* | |

### Response envelope

```json
{ "count": 10000, "nextPageCursor": "01hz…", "results": [ … ] }
```

`count` is the number matching the query across all pages, not the page size.

**`count` saturates at 10,000 — verified.** It is not a true total. Below the cap it is exact; at or above it, it clamps:

| Query | `count` |
|---|---|
| unfiltered | **10000** |
| `location=feed` | **10000** |
| `location=later` | 2275 |
| `location=archive` | 261 |
| `location=shortlist` | 19 |
| `location=new` | 0 |
| *sum of locations* | **12555** |
| `category=rss` | **10000** |
| `category=article` | 1879 |
| *sum of all categories* | **13665** |

The per-location counts sum to more than the unfiltered total, which is only possible if the total is clamped. Two unrelated queries landing on exactly 10,000 while every smaller query returns a precise, non-round number confirms it.

**Consequences.** Never use `count` to size a sweep, to compute progress, or to decide that pagination is complete — only a `null` `nextPageCursor` means complete. A `count` of exactly 10,000 should be read as "10,000 or more".

**Working past the cap.** Slicing a capped set by `updatedAfter` returns exact sub-counts (`feed` over the last 7/30/365 days: 108 / 1120 / 6849), so a set larger than the cap can be enumerated as a series of time windows. This matters only if `feed` is ever synced; see [Reconciliation cost](#reconciliation-cost).

### Document fields

Observed across 200 real documents. "Max bytes" is the largest UTF-8 encoding seen in that sample and is a guide, not a documented ceiling — do not treat these as safe fixed buffer sizes without a bounds check.

| Field | Type | Nullable | Max bytes | Notes |
|---|---|---|---|---|
| `id` | string | no | 26 | ULID. Fixed width in every sample. |
| `url` | string | no | 56 | The `read.readwise.io` reader URL, not the source. |
| `source_url` | string | **yes** | 256 | The original article URL. |
| `title` | string | **yes** | 318 | |
| `author` | string | **yes** | 68 | |
| `source` | string | **yes** | 26 | e.g. `Reader RSS`, `Reader Share Sheet iOS`. |
| `category` | string | no | 9 | `rss`, `article`, `tweet`, `email`, `highlight`, `pdf`, `video`. |
| `location` | string | **yes** | 9 | See below. |
| `tags` | **object** | no | — | Empty `{}` throughout the sample. The docs describe a list; the wire format is an object. |
| `site_name` | string | **yes** | 40 | |
| `word_count` | integer | **yes** | — | Max 7,763 observed. |
| `reading_time` | **string** | **yes** | 7 | Human text such as `"25 mins"`. Not a number. |
| `listening_time` | **string** | **yes** | 7 | Null on 199 of 200 documents. |
| `created_at` | string | no | 32 | ISO 8601, microsecond precision, `+00:00` offset. |
| `updated_at` | string | no | 32 | |
| `saved_at` | string | no | 32 | |
| `last_moved_at` | string | no | 32 | |
| `published_date` | string | **yes** | 10 | Date only, `YYYY-MM-DD`. |
| `summary` | string | **yes** | **2093** | Exceeds the 512-byte parser limit. |
| `image_url` | string | **yes** | 234 | |
| `content` | string | **yes** | 154 | Null on 198 of 200. **Not** the article body. |
| `parent_id` | string | **yes** | 26 | Set on highlights/notes; null on 198 of 200. |
| `reading_progress` | number | no | — | `0.0`–`1.0` fraction. **Read-only.** |
| `first_opened_at` | string | **yes** | 32 | |
| `last_opened_at` | string | **yes** | 32 | |
| `html_content` | string | — | **87909** | Only with `withHtmlContent=true`. |

**Where the published documentation is wrong.** `tags` is an object, not a list. `reading_time` and `listening_time` are strings, not numbers. `content` exists but is not documented and is not the body. `location` has an undocumented fifth value.

**Location values — verified:** `new`, `later`, `archive`, `feed`, and **`shortlist`**, which the documentation omits entirely. `shortlist` appeared on 16 of 200 sampled documents. A client that switches exhaustively on the four documented values will mishandle real data, so treat the field as an open set with a fallback.

Optional fields arrive as JSON `null`, not as absent keys, so a parser must handle a null for every nullable field rather than assuming a string.

## Pagination

**Verified.** Cursor-based:

1. Request `/list/?limit=100`.
2. Read `nextPageCursor` from the envelope.
3. Repeat with `?pageCursor=<cursor>` until `nextPageCursor` is `null`.

The cursor is a ULID matching the last document of the page. Treat it as opaque — do not derive it from document ids.

**Sizes — verified.** A 100-document metadata page is **135,129 bytes** (page 1) and **142,518 bytes** (page 2), averaging ~1.4 KB per document. Against a ~380 KB RAM ceiling with no PSRAM, a single page is over a third of all memory. It must be consumed as it arrives.

## Incremental sync

`updatedAfter` takes an ISO 8601 timestamp and returns documents whose `updated_at` is later. A window matching nothing returns a well-formed empty envelope, `{"count": 0, "nextPageCursor": null, "results": []}`, with status `200` — a no-op sync is not an error and must not be reported as one.

**Clock skew.** The device has no reliable RTC across power cycles. Do not synthesize the cursor from local time. Persist the highest `updated_at` actually observed in a completed sync and send that back as the next `updatedAfter`, so the window is anchored to server time. Re-sending a slightly old timestamp costs a few duplicate documents; sending a slightly future one loses documents permanently.

**Commit the cursor only after a page set completes.** A sync interrupted mid-pagination must not advance the stored timestamp, or the unprocessed remainder is never seen again.

### Deletion and tombstones

**Verified, and the most consequential limitation in this document.** The API emits no tombstone of any kind:

| Probe | Result |
|---|---|
| `updatedAfter` window spanning a deletion | Document absent. No marker, no flag, no status field. |
| `GET /list/?id=<deleted id>` | `{"count": 0, "nextPageCursor": null, "results": []}` — not `404`. |

Incremental sync therefore cannot detect that a document was deleted remotely. The only way to discover deletions is a **full sweep**: page through the entire library, collect every id, and treat locally-cached ids absent from that set as deleted.

Because a missing document is indistinguishable from a network failure, a sweep must only drive deletions if it completed successfully end to end. A partial sweep must never be used to expire local data.

### Reconciliation cost

Sweep cost is set by how many documents are in scope, at 100 per request and 20 requests/minute, with a fresh TLS handshake each time because the server closes every connection.

**Scoped to the sweepable locations, a sweep is cheap.** The sweep covers `later` + `shortlist` (see MVP decisions; `feed` is synced but deliberately excluded from the sweep), which on the probe account is 2,294 documents: ~24 requests, a little over a minute, ~3 MB. That is affordable as an occasional explicit operation, and it is comfortably below the `count` cap, so `count` is exact for this scope and the sweep is sound.

**Unscoped, it is not.** The whole library is 10,000+ documents — at least 100 requests, five-plus minutes, ~14 MB — and because `count` saturates, an unscoped sweep cannot even tell you in advance how much work remains.

So: always scope the sweep by `location` to the synced set. Never sweep unfiltered. If `feed` is ever brought into scope it exceeds the cap on its own and would need `updatedAfter` time-windowing rather than a single pass.

One caveat left open deliberately: whether **pagination** also stops at 10,000, or only `count` does, was not tested — confirming it would mean 100+ requests and a five-minute rate-limit burn for a case the MVP scope never reaches. If `feed` sync is ever proposed, test that first, because time-windowing is only necessary if pagination is capped too.

## Updating documents

```http
PATCH /api/v3/update/<document_id>/
Content-Type: application/json
```

**Rate limit: 50 requests/minute** *(documented-only; not exercised)*. Returns `200` with `{"id": "…", "url": "…"}`.

**The endpoint never reports rejected fields.** It answers `200` for unknown fields and for fields it refuses to write. A `200` is therefore not evidence that anything changed; the only way to confirm a write is to read the document back.

| Field | Writable | Evidence |
|---|---|---|
| `title` | **yes** | verified — value changed on read-back |
| `location` | **yes** | verified — `later` → `archive` persisted |
| `seen` | **yes** | verified — set `first_opened_at` as a side effect |
| `reading_progress` | **no** | verified — see below |
| `author`, `summary`, `notes`, `category`, `tags`, `published_date`, `image_url` | *documented-only* | not exercised |

**`reading_progress` is read-only.** Four write attempts — `0.42`, `42`, `"0.42"`, and `1.0` — each returned `200` and each left the stored value at `0`. The same sequence was repeated on a freshly-saved 3,159-word article to rule out "the document has no content", with the same result. In the same session a `title` write on the same document persisted, and a `seen` write took effect, so the endpoint itself was working.

The API exposes reading progress as a value Readwise computes from its own clients. There is no documented location, scroll-offset, or CFI field to write instead.

**Consequence for the product.** Reading position sync is one-way: down from Readwise, never up. A device that syncs progress to Readwise cannot be built on this API as it stands. Phases 3–5 must be scoped accordingly — the device can still mark documents `seen` and move them between `location`s, which covers "archive when finished" but not "resume where I left off on another device".

## Deleting documents

```http
DELETE /api/v3/delete/<document_id>/
```

**Verified:** returns `204` with an empty body. Rate limit 20/minute *(documented-only)*. See [Deletion and tombstones](#deletion-and-tombstones) for what the server does not tell you afterwards.

## Creating highlights (Highlights API v2)

```http
POST /api/v2/highlights/
Content-Type: application/json

{"highlights": [{"text": "...", "title": "...", "source_url": "...",
                 "source_type": "crosspoint", "category": "articles"}, ...]}
```

Used by the on-device highlights feature (`lib/Readwise/ReadwiseHighlightSync`).
This is the **older Highlights API**, not Reader v3, but it accepts the same
token. The firmware batches up to 16 highlights per POST, omits `location` and
`highlighted_at` (no reliable RTC; no meaningful location for extracted plain
text), and sends `title`/`source_url` from the cached document. Rate limit
240/minute *(documented-only)*.

**Verified** (probe section 13, fixture `highlights_create.json`):

* Success is **`200`**, not `201`, with a per-book array; each book object
  carries `modified_highlights` with the created numeric highlight ids.
* All five fields the firmware sends are accepted; `source_type` round-trips
  verbatim as the book's `source`.
* **Grouping:** the highlight lands in a Readwise "book" keyed by
  `title`/`source_url` — the article's name and URL, so grouping is correct.
  It does **not** appear as a Reader child document. Tested twice: once
  against an empty throwaway, then against a throwaway saved **with html
  content containing the exact highlight text**, matching `title` and
  `source_url`, polled for **3 minutes** — no `category: highlight` child
  document appeared, `parent_id` was never set, and the parent's `updated_at`
  never moved. The position-matching sync Readwise documents is
  extension↔Reader; there is no observable v2→Reader propagation at the API
  level. (Whether Reader's *web UI* surfaces the Readwise-side highlight in a
  matching document's Notebook tab was checked by hand: it does not.) See
  [Creating highlights via Reader v3](#creating-highlights-via-reader-v3) for
  the other write path and why it does not help either.
* **The linkage is by internal id, not matchable fields.** A highlight made in
  the Reader UI produced a v3 child document AND a v2 book with
  `source: "reader"` and `location_type: "offset"` — a *different* book from
  the API-created one despite an identical `source_url`. Re-posting a
  highlight with Reader's exact signature (`source_type: "reader"`,
  Reader's Title-Case title, offset location, same URL — the server accepts
  `source_type: "reader"` verbatim) landed in yet another separate book and
  never propagated into Reader. Uploads appearing inline in Reader is not
  achievable by any third-party client; it would require a Readwise feature.
* `location` may be omitted: the server assigns `location: 1,
  location_type: "order"` automatically (verified by reading the created
  highlight back via `GET /api/v2/highlights/<id>/`).
* **Dedup:** a byte-identical re-POST answers with the same book and an
  **empty** `modified_highlights` — no duplicate is created. This is what
  makes the firmware's non-atomic uploaded-flag patch safe: a torn patch only
  re-sends, never duplicates.
* A text-only payload (no title/source_url) is accepted and lands in a generic
  "Quotes" book with `category: "books"` — the fallback when a document has
  been evicted from the local cache.
* Cleanup path `DELETE /api/v2/highlights/<id>/` returns `204`.

## Creating highlights via Reader v3

Readwise support suggested Document CREATE with `category: "highlight"` as the
way to get highlights that behave like Reader-native ones. It does create real
highlight-category Reader documents, but they are **orphans**, so it does not
solve the attachment problem either. The firmware stays on v2.

**Verified:**

* `category: "highlight"` is honoured **only when `html` is omitted**. Passing
  `html` silently coerces the document to `category: "article"` — which is why
  an earlier round of testing wrongly concluded v3 could not create highlights
  at all. With `url` + `title` (the highlight text) + `category: "highlight"`
  and no `html`, the category sticks.
* `should_clean_html: false` requires `author` and `title` (`400` otherwise),
  and still yields `category: "article"`.
* `category` is not honoured uniformly: a control save with `category: "email"`
  also came back as `article`.
* **No linkage to the parent document is achievable.** Four variants, all
  `201`, all with `parent_id: null` after 90 s:
  * `parent_id` passed on CREATE — accepted, silently ignored
  * `url` = the parent's Reader URL (`read.readwise.io/read/<id>`)
  * `url` = the parent's `source_url` byte-identical — and note this creates a
    **new** document rather than deduping into the parent, so highlight-category
    documents occupy a separate URL namespace
  * a control with an unrelated URL
* `PATCH /api/v3/update/<id>/` on a highlight document is refused outright:
  `400 ["Updating 'highlight' documents is not supported."]` — so `parent_id`
  cannot be attached after the fact either.
* Checked by hand in the Reader web UI: none of the four appear inline or in
  the parent document's Notebook tab.

## Rate limiting

**Verified.** Exceeding a limit returns `429` with a JSON body and a `retry-after` header:

```json
{"detail": "Request was throttled. Expected available in 16 seconds."}
```

```http
retry-after: 16
```

Two implementation traps, both observed:

- **The header name is lowercase on the wire.** A case-sensitive lookup for `Retry-After` misses it — this happened during the probe run itself. Header lookup must be case-insensitive.
- **The delay is also in the body.** If the HTTP client does not expose response headers, parse `detail`. Prefer the header.

**Recommended handling.** Honour `retry-after` for a single retry, then abandon the sync pass and surface the failure. Never loop waiting: the value can exceed the FreeRTOS watchdog window, and a blocking retry inside an activity will reset the device. A sync that gives up is recoverable; a watchdog reset is not.

Observed limits: LIST is genuinely 20/minute — the probe run tripped it after 20 requests within the minute, including the requests made by earlier probes.

## Memory implications

Measured against the ~380 KB usable RAM of the ESP32-C3, which has no PSRAM:

| Payload | Bytes | Share of RAM |
|---|---|---|
| 100-document metadata page | 135,129–142,518 | ~36% |
| One article `html_content` (7,763 words) | 87,909 | ~23% |
| One document's metadata | ~1,400 | negligible |

Neither a list page nor an article body may be held in memory. Both must be consumed as they stream, following the established idiom in [src/network/OtaUpdater.cpp](../src/network/OtaUpdater.cpp) — a `HttpDownloader::fetchUrl` data callback feeding a parser incrementally, never accumulating into a `std::string`. The comment there is worth heeding: under `-fno-exceptions`, an allocation failure stacked on top of a live TLS session aborts rather than returning null.

### The 512-byte parser ceiling

[lib/JsonParser/StreamingJsonParser.h](../lib/JsonParser/StreamingJsonParser.h) has `TOKEN_BUF_SIZE = 512`. On overflow it sets an internal flag and **suppresses the value callback entirely** ([StreamingJsonParser.cpp:242](../lib/JsonParser/StreamingJsonParser.cpp)) — it does not truncate and does not raise an error. The consumer sees the key and then the next event, with no indication that a value went missing.

Against real Reader data this silently discards:

- `html_content` — always, by two orders of magnitude.
- `summary` — observed to 2,093 bytes.
- `title` — observed to 318 bytes, so under the limit today, but not by a comfortable margin.

**Resolved in phase 3:** the parser gained an opt-in streaming string sink (`JsonCallbacks::onStringChunk`/`onStringEnd`). When both callbacks are set, string values of any length stream through the fixed token buffer in chunks; consumers that leave them unset keep the legacy drop-on-overflow behaviour unchanged, which is why `ReadwiseContract.OversizedStringsAreSilentlyDroppedByTodaysParser` still passes — it pins the legacy path that `ReleaseJsonParser` and other existing consumers rely on. `lib/Readwise/ReadwiseListParser` is the streaming-mode consumer, delivering `html_content` to a `BodySink` without ever buffering it.

## MVP decisions

Settled for phase 2 to build against.

### Content is stream-stripped to plain text

The API offers no plain-text body — `content` is not it, and `html_content` is the only source. The device fetches `html_content` and strips tags on the fly into a plain-text file on SD, keeping paragraph breaks and discarding everything else.

*Why:* it avoids routing Readwise documents through the EPUB layout and section-cache pipeline, which is the larger cost by far, and it keeps peak RAM at one chunk rather than one document. The trade is losing headings, emphasis, and images. Given an 88 KB body against a 380 KB ceiling, this is the only option that fits without new machinery.

### Synced locations: `later`, `shortlist`, and `feed` (unread-only)

**Revised (owner decision, 2026-08-05).** The original MVP synced `new` + `later`. The library views are now Later, Shortlist and Feed, and `new` (the Inbox) is dropped entirely — its index file, records and bodies are cleared on the first post-upgrade sync.

- `later` and `shortlist` sync normally and participate in the deletion sweep. Both are small and exact-countable, so the sweep stays sound.
- `feed` syncs **unread-only, capped at the newest ~50 items** (`DEFAULT_FEED_CAP`), additive to the document cap. The API offers no server-side unread filter, so documents with `first_opened_at` set are skipped client-side during the pull; a feed item read (on the device or elsewhere) is removed locally — record, index entry and body — at the next sync. The feed walk is hard-bounded at `FEED_MAX_PAGES` (5) pages per pass and skipped items still advance the `updatedAfter` cursor, so the firehose is never enumerated. Feed is **excluded from the reconcile sweep**; its items expire via the unread filter and the cap instead.
- **Ordering assumption:** the capped pull assumes list results arrive newest-first. This was not recorded during the probe run. If it is wrong, the first sync collects older unread items within the page bound, but the merge keeps the newest-updated documents, so the set converges over subsequent syncs. Worth confirming with one `scripts/readwise_probe.py` run.

Because the feed walk never paginates past `FEED_MAX_PAGES`, open risk #11 (whether pagination caps at 10,000 like `count`) remains untested but out of reach.

### Cap: 100 documents of metadata, bodies prefetched during sync

One `limit=100` page of metadata, tunable in settings. **Revised during hardware testing (owner decision):** sync also downloads every missing article body, so "Sync" is the only operation that needs connectivity and the whole library reads offline afterwards. The on-demand per-document fetch remains as the retry path for bodies that failed during the pass.

*Cost accepted:* body fetches share the list endpoint's 20 req/min budget, so a large first sync throttles — the engine waits out each `retry-after` between documents and shows per-document progress. Incremental syncs only fetch bodies for new/changed documents, so the steady-state cost is small.

### Cached bodies are dropped on archive and on confirmed deletion

A cached body is discarded when the document's `location` moves to `archive` or `feed`. The whole cache entry — metadata included — is discarded only when a **successfully completed** full sweep shows the id is gone, per [Deletion and tombstones](#deletion-and-tombstones). Metadata is retained until then, because a document missing from an incremental window has not necessarily been deleted.

## Limitations and open risks

| # | Limitation | Impact |
|---|---|---|
| 1 | `reading_progress` is not writable | Reading position sync is one-way. The device cannot tell Readwise where the user stopped. |
| 2 | No deletion tombstones | Deletions require a full sweep; incremental sync alone will accumulate stale entries indefinitely. |
| 3 | An unscoped full sweep costs 5+ minutes and ~14 MB | Reconciliation must be explicit and scoped by `location`. Scoped to `new`+`later` (2,275 docs) it is ~23 requests and affordable. |
| 4 | `StreamingJsonParser` drops strings over 512 bytes silently | Blocks phase 4 until extended. Affects `summary` as well as `html_content`. |
| 5 | No certificate verification on the wolfSSL path | Token interceptable by an active MITM. Accepted risk, recorded above. |
| 6 | `PATCH` returns `200` for fields it ignores | No write can be trusted without a read-back. Cheap writes become two round trips. |
| 7 | Published docs disagree with the wire format | `tags`, `reading_time`, `listening_time`, `location` values. Trust this document and the fixtures over the vendor docs. |
| 8 | `count` saturates at 10,000 | **Resolved: confirmed cap.** `count` cannot size a sweep or signal completion; only a null `nextPageCursor` does. Harmless at the MVP's scoped set of 2,275. |
| 9 | Rate limits for update/delete not exercised | 50/min and 20/min are documented-only. |
| 10 | Token has no expiry or refresh | A revoked token surfaces only as a `401` at request time. |
| 11 | Unknown whether pagination is capped at 10,000 like `count` | Untested by choice — out of reach of the MVP scope. Must be settled before `feed` could ever be synced. |

Risks 1, 2, and 4 are the ones that shape phases 3–5. Risk 8 is closed: the cap is real, but the MVP's scoped sweep sits well below it, so reconciliation is sound for the synced set.

## Reproducing this

The probe script never accepts a token as an argument, refuses to write inside the repository, mutates only a throwaway document it creates itself, and aborts if that document's URL turns out to already exist. The rate-limit probe is opt-in because it leaves the account throttled for up to a minute.

```bash
export READWISE_TOKEN=<token from readwise.io/access_token>
```

```bash
python scripts/readwise_probe.py --out /tmp/readwise-raw --rate-limit-probe
```

```bash
python scripts/readwise_sanitize.py --in /tmp/readwise-raw --out test/readwise_contract/fixtures
```

The sanitizer refuses to emit anything containing a token-shaped string or an unmapped real document id, so regenerating fixtures from a fresh capture is safe. Raw probe output is not sanitized and must never be committed.

```bash
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure
```
