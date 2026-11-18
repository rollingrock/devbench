"""find_port: the bound port from runtime.json, else the game's default."""

import pytest

from devbench import find_port


def write_runtime(root, folder, text):
    path = root / "devbench" / folder / "runtime.json"
    path.parent.mkdir(parents=True)
    path.write_text(text, encoding="utf-8")


@pytest.mark.parametrize(
    ("game", "vr", "folder"),
    [
        ("skyrim", False, "se"),
        ("skyrim", True, "vr"),
        ("fallout4", False, "fallout4"),
        ("fallout4", True, "fallout4vr"),
    ],
)
def test_reads_the_port_from_runtime_json(tmp_path, monkeypatch, game, vr, folder):
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    write_runtime(tmp_path, folder, '{"port":9001}\n')

    assert find_port(game, vr) == 9001


@pytest.mark.parametrize(
    ("game", "vr", "port"),
    [("skyrim", False, 8920), ("skyrim", True, 8921), ("fallout4", False, 8930), ("fallout4", True, 8931)],
)
def test_default_port_without_runtime_json(tmp_path, monkeypatch, game, vr, port):
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))

    assert find_port(game, vr) == port


def test_default_port_without_localappdata(monkeypatch):
    monkeypatch.delenv("LOCALAPPDATA", raising=False)

    assert find_port("fallout4", True) == 8931


@pytest.mark.parametrize("text", ["not json", "{}", '{"port":"x"}', '{"port":0}', "[1]"])
def test_unreadable_runtime_json_falls_back_to_the_default(tmp_path, monkeypatch, text):
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    write_runtime(tmp_path, "fallout4vr", text)

    assert find_port("fallout4", True) == 8931


def test_another_game_runtime_json_is_not_read(tmp_path, monkeypatch):
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    write_runtime(tmp_path, "fallout4", '{"port":9001}')

    assert find_port("fallout4", True) == 8931


def test_unknown_game_raises():
    with pytest.raises(ValueError, match="starfield"):
        find_port("starfield", False)
