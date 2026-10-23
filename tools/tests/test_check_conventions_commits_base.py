import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_conventions import Check


def repository(root):
    def git(*args):
        subprocess.run(["git", "-c", f"safe.directory={root}", "-c", "user.name=Conventions Test",
                        "-c", "user.email=test@example.com", "-c", "commit.gpgsign=false", *args],
                       cwd=root, capture_output=True, check=True)

    def commit(path, subject):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"# {subject}\n", encoding="utf-8")
        git("add", "-A")
        git("commit", "-qm", subject)

    git("init", "-q", "-b", "main")
    commit("README.md", "docs: add base")
    git("tag", "start")
    commit("docs/dev/Fork.md", "Fork work without a type")
    git("tag", "fork")
    git("checkout", "-q", "-b", "upstream", "start")
    commit("docs/dev/Upstream.md", "feat: upstream work")
    git("tag", "upstream")
    git("checkout", "-q", "main")
    git("merge", "-q", "--no-ff", "-m", "Merge upstream", "upstream")
    commit("docs/dev/Realign.md", "docs: describe the realignment")


class CommitsBaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        repository(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def subjects(self, base, commits_base=None):
        previous = os.getcwd()
        os.chdir(self.root)
        try:
            check = Check(base, "HEAD", commits_base)
            check.commits()
        finally:
            os.chdir(previous)
        return [finding[3] for finding in check.findings if finding[0] == "commit-subject"]

    def test_realignment_checks_the_subjects_it_brings(self):
        self.assertEqual(self.subjects("upstream", "fork"), [])

    def test_without_a_commits_base_the_older_fork_commit_is_reported(self):
        subjects = self.subjects("upstream")
        self.assertEqual(len(subjects), 1, subjects)
        self.assertIn("Fork work without a type", subjects[0])


if __name__ == "__main__":
    unittest.main()
