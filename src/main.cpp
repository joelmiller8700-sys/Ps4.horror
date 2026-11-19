#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <utility>

#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>

namespace {

constexpr int SCREEN_W = 1920;
constexpr int SCREEN_H = 1080;
constexpr int MD5_SIZEOF = 16;

typedef struct ShaderBlob {
    char* ident;
    unsigned char hash[MD5_SIZEOF];
    uint64_t len;
    unsigned char* code;
} OrbisShaderBlob;

extern "C" OrbisShaderBlob scePrecompiledShaderEntries[];

int32_t g_pigletMod = -1;
int32_t g_precompiledMod = -1;

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;

GLuint g_program = GL_NONE;
GLint g_uModelView = -1;
GLint g_uProjection = -1;
GLint g_uTexSpace = -1;
GLint g_sSampler = -1;
GLint g_uOpacity = -1;
GLint g_aVertex = -1;

GLuint g_faceTextures[6] = {0, 0, 0, 0, 0, 0};

int g_padHandle = -1;
int32_t g_userId = -1;

static const GLfloat kQuadVerts[] = {
    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,

    0.0f, 1.0f, 0.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 0.0f, 1.0f, 0.0f
};

std::pair<unsigned char*, uint64_t> fetchShaderByName(const char* shaderName) {
    for (OrbisShaderBlob* entry = scePrecompiledShaderEntries; entry->ident != nullptr; ++entry) {
        if (std::strcmp(shaderName, entry->ident) == 0) {
            return std::make_pair(entry->code, entry->len);
        }
    }
    return std::make_pair(nullptr, static_cast<uint64_t>(0));
}

bool loadModules() {
    int mstartRet = 0;
    std::string prefix = sceKernelGetFsSandboxRandomWord();
    prefix = "/" + prefix + "/common/lib/";

    std::string pigletPath = prefix + "libScePigletv2VSH.sprx";
    std::string shadersPath = prefix + "libScePrecompiledShaders.sprx";

    g_pigletMod = static_cast<int32_t>(sceKernelLoadStartModule(pigletPath.c_str(), 0, nullptr, 0, nullptr, &mstartRet));
    if (g_pigletMod < ORBIS_OK) {
        return false;
    }

    g_precompiledMod = static_cast<int32_t>(sceKernelLoadStartModule(shadersPath.c_str(), 0, nullptr, 0, nullptr, &mstartRet));
    if (g_precompiledMod < ORBIS_OK) {
        return false;
    }

    return true;
}

bool createContext() {
    OrbisPglConfig pglConfig{};
    OrbisPglWindow renderWindow{0, static_cast<khronos_uint32_t>(SCREEN_W), static_cast<khronos_uint32_t>(SCREEN_H)};

    std::memset(&pglConfig, 0, sizeof(pglConfig));
    pglConfig.size = sizeof(pglConfig);
    pglConfig.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    pglConfig.processOrder = 1;
    pglConfig.systemSharedMemorySize = 250 * 1024 * 1024;
    pglConfig.videoSharedMemorySize = 512 * 1024 * 1024;
    pglConfig.maxMappedFlexibleMemory = 170 * 1024 * 1024;
    pglConfig.drawCommandBufferSize = 1 * 1024 * 1024;
    pglConfig.lcueResourceBufferSize = 1 * 1024 * 1024;
    pglConfig.dbgPosCmd_0x40 = SCREEN_W;
    pglConfig.dbgPosCmd_0x44 = SCREEN_H;
    pglConfig.dbgPosCmd_0x48 = 0;
    pglConfig.dbgPosCmd_0x4C = 0;
    pglConfig.unk_0x5C = 2;

    if (!scePigletSetConfigurationVSH(&pglConfig)) {
        return false;
    }

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) {
        return false;
    }

    int major = -1, minor = -1;
    if (!eglInitialize(g_display, &major, &minor)) {
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        return false;
    }

    EGLConfig config{};
    EGLint numConfigs = 0;
    const EGLint depthOptions[3] = {24, 16, 0};

    for (int i = 0; i < 3; ++i) {
        const EGLint attribs[] = {
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, depthOptions[i],
            EGL_STENCIL_SIZE, 0,
            EGL_SAMPLE_BUFFERS, 0,
            EGL_SAMPLES, 0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE
        };
        if (eglChooseConfig(g_display, attribs, &config, 1, &numConfigs) && numConfigs >= 1) {
            break;
        }
    }

    if (numConfigs < 1) {
        return false;
    }

