#include <funnel-egl.h>
#include <funnel.h>

#include <GLES2/gl2.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

static void initialize_egl(Display *x11_display, Window x11_window,
                           EGLDisplay *egl_display, EGLContext *egl_context,
                           EGLSurface *egl_surface) {

    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress(
            "eglQueryDeviceStringEXT");
    PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT =
        (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress(
            "eglQueryDisplayAttribEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress(
            "eglQueryDmaBufModifiersEXT");
    assert(eglQueryDeviceStringEXT);
    assert(eglQueryDisplayAttribEXT);
    assert(eglQueryDmaBufModifiersEXT);

    // Set OpenGL rendering API
    eglBindAPI(EGL_OPENGL_API);

    // get an EGL display connection
    EGLDisplay display = eglGetDisplay(x11_display);

    // initialize the EGL display connection
    eglInitialize(display, NULL, NULL);

    // get an appropriate EGL frame buffer configuration
    EGLConfig config;
    EGLint num_config;
    EGLint const attribute_list_config[] = {EGL_RED_SIZE,  8, EGL_GREEN_SIZE, 8,
                                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_NONE};
    eglChooseConfig(display, attribute_list_config, &config, 1, &num_config);

    // create an EGL rendering context
    EGLint const attrib_list[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                                  EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    EGLContext context =
        eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);

    // create an EGL window surface
    EGLSurface surface =
        eglCreateWindowSurface(display, config, x11_window, NULL);

    // connect the context to the surface
    eglMakeCurrent(display, surface, surface, context);

    // Return
    *egl_display = display;
    *egl_context = context;
    *egl_surface = surface;
}

int u_frame;

void gl_setup_scene(void) {
    // Shader source that draws a textures quad
    const char *vertex_shader_source =
        "#version 330 core\n"
        "uniform float frame;\n"
        "layout (location = 0) in vec3 aPos;\n"

        "void main()\n"
        "{\n"
        "   float a = frame * 3.141592 / 4.;"
        "   mat4 rot = mat4(cos(a), -sin(a), 0., 0.,\n"
        "                   sin(a),  cos(a), 0., 0., \n"
        "                       0.,      0., 1., 0., \n"
        "                       0.,      0., 0., 1.);\n"
        "   vec4 pos = vec4(aPos.x, aPos.y, aPos.z, 1.0);\n"
        "   pos = vec4(0.1,0.1,0.1,1.0) * pos;\n"
        "   pos += vec4(0.5,0.5,0.0,0.0);\n"
        "   gl_Position = rot * pos;\n"
        "}";

    const char *fragment_shader_source =
        "#version 330 core\n"
        "out vec4 FragColor;\n"

        "void main()\n"
        "{\n"
        "   FragColor = vec4(1., 0., 0., 1.);\n"
        "}";

    // vertex shader
    int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);
    // fragment shader
    int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);
    // link shaders
    int shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);
    // delete shaders
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    u_frame = glGetUniformLocation(shader_program, "frame");

    // quad
    float vertices[] = {
        0.f, 1.f, 0.0f, 1.f, -1.f, 0.0f, -1.f, -1.f, 0.0f,
    };
    unsigned int indices[] = {
        0, 1, 2, // first Triangle
    };

    unsigned int VBO, VAO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindVertexArray(0);

    // Prebind needed stuff for drawing
    glUseProgram(shader_program);
    glBindVertexArray(VAO);
}

void gl_draw_scene(GLuint texture) {
    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // draw quad
    // VAO and shader program are already bound from the call to gl_setup_scene
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void gl_draw_triangle(void) {
    static float frame = 0;
    // clear
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUniform1f(u_frame, frame++);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, 0);
}

void create_x11_window(Display **x11_display, Window *x11_window, int w,
                       int h) {
    // Open X11 display and create window
    Display *display = XOpenDisplay(NULL);
    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 10, 10, w, h, 1,
        BlackPixel(display, screen), WhitePixel(display, screen));
    XStoreName(display, window, "Client");
    XMapWindow(display, window);

    // Return
    *x11_display = display;
    *x11_window = window;
}

EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;

int do_init(uint32_t width, uint32_t height) {
    // Create X11 window
    Display *x11_display;
    Window x11_window;
    create_x11_window(&x11_display, &x11_window, width, height);

    // Initialize EGL
    initialize_egl(x11_display, x11_window, &egl_display, &egl_context,
                   &egl_surface);

    // Setup GL scene
    gl_setup_scene();

    return 0;
}

double timef(void) {
    double val;
    static double base = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);

    val = tv.tv_sec + (double)tv.tv_usec / 1000000.f;
    if (!base)
        base = val;

    return val - base;
}

