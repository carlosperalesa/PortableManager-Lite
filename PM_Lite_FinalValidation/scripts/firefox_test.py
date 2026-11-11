"""Phase 2 helpers only. No installation, profile editing, or automatic launch."""
import os
from pathlib import Path
from audit import snapshot

def baseline():
    return snapshot([Path(os.environ["APPDATA"])/"Mozilla", Path(os.environ["LOCALAPPDATA"])/"Mozilla", Path.home()/".mozilla", Path(os.environ["TEMP"])])

MANUAL_STEPS = ["Confirm marker/GUID/serial", "Check host Firefox processes and ask user to close them", "Capture host baseline", "Reuse verified portable installation or user selects installer target", "Open local USB HTML through FirefoxPortable.exe", "User creates PM_POC_TEST bookmark and changes an innocuous preference", "Normal close; verify portable processes ended", "Read-only persistence evidence and second GUI session", "Normal close and compare host audit; preserve all residues"]
if __name__ == "__main__":
    raise SystemExit("Pending USB confirmation and guided GUI validation; no application launched")
