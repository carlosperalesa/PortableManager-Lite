"""Read-only Restart Manager query for registered USB files; never shuts apps down.
This is not a global handle enumeration and does not include directory handles.
"""
import ctypes,json
from ctypes import wintypes as w
from pathlib import Path
from volume import identify,require_identity
ROOT=Path(__file__).resolve().parents[1]
class Unique(ctypes.Structure):
    _fields_=[("pid",w.DWORD),("start",w.FILETIME)]
class Info(ctypes.Structure):
    _fields_=[("process",Unique),("app",w.WCHAR*256),("service",w.WCHAR*64),("kind",w.UINT),("status",w.ULONG),("session",w.DWORD),("restartable",w.BOOL)]
def main():
    require_identity(json.loads((ROOT/"reports/usb_identity.json").read_text()),identify("E:/"))
    files=[str(p) for p in Path("E:/PM_POC").rglob("*") if p.is_file()]
    files.append("E:\\PM_POC_VOLUME_MARKER.txt")
    dll=ctypes.WinDLL("rstrtmgr")
    session=w.DWORD(); key=ctypes.create_unicode_buffer(33)
    code=dll.RmStartSession(ctypes.byref(session),0,key)
    if code: raise OSError(code,"RmStartSession")
    try:
        for start in range(0,len(files),500):
            batch=files[start:start+500]; arr=(w.LPCWSTR*len(batch))(*batch)
            code=dll.RmRegisterResources(session,len(batch),arr,0,None,0,None)
            if code: raise OSError(code,"RmRegisterResources")
        needed=w.UINT(); count=w.UINT(); reason=w.DWORD()
        code=dll.RmGetList(session,ctypes.byref(needed),ctypes.byref(count),None,ctypes.byref(reason))
        users=[]
        if code==234:
            for _ in range(5):
                count=w.UINT(needed.value); array=(Info*count.value)()
                code=dll.RmGetList(session,ctypes.byref(needed),ctypes.byref(count),array,ctypes.byref(reason))
                if code==0:
                    users=[{"pid":i.process.pid,"app":i.app,"service":i.service} for i in array[:count.value]]
                    break
                if code!=234: break
        if code: raise OSError(code,"RmGetList")
        report={"registered_files":len(files),"applications_using_registered_files":users,"scope":"Restart Manager registered PM_POC files and marker; not all system handles or directory handles","no_shutdown_called":True}
        (ROOT/"reports/usb_open_files.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
        print(json.dumps(report,indent=2,ensure_ascii=False))
    finally: dll.RmEndSession(session)
if __name__=="__main__": main()
