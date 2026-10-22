import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_conventions import Check

EXPORT = "core/libs/prx/libSceExample/Export.cpp"


def repository(root):
    def git(*args):
        subprocess.run(["git", "-c", f"safe.directory={root}", "-c", "user.name=Conventions Test",
                        "-c", "user.email=test@example.com", "-c", "commit.gpgsign=false", *args],
                       cwd=root, capture_output=True, check=True)

    def write(path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    git("init", "-q")
    write("core/libs/prx/libSceOther/src/Other.cpp", "int APS5_VABI sceShared() { return now(); }\n")
    git("add", ".")
    git("commit", "-qm", "docs: add base")
    git("tag", "base")
    write(EXPORT, "int APS5_VABI sceShared() { return now(); }\n")
    git("add", "-A")
    git("commit", "-qm", "feat: add example")


class DuplicatePatternTests(unittest.TestCase):
    def test_many_new_exports_fit_the_duplicate_search(self):
        exports = {f"sceGenerated{index:06d}": (EXPORT, 1) for index in range(8000)}
        exports["sceShared"] = (EXPORT, 1)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repository(root)
            previous = os.getcwd()
            os.chdir(root)
            try:
                check = Check("base", "HEAD")
                check.duplicates(exports)
            finally:
                os.chdir(previous)
        self.assertEqual(check.findings, [("duplicate-export", EXPORT, 1, "sceShared is also in libSceOther")])


if __name__ == "__main__":
    unittest.main()
