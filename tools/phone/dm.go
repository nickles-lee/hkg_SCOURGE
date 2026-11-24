// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only

// dm.go - device-mapper control through the /dev/block/dm-ctrl ioctls.
//
// Why this file exists: the dmctl build on the validated phone can create/delete/list dm devices but
// has NO table-reload capability, and a *live* table reload is the whole point
// of the rig - it is how the bytes served by an already-enumerated, already-open
// USB mass-storage LUN change underneath the host, with no media change and no
// re-enumeration.
//
// Everything here was verified against linux v4.14 (this phone's dm interface
// reports 4.37.0):
//   include/uapi/linux/dm-ioctl.h  -> struct dm_ioctl, struct dm_target_spec
//   drivers/md/dm-ioctl.c          -> ctl_ioctl, copy_params, populate_table
// Three things that are easy to get wrong and were:
//   * ctl_ioctl() only checks _IOC_TYPE (0xfd) and _IOC_NR - the ioctl command's
//     size bits are ignored, so a "wrong" _IOWR size is harmless.
//   * copy_params() treats dm_ioctl.data_size as the TOTAL buffer size and
//     rejects anything below offsetof(dm_ioctl, data)=305. Passing 0 for a
//     payload-less call gives -EINVAL.
//   * struct dm_target_spec in this version HAS a u32 `next` field (bytes from
//     the start of THIS record to the start of the NEXT), and the params string
//     starts at sizeof(dm_target_spec)=40 (see the derivation below), not 52.
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"strings"
	"syscall"
	"unsafe"
)

const (
	dmIoctl = 0xfd // DM_IOCTL

	// Command numbers come from the enum in uapi linux/dm-ioctl.h and are easy
	// to get off-by-one: TABLE_CLEAR=10, TABLE_DEPS=11, TABLE_STATUS=12. Using
	// 11 instead of 12 silently returns a 16-byte deps struct.
	dmVersion      = 0x00
	dmRemoveAll    = 0x01
	dmListDevices  = 0x02
	dmDevCreate    = 0x03
	dmDevRemove    = 0x04
	dmDevRename    = 0x05
	dmDevSuspend   = 0x06
	dmDevStatus    = 0x07
	dmDevWait      = 0x08
	dmTableLoad    = 0x09
	dmTableClear   = 0x0a
	dmTableDeps    = 0x0b
	dmTableStatus  = 0x0c
	dmListVersions = 0x0d
	dmTargetMsg    = 0x0e

	// Flags as defined in linux v4.14 include/uapi/linux/dm-ioctl.h. Getting
	// these wrong is silent and nasty: the first version of this file used
	// libdm's internal names, where 1<<1 is a "name" flag - in the uapi 1<<1
	// is DM_SUSPEND_FLAG and 1<<14 is DM_UUID_FLAG, which made DM_TABLE_STATUS
	// look the device up by UUID and fail with ENXIO.
	fReadOnlyIn    = 1 << 0  // DM_READONLY_FLAG
	fSuspendIn     = 1 << 1  // DM_SUSPEND_FLAG
	fPersistentDev = 1 << 3  // DM_PERSISTENT_DEV_FLAG
	fStatusTable   = 1 << 4  // DM_STATUS_TABLE_FLAG - ask for the table itself
	fActiveOut     = 1 << 5  // out
	fInactiveOut   = 1 << 6  // out
	fBufferFull    = 1 << 8  // out
	fSkipBdget     = 1 << 9
	fSkipLockfs    = 1 << 10
	fNoFlush       = 1 << 11
	fQueryInactive = 1 << 12
	fUEventOut     = 1 << 13
	fUUIDIn        = 1 << 14
	fSecureData    = 1 << 15

	dmNameLen      = 128
	dmUuidLen      = 129
	dmMaxTypeName  = 16 // DM_MAX_TYPE_NAME in v4.14 (it is 32 only in newer kernels)
	dmIfaceMajor   = 4
	// sizeof(struct dm_target_spec) = u64 start + u64 len + s32 status +
	// u32 next + char target_type[16] = 40. Getting this wrong is silent and
	// fatal: the params string then lands 16 bytes past where the kernel looks
	// for it, dm-linear sees an empty argument list and DM_TABLE_LOAD answers
	// EINVAL - while a target with no params ("error") works fine, which is
	// exactly the confusing signature we hit. Confirmed empirically: the kernel
	// writes a 40+8=48 byte record for "linear 0 1048576 7:216 0".
	targetSpecSize = 40
)

