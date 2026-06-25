wl_protocol_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner = find_program('wayland-scanner', native: true)

wayland_scanner_server = generator(
    wayland_scanner,
    output: '@BASENAME@-protocol.h',
    arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_code = generator(
    wayland_scanner,
    output: '@BASENAME@-protocol.c',
    arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

ext_workspace_xml = join_paths(wl_protocol_dir, 'staging/ext-workspace/ext-workspace-v1.xml')

ext_workspace_protocol_src = [wayland_scanner_code.process(ext_workspace_xml)]
ext_workspace_protocol_headers = [wayland_scanner_server.process(ext_workspace_xml)]

lib_ext_workspace_protocol = static_library(
    'ext_workspace_protocol',
    ext_workspace_protocol_src + ext_workspace_protocol_headers,
    dependencies: [wayland_server],
)

ext_workspace_protocol = declare_dependency(
    link_with: lib_ext_workspace_protocol,
    sources: ext_workspace_protocol_headers,
)
