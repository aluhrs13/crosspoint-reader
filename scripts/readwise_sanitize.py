#!/usr/bin/env python3
"""Turn raw Readwise probe output into sanitized test fixtures.

``readwise_probe.py`` records real responses from a real account. Nothing it
writes may be committed. This script is the only sanctioned path from that raw
capture to ``test/readwise_contract/fixtures/``: it replaces every identifier,
URL, and piece of human-authored text with neutral substitutes, then refuses to
write anything that still looks like a secret.

Substituted text keeps the **byte length** of the original wherever it can, so
the size findings recorded in ``docs/readwise-api-contract.md`` remain honest
when checked against the fixtures.

Usage:
    python scripts/readwise_sanitize.py \\
        --in /tmp/readwise-raw \\
        --out test/readwise_contract/fixtures
"""

import argparse
import json
import os
import re
import sys

# Fields holding human-authored prose or personal reading history.
TEXT_FIELDS = ("title", "author", "summary", "notes", "site_name", "source")
URL_FIELDS = ("url", "source_url", "image_url", "raw_source_url")

LOREM = (
    "Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod "
    "tempor incididunt ut labore et dolore magna aliqua Ut enim ad minim "
    "veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
    "commodo consequat Duis aute irure dolor in reprehenderit in voluptate "
)

# Anything that looks like a bearer token. The probe script already redacts the
# live token; this is the belt-and-braces check before anything reaches git.
SECRET_RE = re.compile(r"\b[A-Za-z0-9]{40,}\b")

# ULIDs are 26 chars of Crockford base32; document ids are the only place they
# appear, and they are replaced by counter-derived synthetic ids.
ULID_RE = re.compile(r"^[0-9A-HJKMNP-TV-Za-hjkmnp-tv-z]{26}$")


