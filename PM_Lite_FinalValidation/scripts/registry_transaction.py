"""Explicit HKCU roots only; snapshots stay in RAM. ACL restoration NOT provided.
Caller must stop the test app and exclude concurrent writers before restore.
"""
import winreg
PREFIX = "Software\\"

def validate(path):
    if not isinstance(path,str) or not path.startswith(PREFIX) or (len(path.split("\\")) < 3 and path != r"Software\PM_Lite_FinalValidation_Test"):
        raise ValueError("Use a specific HKCU Software subtree")
    if any(not x or x in (".", "..") for x in path.split("\\")):
        raise ValueError("Invalid registry path")
    return path

def capture(path):
    validate(path)
    try: key=winreg.OpenKey(winreg.HKEY_CURRENT_USER,path,0,winreg.KEY_READ)
    except FileNotFoundError: return None
    with key:
        children, values, _ = winreg.QueryInfoKey(key)
        vals={}
        for i in range(values):
            name,data,kind=winreg.EnumValue(key,i); vals[name]=(data,kind)
        return {"values":vals,"subkeys":{name:capture(path+"\\"+name) for name in [winreg.EnumKey(key,i) for i in range(children)]}}

def _delete(path):
    node=capture(path)
    if node is None: return
    for name in node["subkeys"]: _delete(path+"\\"+name)
    winreg.DeleteKey(winreg.HKEY_CURRENT_USER,path)

def _restore(path, original):
    current=capture(path)
    if original is None:
        _delete(path); return
    if current is not None:
        for name in current["subkeys"]:
            if name not in original["subkeys"]: _delete(path+"\\"+name)
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER,path,0,winreg.KEY_READ|winreg.KEY_WRITE) as key:
        if current:
            for name in current["values"]:
                if name not in original["values"]: winreg.DeleteValue(key,name)
        for name,(data,kind) in original["values"].items(): winreg.SetValueEx(key,name,0,kind,data)
    for name,node in original["subkeys"].items(): _restore(path+"\\"+name,node)

class Transaction:
    def __init__(self, authorized):
        self.authorized=frozenset(validate(p) for p in authorized)
        if not self.authorized: raise ValueError("Explicit allowlist required")
        self.snapshots={}
    def begin(self,path):
        if path not in self.authorized: raise PermissionError(path)
        if path in self.snapshots: raise RuntimeError("Snapshot already captured")
        self.snapshots[path]=capture(path)
        return self.snapshots[path] is None
    def restore(self,path,*,writers_stopped=False):
        if path not in self.authorized or path not in self.snapshots: raise PermissionError(path)
        if not writers_stopped: raise RuntimeError("Confirm test writers stopped first")
        _restore(path,self.snapshots[path])
        if capture(path)!=self.snapshots[path]: raise RuntimeError("Restoration verification failed; retain RAM snapshot")
        return True
