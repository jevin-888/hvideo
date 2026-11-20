"""Compile and execute the native version and transaction policy without devices."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class VersionPolicyTests(unittest.TestCase):
    def test_native_version_and_pending_transaction_policy(self):
        with tempfile.TemporaryDirectory(prefix="hsvj-rom-policy-") as directory:
            root = Path(directory)
            source = root / "policy.cpp"
            source.write_text('''#include "version_policy.h"
#include <cassert>
int main() {
    for (bool downgrade : {false, true}) {
        assert(hsvj_rom::version_allowed(49, 50, downgrade));
        assert(hsvj_rom::version_allowed(49, 49, downgrade));
        assert(hsvj_rom::version_allowed(49, 48, downgrade) == downgrade);
        assert(!hsvj_rom::version_allowed(49, 0, downgrade));
        assert(!hsvj_rom::version_allowed(0, 49, downgrade));
    }
    for (const auto& phase : {"armed", "installing", "installed"})
        assert(hsvj_rom::upgrade_pending(phase));
    for (const auto& phase : {"succeeded", "failed", ""})
        assert(!hsvj_rom::upgrade_pending(phase));
}
''', encoding="utf-8")
            executable = root / "policy"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(Path(__file__).resolve().parent), str(source),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
