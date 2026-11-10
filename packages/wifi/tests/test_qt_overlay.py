#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Qt Quick mouse hit-tests with the real page and mock vendor singletons.

Uses host Qt6, not the car's Qt5/Wayland surface manager. No OEM code is run.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-qt-", dir="/tmp/opencode") as directory:
    temporary = Path(directory)
    qml = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
    (temporary / "HomeScreen").mkdir()
    (temporary / "component/common").mkdir(parents=True)
    shutil.copyfile(qml / "HomeScreen/WifiPage.qml", temporary / "HomeScreen/WifiPage.qml")
    for name in ("WifiButton.qml", "WifiSwitch.qml"):
        shutil.copyfile(qml / "component" / name, temporary / "component" / name)
    (temporary / "component/common/QText.qml").write_text('import QtQuick 2.0\nText { property bool textBold: false }\n')
    for module in ("HMIEVENTS", "AppHome_Enum"):
        target = temporary / "imports" / module
        target.mkdir(parents=True)
        (target / "qmldir").write_text(f"module {module}\nDummy 1.0 Dummy.qml\n")
        (target / "Dummy.qml").write_text("import QtQml 2.0\nQtObject {}\n")
    for name in ("setup.cpp", "tst_overlay.qml"):
        shutil.copyfile(ROOT / "tests/qt" / name, temporary / name)
    (temporary / "test.pro").write_text("QT += qmltest quick qml testlib\nCONFIG += testcase c++17\nSOURCES += setup.cpp\nTARGET = overlay-test\n")
    for command in (["qmake6", "test.pro"], ["make", "-s", "-j2"]):
        result = subprocess.run(command, cwd=temporary, capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QSG_RHI_BACKEND="software",
               QML2_IMPORT_PATH=str(temporary / "imports"))
    subprocess.run([str(temporary / "overlay-test"), "-input", str(temporary)],
                   cwd=temporary, env=env, check=True, timeout=30)
