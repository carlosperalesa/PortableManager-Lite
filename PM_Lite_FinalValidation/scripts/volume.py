"""Read-only Windows volume identity; never creates markers."""
import ctypes
from pathlib import Path

def identify(root):
    root = str(Path(root).resolve())
    if not root.endswith("\\"): root += "\\"
    marker = Path(root) / "PM_POC_VOLUME_MARKER.txt"
    if not marker.is_file(): raise ValueError("Required marker missing; explicit approval needed to create it")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    guid = ctypes.create_unicode_buffer(1024)
    serial = ctypes.c_ulong()
    if not kernel.GetVolumeNameForVolumeMountPointW(ctypes.c_wchar_p(root), guid, len(guid)):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel.GetVolumeInformationW(ctypes.c_wchar_p(root), None, 0, ctypes.byref(serial), None, None, None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    import hashlib
    return {"root": root, "guid": guid.value, "serial": f"{serial.value:08X}", "marker_sha256": hashlib.sha256(marker.read_bytes()).hexdigest()}

def require_identity(expected, actual):
    for key in ("guid", "serial", "marker_sha256"):
        if not expected.get(key) or expected[key] != actual.get(key):
            raise ValueError("Volume identity mismatch: " + key)
    return True
