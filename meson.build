project('funnel-test', 'c',
    meson_version: '>=1.1',
    version: '0.1.0',
)

pkg = import('pkgconfig')

native = get_option('native')
static_pipewire = get_option('static_pipewire')

includes = include_directories('include')
if static_pipewire
    pipewire = dependency('libpipewire-static-0.3', native: native, static: true)
else
    pipewire = dependency('libpipewire-0.3', native: native)
endif
gbm = dependency('gbm', native: native)
drm = dependency('libdrm', native: native)

compiler = meson.get_compiler('c', native: native)
has_linux_headers = compiler.has_header('linux/dma-buf.h')

if not has_linux_headers
    error('Missing linux-headers package')
endif

lib_funnel = library('funnel', 'src/funnel.c',
    dependencies: [gbm, drm, pipewire],
    include_directories : includes,
    pic: true,
    native: native,
    install: not native)

funnel = declare_dependency(
    link_with: lib_funnel,
    include_directories: includes
)

if not native
    install_headers(
        'include/funnel.h',
        'include/funnel-gbm.h',
        subdir : 'funnel')

    pkg.generate(libraries : lib_funnel,
                subdirs : 'funnel',
                version : meson.project_version(),
                name : 'libfunnel',
                filebase : 'libfunnel',
                description : 'PipeWire video sharing made easy')
endif

if get_option('egl')
    egl = dependency('egl', native: native)
    deps = [funnel, pipewire, egl, gbm]

    lib_funnel_egl = library('funnel-egl', 'src/egl.c',
        dependencies: deps,
        pic: true,
        native: native,
        install: not native,
    )
    funnel_egl = declare_dependency(
        link_with: lib_funnel_egl,
        include_directories: includes)

    if not native
        install_headers('include/funnel-egl.h', subdir : 'funnel')

        pkg.generate(libraries : lib_funnel_egl,
                     subdirs : 'funnel',
                     version : meson.project_version(),
                     name : 'libfunnel-egl',
                     filebase : 'libfunnel-egl',
                     description : 'EGL API for libfunnel')
    endif
endif

if get_option('vulkan')
    vulkan = dependency('vulkan', native: native)
    deps = [funnel, pipewire, vulkan, gbm, drm]

    lib_funnel_vk = library('funnel-vk', 'src/vulkan.c',
        dependencies: [funnel, pipewire, vulkan, gbm, drm],
        pic: true,
        native: native,
        install: not native
    )
    funnel_vk = declare_dependency(
        link_with: lib_funnel_vk,
        include_directories: includes)

    if not native
        install_headers('include/funnel-vk.h', subdir : 'funnel')

        pkg.generate(libraries : lib_funnel_vk,
                     subdirs : 'funnel',
                     version : meson.project_version(),
                     name : 'libfunnel-vk',
                     filebase : 'libfunnel-vk',
                     description : 'Vulkan API for libfunnel')
    endif
endif

if get_option('test_apps')
    if native
        error('test_apps is mutually exclusive with native')
    endif

    if get_option('egl')
        gl = dependency('GL', native: native)
        x11 = dependency('x11', native: native)

        executable('funnel-test-egl', 'test-egl.c',
            dependencies: [gl, egl, x11, funnel, funnel_egl],
            install: true,
        )
    endif

    if get_option('vulkan')
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
            install: true,
            native: native)
    endif
endif
