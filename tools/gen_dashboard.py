#!/usr/bin/env python3
"""
gen_dashboard.py - Build a static results dashboard for GitHub Pages.

Inputs:
  --reports DIR    directory of JUnit XML files (as emitted by tests/test_harness.h)
  --workflow FILE  the CI workflow YAML, mined for the job-dependency graph
  --out DIR        output directory (an index.html is written there)

Stdlib only (xml.etree + a tiny indentation-aware scan of the workflow), so it
runs in CI and locally without third-party packages - matching the project's
zero-dependency ethos.
"""
import argparse
import html
import os
import re
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone


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
        # root may be <testsuites> or a single <testsuite>
        ts_nodes = root.iter("testsuite")
        for ts in ts_nodes:
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

    Handles `needs: a`, `needs: [a, b]`, and a block list of `- a` items.
    Kept dependency-free; the workflow's structure is regular and ours."""
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
        # leaving the jobs: block (a new top-level key at column 0)
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
                pending_list = True  # block list follows
            continue
        if pending_list:
            m_item = re.match(r"^      -\s*(.+)$", line)
            if m_item:
                jobs[cur].append(m_item.group(1).strip().strip("\"'"))
                continue
            pending_list = False
    return jobs


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


def render(suites, jobs):
    total = sum(s["tests"] for s in suites)
    failed = sum(s["failures"] for s in suites)
    overall_ok = failed == 0 and total > 0
    badge = ("PASS", "#1a7f37") if overall_ok else \
            (("FAIL", "#cf222e") if total else ("NO DATA", "#9a6700"))

    env = os.environ
    repo = env.get("GITHUB_REPOSITORY", "")
    sha = env.get("GITHUB_SHA", "")[:7]
    ref = env.get("GITHUB_REF_NAME", "")
    run = env.get("GITHUB_RUN_NUMBER", "")
    server = env.get("GITHUB_SERVER_URL", "https://github.com")
    run_id = env.get("GITHUB_RUN_ID", "")
    when = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    run_link = f"{server}/{repo}/actions/runs/{run_id}" if repo and run_id else ""

    rows = []
    for s in suites:
        ok = s["failures"] == 0
        rows.append(
            f'<h3>{html.escape(s["name"])} '
            f'<span class="pill {"ok" if ok else "bad"}">'
            f'{s["tests"] - s["failures"]}/{s["tests"]} passed</span></h3>'
        )
        rows.append('<table><thead><tr><th>Test</th><th>Checks</th><th>Result</th></tr></thead><tbody>')
        for c in s["cases"]:
            res = '<span class="ok">PASS</span>' if not c["failed"] \
                  else '<span class="bad">FAIL</span>'
            detail = ""
            if c["failed"] and c["message"].strip():
                detail = f'<div class="msg">{html.escape(c["message"].strip())}</div>'
            rows.append(
                f'<tr><td>{html.escape(c["name"])}{detail}</td>'
                f'<td>{html.escape(c["assertions"])}</td><td>{res}</td></tr>'
            )
        rows.append("</tbody></table>")
    suites_html = "\n".join(rows) if suites else "<p>No JUnit reports found.</p>"

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mmap simulator - CI results</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
         margin: 0 auto; max-width: 960px; padding: 24px; line-height: 1.5; }}
  h1 {{ margin-bottom: 4px; }}
  .meta {{ color: #6e7781; font-size: 14px; margin-bottom: 16px; }}
  .meta a {{ color: inherit; }}
  .badge {{ display: inline-block; padding: 4px 12px; border-radius: 999px;
            color: #fff; font-weight: 700; background: {badge[1]}; }}
  table {{ border-collapse: collapse; width: 100%; margin: 8px 0 24px; }}
  th, td {{ text-align: left; padding: 6px 10px; border-bottom: 1px solid #d0d7de; vertical-align: top; }}
  th {{ font-size: 13px; color: #6e7781; }}
  .ok {{ color: #1a7f37; font-weight: 600; }}
  .bad {{ color: #cf222e; font-weight: 600; }}
  .pill {{ font-size: 12px; padding: 2px 8px; border-radius: 999px; }}
  .pill.ok {{ background: #dafbe1; color: #1a7f37; }}
  .pill.bad {{ background: #ffebe9; color: #cf222e; }}
  .msg {{ font-family: ui-monospace, monospace; font-size: 12px; color: #cf222e;
          white-space: pre-wrap; margin-top: 4px; }}
  .card {{ border: 1px solid #d0d7de; border-radius: 8px; padding: 16px; margin: 16px 0; }}
  pre.mermaid {{ text-align: center; }}
</style>
</head>
<body>
  <h1>mmap / ld.so simulator &mdash; CI results</h1>
  <div class="meta">
    <span class="badge">{badge[0]}</span>
    &nbsp; {total - failed}/{total} tests passed across {len(suites)} suite(s)
    &middot; {when}
    {f'&middot; <code>{html.escape(ref)}</code> @ <code>{html.escape(sha)}</code>' if sha else ''}
    {f'&middot; run #{html.escape(run)}' if run else ''}
    {f'&middot; <a href="{run_link}">workflow run</a>' if run_link else ''}
  </div>

  <div class="card">
    <h2>Job dependency graph</h2>
    <pre class="mermaid">
{mermaid_graph(jobs)}
    </pre>
  </div>

  <h2>Test results</h2>
  {suites_html}

  <script type="module">
    import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
    mermaid.initialize({{ startOnLoad: true }});
  </script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reports", default="reports")
    ap.add_argument("--workflow", default=".github/workflows/ci.yml")
    ap.add_argument("--out", default="site")
    args = ap.parse_args()

    suites = parse_junit(args.reports)
    jobs = parse_job_graph(args.workflow)
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, "index.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(render(suites, jobs))
    print(f"wrote {out}: {len(suites)} suite(s), {len(jobs)} job(s)")


if __name__ == "__main__":
    main()
