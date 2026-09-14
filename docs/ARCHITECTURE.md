# VCV host architecture

## Engine boundary

The repository root is the Rack-facing host. `er-301-native/` is the upstream-derived
ER-301 engine/runtime. The host links the complete ER-301 engine image and communicates
with it through the bridge in `src/er301_bridge.*`.

## Multiple instances

The ER-301 firmware was designed around process-global state. To preserve that code
without a firmware-wide singleton rewrite, additional Rack modules load private copies
of the engine image. Each instance therefore receives isolated emulator, Lua, task,
display, heap, and session state. Shared package archives/installation state live on
the common virtual front card.

## Runtime resources

`res/er301/xroot/` is the packaged ER-301 runtime. It mirrors
`er-301-native/xroot/` except for one deliberate VCV-only change in
`Package/Manager.lua`: the bundled Core package is installed once on a fresh
virtual front card.

Keep the packaged runtime synchronized with:

```sh
make runtime-check
make runtime-sync
```

`runtime-sync` refreshes the upstream-derived runtime while preserving that
VCV-specific package bootstrap.

## Desktop bridges

The ER-301 runtime can request host-native file selection and Rack-hosted text entry.
The requests cross the engine boundary asynchronously so operating-system/UI work stays
on the Rack UI thread. Returned values are fed back into the original ER-301 chooser
and keyboard paths, preserving the ER-301's validation and commit behavior.

## Audio

The embedded engine runs natively at 48 kHz or 96 kHz. A fixed-storage host adapter
converts between Rack's sample rate and the selected ER-301 rate when necessary. Audio
processing remains allocation-free in the Rack process callback.
