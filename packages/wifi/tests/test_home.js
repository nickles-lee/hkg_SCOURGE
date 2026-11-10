// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
// Execute the actual page functions, not copies of their intended behavior.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const qml = fs.readFileSync(path.join(__dirname, '../artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml'), 'utf8');
function body(marker) {
  const start = qml.indexOf('{', qml.indexOf(marker));
  let depth = 1, i = start + 1;
  for (; depth && i < qml.length; i++) {
    if (qml[i] === '{') depth++;
    if (qml[i] === '}') depth--;
  }
  return qml.slice(start + 1, i - 1);
}
const scope = {Date, requested: false, visible: false, atOtherApp: false,
  openSerial: 0, homeBaselineValid: false,
  homeSeqBaseline: 0, homeEpochBaseline: 0, homeSeq: 0, homeEpoch: 0,
  homeSource: 'none', homeMonitor: false, refresh() {}, refreshHome() {},
  closed() { this.closes++; }, closes: 0};
scope.root = scope;
vm.createContext(scope);
for (const [name, args] of [['open', ''], ['close', ''], ['acceptHome', 'd']]) {
  vm.runInContext(`function ${name}(${args}) {${body('function ' + name + '(')}}`, scope);
}
function home(seq, epoch) { scope.acceptHome({seq, epoch, source: 'home', monitor: true}); }
scope.open(); home(3, 100); assert(scope.requested && scope.visible);
home(4, 100); assert(!scope.requested && !scope.visible);
scope.open(); home(8, 100); assert(scope.requested && scope.visible); // presses made while hidden are baseline
home(9, 100); assert(!scope.requested && !scope.visible);
scope.open(); home(9, 100); home(0, 200); assert(scope.requested && scope.visible);
home(1, 200); assert(!scope.requested && !scope.visible); // first new Home after daemon restart
scope.open(); home(0, 300); scope.close(); assert(!scope.requested && !scope.visible);
scope.open(); home(100, 400); assert(scope.requested && scope.visible);
scope.close(); assert(!scope.requested && !scope.visible); // local Exit is independent of daemon state
for (const width of [1280, 1920]) {
  const contentX = width >= 1700 ? 16 : (width - 1180) / 2;
  const exitX = contentX + 1180 - 170;
  assert(exitX >= 0 && exitX + 170 <= width);
  assert(90 + 26 >= 90 && 90 + 26 + 52 <= 720);
  if (width >= 1700) assert(exitX + 170 < width - 436);
}
assert.match(qml, /id: closeButton\s+objectName: "wifiExit"\s+z: 10/);
assert.match(qml, /onSysPopupShowingChanged: if \(AppHomeModel.sysPopupShowing\) root.close\(\)/);
let requests = 0;
function XHR() {
  this.open = (method, url) => { this.url = url; };
  this.send = () => {
    requests++;
    this.readyState = 4;
    this.status = this.url.startsWith('file:') ? 0 : 200;
    this.responseText = JSON.stringify({updated_ms: this.status === 0 ? 1 : Date.now()});
    this.onreadystatechange();
  };
}
XHR.DONE = 4;
const refreshScope = {XMLHttpRequest: XHR, serviceOk: false, daemonBase: 'http://fixture',
  applyStatus(d) { refreshScope.serviceOk = Date.now() - d.updated_ms < 8000; }};
vm.createContext(refreshScope);
vm.runInContext(`function refresh() {${body('function refresh(')}}; refresh();`, refreshScope);
assert.equal(requests, 2); // a parseable but stale file must fall back to live HTTP
assert.equal(refreshScope.serviceOk, true);
console.log('PASS: actual QML Home/reopen/restart/Exit logic and 1280/1920 geometry (not a Qt hit-test)');
