#!/usr/bin/env python3
"""
gen_dashboard.py - Build a static GitHub Pages site for the mmap simulator.

The site has five pages sharing a common nav:
  index.html      Overview  - overall badge, a visual requirement-coverage bar,
                  the requirement->test traceability matrix, and the CI
                  job-dependency graph.
  tests.html      Tests     - per-suite pass/fail bars and per-test results
                  (with a "failing only" filter), each test annotated with the
                  requirement(s) it covers.
  visualize.html  Visualize - interactive SVG address-space viewer (vma_viz.js).
  docs.html       Docs      - the design documents (docs/*.md) rendered
                  client-side, with an auto table-of-contents.
  team.html       Team      - the AI agent-team design (docs/agent_team.md).

Inputs:
  --reports DIR        directory of JUnit XML files (from tests/test_harness.h)
  --workflow FILE      CI workflow YAML, mined for the job-dependency graph
  --requirements FILE  requirements registry JSON (the traceability source)
  --docs DIR           directory of Markdown design docs to surface
  --out DIR            output directory (the site is written here)

Stdlib only (xml.etree + json + a tiny indentation-aware workflow scan), so it
runs in CI and locally without third-party packages. Markdown and the job graph
are rendered client-side via the same CDN approach already used for Mermaid.
"""
import argparse
import html
import json
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone


# ----------------------------- parsing ------------------------------------

def parse_junit(reports_dir):
    """Return list of suites: {name, tests, failures, cases:[{name,assertions,failed,message}]}."""
    suites = []
    if not os.path.isdir(reports_dir):
        return suites
    for fn in sorted(os.listdir(reports_dir)):
        if not fn.endswith(".xml"):
            continue
        path = os.path.join(reports_dir, fn)
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as e:
            print(f"warn: cannot parse {path}: {e}", file=sys.stderr)
            continue
        for ts in root.iter("testsuite"):
            cases = []
            for tc in ts.findall("testcase"):
                failure = tc.find("failure")
                cases.append({
                    "name": tc.get("name", "(unnamed)"),
                    "assertions": tc.get("assertions", "0"),
                    "failed": failure is not None,
                    "message": (failure.get("message", "") + "\n" + (failure.text or "")
                                if failure is not None else ""),
                })
            suites.append({
                "name": ts.get("name", os.path.splitext(fn)[0]),
                "tests": int(ts.get("tests", len(cases))),
                "failures": int(ts.get("failures", sum(c["failed"] for c in cases))),
                "cases": cases,
            })
    return suites


def parse_job_graph(workflow_path):
    """Extract {job_id: [needs...]} from the workflow YAML via a small scan.

    Handles `needs: a`, `needs: [a, b]`, and a block list of `- a` items."""
    jobs = {}
    if not os.path.isfile(workflow_path):
        return jobs
    lines = open(workflow_path, encoding="utf-8").read().splitlines()
    in_jobs = False
    cur = None
    pending_list = False
    for line in lines:
        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        if re.match(r"^\S", line):
            break
        m_job = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if m_job:
            cur = m_job.group(1)
            jobs[cur] = []
            pending_list = False
            continue
        if cur is None:
            continue
        m_needs = re.match(r"^    needs:\s*(.*)$", line)
        if m_needs:
            val = m_needs.group(1).strip()
            if val and not val.startswith("#"):
                val = val.strip("[]")
                jobs[cur] = [d.strip().strip("\"'") for d in val.split(",") if d.strip()]
                pending_list = False
            else:
                pending_list = True
            continue
        if pending_list:
            m_item = re.match(r"^      -\s*(.+)$", line)
            if m_item:
                jobs[cur].append(m_item.group(1).strip().strip("\"'"))
                continue
            pending_list = False
    return jobs


def parse_requirements(path):
    """Load the requirements registry JSON. Returns {categories, requirements}."""
    if not path or not os.path.isfile(path):
        return {"categories": {}, "requirements": []}
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return {
        "categories": data.get("categories", {}),
        "requirements": data.get("requirements", []),
    }


# ----------------------------- traceability -------------------------------

def index_tests(suites):
    """Map test name -> {"failed": bool, "suite": name}. Last write wins."""
    idx = {}
    for s in suites:
        for c in s["cases"]:
            idx[c["name"]] = {"failed": c["failed"], "suite": s["name"]}
    return idx


