# Crash-safe journal compaction design

ABEX does not truncate the active journal in place. An in-place rewrite can destroy both the old
and new recovery point if the host loses power between truncation, write, and metadata persistence.
Compaction therefore uses immutable checkpoints, append-only segments, and an atomically replaced
manifest.

## Recovery layout

```text
state/
  orders.manifest                 active checkpoint and segment set
  orders.checkpoint.<sequence>    latest Order for every clientOrderId
  orders.segment.<first>.<last>   records after the checkpoint barrier
  orders.active                   current O_APPEND segment
  archive/                        optional immutable audit retention
```

Every checkpoint and segment retains the existing schema version, record sequence, payload
checksum, and strict mid-file corruption policy. Record sequences never restart after compaction.

## Compaction transaction

1. Acquire the gateway persistence-order gate. Stop accepting new persistent mutations, flush both
   venue execution lanes, then flush the operational-event writer.
2. Capture barrier sequence `N` and the latest complete `Order` snapshot for every client order.
3. Write `orders.checkpoint.N.tmp` with a header, the snapshots, the required operational-event
   retention window, and a footer containing record count and whole-file checksum.
4. `fdatasync()` the temporary checkpoint, rename it to `orders.checkpoint.N`, then `fsync()` the
   containing directory.
5. Rotate `orders.active` to an immutable segment and open a new `O_APPEND` active file while the
   process still owns the exclusive journal lock.
6. Write `orders.manifest.tmp` naming the checkpoint, retained segments, next record sequence, and
   active segment. `fdatasync()` it, atomically rename it to `orders.manifest`, and `fsync()` the
   directory again.
7. Release the persistence gate. Old files are now unreachable from the committed manifest and may
   be moved to `archive/`; delete them only after the configured audit-retention period.

The maximum stop-the-world portion can later be reduced with copy-on-write snapshots, but the
initial implementation should favor an auditable barrier over cleverness.

## Startup and crash rules

- A `.tmp` file is never a recovery source and may be removed after startup validation.
- If no valid manifest exists, recovery falls back to the current single JSONL journal.
- If the manifest is valid, recovery loads the checkpoint and then only segments whose sequence is
  greater than `N`, rejecting gaps, overlap, or a sequence regression.
- A torn final record in the active segment is repaired exactly as today. Corruption in a checkpoint
  or immutable segment is fatal.
- The old manifest and files remain a valid recovery set until the new manifest rename and directory
  sync both complete.

## Retention policy

Compaction is not audit deletion. Order snapshots are retained indefinitely unless business policy
says otherwise. Operational events may use a time/count window in the online checkpoint, while old
immutable segments are compressed and archived for the regulatory retention period. The UI's
bounded in-memory caches are independent of durable retention.

## Required fault-injection tests before enabling automatic compaction

Test process termination after each numbered transaction step, manifest checksum failure, checkpoint
checksum failure, missing/overlapping segments, a torn active append, disk-full behavior, restart
during archive movement, and concurrent read APIs during the barrier. Automatic compaction must stay
disabled until every crash point recovers either the complete old set or the complete new set.
