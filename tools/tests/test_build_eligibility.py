import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/build.yml"

HARNESS = r"""
const fs = require('fs');
const scenario = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const script = fs.readFileSync(process.argv[3], 'utf8');
const calls = [];
const outputs = {};
const warnings = [];
let summary = '';
const github = {
  rest: {
    pulls: {
      get: async () => ({ data: { state: 'open', base: { ref: 'main' }, head: { sha: 'head' }, mergeable: true, user: { login: 'author' } } }),
      listFiles: async () => ({ data: [] })
    },
    repos: {
      listCommits: async (params) => {
        calls.push(['listCommits', params.sha, params.author]);
        return { data: scenario.commits };
      },
      getBranch: async () => ({ data: { commit: { sha: 'main' } } }),
      getCollaboratorPermissionLevel: async () => ({ data: { permission: 'write' } })
    },
    search: {
      issuesAndPullRequests: async (params) => {
        calls.push(['search', params.q]);
        if (scenario.search === null) {
          throw Object.assign(new Error('API rate limit exceeded'), { status: 403 });
        }
        return { data: scenario.search };
      }
    }
  },
  paginate: async () => scenario.files.map((filename) => ({ filename }))
};
const context = {
  eventName: scenario.manual ? 'workflow_dispatch' : 'pull_request',
  actor: 'maintainer',
  repo: { owner: 'owner', repo: 'project' },
  payload: { pull_request: { number: 7, head: { sha: 'head' }, user: { login: 'author' } } }
};
const core = {
  setOutput: (name, value) => { outputs[name] = value; },
  info: () => {},
  warning: (message) => { warnings.push(String(message)); },
  summary: { addRaw: (text) => { summary += text; return { write: async () => {} }; } }
};
const AsyncFunction = Object.getPrototypeOf(async () => {}).constructor;
new AsyncFunction('github', 'context', 'core', script)(github, context, core).then(
  () => process.stdout.write(JSON.stringify({ outputs, warnings, calls, summary })),
  (error) => { console.error(error); process.exit(1); }
);
"""


def gate_script():
    lines = WORKFLOW.read_text(encoding="utf-8").splitlines()
    start = next(index for index, line in enumerate(lines) if line.strip() == "id: gate")
    start = next(index for index in range(start, len(lines)) if lines[index].strip() == "script: |") + 1
    indent = len(lines[start]) - len(lines[start].lstrip())
    script = []
    for line in lines[start:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        script.append(line[indent:])
    return "\n".join(script)


class BuildEligibilityTests(unittest.TestCase):
    def setUp(self):
        self.node = shutil.which("node")
        if not self.node:
            self.skipTest("the eligibility script runs in node")
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "harness.js").write_text(HARNESS, encoding="utf-8")
        (self.root / "gate.js").write_text(gate_script(), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_gate(self, files=("core/relinker/main.cpp",), manual=False):
        scenario = self.root / "scenario.json"
        scenario.write_text(json.dumps({"commits": [], "search": None, "files": list(files), "manual": manual}),
                            encoding="utf-8")
        result = subprocess.run([self.node, "harness.js", "scenario.json", "gate.js"], cwd=self.root,
                                capture_output=True, text=True, env=dict(os.environ), timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_code_change_runs_every_build(self):
        result = self.run_gate()
        self.assertIs(result["outputs"]["code"], True)
        self.assertIn("changes code", result["summary"])
        self.assertEqual(result["calls"], [])

    def test_documentation_only_skips_the_builds(self):
        result = self.run_gate(files=("README.md", "docs/research/cpu-chain.md", "core/libs/notes.md", "NOTICE",
                                      ".github/ISSUE_TEMPLATE/bug.yml", ".github/CODEOWNERS"))
        self.assertIs(result["outputs"]["code"], False)
        self.assertIn("documentation only", result["summary"])

    def test_one_code_file_among_documentation_runs_every_build(self):
        result = self.run_gate(files=("README.md", "docs/dev/BUILD.md", "tools/check_conventions.py"))
        self.assertIs(result["outputs"]["code"], True)

    def test_files_beside_documentation_count_as_code(self):
        for path in (".github/workflows/build.yml", ".github/dependabot.yml", "docs.cmake", "LICENSE.txt"):
            with self.subTest(path=path):
                self.assertIs(self.run_gate(files=(path,))["outputs"]["code"], True)

    def test_pull_request_without_files_runs_every_build(self):
        self.assertIs(self.run_gate(files=())["outputs"]["code"], True)

    def test_manual_run_builds_without_reading_the_pull_request(self):
        result = self.run_gate(files=("README.md",), manual=True)
        self.assertIs(result["outputs"]["code"], True)
        self.assertEqual(result["calls"], [])


if __name__ == "__main__":
    unittest.main()