// dmHdr mirrors struct dm_ioctl as of linux 4.14. Note `data` is an INLINE char
// array here (offsetof=305, sizeof=312), not the pointer it became later, and
// event_nr / padding / dev sit between flags and name.
type dmHdr struct {
	Version     [3]uint32
	DataSize    uint32
	DataStart   uint32
	TargetCount uint32
	OpenCount   int32
	Flags       uint32
	EventNr     uint32
	Padding     uint32
	Dev         uint64
	Name        [dmNameLen]byte
	UUID        [dmUuidLen]byte
	Data        [7]byte
}

const dataFloor = unsafe.Offsetof(dmHdr{}.Data) // 305 on arm64

var hdrSize = int(unsafe.Sizeof(dmHdr{})) // 312 on arm64

// payloadStart: where the first dm_target_spec goes - >= dataFloor and 8-aligned.
const payloadStart = 312

// ioctlSize feeds only the command's size bits; 4.14 ignores them (see above).
var ioctlSize = hdrSize

func iowr(nr int) uintptr {
	const iocRead, iocWrite = 2, 1
	return uintptr(iocRead|iocWrite)<<30 |
		uintptr(ioctlSize&0x3FFF)<<16 |
		uintptr(dmIoctl)<<8 |
		uintptr(nr)
}

type dmDev struct{ f *os.File }

func openCtrl() (*dmDev, error) {
	f, err := os.OpenFile("/dev/block/dm-ctrl", os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open /dev/block/dm-ctrl: %w", err)
	}
	return &dmDev{f: f}, nil
}

func (d *dmDev) Close() { d.f.Close() }

// rawIoctl issues one ioctl. Go's syscall.Syscall returns (r1, r2, errno) and
// errno is the THIRD value; reading r2 by mistake produces nonsense such as
// "errno 3225484544", which is really a returned value.
func rawIoctl(fd, cmd, arg uintptr) error {
	r1, _, errno := syscall.Syscall(syscall.SYS_IOCTL, fd, cmd, arg)
	if errno != 0 {
		return syscall.Errno(errno)
	}
	if r1 >= 0xFFFFFFFFFFFFF001 && r1 <= 0xFFFFFFFFFFFFFFFE { // -1..-4095
		return syscall.Errno(-int64(r1))
	}
	return nil
}

// newBuf builds a whole ioctl buffer: header at 0, optional payload at
// payloadStart. data_size is the TOTAL size (that is what the kernel allocates
// and copies), so an empty call still reports 312, never 0.
func newBuf(name string, flags uint32, payload []byte) []byte {
	n := payloadStart
	if len(payload) > 0 {
		n = payloadStart + len(payload)
	}
	buf := make([]byte, n)
	h := (*dmHdr)(unsafe.Pointer(&buf[0]))
	h.Version[0] = dmIfaceMajor
	h.DataSize = uint32(n)
	h.DataStart = payloadStart
	h.Flags = flags
	if name != "" {
		copy(h.Name[:], name)
	}
	if len(payload) > 0 {
		copy(buf[payloadStart:], payload)
	}
	return buf
}

func hdr(buf []byte) *dmHdr { return (*dmHdr)(unsafe.Pointer(&buf[0])) }

// do issues the ioctl and annotates failures with the command name.
func (d *dmDev) do(nr int, buf []byte, name string) error {
	if err := rawIoctl(d.f.Fd(), iowr(nr), uintptr(unsafe.Pointer(&buf[0]))); err != nil {
		return fmt.Errorf("%s on '%s': %w", ioctlName(nr), name, err)
	}
	return nil
}

// version asks the kernel which dm interface it speaks. Also proves the ioctl
// encoding and that we can open the control node at all.
func (d *dmDev) version() (string, error) {
	buf := newBuf("", 0, nil)
	if err := d.do(dmVersion, buf, ""); err != nil {
		return "", err
	}
	h := hdr(buf)
	return fmt.Sprintf("dm interface %d.%d.%d (our struct: sizeof=%d offsetof(data)=%d, ioctl type 0x%02x)",
		h.Version[0], h.Version[1], h.Version[2], hdrSize, dataFloor, dmIoctl), nil
}

// list returns the names of all dm devices as the kernel reports them - the
// cheapest possible proof that our header layout is right.
func (d *dmDev) list() ([]string, error) {
	buf := newBuf("", 0, make([]byte, 1<<16))
	if err := d.do(dmListDevices, buf, ""); err != nil {
		return nil, err
	}
	h := hdr(buf)
	start := int(h.DataStart)
	end := int(h.DataSize) // on output data_size = data_start + used
	if end > len(buf) {
		end = len(buf)
	}
	var out []string
	off := start
	// struct dm_name_list { u64 dev; u32 next; char name[0]; }
	for off+12 <= end {
		next := int(binary.LittleEndian.Uint32(buf[off+8:]))
		p := off + 12
		q := p
		for q < end && buf[q] != 0 {
			q++
		}
		out = append(out, string(buf[p:q]))
		if next <= 0 {
			break
		}
		off += next
	}
	return out, nil
}

