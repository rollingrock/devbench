"""The HTML report of a run: one page with the totals, the notes, the failures (with their picture and log tail), each
test's result by area, the mod's sections, the game's dialogs the run closed and a gallery of the mod's pictures.

render() needs no pytest session, so a script can render a Record it built.
"""

from __future__ import annotations

import html
from datetime import datetime
from pathlib import Path

from devbench.testing._record import Picture, Record, Result

# the outcomes the totals count, in the order they show
_OUTCOMES = ("passed", "failed", "error", "skipped")

STYLE = """
:root { --bg: #ffffff; --fg: #1d2125; --muted: #5f6b76; --line: #d8dde2; --card: #f5f7f9;
  --pass: #1f7a3a; --fail: #b3261e; --skip: #8a6d00; }
@media (prefers-color-scheme: dark) { :root { --bg: #15181b; --fg: #e3e7ea; --muted: #97a3ad; --line: #2c3237;
  --card: #1d2125; --pass: #5cc97f; --fail: #ff8a80; --skip: #e3c55a; } }
* { box-sizing: border-box; }
body { margin: 0; padding: 24px 16px 48px; background: var(--bg); color: var(--fg);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1100px; margin: 0 auto; }
h1 { font-size: 22px; margin: 0 0 4px; } h2 { font-size: 17px; margin: 32px 0 8px; }
a { color: inherit; }
.meta { color: var(--muted); margin-bottom: 16px; }
.totals { display: flex; flex-wrap: wrap; gap: 8px; margin: 12px 0; }
.total { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 8px 14px;
  min-width: 96px; }
.total b { display: block; font-size: 20px; }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); vertical-align: top; }
th { color: var(--muted); font-weight: 600; font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }
.passed { color: var(--pass); } .failed, .error { color: var(--fail); } .skipped { color: var(--skip); }
.badge { font-weight: 600; }
pre { white-space: pre-wrap; word-break: break-word; background: var(--card); border: 1px solid var(--line);
  border-radius: 6px; padding: 8px; margin: 6px 0 0; font-size: 12px; }
details { margin-top: 6px; } details summary { cursor: pointer; color: var(--muted); }
ul.list { margin: 4px 0 0; padding-left: 18px; font-size: 13px; }
.shot img { width: 220px; border: 1px solid var(--line); border-radius: 4px; display: block; }
.gallery { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
.gallery figure { margin: 0; } .gallery img { width: 100%; border: 1px solid var(--line); border-radius: 4px; }
.gallery figcaption { font-size: 12px; color: var(--muted); }
@media (max-width: 640px) { .shot img { width: 120px; } td, th { padding: 4px; } }
"""


def _e(text: object) -> str:
    return html.escape(str(text))


def _shot(path: str, alt: str) -> str:
    return f"<a class='shot' href='{_e(path)}'><img src='{_e(path)}' alt='{_e(alt)}' loading='lazy'></a>"


def _shown_pictures(record: Record) -> list[Picture]:
    """The mod's pictures whose file was written: a reserved file the mod never wrote is left out."""
    if record.directory is None:
        return []
    return [picture for picture in record.pictures if (record.directory / picture.path).is_file()]


def _by_area(record: Record) -> list[tuple[str, list[Result]]]:
    """The results grouped by file: the files Config.areas lists in its order, then the others as they ran."""
    files = [file for file in record.areas if any(result.file == file for result in record.results)]
    files += list(dict.fromkeys(result.file for result in record.results if result.file not in record.areas))
    return [(record.areas.get(file, file), [r for r in record.results if r.file == file]) for file in files]


def _failures(record: Record) -> list[str]:
    """
    The failures section: each failed or errored test's message, its open menus and message box (or why they were
    not read), why it has no picture, its log tail (collapsed),
    and a picture column when any failure has a picture.
    """
    failed = [result for result in record.results if result.outcome in ("failed", "error")]
    if not failed:
        return []
    taken = {result.nodeid: record.failures.get(result.nodeid) for result in failed}
    with_picture = any(failure and failure.picture for failure in taken.values())
    picture_column = "<th>Picture</th>" if with_picture else ""
    out = [f"<h2>Failures</h2><table><tr><th>Test</th><th>What</th>{picture_column}</tr>"]
    for result in failed:
        failure = taken[result.nodeid]
        what = f"<pre>{_e(result.message)}</pre>"
        for line in failure.menu_lines() if failure else []:
            what += f"<div class='meta'>{_e(line)}</div>"
        if failure and failure.picture_error:
            what += f"<div class='meta'>No picture: {_e(failure.picture_error)}</div>"
        if failure and failure.log is not None:
            lines = len(failure.log)
            what += (
                f"<details><summary>The log since the test started ({lines} lines)</summary>"
                f"<pre>{_e(chr(10).join(failure.log))}</pre></details>"
            )
        row = f"<tr><td class='failed'>{_e(result.name)}<br><small>{_e(result.file)}</small></td><td>{what}</td>"
        if with_picture:
            picture = failure.picture if failure else None
            row += f"<td>{_shot(picture, 'the failure') if picture else ''}</td>"
        out.append(row + "</tr>")
    out.append("</table>")
    return out


