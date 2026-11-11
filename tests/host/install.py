# contract: install.sh --crossover <bottle> [app] gives the bottle a root of its own, links to CrossOver's files with
# the release's libraries as lib/dxmt, names that root in the bottle's environment with DXMT chosen, and keeps the
# bottle's first configuration beside it. what it does not find, the bottle or a CrossOver with DXMT, it names, and
# then changes nothing
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(sys.argv.pop())


class Install(unittest.TestCase):
    def setUp(self):
        self.home = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.home)
        self.cx = self.home / "CrossOver.app/Contents/SharedSupport/CrossOver"
        for path in ["bin/wine", "share/note", "lib/wine/ntdll.so", "lib/dxmt/x86_64-windows/d3d11.dll"]:
            (self.cx / path).parent.mkdir(parents=True, exist_ok=True)
            (self.cx / path).write_text("CrossOver's")
        self.release = self.home / "release"
        for path in ["x86_64-windows/d3d12.dll", "x86_64-unix/winemetal.so", "aarch64-windows/d3d12.dll"]:
            (self.release / path).parent.mkdir(parents=True, exist_ok=True)
            (self.release / path).write_text("Mullion's")
        shutil.copy(SCRIPT, self.release / "install.sh")
        self.bottles = self.home / "Library/Application Support/CrossOver/Bottles"

    def bottle(self, name, text):
        conf = self.bottles / name / "cxbottle.conf"
        conf.parent.mkdir(parents=True)
        conf.write_text(text)
        return conf

    def install(self, *arguments):
        return subprocess.run(["sh", self.release / "install.sh", "--crossover", *arguments], capture_output=True,
                              text=True, env={"HOME": str(self.home), "PATH": "/usr/bin:/bin"})

    def test_installs(self):
        before = ('[Bottle]\n"Version" = "26.3"\n\n[EnvironmentVariables]\n"WINEMSYNC" = "1"\n'
                  '"CX_GRAPHICS_BACKEND" = "d3dmetal"\n')
        for name, text in [("Steam", before), ("My Games", '[Bottle]\n"Version" = "26.3"\n')]:
            conf = self.bottle(name, text)
            root = self.home / "Library/Mullion/Bottles" / name
            for _ in range(2):
                done = self.install(name, self.home / "CrossOver.app")
                self.assertEqual(done.returncode, 0, done.stderr)
                lines = conf.read_text().splitlines()
                at = lines.index("[EnvironmentVariables]")
                self.assertEqual(lines[at + 1:at + 3], ['"CX_GRAPHICS_BACKEND" = "dxmt"', f'"CX_ROOT" = "{root}"'])
                self.assertEqual(sum("CX_GRAPHICS_BACKEND" in line or "CX_ROOT" in line for line in lines), 2)
                self.assertEqual([line for line in lines if "CX_" not in line and line != "[EnvironmentVariables]"],
                                 [line for line in text.splitlines() if "CX_" not in line and line != "[EnvironmentVariables]"])
                self.assertEqual((conf.parent / "cxbottle.conf.before-mullion").read_text(), text)
                for linked in ["bin", "share", "lib/wine"]:
                    self.assertEqual((root / linked).resolve(), (self.cx / linked).resolve())
                    self.assertTrue((root / linked).is_symlink())
                self.assertEqual({str(path.relative_to(root / "lib/dxmt")) for path in (root / "lib/dxmt").rglob("*.*")},
                                 {"x86_64-windows/d3d12.dll", "x86_64-unix/winemetal.so", "aarch64-windows/d3d12.dll"})
                self.assertEqual((root / "lib/dxmt/x86_64-windows/d3d12.dll").read_text(), "Mullion's")
                self.assertEqual((self.cx / "lib/dxmt/x86_64-windows/d3d11.dll").read_text(), "CrossOver's")

    def test_names_what_it_does_not_find(self):
        text = '[Bottle]\n"Version" = "26.3"\n'
        conf = self.bottle("My Games", text)
        # a name with a space that was not quoted arrives as its first word
        for arguments in [("My", "Games"), ("Steam",), ()]:
            done = self.install(*arguments)
            self.assertNotEqual(done.returncode, 0)
            if arguments:
                self.assertIn(f'"{arguments[0]}"', done.stderr)
                self.assertIn("My Games", done.stderr)
        # a CrossOver without DXMT, and none where it is looked for
        shutil.rmtree(self.cx / "lib/dxmt")
        for arguments in [("My Games", self.home / "CrossOver.app"), ("My Games",)]:
            done = self.install(*arguments)
            self.assertNotEqual(done.returncode, 0)
            self.assertIn("DXMT", done.stderr)
            self.assertNotIn("usage", done.stderr)
            if len(arguments) > 1:
                self.assertIn(str(arguments[1]), done.stderr)
        self.assertEqual(conf.read_text(), text)
        self.assertFalse((conf.parent / "cxbottle.conf.before-mullion").exists())
        self.assertFalse((self.home / "Library/Mullion").exists())


if __name__ == "__main__":
    unittest.main()
