// Horror House - step 2: first-person walk test
// Piglet (OpenGL ES) with the system's precompiled "texmap" shaders.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <iostream>
#include <string>
#include <utility>

#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>
#include <orbis/Rtc.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>

#define SCREEN_W 1920
#define SCREEN_H 1080
#define WANT_DEPTH 16      // set to 0 if the app closes immediately
#define TILE_M 2.0f
#define MAX_SURFS 400

// ---------- Precompiled shaders ----------
struct ShaderBlob {
    char *ident;
    unsigned char hash[16];
    uint64_t len;
    unsigned char *code;
};

extern "C" ShaderBlob scePrecompiledShaderEntries[];

static std::pair<unsigned char *, uint64_t> fetchShaderByName(const char *name) {
    for (ShaderBlob *e = scePrecompiledShaderEntries; e->ident != nullptr; ++e) {
        if (strcmp(name, e->ident) == 0)
            return std::make_pair(static_cast<unsigned char *>(e->code),
                                  static_cast<uint64_t>(e->len));
    }
    return std::make_pair(static_cast<unsigned char *>(nullptr),
                          static_cast<uint64_t>(0));
}

// ---------- Globals ----------
static int32_t g_piglet = -1;
static int32_t g_precompiled = -1;

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLContext g_context = EGL_NO_CONTEXT;

static GLuint g_prog = 0;
static GLint g_uMV = -1, g_uProj = -1, g_uTS = -1;
static GLint g_uSampler = -1, g_uOpacity = -1, g_aVertex = -1;
static bool g_hasDepth = false;

static int g_pad = -1;

static GLuint texWall = 0, texFloor = 0, texCeil = 0, texWood = 0;
static GLuint texBlack = 0, texRed = 0, texGreen = 0, texWhite = 0;
static GLuint texVig = 0;

// ---------- Setup ----------
static bool loadModules() {
    int mstart_ret = 0;

    std::string prefix{sceKernelGetFsSandboxRandomWord()};
    prefix = "/" + prefix + "/common/lib/";

    std::string pigletname{prefix + "libScePigletv2VSH.sprx"};
    std::string precompiledname{prefix + "libScePrecompiledShaders.sprx"};

    g_piglet = static_cast<int32_t>(
        sceKernelLoadStartModule(pigletname.c_str(), 0, nullptr, 0, nullptr, &mstart_ret));
    if (g_piglet < ORBIS_OK)
        return false;

    g_precompiled = static_cast<int32_t>(
        sceKernelLoadStartModule(precompiledname.c_str(), 0, nullptr, 0, nullptr, &mstart_ret));
    if (g_precompiled < ORBIS_OK)
        return false;

    return true;
}

static bool createContext() {
    OrbisPglConfig pgl_config{};
    OrbisPglWindow render_window{0, static_cast<khronos_uint32_t>(SCREEN_W),
                                 static_cast<khronos_uint32_t>(SCREEN_H)};
    EGLConfig config{};
    EGLint num_configs = 0;
    EGLint major = -1, minor = -1;

    EGLint attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE,
    };

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };

    const EGLint window_attribs[] = {
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        EGL_NONE,
    };

    memset(&pgl_config, 0, sizeof(pgl_config));
    pgl_config.size = sizeof(pgl_config);
    pgl_config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT |
                       ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    pgl_config.processOrder = 1;
    pgl_config.systemSharedMemorySize = 250 * 1024 * 1024;
    pgl_config.videoSharedMemorySize = 512 * 1024 * 1024;
    pgl_config.maxMappedFlexibleMemory = 170 * 1024 * 1024;
    pgl_config.drawCommandBufferSize = 1 * 1024 * 1024;
    pgl_config.lcueResourceBufferSize = 1 * 1024 * 1024;
    pgl_config.dbgPosCmd_0x40 = SCREEN_W;
    pgl_config.dbgPosCmd_0x44 = SCREEN_H;
    pgl_config.dbgPosCmd_0x48 = 0;
    pgl_config.dbgPosCmd_0x4C = 0;
    pgl_config.unk_0x5C = 2;

    if (!scePigletSetConfigurationVSH(&pgl_config))
        return false;

    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY)
        return false;

    if (!eglInitialize(g_display, &major, &minor))
        return false;

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return false;

    // Try vsync first, then fall back to free-running
    if (!eglSwapInterval(g_display, 1))
        eglSwapInterval(g_display, 0);

    // Try to get a depth buffer, fall back to none
    const EGLint depthTry[2] = {WANT_DEPTH, 0};
    bool chosen = false;

    for (int i = 0; i < 2 && !chosen; i++) {
        attribs[9] = depthTry[i];
        num_configs = 0;

        if (eglChooseConfig(g_display, attribs, &config, 1, &num_configs) &&
            num_configs >= 1) {
            chosen = true;
            g_hasDepth = depthTry[i] > 0;
        }
    }

    if (!chosen)
        return false;

    g_surface = eglCreateWindowSurface(g_display, config, &render_window, window_attribs);
    if (g_surface == EGL_NO_SURFACE)
        return false;

    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_context == EGL_NO_CONTEXT)
        return false;

    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context))
        return false;

    return true;
}