// targetSpec is one table line.
type targetSpec struct {
	Start  uint64
	Len    uint64
	Type   string
	Params string
}

// packTargets writes the dm_target_spec stream for DM_TABLE_LOAD.
// `next` is relative to the START OF THE CURRENT RECORD (4.14 semantics), so the
// last record carries 0.
func packTargets(ts []targetSpec) []byte {
	size := func(t targetSpec) int {
		n := targetSpecSize + len(t.Params) + 1
		if n%8 != 0 {
			n += 8 - n%8
		}
		return n
	}
	var total int
	for _, t := range ts {
		total += size(t)
	}
	out := make([]byte, total)
	off := 0
	for i, t := range ts {
		binary.LittleEndian.PutUint64(out[off:], t.Start)
		binary.LittleEndian.PutUint64(out[off+8:], t.Len)
		binary.LittleEndian.PutUint32(out[off+16:], 0) // status: output only
		next := 0
		if i != len(ts)-1 {
			next = size(t)
		}
		binary.LittleEndian.PutUint32(out[off+20:], uint32(next))
		copy(out[off+24:off+24+dmMaxTypeName], t.Type)
		p := off + targetSpecSize
		copy(out[p:], t.Params)
		out[p+len(t.Params)] = 0
		off += size(t)
	}
	return out
}

// loadTable installs the inactive table. This is half of the live flip.
// No "name"/"inactive" flags exist in the uapi: the device is found by name
// always, and TABLE_LOAD always writes the inactive table.
func (d *dmDev) loadTable(name string, ts []targetSpec, readOnly bool) error {
	payload := packTargets(ts)
	flags := uint32(0)
	if readOnly {
		flags |= fReadOnlyIn
	}
	buf := newBuf(name, flags, payload)
	hdr(buf).TargetCount = uint32(len(ts))
	return d.do(dmTableLoad, buf, name)
}

// loadTableBuf is loadTable with a pre-packed payload (used by the diagnostic).
func (d *dmDev) loadTableBuf(name string, payload []byte, readOnly bool) error {
	flags := uint32(0)
	if readOnly {
		flags |= fReadOnlyIn
	}
	buf := newBuf(name, flags, payload)
	n := 0
	// Count the records in the payload the same way the kernel walks them.
	for off := 0; off+targetSpecSize <= len(payload); {
		next := int(binary.LittleEndian.Uint32(payload[off+20:]))
		n++
		if next <= 0 {
			break
		}
		off += next
	}
	hdr(buf).TargetCount = uint32(n)
	return d.do(dmTableLoad, buf, name)
}

// suspend freezes I/O on the device (DM_SUSPEND_FLAG set).
func (d *dmDev) suspend(name string) error {
	return d.do(dmDevSuspend, newBuf(name, fSuspendIn, nil), name)
}

// resume swaps the inactive table in and un-freezes (DM_SUSPEND_FLAG clear).
// THIS is the moment the bytes the host reads change.
func (d *dmDev) resume(name string) error {
	return d.do(dmDevSuspend, newBuf(name, 0, nil), name)
}

func (d *dmDev) create(name string, readOnly bool) error {
	flags := uint32(0)
	if readOnly {
		flags |= fReadOnlyIn
	}
	return d.do(dmDevCreate, newBuf(name, flags, nil), name)
}

func (d *dmDev) remove(name string) error {
	return d.do(dmDevRemove, newBuf(name, 0, nil), name)
}

// status reads back the LIVE table - the only trustworthy way to prove a reload
// actually took effect inside the kernel. DM_STATUS_TABLE_FLAG is what asks for
// the table rather than just the device's status words.
func (d *dmDev) status(name string) (string, error) {
	buf := newBuf(name, fStatusTable, make([]byte, 1<<17))
	if err := d.do(dmTableStatus, buf, name); err != nil {
		return "", err
	}
	h := hdr(buf)
	start := int(h.DataStart)
	end := int(h.DataSize)
	if end > len(buf) {
		end = len(buf)
	}
	var lines []string
	off := start
	for off+targetSpecSize <= end {
		sectStart := binary.LittleEndian.Uint64(buf[off:])
		ln := binary.LittleEndian.Uint64(buf[off+8:])
		st := int32(binary.LittleEndian.Uint32(buf[off+16:]))
		next := int(binary.LittleEndian.Uint32(buf[off+20:]))
		typ := cstr(buf[off+24 : off+24+dmMaxTypeName])
		p := off + targetSpecSize
		q := p
		for q < end && buf[q] != 0 {
			q++
		}
		params := string(buf[p:q])
		line := fmt.Sprintf("  %10d +%d  %s %s", sectStart, ln, typ, params)
		if st != 0 {
			line += fmt.Sprintf("   [status=%d]", st)
		}
		lines = append(lines, line)
		if next <= 0 {
			break
		}
		// DM_TABLE_STATUS is the odd one out: retrieve_status sets
		// spec->next = outptr - outbuf, i.e. an offset from the FIRST record
		// (dm-ioctl.c:1173), while DM_LIST_DEVICES/DM_LIST_VERSIONS use
		// record-to-record offsets.
		noff := start + next
		if noff <= off {
			break
		}
		off = noff
	}
	out := fmt.Sprintf("targets=%d open=%d\n", h.TargetCount, h.OpenCount)
	if len(lines) == 0 {
		out += fmt.Sprintf("  (no records parsed: data_start=%d data_size=%d buflen=%d)\n", start, end, len(buf))
	}
	for _, l := range lines {
		out += l + "\n"
	}
	return out, nil
}

