// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only

// phone-rig - stand-alone on-device rig for the two-pass USB update bench.
//
// Single static binary (CGO_ENABLED=0, GOOS=linux GOARCH=arm64) so the phone
// needs no interpreter, no Termux and no PC attached. Run as root:
//
//   phone-rig probe                      prove the dm ioctl interface works
//   phone-rig status  -n phone-rig       what the LUN is actually serving now
//   phone-rig flip    -n phone-rig -mode evil   live table reload, no re-enumeration
//   phone-rig watch   -c config.json     volume+gap pass detector (heuristic)
//   phone-rig watchtrace -c config.json  offset-based pass detector (deterministic)
//
// The important subcommand is `flip`: it does DM_TABLE_LOAD + DM_DEV_SUSPEND
// (resume), which is the only way to change the bytes served by an already open
// mass-storage LUN without any media change. The dmctl build on the validated
// phone could not do this; other builds may differ.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

// BLKFLSBUF: ask the block layer to drop the device's buffer/page cache.
// Needed after a live table swap: proven on this phone that reads through the
// dm node otherwise keep returning the PREVIOUS backing's bytes.
const blkFlsbuf = 0x1261

// cfg is the production description of one rig run.
type cfg struct {
	DMName     string `json:"dmname"`
	DMNode     string `json:"dmnode"`
	GenDev     string `json:"gendev"`
	EvilDev    string `json:"evildev"`
	// EvilOff is where the padded evil image's bytes start on the evil loop.
	// A full-disk evil envelope uses the region offset; a standalone
	// sector-padded region image starts at 0. Defaults to 0 when unset.
	EvilOff     uint64 `json:"evil_off_sectors"`
	TotalSec   uint64 `json:"total_sectors"`
	RegionOff  uint64 `json:"region_off_sectors"`
	RegionLen  uint64 `json:"region_len_sectors"`
	GapMs      int    `json:"gap_ms"`
	CoverFrac  float64 `json:"cover_frac"`
	SampleMs   int    `json:"sample_ms"`
	MaxFlips   int    `json:"max_flips"`
	Log        string `json:"log"`
	// watchtrace: flip at the START of this 1-based read pass (upstream's
	// switch_after_pass=2 means CHECK reads genuine, INSTALL reads evil).
	SwitchAfterPass int `json:"switch_after_pass"`
	// TraceBufKB is the per-CPU ftrace buffer size. Default 256 KB.
	TraceBufKB int `json:"trace_buf_kb"`
	// MinPassFrac is the minimum fraction of the region that must actually have
	// been read before a return-to-start counts as a pass. Tiny header/tail
	// probes must not consume a pass. Default 0.8.
	MinPassFrac float64 `json:"min_pass_frac"`
	// QuietFlipMs flips to evil after a full sweep has been read and the HU has
	// issued no reads for this long. The car pauses ~85 s between the CHECK
	// (md5+RSA) pass and the INSTALL pass; flipping in that window means every
	// INSTALL read sees evil bytes. Flipping on INSTALL's first read instead
	// leaks one ~16 KB genuine read into the TE2 stream and corrupts decryption
	// (offline-proven: hybrid TE2 decrypts but gzip -t fails). Default 2000 ms.
	// Values <= 0 are normalised to the default; there is no legacy trigger.
	QuietFlipMs int `json:"quiet_flip_ms"`
	// LogOnly makes watchtrace log sweeps without ever flipping. Used for a
	// safe dry run on the car to capture the real HU read pattern.
	LogOnly bool `json:"log_only"`
	// TraceLog stores every in-region block read (tsv) for offline replay.
	TraceLog string `json:"trace_log"`
	// SuspendSwap defaults to TRUE when unset. linux 4.14 dm_swap_table()
	// refuses to bind a new table unless the device is suspended first
	// ("device must be suspended", dm.c:2266), so the flip must be
	// DM_DEV_SUSPEND(suspend) -> DM_TABLE_LOAD -> DM_DEV_SUSPEND(resume).
	// While suspended, I/O is queued rather than failed, which is what makes
	// the swap invisible to a head unit that is mid-read.
	SuspendSwap *bool  `json:"suspend_swap"`
	ReadOnly   bool   `json:"read_only"` // dm-level RO; the LUN's ro=1 is separate
}

