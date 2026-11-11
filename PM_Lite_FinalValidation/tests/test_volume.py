import unittest
from volume import require_identity, identify
import tempfile
class VolumeTests(unittest.TestCase):
    def test_identity_not_drive_letter(self):
        a=dict(guid="g",serial="s",marker_sha256="m",root="E:")
        self.assertTrue(require_identity(a,dict(a,root="F:")))
        for key in ("guid","serial","marker_sha256"):
            with self.subTest(key=key), self.assertRaises(ValueError): require_identity(a,dict(a,**{key:"other"}))
    def test_missing_identity_rejected(self):
        with self.assertRaises(ValueError): require_identity({}, {})

    def test_missing_marker_rejected_without_creation(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaises(ValueError): identify(root)
