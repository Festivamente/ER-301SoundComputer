# Upstream ER-301 relationship

The `er-301-native/` directory is derived from Brian Clarkson / Orthogonal Devices'
ER-301 repository:

https://github.com/odevices/er-301

It retains the upstream MIT license in `er-301-native/LICENSE.md`.
Compatibility changes made in this tree for the VCV port are distributed under
that same MIT License.

## Maintenance policy

Treat this directory as upstream code, not as a general cleanup target. Avoid cosmetic
reformatting, comment cleanup, renaming, or unrelated modernization. The goal is to keep
the VCV port's changes minimal and attributable so future upstream comparison remains
practical.

The VCV port currently requires upstream-tree changes in a few functional categories:

- a desktop host build target and target/platform selection;
- Windows HAL/build support alongside Linux and Darwin;
- host-controlled runtime paths and lifecycle hooks;
- isolated native-package loading required for multiple Rack instances;
- host file-dialog and text-entry bridges;
- small ER-301 Lua hooks that route file/text requests through those host bridges.

The packaged Lua runtime under `res/er301/xroot/` mirrors the upstream-derived
`xroot/` directory with one deliberate host-only exception: its
`Package/Manager.lua` contains the one-time bootstrap for the bundled Core
package. The upstream-derived copy remains untouched.

When updating from Orthogonal Devices upstream, merge upstream first, then reapply or
reconcile only the integration changes required by the VCV host. Review the packaged
`Package/Manager.lua` bootstrap whenever the upstream package manager changes.
