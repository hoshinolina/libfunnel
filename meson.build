project('funnel-test', 'c')

native = get_option('native')

includes = include_directories('include')

pipewire = dependency('libpipewire-0.3', native: native)
gbm = dependency('gbm', native: native)
drm = dependency('libdrm', native: native)
egl = dependency('egl', native: native)
vulkan = dependency('vulkan', native: native)

compiler = meson.get_compiler('c', native: native)
has_linux_headers = compiler.has_header('linux/dma-buf.h')

if not has_linux_headers
    error('Missing linux-headers package')
endif

lib_funnel = both_libraries('funnel', 'src/funnel.c',
    dependencies: [gbm, drm, pipewire],
    include_directories : includes,
    pic: true,
    native: native)

funnel = declare_dependency(
    link_with: lib_funnel.get_shared_lib(),
    include_directories: includes
)
funnel_static = declare_dependency(
    link_with: lib_funnel.get_static_lib(),
    include_directories : includes
)

lib_funnel_egl = both_libraries('funnel-egl', 'src/egl.c',
    dependencies: [funnel, pipewire, egl, gbm],
    pic: true,
    native: native
)
lib_funnel_vk = both_libraries('funnel-vk', 'src/vulkan.c',
    dependencies: [funnel, pipewire, vulkan, gbm, drm],
    pic: true,
    native: native
)

funnel_egl = declare_dependency(
    link_with: lib_funnel_egl.get_shared_lib(),
    include_directories: includes)
funnel_egl_static = declare_dependency(
    link_with: lib_funnel_egl.get_static_lib(),
    include_directories: includes)

funnel_vk = declare_dependency(
    link_with: lib_funnel_vk.get_shared_lib(),
    include_directories: includes)
funnel_vk_static = declare_dependency(
    link_with: lib_funnel_vk.get_static_lib(),
    include_directories: includes)

if get_option('test_apps')
    if native
        error('test-apps is mutually exclusive with native')
    endif

    gl = dependency('GL', native: native)
    x11 = dependency('x11', native: native)

    executable('funnel-test-egl', 'test-egl.c',
        dependencies: [gl, egl, x11, funnel, funnel_egl],
    )

    wayland = dependency('wayland-client', native: native)
    wl_mod = import('unstable-wayland')

    xml = wl_mod.find_protocol('xdg-shell')
    xdg_shell = wl_mod.scan_xml(xml)

    xml = wl_mod.find_protocol(
    'xdg-decoration',
    state : 'unstable',
    version : 1,
    )
    xdg_decoration = wl_mod.scan_xml(xml)

    glsl_compiler = find_program('glslang', 'glslangValidator')
    glsl_args = [
    '--quiet',
    '--target-env', 'vulkan1.0',
    '--vn', '@BASENAME@',
    '--depfile', '@DEPFILE@',
    '@INPUT@',
    '-o', '@OUTPUT@',
    ]
    glsl_generator = generator(
    glsl_compiler,
    output    : [ '@BASENAME@.h' ],
    depfile   : '@BASENAME@.h.d',
    arguments : glsl_args,
    )

    demo_shaders = files([
    'shaders/triangle_frag.frag',
    'shaders/triangle_vert.vert',
    ])

    executable('funnel-test-vk', 'test-vk.c', xdg_shell, xdg_decoration,
        glsl_generator.process(demo_shaders),
        dependencies: [wayland, funnel, funnel_vk, vulkan],
        native: native)
endif