static bool createShaders() {
    auto vert = fetchShaderByName("texmap/v_2.vert");
    auto frag = fetchShaderByName("texmap/f_2.frag");

    if (vert.second == 0 || frag.second == 0)
        return false;

    GLuint fragment_id = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint vertex_id = glCreateShader(GL_VERTEX_SHADER);
    g_prog = glCreateProgram();

    if (!fragment_id || !vertex_id || !g_prog)
        return false;

    // PS4's glShaderBinary only has format 0 (PSSL)
    glShaderBinary(1, &vertex_id, 0, vert.first, static_cast<GLsizei>(vert.second));
    if (glGetError()) return false;

    glShaderBinary(1, &fragment_id, 0, frag.first, static_cast<GLsizei>(frag.second));
    if (glGetError()) return false;

    glAttachShader(g_prog, vertex_id);
    if (glGetError()) return false;

    glAttachShader(g_prog, fragment_id);
    if (glGetError()) return false;

    glLinkProgram(g_prog);
    if (glGetError()) return false;

    g_uMV = glGetUniformLocation(g_prog, "u_modelViewMatrix");
    g_uProj = glGetUniformLocation(g_prog, "u_projectionMatrix");
    g_uTS = glGetUniformLocation(g_prog, "u_textureSpaceMatrix");
    g_uSampler = glGetUniformLocation(g_prog, "s_sampler");
    g_uOpacity = glGetUniformLocation(g_prog, "u_opacity");
    g_aVertex = glGetAttribLocation(g_prog, "a_vertex");

    if (g_uMV < 0 || g_uProj < 0 || g_uTS < 0 ||
        g_uSampler < 0 || g_uOpacity < 0 || g_aVertex < 0)
        return false;

    glDeleteShader(vertex_id);
    glDeleteShader(fragment_id);

    return true;
}

static bool setupSystem() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);

    int ret = sceSystemServiceHideSplashScreen();
    return ret >= ORBIS_OK;
}

static bool initPad() {
    if (scePadInit() != 0)
        return false;

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);

    int userID = 0;
    sceUserServiceGetInitialUser(&userID);

    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

// ---------- Procedural textures ----------
static unsigned int hash2(int x, int y) {
    unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void setPx(unsigned char *b, int w, int x, int y,
                  int r, int g, int bl, int a) {
    unsigned char *p = b + (y * w + x) * 4;
    p[0] = (unsigned char)clamp255(r);
    p[1] = (unsigned char)clamp255(g);
    p[2] = (unsigned char)clamp255(bl);
    p[3] = (unsigned char)clamp255(a);
}

static GLuint makeTexture(const unsigned char *rgba, int w, int h) {
    GLuint id = 0;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    return id;
}

static GLuint makeSolid(int r, int g, int b) {
    unsigned char buf[4 * 4 * 4];

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            setPx(buf, 4, x, y, r, g, b, 255);

    return makeTexture(buf, 4, 4);
}

static void genWall(unsigned char *b) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            int stripe = ((x / 16) % 2) ? 8 : 0;
            int n = (int)(hash2(x, y) % 17) - 8;
            int stain = ((hash2(x / 12, y / 12) % 7) == 0) ? 16 : 0;
            int base = 70 - stain - stripe;

            setPx(b, 128, x, y, base + 18 + n, base + 10 + n, base - 4 + n, 255);
        }
    }
}

static void genFloor(unsigned char *b) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            int plank = y / 32;
            int off = (plank * 37) % 128;
            int ux = (x + off) % 128;

            bool seam = ((y % 32) < 2) || ((ux % 64) < 2);
            int grain = (int)(hash2(x / 3, y) % 15) - 7;
            int tone = 34 + (int)(hash2(plank, ux / 64) % 16);

            if (seam)
                tone = 10;

            setPx(b, 128, x, y, tone + 20 + grain, tone + 10 + grain, tone + grain / 2, 255);
        }
    }
}

