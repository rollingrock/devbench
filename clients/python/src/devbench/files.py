"""The game's files through devbench's files tool, and a crash-safe backup of the files a run changes.

The files tool sees the game's Data folder as the game does (under MO2: the mods' files and overwrite) and
Documents/My Games/<game>. root is "data" or "mygames"; a path is relative to it, '/' separated.
"""

from __future__ import annotations

import base64
import json
import os
from datetime import datetime
from pathlib import Path
from typing import Any

from devbench.client import Devbench, DevbenchError

# the error the files tool's read gives a missing file; an unknown tool is a 404 too, so the status alone is not enough
_NO_FILE = "files: no file"


def list_files(bench: Devbench, root: str, glob: str) -> list[dict[str, Any]]:
    """
    The files under root whose path matches glob ('*' and '?' in a name, '**' for any folders, case ignored), sorted
    by path, each a dict with path, bytes and modifiedMs. A listing that is not whole (the walk failed partway, or it
    hit the tool's 10000-file cap) raises DevbenchError: a backup taken from it would miss files.
    """
    answer = bench.tool("files", action="list", root=root, path=glob)
    if answer.get("incomplete") or answer.get("capped"):
        why = answer.get("reason") or "more than the tool lists"
        raise DevbenchError(f"files list {root}/{glob}: listing is not whole: {why}", tool="files", answer=answer)
    return answer["files"]


def read_bytes(bench: Devbench, root: str, path: str) -> bytes | None:
    """The bytes of a file as the game sees it, or None when there is no file. Any other refusal raises."""
    try:
        answer = bench.tool("files", action="read", root=root, path=path, encoding="base64")
    except DevbenchError as error:
        if (
            error.status == 404
            and isinstance(error.answer, dict)
            and str(error.answer.get("error", "")).startswith(_NO_FILE)
        ):
            return None
        raise
    return base64.b64decode(answer["content"])


def read_text(bench: Devbench, root: str, path: str) -> str | None:
    """
    The text of a file, or None when there is no file. UTF-8 (a BOM is dropped); a file that is not UTF-8 is read as
    Windows-1252, as older mods' MCM INI files are.
    """
    data = read_bytes(bench, root, path)
    if data is None:
        return None
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp1252", errors="replace")


def write_bytes(bench: Devbench, root: str, path: str, data: bytes) -> None:
    """Create or replace a file, and its folders, with data exactly."""
    content = base64.b64encode(data).decode("ascii")
    bench.tool("files", action="write", root=root, path=path, encoding="base64", content=content)


def write_text(bench: Devbench, root: str, path: str, text: str) -> None:
    """Create or replace a file with text as UTF-8, no BOM, its newlines as given."""
    write_bytes(bench, root, path, text.encode("utf-8"))


def delete_file(bench: Devbench, root: str, path: str) -> bool:
    """Delete a file; False when there was none."""
    return bench.tool("files", action="delete", root=root, path=path)["deleted"]


def _key(root: str, path: str) -> tuple[str, str]:
    """A file's identity for comparing listings: Windows, and the game, ignore case."""
    return root, path.replace("\\", "/").lower()


class Backup:
    """
    The files of globs ((root, glob) pairs), saved before a run changes them and put back after it. The manifest at
    <folder>/manifest.json is written whole before the run starts, so a run that a crash ended is put back by the next
    one. It holds the globs it was taken with, which a restore uses, and each file's bytes as base64.
    """

    def __init__(self, bench: Devbench, globs: list[tuple[str, str]], folder: str | os.PathLike[str]) -> None:
        self.bench = bench
        self.globs = [(root, glob) for root, glob in globs]
        self.folder = Path(folder)
        self.manifest = self.folder / "manifest.json"

    def pending(self) -> bool:
        """Whether a backup is waiting to be put back: a run is going, or one did not end. A partial one is not."""
        return self.manifest.is_file()

    def take(self) -> int:
        """
        Save every file the globs list and return how many. The manifest goes to a .partial file first and replaces
        manifest.json only once whole. A backup still pending raises RuntimeError: taking over it would lose the
        files it holds.
        """
        if self.pending():
            raise RuntimeError(f"a backup is still pending at {self.manifest}: restore it before taking another")
        files = []
        for root, glob in self.globs:
            for listed in list_files(self.bench, root, glob):
                data = read_bytes(self.bench, root, listed["path"])
                if data is not None:  # gone between the listing and the read
                    content = base64.b64encode(data).decode("ascii")
                    files.append({"root": root, "path": listed["path"], "content": content})
        manifest = {"taken": datetime.now().isoformat(timespec="seconds"), "globs": self.globs, "files": files}
        self.folder.mkdir(parents=True, exist_ok=True)
        partial = self.manifest.with_suffix(".partial")
        partial.write_text(json.dumps(manifest), encoding="utf-8")
        os.replace(partial, self.manifest)
        return len(files)

    def restore(self) -> tuple[list[str], list[str]]:
        """
        Put the manifest's files back: write each one whose bytes differ, delete each file its globs list now that it
        lacks (case ignored), then remove the manifest. Returns the written and deleted files as "root/path". A
        restore that fails partway keeps the manifest, so it can run again. No pending backup raises
        FileNotFoundError.
        """
        if not self.pending():
            raise FileNotFoundError(f"no backup to restore at {self.manifest}")
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        kept = {_key(entry["root"], entry["path"]) for entry in manifest["files"]}
        written: list[str] = []
        deleted: list[str] = []
        for entry in manifest["files"]:
            data = base64.b64decode(entry["content"])
            if read_bytes(self.bench, entry["root"], entry["path"]) != data:
                write_bytes(self.bench, entry["root"], entry["path"], data)
                written.append(f"{entry['root']}/{entry['path']}")
        for root, glob in manifest["globs"]:
            for listed in list_files(self.bench, root, glob):
                if _key(root, listed["path"]) not in kept:
                    delete_file(self.bench, root, listed["path"])
                    deleted.append(f"{root}/{listed['path']}")
        self.manifest.unlink()
        return written, deleted