func (c *cfg) swapSuspend() bool {
	return c.SuspendSwap == nil || *c.SuspendSwap
}

func loadCfg(path string) (*cfg, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	c := &cfg{}
	if err := json.Unmarshal(b, c); err != nil {
		return nil, err
	}
	if c.GapMs == 0 {
		c.GapMs = 300
	}
	if c.CoverFrac == 0 {
		c.CoverFrac = 0.95
	}
	if c.SampleMs == 0 {
		c.SampleMs = 40
	}
	if c.MaxFlips == 0 {
		c.MaxFlips = 1
	}
	if c.SwitchAfterPass == 0 {
		c.SwitchAfterPass = 1
	}
	if c.TraceBufKB == 0 {
		c.TraceBufKB = 256
	}
	if c.MinPassFrac == 0 {
		c.MinPassFrac = 0.8
	}
	if c.QuietFlipMs <= 0 {
		c.QuietFlipMs = 2000
	}
	if c.Log == "" {
		c.Log = "/data/local/tmp/scourge/events.jsonl"
	}
	if c.TraceLog == "" {
		c.TraceLog = "/data/local/tmp/scourge/trace_reads.tsv"
	}
	// dm-linear's params are "MAJ:MIN offset", never a path - passing
	// /dev/block/loop27 makes its constructor fail and DM_TABLE_LOAD answers
	// EINVAL. Resolve node paths once, here, so every subcommand agrees.
	for _, p := range []*string{&c.GenDev, &c.EvilDev} {
		if *p == "" {
			continue
		}
		dv, err := devArg(*p)
		if err != nil {
			return nil, err
		}
		*p = dv
	}
	return c, nil
}

// validateCfg rejects geometry that would underflow the evil table's tail
// segment or point past the end of the modified backing device.
func validateCfg(c *cfg) error {
	if c.RegionLen == 0 {
		return fmt.Errorf("region_len_sectors must be > 0")
	}
	if c.RegionOff >= c.TotalSec {
		return fmt.Errorf("region_off_sectors %d >= total_sectors %d", c.RegionOff, c.TotalSec)
	}
	if c.RegionOff+c.RegionLen >= c.TotalSec {
		return fmt.Errorf("region %d+%d must end before total_sectors %d (zero-length tail segment)", c.RegionOff, c.RegionLen, c.TotalSec)
	}
	return nil
}

// tableDesc renders a target list the same way dmDev.segments does, so a
// readback can be compared by content, not just by segment count.
func tableDesc(ts []targetSpec) string {
	parts := make([]string, 0, len(ts))
	for _, t := range ts {
		parts = append(parts, fmt.Sprintf("%d+%d %s %s", t.Start, t.Len, t.Type, t.Params))
	}
	return strings.Join(parts, " | ")
}

// devArg turns a block node path into the "MAJ:MIN" form dm targets want.
func devArg(p string) (string, error) {
	if strings.ContainsAny(p, ":") {
		return p, nil
	}
	var st syscall.Stat_t
	if err := syscall.Stat(p, &st); err != nil {
		return "", fmt.Errorf("stat %s: %w", p, err)
	}
	if st.Mode&syscall.S_IFMT != syscall.S_IFBLK {
		return "", fmt.Errorf("%s is not a block device", p)
	}
	return fmt.Sprintf("%d:%d", linuxMajor(st.Rdev), linuxMinor(st.Rdev)), nil
}

// linuxMajor/linuxMinor decode a Linux kernel dev_t (the glibc layout, which is
// what stat().st_rdev holds): major = (rdev>>8)&0xfff | (rdev>>32)&0xfffff000,
// minor = (rdev&0xff) | ((rdev>>12)&0xffffff00).
func linuxMajor(rdev uint64) uint64 { return (rdev>>8)&0xfff | (rdev>>32)&0xfffff000 }
func linuxMinor(rdev uint64) uint64 { return (rdev & 0xff) | ((rdev >> 12) & 0xffffff00) }