// targetTypes lists the target types this kernel has registered, via
// DM_LIST_VERSIONS. Decisive when a TABLE_LOAD fails with EINVAL and you need to
// know whether the target type exists at all (dm-linear can be a module, and on
// this phone /proc/modules shows none loaded).
//
// struct dm_target_versions { u32 next; u32 version[3]; char name[0]; }
// with `next` = bytes from the start of this record to the next one.
func (d *dmDev) targetTypes() ([]string, error) {
	buf := newBuf("", 0, make([]byte, 1<<16))
	if err := d.do(dmListVersions, buf, ""); err != nil {
		return nil, err
	}
	h := hdr(buf)
	off := int(h.DataStart)
	end := int(h.DataSize)
	if end > len(buf) {
		end = len(buf)
	}
	var out []string
	for off+16 <= end {
		next := int(binary.LittleEndian.Uint32(buf[off:]))
		v0 := binary.LittleEndian.Uint32(buf[off+4:])
		v1 := binary.LittleEndian.Uint32(buf[off+8:])
		v2 := binary.LittleEndian.Uint32(buf[off+12:])
		p := off + 16
		q := p
		for q < end && buf[q] != 0 {
			q++
		}
		out = append(out, fmt.Sprintf("%s %d.%d.%d", string(buf[p:q]), v0, v1, v2))
		if next <= 0 {
			break
		}
		off += next
	}
	return out, nil
}

// statusRaw returns the kernel's raw payload bytes for DM_TABLE_STATUS - used to
// compare against what we send, byte for byte.
func (d *dmDev) statusRaw(name string) ([]byte, error) {
	buf := newBuf(name, fStatusTable, make([]byte, 1<<17))
	if err := d.do(dmTableStatus, buf, name); err != nil {
		return nil, err
	}
	h := hdr(buf)
	start := int(h.DataStart)
	end := int(h.DataSize)
	if end > len(buf) {
		end = len(buf)
	}
	if start > end {
		start = end
	}
	return buf[start:end], nil
}

// segments returns how many target records the LIVE table has, plus a compact
// description, so a flip can be verified instead of assumed.
func (d *dmDev) segments(name string) (int, string, error) {
	raw, err := d.statusRaw(name)
	if err != nil {
		return 0, "", err
	}
	n := 0
	var desc []string
	for off := 0; off+targetSpecSize <= len(raw); {
		sect := binary.LittleEndian.Uint64(raw[off:])
		ln := binary.LittleEndian.Uint64(raw[off+8:])
		next := int(binary.LittleEndian.Uint32(raw[off+20:]))
		typ := cstr(raw[off+24 : off+24+dmMaxTypeName])
		p := off + targetSpecSize
		q := p
		for q < len(raw) && raw[q] != 0 {
			q++
		}
		desc = append(desc, fmt.Sprintf("%d+%d %s %s", sect, ln, typ, string(raw[p:q])))
		n++
		if next <= 0 {
			break
		}
		off = next // DM_TABLE_STATUS: next is relative to the FIRST record
	}
	return n, strings.Join(desc, " | "), nil
}

func cstr(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}

func ioctlName(nr int) string {
	switch nr {
	case dmVersion:
		return "DM_VERSION"
	case dmListDevices:
		return "DM_LIST_DEVICES"
	case dmDevCreate:
		return "DM_DEV_CREATE"
	case dmDevRemove:
		return "DM_DEV_REMOVE"
	case dmDevSuspend:
		return "DM_DEV_SUSPEND"
	case dmTableLoad:
		return "DM_TABLE_LOAD"
	case dmTableStatus:
		return "DM_TABLE_STATUS"
	case dmTableDeps:
		return "DM_TABLE_DEPS"
	}
	return fmt.Sprintf("DM_IOCTL(%d)", nr)
}
