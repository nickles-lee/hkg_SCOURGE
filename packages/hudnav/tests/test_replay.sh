#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# hudnav-replay.sh safety properties, against a recording fake micom-send. Nothing real is sent.
set -eu
cd "$(dirname "$0")/.."
R=scripts/hudnav-replay.sh
T=$(mktemp -d /tmp/hudnav-replay.XXXXXX)
trap 'rm -rf "$T"' EXIT
export HUDNAV_REPLAY_STEP_S=0 HUDNAV_REPLAY_STOP="$T/stop" HUDNAV_MICOM_SEND="$T/send"
SENT="$T/sent"

cat > "$T/send" <<EOF
#!/bin/sh
echo "\$1" >> "$SENT"
[ -n "\${KILL_AFTER:-}" ] && [ "\$(wc -l < "$SENT")" -ge "\$KILL_AFTER" ] && : > "$HUDNAV_REPLAY_STOP"
exit 0
EOF
chmod 755 "$T/send"
count() { [ -f "$SENT" ] && wc -l < "$SENT" || echo 0; }

cat > "$T/two.txt" <<'EOF'
# fixture
STEP
4cc 0d 00 00 0c 2c 01 00 00
63e 00 00 2c 01 6e 00 90 01
STEP
4cc 0d 00 00 0c 22 01 00 00
640 ff ff ff 00 ff ff ff 5a
EOF

# 1. no --yes: validates, prints, sends NOTHING
out=$(sh $R "$T/two.txt")
echo "$out" | grep -q 'DRY RUN'
[ "$(count)" = 0 ] || { echo "dry run sent frames"; exit 1; }

# 2. --yes: the 4 fixture frames in order with the right array encoding, then the clear set twice (12)
rm -f "$SENT"; sh $R "$T/two.txt" --yes > "$T/run.txt"
[ "$(count)" = 20 ] || { echo "expected 20 sends, got $(count)"; cat "$SENT"; exit 1; }
sed -n 1p "$SENT" | grep -qx 'array:byte:0x04,0xcc,0x08,0x0d,0x00,0x00,0x0c,0x2c,0x01,0x00,0x00'
sed -n 2p "$SENT" | grep -qx 'array:byte:0x06,0x3e,0x08,0x00,0x00,0x2c,0x01,0x6e,0x00,0x90,0x01'
sed -n 4p "$SENT" | grep -qx 'array:byte:0x06,0x40,0x08,0xff,0xff,0xff,0x00,0xff,0xff,0xff,0x5a'
# clear set last: the stock END-OF-ROUTE 0x4CC (type 03). NOT type 09 = "Calculating" (HUD spinner)
tail -n 16 "$SENT" | grep -qx 'array:byte:0x04,0xcc,0x08,0x03,0xff,0xff,0xff,0xff,0xff,0xff,0xf0'
! tail -n 16 "$SENT" | grep -q 'array:byte:0x04,0xcc,0x08,0x09'
# the LAST two frames must be the stock idle (compass) pair: 03 alone leaves the checkered
# "arrived" flag on the HUD (seen on the car); idle 08 returns the display to normal
[ "$(tail -n 2 "$SENT" | head -n 1)" = 'array:byte:0x04,0xcc,0x08,0x08,0x00,0x00,0x21,0x00,0x00,0x00,0x00' ]
[ "$(tail -n 1 "$SENT")" = 'array:byte:0x04,0xe8,0x08,0xff,0xff,0xff,0x21,0xff,0xff,0xff,0x00' ]
tail -n 16 "$SENT" | grep -qx 'array:byte:0x06,0x41,0x08,0xff,0xff,0xff,0xff,0x0f,0xff,0xff,0xff'
grep -q 'step 1/2  0x4CC type=0x0d dir=0x0c dist=300 m' "$T/run.txt"

# 2b. --guide-flag: 0x4A3 B6=01 first, stock idle 0x4A3 restored LAST; without it, never any 0x4A3
! grep -q 'array:byte:0x04,0xa3' "$SENT"
rm -f "$SENT"; sh $R "$T/two.txt" --yes --guide-flag > /dev/null
[ "$(count)" = 22 ] || { echo "expected 22 sends with --guide-flag, got $(count)"; exit 1; }
[ "$(head -n 1 "$SENT")" = 'array:byte:0x04,0xa3,0x08,0x40,0x00,0x24,0x08,0x00,0x00,0x01,0x00' ]
[ "$(tail -n 1 "$SENT")" = 'array:byte:0x04,0xa3,0x08,0x40,0x00,0x24,0x08,0x00,0x00,0x00,0x00' ]

