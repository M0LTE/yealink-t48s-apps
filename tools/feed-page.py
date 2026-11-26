#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""feed-page.py SITE: a small index.html for people, from SITE/index.json.
Apps are grouped by category as on the phone (issue 30): the fixed order, General for
an app without one, and no headings when there is only one group."""
import html
import json
import os
import sys

CATEGORIES = ["General", "Games", "Radio", "Tools", "Media"]

d = sys.argv[1]
with open(os.path.join(d, "index.json")) as f:
    feed = json.load(f)
shots = sorted(os.listdir(os.path.join(d, "screenshots")))


def category(a):
    c = a.get("category")
    return c if c in CATEGORIES else "General"


def entry(a, h):
    e = {k: html.escape(str(v)) for k, v in a.items()}
    pics = "".join(f'<img src="screenshots/{html.escape(s)}" width="400" alt="">'
                   for s in shots if s.startswith(a["id"] + "-"))
    return (f'<{h}>{e["name"]} <small>{e["version"]}</small></{h}><p>{e["description"]}</p>'
            f'<p><a href="{e["url"]}">{e["url"]}</a>, {a["size"]} bytes, SHA-256 <code>{e["sha256"]}</code></p>{pics}')


groups = [c for c in CATEGORIES if any(category(a) == c for a in feed["apps"])]
rows = []
for c in groups:
    if len(groups) > 1:
        rows.append(f"<h2>{c}</h2>")
    rows += [entry(a, "h3" if len(groups) > 1 else "h2") for a in feed["apps"] if category(a) == c]
print(f"""<!doctype html><meta charset="utf-8"><title>T48 apps</title>
<style>body{{font-family:sans-serif;max-width:860px;margin:2em auto;padding:0 1em}}img{{margin:4px;border:1px solid #ccc}}code{{font-size:80%}}h2{{margin-top:1.6em;border-bottom:1px solid #ccc}}</style>
<h1>Apps for the open T48S firmware</h1>
<p>The phone's app feed is <a href="index.json">index.json</a> at this address. Source:
<a href="https://github.com/M0LTE/yealink-t48s-apps">M0LTE/yealink-t48s-apps</a>. Updated {html.escape(feed["generated"])}.</p>
{"".join(rows)}""")
