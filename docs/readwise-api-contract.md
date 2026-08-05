# Readwise Reader API Contract

The bounded contract CrossPoint relies on when talking to the Readwise Reader API. This document exists so the persisted schemas, sync engine, and UI designed in later phases are built against measured behaviour rather than the published documentation, which this exercise found to be wrong in several places that matter.

Everything marked **verified** was observed against a live Reader account on 2026-08-05 using [scripts/readwise_probe.py](../scripts/readwise_probe.py). Everything marked *documented-only* comes from `readwise.io/reader_api` and has not been exercised here. Fixtures capturing the verified responses live in [test/readwise_contract/fixtures/](../test/readwise_contract/fixtures/) and are guarded by [test/readwise_contract/ReadwiseContractTest.cpp](../test/readwise_contract/ReadwiseContractTest.cpp).

## Headline findings

Four results change what the feature can be. Read these before the reference sections.

1. **`reading_progress` cannot be written.** `PATCH /update/` accepts it, answers `200`, and silently discards it. The device can read a reading position down from Readwise but cannot push one up. Verified against both an empty document and a 3,159-word article, with `title`, `seen`, and `location` writes succeeding in the same sequence as controls.
2. **There are no deletion tombstones.** A deleted document simply disappears. An `updatedAfter` window spanning the deletion does not mention it, and fetching it by `id` returns `count: 0` rather than `404`. Incremental sync structurally cannot observe deletions.
3. **A single article body is 88 KB.** That is roughly a quarter of the device's entire RAM for one field of one document, and a full metadata page is 135 KB. Nothing may be buffered whole.
4. **Today's `StreamingJsonParser` silently drops any string over 512 bytes.** It does not truncate and does not error — the value callback is suppressed. This already affects `summary` (observed to 2,093 bytes), not just `html_content`.

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

**TLS posture (accepted risk).** The wolfSSL path selected by `FREEINK_NET_WOLFSSL` performs no certificate verification: every call site in [lib/KOReaderSync/KOReaderSyncClient.cpp](../lib/KOReaderSync/KOReaderSyncClient.cpp) calls `setInsecure()`, and no CA bundle or pinned certificate exists for that backend. The `esp_crt_bundle_attach` verification in [src/network/HttpDownloader.cpp](../src/network/HttpDownloader.cpp) applies only to the legacy `esp_http_client` path, which is not the one in use.

A Readwise token therefore crosses an unverified TLS session and is interceptable by an active MITM on the local network. This is the same posture as the existing KOReader sync and is accepted for shipping. It is recorded here so the decision is explicit rather than accidental.

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

`count` is the total matching the query across all pages, not the page size. The probe account reported **10,000**, which is large enough that a full sweep is a real cost, not a rounding error — see [Reconciliation cost](#reconciliation-cost).

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

With `count: 10000` on the probe account, a full sweep is 100 requests at 100 documents each. The list endpoint allows 20 requests/minute, so a complete sweep takes **at least five minutes of continuous requests** and transfers roughly 14 MB — with a fresh TLS handshake per request, since the server closes each connection.

This is not something to run on every sync. Phase 2 and 3 should treat reconciliation as an occasional, explicitly-triggered operation, and should scope the sweep by `location` so only synced locations are swept.

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

Phase 4 must extend the parser with a streaming string sink that delivers long values in chunks, before any field above can be consumed. `ReadwiseContract.OversizedStringsAreSilentlyDroppedByTodaysParser` pins the current behaviour and is expected to fail — deliberately — when that work lands. [lib/JsonParser/ReleaseJsonParser.cpp](../lib/JsonParser/ReleaseJsonParser.cpp) is the model for layering a domain parser over the streaming core.

## MVP decisions

Settled for phase 2 to build against.

### Content is stream-stripped to plain text

The API offers no plain-text body — `content` is not it, and `html_content` is the only source. The device fetches `html_content` and strips tags on the fly into a plain-text file on SD, keeping paragraph breaks and discarding everything else.

*Why:* it avoids routing Readwise documents through the EPUB layout and section-cache pipeline, which is the larger cost by far, and it keeps peak RAM at one chunk rather than one document. The trade is losing headings, emphasis, and images. Given an 88 KB body against a 380 KB ceiling, this is the only option that fits without new machinery.

### Synced locations: `new` and `later`

`archive` and `feed` are not synced by default. `feed` in particular is unbounded and dominated the probe account at 146 of 200 sampled documents — syncing it would mean syncing an RSS firehose onto an e-reader.

Note that `shortlist` exists and is not in the published docs. It should probably be synced too, but that is a product call to confirm rather than something the API forced.

### Cap: 100 documents of metadata, bodies fetched on demand

One `limit=100` page of metadata, tunable in settings. Bodies are **not** prefetched — a body is fetched when the document is opened, cached to SD, and evicted LRU.

*Why:* prefetching 100 bodies would mean ~9 MB of transfer, 100 TLS handshakes against a 20/min ceiling, and a large SD write burst, to produce articles the user mostly will not read. On-demand fetch makes the cap soft and keeps the sync pass short.

### Cached bodies are dropped on archive and on confirmed deletion

A cached body is discarded when the document's `location` moves to `archive` or `feed`. The whole cache entry — metadata included — is discarded only when a **successfully completed** full sweep shows the id is gone, per [Deletion and tombstones](#deletion-and-tombstones). Metadata is retained until then, because a document missing from an incremental window has not necessarily been deleted.

## Limitations and open risks

| # | Limitation | Impact |
|---|---|---|
| 1 | `reading_progress` is not writable | Reading position sync is one-way. The device cannot tell Readwise where the user stopped. |
| 2 | No deletion tombstones | Deletions require a full sweep; incremental sync alone will accumulate stale entries indefinitely. |
| 3 | Full sweep costs ~5 minutes and ~14 MB at 10k documents | Reconciliation cannot be routine. Needs to be explicit and scoped by location. |
| 4 | `StreamingJsonParser` drops strings over 512 bytes silently | Blocks phase 4 until extended. Affects `summary` as well as `html_content`. |
| 5 | No certificate verification on the wolfSSL path | Token interceptable by an active MITM. Accepted risk, recorded above. |
| 6 | `PATCH` returns `200` for fields it ignores | No write can be trusted without a read-back. Cheap writes become two round trips. |
| 7 | Published docs disagree with the wire format | `tags`, `reading_time`, `listening_time`, `location` values. Trust this document and the fixtures over the vendor docs. |
| 8 | `count: 10000` may be a server-side cap rather than a true total | Not distinguished by the probe. If it is a cap, sweep-based reconciliation is unsound on very large libraries. **Unresolved.** |
| 9 | Rate limits for update/delete not exercised | 50/min and 20/min are documented-only. |
| 10 | Token has no expiry or refresh | A revoked token surfaces only as a `401` at request time. |

Risk 8 is the one worth closing before phase 2 freezes schemas: it decides whether a full sweep can ever be authoritative.

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
