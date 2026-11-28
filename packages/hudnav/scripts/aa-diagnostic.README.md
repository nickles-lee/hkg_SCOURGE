# Android Auto log-only diagnostic for AU Gen5W 260319

This package contains a background receiver and an Android Auto callback shim. The
receiver is built with only its log backend. It records AppManager ownership and
numeric navigation callback arguments. It omits road names and turn images.

`sh run-aa-diagnostic.sh --check` verifies the package and the two stock 260319
projection binaries. Starting the same script without arguments opens a temporary
socket and prints its path, the shim path, and the navigation log path. The receiver
and monitor stop with the script. Logs remain in the printed temporary directory.

The launcher starts the diagnostic receiver only. It does not start or restart
AppProjection. A head-unit test must separately arrange for the existing projection
launcher to load the supplied shim for that session and pass the printed socket path as
`HUDNAV_SOCK`. This package includes no firmware image or persistent installer.

During a parked test, start and cancel a route via Android Auto. Look for
`APPMANAGER owner=111`, `RX STATE`, `RX TURN`, and `RX DIST` records and an
`APPMANAGER owner=0` release. `RX` records show callback data before the output
gate accepts it. `FRAME` records are the daemon's computed display values.
This package has not been run on the head unit.