// table builds the splice for a mode.
//
// genuine: one segment - the whole disk straight from the genuine loop.
// evil:    three segments - genuine head, evil body, genuine tail.
// Both tables address the same two loop devices, so a flip is a table swap and
// nothing else: no loop churn, no LUN churn, no USB re-enumeration.
func table(c *cfg, mode string) []targetSpec {
	off, ln, tot := c.RegionOff, c.RegionLen, c.TotalSec
	if mode == "genuine" {
		return []targetSpec{{Start: 0, Len: tot, Type: "linear", Params: fmt.Sprintf("%s 0", c.GenDev)}}
	}
	return []targetSpec{
		{Start: 0, Len: off, Type: "linear", Params: fmt.Sprintf("%s 0", c.GenDev)},
		{Start: off, Len: ln, Type: "linear", Params: fmt.Sprintf("%s %d", c.EvilDev, c.EvilOff)},
		{Start: off + ln, Len: tot - off - ln, Type: "linear", Params: fmt.Sprintf("%s %d", c.GenDev, off+ln)},
	}
}

// dropPageCache flushes clean page-cache pages system-wide.
//
// This is not cosmetic: after a live table swap the dm node's address_space
// still holds blocks read through the OLD mapping, so the host (and our own
// verification reads) keep seeing the previous content. linux 4.14 has no way
// to flush one block device: BLKFLSBUF returns EINVAL for dm because
// blkdev_flushbuf() requires disk->fops->flush and dm does not provide one
// (observed: "invalid argument"). /proc/sys/vm/drop_caches=1 is the lever that
// actually works, and the suspend/resume around the swap guarantees no I/O is
// in flight while we do it.
func dropPageCache() error {
	if err := os.WriteFile("/proc/sys/vm/drop_caches", []byte("1\n"), 0); err != nil {
		return fmt.Errorf("drop_caches: %w", err)
	}
	return nil
}

func flushbufs(node string) error {
	f, err := os.OpenFile(node, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("open %s: %w", node, err)
	}
	defer f.Close()
	if err := rawIoctl(f.Fd(), blkFlsbuf, 0); err != nil {
		return fmt.Errorf("BLKFLSBUF %s: %w", node, err)
	}
	return nil
}

// flip performs the live table reload. Returns how long the swap itself took.
func flip(c *cfg, mode string) (time.Duration, error) {
	if err := validateCfg(c); err != nil {
		return 0, err
	}
	d, err := openCtrl()
	if err != nil {
		return 0, err
	}
	defer d.Close()
	if msg, err := d.version(); err != nil {
		return 0, err
	} else {
		logf(c, "dm-if", msg)
	}
	ts := table(c, mode)
	t0 := time.Now()
	// 4.14 will not bind a new table onto a live device unless we suspend it
	// first, so the real flip is suspend -> load -> resume. Suspend queues I/O
	// instead of failing it, so the host just sees a brief stall.
	if c.swapSuspend() {
		if err := d.suspend(c.DMName); err != nil {
			return 0, fmt.Errorf("suspend: %w", err)
		}
	}
	if err := d.loadTable(c.DMName, ts, c.ReadOnly); err != nil {
		if c.swapSuspend() {
			// Never leave the device frozen: resume with whatever table
			// it already had, or the head unit hangs.
			if rerr := d.resume(c.DMName); rerr != nil {
				logf(c, "CRIT", "load failed AND resume failed: "+rerr.Error()+" - LUN may be stalled; power-cycle the phone")
			}
		}
		return 0, err
	}
	if err := d.resume(c.DMName); err != nil {
		// On this 4.14 kernel the resume ioctl can return EINVAL even when the
		// kernel has already activated the new table (seen under heavy I/O).
		// Never abort on that alone: read the LIVE table back and only fail if
		// the swap really did not happen. Above all, never leave the LUN
		// suspended - that stalls the USB gadget and hangs the head unit.
		if n, desc, serr := d.segments(c.DMName); serr == nil && n == len(ts) {
			logf(c, "WARN", fmt.Sprintf("resume returned %v but live table is correct (%s) - continuing", err, desc))
		} else {
			_ = d.resume(c.DMName) // last chance to un-freeze before returning
			return 0, fmt.Errorf("resume: %w (live table n=%v desc=%q err=%v)", err, n, desc, serr)
		}
	}
	el := time.Since(t0)

	// Prove the kernel really swapped it: read the LIVE table back and count
	// segments. A flip that "succeeded" but left the old table in place is the
	// failure mode that would silently produce a genuine install.
	if c.DMName != "" {
		got, desc, err := d.segments(c.DMName)
		if err != nil {
			// Never leave the device suspended just because the readback failed.
			_ = d.resume(c.DMName)
			logf(c, "CRIT", "cannot read back live table: "+err.Error())
			return 0, fmt.Errorf("cannot read back live table: %w", err)
		}
		want := tableDesc(ts)
		if got != len(ts) || desc != want {
			// Compare content, not just segment count: a stale or wrong table
			// with the same number of segments must also be a stop.
			logf(c, "CRIT", fmt.Sprintf("live table mismatch: got %d %q, want %d %q", got, desc, len(ts), want))
			return 0, fmt.Errorf("live table mismatch")
		}
		logf(c, "live", desc)
	}

	if c.DMNode != "" {
		// dm has no ->flush in 4.14's block_device_operations, so BLKFLSBUF on
		// the dm node returns EINVAL. Keep trying, but the real work is done by
		// dropPageCache below.
		if err := flushbufs(c.DMNode); err != nil {
			logf(c, "WARN", err.Error())
		}
	}
	if err := dropPageCache(); err != nil {
		logf(c, "CRIT", err.Error()+" - host will keep reading the OLD mapping")
		return el, err
	}
	logf(c, "flip", fmt.Sprintf("mode=%s targets=%d swap=%s", mode, len(ts), el))
	return el, nil
}