# 3. whitelist: an out-of-set ID aborts BEFORE any send 
printf 'STEP\n4ea 40 00 24 08 00 00 01 00\n' > "$T/bad_id.txt"
rm -f "$SENT"; ! sh $R "$T/bad_id.txt" --yes 2> "$T/err.txt"
grep -q 'not in the allowed set' "$T/err.txt"; [ "$(count)" = 0 ]
# ... also when the bad frame is the LAST line of an otherwise valid file
cat "$T/two.txt" > "$T/late_bad.txt"; printf '4ea 00 00 00 00 00 00 10 00\n' >> "$T/late_bad.txt"
rm -f "$SENT"; ! sh $R "$T/late_bad.txt" --yes 2> /dev/null; [ "$(count)" = 0 ]
# malformed byte / wrong length
printf 'STEP\n4cc 0d 00 00 0c 2c 01 00\n' > "$T/short.txt"
rm -f "$SENT"; ! sh $R "$T/short.txt" --yes 2> /dev/null; [ "$(count)" = 0 ]
printf 'STEP\n4cc 0d 00 00 0c 2c 01 00 zz\n' > "$T/zz.txt"
rm -f "$SENT"; ! sh $R "$T/zz.txt" --yes 2> /dev/null; [ "$(count)" = 0 ]

# 4. kill switch: the stop file appears after 2 sends; it must stop early and still send the clear set
rm -f "$SENT" "$HUDNAV_REPLAY_STOP"
KILL_AFTER=2 sh $R "$T/two.txt" --yes > "$T/kill.txt"
grep -q 'stopped early' "$T/kill.txt"
[ "$(count)" = 18 ] || { echo "expected 2 + 16 clear sends, got $(count)"; exit 1; }
tail -n 16 "$SENT" | grep -qx 'array:byte:0x04,0xcc,0x08,0x03,0xff,0xff,0xff,0xff,0xff,0xff,0xf0'

# 4b. --no-clear: a normal finish leaves the frames on the display (no clear set sent)
rm -f "$SENT" "$HUDNAV_REPLAY_STOP"
sh $R "$T/two.txt" --yes --no-clear > "$T/nc.txt"
[ "$(count)" = 4 ] || { echo "--no-clear: expected 4 sends, got $(count)"; exit 1; }
grep -q 'frames left on the display' "$T/nc.txt"
# ... but the kill switch still clears even with --no-clear
rm -f "$SENT" "$HUDNAV_REPLAY_STOP"
KILL_AFTER=2 sh $R "$T/two.txt" --yes --no-clear > "$T/nc2.txt"
[ "$(count)" = 18 ] || { echo "--no-clear + kill: expected 18 sends, got $(count)"; exit 1; }

# 4c. TP text + PAUSE: tp goes through the (fake) CANService with SetCANTPMicomRecv, validation is strict
cat > "$T/canservice" <<EOF2
#!/bin/sh
echo "\$*" >> "$T/tpsent"
exit 0
EOF2
chmod 755 "$T/canservice"
export HUDNAV_CANSERVICE="$T/canservice"
printf 'STEP
4a3 48 28 24 08 00 00 01 00
PAUSE 0.01
tp 06 e7 03 f2 00 00 00
4cc 21 00 00 0c 78 00 00 00
' > "$T/tp.txt"
rm -f "$SENT" "$T/tpsent"; sh $R "$T/tp.txt" --yes --no-clear > /dev/null
[ "$(count)" = 2 ] || { echo "tp: expected 2 CAN sends, got $(count)"; exit 1; }
[ "$(wc -l < "$T/tpsent")" = 1 ]
grep -q 'SetCANTPMicomRecv array:byte:0x06,0xe7,0x03,0xf2,0x00,0x00,0x00' "$T/tpsent"
grep -q 'dest=com.lge.car.micom' "$T/tpsent"
# a TP to any other id, a wrong length byte, or a bad pause aborts BEFORE sending anything
for bad in 'tp 06 df 03 f2 00 00 00' 'tp 06 e7 05 f2 00 00 00' 'PAUSE x'; do
    printf 'STEP
4cc 21 00 00 0c 78 00 00 00
%s
' "$bad" > "$T/badtp.txt"
    rm -f "$SENT" "$T/tpsent"; ! sh $R "$T/badtp.txt" --yes --no-clear 2> /dev/null; [ "$(count)" = 0 ]; [ ! -f "$T/tpsent" ]
done

# 5. every locally generated replay file passes validation. Real car captures are
# deliberately NOT shipped (private data); a porter may add their own redacted ones.
for f in "$T/two.txt" "$T/tp.txt"; do
    sh $R "$f" | grep -q 'DRY RUN' || { echo "invalid replay file $f"; exit 1; }
done
echo "SKIP: optional car-capture replay files not shipped (private data)"
echo "replay safety checks passed"
