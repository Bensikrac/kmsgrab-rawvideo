#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-client.h>
#include <sys/mman.h>

void EGLAPIENTRY eglDebugCallback(EGLenum error,
                                  const char *command,
                                  EGLint messageType,
                                  EGLLabelKHR threadLabel,
                                  EGLLabelKHR objectLabel,
                                  const char *message)
{
    const char *etype = "UNKNOWN";
    switch (messageType) {
        case EGL_DEBUG_MSG_CRITICAL_KHR: etype = "CRITICAL"; break;
        case EGL_DEBUG_MSG_ERROR_KHR:    etype = "ERROR"; break;
        case EGL_DEBUG_MSG_WARN_KHR:     etype = "WARNING"; break;
        case EGL_DEBUG_MSG_INFO_KHR:     etype = "INFO"; break;
    }
    fprintf(stderr,
        "[EGL DEBUG] %s: eglError=0x%x, command=%s, message=%s\n",
        etype, error, command, message);
}

// ----------------------------------------------------------------------------
// Missing Definitions for Compatibility
// ----------------------------------------------------------------------------
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, void *image);
typedef EGLImage (EGLAPIENTRYP PFNEGLCREATEIMAGEPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list);

#define DRM_NUM_BUF_ATTRS 4

// ----------------------------------------------------------------------------
// Debug Helpers
// ----------------------------------------------------------------------------
const char* egl_error_str(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "UNKNOWN";
    }
}

// Safe format string function
void get_drm_format_str(uint32_t format, char *buf) {
    buf[0] = (char)(format & 0xFF);
    buf[1] = (char)((format >> 8) & 0xFF);
    buf[2] = (char)((format >> 16) & 0xFF);
    buf[3] = (char)((format >> 24) & 0xFF);
    buf[4] = '\0';
}

