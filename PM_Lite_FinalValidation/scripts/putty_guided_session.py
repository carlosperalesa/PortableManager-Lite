"""Keep the authorized D3 transaction in RAM across two manual GUI sessions.
No automatic cleanup on failure. Requires exclusive use of the test PuTTY key.
"""
import json,subprocess,sys,zipfile,time
from pathlib import Path
from datetime import datetime,timezone
from volume import identify,require_identity
from audit import sha256
from putty_test import begin,KEY
from registry_transaction import capture
ROOT=Path(__file__).resolve().parents[1]
APP=Path("E:/PM_POC/App/PuTTY")
def processes():
    return json.loads(subprocess.check_output(["pwsh.exe","-NoProfile","-Command","ConvertTo-Json -InputObject @(Get-CimInstance Win32_Process -Filter \"Name = 'putty.exe'\" | Select-Object ProcessId,ExecutablePath)"],text=True))
def identity():
    require_identity(json.loads((ROOT/"reports/usb_identity.json").read_text()),identify("E:/"))
def save(**fields):
    path=ROOT/"reports/d3_putty.json"
    r=json.loads(path.read_text(encoding="utf-8")); r.update(fields)
    r["updated_utc"]=datetime.now(timezone.utc).isoformat()
    path.write_text(json.dumps(r,ensure_ascii=False,indent=2),encoding="utf-8")
def launch():
    identity()
    if processes(): raise RuntimeError("Existing PuTTY process; stop")
    proc=subprocess.Popen([str(APP/"PUTTY.EXE")],cwd=APP)
    save(active_process_id=proc.pid)
    print("PUTTY_OPEN pid="+str(proc.pid),flush=True)
def session_evidence():
    if processes(): raise RuntimeError("Close PuTTY normally before continuing")
    node=capture(KEY)
    if node is None: raise RuntimeError("PuTTY key missing; persistence not verified")
    session=node.get("subkeys",{}).get("Sessions",{}).get("subkeys",{}).get("PM_POC_TEST")
    if session is None: raise RuntimeError("PM_POC_TEST session missing")
    host=session["values"].get("HostName")
    if host is None or host[0]!="127.0.0.1": raise RuntimeError("Expected dummy host 127.0.0.1")
    return {"session":"PM_POC_TEST","host":"127.0.0.1","persisted":True,"processes":[]}
def wait_command(command):
    signal=ROOT/"reports"/("d3_"+command+".signal")
    save(waiting_for=command,control_signal=str(signal))
    while not signal.exists(): time.sleep(1)
    if signal.read_text(encoding="utf-8").strip()!=command: raise RuntimeError("Invalid control signal")

def main():
    identity()
    if any((ROOT/"reports"/("d3_"+c+".signal")).exists() for c in ("FIRST_CLOSED","SECOND_CLOSED")): raise RuntimeError("Stale D3 control file; inspect before restarting")
    if processes(): raise RuntimeError("Existing PuTTY process")
    if capture(KEY) is not None: raise RuntimeError("Preexisting PuTTY key: authorization required; no changes made")
    package=ROOT.parent/"putty.zip"
    if sha256(package).upper()!="E0B36E5381ECC78DD06A291BAB25FB9D95BC991D0D864CF2741A5BDE82816668": raise RuntimeError("Package hash mismatch")
    if APP.exists(): raise RuntimeError("Installation exists; inspect rather than overwrite")
    with zipfile.ZipFile(package) as z:
        if z.testzip() is not None: raise RuntimeError("ZIP CRC failure")
        for item in z.infolist():
            target=(APP/item.filename).resolve()
            if not target.is_relative_to(APP.resolve()): raise RuntimeError("Unsafe ZIP member")
        APP.mkdir()
        for item in z.infolist():
            target=APP/item.filename
            if item.is_dir(): target.mkdir(parents=True,exist_ok=True)
            else:
                target.parent.mkdir(parents=True,exist_ok=True)
                with target.open("xb") as f: f.write(z.read(item))
    if not (APP/"PUTTY.EXE").is_file(): raise RuntimeError("Expected PUTTY.EXE missing")
    global ACTIVE_TRANSACTION
    tx=begin()
    ACTIVE_TRANSACTION=tx
    save(status="en_curso",phase="primera sesion GUI",executed_in_rebuilt_environment=True,reason="Prueba D3 guiada autorizada",original_key_absent=True,ram_snapshot_active=True,installation=str(APP),usb_identity=identify("E:/"),acl_restoration_claimed=False)
    launch()
    print("WAIT_FIRST_CLOSE: save dummy session then close normally; input FIRST_CLOSED",flush=True)
    wait_command("FIRST_CLOSED")
    evidence=session_evidence(); save(first_close_evidence=evidence,phase="segunda sesion GUI")
    launch()
    print("WAIT_SECOND_CLOSE: visually verify saved session; close normally; input SECOND_CLOSED",flush=True)
    wait_command("SECOND_CLOSED")
    evidence=session_evidence(); save(second_close_evidence=evidence)
    identity()
    # Both GUI instances ended and user confirmed exclusive test session.
    tx.restore(KEY,writers_stopped=True)
    save(status="restauracion_verificada_auditoria_pendiente",phase="auditoria posterior pendiente",persistence_verified=True,restored_key_absent=capture(KEY) is None,ram_snapshot_active=False,active_process_id=None)
    print("D3_RESTORED_ABSENT; host residue audit still pending",flush=True)
if __name__=="__main__":
    ACTIVE_TRANSACTION=None
    try: main()
    except Exception as error:
        print("STOP: "+str(error),flush=True)
        if ACTIVE_TRANSACTION is not None:
            save(status="bloqueado_restauracion_pendiente",error=str(error),ram_snapshot_active=True)
            print("RAM snapshot retained; do not terminate this process. Manual diagnosis required.",flush=True)
            while True: time.sleep(30)
        else: raise

