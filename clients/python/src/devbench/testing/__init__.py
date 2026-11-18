"""The opt-in pytest plugin for in-game end-to-end tests. A mod's rootdir conftest turns it on and configures it:

    pytest_plugins = ["devbench.testing"]
    DEVBENCH = devbench.testing.Config(game="fallout4", vr=True, tool="mymod", ...)

pytest registers this module itself, so the hooks and fixtures are re-exported here. devbench.testing.report renders
the run's record (the report fixture) as an HTML page.

PYTEST_DONT_REWRITE: the conftest imports this package for Config before pytest loads it as a plugin, too late to
rewrite its asserts, which pytest would warn about on every run; they are not test asserts, so none is lost.
"""

from devbench.testing._config import Area, Config, Dependency
from devbench.testing._plugin import (
    bench,
    devbench_test_mark,
    events,
    game,
    handedness_fixture,
    log_mark,
    mod,
    pytest_addoption,
    pytest_collection_modifyitems,
    pytest_configure,
    pytest_sessionstart,
    pytest_terminal_summary,
    report_fixture,
)
from devbench.testing._record import Failure, Picture, Record, Result, Section

__all__ = [
    "Area",
    "Config",
    "Dependency",
    "Failure",
    "Picture",
    "Record",
    "Result",
    "Section",
    "bench",
    "devbench_test_mark",
    "events",
    "game",
    "handedness_fixture",
    "log_mark",
    "mod",
    "pytest_addoption",
    "pytest_collection_modifyitems",
    "pytest_configure",
    "pytest_sessionstart",
    "pytest_terminal_summary",
    "report_fixture",
]
