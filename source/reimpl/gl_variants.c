/*
 * Specialized fragment programs for the game's "uber" shader.
 *
 * FF7 draws everything with a single program whose fragment shader branches
 * on three bool uniforms (enableTex, enableOES, enableAlphaTest) and contains
 * a `discard`. On the Vita's SGX every fragment pays for the branches, and a
 * program that may discard defeats the tile-based hidden surface removal for
 * every draw made with it, alpha-tested or not.
 *
 * The game's program is kept for its bookkeeping (uniform locations, link
 * status), but never drawn with: uniform writes made while it is "in use" are
 * shadowed, and every glDrawArrays binds the variant matching the current
 * bools, compiled once on first use:
 *
 *     0: color only          1: texture             2: OES texture
 *     3: texture + discard   4: OES texture + discard
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/gl_variants.h"

#include "utils/logger.h"

#include <string.h>

#define MAX_PROGS    64
#define MAX_UNIFORMS 16
#define NUM_VARIANTS 5

typedef struct {
    char name[32];
    GLint loc;          // location in the game's program
    int is_matrix;
    GLint i;            // glUniform1i value
    GLfloat m[16];      // glUniformMatrix4fv value
    unsigned version;   // bumped on every write
} shadow_uniform;

static GLuint uber_fs;                 // the game's branching fragment shader
static GLuint attached_vs[MAX_PROGS];  // vertex shader attached to each program
static GLuint uber_prog;               // program linked with uber_fs
static GLuint game_prog;               // what the game believes is in use
static GLuint gl_prog;                 // what is really bound

static shadow_uniform uniforms[MAX_UNIFORMS];
static int num_uniforms;
static int u_tex = -1, u_oes = -1, u_alpha = -1;

static struct {
    GLuint prog;
    int failed;
    GLint loc[MAX_UNIFORMS];
    unsigned version[MAX_UNIFORMS];
} variants[NUM_VARIANTS];

static unsigned draws[NUM_VARIANTS];

static const char * const variant_src[NUM_VARIANTS] = {
    "varying mediump vec4 colorVarying;\n"
    "varying mediump vec2 v_texCoord;\n"
    "void main() { gl_FragColor = colorVarying; }\n",

    "varying mediump vec4 colorVarying;\n"
    "varying mediump vec2 v_texCoord;\n"
    "uniform sampler2D s_texture;\n"
    "void main() { gl_FragColor = texture2D(s_texture, v_texCoord) * colorVarying; }\n",

    "varying mediump vec4 colorVarying;\n"
    "varying mediump vec2 v_texCoord;\n"
    "uniform sampler2D s_texture_OES;\n"
    "void main() { gl_FragColor = texture2D(s_texture_OES, v_texCoord) * colorVarying; }\n",

    "varying mediump vec4 colorVarying;\n"
    "varying mediump vec2 v_texCoord;\n"
    "uniform sampler2D s_texture;\n"
    "void main() {\n"
    "    mediump vec4 val = texture2D(s_texture, v_texCoord);\n"
    "    if (val.a <= 0.0) discard;\n"
    "    gl_FragColor = val * colorVarying;\n"
    "}\n",

    "varying mediump vec4 colorVarying;\n"
    "varying mediump vec2 v_texCoord;\n"
    "uniform sampler2D s_texture_OES;\n"
    "void main() {\n"
    "    mediump vec4 val = texture2D(s_texture_OES, v_texCoord);\n"
    "    if (val.a <= 0.0) discard;\n"
    "    gl_FragColor = val * colorVarying;\n"
    "}\n",
};

static const char * const attribs[] = { "position", "color", "texcoord0" };

void gl_variants_shader_source(GLuint shader, const char * src) {
    if (strstr(src, "enableAlphaTest") && strstr(src, "gl_FragColor")) {
        uber_fs = shader;
        l_info("gl_variants: fragment shader %u is the uber shader, specializing it", shader);
    }
}

void glAttachShader_variants(GLuint prog, GLuint shader) {
    glAttachShader(prog, shader);
    if (shader == uber_fs) uber_prog = prog;
    else if (prog < MAX_PROGS) attached_vs[prog] = shader;
}

static int build_variant(int v) {
    GLuint vs = uber_prog < MAX_PROGS ? attached_vs[uber_prog] : 0;
    if (!vs) return 0;

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const GLchar * src = variant_src[v];
    GLint len = (GLint) strlen(src);
    glShaderSource(fs, 1, &src, &len);
    glCompileShader(fs);
    GLint ok = 0;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        l_error("gl_variants: variant %d failed to compile", v);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    // Same attribute slots as the game's program
    for (unsigned a = 0; a < sizeof(attribs) / sizeof(attribs[0]); a++) {
        GLint loc = glGetAttribLocation(uber_prog, attribs[a]);
        if (loc >= 0) glBindAttribLocation(p, (GLuint) loc, attribs[a]);
    }
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        l_error("gl_variants: variant %d failed to link", v);
        return 0;
    }

    variants[v].prog = p;
    for (int u = 0; u < num_uniforms; u++) {
        variants[v].loc[u] = glGetUniformLocation(p, uniforms[u].name);
        variants[v].version[u] = 0;
    }
    l_info("gl_variants: variant %d ready (program %u)", v, p);
    return 1;
}

GLint glGetUniformLocation_variants(GLuint prog, const GLchar * name) {
    GLint loc = glGetUniformLocation(prog, name);
    if (prog != uber_prog || loc == -1) return loc;

    for (int u = 0; u < num_uniforms; u++)
        if (uniforms[u].loc == loc) return loc;
    if (num_uniforms == MAX_UNIFORMS) return loc;

    int u = num_uniforms++;
    strncpy(uniforms[u].name, name, sizeof(uniforms[u].name) - 1);
    uniforms[u].loc = loc;
    uniforms[u].is_matrix = strstr(name, "Matrix") != NULL;
    if (!strcmp(name, "enableTex")) u_tex = u;
    else if (!strcmp(name, "enableOES")) u_oes = u;
    else if (!strcmp(name, "enableAlphaTest")) u_alpha = u;
    // Variants built before this uniform was known look it up now
    for (int v = 0; v < NUM_VARIANTS; v++)
        if (variants[v].prog) variants[v].loc[u] = glGetUniformLocation(variants[v].prog, name);
    return loc;
}

static shadow_uniform * find_uniform(GLint loc) {
    for (int u = 0; u < num_uniforms; u++)
        if (uniforms[u].loc == loc) return &uniforms[u];
    return NULL;
}

static inline int uber_in_use(void) {
    return uber_prog && game_prog == uber_prog;
}

void glUseProgram_variants(GLuint prog) {
    game_prog = prog;
    if (uber_in_use()) return; // the variant is bound at draw time
    if (gl_prog != prog) {
        glUseProgram(prog);
        gl_prog = prog;
    }
}

void glUniform1i_variants(GLint loc, GLint value) {
    if (!uber_in_use()) {
        glUniform1i(loc, value);
        return;
    }
    shadow_uniform * s = find_uniform(loc);
    if (s && s->i != value) {
        s->i = value;
        s->version++;
    }
}

void glUniformMatrix4fv_variants(GLint loc, GLsizei count, GLboolean transpose, const GLfloat * value) {
    if (!uber_in_use()) {
        glUniformMatrix4fv(loc, count, transpose, value);
        return;
    }
    shadow_uniform * s = find_uniform(loc);
    if (s && count >= 1 && !transpose && memcmp(s->m, value, sizeof(s->m))) {
        memcpy(s->m, value, sizeof(s->m));
        s->version++;
    }
}

static inline int flag(int u) {
    return u >= 0 && uniforms[u].i;
}

// Binds the variant for the current state; 0 if it is not available
static int bind_variant(void) {
    int v = 0;
    if (flag(u_tex)) v = 1 + flag(u_oes) + (flag(u_alpha) ? 2 : 0);

    if (!variants[v].prog) {
        if (variants[v].failed) return 0;
        if (!build_variant(v)) {
            variants[v].failed = 1;
            return 0;
        }
    }

    GLuint p = variants[v].prog;
    if (gl_prog != p) {
        glUseProgram(p);
        gl_prog = p;
    }
    for (int u = 0; u < num_uniforms; u++) {
        GLint loc = variants[v].loc[u];
        if (loc == -1 || variants[v].version[u] == uniforms[u].version) continue;
        variants[v].version[u] = uniforms[u].version;
        if (uniforms[u].is_matrix) glUniformMatrix4fv(loc, 1, GL_FALSE, uniforms[u].m);
        else glUniform1i(loc, uniforms[u].i);
    }
    draws[v]++;
    return 1;
}

int gl_variants_before_draw(void) {
    if (!uber_in_use()) return 1;
    if (bind_variant()) return 1;

    // Fallback: the game's own program with every shadowed uniform
    if (gl_prog != uber_prog) {
        glUseProgram(uber_prog);
        gl_prog = uber_prog;
    }
    for (int u = 0; u < num_uniforms; u++) {
        if (uniforms[u].is_matrix) glUniformMatrix4fv(uniforms[u].loc, 1, GL_FALSE, uniforms[u].m);
        else glUniform1i(uniforms[u].loc, uniforms[u].i);
    }
    return 1;
}

void gl_variants_report(double seconds) {
    if (seconds <= 0 || !uber_prog) return;
    l_info("shader variants/s: color %.0f, tex %.0f, oes %.0f, tex+discard %.0f, oes+discard %.0f",
           draws[0] / seconds, draws[1] / seconds, draws[2] / seconds, draws[3] / seconds, draws[4] / seconds);
    memset(draws, 0, sizeof(draws));
}
