# wayfire-ext-workspace-manager-v1

Out-of-tree Wayfire plugin implementing the `ext_workspace_manager_v1`
Wayland protocol from `wayland-protocols`.

## Build

```sh
meson setup -Dprefix=/usr build
meson compile -C build
sudo meson install -C build
```

Enable the plugin by adding `ext-workspace-manager` to the Wayfire
`core/plugins` list.
