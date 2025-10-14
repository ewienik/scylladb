#
# Copyright (C) 2025-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
#

from pathlib import Path
import pytest
from pytest import Collector
from pytest import File
from pytest import Item
import subprocess

def pytest_collect_file(parent: Collector, file_path: Path) -> Collector | None:
    if file_path.name == "run-validator":
        return ValidatorFile.from_parent(parent, path=file_path)


class ValidatorFile(pytest.File):
    def collect(self):
        yield ValidatorItem.from_parent(self, name='vector-search-validator', path=self.path)


class ValidatorItem(pytest.Item):
    def runtest(self):
        subprocess.run([self.path, "build/dev/scylla", "build"], check=True)

    #def repr_failure(self, excinfo):
    #    """Called when self.runtest() raises an exception."""
    #    if isinstance(excinfo.value, YamlException):
    #        return "\n".join(
    #            [
    #                "usecase execution failed",
    #                "   spec failed: {1!r}: {2!r}".format(*excinfo.value.args),
    #                "   no further details known at this point.",
    #            ]
    #        )
    #    return super().repr_failure(excinfo)

    def reportinfo(self):
        return self.path, 0, f"usecase: {self.name}"


#class ValidatorException(Exception):
#    """Validator exception for error reporting."""
