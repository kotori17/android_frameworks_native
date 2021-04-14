/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BlurFilter.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <ui/GraphicTypes.h>
#include <cstdint>

#include <utils/Trace.h>

namespace android {
namespace renderengine {
namespace gl {

// This needs to be located in .rodata to get a pointer for the OpenGL API.
static const GLenum kInvalidateAttachment = GL_COLOR_ATTACHMENT0;

BlurFilter::BlurFilter(GLESRenderEngine& engine)
      : mEngine(engine),
        mCompositionFbo(engine),
        mPingFbo(engine),
        mPongFbo(engine),
        mMixProgram(engine),
        mBlurProgram(engine) {
    mMixProgram.compile(getMixVertShader(), getMixFragShader());
    mMPosLoc = mMixProgram.getAttributeLocation("aPosition");
    mMUvLoc = mMixProgram.getAttributeLocation("aUV");
    mMBlurTextureLoc = mMixProgram.getUniformLocation("uBlurTexture");
    mMCompositionTextureLoc = mMixProgram.getUniformLocation("uCompositionTexture");
    mMBlurOpacityLoc = mMixProgram.getUniformLocation("uBlurOpacity");

    mBlurProgram.compile(getBlurVertShader(), getBlurFragShader());
    mBPosLoc = mBlurProgram.getAttributeLocation("aPosition");
    mBUvLoc = mBlurProgram.getAttributeLocation("aUV");
    mBTextureLoc = mBlurProgram.getUniformLocation("uTexture");
    mBOffsetLoc = mBlurProgram.getUniformLocation("uOffset");

    // Initialize constant shader uniforms
    mMixProgram.useProgram();
    glUniform1i(mMBlurTextureLoc, 0);
    glUniform1i(mMCompositionTextureLoc, 1);
    mDitherMixProgram.useProgram();
    glUniform1i(mDBlurTextureLoc, 0);
    glUniform1i(mDCompositionTextureLoc, 1);
    glUniform1i(mDDitherTextureLoc, 2);
    mBlurProgram.useProgram();
    glUniform1i(mBTextureLoc, 0);
    glUseProgram(0);

    static constexpr auto size = 2.0f;
    static constexpr auto translation = 1.0f;
    const GLfloat vboData[] = {
        // Vertex data
        translation - size, -translation - size,
        translation - size, -translation + size,
        translation + size, -translation + size,
        // UV data
        0.0f, 0.0f - translation,
        0.0f, size - translation,
        size, size - translation
    };
    mMeshBuffer.allocateBuffers(vboData, 12 /* size */);
}

status_t BlurFilter::setAsDrawTarget(const DisplaySettings& display, uint32_t radius) {
    ATRACE_NAME("BlurFilter::setAsDrawTarget");
    mRadius = radius;
    mDisplayX = display.physicalDisplay.left;
    mDisplayY = display.physicalDisplay.top;

    if (mDisplayWidth < display.physicalDisplay.width() ||
        mDisplayHeight < display.physicalDisplay.height()) {
        ATRACE_NAME("BlurFilter::allocatingTextures");

        mDisplayWidth = display.physicalDisplay.width();
        mDisplayHeight = display.physicalDisplay.height();
        mCompositionFbo.allocateBuffers(mDisplayWidth, mDisplayHeight);

        const uint32_t fboWidth = floorf(mDisplayWidth * kFboScale);
        const uint32_t fboHeight = floorf(mDisplayHeight * kFboScale);
        mPingFbo.allocateBuffers(fboWidth, fboHeight);
        mPongFbo.allocateBuffers(fboWidth, fboHeight);

        if (mPingFbo.getStatus() != GL_FRAMEBUFFER_COMPLETE) {
            ALOGE("Invalid ping buffer");
            return mPingFbo.getStatus();
        }
        if (mPongFbo.getStatus() != GL_FRAMEBUFFER_COMPLETE) {
            ALOGE("Invalid pong buffer");
            return mPongFbo.getStatus();
        }
        if (mCompositionFbo.getStatus() != GL_FRAMEBUFFER_COMPLETE) {
            ALOGE("Invalid composition buffer");
            return mCompositionFbo.getStatus();
        }
        if (!mBlurProgram.isValid()) {
            ALOGE("Invalid shader");
            return GL_INVALID_OPERATION;
        }
    }

    mCompositionFbo.bind();
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &kInvalidateAttachment);
    glViewport(0, 0, mCompositionFbo.getBufferWidth(), mCompositionFbo.getBufferHeight());
    return NO_ERROR;
}

void BlurFilter::drawMesh(GLuint uv, GLuint position) {

    glEnableVertexAttribArray(uv);
    glEnableVertexAttribArray(position);
    mMeshBuffer.bind();
    glVertexAttribPointer(position, 2 /* size */, GL_FLOAT, GL_FALSE,
                          2 * sizeof(GLfloat) /* stride */, 0 /* offset */);
    glVertexAttribPointer(uv, 2 /* size */, GL_FLOAT, GL_FALSE, 0 /* stride */,
                          (GLvoid*)(6 * sizeof(GLfloat)) /* offset */);
    mMeshBuffer.unbind();

    // draw mesh
    glDrawArrays(GL_TRIANGLES, 0 /* first */, 3 /* count */);
}

status_t BlurFilter::prepare() {
    ATRACE_NAME("BlurFilter::prepare");

    // Kawase is an approximation of Gaussian, but it behaves differently from it.
    // A radius transformation is required for approximating them, and also to introduce
    // non-integer steps, necessary to smoothly interpolate large radii.
    const auto radius = mRadius / 6.0f;

    // Calculate how many passes we'll do, based on the radius.
    const auto passes = max((uint32_t)1, (uint32_t)ceil(radius));

    const float radiusByPasses = radius / (float)passes;
    const float stepX = radiusByPasses / (float)mCompositionFbo.getBufferWidth();
    const float stepY = radiusByPasses / (float)mCompositionFbo.getBufferHeight();

    // This initial downscaling blit makes the first pass correct and improves performance.
    GLFramebuffer* read = &mCompositionFbo;
    GLFramebuffer* draw = &mPingFbo;
    read->bindAsReadBuffer();
    draw->bindAsDrawBuffer();
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &kInvalidateAttachment);
    glBlitFramebuffer(0, 0,
                      read->getBufferWidth(), read->getBufferHeight(),
                      0, 0,
                      draw->getBufferWidth(), draw->getBufferHeight(),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    read = &mPingFbo;
    draw = &mPongFbo;

    // And now we'll ping pong between our textures, to accumulate the result of various offsets.
    mBlurProgram.useProgram();
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &kInvalidateAttachment);
    glViewport(0, 0, draw->getBufferWidth(), draw->getBufferHeight());
    for (auto i = 1; i < passes; i++) {
        ATRACE_NAME("BlurFilter::renderPass");
        draw->bind();

        glBindTexture(GL_TEXTURE_2D, read->getTextureName());
        glUniform2f(mBOffsetLoc, stepX * i, stepY * i);

        drawMesh(mBUvLoc, mBPosLoc);

        // Swap buffers for next iteration
        std::swap(read, draw);
    }
    mLastDrawTarget = read;

