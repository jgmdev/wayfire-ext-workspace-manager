# Agent Notes

This repository is an out-of-tree Wayfire plugin implementing the
`ext_workspace_manager_v1` Wayland protocol.

## Project Shape

- Top-level `meson.build` uses:
  - `dependency('wayfire', version: '>=0.11.0')`
  - `dependency('wayland-server')`
  - `dependency('wayland-protocols')`
  - `-DWLR_USE_UNSTABLE`
  - `-DWAYFIRE_PLUGIN`
- `protocol/meson.build` generates server-side protocol code from:
  - `wayland-protocols/staging/ext-workspace/ext-workspace-v1.xml`
- `src/meson.build` builds:
  - `libext-workspace-manager.so`
  - installed to `join_paths(get_option('libdir'), 'wayfire')`
- `metadata/meson.build` installs:
  - `metadata/ext-workspace-manager.xml`
  - installed to `wayfire.get_variable(pkgconfig: 'metadatadir')`

## Build Commands

```sh
meson setup build
meson compile -C build
meson install -C build --dry-run
```

Enable the plugin by adding `ext-workspace-manager` to Wayfire's
`core/plugins` list.

## Commit Preparation

- Use a descriptive commit subject no longer than 50 characters.
- Wrap commit message body lines at 72 characters.
- Prepare commit messages in a temporary file and pass it with `git commit -F`;
  do not use nested `git commit -m` flags.
- Include the relevant verification commands and results in the body when a
  change has been tested.

## Wayfire API

The wayfire API can be inspected on ../wayfire if not it can be cloned from
https://github.com/WayfireWM/wayfire or by using the system wide installed
headers.

## Protocol Model

Wayfire's native workspace model is a fixed 2D grid per `wf::workspace_set_t`.
This plugin maps it to `ext-workspace-v1` as follows:

- one `ext_workspace_group_handle_v1` per active `wf::workspace_set_t`
- one `ext_workspace_handle_v1` per valid `(x, y)` coordinate in that workspace set
- workspace group output membership comes from outputs whose current `wset()` is the group
- workspace activation maps to `workspace_set_t::request_workspace({x, y})`

Workspace IDs are intentionally simple:

- protocol id: `wset-<workspace-set-index>:<x>,<y>`
- coordinates: Wayland array containing two `uint32_t` values, `[x, y]`

Workspace names come from `ext-workspace-manager/names`, a comma-separated list
in 1-based linear grid order. Empty or missing entries use the generated
1-based linear index, `y * grid_width + x + 1`. Workspaces created through the
protocol use the requested name if the configured list does not override that
slot.

`activate` is advertised on all workspaces. `remove` is advertised only for
workspaces on a removable rightmost column or bottom row. Group capabilities
advertise `create_workspace`. Unsupported client requests (`deactivate`,
`assign`) are ignored.

Dynamic creation/removal is grid-based:

- `create_workspace` grows the target workspace set by one column.
- The requested name is stored for the first new workspace in that column when
  the configured name list does not override it.
- `remove` shrinks a rightmost column or bottom row; non-edge removal requests
  are ignored.

## Implementation Notes

- Main implementation file: `src/ext-workspace-manager.cpp`.
- The plugin class is `ext_workspace_manager_plugin_t` and is exported with
  `DECLARE_WAYFIRE_PLUGIN`.
- The plugin creates one `wl_global` for `ext_workspace_manager_v1`.
- It is marked non-unloadable because it exposes Wayland globals, matching the
  pattern of Wayfire's in-tree protocol plugins.
- Per-client protocol objects are separate from Wayfire model objects:
  - `manager_client_t`
  - `group_handle_t`
  - `workspace_handle_t`
- Removed protocol handles are retained in retired vectors until clients destroy
  them, so post-`removed` `destroy` requests still have valid user data.
- `commit` applies pending removals, creations, and the last pending activation
  per workspace set, then broadcasts a sync to clients.
- `stop` sends `finished` and destroys the manager resource.

## Wayfire Signals Used

The plugin listens to output-layout and output/workspace changes, then sends a
fresh protocol sync:

- `wf::output_added_signal`
- `wf::output_removed_signal`
- `wf::workspace_changed_signal`
- `wf::workspace_set_changed_signal`
- `wf::workspace_grid_changed_signal`

On grid changes, new workspace handles are created and no-longer-valid handles
receive `workspace_leave` followed by `removed`.

## Important API Details

- Wayland generated request implementation structs need the `struct` tag in C++:
  - `static const struct ext_workspace_manager_v1_interface ...`
  - `static const struct ext_workspace_group_handle_v1_interface ...`
  - `static const struct ext_workspace_handle_v1_interface ...`
- Do not name helper methods `wl_client`; GCC 16 treats that as changing the
  meaning of the `struct wl_client` type.
- `output_enter` and `output_leave` need the client-specific `wl_output`
  resource. The plugin currently finds it by iterating `output->handle->resources`.
- Wayfire's `workspace_changed_signal` is emitted on outputs, while
  `workspace_grid_changed_signal` is emitted on workspace sets.

## Verified State

The project was verified with:

```sh
meson setup build --reconfigure
meson compile -C build
meson install -C build --dry-run
```

Expected dry-run install paths:

- `/usr/local/lib/wayfire/libext-workspace-manager.so`
- `/usr/share/wayfire/metadata/ext-workspace-manager.xml`

## Known Limitations

- No workspace reassignment is implemented.
- No urgency state is implemented.