static void genCeil(unsigned char *b) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            int n = (int)(hash2(x, y) % 13) - 6;
            int blot = ((hash2(x / 20, y / 20) % 5) == 0) ? 12 : 0;
            int v = 62 - blot + n;

            setPx(b, 128, x, y, v, v - 2, v - 6, 255);
        }
    }
}

static void genWood(unsigned char *b) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            int line = (int)(hash2(x / 2, y / 24) % 12);
            int n = (int)(hash2(x, y) % 9) - 4;
            int v = 48 + line + n;

            setPx(b, 128, x, y, v + 22, v + 10, v - 6, 255);
        }
    }
}

static void genVignette(unsigned char *b) {
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            float dx = ((float)x - 63.5f) / 64.0f;
            float dy = ((float)y - 63.5f) / 64.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float a = (r - 0.30f) / 0.70f;

            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            a = a * a;

            setPx(b, 128, x, y, 0, 0, 0, (int)(a * 255.0f));
        }
    }
}

static void createTextures() {
    static unsigned char buf[128 * 128 * 4];

    genWall(buf);     texWall = makeTexture(buf, 128, 128);
    genFloor(buf);    texFloor = makeTexture(buf, 128, 128);
    genCeil(buf);     texCeil = makeTexture(buf, 128, 128);
    genWood(buf);     texWood = makeTexture(buf, 128, 128);
    genVignette(buf); texVig = makeTexture(buf, 128, 128);

    texBlack = makeSolid(0, 0, 0);
    texRed = makeSolid(230, 30, 30);
    texGreen = makeSolid(30, 220, 60);
    texWhite = makeSolid(255, 255, 255);
}

// ---------- Level geometry ----------
struct Surf {
    glm::mat4 model;
    GLuint tex;
    float rx, ry;
    float cx, cy, cz;
};

static Surf surfs[MAX_SURFS];
static int nsurf = 0;

struct Box {
    float x0, z0, x1, z1;
};

static Box colls[16];
static int ncoll = 0;

static void addSurf(const glm::mat4 &model, GLuint tex, float rx, float ry,
                    float cx, float cy, float cz) {
    if (nsurf >= MAX_SURFS)
        return;

    Surf &s = surfs[nsurf++];
    s.model = model;
    s.tex = tex;
    s.rx = rx;
    s.ry = ry;
    s.cx = cx;
    s.cy = cy;
    s.cz = cz;
}

// Horizontal surface (floor or ceiling) at height y
static void addFloorRect(float x0, float z0, float x1, float z1, float y, GLuint tex) {
    for (float z = z0; z < z1 - 0.001f; z += TILE_M) {
        float zz = (z + TILE_M < z1) ? (z + TILE_M) : z1;

        for (float x = x0; x < x1 - 0.001f; x += TILE_M) {
            float xx = (x + TILE_M < x1) ? (x + TILE_M) : x1;

            float w = xx - x;
            float d = zz - z;

            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, zz));
            m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::scale(m, glm::vec3(w, d, 1.0f));

            addSurf(m, tex, w, d, x + w * 0.5f, y, z + d * 0.5f);
        }
    }
}

// Wall lying in the plane z = zc, spanning x0..x1
static void addWallZ(float zc, float x0, float x1, float y0, float y1, GLuint tex) {
    for (float y = y0; y < y1 - 0.001f; y += TILE_M) {
        float yy = (y + TILE_M < y1) ? (y + TILE_M) : y1;

        for (float x = x0; x < x1 - 0.001f; x += TILE_M) {
            float xx = (x + TILE_M < x1) ? (x + TILE_M) : x1;

            float w = xx - x;
            float h = yy - y;

            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, zc));
            m = glm::scale(m, glm::vec3(w, h, 1.0f));

            addSurf(m, tex, w, h, x + w * 0.5f, y + h * 0.5f, zc);
        }
    }
}

// Wall lying in the plane x = xc, spanning z0..z1
static void addWallX(float xc, float z0, float z1, float y0, float y1, GLuint tex) {
    for (float y = y0; y < y1 - 0.001f; y += TILE_M) {
        float yy = (y + TILE_M < y1) ? (y + TILE_M) : y1;

        for (float z = z0; z < z1 - 0.001f; z += TILE_M) {
            float zz = (z + TILE_M < z1) ? (z + TILE_M) : z1;

            float d = zz - z;
            float h = yy - y;

            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(xc, y, z));
            m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::scale(m, glm::vec3(d, h, 1.0f));

            addSurf(m, tex, d, h, xc, y + h * 0.5f, z + d * 0.5f);
        }
    }
}

