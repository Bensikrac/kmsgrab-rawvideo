#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-client.h>

// ----------------------------------------------------------------------------
// Definitions & Prototypes
// ----------------------------------------------------------------------------
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, void *image);
typedef EGLImage (EGLAPIENTRYP PFNEGLCREATEIMAGEPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list);

#define DRM_NUM_BUF_ATTRS 4

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

void get_drm_format_str(uint32_t format, char *buf) {
    buf[0] = (char)(format & 0xFF);
    buf[1] = (char)((format >> 8) & 0xFF);
    buf[2] = (char)((format >> 16) & 0xFF);
    buf[3] = (char)((format >> 24) & 0xFF);
    buf[4] = '\0';
}

void setup_dma_buf_attrs(intptr_t *img_attr, uint32_t format, uint32_t width, uint32_t height, 
                         const int *fds, const uint32_t *offsets, const uint32_t *pitches, 
                         const uint64_t *modifiers, int num_planes, bool use_modifier) {
    
    const uint32_t plane_fd_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, 
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT
    };
    const uint32_t plane_offset_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, 
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT
    };
    const uint32_t plane_pitch_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, 
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT
    };
    const uint32_t plane_modifier_lo_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, 
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
    };
    const uint32_t plane_modifier_hi_attrs[DRM_NUM_BUF_ATTRS] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, 
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
    };

    size_t k = 0;
    img_attr[k++] = EGL_LINUX_DRM_FOURCC_EXT; img_attr[k++] = format;
    img_attr[k++] = EGL_WIDTH; img_attr[k++] = width;
    img_attr[k++] = EGL_HEIGHT; img_attr[k++] = height;

    if (num_planes > DRM_NUM_BUF_ATTRS) num_planes = DRM_NUM_BUF_ATTRS;

    for (int i = 0; i < num_planes; ++i) {
        img_attr[k++] = plane_fd_attrs[i]; img_attr[k++] = fds[i];
        img_attr[k++] = plane_offset_attrs[i]; img_attr[k++] = offsets[i];
        img_attr[k++] = plane_pitch_attrs[i]; img_attr[k++] = pitches[i];
        if (use_modifier) {
            img_attr[k++] = plane_modifier_lo_attrs[i]; img_attr[k++] = modifiers[i] & 0xFFFFFFFFULL;
            img_attr[k++] = plane_modifier_hi_attrs[i]; img_attr[k++] = modifiers[i] >> 32ULL;
        }
    }
    img_attr[k++] = EGL_NONE;
}

uint32_t get_current_fb_id(int drm_fd) {
    uint32_t fb_id = 0;
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) return 0;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
            if (enc && enc->crtc_id) {
                drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, enc->crtc_id);
                if (crtc) {
                    fb_id = crtc->buffer_id;
                    drmModeFreeCrtc(crtc);
                }
                drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(conn);
            if (fb_id) break;
        }
    }
    drmModeFreeResources(res);
    return fb_id;
}