def _areas(record: Record, pictures: list[Picture]) -> list[str]:
    """A table per area; a picture column only in an area where some test has a picture the mod wrote."""
    out = []
    for area, results in _by_area(record):
        shots = {
            result.nodeid: [p for p in pictures if p.nodeid == result.nodeid]
            for result in results
            if any(p.nodeid == result.nodeid for p in pictures)
        }
        out.append(f"<h2>{_e(area)}</h2>")
        out.append(
            "<table><tr><th>Test</th><th>Result</th><th>Time</th>" + ("<th>Picture</th>" if shots else "") + "</tr>"
        )
        for result in results:
            skipped = ""
            if result.outcome == "skipped" and result.message:
                skipped = f"<br><small>{_e(result.message)}</small>"
            row = (
                f"<tr><td>{_e(result.name)}{skipped}</td><td class='badge {_e(result.outcome)}'>{_e(result.outcome)}"
                f"</td><td>{result.duration:.1f} s</td>"
            )
            if shots:
                row += "<td>" + "".join(_shot(p.path, p.label or "") for p in shots.get(result.nodeid, [])) + "</td>"
            out.append(row + "</tr>")
        out.append("</table>")
    return out


def _sections(record: Record) -> list[str]:
    """The mod's sections in the order they were first added: intro, then the escaped table or the raw HTML."""
    out = []
    for section in record.sections:
        out.append(f"<h2>{_e(section.title)}</h2>")
        if section.intro:
            out.append(f"<div class='meta'>{_e(section.intro)}</div>")
        if section.columns is not None:
            out.append("<table><tr>" + "".join(f"<th>{_e(column)}</th>" for column in section.columns) + "</tr>")
            out += ["<tr>" + "".join(f"<td>{_e(cell)}</td>" for cell in row) + "</tr>" for row in section.rows]
            out.append("</table>")
        out += section.html or []
    return out


def render(record: Record, now: datetime | None = None) -> str:
    """The page of a record, as HTML; now (default: the current time) ends the run's duration."""
    seconds = ((now or datetime.now()) - record.started).total_seconds()
    counts = {outcome: sum(r.outcome == outcome for r in record.results) for outcome in _OUTCOMES}
    pictures = _shown_pictures(record)
    out = [
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>",
        f"<title>{_e(record.title)} E2E Report</title><style>{STYLE}</style></head><body><main>",
        f"<h1>{_e(record.title)}: end-to-end run</h1>",
        f"<div class='meta'>{record.started:%Y-%m-%d %H:%M} &middot; {seconds / 60:.1f} min &middot; "
        f"{len(record.results)} tests</div>",
        "<div class='totals'>",
        *(f"<div class='total'><b class='{o}'>{counts[o]}</b>{o}</div>" for o in _OUTCOMES),
        "</div>",
        *(f"<div class='meta'>{_e(note)}</div>" for note in record.notes),
        *_failures(record),
        *_areas(record, pictures),
        *_sections(record),
    ]
    if record.dialogs:
        out.append("<h2>Game dialogs the run closed</h2><ul class='list'>")
        out += [f"<li>{_e(said)}</li>" for said in record.dialogs]
        out.append("</ul>")
    if pictures:
        out.append("<h2>Pictures</h2><div class='gallery'>")
        for picture in pictures:
            caption = picture.label or picture.nodeid.split("::", 1)[-1]
            out.append(
                f"<figure><a href='{_e(picture.path)}'><img src='{_e(picture.path)}' alt='' loading='lazy'></a>"
                f"<figcaption>{_e(caption)}</figcaption></figure>"
            )
        out.append("</div>")
    out.append("</main></body></html>")
    return "\n".join(out)


def write(record: Record, folder: Path | None = None) -> Path:
    """Write the page as index.html in folder (default: the record's folder), and return its path."""
    folder = folder or record.folder
    folder.mkdir(parents=True, exist_ok=True)
    page = folder / "index.html"
    page.write_text(render(record), encoding="utf-8")
    return page