static void addBox(float x0, float z0, float x1, float z1,
                   float y0, float y1, GLuint tex) {
    addWallZ(z0, x0, x1, y0, y1, tex);
    addWallZ(z1, x0, x1, y0, y1, tex);
    addWallX(x0, z0, z1, y0, y1, tex);
    addWallX(x1, z0, z1, y0, y1, tex);
    addFloorRect(x0, z0, x1, z1, y1, tex);

    if (ncoll < 16) {
        colls[ncoll].x0 = x0;
        colls[ncoll].z0 = z0;
        colls[ncoll].x1 = x1;
        colls[ncoll].z1 = z1;
        ncoll++;
    }
}

static void addColl(float x0, float z0, float x1, float z1) {
    if (ncoll < 16) {
        colls[ncoll].x0 = x0;
        colls[ncoll].z0 = z0;
        colls[ncoll].x1 = x1;
        colls[ncoll].z1 = z1;
        ncoll++;
    }
}

static void buildLevel() {
    // Room 12 x 12 meters, 3 meters high
    addFloorRect(0, 0, 12, 12, 0.0f, texFloor);
    addFloorRect(0, 0, 12, 12, 3.0f, texCeil);

    addWallZ(0.0f, 0, 12, 0, 3, texWall);
    addWallZ(12.0f, 0, 12, 0, 3, texWall);
    addWallX(0.0f, 0, 12, 0, 3, texWall);
    addWallX(12.0f, 0, 12, 0, 3, texWall);

    // Partition wall with a doorway between x = 5 and x = 7
    addWallZ(6.0f, 0, 5, 0, 3, texWall);
    addWallZ(6.0f, 7, 12, 0, 3, texWall);
    addWallZ(6.0f, 5, 7, 2.2f, 3, texWall);
    addColl(0.0f, 5.85f, 5.0f, 6.15f);
    addColl(7.0f, 5.85f, 12.0f, 6.15f);

    // Table and pillar
    addBox(8, 2, 10, 4, 0.0f, 0.8f, texWood);
    addBox(2.0f, 9.0f, 2.6f, 9.6f, 0.0f, 3.0f, texWall);
}

static bool blocked(float x, float z) {
    const float r = 0.3f;

    if (x < r || x > 12.0f - r || z < r || z > 12.0f - r)
        return true;

    for (int i = 0; i < ncoll; i++) {
        if (x > colls[i].x0 - r && x < colls[i].x1 + r &&
            z > colls[i].z0 - r && z < colls[i].z1 + r)
            return true;
    }

    return false;
}

