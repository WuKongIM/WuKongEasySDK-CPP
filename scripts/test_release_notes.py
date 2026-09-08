"""Publishing must fail before artifacts exist for invalid release notes/tags."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import prebuilt


class ReleaseNotesTests(unittest.TestCase):
    def check(self, changelog, tag='v0.1.0'):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'CMakeLists.txt').write_text('project(WuKongEasySDK VERSION 0.1.0 LANGUAGES CXX)')
            (root / 'CHANGELOG.md').write_text(changelog)
            with patch.object(prebuilt, 'ROOT', root):
                return prebuilt.release_notes(tag)

    def test_exact_version(self):
        self.assertEqual(self.check('## [0.1.0] - 2026-09-08\n\n- Ship SDK.\n'), ('0.1.0', '- Ship SDK.'))

    def test_missing_empty_duplicate_malformed_and_wrong_tag(self):
        valid = '## [0.1.0] - 2026-09-08\n\n- Ship SDK.\n'
        for text in ('## Unreleased\n- Pending\n', '## [0.1.0] - 2026-09-08\n',
                     valid + valid, valid + '## [0.1.0] - invalid\n- Duplicate\n',
                     '## [0.1.0]\n- Missing date\n', '## [0.1.0] - 2026-99-99\n- Bad date\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.check(text)
        with self.assertRaises(ValueError):
            self.check(valid, 'v0.2.0')


if __name__ == '__main__':
    unittest.main()
