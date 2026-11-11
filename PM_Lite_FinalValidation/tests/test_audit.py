import tempfile
import unittest
from pathlib import Path
from audit import snapshot,compare,metadata_snapshot
class AuditTests(unittest.TestCase):
    def test_changes(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d); (p/"changed").write_text("a"); (p/"deleted").write_text("b")
            a=snapshot([p]); (p/"changed").write_text("c"); (p/"deleted").unlink(); (p/"new").write_text("d")
            delta=compare(a,snapshot([p]))
            for k,n in (("new","new"),("deleted","deleted"),("modified","changed")): self.assertEqual(delta[k],[str(p/n)])
            self.assertTrue(delta["complete"])
    def test_errors_preclude_clean(self):
        a={"files":{},"errors":["locked"]}
        self.assertFalse(compare(a,{"files":{},"errors":[]})["complete"])

    def test_broad_metadata_has_no_content(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"example.txt"; path.write_text("synthetic confidential content")
            r=metadata_snapshot([d])
            self.assertEqual(r["mode"],"metadata_only")
            self.assertEqual(set(r["files"][str(path)]),{"size","mtime_ns","ctime_ns"})
            self.assertIn(d,r["directories"])
            self.assertEqual(r["errors"],[])