// ---- diskstats pass detector ----

type dsRow struct {
	reads      uint64
	sectorsRd  uint64
}

// readDiskstats pulls one row for the named block device (last field match).
func readDiskstats(name string) (*dsRow, error) {
	f, err := os.Open("/proc/diskstats")
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var found *dsRow
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		f2 := strings.Fields(sc.Text())
		if len(f2) < 7 {
			continue
		}
		if !strings.HasSuffix(f2[2], name) {
			continue
		}
		// exact device-name match (avoid dm-N matching dm-NN)
		if f2[2] != name {
			continue
		}
		r, _ := strconv.ParseUint(f2[3], 10, 64)
		s, _ := strconv.ParseUint(f2[5], 10, 64)
		found = &dsRow{reads: r, sectorsRd: s}
	}
	if found == nil {
		return nil, fmt.Errorf("%s not in /proc/diskstats", name)
	}
	return found, nil
}

// dsKey is the name /proc/diskstats uses for our device.
//
// It is NOT the dm name: device-mapper gendisks are named after the node
// (dm-N), so sampling "phone-rig" finds nothing. Derive it from the node path.
func dsKey(c *cfg) string {
	if c.DMNode != "" {
		return strings.TrimPrefix(c.DMNode, "/dev/block/")
	}
	return c.DMName
}

// watch samples diskstats and flips when a full read pass has just ended.
//
// A "pass" = a burst of activity that read at least CoverFrac * region sectors,
// followed by GapMs of silence on the device. That silence is the CHECK->INSTALL
// gap; flipping inside it means the next pass (INSTALL) reads the patched body.
// If the detection is wrong the failure mode is benign: the head unit installs
// the genuine image and we simply did nothing harmful.
func watch(c *cfg) error {
	key := dsKey(c)
	prev, err := readDiskstats(key)
	if err != nil {
		return err
	}
	burstSectors := uint64(0)
	burstStart := time.Now()
	lastIO := time.Now()
	flips := 0
	need := uint64(float64(c.RegionLen) * c.CoverFrac)
	tick := time.Duration(c.SampleMs) * time.Millisecond
	logf(c, "watch", fmt.Sprintf("start disk=%s region=%d..%d need_sectors=%d gap=%dms maxflips=%d",
		key, c.RegionOff, c.RegionOff+c.RegionLen, need, c.GapMs, c.MaxFlips))
	for {
		time.Sleep(tick)
		now, err := readDiskstats(key)
		if err != nil {
			logf(c, "WARN", err.Error())
			continue
		}
		dSec := uint64(0)
		if now.sectorsRd > prev.sectorsRd {
			dSec = now.sectorsRd - prev.sectorsRd
		}
		prev = now
		if dSec > 0 {
			if burstSectors == 0 {
				burstStart = time.Now()
			}
			burstSectors += dSec
			lastIO = time.Now()
			continue
		}
		// no new reads this sample: is the burst over?
		if burstSectors > 0 && time.Since(lastIO) >= time.Duration(c.GapMs)*time.Millisecond {
			frac := float64(burstSectors) / float64(c.RegionLen)
			logf(c, "burst", fmt.Sprintf("sectors=%d frac=%.3f dur=%s gap=%s",
				burstSectors, frac, lastIO.Sub(burstStart), time.Since(lastIO)))
			if frac >= c.CoverFrac && flips < c.MaxFlips {
				if _, err := flip(c, "evil"); err != nil {
					logf(c, "ERROR", err.Error())
				} else {
					flips++
					logf(c, "ARMED", fmt.Sprintf("flipped to evil after pass %d; next pass reads patched bytes", flips))
					if flips >= c.MaxFlips {
						logf(c, "watch", "max flips reached, exiting")
						return nil
					}
				}
			}
			burstSectors = 0
		}
	}
}

