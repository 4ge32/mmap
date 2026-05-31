#!/usr/bin/env python3
"""
check_agent_team_doc.py - Guard against drift between the agent-team design doc
(docs/agent_team.md, rendered on the Pages "Team" tab) and the actual agent and
skill definitions under .claude/.

Fails (exit 1) if any agent in .claude/agents/*.md or any skill in
.claude/skills/*/SKILL.md is not mentioned by name in docs/agent_team.md.
Stdlib only; run by CI and locally.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "agent_team.md")
AGENTS_DIR = os.path.join(ROOT, ".claude", "agents")
SKILLS_DIR = os.path.join(ROOT, ".claude", "skills")


def frontmatter_name(path):
    """Return the `name:` from a markdown frontmatter block, or the filename."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return None
    m = re.search(r"^name:\s*(\S+)\s*$", text, re.MULTILINE)
    if m:
        return m.group(1)
    return os.path.splitext(os.path.basename(path))[0]


def main():
    if not os.path.isfile(DOC):
        print(f"error: {DOC} is missing", file=sys.stderr)
        return 1
    doc = open(DOC, encoding="utf-8").read()

    expected = []
    if os.path.isdir(AGENTS_DIR):
        for fn in sorted(os.listdir(AGENTS_DIR)):
            if fn.endswith(".md"):
                expected.append(frontmatter_name(os.path.join(AGENTS_DIR, fn)))
    if os.path.isdir(SKILLS_DIR):
        for d in sorted(os.listdir(SKILLS_DIR)):
            sk = os.path.join(SKILLS_DIR, d, "SKILL.md")
            if os.path.isfile(sk):
                expected.append(frontmatter_name(sk))

    missing = [name for name in expected if name and name not in doc]
    if missing:
        print("error: docs/agent_team.md does not mention these agents/skills:",
              file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)
        print("Update docs/agent_team.md (design-visualizer owns it) to keep the "
              "Team page in sync with .claude/.", file=sys.stderr)
        return 1

    print(f"agent-team doc OK: all {len(expected)} agents/skills referenced "
          "in docs/agent_team.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
