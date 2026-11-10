#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Run the allocator extracted from the final payload; reject missing bindings."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from check_layout import check

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "tweaks/out/usb/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"
text = path.read_text()
assert check(text) == {"battery": 0, "wifi": 1, "tweaks": 2, "reboot": 3}
bad = re.sub(r"property var slot: firstEmptySlot\(0\)", "", text)
try:
    check(bad)
except AssertionError:
    pass
else:
    raise AssertionError("lost Battery placement was accepted")
# A tile block removed entirely is the exact V4 packing regression.
missing_block = re.sub(r"BatteryTile\s*\{[^{}]*\n[^{}]*\}\n", "", text)
try:
    check(missing_block)
except AssertionError as error:
    assert "missing custom tiles" in str(error), error
else:
    raise AssertionError("removed Battery tile block was accepted")
functions = "\n".join(re.search(r"    function " + name + r"\([^)]*\)\s*\{.*?\n    \}", text, re.S)[0]
                      for name in ("emptySlots", "firstEmptySlot"))
script = r'''
const vm = require('vm'), assert = require('assert');
for (const size of [8,12]) {
    let pages;
    const model = (used) => ({getCount:()=>size, getItem:i=>({isEditingVisible:()=>used.includes(i)})});
    const full = Array.from({length:size},(_,i)=>i);
    const ctx = vm.createContext({nbrItemsPerPage:size,totalPagesNormalMode:3,getPageDisplayListModel:p=>pages[p]});
    vm.runInContext(FUNCTIONS,ctx);
    pages = [model(full),model([0,1,2]),model([])];
    for (let i=0;i<4;i++) {
        const s=ctx.firstEmptySlot(i);
        assert(s.page===1 && s.index===i+3,'full first page must allocate onto second');
        const cols=size===12?6:4;
        assert((s.index%cols)>=0 && Math.floor(s.index/cols)<2);
    }
    pages=[model(full),model(full.filter(i=>i!==size-1)),model([])];
    assert(ctx.firstEmptySlot(0).page===1 && ctx.firstEmptySlot(0).index===size-1);
    assert(ctx.firstEmptySlot(1).page===2 && ctx.firstEmptySlot(1).index===0);
    pages=[model(full),model(full),model(full)];
    assert(ctx.firstEmptySlot(0)===null && ctx.firstEmptySlot(3)===null);
    pages=[model(full),model([1,2,3]),model([])];
    assert(ctx.firstEmptySlot(0).index===0,'rearranged model must re-resolve');
}
'''.replace("FUNCTIONS", json.dumps(functions))
if shutil.which("node") is None:
    # The static gate (check_layout) already ran; the behavioral allocator
    # run needs node, which is not part of the documented build deps.
    print("SKIP: node not available; allocator execution not run (static checks passed)")
else:
    subprocess.run(["node", "-e", script], check=True)
    print("PASS: final payload tile bindings; full first page spills to second; holes, overflow, rearrangement and full grids at 1280/1920")