    return NO_ERROR;
}

status_t BlurFilter::render(bool multiPass) {
    ATRACE_NAME("BlurFilter::render");

    // Now let's scale our blur up. It will be interpolated with the larger composited
    // texture for the first frames, to hide downscaling artifacts.
    GLfloat opacity = fmin(1.0, mRadius / kMaxCrossFadeRadius);

    // When doing multiple passes, we cannot try to read mCompositionFbo, given that we'll
    // be writing onto it. Let's disable the crossfade, otherwise we'd need 1 extra frame buffer,
    // as large as the screen size.
    if (opacity >= 1 || multiPass) {
        mLastDrawTarget->bindAsReadBuffer();
        glBlitFramebuffer(0, 0, mLastDrawTarget->getBufferWidth(),
                          mLastDrawTarget->getBufferHeight(), mDisplayX, mDisplayY, mDisplayWidth,
                          mDisplayHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        return NO_ERROR;
    }

    mMixProgram.useProgram();
    glUniform1f(mMBlurOpacityLoc, opacity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mLastDrawTarget->getTextureName());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mCompositionFbo.getTextureName());

    drawMesh(mMUvLoc, mMPosLoc);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    mEngine.checkErrors("Drawing blur mesh");
    return NO_ERROR;
}

string BlurFilter::getBlurVertShader() const {
    return R"SHADER(#version 310 es
        precision mediump float;

        uniform vec2 uOffset;

        in vec2 aPosition;
        in vec2 aUV;
        out vec2 vUV;
        out vec2 vBlurTaps[4];

        void main() {
            vUV = aUV;
            gl_Position = vec4(aPosition, 0.0, 1.0);

            vBlurTaps[0] = vUV + vec2( uOffset.x,  uOffset.y);
            vBlurTaps[1] = vUV + vec2( uOffset.x, -uOffset.y);
            vBlurTaps[2] = vUV + vec2(-uOffset.x,  uOffset.y);
            vBlurTaps[3] = vUV + vec2(-uOffset.x, -uOffset.y);
        }
    )SHADER";
}

string BlurFilter::getBlurFragShader() const {
    return R"SHADER(#version 310 es
        precision mediump float;

        uniform sampler2D uTexture;

        in vec2 vUV;
        in vec2 vBlurTaps[4];
        out vec4 fragColor;

        void main() {
            vec3 sum = texture(uTexture, vUV).rgb;
            sum += texture(uTexture, vBlurTaps[0]).rgb;
            sum += texture(uTexture, vBlurTaps[1]).rgb;
            sum += texture(uTexture, vBlurTaps[2]).rgb;
            sum += texture(uTexture, vBlurTaps[3]).rgb;

            fragColor = vec4(sum * 0.2, 1.0);
        }
    )SHADER";
}

string BlurFilter::getMixVertShader() const {
    return R"SHADER(#version 310 es
        precision mediump float;

        in vec2 aPosition;
        in vec2 aUV;
        out vec2 vUV;

        void main() {
            vUV = aUV;
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )SHADER";
}

string BlurFilter::getMixFragShader() const {
    string shader = R"SHADER(#version 310 es
        precision mediump float;

        in vec2 vUV;
        out vec4 fragColor;

        uniform sampler2D uCompositionTexture;
        uniform sampler2D uBlurTexture;
        uniform float uBlurOpacity;

        void main() {
            vec3 blurred = texture(uBlurTexture, vUV).rgb;
            vec3 composition = texture(uCompositionTexture, vUV).rgb;
            fragColor = vec4(mix(composition, blurred, uBlurOpacity), 1.0);
        }
    )SHADER";
    return shader;
}

} // namespace gl
} // namespace renderengine
} // namespace android
