"""The HTML report: render() and the record's sections and pictures, on records built by hand, no pytest session."""

import re
from datetime import datetime, timedelta

import pytest

from devbench.testing import Failure, Record, Result
from devbench.testing.report import render, write

STARTED = datetime(2026, 11, 15, 10, 0)


def _result(nodeid, outcome="passed", message="", areas=None):
    path, _, name = nodeid.partition("::")
    return Result(nodeid, path, name, (areas or {}).get(path, path), outcome, "call", 1.5, message)


def _record(tmp_path=None, **kwargs):
    return Record(title="My <Mod>", started=STARTED, directory=tmp_path, **kwargs)


def test_the_page_has_the_title_totals_and_escaped_notes():
    record = _record()
    record.results += [_result("test_a.py::test_one"), _result("test_a.py::test_two", "failed", "x < y")]
    record.note("a <b> note")

    text = render(record, now=STARTED + timedelta(minutes=3))

    assert "<h1>My &lt;Mod&gt;: end-to-end run</h1>" in text
    assert "2026-11-15 10:00 &middot; 3.0 min &middot; 2 tests" in text
    assert "<b class='passed'>1</b>passed" in text and "<b class='failed'>1</b>failed" in text
    assert "a &lt;b&gt; note" in text and "x &lt; y" in text


def test_areas_follow_the_config_then_the_unlisted_files():
    areas = {"test_c.py": "C", "test_a.py": "A"}
    record = _record(areas=areas)
    record.results += [_result(f"test_{n}.py::test_one", areas=areas) for n in ("b", "a", "c")]

    assert re.findall(r"<h2>(.*?)</h2>", render(record)) == ["C", "A", "test_b.py"]


def test_failures_come_first_with_their_log_and_why_no_picture():
    record = _record()
    record.results += [_result("test_a.py::test_one", "failed", "broke"), _result("test_a.py::test_two", "error")]
    record.failures["test_a.py::test_one"] = Failure(picture_error="rendertarget: 501", log=["line 1", "<line 2>"])

    text = render(record)

    assert re.findall(r"<h2>(.*?)</h2>", text) == ["Failures", "test_a.py"]
    failures = text[: text.index("<h2>test_a.py")]
    assert "No picture: rendertarget: 501" in failures
    assert "(2 lines)</summary><pre>line 1\n&lt;line 2&gt;</pre>" in failures
    assert "test_two" in failures and "<th>Picture</th>" not in failures


def test_a_picture_column_shows_only_where_a_picture_was_written(tmp_path):
    record = _record(tmp_path)
    record.results += [_result("test_a.py::test_one"), _result("test_b.py::test_one")]
    record.picture("test_a.py::test_one", label="the panel").write_bytes(b"png")
    record.picture("test_b.py::test_one")

    text = render(record)

    area_a = text[text.index("<h2>test_a.py") : text.index("<h2>test_b.py")]
    area_b = text[text.index("<h2>test_b.py") : text.index("<h2>Pictures")]
    assert "<th>Picture</th>" in area_a and "pictures/001-test_one.png" in area_a
    assert "<th>Picture</th>" not in area_b
    assert "002-test_one.png" not in text


def test_a_page_without_pictures_has_no_gallery():
    record = _record()
    record.results.append(_result("test_a.py::test_one"))

    assert "<h2>Pictures</h2>" not in render(record)


def test_sections_render_tables_escaped_and_html_raw():
    record = _record()
    record.section("Placements", intro="hands", columns=["Hand", "Landed"], rows=[["left", "<1 cm>"]])
    record.section("Placements", rows=[("right", 2.5)])
    record.section("Perf", intro="per frame", html="<details><pre>raw</pre></details>")
    record.section("Perf", intro="ignored: the first intro stays", html="<p>more</p>")

    text = render(record)

    assert re.findall(r"<h2>(.*?)</h2>", text) == ["Placements", "Perf"]
    assert "<div class='meta'>hands</div>" in text
    assert "<td>&lt;1 cm&gt;</td>" in text and "<td>2.5</td>" in text
    assert "<details><pre>raw</pre></details>\n<p>more</p>" in text and "ignored" not in text


@pytest.mark.parametrize(
    ("first", "second", "error"),
    [
        ({"columns": ["A"]}, {"columns": ["B"]}, "has columns"),
        ({"columns": ["A"]}, {"rows": [["1", "2"]]}, "a row of 2 cells"),
        ({"columns": ["A"]}, {"html": "<p>"}, "holds a table"),
        ({"html": "<p>"}, {"rows": [["1"]]}, "holds HTML"),
        ({"intro": "x"}, {"rows": [["1"]]}, "first rows need columns"),
        ({}, {"columns": ["A"], "html": "<p>"}, "not both"),
    ],
)
def test_a_section_keeps_one_shape(first, second, error):
    record = _record()
    record.section("S", **first)

    with pytest.raises(ValueError, match=error):
        record.section("S", **second)


def test_light_and_dark_styles_are_kept():
    text = render(_record())

    assert "--bg: #ffffff" in text and "@media (prefers-color-scheme: dark)" in text


def test_a_record_without_a_folder_has_no_pictures_or_files():
    record = _record()

    with pytest.raises(ValueError, match="no report folder"):
        record.picture("test_a.py::test_one")


def test_write_puts_the_page_in_the_folder(tmp_path):
    record = _record(tmp_path / "run")
    record.results.append(_result("test_a.py::test_one"))

    page = write(record)

    assert page == tmp_path / "run" / "index.html"
    assert "test_one" in page.read_text(encoding="utf-8")
