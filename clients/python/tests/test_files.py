"""Files and backup: the files tool's actions, and a backup that puts back exactly what a run changed."""

import json

import pytest
from fake_devbench import FakeFiles

from devbench import (
    Backup,
    DevbenchError,
    delete_file,
    list_files,
    read_bytes,
    read_text,
    write_bytes,
    write_text,
)


@pytest.fixture
def files(fake):
    tool = FakeFiles()
    fake.tools["files"] = tool
    return tool


def test_read_bytes_returns_the_bytes(files, bench):
    files.files[("data", "MCM/Settings/Mod.ini")] = b"\x00\xffraw"

    assert read_bytes(bench, "data", "MCM/Settings/Mod.ini") == b"\x00\xffraw"


def test_read_bytes_of_no_file_is_none(files, bench):
    assert read_bytes(bench, "data", "missing.ini") is None


def test_read_bytes_without_the_files_tool_raises(fake, bench):
    # an unknown tool is a 404 too; it must not pass for a missing file
    with pytest.raises(DevbenchError):
        read_bytes(bench, "data", "missing.ini")


def test_read_bytes_raises_other_refusals(fake, bench):
    fake.tools["files"] = lambda args: (422, {"ok": False, "error": "files: 'big' has 99 bytes, more than ..."})

    with pytest.raises(DevbenchError):
        read_bytes(bench, "data", "big")


def test_read_text_drops_a_utf8_bom(files, bench):
    files.files[("mygames", "a.toml")] = "﻿key = 'é'".encode()

    assert read_text(bench, "mygames", "a.toml") == "key = 'é'"


def test_read_text_falls_back_to_cp1252(files, bench):
    files.files[("data", "old.ini")] = "sName=Café".encode("cp1252")

    assert read_text(bench, "data", "old.ini") == "sName=Café"


def test_read_text_of_no_file_is_none(files, bench):
    assert read_text(bench, "data", "missing.ini") is None


def test_write_text_writes_utf8_without_bom(files, bench):
    write_text(bench, "mygames", "a.toml", "é\r\n")

    assert files.files[("mygames", "a.toml")] == "é\r\n".encode()


def test_write_bytes_then_delete(files, bench):
    write_bytes(bench, "data", "x.bin", b"\x01\x02")

    assert files.files[("data", "x.bin")] == b"\x01\x02"
    assert delete_file(bench, "data", "x.bin") is True
    assert delete_file(bench, "data", "x.bin") is False


def test_list_files_returns_the_matches(files, bench):
    files.files.update({("data", "MCM/Settings/A.ini"): b"a", ("data", "MCM/Other/B.ini"): b"b"})

    assert [f["path"] for f in list_files(bench, "data", "MCM/Settings/*")] == ["MCM/Settings/A.ini"]


@pytest.mark.parametrize("flags", [{"incomplete": True, "reason": "access denied"}, {"capped": True}])
def test_list_files_that_is_not_whole_raises(files, bench, flags):
    files.listing = flags

    with pytest.raises(DevbenchError, match="not whole"):
        list_files(bench, "data", "**")


GLOBS = [("mygames", "Mods_Config/Mod/*"), ("data", "MCM/Settings/*")]


@pytest.fixture
def backup(files, bench, tmp_path):
    files.files.update(
        {
            ("mygames", "Mods_Config/Mod/Mod.toml"): b"original",
            ("mygames", "Mods_Config/Mod/Mod.toml.bak"): b"older",
            ("data", "MCM/Settings/Mod.ini"): b"[Main]\r\n",
            ("data", "Other/untouched.txt"): b"outside the globs",
        }
    )
    return Backup(bench, GLOBS, tmp_path / ".backup")


def test_take_then_restore_round_trips(files, backup):
    before = dict(files.files)

    assert backup.take() == 3
    assert backup.pending()
    assert backup.restore() == ([], [])
    assert files.files == before
    assert files.writes == []
    assert not backup.pending()


def test_restore_writes_back_a_changed_file_only(files, backup):
    backup.take()
    files.files[("mygames", "Mods_Config/Mod/Mod.toml")] = b"changed by the run"

    written, deleted = backup.restore()

    assert written == ["mygames/Mods_Config/Mod/Mod.toml"]
    assert deleted == []
    assert files.writes == ["mygames/Mods_Config/Mod/Mod.toml"]
    assert files.files[("mygames", "Mods_Config/Mod/Mod.toml")] == b"original"


def test_restore_deletes_a_file_the_run_created(files, backup):
    backup.take()
    files.files[("data", "MCM/Settings/New.ini")] = b"made by the run"
    files.files[("data", "Other/new.txt")] = b"outside the globs"

    written, deleted = backup.restore()

    assert written == []
    assert deleted == ["data/MCM/Settings/New.ini"]
    assert ("data", "Other/new.txt") in files.files


def test_restore_writes_back_a_file_the_run_deleted(files, backup):
    backup.take()
    del files.files[("data", "MCM/Settings/Mod.ini")]

    assert backup.restore() == (["data/MCM/Settings/Mod.ini"], [])
    assert files.files[("data", "MCM/Settings/Mod.ini")] == b"[Main]\r\n"


def test_restore_ignores_case(files, backup):
    backup.take()
    # the game rewrote the file under another case: the same file on Windows, not one the run created
    data = files.files.pop(("data", "MCM/Settings/Mod.ini"))
    files.files[("data", "MCM/Settings/mod.INI")] = data

    assert backup.restore() == ([], [])


def test_restore_uses_the_manifests_globs(files, bench, backup, tmp_path):
    backup.take()
    files.files[("data", "MCM/Settings/New.ini")] = b"made by the run"

    # a crash restore made with other globs still deletes against what was backed up
    written, deleted = Backup(bench, [], tmp_path / ".backup").restore()

    assert deleted == ["data/MCM/Settings/New.ini"]


def test_a_partial_manifest_is_not_pending(backup):
    backup.folder.mkdir()
    backup.manifest.with_suffix(".partial").write_text("{", encoding="utf-8")

    assert not backup.pending()
    assert backup.take() == 3
    assert backup.pending()


def test_take_writes_the_manifest_format(backup):
    backup.take()

    manifest = json.loads(backup.manifest.read_text(encoding="utf-8"))
    assert set(manifest) == {"taken", "globs", "files"}
    assert manifest["globs"] == [list(glob) for glob in GLOBS]
    entry = manifest["files"][0]
    assert set(entry) == {"root", "path", "content"}
    assert not backup.manifest.with_suffix(".partial").exists()


def test_take_while_pending_raises(files, backup):
    backup.take()
    files.files[("mygames", "Mods_Config/Mod/Mod.toml")] = b"changed by the run"

    with pytest.raises(RuntimeError, match="still pending"):
        backup.take()
    backup.restore()
    assert files.files[("mygames", "Mods_Config/Mod/Mod.toml")] == b"original"


def test_restore_with_nothing_pending_raises(backup):
    with pytest.raises(FileNotFoundError):
        backup.restore()


def test_a_failed_take_leaves_nothing_pending(files, backup):
    files.listing = {"incomplete": True, "reason": "access denied"}

    with pytest.raises(DevbenchError):
        backup.take()
    assert not backup.pending()


def test_a_failed_restore_keeps_the_manifest(fake, files, backup):
    backup.take()
    fake.tools["files"] = lambda args: (500, {"ok": False, "error": "files: cannot write"})

    with pytest.raises(DevbenchError):
        backup.restore()
    assert backup.pending()
