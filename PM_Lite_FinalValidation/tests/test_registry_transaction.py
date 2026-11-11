import unittest
import uuid
import winreg
from registry_transaction import Transaction,capture,validate
BASE=r"Software\PM_Lite_FinalValidation_Test"
class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.parent_absent=capture(BASE) is None
        self.path=BASE+"\\"+uuid.uuid4().hex
        self.tx=Transaction([self.path]); self.tx.begin(self.path)
    def tearDown(self):
        self.tx.restore(self.path,writers_stopped=True)
        if self.parent_absent and capture(BASE)=={"values":{},"subkeys":{}}: winreg.DeleteKey(winreg.HKEY_CURRENT_USER,BASE)
    def test_absent_restoration(self):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER,self.path+"\\child") as k: winreg.SetValueEx(k,"test",0,winreg.REG_SZ,"synthetic")
        self.assertTrue(self.tx.restore(self.path,writers_stopped=True)); self.assertIsNone(capture(self.path))
    def test_existing_restoration(self):
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER,self.path) as k: winreg.SetValueEx(k,"original",0,winreg.REG_DWORD,7)
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER,self.path+"\\originalchild") as k: winreg.SetValueEx(k,"keep",0,winreg.REG_MULTI_SZ,["synthetic"])
        tx=Transaction([self.path]); tx.begin(self.path)
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER,self.path+"\\originalchild")
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,self.path,0,winreg.KEY_WRITE) as k:
            winreg.SetValueEx(k,"original",0,winreg.REG_SZ,"changed"); winreg.SetValueEx(k,"new",0,winreg.REG_BINARY,b"synthetic")
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER,self.path+"\\newchild"): pass
        self.assertTrue(tx.restore(self.path,writers_stopped=True))
        self.assertEqual(capture(self.path),tx.snapshots[self.path])
    def test_guards(self):
        with self.assertRaises(PermissionError): self.tx.begin(r"Software\Other\Key")
        with self.assertRaises(RuntimeError): self.tx.restore(self.path)
        with self.assertRaises(RuntimeError): self.tx.begin(self.path)
        for path in ("", "Software", "Software\\Other", "HKLM\\Software\\X", "Software\\..\\X"):
            with self.subTest(path=path),self.assertRaises(ValueError): validate(path)