// ----------------------------------------------------------------------------
// Attribute Setup (Exact logic from gpu-screen-recorder)
// ----------------------------------------------------------------------------
void setup_dma_buf_attrs(intptr_t *img_attr, uint32_t format, uint32_t width, uint32_t height, 
                         const int *fds, const uint32_t *offsets, const uint32_t *pitches, 
                         const uint64_t *modifiers, int num_planes, bool use_modifier) {
    
    const uint32_t plane_fd_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT
    };

    const uint32_t plane_offset_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT
    };

    const uint32_t plane_pitch_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT
    };

    const uint32_t plane_modifier_lo_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
    };

    const uint32_t plane_modifier_hi_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
    };

    size_t k = 0;

    // Order: FourCC, Width, Height (Matches GSR)
    img_attr[k++] = EGL_LINUX_DRM_FOURCC_EXT;
    img_attr[k++] = format;

    img_attr[k++] = EGL_WIDTH;
    img_attr[k++] = width;

    img_attr[k++] = EGL_HEIGHT;
    img_attr[k++] = height;

    if (num_planes > DRM_NUM_BUF_ATTRS) num_planes = DRM_NUM_BUF_ATTRS;

    for (int i = 0; i < num_planes; ++i) {
        img_attr[k++] = plane_fd_attrs[i];
        img_attr[k++] = fds[i];

        img_attr[k++] = plane_offset_attrs[i];
        img_attr[k++] = offsets[i];

        img_attr[k++] = plane_pitch_attrs[i];
        img_attr[k++] = pitches[i];

        if (use_modifier) {
            img_attr[k++] = plane_modifier_lo_attrs[i];
            img_attr[k++] = modifiers[i] & 0xFFFFFFFFULL;

            img_attr[k++] = plane_modifier_hi_attrs[i];
            img_attr[k++] = modifiers[i] >> 32ULL;
        }
    }

    img_attr[k++] = EGL_NONE;
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main() {
struct wl_display *wl_display = wl_display_connect(NULL);
if (!wl_display) {
    fprintf(stderr, "Failed to connect to Wayland display\n");
    return -1;
}

/*PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR =
    (PFNEGLDEBUGMESSAGECONTROLKHRPROC)eglGetProcAddress("eglDebugMessageControlKHR");

if (eglDebugMessageControlKHR) {
    EGLAttrib attribs[] = {
        EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
        EGL_DEBUG_MSG_ERROR_KHR,    EGL_TRUE,
        EGL_DEBUG_MSG_WARN_KHR,     EGL_TRUE,
        EGL_DEBUG_MSG_INFO_KHR,     EGL_TRUE,
        EGL_NONE
    };
    eglDebugMessageControlKHR(eglDebugCallback, attribs);
    fprintf(stderr, "EGL_KHR_debug callback registered.\n");
} else {
    fprintf(stderr, "Failed to get eglDebugMessageControlKHR\n");
}*/
    int ret;
    int drm_fd = open("/dev/dri/card1", O_RDWR);
    if (drm_fd < 0) { perror("open /dev/dri/card1"); return 1; }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    // 1. EGL Initialization
PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
    (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, wl_display, NULL);

//    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig config;
    EGLint num_config = 0;
    eglChooseConfig(display, attribs, &config, 1, &num_config);
    
    //EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE };
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    PFNEGLCREATEIMAGEPROC eglCreateImage = (PFNEGLCREATEIMAGEPROC)eglGetProcAddress("eglCreateImage");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = 
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglCreateImage || !glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "Failed to get EGL function pointers.\n");
        return 1;
    }

    // 2. Find DRM FB
    drmModeRes *res = drmModeGetResources(drm_fd);
    uint32_t fb_id = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
            if (enc && enc->crtc_id) {
                drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, enc->crtc_id);
                if (crtc) { fb_id = crtc->buffer_id; drmModeFreeCrtc(crtc); }
                drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(conn);
            if (fb_id) break;
        }
    }
    drmModeFreeResources(res);

    // 3. Get FB Details
    uint32_t width, height, pixel_format;
    uint64_t modifier;
    int prime_fds[4] = {-1, -1, -1, -1};
    uint32_t pitches[4], offsets[4];
    int num_planes = 0;

    drmModeFB2Ptr fb2 = drmModeGetFB2(drm_fd, fb_id);
    if (fb2) {
        width = fb2->width; height = fb2->height; 
        pixel_format = fb2->pixel_format; 
        modifier = fb2->modifier;
        
        char fmt_str[5];
        get_drm_format_str(pixel_format, fmt_str);
        printf("Screen: %dx%d\n", width, height);
        printf("Format: %s (0x%x)\n", fmt_str, pixel_format);
        printf("Modifier: 0x%lx\n", modifier);

        for (int i = 0; i < 4; i++) {
            if (fb2->handles[i]) {
                ret = drmPrimeHandleToFD(drm_fd, fb2->handles[i], O_RDONLY, &prime_fds[i]);
                if (ret || prime_fds[i] < 0) {
                    perror("drmPrimeHandleToFD");
                    return 1;
                }
                pitches[i] = fb2->pitches[i];
                offsets[i] = fb2->offsets[i];
                num_planes++;
                printf("Plane %d: FD=%d, Pitch=%u, Offset=%u\n", i, prime_fds[i], pitches[i], offsets[i]);
            }
	    else{
		printf("No plane found for id %d\n", i);
	    }
        }
        drmModeFreeFB2(fb2);
    } else { perror("drmModeGetFB2"); return 1; }

 int dma_fd = prime_fds[0]; 
    uint32_t stride = pitches[0];
    uint32_t offset = offsets[0];         // Offset usually 0 for simple RGBA buffers.
    size_t size = stride * height;

    // Map the DMA buffer into user space
    void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_fd, offset);
    
    if (map == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // Write to file
    FILE *f = fopen("screenshot.raw", "wb");
    if (f) {
        // If stride == width * 4, we can write the whole block.
        // If stride has padding, you must loop row-by-row.
        fwrite(map, 1, size, f);
        fclose(f);
        printf("Saved screenshot.raw (%zu bytes)\n", size);
    } else {
        perror("fopen failed");
    }

    // Cleanup
    munmap(map, size);
    for(int i=0; i<num_planes; i++) close(prime_fds[i]);
    close(drm_fd);
    return 0;
}
