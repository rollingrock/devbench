"""A dump as the report keeps it: devbench's BMP turned into a PNG, whole or its left half."""

import pytest
from fake_devbench import make_bmp, read_png

from devbench.testing._picture import bmp_to_png


def _pixel(x, y):
    """A stored BMP pixel (B, G, R) that names its column and file row."""
    return bytes([x, y, 200])


def _rgb(x, y):
    return (200, y, x)


def test_a_bottom_up_bmp_becomes_a_top_down_png():
    width, height, pixels = read_png(bmp_to_png(make_bmp(5, 3, _pixel)))

    assert (width, height) == (5, 3)
    assert pixels == [[_rgb(x, y) for x in range(5)] for y in (2, 1, 0)]


def test_a_top_down_bmp_keeps_its_row_order():
    _, _, pixels = read_png(bmp_to_png(make_bmp(2, 2, _pixel, top_down=True)))

    assert pixels == [[_rgb(x, y) for x in range(2)] for y in (0, 1)]


@pytest.mark.parametrize(("width", "kept"), [(6, 3), (5, 2)])
def test_left_half_keeps_the_left_columns(width, kept):
    width, height, pixels = read_png(bmp_to_png(make_bmp(width, 2, _pixel), left_half=True))

    assert (width, height) == (kept, 2)
    assert pixels == [[_rgb(x, y) for x in range(kept)] for y in (1, 0)]


@pytest.mark.parametrize(
    ("data", "error"),
    [
        (b"PNG not a bmp" * 10, "not a BMP"),
        (make_bmp(2, 2, _pixel, bits=32), "32-bit BMP"),
        (make_bmp(4, 4, _pixel)[:-10], "shorter than its header"),
    ],
)
def test_other_files_are_refused(data, error):
    with pytest.raises(ValueError, match=error):
        bmp_to_png(data)