def compute_traceability(reqs, test_idx):
    """Annotate each requirement with coverage status against the test index.

    Status per requirement:
      passing  - has >=1 mapped test, all mapped tests exist and pass
      failing  - has >=1 mapped test that exists and failed
      partial  - some mapped test names are missing from the JUnit reports
      no-test  - no tests mapped
    Also returns the set of test names that no requirement references."""
    referenced = set()
    rows = []
    for r in reqs["requirements"]:
        mapped = r.get("tests", [])
        present, missing, failing = [], [], []
        for t in mapped:
            referenced.add(t)
            if t not in test_idx:
                missing.append(t)
            elif test_idx[t]["failed"]:
                failing.append(t)
            else:
                present.append(t)
        if not mapped:
            status = "no-test"
        elif failing:
            status = "failing"
        elif missing:
            status = "partial"
        else:
            status = "passing"
        rows.append({**r, "status": status, "present": present,
                     "missing": missing, "failing": failing})
    orphan_tests = sorted(set(test_idx) - referenced)
    return rows, orphan_tests


def reverse_map(trace_rows):
    """Map test name -> [requirement ids] for the Tests page annotations."""
    rev = {}
    for r in trace_rows:
        for t in r.get("tests", []):
            rev.setdefault(t, []).append(r["id"])
    return rev


# ----------------------------- rendering ----------------------------------

