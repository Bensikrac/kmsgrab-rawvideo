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
// Definitions
// ----------------------------------------------------------------------------
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, void *image);
typedef EGLImage (EGLAPIENTRYP PFNEGLCREATEIMAGEPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list);

#define DRM_NUM_BUF_ATTRS 4

// Helper to determine plane type
static uint32_t get_plane_type(int drm_fd, uint32_t plane_id) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) return DRM_PLANE_TYPE_OVERLAY;

    uint32_t type = DRM_PLANE_TYPE_OVERLAY;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (prop && strcmp(prop->name, "type") == 0) {
            type = props->prop_values[i];
            drmModeFreeProperty(prop);
            break;
        }
        if (prop) drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return type;
}

const char* egl_error_str(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        default: return "UNKNOWN";
    }
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

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main() {
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

    // 2. Find Primary CRTC and Cursor Plane
    uint32_t crtc_id = 0;
    uint32_t primary_fb_id = 0;
    
    // We need to find the active CRTC first
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (res) {
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
            if (conn && conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
                drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
                if (enc && enc->crtc_id) {
                    crtc_id = enc->crtc_id;
                    drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
                    if (crtc) {
                        primary_fb_id = crtc->buffer_id;
                        drmModeFreeCrtc(crtc);
                    }
                    drmModeFreeEncoder(enc);
                }
                drmModeFreeConnector(conn);
                if (crtc_id) break;
            }
        }
        drmModeFreeResources(res);
    }

    if (!crtc_id) {
        fprintf(stderr, "Failed to find active CRTC.\n");
        return 1;
    }

    // Find Cursor Plane for this CRTC
    uint32_t cursor_plane_id = 0;
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    if (plane_res) {
        for (uint32_t i = 0; i < plane_res->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
            if (plane) {
                // Check if plane is usable by this CRTC
                if (plane->possible_crtcs & (1 << 0)) { // simplistic check, assumes index 0 matches, 
                                                        // real code should map crtc index
                    uint32_t type = get_plane_type(drm_fd, plane->plane_id);
                    if (type == DRM_PLANE_TYPE_CURSOR) {
                        cursor_plane_id = plane->plane_id;
                        drmModeFreePlane(plane);
                        break;
                    }
                }
                drmModeFreePlane(plane);
            }
        }
        drmModeFreePlaneResources(plane_res);
    }

    fprintf(stderr, "Active CRTC: %u, Cursor Plane: %u\n", crtc_id, cursor_plane_id);

    // 3. Shader Setup (Handles Screen + Cursor)
    const char *vshader_src = 
        "#version 300 es\n"
        "in vec2 pos;\n"
        "out vec2 v_tex;\n"
        "void main(){\n"
        "   v_tex = pos*0.5+0.5;\n" // 0..1 range
        "   gl_Position = vec4(pos,0.0,1.0);\n"
        "}\n";

    // Fragment shader composites cursor on top of screen
    const char *fshader_src = 
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;\n"
        "in vec2 v_tex;\n"
        "uniform samplerExternalOES tex_screen;\n"
        "uniform samplerExternalOES tex_cursor;\n"
        "uniform vec2 screen_size;\n"
        "uniform vec2 cursor_pos;\n"   // DRM coordinates (top-left origin)
        "uniform vec2 cursor_size;\n"
        "uniform int cursor_enabled;\n"
        "out vec4 frag_color;\n"
        "void main(){\n"
        "   vec4 bg = texture(tex_screen, v_tex);\n"
        "   if (cursor_enabled == 1) {\n"
        "       vec2 px_coord = v_tex * screen_size;\n"
        "       // DRM/Y is top-down, GL is bottom-up. Convert DRM Y to GL Y.\n"
        "       float gl_cursor_y = screen_size.y - cursor_pos.y - cursor_size.y;\n"
        "       vec2 cursor_uv = (px_coord - vec2(cursor_pos.x, gl_cursor_y)) / cursor_size;\n"
        "       if (cursor_uv.x >= 0.0 && cursor_uv.x <= 1.0 && cursor_uv.y >= 0.0 && cursor_uv.y <= 1.0) {\n"
        "           vec4 cur = texture(tex_cursor, cursor_uv);\n"
        "           // Alpha blending\n"
        "           frag_color = vec4(bg.rgb * (1.0 - cur.a) + cur.rgb * cur.a, 1.0);\n"
        "       } else {\n"
        "           frag_color = bg;\n"
        "       }\n"
        "   } else {\n"
        "       frag_color = bg;\n"
        "   }\n"
        "}\n";

    GLuint vshader = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vshader, 1, &vshader_src, NULL); glCompileShader(vshader);
    GLuint fshader = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fshader, 1, &fshader_src, NULL); glCompileShader(fshader);
    
    GLuint program = glCreateProgram(); glAttachShader(program, vshader); glAttachShader(program, fshader); glLinkProgram(program); glUseProgram(program);

    // Uniforms
    GLint loc_screen = glGetUniformLocation(program, "tex_screen");
    GLint loc_cursor = glGetUniformLocation(program, "tex_cursor");
    GLint loc_screen_size = glGetUniformLocation(program, "screen_size");
    GLint loc_cursor_pos = glGetUniformLocation(program, "cursor_pos");
    GLint loc_cursor_size = glGetUniformLocation(program, "cursor_size");
    GLint loc_cursor_en = glGetUniformLocation(program, "cursor_enabled");

    glUniform1i(loc_screen, 0);
    glUniform1i(loc_cursor, 1);

    float verts[] = { -1, -1,  1, -1,  1, 1,  -1, 1 };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    GLint pos_loc = glGetAttribLocation(program, "pos"); glEnableVertexAttribArray(pos_loc); glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // GL Objects
    GLuint tex_screen = 0, tex_cursor = 0;
    GLuint fbo = 0, tex_out = 0;
    glGenTextures(1, &tex_screen);
    glGenTextures(1, &tex_cursor);
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex_out);

    // State
    uint32_t last_screen_fb = 0;
    uint32_t last_cursor_fb = 0;
    EGLImage screen_image = NULL;
    EGLImage cursor_image = NULL;
    
    uint32_t width = 0, height = 0;
    uint32_t cursor_w = 0, cursor_h = 0;
    size_t size = 0;
    unsigned char *pixels = NULL;

    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (1) {
        next_frame.tv_nsec += 16666666;
        if (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }

        // 4. Update Screen Plane
        uint32_t cur_fb = 0;
        drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
        if (crtc) {
            cur_fb = crtc->buffer_id;
            if (crtc->width != width || crtc->height != height) {
                width = crtc->width; height = crtc->height;
                size = width * height * 4;
                pixels = realloc(pixels, size);
                glBindTexture(GL_TEXTURE_2D, tex_out);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_out, 0);
                glUniform2f(loc_screen_size, width, height);
                fprintf(stderr, "Screen: %dx%d\n", width, height);
            }
            drmModeFreeCrtc(crtc);
        }

        if (cur_fb && cur_fb != last_screen_fb) {
            if (screen_image) eglDestroyImage(display, screen_image);
            screen_image = NULL;
            
            drmModeFB2Ptr fb2 = drmModeGetFB2(drm_fd, cur_fb);
            if (fb2) {
                int fds[4] = {-1}; uint32_t pitches[4], offsets[4]; uint64_t mod = fb2->modifier;
                int n = 0;
                for (int i=0; i<4 && fb2->handles[i]; i++) {
                     drmPrimeHandleToFD(drm_fd, fb2->handles[i], O_RDONLY, &fds[i]);
                     pitches[i] = fb2->pitches[i]; offsets[i] = fb2->offsets[i]; n++;
                }
                
                intptr_t attrs[44];
                setup_dma_buf_attrs(attrs, fb2->pixel_format, width, height, fds, offsets, pitches, &mod, n, true);
                while(eglGetError() != EGL_SUCCESS);
                screen_image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, (EGLAttrib*)attrs);
                
                for(int i=0; i<n; i++) if(fds[i]>=0) close(fds[i]);
                
                if (screen_image) {
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_screen);
                    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, screen_image);
                    last_screen_fb = cur_fb;
                }
                drmModeFreeFB2(fb2);
            }
        }

        // 5. Update Cursor Plane
        int cursor_enabled = 0;
        uint32_t cursor_x = 0, cursor_y = 0;

        if (cursor_plane_id) {
            drmModePlane *plane = drmModeGetPlane(drm_fd, cursor_plane_id);
            if (plane) {
                cursor_x = plane->crtc_x;
                cursor_y = plane->crtc_y;
                uint32_t cfb = plane->fb_id;
                
                // Check visibility
                if (cfb && plane->crtc_id) {
                    cursor_enabled = 1;
                    
                    // Cursor image changed?
                    if (cfb != last_cursor_fb) {
                        if (cursor_image) eglDestroyImage(display, cursor_image);
                        cursor_image = NULL;
                        
                        drmModeFB2Ptr fb2 = drmModeGetFB2(drm_fd, cfb);
                        if (fb2) {
                            cursor_w = fb2->width; cursor_h = fb2->height;
                            
                            int fds[4] = {-1}; uint32_t pitches[4], offsets[4]; uint64_t mod = fb2->modifier;
                            int n=0;
                            for(int i=0; i<4 && fb2->handles[i]; i++) {
                                drmPrimeHandleToFD(drm_fd, fb2->handles[i], O_RDONLY, &fds[i]);
                                pitches[i]=fb2->pitches[i]; offsets[i]=fb2->offsets[i]; n++;
                            }
                            
                            intptr_t attrs[44];
                            // Usually ARGB8888 or similar
                            setup_dma_buf_attrs(attrs, fb2->pixel_format, cursor_w, cursor_h, fds, offsets, pitches, &mod, n, mod != 0);
                            
                            while(eglGetError() != EGL_SUCCESS);
                            cursor_image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, (EGLAttrib*)attrs);
                            
                            for(int i=0; i<n; i++) if(fds[i]>=0) close(fds[i]);

                            if (cursor_image) {
                                glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_cursor);
                                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, cursor_image);
                                last_cursor_fb = cfb;
                            }
                            drmModeFreeFB2(fb2);
                        }
                    }
                }
                drmModeFreePlane(plane);
            }
        }

        if (!cursor_enabled) {
            if (last_cursor_fb != 0) { /* Reset state if needed */ }
            last_cursor_fb = 0;
        }

        // 6. Render
        glViewport(0, 0, width, height);
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_screen);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_cursor);

        glUniform2f(loc_cursor_pos, cursor_x, cursor_y);
        glUniform2f(loc_cursor_size, cursor_w, cursor_h);
        glUniform1i(loc_cursor_en, cursor_enabled);

        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        
        // 7. Read
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (pixels && size > 0) {
            if (fwrite(pixels, 1, size, stdout) != size) break;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }

    return 0;
}
