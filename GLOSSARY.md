# vbd

A block-device server for development and test machines: a disk forked from
a shared image, fetched lazily, with every write kept so the disk can be
reopened as it was at an earlier moment.

## Language

### The disk

**Volume**:
A virtual disk of fixed size, the thing a user creates, reads, writes and
serves as a block device.
_Avoid_: disk image, device (the device is what a served volume appears as)

**Chunk**:
One of the equal pieces a volume is divided into, numbered from 0; the unit
in which content is stored and fetched.
_Avoid_: block, page, sector

**Base**:
The immutable image a volume falls back to for chunks it was never written
with. A volume without one falls back to zeros.
_Avoid_: backing file, parent, golden image

**Manifest**:
The checksum of every chunk of a base, made from a trusted copy of it and
given to a volume when it is created, against which each chunk that later
arrives from the base is checked.
_Avoid_: sums, checksum file, hash list, signature

**Fetched**:
The number of distinct base chunks a volume holds in its base cache.
_Avoid_: downloaded, cached count

### Storage

**Log**:
An append-only sequence of records together with the map that indexes it.
A volume has a data log, and a base cache if it has a base.
_Avoid_: journal, record (a record is one entry of a log)

**Data log**:
The log holding everything a volume was written with.
_Avoid_: the log (when the base cache could be meant too)

**Base cache**:
The log holding the base chunks fetched so far. It can always be rebuilt
from the base.
_Avoid_: cache log, local copy

**Record**:
One entry of a log, always the same size: a header and one chunk's worth of
data.
_Avoid_: entry, block, log

**Map**:
The index of a log: for each chunk, the newest record that holds it.
_Avoid_: index, table, metadata

**Super block**:
The description of the volume stored in front of a log's records: its
size, its chunk size and its base.
_Avoid_: header (a header belongs to a record)

**Checkpoint**:
A copy of a log's map stored in that log, covering every record before it,
so the map need not be rebuilt from those records.
_Avoid_: index file, metadata, snapshot

### Time

**Epoch**:
The number of snapshots a volume had when a record was written.
_Avoid_: generation, version

**Snapshot**:
A moment in a volume's history, numbered from 0, whose content stays
readable however the volume changes afterwards.
_Avoid_: checkpoint, backup

**Snapshot view**:
A volume opened read-only as it was at one snapshot.
_Avoid_: read-only open, view