CSS = """
  :root { color-scheme: light dark; }
  body { font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
         margin: 0 auto; max-width: 1000px; padding: 0 24px 48px; line-height: 1.5; }
  header.site { display: flex; align-items: baseline; gap: 16px; flex-wrap: wrap;
                border-bottom: 1px solid #d0d7de; padding: 16px 0; margin-bottom: 16px; }
  header.site h1 { font-size: 18px; margin: 0; }
  nav.site a { margin-right: 14px; text-decoration: none; font-weight: 600; color: #57606a; }
  nav.site a.active { color: #0969da; border-bottom: 2px solid #0969da; padding-bottom: 4px; }
  .meta { color: #6e7781; font-size: 14px; margin: 8px 0 20px; }
  .meta a { color: inherit; }
  .badge { display: inline-block; padding: 4px 12px; border-radius: 999px;
           color: #fff; font-weight: 700; }
  table { border-collapse: collapse; width: 100%; margin: 8px 0 24px; }
  th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid #d0d7de; vertical-align: top; }
  th { font-size: 13px; color: #6e7781; }
  .ok { color: #1a7f37; font-weight: 600; }
  .bad { color: #cf222e; font-weight: 600; }
  .pill { font-size: 12px; padding: 2px 8px; border-radius: 999px; white-space: nowrap; }
  .pill.ok { background: #dafbe1; color: #1a7f37; }
  .pill.bad { background: #ffebe9; color: #cf222e; }
  .pill.warn { background: #fff8c5; color: #7d4e00; }
  .pill.none { background: #eaeef2; color: #57606a; }
  .chip { display: inline-block; font-size: 11px; padding: 1px 6px; margin: 1px 2px;
          border-radius: 6px; background: #ddf4ff; color: #0969da; font-family: ui-monospace, monospace; }
  .msg { font-family: ui-monospace, monospace; font-size: 12px; color: #cf222e;
         white-space: pre-wrap; margin-top: 4px; }
  .card { border: 1px solid #d0d7de; border-radius: 8px; padding: 16px; margin: 16px 0; }
  .warnbox { border: 1px solid #d4a72c; background: #fff8c5; border-radius: 8px;
             padding: 12px 16px; margin: 16px 0; color: #7d4e00; }
  pre.mermaid { text-align: center; }
  #docnav a { display: block; padding: 2px 0; text-decoration: none; color: #0969da; }
  .doclayout { display: grid; grid-template-columns: 220px 1fr; gap: 24px; }
  #doccontent { min-width: 0; overflow-x: auto; }
  @media (max-width: 720px) { .doclayout { grid-template-columns: 1fr; } }
  /* interactive VMA visualizer */
  .vmaviz-controls { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin: 8px 0; }
  .vmaviz-btn, .vmaviz-controls select { font: inherit; padding: 4px 10px; border: 1px solid #d0d7de;
            border-radius: 6px; background: #f6f8fa; cursor: pointer; }
  .vmaviz-btn:disabled { opacity: .45; cursor: default; }
  .vmaviz-svg { width: 100%; height: auto; border: 1px solid #d0d7de; border-radius: 8px; background: #fff; }
  .vmaviz-legend { display: flex; gap: 14px; flex-wrap: wrap; margin: 10px 0; font-size: 12px; color: #57606a; }
  .vmaviz-key i { display: inline-block; width: 12px; height: 12px; border: 1px solid #374151;
            border-radius: 2px; margin-right: 5px; vertical-align: -1px; }
  .vmaviz-foot { margin-top: 8px; }
  .vmaviz-counter { font-size: 12px; color: #6e7781; font-weight: 600; margin-right: 8px; }
  .vmaviz-note { display: inline; }
  /* Overview coverage chart */
  .cov-wrap { display: flex; gap: 24px; align-items: center; flex-wrap: wrap; margin: 8px 0 20px; }
  .cov-bar { display: flex; height: 26px; width: 100%; max-width: 520px; border-radius: 6px;
             overflow: hidden; border: 1px solid #d0d7de; }
  .cov-seg { height: 100%; }
  .cov-seg.passing { background: #1a7f37; }
  .cov-seg.failing { background: #cf222e; }
  .cov-seg.partial { background: #d4a72c; }
  .cov-seg.no-test { background: #afb8c1; }
  .cov-legend { display: flex; gap: 14px; flex-wrap: wrap; font-size: 13px; }
  .cov-legend span { display: inline-flex; align-items: center; gap: 6px; }
  .cov-legend i { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
  .cov-num { font-weight: 700; font-size: 22px; }
  /* Tests pass/fail bar + filter */
  .suite-head { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; margin-top: 20px; }
  .passbar { width: 160px; height: 10px; border-radius: 5px; background: #ffd7d5; overflow: hidden;
             border: 1px solid #d0d7de; }
  .passbar > i { display: block; height: 100%; background: #1a7f37; }
  .controls { display: flex; gap: 12px; align-items: center; margin: 12px 0; font-size: 14px; }
  .controls label { cursor: pointer; user-select: none; }
  body.failing-only tr.tc-pass { display: none; }
  body.failing-only h3.suite-allpass { opacity: .5; }
  /* In-page table of contents (Docs / Team) */
  .toc { font-size: 13px; }
  .toc a { display: block; padding: 1px 0; text-decoration: none; color: #0969da; }
  .toc a.h3 { padding-left: 12px; color: #57606a; }
  .anchored { scroll-margin-top: 12px; }
  .anchored .anchor { opacity: 0; text-decoration: none; margin-left: 6px; color: #8b949e; }
  .anchored:hover .anchor { opacity: 1; }
"""

STATUS_PILL = {
    "passing": ('ok', 'covered &amp; passing'),
    "failing": ('bad', 'failing'),
    "partial": ('warn', 'partial (test missing)'),
    "no-test": ('none', 'no test'),
}


def meta_block(suites):
    total = sum(s["tests"] for s in suites)
    failed = sum(s["failures"] for s in suites)
    env = os.environ
    repo = env.get("GITHUB_REPOSITORY", "")
    sha = env.get("GITHUB_SHA", "")[:7]
    ref = env.get("GITHUB_REF_NAME", "")
    run = env.get("GITHUB_RUN_NUMBER", "")
    server = env.get("GITHUB_SERVER_URL", "https://github.com")
    run_id = env.get("GITHUB_RUN_ID", "")
    when = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    run_link = f"{server}/{repo}/actions/runs/{run_id}" if repo and run_id else ""
    parts = [f"{total - failed}/{total} tests passed across {len(suites)} suite(s)", when]
    if sha:
        parts.append(f"<code>{html.escape(ref)}</code> @ <code>{html.escape(sha)}</code>")
    if run:
        parts.append(f"run #{html.escape(run)}")
    if run_link:
        parts.append(f'<a href="{run_link}">workflow run</a>')
    return '<div class="meta">' + ' &middot; '.join(parts) + '</div>'