// ---- block-trace pass detector (deterministic) ----
//
// Upstream's Pi/nbdkit rig counts physical read passes and switches at pass #2.
// Our f_mass_storage LUN is opened once, so instead we watch the kernel block
// layer: `block_rq_issue` events on the genuine loop carry the exact sector the
// head unit is reading. We use upstream's proven "return-to-start-after-deep"
// trigger, which copes with the HU reading the file tail first and caching the
// last blocks (the exact reason upstream's old end-of-region trigger failed).
//
// A read is "near" if it starts within 64 KB of the region start, and "deep" if
// the largest sector seen in the previous sweep is past the region midpoint.
// A new sweep begins on a near read after a deep one. We flip when the start of
// pass #SwitchAfterPass is observed.

const ftraceRoot = "/sys/kernel/debug/tracing"

func ftraceWrite(rel, val string) error {
	return os.WriteFile(ftraceRoot+rel, []byte(val), 0)
}

// devtOf resolves cfg's "MAJ:MIN" into the encoded dev_t the ftrace filter uses.
func devtOf(s string) (uint64, error) {
	p := strings.Split(s, ":")
	if len(p) != 2 {
		return 0, fmt.Errorf("bad device %q", s)
	}
	maj, e1 := strconv.ParseUint(p[0], 10, 32)
	min, e2 := strconv.ParseUint(p[1], 10, 32)
	if e1 != nil || e2 != nil {
		return 0, fmt.Errorf("bad device %q", s)
	}
	return (maj << 20) | min, nil
}

