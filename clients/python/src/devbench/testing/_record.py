"""The record of a run: its notes, the dialogs it closed, each test's result, the mod's sections and the pictures, which
devbench.testing.report renders."""

from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path, PurePosixPath

import pytest

# the end of a failure's text kept when pytest gives no one-line crash message
_FAILURE_TAIL = 3000

# the longest test name a picture's file name keeps
_PICTURE_NAME = 80


@dataclass
class Result:
    """
    One test's result: the call's outcome, or "error" / "skipped" when its setup or teardown failed or skipped (phase
    says which). area is the area its file belongs to, or the file name when Config.areas does not list it.
    """

    nodeid: str
    file: str
    name: str
    area: str
    outcome: str
    phase: str
    duration: float
    message: str


@dataclass
class Failure:
    """
    What the plugin took when a test failed: the open menus and a message box's text (or why the menus were not
    read), a picture of a render target (its path in the report folder) or why none was taken, and the mod log's
    lines since the test started. None where the run did not take it.
    """

    menus: list[str] | None = None
    message_box: str | None = None
    menus_error: str | None = None
    picture: str | None = None
    picture_error: str | None = None
    log: list[str] | None = None

    def menu_lines(self) -> list[str]:
        """The menus part as text lines for the report and the terminal; empty when none was taken."""
        if self.menus_error is not None:
            return [f"Menus not read: {self.menus_error}"]
        if self.menus is None:
            return []
        lines = [f"Open menus: {', '.join(self.menus) or '(none)'}"]
        if self.message_box is not None:
            lines.append(f"Message box: {self.message_box}")
        return lines


@dataclass
class Picture:
    """A picture a mod reserved for a test: its path in the report folder, shown when the file exists."""

    nodeid: str
    path: str
    label: str | None


@dataclass
class Section:
    """A mod's section of the page: a table (columns and rows of text) or raw HTML, under an optional intro line."""

    title: str
    intro: str | None = None
    columns: tuple[str, ...] | None = None
    rows: list[tuple[str, ...]] = field(default_factory=list)
    html: list[str] | None = None


def _picture_name(nodeid: str) -> str:
    """A test's name as a file name: what follows the file in its node id, with other than [A-Za-z0-9_-] as '_'."""
    name = nodeid.split("::", 1)[-1]
    return "".join(c if c.isascii() and (c.isalnum() or c in "-_") else "_" for c in name)[:_PICTURE_NAME]


@dataclass
class Record:
    """
    What a run found, as the plugin and the tests add it: notes, the dialogs closed, the results in run order, what
    failed tests left (failures by node id), the mod's sections and pictures. directory is the run's report folder;
    folder creates it on first use. The plugin keeps current_test on the running test, from its setup to its
    teardown, so a mod's helper can tag a row with it.
    """

    areas: Mapping[str, str] = field(default_factory=dict)
    title: str = "devbench"
    started: datetime = field(default_factory=datetime.now)
    directory: Path | None = None
    notes: list[str] = field(default_factory=list)
    dialogs: list[str] = field(default_factory=list)
    results: list[Result] = field(default_factory=list)
    failures: dict[str, Failure] = field(default_factory=dict)
    sections: list[Section] = field(default_factory=list)
    pictures: list[Picture] = field(default_factory=list)
    # the running test's name (what follows its file in the node id: "test_x[p]", "TestC::test_x"); None between tests
    current_test: str | None = None
    _reserved: int = field(default=0, init=False, repr=False)

    @property
    def folder(self) -> Path:
        """The run's report folder, created on first use, for files a mod writes beside the page (perf.json)."""
        if self.directory is None:
            raise ValueError("this record has no report folder: set Record.directory")
        self.directory.mkdir(parents=True, exist_ok=True)
        return self.directory

    def note(self, text: str) -> None:
        """Add a line about the run as a whole."""
        self.notes.append(text)

    def section(
        self,
        title: str,
        *,
        intro: str | None = None,
        columns: Sequence[object] | None = None,
        rows: Iterable[Sequence[object]] | None = None,
        html: str | None = None,
    ) -> None:
        """
        Add to the mod's section of that title, made on first use: a table's rows (columns name them; cells are shown
        as escaped text) or raw HTML. A section is one kind: columns other than its own, rows of another length, or
        a table call on an HTML section (or the reverse) raise ValueError. The first intro given stays.
        """
        table = columns is not None or rows is not None
        if table and html is not None:
            raise ValueError(f"section {title!r}: give columns and rows, or html, not both")
        section = next((s for s in self.sections if s.title == title), None)
        if section is None:
            section = Section(title)
            self.sections.append(section)
        if section.intro is None:
            section.intro = intro
        if table:
            if section.html is not None:
                raise ValueError(f"section {title!r} holds HTML, not a table")
            wanted = tuple(str(column) for column in columns) if columns is not None else None
            if section.columns is None:
                if wanted is None:
                    raise ValueError(f"section {title!r}: its first rows need columns")
                section.columns = wanted
            elif wanted is not None and wanted != section.columns:
                raise ValueError(f"section {title!r} has columns {list(section.columns)}, not {list(wanted)}")
            for row in rows or ():
                cells = tuple(str(cell) for cell in row)
                if len(cells) != len(section.columns):
                    raise ValueError(
                        f"section {title!r}: a row of {len(cells)} cells for {len(section.columns)} columns"
                    )
                section.rows.append(cells)
        elif html is not None:
            if section.columns is not None:
                raise ValueError(f"section {title!r} holds a table, not HTML")
            section.html = [*(section.html or []), html]

    def picture(self, nodeid: str, label: str | None = None, suffix: str = ".png") -> Path:
        """
        Reserve a file in the report's pictures folder for a picture of a test, which the mod writes. The page shows
        it in the test's row and in the gallery when the file exists by the time the page is written.
        """
        path = self.reserve(nodeid, suffix)
        self.pictures.append(Picture(nodeid, path.relative_to(self.folder).as_posix(), label))
        return path

    def reserve(self, nodeid: str, suffix: str) -> Path:
        """A new file name in the pictures folder for a test (numbered in reserve order), with the folder made."""
        self._reserved += 1
        path = self.folder / "pictures" / f"{self._reserved:03d}-{_picture_name(nodeid)}{suffix}"
        path.parent.mkdir(parents=True, exist_ok=True)
        return path

    def add(self, report: pytest.TestReport) -> None:
        """
        Fold one of pytest's phase reports into its test's result. A passed setup or teardown adds nothing; a later
        phase that did not pass overrides a passed call (a failed teardown makes the test an error), and the first
        failure's message is kept.
        """
        if report.when != "call" and report.passed:
            return
        outcome = report.outcome if report.when == "call" else ("error" if report.failed else report.outcome)
        message = ""
        if report.failed:
            crash = getattr(report.longrepr, "reprcrash", None)
            message = crash.message if crash else str(report.longrepr)[-_FAILURE_TAIL:]
        elif report.skipped and isinstance(report.longrepr, tuple):
            message = report.longrepr[2]
        result = next((result for result in self.results if result.nodeid == report.nodeid), None)
        if result:
            if outcome != "passed":
                result.outcome, result.phase = outcome, report.when
                result.message = result.message or message
            return
        path, _, name = report.nodeid.partition("::")
        file = PurePosixPath(path).name
        self.results.append(
            Result(
                report.nodeid,
                file,
                name,
                self.areas.get(file, file),
                outcome,
                report.when,
                report.duration,
                message,
            )
        )