def page_shell(active, title, body, with_mermaid=False, src_scripts=()):
    nav_items = [("index.html", "Overview"), ("tests.html", "Tests"),
                 ("visualize.html", "Visualize"), ("docs.html", "Docs"),
                 ("team.html", "Team")]
    nav = "".join(
        f'<a href="{href}" class="{"active" if href == active else ""}">{label}</a>'
        for href, label in nav_items
    )
    scripts = ""
    if with_mermaid:
        scripts += (
            '<script type="module">\n'
            "import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';\n"
            "mermaid.initialize({ startOnLoad: true });\n"
            "</script>\n"
        )
    for s in src_scripts:
        scripts += f'<script src="{s}"></script>\n'
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>{CSS}</style>
</head>
<body>
  <header class="site">
    <h1>mmap / ld.so simulator</h1>
    <nav class="site">{nav}</nav>
  </header>
  {body}
  {scripts}
</body>
</html>
"""


def render_overview(suites, jobs, trace_rows, categories, orphan_tests):
    total = sum(s["tests"] for s in suites)
    failed = sum(s["failures"] for s in suites)
    overall_ok = failed == 0 and total > 0
    badge = ("PASS", "#1a7f37") if overall_ok else \
            (("FAIL", "#cf222e") if total else ("NO DATA", "#9a6700"))

    out = [f'<h2><span class="badge" style="background:{badge[1]}">{badge[0]}</span></h2>',
           meta_block(suites)]

    # Requirement coverage summary by category
    cat_counts = {}
    for r in trace_rows:
        c = r["category"]
        cat_counts.setdefault(c, {"passing": 0, "failing": 0, "partial": 0, "no-test": 0})
        cat_counts[c][r["status"]] += 1
    if trace_rows:
        out.append('<h2>Requirement coverage</h2>')
        # Visual coverage bar (inline CSS, no JS) summing the four statuses.
        tot = {"passing": 0, "failing": 0, "partial": 0, "no-test": 0}
        for r in trace_rows:
            tot[r["status"]] += 1
        n = len(trace_rows)
        seg = []
        for key in ("passing", "failing", "partial", "no-test"):
            if tot[key]:
                pct = 100.0 * tot[key] / n
                seg.append(f'<div class="cov-seg {key}" style="width:{pct:.4f}%" '
                           f'title="{key}: {tot[key]}"></div>')
        legend = []
        for key, lab in (("passing", "passing"), ("failing", "failing"),
                         ("partial", "partial"), ("no-test", "no test")):
            cls = key.replace("no-test", "no-test")
            legend.append(f'<span><i class="cov-seg {cls}"></i>{lab} '
                          f'<b>{tot[key]}</b></span>')
        out.append(
            '<div class="cov-wrap">'
            f'<div><div class="cov-num">{tot["passing"]}/{n}</div>'
            '<div class="meta" style="margin:0">requirements covered &amp; passing</div></div>'
            f'<div style="flex:1;min-width:260px"><div class="cov-bar">{"".join(seg)}</div>'
            f'<div class="cov-legend" style="margin-top:8px">{"".join(legend)}</div></div>'
            '</div>'
        )
        out.append('<table><thead><tr><th>Category</th><th>Passing</th>'
                   '<th>Failing</th><th>Partial</th><th>No&nbsp;test</th></tr></thead><tbody>')
        for cat, label in categories.items():
            cc = cat_counts.get(cat, {"passing": 0, "failing": 0, "partial": 0, "no-test": 0})
            out.append(
                f'<tr><td>{html.escape(cat)} &mdash; {html.escape(label)}</td>'
                f'<td class="ok">{cc["passing"]}</td>'
                f'<td class="{"bad" if cc["failing"] else ""}">{cc["failing"]}</td>'
                f'<td>{cc["partial"]}</td><td>{cc["no-test"]}</td></tr>'
            )
        out.append('</tbody></table>')

        # Traceability matrix
        out.append('<h2>Traceability matrix</h2>')
        out.append('<p class="meta">Each requirement maps to the test(s) that exercise it; '
                   'status reflects the latest JUnit results.</p>')
        out.append('<table><thead><tr><th>ID</th><th>Requirement</th>'
                   '<th>Status</th><th>Covering tests</th></tr></thead><tbody>')
        for r in trace_rows:
            cls, label = STATUS_PILL[r["status"]]
            tests_html = []
            for t in r.get("tests", []):
                miss = t in r["missing"]
                fail = t in r["failing"]
                anchor = "" if miss else f'href="tests.html#{slug(t)}"'
                style = "bad" if fail else ("warn" if miss else "")
                suffix = " (missing)" if miss else (" (failing)" if fail else "")
                tag = "a" if anchor else "span"
                tests_html.append(
                    f'<{tag} {anchor} class="chip {style}">{html.escape(t)}{suffix}</{tag}>'
                )
            src = r.get("source", "")
            src_html = f'<div class="meta">{html.escape(src)}</div>' if src else ""
            out.append(
                f'<tr id="{slug(r["id"])}"><td><code>{html.escape(r["id"])}</code></td>'
                f'<td>{html.escape(r["title"])}{src_html}</td>'
                f'<td><span class="pill {cls}">{label}</span></td>'
                f'<td>{" ".join(tests_html) or "&mdash;"}</td></tr>'
            )
        out.append('</tbody></table>')

    # Consistency warnings (orphan tests / missing test names)
    missing_names = sorted({t for r in trace_rows for t in r["missing"]})
    if orphan_tests or missing_names:
        out.append('<div class="warnbox"><strong>Traceability warnings</strong><ul>')
        for t in missing_names:
            out.append(f'<li>Requirement references unknown test: <code>{html.escape(t)}</code></li>')
        for t in orphan_tests:
            out.append(f'<li>Test not mapped to any requirement: <code>{html.escape(t)}</code></li>')
        out.append('</ul></div>')

    # Job graph
    out.append('<div class="card"><h2>CI job dependency graph</h2>'
               f'<pre class="mermaid">\n{mermaid_graph(jobs)}\n</pre></div>')
    return page_shell("index.html", "mmap simulator - Overview",
                      "\n".join(out), with_mermaid=True)


def render_tests(suites, rev):
    out = ['<h2>Test results</h2>', meta_block(suites)]
    if not suites:
        out.append("<p>No JUnit reports found.</p>")
    else:
        out.append(
            '<div class="controls"><label><input type="checkbox" id="failonly"> '
            'Show failing only</label></div>'
        )
    for s in suites:
        ok = s["failures"] == 0
        passed = s["tests"] - s["failures"]
        pct = (100.0 * passed / s["tests"]) if s["tests"] else 0.0
        h3_cls = ' class="suite-allpass"' if ok else ""
        pill_cls = "ok" if ok else "bad"
        out.append(
            '<div class="suite-head">'
            f'<h3{h3_cls} style="margin:0">{html.escape(s["name"])}</h3>'
            f'<span class="pill {pill_cls}">{passed}/{s["tests"]} passed</span>'
            f'<span class="passbar" title="{pct:.0f}% passing">'
            f'<i style="width:{pct:.4f}%"></i></span>'
            '</div>'
        )
        out.append('<table><thead><tr><th>Test</th><th>Covers</th>'
                   '<th>Checks</th><th>Result</th></tr></thead><tbody>')
        for c in s["cases"]:
            res = '<span class="ok">PASS</span>' if not c["failed"] \
                  else '<span class="bad">FAIL</span>'
            row_cls = "tc-pass" if not c["failed"] else "tc-fail"
            detail = ""
            if c["failed"] and c["message"].strip():
                detail = f'<div class="msg">{html.escape(c["message"].strip())}</div>'
            covers = " ".join(
                f'<a class="chip" href="index.html#{slug(rid)}">{html.escape(rid)}</a>'
                for rid in rev.get(c["name"], [])
            ) or "&mdash;"
            out.append(
                f'<tr id="{slug(c["name"])}" class="{row_cls}">'
                f'<td>{html.escape(c["name"])}{detail}</td>'
                f'<td>{covers}</td>'
                f'<td>{html.escape(c["assertions"])}</td><td>{res}</td></tr>'
            )
        out.append("</tbody></table>")
    # Progressive-enhancement filter: toggles a body class; no JS = all visible.
    out.append(
        '<script>\n'
        'var fo=document.getElementById("failonly");\n'
        'if(fo){fo.addEventListener("change",function(){'
        'document.body.classList.toggle("failing-only",fo.checked);});}\n'
        '</script>'
    )
    return page_shell("tests.html", "mmap simulator - Tests", "\n".join(out))


def render_visualize():
    """Interactive page: step through VMA address-space scenarios (vma_viz.js)."""
    body = """
  <h2>Interactive address-space visualizer</h2>
  <p class="meta">Step through how the simulator's operations reshape the VMA
  list. Pick a scenario, then use Prev / Next / Play. Scenarios mirror
  <code>docs/ldso_sequence.md</code> and the operation semantics in
  <code>docs/mmap_model_spec.md</code>.</p>
  <div id="vmaviz"></div>
