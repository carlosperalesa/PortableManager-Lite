"""Metadata and SHA256 only. Errors prevent a clean verdict; no cleanup."""
import hashlib
import os
from pathlib import Path

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024*1024), b""): h.update(block)
    return h.hexdigest()

def snapshot(roots):
    result = {"files": {}, "errors": [], "roots": {}}
    for root in roots:
        root = Path(root)
        result["roots"][str(root)] = root.exists()
        def error(e): result["errors"].append(str(e))
        for base, dirs, names in os.walk(root, followlinks=False, onerror=error):
            dirs[:] = [d for d in dirs if not (Path(base)/d).is_junction() and not (Path(base)/d).is_symlink()]
            for name in names:
                path = Path(base)/name
                try:
                    if path.is_symlink(): continue
                    before = path.stat()
                    digest = sha256(path)
                    after = path.stat()
                    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                        raise OSError("File changed during inventory: " + str(path))
                    result["files"][str(path)] = {"size": after.st_size, "mtime_ns": after.st_mtime_ns, "ctime_ns": after.st_ctime_ns, "sha256": digest}
                except OSError as e: error(e)
    return result

def compare(before, after):
    a,b = before["files"],after["files"]
    return {"new": sorted(b.keys()-a.keys()), "deleted": sorted(a.keys()-b.keys()), "modified": sorted(k for k in a.keys() & b.keys() if a[k] != b[k]), "roots_changed": before.get("roots") != after.get("roots"), "complete": not before["errors"] and not after["errors"], "errors": before["errors"]+after["errors"]}

def registry_inventory(paths):
    # Only names/types, never value contents or credentials in disk reports.
    from registry_transaction import capture
    def redact(node):
        if node is None: return None
        return {"values": {k: {"type": v[1]} for k,v in node["values"].items()}, "subkeys": {k:redact(v) for k,v in node["subkeys"].items()}}
    return {path:redact(capture(path)) for path in paths}


def metadata_snapshot(roots):
    """Broad coverage without document contents. No content hashes in this mode."""
    result={"files":{},"directories":{},"errors":[],"roots":{},"mode":"metadata_only"}
    for root in map(Path,roots):
        result["roots"][str(root)]=root.exists()
        if not root.exists(): continue
        def error(e): result["errors"].append(str(e))
        for base,dirs,names in os.walk(root,followlinks=False,onerror=error):
            dirs[:]=[d for d in dirs if not (Path(base)/d).is_symlink() and not (Path(base)/d).is_junction()]
            try:
                stat=Path(base).stat(); result["directories"][str(base)]={"mtime_ns":stat.st_mtime_ns}
            except OSError as e: error(e)
            for name in names:
                path=Path(base)/name
                try:
                    if path.is_symlink(): continue
                    stat=path.stat()
                    result["files"][str(path)]={"size":stat.st_size,"mtime_ns":stat.st_mtime_ns,"ctime_ns":stat.st_ctime_ns}
                except OSError as e: error(e)
    return result