int main() {
    // Disable stdout buffering for pipeline compatibility
    setbuf(stdout, NULL);

    struct wl_display *wl_display = wl_display_connect(NULL);
    if (!wl_display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return -1;
    }

    int ret;
    int drm_fd = open("/dev/dri/card1", O_RDWR);
    if (drm_fd < 0) { perror("open /dev/dri/card1"); return 1; }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    // 1. EGL Initialization
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, wl_display, NULL);

    eglInitialize(display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig config;
    EGLint num_config = 0;
    eglChooseConfig(display, attribs, &config, 1, &num_config);
    
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

    // 2. Shader Setup
    const char *vshader_src = 
        "#version 300 es\nin vec2 pos;out vec2 v_tex;void main(){v_tex=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}\n";
    const char *fshader_src = 
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;\n"
        "in vec2 v_tex;\n"
        "uniform samplerExternalOES tex;\n"
        "out vec4 frag_color;\n"
        "void main(){frag_color=texture(tex,v_tex);}\n";

    GLuint vshader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vshader, 1, &vshader_src, NULL); glCompileShader(vshader);
    GLuint fshader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fshader, 1, &fshader_src, NULL); glCompileShader(fshader);
    
    GLuint program = glCreateProgram(); glAttachShader(program, vshader); glAttachShader(program, fshader); glLinkProgram(program); glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "tex"), 0);

    float verts[] = { -1, -1,  1, -1,  1, 1,  -1, 1 };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint pos_loc = glGetAttribLocation(program, "pos"); glEnableVertexAttribArray(pos_loc); glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // State tracking
    uint32_t last_fb_id = 0;
    EGLImage current_image = NULL; // Tracks the currently bound EGLImage
    GLuint tex_external = 0;
    GLuint fbo = 0, tex_out = 0;
    
    uint32_t width = 0, height = 0;
    size_t size = 0;
    unsigned char *pixels = NULL;

    glGenTextures(1, &tex_external);
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex_out);

    // Timing for 60 FPS
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (1) {
        // Calculate next frame time (1/60s ~ 16.6ms)
        next_frame.tv_nsec += 16666666;
        if (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }

        // 3. Find current DRM FB
        uint32_t fb_id = get_current_fb_id(drm_fd);
        
        if (fb_id == 0) {
            // No active framebuffer, wait and retry
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
            continue;
        }

        // 4. Handle FB Change / Initialization
        if (fb_id != last_fb_id) {
            // Attempt to setup the new buffer
            int prime_fds[4] = {-1, -1, -1, -1};
            uint32_t pitches[4], offsets[4];
            uint64_t modifier = 0;
            int num_planes = 0;
            uint32_t pixel_format = 0;
            uint32_t w = 0, h = 0;

            drmModeFB2Ptr fb2 = drmModeGetFB2(drm_fd, fb_id);
            if (fb2) {
                w = fb2->width; 
                h = fb2->height; 
                pixel_format = fb2->pixel_format; 
                modifier = fb2->modifier;
                
                for (int i = 0; i < 4; i++) {
                    if (fb2->handles[i]) {
                        ret = drmPrimeHandleToFD(drm_fd, fb2->handles[i], O_RDONLY, &prime_fds[i]);
                        if (ret || prime_fds[i] < 0) {
                            perror("drmPrimeHandleToFD");
                            // Close any FDs we managed to open so far
                            for(int j=0; j<i; j++) close(prime_fds[j]);
                            fb_id = 0; // Mark as failure
                            break;
                        }
                        pitches[i] = fb2->pitches[i];
                        offsets[i] = fb2->offsets[i];
                        num_planes++;
                    }
                }
                drmModeFreeFB2(fb2);
            } else { 
                perror("drmModeGetFB2"); 
                fb_id = 0; 
            }

            if (fb_id != 0 && num_planes > 0) {
                // Create New EGLImage
                intptr_t img_attribs[44];
                setup_dma_buf_attrs(img_attribs, pixel_format, w, h, prime_fds, offsets, pitches, &modifier, num_planes, modifier != 0);

                while (eglGetError() != EGL_SUCCESS); // Flush errors
                
                EGLImage new_image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, (const EGLAttrib*)img_attribs);
                EGLint err = eglGetError();

                // Close FDs immediately after import attempt
                for(int i=0; i<num_planes; i++) close(prime_fds[i]);

                if (new_image && err == EGL_SUCCESS) {
                    // Bind the new image to the texture
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_external);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    
                    // This implicitly detaches the old image
                    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, new_image);

                    // Now safe to destroy the OLD image
                    if (current_image) {
                        eglDestroyImage(display, current_image);
                    }
                    current_image = new_image;
                    last_fb_id = fb_id;

                    // Handle resolution changes
                    if (w != width || h != height) {
                        width = w; height = h;
                        size = width * height * 4;
                        pixels = realloc(pixels, size);
                        
                        glBindTexture(GL_TEXTURE_2D, tex_out);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        
                        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_out, 0);
                        
                        fprintf(stderr, "Init: %dx%d Format: 0x%x\n", width, height, pixel_format);
                    }
                } else {
                    fprintf(stderr, "eglCreateImage failed: %s (0x%x)\n", egl_error_str(err), err);
                    if (new_image) eglDestroyImage(display, new_image); // Cleanup if created but error set
                    // Do not update last_fb_id so we retry next frame
                }
            } else {
                // Failed to get FB details or prime FDs
                if (fb_id != 0) {
                    // Close FDs if we failed after prime but before import logic
                    for(int i=0; i<4; i++) if(prime_fds[i] >= 0) close(prime_fds[i]);
                }
            }
        } // End FB change handling

        // Only render if we have a valid image setup
        if (current_image && width > 0 && height > 0) {
            // 5. Render to FBO
            glViewport(0, 0, width, height); 
            glClearColor(0,0,0,1); 
            glClear(GL_COLOR_BUFFER_BIT);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_external);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            
            // 6. Read Pixels
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

            // 7. Output to stdout
            if (pixels && size > 0) {
                size_t written = fwrite(pixels, 1, size, stdout);
                if (written != size) {
                    break; // Pipe closed
                }
            }
        }

        // Sleep until next frame
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }

    free(pixels);
    if (current_image) eglDestroyImage(display, current_image);
    close(drm_fd);
    return 0;
}