"""
    return page_shell("visualize.html", "mmap simulator - Visualize", body,
                      src_scripts=("vma_viz.js",))


# Shared client-side Markdown renderer (marked + mermaid via CDN) with an
# auto-generated in-page TOC and anchored headings. Used by Docs and Team.
MD_RENDER_JS = """
  <script type="module">
    import { marked } from 'https://cdn.jsdelivr.net/npm/marked@12/lib/marked.esm.js';
    import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
    mermaid.initialize({ startOnLoad: false });
    const content = document.getElementById('doccontent');
    const tocEl = document.getElementById('toc');
    function slugify(t) {
      return t.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
    }
    async function load(path) {
      try {
        const r = await fetch(path);
        if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
        content.innerHTML = marked.parse(await r.text());
        // ```mermaid fences -> <pre class="mermaid"> then render.
        const blocks = content.querySelectorAll('code.language-mermaid');
        blocks.forEach(c => {
          const pre = document.createElement('pre');
          pre.className = 'mermaid';
          pre.textContent = c.textContent;
          c.parentElement.replaceWith(pre);
        });
        if (blocks.length) {
          try { await mermaid.run({ querySelector: '#doccontent pre.mermaid' }); }
          catch (err) { /* leave source visible on failure */ }
        }
        // Anchor headings + build a TOC.
        if (tocEl) tocEl.innerHTML = '';
        const used = {};
        content.querySelectorAll('h2, h3').forEach(h => {
          let id = slugify(h.textContent) || 'sec';
          if (used[id] != null) { used[id]++; id = id + '-' + used[id]; } else used[id] = 0;
          h.id = id;
          h.classList.add('anchored');
          const a = document.createElement('a');
          a.href = '#' + id; a.className = 'anchor'; a.textContent = '#';
          h.appendChild(a);
          if (tocEl) {
            const link = document.createElement('a');
            link.href = '#' + id; link.textContent = h.textContent.replace(/#$/, '');
            link.className = h.tagName === 'H3' ? 'h3' : 'h2';
            tocEl.appendChild(link);
          }
        });
      } catch (e) {
        content.innerHTML = '<p class="bad">Failed to load ' + path + ': ' + e.message +
          '<br>(serve the site over HTTP, e.g. <code>python3 -m http.server -d site</code>)</p>';
      }
    }
    const navLinks = document.querySelectorAll('#docnav a');
    navLinks.forEach(a => {
      a.addEventListener('click', ev => {
        ev.preventDefault();
        navLinks.forEach(x => x.classList.remove('active'));
        a.classList.add('active');
        load(a.dataset.doc);
      });
    });
    const firstLink = document.querySelector('#docnav a');
    if (firstLink) { firstLink.classList.add('active'); load(firstLink.dataset.doc); }
  </script>
"""


def render_docs(doc_files):
    """Docs page renders Markdown client-side via marked + mermaid (CDN)."""
    nav = "".join(
        f'<a href="#" data-doc="docs/{html.escape(fn)}">{html.escape(fn)}</a>'
        for fn in doc_files
    )
    body = f"""
  <h2>Design documents</h2>
  <p class="meta">Rendered from the repository's <code>docs/*.md</code> sources.</p>
  <div class="doclayout">
    <nav id="docnav">{nav}</nav>
    <article id="doccontent">Select a document.</article>
    <nav id="toc" class="toc"></nav>
  </div>
  {MD_RENDER_JS}
"""
    return page_shell("docs.html", "mmap simulator - Docs", body)


def render_team(team_doc):
    """Team page: render the agent-team design doc (docs/agent_team.md) with the
    same Markdown+Mermaid+TOC pipeline as Docs, but pinned to a single file."""
    if not team_doc:
        body = '<h2>AI Agent Team</h2><p>docs/agent_team.md not found.</p>'
        return page_shell("team.html", "mmap simulator - Team", body)
    # Hidden single-entry docnav so the shared MD_RENDER_JS loads this file.
    nav = (f'<a href="#" data-doc="docs/{html.escape(team_doc)}" '
           f'style="display:none"></a>')
    body = f"""
  <h2>AI Agent Team</h2>
  <p class="meta">How this repository is built and maintained by a team of
  Claude Code agents and skills. Source: <code>docs/{html.escape(team_doc)}</code>.</p>
  <div class="doclayout">
    <nav id="docnav">{nav}</nav>
    <article id="doccontent">Loading…</article>
    <nav id="toc" class="toc"></nav>
  </div>
  {MD_RENDER_JS}
"""
    return page_shell("team.html", "mmap simulator - Team", body)


def mermaid_graph(jobs):
    lines = ["graph LR"]
    for job in jobs:
        node = job.replace("-", "_")
        lines.append(f'  {node}["{job}"]')
    for job, needs in jobs.items():
        node = job.replace("-", "_")
        for dep in needs:
            lines.append(f'  {dep.replace("-", "_")} --> {node}')
    return "\n".join(lines)


def slug(text):
    return re.sub(r"[^a-zA-Z0-9]+", "-", text).strip("-").lower()


# ----------------------------- main ---------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reports", default="reports")
    ap.add_argument("--workflow", default=".github/workflows/ci.yml")
    ap.add_argument("--requirements", default="docs/requirements.json")
    ap.add_argument("--docs", default="docs")
    ap.add_argument("--out", default="site")
    args = ap.parse_args()

    suites = parse_junit(args.reports)
    jobs = parse_job_graph(args.workflow)
    reqs = parse_requirements(args.requirements)
    test_idx = index_tests(suites)
    trace_rows, orphan_tests = compute_traceability(reqs, test_idx)

    # Surface and copy the markdown docs into the site so fetch() can read them.
    doc_files = []
    if args.docs and os.path.isdir(args.docs):
        doc_files = sorted(fn for fn in os.listdir(args.docs) if fn.endswith(".md"))
        dest = os.path.join(args.out, "docs")
        os.makedirs(dest, exist_ok=True)
        for fn in doc_files:
            shutil.copyfile(os.path.join(args.docs, fn), os.path.join(dest, fn))

    os.makedirs(args.out, exist_ok=True)

    # Copy the interactive visualizer asset alongside the pages (served as a
    # plain <script src>; zero-dependency, no CDN). Sits next to this script.
    viz_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vma_viz.js")
    if os.path.isfile(viz_src):
        shutil.copyfile(viz_src, os.path.join(args.out, "vma_viz.js"))

    team_doc = "agent_team.md" if "agent_team.md" in doc_files else ""
    rev = reverse_map(trace_rows)
    pages = {
        "index.html": render_overview(suites, jobs, trace_rows,
                                       reqs["categories"], orphan_tests),
        "tests.html": render_tests(suites, rev),
        "visualize.html": render_visualize(),
        "docs.html": render_docs(doc_files),
        "team.html": render_team(team_doc),
    }
    for name, content in pages.items():
        with open(os.path.join(args.out, name), "w", encoding="utf-8") as f:
            f.write(content)

    # Surface consistency problems on stderr (non-fatal) for CI logs.
    missing_names = sorted({t for r in trace_rows for t in r["missing"]})
    for t in missing_names:
        print(f"warn: requirement references unknown test: {t!r}", file=sys.stderr)
    for t in orphan_tests:
        print(f"warn: test not mapped to any requirement: {t!r}", file=sys.stderr)

    print(f"wrote {args.out}/: {len(suites)} suite(s), {len(jobs)} job(s), "
          f"{len(trace_rows)} requirement(s), {len(doc_files)} doc(s); "
          f"{len(missing_names)} missing-test + {len(orphan_tests)} orphan-test warning(s)")


if __name__ == "__main__":
    main()