class Sanitizer:
    """Maps real values to stable synthetic ones, consistently across a run."""

    def __init__(self):
        self.id_map = {}
        self.num_map = {}

    def doc_id(self, real):
        if real is None:
            return None
        if real not in self.id_map:
            self.id_map[real] = f"01hzzzzzzzzzzzzzzzzzzzzz{len(self.id_map):02d}"
        return self.id_map[real]

    def text(self, value):
        """Neutral filler of the same byte length as the original."""
        if value is None:
            return None
        target = len(value.encode("utf-8"))
        if target == 0:
            return ""
        filler = (LOREM * (target // len(LOREM) + 1))[:target]
        # LOREM is pure ASCII, so byte length equals character length.
        return filler

    def url(self, value, doc_id=None):
        if value is None:
            return None
        if value.startswith("https://read.readwise.io/read/") and doc_id:
            return f"https://read.readwise.io/read/{doc_id}"
        target = len(value.encode("utf-8"))
        base = "https://example.com/"
        if target <= len(base):
            return base[:target]
        # Pad with a dashed path rather than a run of letters, so the padding
        # cannot itself look like a 40-character token to the secret scanner.
        pad = ("path-segment/" * (target // 13 + 1))[: target - len(base)]
        return base + pad

    def html(self, value):
        """Keep tag structure (the stripper is what consumes this) but replace
        all text nodes and attribute values, preserving overall byte length.

        Attribute values matter: real ``html_content`` carries image URLs and
        asset hashes inside ``src``/``href``, which are as identifying as the
        prose.
        """
        if value is None:
            return None
        parts = re.split(r"(<[^>]*>)", value)
        out = []
        for part in parts:
            if part.startswith("<"):
                out.append(self._scrub_attrs(part))
            else:
                out.append(self.text(part))
        return "".join(out)

    def _scrub_attrs(self, tag):
        def repl(match):
            name, quote, value = match.group(1), match.group(2), match.group(3)
            # `dir` drives RTL rendering, so its value is structural, not data.
            if name.lower() == "dir":
                return match.group(0)
            # Dashed filler, so the replacement cannot itself look like a token
            # or a ULID to the secret scanner below.
            filler = ("attr-" * (len(value) // 5 + 1))[: len(value)]
            return f"{name}={quote}{filler}{quote}"

        return re.sub(r"([A-Za-z_:][-\w:.]*)=(\"|')([^\"']*)\2", repl, tag)

    def document(self, doc):
        if not isinstance(doc, dict):
            return doc
        out = dict(doc)
        new_id = self.doc_id(out.get("id"))
        if "id" in out:
            out["id"] = new_id
        if out.get("parent_id"):
            out["parent_id"] = self.doc_id(out["parent_id"])
        for field in TEXT_FIELDS:
            if field in out:
                out[field] = self.text(out[field])
        for field in URL_FIELDS:
            if field in out:
                out[field] = self.url(out[field], new_id)
        if out.get("content"):
            out["content"] = self.text(out["content"])
        if out.get("html_content"):
            out["html_content"] = self.html(out["html_content"])
        # tags are user-authored labels
        if isinstance(out.get("tags"), dict) and out["tags"]:
            out["tags"] = {f"tag{i}": v for i, v in enumerate(out["tags"].values())}
        return out

    def num_id(self, real):
        """Numeric v2 ids (book ids, highlight ids) mapped to stable synthetic
        values. Not secret, but account-specific."""
        if not isinstance(real, int):
            return real
        if real not in self.num_map:
            self.num_map[real] = 10000001 + len(self.num_map)
        return self.num_map[real]

    def v2_book(self, book):
        """Sanitize one per-book object from the POST /v2/highlights/ response."""
        if not isinstance(book, dict):
            return book
        out = dict(book)
        if "id" in out:
            out["id"] = self.num_id(out["id"])
        for field in TEXT_FIELDS:
            if field in out:
                out[field] = self.text(out[field])
        for field in URL_FIELDS + ("cover_image_url", "highlights_url"):
            if field in out:
                out[field] = self.url(out[field])
        if isinstance(out.get("modified_highlights"), list):
            out["modified_highlights"] = [self.num_id(v) for v in out["modified_highlights"]]
        return out

    def v2_create_response(self, body):
        if not isinstance(body, list):
            return body
        return [self.v2_book(b) for b in body]

    def id_url_response(self, body):
        """Sanitize the ``{"id": ..., "url": ...}`` body returned by save and
        update. It carries a real document id and must not be written raw."""
        if not isinstance(body, dict):
            return body
        out = dict(body)
        new_id = self.doc_id(out.get("id"))
        if "id" in out:
            out["id"] = new_id
        if "url" in out:
            out["url"] = self.url(out["url"], new_id)
        return out

    def page(self, body):
        if not isinstance(body, dict):
            return body
        out = dict(body)
        if isinstance(out.get("results"), list):
            out["results"] = [self.document(d) for d in out["results"]]
        if out.get("nextPageCursor"):
            out["nextPageCursor"] = self.doc_id(out["nextPageCursor"])
        return out


def unicode_fixture(sanitizer, template):
    """Build the Unicode case from a real page shape.

    The live account is English, so no captured response exercises CJK, RTL, or
    combining marks. Rather than omit the case issue #2 requires, synthesize it
    on top of a real response shape so the field set stays authentic.
    """
    doc = dict(template["results"][0])
    doc["title"] = "日本語のタイトル — 中文標題 🇯🇵📚 مرحبا بالعالم"
    doc["author"] = "Ünïcödé Áuthör ✍️ עברית"
    doc["summary"] = "Zalgo-ish combining marks: áèîõü " "and an emoji family 👨‍👩‍👧‍👦 plus RTL نص عربي طويل."
    doc["site_name"] = "例え.テスト"
    doc["html_content"] = (
        "<div><h1>見出し</h1><p>本文のテキストです。</p>"
        "<p dir=\"rtl\">هذا نص عربي للاختبار.</p>"
        "<p>Combining: é ö ñ — emoji: 🚀🌍</p></div>"
    )
    return {"count": 1, "nextPageCursor": None, "results": [doc]}


def write(out_dir, name, payload):
    path = os.path.join(out_dir, name)
    text = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    print(f"  {name}  ({len(text.encode('utf-8'))} bytes)")
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in", dest="src", required=True, help="raw probe dir")
    parser.add_argument("--out", dest="dst", required=True, help="fixture dir")
    args = parser.parse_args()

    with open(os.path.join(args.src, "probes.json"), encoding="utf-8") as handle:
        probes = {r["probe"]: r for r in json.load(handle)}

    os.makedirs(args.dst, exist_ok=True)
    s = Sanitizer()
    written = []

    def body(name):
        rec = probes.get(name)
        return rec.get("body") if rec else None

    print("writing fixtures:")

    # Normal page. Trimmed to 5 documents: the test needs field coverage, not
    # 135 KB of repetition in the repository.
    page1 = s.page(body("03_list_page1"))
    normal = dict(page1)
    normal["results"] = page1["results"][:5]
    written.append(write(args.dst, "list_normal.json", normal))

    # Optional-field case: the documents that actually carried nulls.
    all_docs = page1["results"] + s.page(body("04_list_page2"))["results"]
    nullable = [
        d
        for d in all_docs
        if any(
            d.get(f) is None
            for f in ("title", "author", "site_name", "published_date", "location", "word_count")
        )
    ][:5]
    written.append(
        write(
            args.dst,
            "list_optional_fields.json",
            {"count": len(nullable), "nextPageCursor": None, "results": nullable},
        )
    )

    # Oversized content: kept at full length on purpose. This is the fixture
    # that proves a single html_content field exceeds any sane RAM budget.
    written.append(
        write(args.dst, "list_oversized_content.json", s.page(body("05_list_with_html")))
    )

    # Pagination: non-null cursor, then null.
    p1 = dict(page1)
    p1["results"] = page1["results"][:3]
    written.append(write(args.dst, "list_page1.json", p1))
    p2 = s.page(body("04_list_page2"))
    p2["results"] = p2["results"][:3]
    p2["nextPageCursor"] = None
    written.append(write(args.dst, "list_page2.json", p2))

    written.append(write(args.dst, "list_unicode.json", unicode_fixture(s, normal)))
    written.append(write(args.dst, "list_empty.json", body("06_list_empty")))

    # The tombstone finding, recorded as data.
    written.append(
        write(
            args.dst,
            "delete_then_list.json",
            {
                "_comment": "updatedAfter spanning a deletion. The deleted document "
                "is absent entirely - the API emits no tombstone.",
                "before_delete": s.page(body("10b_list_before_delete")),
                "after_delete": s.page(body("10d_list_after_delete")),
                "by_deleted_id": s.page(body("10e_list_by_deleted_id")),
            },
        )
    )

    written.append(write(args.dst, "error_401.json", body("02_auth_invalid")))

    rl = next((probes[k] for k in probes if k.startswith("11_") and probes[k]["status"] == 429), None)
    if rl:
        headers = {k.lower(): v for k, v in rl["response_headers"].items()}
        written.append(
            write(
                args.dst,
                "error_429.json",
                {
                    "_comment": "The retry-after header name is lowercase on the "
                    "wire; header lookup must be case-insensitive.",
                    "retry_after_header": headers.get("retry-after"),
                    "body": rl["body"],
                },
            )
        )

    written.append(
        write(
            args.dst,
            "update_response.json",
            s.id_url_response(body("07b_update_progress")),
        )
    )

    # POST /v2/highlights/ create responses, from probe section 13. Answers are
    # recorded as data, delete_then_list.json-style: the first create returns
    # the target book with one modified highlight; the byte-identical re-POST
    # returns the same book with modified_highlights EMPTY (server-side dedup,
    # which is what makes the firmware's torn uploaded-flag patch harmless);
    # a text-only create lands in a generic "Quotes" book.
    if body("13b_create_highlight") is not None:
        written.append(
            write(
                args.dst,
                "highlights_create.json",
                {
                    "_comment": "HTTP 200 (not 201). Same book id in first and "
                    "redundant create; dedup signalled by empty "
                    "modified_highlights. The v3 Reader document list showed no "
                    "new child document and an unchanged updated_at.",
                    "first_create": s.v2_create_response(body("13b_create_highlight")),
                    "redundant_create": s.v2_create_response(body("13c_redundant_create")),
                    "text_only_create": s.v2_create_response(body("13d_create_text_only")),
                },
            )
        )

    # --- refuse to emit anything that still looks sensitive ----------------
    problems = []
    synthetic = set(s.id_map.values())
    for text in written:
        for hit in SECRET_RE.findall(text):
            problems.append(f"possible secret: {hit[:12]}...")
        for real_id in s.id_map:
            if real_id in text:
                problems.append(f"unmapped real document id: {real_id}")
        # Catch any ULID-shaped string that is not one we minted, which would
        # mean a real id reached the output through a path not covered above.
        for token in re.findall(r"[0-9A-Za-z]{26}", text):
            if ULID_RE.match(token) and token not in synthetic:
                problems.append(f"unrecognized ULID-shaped value: {token}")
    if "readwise.io/access_token" in "".join(written):
        problems.append("token URL present")
    if problems:
        for problem in sorted(set(problems)):
            print(f"FAIL {problem}", file=sys.stderr)
        raise SystemExit("sanitization failed; fixtures not safe to commit")

    print(f"\nok: {len(written)} fixtures, no secrets or real ids detected")


if __name__ == "__main__":
    main()