    const EGLint windowAttribs[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        EGL_NONE
    };
    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    g_surface = eglCreateWindowSurface(g_display, config, &renderWindow, windowAttribs);
    if (g_surface == EGL_NO_SURFACE) {
        return false;
    }

    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (g_context == EGL_NO_CONTEXT) {
        return false;
    }

    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        return false;
    }

    if (!eglSwapInterval(g_display, 1)) {
        eglSwapInterval(g_display, 0);
    }

    return true;
}

bool createShaders() {
    auto vert = fetchShaderByName("texmap/v_2.vert");
    auto frag = fetchShaderByName("texmap/f_2.frag");
    if (vert.second == 0 || frag.second == 0) {
        return false;
    }

    GLuint vertId = glCreateShader(GL_VERTEX_SHADER);
    GLuint fragId = glCreateShader(GL_FRAGMENT_SHADER);
    g_program = glCreateProgram();
    if (!vertId || !fragId || !g_program) {
        return false;
    }

    glShaderBinary(1, &vertId, 0, vert.first, vert.second);
    if (glGetError()) return false;

    glShaderBinary(1, &fragId, 0, frag.first, frag.second);
    if (glGetError()) return false;

    glAttachShader(g_program, vertId);
    glAttachShader(g_program, fragId);
    glLinkProgram(g_program);
    if (glGetError()) return false;

    g_uModelView = glGetUniformLocation(g_program, "u_modelViewMatrix");
    g_uProjection = glGetUniformLocation(g_program, "u_projectionMatrix");
    g_uTexSpace = glGetUniformLocation(g_program, "u_textureSpaceMatrix");
    g_sSampler = glGetUniformLocation(g_program, "s_sampler");
    g_uOpacity = glGetUniformLocation(g_program, "u_opacity");
    g_aVertex = glGetAttribLocation(g_program, "a_vertex");

    glDeleteShader(vertId);
    glDeleteShader(fragId);

    return (g_uModelView >= 0 && g_uProjection >= 0 && g_uTexSpace >= 0 &&
            g_sSampler >= 0 && g_uOpacity >= 0 && g_aVertex >= 0);
}

GLuint createCheckerTexture(uint8_t r, uint8_t g, uint8_t b) {
    constexpr int TEX_SIZE = 64;
    uint8_t pixels[TEX_SIZE * TEX_SIZE * 4];

    for (int y = 0; y < TEX_SIZE; ++y) {
        for (int x = 0; x < TEX_SIZE; ++x) {
            int idx = (y * TEX_SIZE + x) * 4;
            bool border = (x < 4 || x >= TEX_SIZE - 4 || y < 4 || y >= TEX_SIZE - 4);
            bool checker = (((x / 16) + (y / 16)) % 2) == 0;

            if (border) {
                pixels[idx + 0] = 240;
                pixels[idx + 1] = 240;
                pixels[idx + 2] = 240;
                pixels[idx + 3] = 255;
            } else if (checker) {
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = 255;
            } else {
                pixels[idx + 0] = static_cast<uint8_t>(r * 0.65f);
                pixels[idx + 1] = static_cast<uint8_t>(g * 0.65f);
                pixels[idx + 2] = static_cast<uint8_t>(b * 0.65f);
                pixels[idx + 3] = 255;
            }
        }
    }

    GLuint texId = 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texId;
}

void initTextures() {
    g_faceTextures[0] = createCheckerTexture(200, 45, 45);   // Front - Red
    g_faceTextures[1] = createCheckerTexture(45, 190, 70);   // Back  - Green
    g_faceTextures[2] = createCheckerTexture(50, 110, 220);  // Right - Blue
    g_faceTextures[3] = createCheckerTexture(220, 190, 45);  // Left  - Yellow
    g_faceTextures[4] = createCheckerTexture(175, 55, 200);  // Top   - Purple
    g_faceTextures[5] = createCheckerTexture(45, 190, 200);  // Bottom- Cyan
}

void initController() {
    if (scePadInit() != 0) {
        return;
    }

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    sceUserServiceGetInitialUser(&g_userId);

    g_padHandle = scePadOpen(g_userId, 0, 0, nullptr);
}