// ---------- Drawing ----------
static void drawQuad(GLuint tex, const glm::mat4 &proj, const glm::mat4 &mv,
                     float repX, float repY, float opacity) {
    static const GLfloat verts[] = {
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f,

        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f
    };

    glm::mat4 ts = glm::scale(glm::mat4(1.0f), glm::vec3(repX, repY, 1.0f));

    glUniform1f(g_uOpacity, opacity);
    glUniformMatrix4fv(g_uMV, 1, GL_FALSE, &mv[0][0]);
    glUniformMatrix4fv(g_uProj, 1, GL_FALSE, &proj[0][0]);
    glUniformMatrix4fv(g_uTS, 1, GL_FALSE, &ts[0][0]);
    glVertexAttribPointer(g_aVertex, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glEnableVertexAttribArray(g_aVertex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g_uSampler, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void rect2D(GLuint tex, float x, float y, float w, float h, float opacity) {
    glm::mat4 proj = glm::ortho(0.0f, (float)SCREEN_W, (float)SCREEN_H, 0.0f, -1.0f, 1.0f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f));
    m = glm::scale(m, glm::vec3(w, h, 1.0f));

    drawQuad(tex, proj, m, 1.0f, 1.0f, opacity);
}

// ---------- Player ----------
static float camX = 6.0f, camZ = 10.5f;
static float camYaw = 0.0f, camPitch = 0.0f;
static float fps = 0.0f;

static float deadZone(float v) {
    return (v > -0.15f && v < 0.15f) ? 0.0f : v;
}

static void updatePlayer(float dt) {
    if (g_pad < 0)
        return;

    OrbisPadData pd;
    scePadReadState(g_pad, &pd);

    float lx = deadZone(((int)pd.leftStick.x - 128) / 128.0f);
    float ly = deadZone(((int)pd.leftStick.y - 128) / 128.0f);
    float rx = deadZone(((int)pd.rightStick.x - 128) / 128.0f);
    float ry = deadZone(((int)pd.rightStick.y - 128) / 128.0f);

    camYaw += rx * 2.4f * dt;
    camPitch -= ry * 1.6f * dt;

    if (camPitch > 1.2f) camPitch = 1.2f;
    if (camPitch < -1.2f) camPitch = -1.2f;

    float fx = sinf(camYaw), fz = -cosf(camYaw);
    float rxv = cosf(camYaw), rzv = sinf(camYaw);

    float mf = -ly;
    float mr = lx;
    float speed = 2.2f;

    float dx = (fx * mf + rxv * mr) * speed * dt;
    float dz = (fz * mf + rzv * mr) * speed * dt;

    if (!blocked(camX + dx, camZ))
        camX += dx;

    if (!blocked(camX, camZ + dz))
        camZ += dz;
}

static void render() {
    glViewport(0, 0, SCREEN_W, SCREEN_H);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(g_prog);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // keep the alpha channel at 1.0 so the screen is never transparent
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    if (g_hasDepth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    float cp = cosf(camPitch);
    float dirX = sinf(camYaw) * cp;
    float dirY = sinf(camPitch);
    float dirZ = -cosf(camYaw) * cp;

    glm::vec3 eye(camX, 1.6f, camZ);
    glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(dirX, dirY, dirZ),
                                 glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(70.0f),
                                      (float)SCREEN_W / (float)SCREEN_H,
                                      0.1f, 60.0f);

    int vis[MAX_SURFS];
    float fog[MAX_SURFS];
    int nvis = 0;

    for (int i = 0; i < nsurf; i++) {
        Surf &s = surfs[i];

        float ex = s.cx - camX;
        float ey = s.cy - 1.6f;
        float ez = s.cz - camZ;

        // skip what is clearly behind us
        if (ex * dirX + ey * dirY + ez * dirZ < -3.0f)
            continue;

        float dist = sqrtf(ex * ex + ey * ey + ez * ez);
        float f = (dist - 2.0f) / 8.0f;

        if (f < 0.0f) f = 0.0f;
        if (f > 0.95f) f = 0.95f;

        vis[nvis] = i;
        fog[nvis] = f;
        nvis++;
    }

    // Pass 1: the surfaces
    for (int k = 0; k < nvis; k++) {
        Surf &s = surfs[vis[k]];
        drawQuad(s.tex, proj, view * s.model, s.rx, s.ry, 1.0f);
    }

    // Pass 2: darkness (black layer over each surface, stronger with distance)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int k = 0; k < nvis; k++) {
        if (fog[k] < 0.02f)
            continue;

        Surf &s = surfs[vis[k]];
        drawQuad(texBlack, proj, view * s.model, 1.0f, 1.0f, fog[k]);
    }

    // 2D overlay: flashlight vignette + diagnostics
    glDisable(GL_DEPTH_TEST);

    rect2D(texVig, 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0.9f);

    // green = depth buffer works, red = no depth buffer
    rect2D(g_hasDepth ? texGreen : texRed, 40, 40, 40, 40, 1.0f);

    // FPS bar (the white mark is 60 FPS)
    float bar = fps * 6.0f;
    if (bar > 480.0f) bar = 480.0f;
    rect2D(texGreen, 100, 50, bar, 20, 1.0f);
    rect2D(texWhite, 100 + 360, 40, 4, 40, 1.0f);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    eglSwapBuffers(g_display, g_surface);
}

int main() {
    if (!loadModules()) return EXIT_FAILURE;
    if (!createContext()) return EXIT_FAILURE;
    if (!createShaders()) return EXIT_FAILURE;
    if (!setupSystem()) return EXIT_FAILURE;

    initPad();
    createTextures();
    buildLevel();

    uint64_t last = sceKernelGetProcessTime();
    float fpsTime = 0.0f;
    int fpsFrames = 0;

    for (;;) {
        uint64_t now = sceKernelGetProcessTime();
        float rawDt = (float)(now - last) / 1000000.0f;
        last = now;

        float dt = rawDt;
        if (dt > 0.1f) dt = 0.1f;
        if (dt <= 0.0f) dt = 0.001f;

        fpsTime += rawDt;
        fpsFrames++;

        if (fpsTime >= 0.5f) {
            fps = (float)fpsFrames / fpsTime;
            fpsTime = 0.0f;
            fpsFrames = 0;
        }

        updatePlayer(dt);
        render();
    }

    return EXIT_SUCCESS;
}
