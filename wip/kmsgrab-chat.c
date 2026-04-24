#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define LOG(...) printf(__VA_ARGS__), printf("\n")

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}

int main() {
    LOG("Opening DRM device...");
    int drm_fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) fail("open DRM");

    LOG("Getting DRM resources...");
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) fail("drmModeGetResources");

    LOG("Connectors: %d", res->count_connectors);

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (!conn) continue;

        LOG("Connector %d status=%d", conn->connector_id, conn->connection);

        if (conn->connection == DRM_MODE_CONNECTED) {
            LOG("Using connector %d", conn->connector_id);
            break;
        }
    }

    if (!conn || conn->connection != DRM_MODE_CONNECTED) {
        LOG("No active connector found");
        return 1;
    }

    LOG("Getting encoder...");
    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    if (!enc) fail("drmModeGetEncoder");

    LOG("Getting CRTC...");
    drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, enc->crtc_id);
    if (!crtc) fail("drmModeGetCrtc");

    LOG("CRTC buffer_id=%u", crtc->buffer_id);

    if (crtc->buffer_id == 0) {
        LOG("No active framebuffer (buffer_id=0)");
        return 1;
    }

    LOG("Getting FB2...");
    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, crtc->buffer_id);
    if (!fb2) {
        LOG("drmModeGetFB2 returned NULL (errno=%d)", errno);
        return 1;
    }

    LOG("FB info:");
    LOG("  size: %ux%u", fb2->width, fb2->height);
    LOG("  format: 0x%x", fb2->pixel_format);
    LOG("  handles[0]: %u", fb2->handles[0]);
    LOG("  pitch[0]: %u", fb2->pitches[0]);
    LOG("  offset[0]: %u", fb2->offsets[0]);
    LOG("  modifier[0]: 0x%lx", fb2->modifier[0]);

    if (fb2->handles[0] == 0) {
        LOG("Invalid handle");
        return 1;
    }

    int dmabuf_fd;
    LOG("Exporting DMA-BUF...");
    if (drmPrimeHandleToFD(drm_fd, fb2->handles[0],
                          DRM_CLOEXEC, &dmabuf_fd)) {
        fail("drmPrimeHandleToFD");
    }

    LOG("DMA-BUF FD = %d", dmabuf_fd);

    // --- EGL ---
    LOG("Initializing EGL...");
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) fail("eglGetDisplay");

    if (!eglInitialize(dpy, NULL, NULL))
        fail("eglInitialize");

    EGLConfig cfg;
    EGLint n;

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);

    EGLContext ctx;
    EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);

    EGLSurface surf;
    EGLint surf_attr[] = {
        EGL_WIDTH, fb2->width,
        EGL_HEIGHT, fb2->height,
        EGL_NONE
    };

    surf = eglCreatePbufferSurface(dpy, cfg, surf_attr);
    eglMakeCurrent(dpy, surf, surf, ctx);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
        (void*)eglGetProcAddress("eglCreateImageKHR");

    if (!eglCreateImageKHR) {
        LOG("Missing EGL_EXT_image_dma_buf_import");
        return 1;
    }

    LOG("Creating EGLImage...");

    EGLint img_attr[] = {
        EGL_WIDTH, fb2->width,
        EGL_HEIGHT, fb2->height,
        EGL_LINUX_DRM_FOURCC_EXT, fb2->pixel_format,

        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, fb2->offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, fb2->pitches[0],

        // modifier support (important!)
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            (uint32_t)(fb2->modifier[0] & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            (uint32_t)(fb2->modifier[0] >> 32),

        EGL_NONE
    };

    EGLImageKHR image = eglCreateImageKHR(
        dpy, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        NULL, img_attr
    );

    if (image == EGL_NO_IMAGE_KHR) {
        LOG("EGLImage creation FAILED");
        return 1;
    }

    LOG("EGLImage created OK");

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
        (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glFramebufferTexture2D(GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("FBO incomplete");
        return 1;
    }

    LOG("Reading pixels...");

    size_t bpp = (fb2->pixel_format == DRM_FORMAT_ABGR16161616) ? 8 : 4;

    uint8_t *pixels = malloc(fb2->width * fb2->height * bpp);

    GLenum type = (bpp == 8) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

    glReadPixels(0, 0,
        fb2->width, fb2->height,
        GL_RGBA, type, pixels);

    FILE *f = fopen("frame.raw", "wb");
    fwrite(pixels, 1, fb2->width * fb2->height * bpp, f);
    fclose(f);

    LOG("Saved frame.raw");

    drmModeFreeFB2(fb2);

    return 0;
}
