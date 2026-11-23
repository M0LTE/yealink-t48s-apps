#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""feed-page.py SITE: a small index.html for people, from SITE/index.json."""
import html
import json
import os
import sys

d = sys.argv[1]
with open(os.path.join(d, "index.json")) as f:
    feed = json.load(f)
shots = sorted(os.listdir(os.path.join(d, "screenshots")))
rows = []
for a in feed["apps"]:
    e = {k: html.escape(str(v)) for k, v in a.items()}
    pics = "".join(f'<img src="screenshots/{html.escape(s)}" width="400" alt="">'
                   for s in shots if s.startswith(a["id"] + "-"))
    rows.append(f'<h2>{e["name"]} <small>{e["version"]}</small></h2><p>{e["description"]}</p>'
                f'<p><a href="{e["url"]}">{e["url"]}</a>, {a["size"]} bytes, SHA-256 <code>{e["sha256"]}</code></p>{pics}')
print(f"""<!doctype html><meta charset="utf-8"><title>T48 apps</title>
<style>body{{font-family:sans-serif;max-width:860px;margin:2em auto;padding:0 1em}}img{{margin:4px;border:1px solid #ccc}}code{{font-size:80%}}</style>
<h1>Apps for the open T48S firmware</h1>
<p>The phone's app feed is <a href="index.json">index.json</a> at this address. Source:
<a href="https://github.com/M0LTE/yealink-t48s-apps">M0LTE/yealink-t48s-apps</a>. Updated {html.escape(feed["generated"])}.</p>
{"".join(rows)}""")