func watchTrace(c *cfg) error {
	if err := validateCfg(c); err != nil {
		return err
	}
	devt, err := devtOf(c.GenDev)
	if err != nil {
		return err
	}
	ev := ftraceRoot + "/events/block/block_rq_issue"

	// Reversible setup. Any failure here is fatal: a half-armed tracer would
	// either miss the CHECK pass or spew unrelated events.
	steps := []struct{ path, val string }{
		{ftraceRoot + "/tracing_on", "0"},
		{ftraceRoot + "/buffer_size_kb", fmt.Sprintf("%d", c.TraceBufKB)},
		{ev + "/filter", fmt.Sprintf("dev == %d", devt)},
		{ftraceRoot + "/trace", "\n"},
		{ev + "/enable", "1"},
	}
	for _, s := range steps {
		if err := os.WriteFile(s.path, []byte(s.val), 0); err != nil {
			return fmt.Errorf("ftrace %s=%s: %w", s.path, s.val, err)
		}
	}
	cleanup := func() {
		_ = ftraceWrite("/tracing_on", "0")
		_ = ftraceWrite(ev + "/enable", "0")
		_ = ftraceWrite(ev + "/filter", "0")
	}
	defer cleanup()

	// The phone's page cache would otherwise answer the HU's reads without
	// issuing any block request - invisible to both the tracer and diskstats.
	if err := dropPageCache(); err != nil {
		return err
	}
	if err := ftraceWrite("/tracing_on", "1"); err != nil {
		return err
	}

	f, err := os.Open(ftraceRoot + "/trace_pipe")
	if err != nil {
		return err
	}
	defer f.Close()

	// Raw per-read log, so a dry run can be replayed through the detector
	// offline with different thresholds before risking a real flip.
	var tf *os.File
	var tw *bufio.Writer
	if c.TraceLog != "" {
		tf, err = os.OpenFile(c.TraceLog, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0644)
		if err != nil {
			return fmt.Errorf("trace log %s: %w", c.TraceLog, err)
		}
		tw = bufio.NewWriterSize(tf, 64*1024)
		defer func() {
			_ = tw.Flush()
			_ = tf.Close()
		}()
		fmt.Fprintln(tw, "#ts_ms\tsector\tcount\tnear\tdeep\tmax")
	}

	regionStart := c.RegionOff
	regionEnd := regionStart + c.RegionLen
	half := regionStart + c.RegionLen/2
	// upstream: near = off <= start + min(64KB, max(4KB, len/8)), bytes -> sectors.
	nearBytes := uint64(0x10000)
	if v := c.RegionLen * 512 / 8; v > 0x1000 {
		nearBytes = v
	}
	if nearBytes > 0x10000 {
		nearBytes = 0x10000
	}
	nearLimit := regionStart + nearBytes/512

	passes := 0
	maxSeen, reads, sectors := uint64(0), uint64(0), uint64(0)
	wasNear := false

	// Quiet-window flip (default). The HU verifies the signed CHECK pass
	// (md5+RSA) before INSTALL, leaving an ~85 s read gap. Flip in that gap so
	// the whole INSTALL stream is evil. Flipping on INSTALL's first read leaks
	// one ~16 KB genuine read into the TE2 stream: offline test proved the
	// resulting hybrid TE2 decrypts but fails gzip -t, which is the 0% error.
	// The scanner goroutine blocks in Scan(), so share state with atomics.
	var lastReadMs, sweepReady, quietAttempted, quietOK atomic.Int64
	lastReadMs.Store(time.Now().UnixMilli())
	nearEnd := regionStart + uint64(float64(c.RegionLen)*0.95)
	if c.QuietFlipMs > 0 {
		go func() {
			tick := time.NewTicker(100 * time.Millisecond)
			defer tick.Stop()
			for range tick.C {
				if quietAttempted.Load() != 0 {
					return
				}
				if sweepReady.Load() == 0 {
					continue
				}
				quiet := time.Since(time.UnixMilli(lastReadMs.Load()))
				if quiet < time.Duration(c.QuietFlipMs)*time.Millisecond {
					continue
				}
				if !quietAttempted.CompareAndSwap(0, 1) {
					return
				}
				logf(c, "QUIET", fmt.Sprintf("no reads for %d ms after full sweep - flipping before INSTALL", quiet.Milliseconds()))
				if c.LogOnly {
					logf(c, "DRYRUN", "would flip here (quiet window); -logonly, leaving GENUINE bytes in place")
					return
				}
				if _, err := flip(c, "evil"); err != nil {
					// Do not claim ARMED or suppress the pass-boundary fallback.
					logf(c, "CRIT", "quiet-window flip failed: "+err.Error())
					return
				}
				quietOK.Store(1)
				logf(c, "ARMED", "flipped to evil in CHECK->INSTALL quiet window (all INSTALL reads evil)")
			}
		}()
	}
	logf(c, "trace", fmt.Sprintf("start dev=%s devt=%d region=%d..%d near<=%d half=%d switch@pass=%d quiet=%dms",
		c.GenDev, devt, regionStart, regionEnd, nearLimit, half, c.SwitchAfterPass, c.QuietFlipMs))

	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		line := sc.Text()
		i := strings.Index(line, "block_rq_issue: ")
		if i < 0 {
			continue
		}
		p := strings.Fields(line[i+len("block_rq_issue: "):])
		if len(p) < 7 || !strings.Contains(p[1], "R") {
			continue
		}
		sector, e1 := strconv.ParseUint(p[4], 10, 64)
		count, e2 := strconv.ParseUint(p[6], 10, 64)
		if e1 != nil || e2 != nil {
			continue
		}
		if sector+count <= regionStart || sector >= regionEnd {
			continue // read does not touch the rootfs region
		}
		reads++
		sectors += count
		if end := sector + count; end > maxSeen {
			maxSeen = end
		}
		if c.QuietFlipMs > 0 {
			lastReadMs.Store(time.Now().UnixMilli())
			if sectors >= uint64(float64(c.RegionLen)*c.MinPassFrac) && maxSeen >= nearEnd {
				sweepReady.Store(1)
			}
		}
		near := sector <= nearLimit
		deep := maxSeen >= half
		if tw != nil {
			fmt.Fprintf(tw, "%d\t%d\t%d\t%v\t%v\t%d\n", time.Now().UnixMilli(), sector, count, near, deep, maxSeen)
			if reads%200 == 0 {
				_ = tw.Flush()
			}
		}

		// A pass boundary is a return to the region start after deep reads.
		// But a boundary only counts as a PASS if the sweep just completed
		// actually read most of the region. Real car runs showed two shapes:
		//   - a tiny tail/header probe before CHECK (0.000 region), and
		//   - CHECK's full 1.000-region read followed by INSTALL's full read.
		// Counting the probe as pass #1 made us switch after INSTALL (too late).
		// Requiring MinPassFrac of real reads makes both shapes count CHECK as
		// pass #1, so switch_after_pass=1 flips at the INSTALL start.
		if near && !wasNear && deep {
			if sectors >= uint64(float64(c.RegionLen)*c.MinPassFrac) {
				passes++
				logf(c, "PASS", fmt.Sprintf("#%d START sector=%d reads=%d sectors=%d (%.3f region) max=%d",
					passes, sector, reads, sectors, float64(sectors)/float64(c.RegionLen), maxSeen))
				if passes >= c.SwitchAfterPass {
					logf(c, "SWITCH", fmt.Sprintf("pass=%d (switch_after_pass=%d)", passes, c.SwitchAfterPass))
					if quietOK.Load() != 0 {
						logf(c, "SKIP", "already flipped in quiet window - INSTALL reads stay evil")
					} else if c.LogOnly {
						logf(c, "DRYRUN", "would flip at pass boundary; -logonly, leaving GENUINE bytes in place")
					} else {
						// Deliberately do NOT flip here. The quiet window is the
						// only safe point: flipping on INSTALL's first read leaks a
						// genuine prefix into the modified container and the
						// update fails at 0%. If the quiet window was missed,
						// leave GENUINE in place: the update installs stock, which
						// is a benign outcome rather than a corrupt one.
						logf(c, "CRIT", "quiet window missed - not flipping at pass boundary; install will use GENUINE bytes")
					}
				}
			} else {
				logf(c, "SKIP", fmt.Sprintf("tiny sweep: sectors=%d (%.3f region) - not a pass",
					sectors, float64(sectors)/float64(c.RegionLen)))
			}
			reads, sectors = 0, 0
			maxSeen = 0
		}
		wasNear = near
	}
	return sc.Err()
}

