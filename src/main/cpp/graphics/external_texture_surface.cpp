#include "external_texture_surface.h"
#include "egl_renderer.h"
#include "shader_utils.h"

#include <GLES2/gl2ext.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>

namespace hgs::service {
bool ExternalTextureSurface::MakeCurrent() const {
    return context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE &&
        eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
}

void ExternalTextureSurface::OnFrame(void* context) {
    static_cast<ExternalTextureSurface*>(context)->available_.store(true, std::memory_order_release);
}

bool ExternalTextureSurface::Open(NativeWindow* output, int width, int height) {
    display_ = EglRenderer::GetSharedDisplay();
    if (display_ == EGL_NO_DISPLAY) return false;
    constexpr EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(display_, attributes, &config, 1, &count) || !count) return false;
    constexpr EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttrs);
    if (context_ == EGL_NO_CONTEXT) return false;
    surface_ = eglCreateWindowSurface(display_, config, reinterpret_cast<EGLNativeWindowType>(output), nullptr);
    if (!MakeCurrent()) return false;
    eglSwapInterval(display_, 0);

    const auto vertex = winehua::CompileShader(GL_VERTEX_SHADER, winehua::kFullscreenQuadVS);
    const auto fragment = winehua::CompileShader(GL_FRAGMENT_SHADER, winehua::kZeroCopyExternalFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return false;
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
    transformLocation_ = glGetUniformLocation(program_, "uTransform");

    constexpr float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    glGenBuffers(1, &vertices_);
    glBindBuffer(GL_ARRAY_BUFFER, vertices_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_ = OH_NativeImage_Create(texture_, GL_TEXTURE_EXTERNAL_OES);
    if (!image_) return false;
    if (OH_ConsumerSurface_SetDefaultSize(image_, width, height) != 0 ||
        OH_ConsumerSurface_SetDefaultUsage(image_, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE) != 0 ||
        OH_NativeImage_SetDropBufferMode(image_, true) != 0) return false;
    listenerSet_ = OH_NativeImage_SetOnFrameAvailableListener(image_, {this, OnFrame}) == 0;
    if (!listenerSet_) return false;
    producer_ = OH_NativeImage_AcquireNativeWindow(image_);
    return producer_ && glGetError() == GL_NO_ERROR;
}

ExternalTextureSurface::DrawResult ExternalTextureSurface::Draw(bool flipY) {
    EGLint width = 0, height = 0;
    if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &width) ||
        !eglQuerySurface(display_, surface_, EGL_HEIGHT, &height)) return DrawResult::Retry;
    if (width <= 0 || height <= 0) return DrawResult::Idle;
    const bool fresh = available_.exchange(false, std::memory_order_acq_rel);
    if (!fresh && (!hasImage_ || (!needsDraw_ && width == drawnWidth_ && height == drawnHeight_)))
        return DrawResult::Idle;
    if (!MakeCurrent()) return DrawResult::Retry;
    if (fresh) {
        float nativeTransform[16];
        if (OH_NativeImage_UpdateSurfaceImage(image_) != 0 ||
            OH_NativeImage_GetTransformMatrixV2(image_, nativeTransform) != 0) return DrawResult::Retry;
        winehua::ComposeExternalTextureTransform(nativeTransform, flipY, transform_);
        hasImage_ = needsDraw_ = true;
    }
    glViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_);
    glUniformMatrix4fv(transformLocation_, 1, GL_FALSE, transform_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (glGetError() != GL_NO_ERROR || eglSwapBuffers(display_, surface_) != EGL_TRUE) return DrawResult::Retry;
    drawnWidth_ = width;
    drawnHeight_ = height;
    needsDraw_ = false;
    return DrawResult::Presented;
}

ExternalTextureSurface::~ExternalTextureSurface() {
    const bool current = MakeCurrent();
    if (image_) {
        if (listenerSet_) OH_NativeImage_UnsetOnFrameAvailableListener(image_);
        OH_NativeImage_Destroy(&image_);
    }
    if (current) {
        glDeleteTextures(1, &texture_);
        glDeleteBuffers(1, &vertices_);
        if (program_) glDeleteProgram(program_);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
}
}
