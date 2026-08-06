#!/usr/bin/env python3
"""Probe the Readwise Reader API and record raw responses for contract analysis.

Phase 1 of the Readwise integration needs facts the published documentation does
not state: whether deletions are observable through ``updatedAfter``, the real
size of ``html_content``, the range of ``reading_progress``, and the magnitude of
``Retry-After``. This script answers them against a live account and writes the
raw evidence to a scratch directory for ``readwise_sanitize.py`` to turn into
test fixtures.

The token is read from the ``READWISE_TOKEN`` environment variable only. It is
deliberately not accepted as a command-line argument so it cannot leak into shell
history or process listings, and it is stripped from everything written to disk.

Usage:
    export READWISE_TOKEN=...
    python scripts/readwise_probe.py --out /tmp/readwise-raw

The output directory must be outside the repository.

This is expected to run against a real account, so it is deliberately
non-destructive:

* No existing document is ever written to or deleted. Every mutating probe
  targets a throwaway document the script creates itself.
* The throwaway URL carries a unique suffix so it cannot collide with something
  already in the library, and the script aborts the mutating probes unless
  ``POST /save/`` answers ``201 Created``. A ``200`` means the id belongs to a
  pre-existing document, which is then left alone.
* Reads of real documents (probes 3-6) do not set ``seen`` or otherwise change
  server state.
* The rate-limit probe is opt-in via ``--rate-limit-probe``; it is read-only but
  leaves the account's LIST endpoint throttled for up to a minute.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

BASE_V2 = "https://readwise.io/api/v2"
BASE_V3 = "https://readwise.io/api/v3"

USER_AGENT = "CrossPoint-ReadwiseProbe/1"

# The mutating probes need a document that is safe to destroy. A unique query
# string guarantees the URL is not already in the library: POST /save/ returns
# 200 with the *existing* id when the URL collides, and deleting that would
# destroy a real document. The uniqueness plus the 201-only guard below makes
# that impossible.
THROWAWAY_URL_BASE = "https://example.com/crosspoint-probe"


class Probe:
    """Accumulates probe results and writes them out with the token removed."""

    def __init__(self, token, out_dir):
        self.token = token
        self.out_dir = out_dir
        self.results = []

    def request(self, name, method, url, *, token=None, body=None, note=""):
        """Perform one request and record everything about it."""
        auth = self.token if token is None else token
        data = None
        headers = {
            "Authorization": f"Token {auth}",
            "User-Agent": USER_AGENT,
        }
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = urllib.request.Request(url, data=data, headers=headers, method=method)

        started = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                status = resp.status
                raw = resp.read()
                resp_headers = dict(resp.headers.items())
        except urllib.error.HTTPError as exc:
            status = exc.code
            raw = exc.read()
            resp_headers = dict(exc.headers.items())
        except urllib.error.URLError as exc:
            status = -1
            raw = str(exc.reason).encode("utf-8")
            resp_headers = {}
        elapsed_ms = int((time.monotonic() - started) * 1000)

        text = raw.decode("utf-8", errors="replace")
        parsed = None
        if text.strip():
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                parsed = None

        record = {
            "probe": name,
            "note": note,
            "method": method,
            # The token never appears in a URL, but redact defensively in case a
            # future probe puts one in a query string.
            "url": self._redact(url),
            "request_body": body,
            "status": status,
            "response_headers": resp_headers,
            "response_bytes": len(raw),
            "elapsed_ms": elapsed_ms,
            "body_is_json": parsed is not None,
            "body_text": None if parsed is not None else self._redact(text),
            "body": parsed,
        }
        self.results.append(record)

        cursor = ""
        if isinstance(parsed, dict) and parsed.get("nextPageCursor"):
            cursor = "  cursor=yes"
        print(
            f"[{name}] {method} {status} {len(raw)}B {elapsed_ms}ms{cursor}",
            file=sys.stderr,
        )
        return record

    def _redact(self, text):
        if self.token and self.token in text:
            return text.replace(self.token, "<REDACTED_TOKEN>")
        return text

    def write(self):
        os.makedirs(self.out_dir, exist_ok=True)
        path = os.path.join(self.out_dir, "probes.json")
        payload = json.dumps(self.results, indent=2, ensure_ascii=False)
        if self.token in payload:
            raise SystemExit("refusing to write: token found in probe output")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(payload)
        print(f"\nwrote {len(self.results)} probes to {path}", file=sys.stderr)
        return path


def list_url(**params):
    query = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
    return f"{BASE_V3}/list/" + (f"?{query}" if query else "")


def iso_now(offset_seconds=0):
    stamp = datetime.now(timezone.utc) + timedelta(seconds=offset_seconds)
    return stamp.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def longest_document(page):
    """Pick the document most likely to have a large body."""
    results = (page or {}).get("results") or []
    if not results:
        return None
    scored = [d for d in results if isinstance(d.get("word_count"), int)]
    if scored:
        return max(scored, key=lambda d: d["word_count"])
    return results[0]


def run(probe, include_rate_limit):
    # --- 1. Auth shape -----------------------------------------------------
    probe.request("01_auth_valid", "GET", f"{BASE_V2}/auth/", note="expect 204")

    # --- 2. Auth failure shape ---------------------------------------------
    probe.request(
        "02_auth_invalid",
        "GET",
        f"{BASE_V2}/auth/",
        token="0000000000000000000000000000000000000000",
        note="expect 401; captures the error body shape",
    )

    # --- 3. First metadata page --------------------------------------------
    page1 = probe.request(
        "03_list_page1",
        "GET",
        list_url(limit=100),
        note="metadata-only page size and total count",
    )
    body1 = page1["body"] if isinstance(page1["body"], dict) else {}

    # --- 4. Cursor mechanics ------------------------------------------------
    cursor = body1.get("nextPageCursor")
    if cursor:
        probe.request(
            "04_list_page2",
            "GET",
            list_url(limit=100, pageCursor=cursor),
            note="second page via nextPageCursor",
        )
    else:
        probe.results.append(
            {
                "probe": "04_list_page2",
                "note": "skipped: account has a single page of documents",
                "skipped": True,
            }
        )
        print("[04_list_page2] skipped (single page)", file=sys.stderr)

    # --- 5. Largest realistic body -----------------------------------------
    target = longest_document(body1)
    if target:
        probe.request(
            "05_list_with_html",
            "GET",
            list_url(id=target["id"], withHtmlContent="true"),
            note=f"largest document by word_count ({target.get('word_count')} words)",
        )
    else:
        print("[05_list_with_html] skipped (no documents)", file=sys.stderr)

    # --- 6. Empty incremental result ---------------------------------------
    probe.request(
        "06_list_empty",
        "GET",
        list_url(updatedAfter=iso_now(60)),
        note="updatedAfter in the future; empty-result shape",
    )

    # --- Throwaway document -------------------------------------------------
    # Every mutating probe below targets this document and nothing else. The
    # account under test is a real one, so no existing document is written to:
    # probes 3-6 are reads, and the update/delete probes operate exclusively on
    # the throwaway created here, which is removed again in probe 10c.
    before_delete = iso_now(-5)
    unique_url = f"{THROWAWAY_URL_BASE}-{int(time.time())}"
    created = probe.request(
        "07a_save_throwaway",
        "POST",
        f"{BASE_V3}/save/",
        body={"url": unique_url, "title": "CrossPoint probe throwaway"},
        note="disposable document; the ONLY document this run mutates",
    )
    throwaway = created["body"].get("id") if isinstance(created["body"], dict) else None
    if not throwaway:
        print("[07-10] skipped (could not create throwaway)", file=sys.stderr)
        return
    if created["status"] != 201:
        # 200 means the URL already existed, so this id belongs to a real
        # document. Refuse to update or delete it.
        print(
            f"[07-10] skipped: save returned {created['status']} (not 201), so the "
            "id may be an existing document; refusing to mutate it",
            file=sys.stderr,
        )
        return

    # Let the write settle so updatedAfter definitely covers it.
    time.sleep(2)

    # --- 7/8/9. Update round-trip, on the throwaway only -------------------
    probe.request(
        "07b_update_progress",
        "PATCH",
        f"{BASE_V3}/update/{throwaway}/",
        body={"reading_progress": 0.42},
        note="does the update endpoint accept reading_progress",
    )
    probe.request(
        "08_list_after_update",
        "GET",
        list_url(id=throwaway),
        note="did reading_progress round-trip and updated_at move",
    )
    probe.request(
        "09a_update_location",
        "PATCH",
        f"{BASE_V3}/update/{throwaway}/",
        body={"location": "later"},
        note="does last_moved_at change on a location write",
    )
    # The control for probe 07b: `seen` and `title` are known-writable, so if
    # they persist while reading_progress does not, the update endpoint is
    # silently discarding reading_progress rather than failing the request.
    probe.request(
        "09b_update_seen_and_title",
        "PATCH",
        f"{BASE_V3}/update/{throwaway}/",
        body={"seen": True, "title": "CrossPoint probe renamed"},
        note="control: known-writable fields",
    )
    time.sleep(2)
    probe.request(
        "09c_list_after_control",
        "GET",
        list_url(id=throwaway),
        note="compare against 08: which of the written fields actually stuck",
    )

    # --- 10. Tombstone behaviour -------------------------------------------
    probe.request(
        "10b_list_before_delete",
        "GET",
        list_url(updatedAfter=before_delete),
        note="throwaway document should be present here",
    )
    probe.request(
        "10c_delete",
        "DELETE",
        f"{BASE_V3}/delete/{throwaway}/",
        note="expect 204; removes the throwaway created in 07a",
    )
    time.sleep(2)
    probe.request(
        "10d_list_after_delete",
        "GET",
        list_url(updatedAfter=before_delete),
        note="THE tombstone question: is the deleted document still listed, "
        "and if so is it marked?",
    )
    probe.request(
        "10e_list_by_deleted_id",
        "GET",
        list_url(id=throwaway),
        note="direct fetch of a deleted id",
    )

    # --- 12. Is `count` a real total or a server-side cap? ----------------
    # If the per-location counts sum to more than the unfiltered total, the
    # total is saturating rather than counting. Each query costs one request,
    # so this runs before the rate-limit probe.
    for location in ("new", "later", "shortlist", "archive", "feed"):
        probe.request(
            f"12_count_location_{location}",
            "GET",
            list_url(limit=1, location=location),
            note="per-location count; compare the sum against probe 03's total",
        )
    # If a capped set slices into exact sub-counts by time, windowing on
    # updatedAfter is a viable way to enumerate past the cap.
    for days in (7, 30, 365):
        probe.request(
            f"12_count_feed_{days}d",
            "GET",
            list_url(limit=1, location="feed", updatedAfter=iso_now(-days * 86400)),
            note="does time-slicing a capped set yield exact counts",
        )

    # --- 13. Highlights API v2 create (for the on-device highlights feature) -
    # Everything here targets a fresh throwaway Reader document and the
    # highlights created against it; both are deleted again at the end. The
    # questions, in order: which fields does POST /v2/highlights/ accept and
    # what does success look like; does a highlight whose title/source_url
    # match an existing Reader document ATTACH to it (visible as a child
    # document with parent_id in the v3 list) or create a standalone book;
    # does re-POSTing identical text+title dedup; and is the minimal
    # text-only payload accepted.
    before_highlights = iso_now(-5)
    hl_url = f"{THROWAWAY_URL_BASE}-hl-{int(time.time())}"
    hl_doc = probe.request(
        "13a_save_highlight_throwaway",
        "POST",
        f"{BASE_V3}/save/",
        body={"url": hl_url, "title": "CrossPoint highlight probe throwaway"},
        note="disposable document for the highlight probes",
    )
    hl_doc_id = hl_doc["body"].get("id") if isinstance(hl_doc["body"], dict) else None
    if not hl_doc_id or hl_doc["status"] != 201:
        print(
            "[13] skipped: could not create a fresh throwaway document for the "
            "highlight probes",
            file=sys.stderr,
        )
    else:
        time.sleep(2)
        hl_payload = {
            "highlights": [
                {
                    "text": "A probe sentence captured by CrossPoint.",
                    "title": "CrossPoint highlight probe throwaway",
                    "source_url": hl_url,
                    "source_type": "crosspoint",
                    "category": "articles",
                    # location and highlighted_at deliberately omitted: the
                    # device has no reliable RTC and no meaningful location,
                    # so the firmware will omit them too.
                }
            ]
        }
        created_hl = probe.request(
            "13b_create_highlight",
            "POST",
            f"{BASE_V2}/highlights/",
            body=hl_payload,
            note="field set the firmware intends to send; success-response shape "
            "and rate-limit header casing",
        )
        probe.request(
            "13c_redundant_create",
            "POST",
            f"{BASE_V2}/highlights/",
            body=hl_payload,
            note="identical re-POST: dedup question -- a torn uploaded-flag on "
            "device would re-send, is that harmless?",
        )
        probe.request(
            "13d_create_text_only",
            "POST",
            f"{BASE_V2}/highlights/",
            body={"highlights": [{"text": "CrossPoint minimal probe highlight."}]},
            note="minimal accepted payload, for documents missing local metadata",
        )
        time.sleep(2)
        probe.request(
            "13e_list_after_highlight",
            "GET",
            list_url(updatedAfter=before_highlights, withFullContent="false"),
            note="THE attach question: did the highlight land as a child of the "
            "throwaway (parent_id set) or as a standalone book?",
        )
        # Clean up every highlight the three creates produced. The v2 create
        # response carries modified_highlights ids on current API versions;
        # collect defensively from both creates.
        hl_ids = []
        for record in (created_hl,):
            body = record["body"] if isinstance(record["body"], dict) else {}
            for key in ("modified_highlights", "highlights"):
                value = body.get(key)
                if isinstance(value, list):
                    for item in value:
                        if isinstance(item, int):
                            hl_ids.append(item)
                        elif isinstance(item, dict) and isinstance(item.get("id"), int):
                            hl_ids.append(item["id"])
        if not hl_ids and isinstance(created_hl["body"], list):
            for item in created_hl["body"]:
                if isinstance(item, dict):
                    for sub in item.get("modified_highlights", []) or []:
                        if isinstance(sub, int):
                            hl_ids.append(sub)
        for hl_id in hl_ids:
            probe.request(
                f"13f_delete_highlight_{hl_id}",
                "DELETE",
                f"{BASE_V2}/highlights/{hl_id}/",
                note="cleanup of a probe-created highlight",
            )
        if not hl_ids:
            print(
                "[13f] no highlight ids recognized in the create response; "
                "clean up probe highlights manually in Readwise",
                file=sys.stderr,
            )
        probe.request(
            "13g_delete_highlight_throwaway",
            "DELETE",
            f"{BASE_V3}/delete/{hl_doc_id}/",
            note="cleanup of the highlight-probe throwaway document",
        )

    # --- 11. Rate limiting (opt-in; leaves LIST throttled for ~a minute) ---
    if not include_rate_limit:
        print(
            "[11] skipped; pass --rate-limit-probe to measure Retry-After "
            "(this throttles the account's LIST endpoint for up to a minute)",
            file=sys.stderr,
        )
        return
    for attempt in range(25):
        record = probe.request(
            f"11_ratelimit_{attempt:02d}",
            "GET",
            list_url(limit=1),
            note="hammering LIST to observe 429 and Retry-After",
        )
        if record["status"] == 429:
            # The server sends this header lowercase ("retry-after"), so the
            # lookup must be case-insensitive. The firmware client has the same
            # obligation.
            headers = {k.lower(): v for k, v in record["response_headers"].items()}
            print(
                f"[11] hit 429 after {attempt + 1} calls; "
                f"retry-after={headers.get('retry-after')}",
                file=sys.stderr,
            )
            break
    else:
        print("[11] no 429 observed in 25 calls", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        required=True,
        help="directory for raw probe output; must be outside the repository",
    )
    parser.add_argument(
        "--rate-limit-probe",
        action="store_true",
        help="deliberately trip the LIST rate limit to measure Retry-After. "
        "Read-only, but leaves the account's LIST endpoint throttled for up to "
        "a minute, so it is off by default.",
    )
    args = parser.parse_args()

    token = os.environ.get("READWISE_TOKEN", "").strip()
    if not token:
        raise SystemExit(
            "READWISE_TOKEN is not set. Export it in your shell; this script "
            "does not accept a token argument."
        )

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.abspath(args.out)
    if out_dir.startswith(repo_root + os.sep) or out_dir == repo_root:
        raise SystemExit(
            f"refusing to write raw probe output inside the repository: {out_dir}"
        )

    probe = Probe(token, out_dir)
    try:
        run(probe, args.rate_limit_probe)
    finally:
        # Always persist whatever was collected, even on an early failure.
        probe.write()


if __name__ == "__main__":
    main()