void drawQuad3D(GLuint texId, const glm::mat4& proj, const glm::mat4& modelView) {
    glm::mat4 texSpace(1.0f);
    glUniform1f(g_uOpacity, 1.0f);
    glUniformMatrix4fv(g_uModelView, 1, GL_FALSE, &modelView[0][0]);
    glUniformMatrix4fv(g_uProjection, 1, GL_FALSE, &proj[0][0]);
    glUniformMatrix4fv(g_uTexSpace, 1, GL_FALSE, &texSpace[0][0]);
    glVertexAttribPointer(g_aVertex, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kQuadVerts);
    glEnableVertexAttribArray(g_aVertex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    glUniform1i(g_sSampler, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

float readStickAxis(uint8_t raw) {
    float v = (static_cast<float>(raw) - 128.0f) / 128.0f;
    if (std::fabs(v) < 0.15f) {
        return 0.0f;
    }
    return v;
}

} // namespace

int main() {
    if (!loadModules()) {
        return EXIT_FAILURE;
    }
    if (!createContext()) {
        return EXIT_FAILURE;
    }
    if (!createShaders()) {
        return EXIT_FAILURE;
    }

    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSystemServiceHideSplashScreen();

    initTextures();
    initController();

    float rotY = 0.0f;
    float rotX = 20.0f;
    float camX = 0.0f;
    float camZ = 4.5f;
    bool darkMode = true;
    uint32_t prevButtons = 0;

    glm::mat4 projection = glm::perspective(
        glm::radians(60.0f),
        static_cast<float>(SCREEN_W) / static_cast<float>(SCREEN_H),
        0.1f,
        100.0f
    );

    glm::mat4 centerQuad = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, -0.5f, 0.0f));
    glm::mat4 scaleQuad = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 1.0f));
    glm::mat4 localFace = scaleQuad * centerQuad;

    for (;;) {
        bool userRotating = false;

        if (g_padHandle >= 0) {
            OrbisPadData padData{};
            if (scePadReadState(g_padHandle, &padData) == 0) {
                float lx = readStickAxis(padData.leftStick.x);
                float ly = readStickAxis(padData.leftStick.y);
                float rx = readStickAxis(padData.rightStick.x);
                float ry = readStickAxis(padData.rightStick.y);

                camX += lx * 0.07f;
                camZ += ly * 0.07f;
                if (camZ < 2.2f) camZ = 2.2f;
                if (camZ > 12.0f) camZ = 12.0f;
                if (camX < -4.0f) camX = -4.0f;
                if (camX > 4.0f) camX = 4.0f;

                if (rx != 0.0f || ry != 0.0f) {
                    rotY += rx * 2.5f;
                    rotX += ry * 2.5f;
                    userRotating = true;
                }

                uint32_t justPressed = padData.buttons & (~prevButtons);
                if (justPressed & ORBIS_PAD_BUTTON_CROSS) {
                    darkMode = !darkMode;
                }
                prevButtons = padData.buttons;
            }
        }

        if (!userRotating) {
            rotY += 0.8f;
        }

        glViewport(0, 0, SCREEN_W, SCREEN_H);
        if (darkMode) {
            glClearColor(0.03f, 0.03f, 0.05f, 1.0f);
        } else {
            glClearColor(0.25f, 0.25f, 0.28f, 1.0f);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glUseProgram(g_program);

        glm::mat4 view = glm::lookAt(
            glm::vec3(camX, 0.4f, camZ),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );

        glm::mat4 cubeBase(1.0f);
        cubeBase = glm::rotate(cubeBase, glm::radians(rotX), glm::vec3(1.0f, 0.0f, 0.0f));
        cubeBase = glm::rotate(cubeBase, glm::radians(rotY), glm::vec3(0.0f, 1.0f, 0.0f));

        // 0: Front (+Z)
        glm::mat4 f0 = glm::translate(cubeBase, glm::vec3(0.0f, 0.0f, 1.0f)) * localFace;
        drawQuad3D(g_faceTextures[0], projection, view * f0);

        // 1: Back (-Z)
        glm::mat4 f1 = glm::rotate(glm::translate(cubeBase, glm::vec3(0.0f, 0.0f, -1.0f)), glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * localFace;
        drawQuad3D(g_faceTextures[1], projection, view * f1);

        // 2: Right (+X)
        glm::mat4 f2 = glm::rotate(glm::translate(cubeBase, glm::vec3(1.0f, 0.0f, 0.0f)), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * localFace;
        drawQuad3D(g_faceTextures[2], projection, view * f2);

        // 3: Left (-X)
        glm::mat4 f3 = glm::rotate(glm::translate(cubeBase, glm::vec3(-1.0f, 0.0f, 0.0f)), glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * localFace;
        drawQuad3D(g_faceTextures[3], projection, view * f3);

        // 4: Top (+Y)
        glm::mat4 f4 = glm::rotate(glm::translate(cubeBase, glm::vec3(0.0f, 1.0f, 0.0f)), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) * localFace;
        drawQuad3D(g_faceTextures[4], projection, view * f4);

        // 5: Bottom (-Y)
        glm::mat4 f5 = glm::rotate(glm::translate(cubeBase, glm::vec3(0.0f, -1.0f, 0.0f)), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) * localFace;
        drawQuad3D(g_faceTextures[5], projection, view * f5);

        if (!eglSwapBuffers(g_display, g_surface)) {
            break;
        }
    }

    return EXIT_SUCCESS;
}
