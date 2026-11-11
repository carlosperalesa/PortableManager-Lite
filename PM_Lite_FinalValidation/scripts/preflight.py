"""Verify original packages without executing them; stdlib + Windows PowerShell."""
import json
import subprocess
import tempfile
import zipfile
from pathlib import Path
from datetime import datetime, timezone
from audit import sha256
ROOT=Path(__file__).resolve().parents[1]
EXPECTED={"FirefoxPortable_160.0_English.paf.exe":"99D406A460EF79B1815980A66649D294ABBB82C103F2013192E81B829A88B7DA", "putty.zip":"E0B36E5381ECC78DD06A291BAB25FB9D95BC991D0D864CF2741A5BDE82816668"}
def ps(code):
    out=subprocess.run(["pwsh.exe","-NoProfile","-NonInteractive","-Command",code],capture_output=True,text=True,check=True)
    return json.loads(out.stdout.lstrip("\ufeff"))
def main():
    report={"timestamp_utc":datetime.now(timezone.utc).isoformat(),"packages":{},"applications_executed":False}
    for name,expected in EXPECTED.items():
        path=ROOT.parent/name
        actual=sha256(path).upper()
        report["packages"][name]={"path":str(path),"sha256":actual,"expected_sha256":expected,"matches":actual==expected}
    ff=report["packages"]["FirefoxPortable_160.0_English.paf.exe"]
    if ff["matches"]:
        ff["authenticode"]=ps("Get-AuthenticodeSignature -LiteralPath '"+ff["path"].replace("'","''")+"' | Select-Object @{n='status';e={$_.Status.ToString()}},StatusMessage,@{n='signer';e={$_.SignerCertificate.Subject}},@{n='thumbprint';e={$_.SignerCertificate.Thumbprint}} | ConvertTo-Json")
    putty=report["packages"]["putty.zip"]
    if putty["matches"]:
        with zipfile.ZipFile(putty["path"]) as archive:
            bad=archive.testzip(); putty["zip_crc_valid"]=bad is None; putty["bad_member"]=bad
            candidates=[n for n in archive.namelist() if Path(n).name.lower()=="putty.exe"]
            putty["executable_members"]=candidates
            putty["versions"]=[]
            if bad is None:
                for member in candidates:
                    with tempfile.TemporaryDirectory(prefix="PM_Lite_Version_") as tmp:
                        target=Path(tmp)/"putty.exe"
                        target.write_bytes(archive.read(member))
                        version=ps("(Get-Item -LiteralPath '"+str(target).replace("'","''")+"').VersionInfo | Select-Object FileVersion,ProductVersion,ProductName | ConvertTo-Json")
                        putty["versions"].append({"member":member,"metadata":version,"sha256":sha256(target)})
    report["ready_for_guided_tests"]=all(v["matches"] for v in report["packages"].values()) and ff.get("authenticode",{}).get("status")=="Valid" and putty.get("zip_crc_valid",False) and bool(putty.get("versions")) and all("0.85" in str(v["metadata"]) for v in putty.get("versions",[]))
    (ROOT/"reports/package_integrity.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
    print(json.dumps(report,indent=2,ensure_ascii=False))
    if not report["ready_for_guided_tests"]: raise SystemExit(1)
if __name__=="__main__": main()