// ---- logging ----

var logFile *os.File

func logf(c *cfg, tag, msg string) {
	line := fmt.Sprintf("%s %-6s %s", time.Now().Format("2006-01-02T15:04:05.000"), tag, msg)
	fmt.Println(line)
	if logFile != nil {
		fmt.Fprintln(logFile, line)
		logFile.Sync()
	}
}

func openLog(path string) {
	if path == "" {
		return
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "log %s: %v\n", path, err)
		return
	}
	logFile = f
}

// ---- CLI ----

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: phone-rig probe|status|flip|watch|watchtrace|create|remove [-c config.json] ...")
		os.Exit(2)
	}
	cmd := os.Args[1]
	fs := flag.NewFlagSet(cmd, flag.ExitOnError)
	var cPath, mode, dmName, dmNode string
	fs.StringVar(&cPath, "c", "", "config json")
	fs.StringVar(&mode, "mode", "evil", "genuine|evil (flip only)")
	fs.StringVar(&dmName, "n", "", "dm device name")
	fs.StringVar(&dmNode, "node", "", "dm block node (for BLKFLSBUF)")
	var ro bool
	var logOnly bool
	// Default is a READ-WRITABLE dm device: write protection lives at the USB
	// layer (lun.0/ro=1), and keeping the dm table writable avoids dm refusing
	// a mode change while the gadget has the device open. Pass -ro to force it.
	fs.BoolVar(&ro, "ro", false, "create/load read-only")
	fs.BoolVar(&logOnly, "logonly", false, "watchtrace: log sweeps, never flip (safe car dry run)")
	// loadtest only (diagnostic): one hand-written target, to separate "our
	// encoding is wrong" from "that target type is not registered".
	var ltType, ltParams, ltDev string
	var ltStart, ltLen uint64
	fs.StringVar(&ltType, "type", "error", "loadtest: target type")
	fs.StringVar(&ltParams, "params", "", "loadtest: target params string")
	fs.StringVar(&ltDev, "dev", "", "loadtest: block node; params become MAJ:MIN 0")
	fs.Uint64Var(&ltStart, "start", 0, "loadtest: start sector")
	fs.Uint64Var(&ltLen, "len", 1048576, "loadtest: length in sectors")
	var hexDump bool
	fs.BoolVar(&hexDump, "hex", false, "status: also dump the kernel's raw payload bytes")
	fs.Parse(os.Args[2:])

	c := &cfg{}
	if cPath != "" {
		var err error
		if c, err = loadCfg(cPath); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if dmName != "" {
		c.DMName = dmName
	}
	if dmNode != "" {
		c.DMNode = dmNode
	}
	if logOnly {
		c.LogOnly = true
	}
	if c.DMName == "" {
		c.DMName = "phone-rig"
	}
	if ro {
		c.ReadOnly = true
	}
	openLog(c.Log)

	switch cmd {
	case "probe":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		msg, err := d.version()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Println(msg)
		if names, err := d.list(); err == nil {
			fmt.Println("devices:", strings.Join(names, " "))
		} else {
			fmt.Fprintln(os.Stderr, "DM_LIST_DEVICES:", err)
		}
	case "loadtest":
		// Diagnostic: load one arbitrary target so we can tell "our encoding is
		// wrong" apart from "this target type is not registered in the kernel".
		// "error" is always built in; "linear" may be an unloaded module.
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		params := ltParams
		if ltDev != "" {
			dv, err := devArg(ltDev)
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
			params = fmt.Sprintf("%s 0", dv)
		}
		ts := []targetSpec{{Start: ltStart, Len: ltLen, Type: ltType, Params: params}}
		fmt.Printf("load type=%q params=%q len=%d ro=%v\n", ltType, params, ltLen, ro)
		// Hex dump of the record we are about to send, so it can be compared
		// byte for byte against what the kernel writes back for a device that
		// dmctl created successfully.
		packed := packTargets(ts)
		fmt.Printf("payload(%d): %x\n", len(packed), packed)
		if err := d.loadTableBuf(c.DMName, packed, ro); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		if err := d.resume(c.DMName); err != nil {
			fmt.Fprintln(os.Stderr, "resume:", err)
			os.Exit(1)
		}
		s, err := d.status(c.DMName)
		if err != nil {
			fmt.Fprintln(os.Stderr, "status:", err)
			os.Exit(1)
		}
		fmt.Println("loaded:", s)
	case "targets":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		tt, err := d.targetTypes()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		for _, t := range tt {
			fmt.Println(t)
		}
	case "list":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		names, err := d.list()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		for _, n := range names {
			fmt.Println(n)
		}
	case "status":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		s, err := d.status(c.DMName)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Printf("table of '%s':\n%s", c.DMName, s)
		if hexDump {
			raw, err := d.statusRaw(c.DMName)
			if err != nil {
				fmt.Fprintln(os.Stderr, "statusRaw:", err)
				break
			}
			fmt.Printf("kernel payload (%d bytes): %x\n", len(raw), raw)
		}
	case "flip":
		if mode != "genuine" && mode != "evil" {
			fmt.Fprintln(os.Stderr, "flip -mode must be genuine or evil")
			os.Exit(2)
		}
		if c.GenDev == "" || c.EvilDev == "" || c.TotalSec == 0 {
			fmt.Fprintln(os.Stderr, "flip needs -c config.json with gendev/evildev/total_sectors/region_off_sectors/region_len_sectors")
			os.Exit(2)
		}
		if _, err := flip(c, mode); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	case "watch":
		if c.GenDev == "" || c.EvilDev == "" || c.TotalSec == 0 {
			fmt.Fprintln(os.Stderr, "watch needs -c config.json (gendev/evildev/total/region)")
			os.Exit(2)
		}
		if err := watch(c); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	case "watchtrace":
		if c.GenDev == "" || c.EvilDev == "" || c.TotalSec == 0 {
			fmt.Fprintln(os.Stderr, "watchtrace needs -c config.json (gendev/evildev/total/region)")
			os.Exit(2)
		}
		if err := watchTrace(c); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	case "create":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		if err := d.create(c.DMName, ro || c.ReadOnly); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Printf("created %s\n", c.DMName)
	case "remove":
		d, err := openCtrl()
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer d.Close()
		if err := d.remove(c.DMName); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		fmt.Printf("removed %s\n", c.DMName)
	default:
		fmt.Fprintln(os.Stderr, "unknown subcommand "+cmd)
		os.Exit(2)
	}
}