int main(int argc, char **argv) {
    int ret;
    struct funnel_ctx *ctx;
    struct funnel_stream *stream;
    uint32_t width = 512;
    uint32_t height = 512;

    enum funnel_mode mode = FUNNEL_ASYNC;
    enum funnel_sync sync = FUNNEL_SYNC_EITHER;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-async"))
            mode = FUNNEL_ASYNC;
        else if (!strcmp(argv[i], "-single"))
            mode = FUNNEL_SINGLE_BUFFERED;
        else if (!strcmp(argv[i], "-double"))
            mode = FUNNEL_DOUBLE_BUFFERED;
        else if (!strcmp(argv[i], "-synchronous"))
            mode = FUNNEL_SYNCHRONOUS;

        else if (!strcmp(argv[i], "-implicit_sync"))
            sync = FUNNEL_SYNC_IMPLICIT;
        else if (!strcmp(argv[i], "-explicit_sync_only"))
            sync = FUNNEL_SYNC_EXPLICIT_ONLY;
        else if (!strcmp(argv[i], "-explicit_sync_hybrid"))
            sync = FUNNEL_SYNC_EXPLICIT_HYBRID;
        else if (!strcmp(argv[i], "-either_sync"))
            sync = FUNNEL_SYNC_EITHER;
    }

    do_init(width, height);

    eglSwapInterval(egl_display, mode == FUNNEL_ASYNC ? 1 : 0);

    ret = funnel_init(&ctx);
    assert(ret == 0);

    ret = funnel_stream_create(ctx, "Funnel Test", &stream);
    assert(ret == 0);

    ret = funnel_stream_init_egl(stream, egl_display);
    assert(ret == 0);

    ret = funnel_stream_set_size(stream, width, height);
    assert(ret == 0);

    ret = funnel_stream_set_mode(stream, mode);
    assert(ret == 0);

    ret = funnel_stream_set_sync(stream, sync);
    assert(ret == 0);

    ret =
        funnel_stream_set_rate(stream, FUNNEL_RATE_VARIABLE,
                               FUNNEL_FRACTION(1, 1), FUNNEL_FRACTION(1000, 1));
    assert(ret == 0);

    ret = funnel_stream_egl_add_format(stream, FUNNEL_EGL_FORMAT_RGBA8888);
    assert(ret == 0);
    ret = funnel_stream_egl_add_format(stream, FUNNEL_EGL_FORMAT_RGB888);
    assert(ret == 0);

    ret = funnel_stream_start(stream);
    assert(ret == 0);

    GLuint fb;
    glGenFramebuffers(1, &fb);

    while (1) {

        assert(glGetError() == GL_NO_ERROR);

        struct funnel_buffer *buf;

        ret = funnel_stream_dequeue(stream, &buf);
        float t = timef();
        assert(ret == 0);
        if (!buf) {
            fprintf(stderr, "[%f] No buffers\n", t);
        } else {
            fprintf(stderr, "[%f] Got buffer\n", t);
        }

        gl_draw_triangle();

        if (buf) {
            EGLImage image;

            ret = funnel_buffer_get_egl_image(buf, &image);
            assert(ret == 0);

            if (funnel_buffer_has_sync(buf)) {
                fprintf(stderr, "[%f] Buffer has sync\n", t);
                EGLSync acquire;
                ret = funnel_buffer_get_acquire_egl_sync(buf, &acquire);
                assert(ret == 0);
                eglWaitSync(egl_display, acquire, 0);
                eglDestroySync(egl_display, acquire);
            }

            GLuint color_tex;
            glGenTextures(1, &color_tex);
            glBindTexture(GL_TEXTURE_2D, color_tex);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

            glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, fb);
            glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER,
                                      GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                                      color_tex, 0);

            glBlitFramebuffer(0, height, width, 0, 0, 0, width, height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);

            glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER,
                                      GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                                      0, 0);
            glDeleteTextures(1, &color_tex);
            glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, 0);

            if (funnel_buffer_has_sync(buf)) {
                EGLSync release = eglCreateSync(
                    egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
                ret = funnel_buffer_set_release_egl_sync(buf, release);
                assert(ret == 0);
                eglDestroySync(egl_display, release);
            } else {
                glFlush();
            }
        }

        eglSwapBuffers(egl_display, egl_surface);

        if (buf) {
            ret = funnel_stream_enqueue(stream, buf);
            if (ret < 0) {
                fprintf(stderr, "Queue failed: %d\n", ret);
            }
            assert(ret == 0 || ret == -ESTALE || ret == -EAGAIN);
        }
    }

    ret = funnel_stream_stop(stream);
    assert(ret == 0);

    funnel_stream_destroy(stream);

    funnel_shutdown(ctx);
}
