"""The mod's settings for the plugin: the DEVBENCH object its rootdir conftest defines."""

from __future__ import annotations

import os
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field

from devbench.checks import version_numbers
from devbench.client import ModTool
from devbench.games.fallout4 import ANY_CELL
from devbench.vr import Handedness, check_handedness

# the games devbench runs in, as find_port and require name them
_GAMES = ("skyrim", "fallout4")


@dataclass(frozen=True)
class Area:
    """
    A report area: its title, and the pytest marker its file's tests carry (None: none). The plugin registers the
    marker and marks the file's tests with it, so -m <marker> selects them with nothing declared in pytest.ini.
    """

    title: str
    marker: str | None = None

    def __post_init__(self) -> None:
        if self.marker is not None and not self.marker.isidentifier():
            raise ValueError(f"{self.marker!r} is not a marker name: use letters, digits and _")


@dataclass(frozen=True)
class Dependency:
    """
    Another mod's devbench tool a run needs: the tool, the actions the suite calls on it, and the lowest version
    (dotted, compared by number) it works with; None skips the version check.
    """

    tool: str
    actions: Sequence[str] = ()
    min_version: str | None = None

    def __post_init__(self) -> None:
        if self.min_version is not None and version_numbers(self.min_version) is None:
            raise ValueError(f"min_version {self.min_version!r} of {self.tool} is not a dotted number")


@dataclass(frozen=True)
class Config:
    """
    What a mod's suite needs: the game (and VR), the mod's devbench tool with the actions its tests call and the
    checkout's version, the files a run changes ((root, glob) pairs, backed up for the run), the mod's log, the report's
    areas by test-file name in run order (a title, or an Area with its marker), and the cell the game runs in. cell is
    where a game started from the main menu goes; a game already loaded is reused when the player is in cell or one of
    cells (a sequence of editor IDs), or whatever its cell with cells=ANY_CELL (that can reach a save). tool
    None skips the mod check. picture is the render target dumped when a test fails (None: no picture); report_dir
    holds the run folders of the report (relative: to the DEVBENCH conftest's folder; None: build/e2e under the first
    folder up from it with a .git).
    handedness ("left" / "right", VR only) is set for the run and the player's put back after; None leaves it alone.
    requires lists other mods' tools the run needs, checked after the mod's own. ready is a condition on the mod's
    tool, polled before the first test until it returns a true value; not within ready_timeout seconds stops the run.
    """

    game: str
    vr: bool
    tool: str | None = None
    actions: Sequence[str] = ()
    version: str | Callable[[], str] | None = None
    backup: Sequence[tuple[str, str]] = ()
    log: str | None = None
    areas: Mapping[str, str | Area] = field(default_factory=dict)
    title: str | None = None
    cell: str = "QASmoke"
    cells: Sequence[str] | str | None = None
    picture: int | None = None
    report_dir: str | os.PathLike[str] | None = None
    handedness: Handedness | None = None
    requires: Sequence[Dependency] = ()
    ready: Callable[[ModTool], object] | None = None
    ready_timeout: float = 30.0

    def __post_init__(self) -> None:
        if self.ready is not None and self.tool is None:
            raise ValueError("ready needs tool: it is a condition on the mod's tool")
        if self.game not in _GAMES:
            raise ValueError(f"unknown game {self.game!r}: expected 'skyrim' or 'fallout4'")
        if isinstance(self.cells, str) and self.cells != ANY_CELL:
            raise ValueError(f"cells {self.cells!r} is neither a sequence of cells nor ANY_CELL")
        if self.handedness is not None:
            check_handedness(self.handedness)
            if not self.vr:
                raise ValueError("handedness needs vr=True: only VR has one")

    def area_titles(self) -> dict[str, str]:
        """Each area's title by test-file name, in areas order: the plain title, or an Area's."""
        return {file: area.title if isinstance(area, Area) else area for file, area in self.areas.items()}

    def area_markers(self) -> dict[str, Area]:
        """The areas that name a marker, by test-file name."""
        return {file: area for file, area in self.areas.items() if isinstance(area, Area) and area.marker is not None}

    def expected_version(self) -> str | None:
        """The checkout's version the mod must report: version itself, or what it returns when it is a callable."""
        return self.version() if callable(self.version) else self.version
