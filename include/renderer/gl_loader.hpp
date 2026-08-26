#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>


#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif

// OpenGL function pointer definitions
#ifndef GL_APIENTRY
#define GL_APIENTRY APIENTRY
#endif

// ---------------------------------------------------------------------------
// OpenGL 1.2+ entry points on Windows
//
// opengl32.dll only exports OpenGL 1.1. Anything newer must be resolved at runtime
// through SDL_GL_GetProcAddress, exactly like the 2.0+ functions below.
//
// The complication is that SDL_opengl.h already *declares* some of these as
// __declspec(dllimport) functions, which is where the unresolved __imp_gl* symbols
// come from at link time. A same-named function pointer would clash with that
// declaration, so the pointers are given distinct names and the call sites are
// redirected with a macro.
//
// Linux is untouched: libGL exports these directly and the system headers declare
// them, so the engine keeps calling them normally there.
// ---------------------------------------------------------------------------
#ifdef _WIN32
typedef void (GL_APIENTRY *PFNGLACTIVETEXTURELITHPROC)(GLenum texture);
typedef void (GL_APIENTRY *PFNGLDRAWBUFFERSLITHPROC)(GLsizei n, const GLenum *bufs);
// GL 1.2, needed by the TESLA texture array, and equally absent from opengl32.dll.
typedef void (GL_APIENTRY *PFNGLTEXIMAGE3DLITHPROC)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels);
typedef void (GL_APIENTRY *PFNGLTEXSUBIMAGE3DLITHPROC)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels);

extern PFNGLACTIVETEXTURELITHPROC  lithium_glActiveTexture;
extern PFNGLDRAWBUFFERSLITHPROC    lithium_glDrawBuffers;
extern PFNGLTEXIMAGE3DLITHPROC     lithium_glTexImage3D;
extern PFNGLTEXSUBIMAGE3DLITHPROC  lithium_glTexSubImage3D;

#define glActiveTexture lithium_glActiveTexture
#define glDrawBuffers   lithium_glDrawBuffers
#define glTexImage3D    lithium_glTexImage3D
#define glTexSubImage3D lithium_glTexSubImage3D
#endif

typedef GLuint (GL_APIENTRY *PFNGLCREATESHADERPROC)(GLenum type);
typedef void (GL_APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void (GL_APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void (GL_APIENTRY *PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint *params);
typedef void (GL_APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLuint (GL_APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
typedef void (GL_APIENTRY *PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void (GL_APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void (GL_APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint *params);
typedef void (GL_APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void (GL_APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void (GL_APIENTRY *PFNGLDELETESHADERPROC)(GLuint shader);
typedef void (GL_APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint program);

typedef void (GL_APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint *arrays);
typedef void (GL_APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void (GL_APIENTRY *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint *arrays);

typedef void (GL_APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint *buffers);
typedef void (GL_APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (GL_APIENTRY *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void (GL_APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint *buffers);

typedef void (GL_APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (GL_APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);

typedef GLint (GL_APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar *name);
typedef void (GL_APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (GL_APIENTRY *PFNGLUNIFORM3FVPROC)(GLint location, GLsizei count, const GLfloat *value);
typedef void (GL_APIENTRY *PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void (GL_APIENTRY *PFNGLUNIFORM1FPROC)(GLint location, GLfloat v0);
typedef void (GL_APIENTRY *PFNGLUNIFORM2FPROC)(GLint location, GLfloat v0, GLfloat v1);
typedef void (GL_APIENTRY *PFNGLUNIFORM4FVPROC)(GLint location, GLsizei count, const GLfloat *value);
typedef void (GL_APIENTRY *PFNGLTEXBUFFERPROC)(GLenum target, GLenum internalformat, GLuint buffer);
typedef void (GL_APIENTRY *PFNGLUNIFORM3FPROC)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);

// FBO Definitions
// GL 1.4. Not declared by <GL/gl.h> on Linux without GL_GLEXT_PROTOTYPES, so it is
// resolved at runtime like the rest of the modern entry points.
// GL 4.1 program binaries. Used to cache linked shader programs on disk so a
// restart does not have to recompile every shader from source.
typedef void (GL_APIENTRY *PFNGLGETPROGRAMBINARYPROC)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary);
typedef void (GL_APIENTRY *PFNGLPROGRAMBINARYPROC)(GLuint program, GLenum binaryFormat, const void *binary, GLsizei length);
typedef void (GL_APIENTRY *PFNGLPROGRAMPARAMETERIPROC)(GLuint program, GLenum pname, GLint value);

typedef void (GL_APIENTRY *PFNGLBLENDFUNCSEPARATEPROC)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
typedef void (GL_APIENTRY *PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint *framebuffers);
typedef void (GL_APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (GL_APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (GL_APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (GL_APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint *framebuffers);
typedef void (GL_APIENTRY *PFNGLGENRENDERBUFFERSPROC)(GLsizei n, GLuint *renderbuffers);
typedef void (GL_APIENTRY *PFNGLBINDRENDERBUFFERPROC)(GLenum target, GLuint renderbuffer);
typedef void (GL_APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC)(unsigned int target, unsigned int attachment, unsigned int renderbuffertarget, unsigned int renderbuffer);
typedef void (*PFNGLDELETERENDERBUFFERSPROC)(int n, const unsigned int *renderbuffers);

// MSAA Extensions
typedef void (*PFNGLTEXIMAGE2DMULTISAMPLEPROC)(unsigned int target, int samples, unsigned int internalformat, int width, int height, unsigned char fixedsamplelocations);
typedef void (GL_APIENTRY *PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (GL_APIENTRY *PFNGLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

typedef void (GL_APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum target);

// Declare function pointers extern
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap;
extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram;

extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays;

extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;

// Timer queries, used by the render profiler to attribute GPU time to passes.
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

// Occlusion queries. The conservative form is allowed to report "visible" for a
// box that in fact contributed no samples, which is exactly the right trade for
// culling: a false positive costs one wasted draw, a false negative makes an
// object vanish.
#ifndef GL_ANY_SAMPLES_PASSED
#define GL_ANY_SAMPLES_PASSED 0x8C2F
#endif
#ifndef GL_ANY_SAMPLES_PASSED_CONSERVATIVE
#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A
#endif
typedef void (GL_APIENTRY *PFNGLGENQUERIESPROC_LZ)(GLsizei n, GLuint* ids);
typedef void (GL_APIENTRY *PFNGLDELETEQUERIESPROC_LZ)(GLsizei n, const GLuint* ids);
typedef void (GL_APIENTRY *PFNGLBEGINQUERYPROC_LZ)(GLenum target, GLuint id);
typedef void (GL_APIENTRY *PFNGLENDQUERYPROC_LZ)(GLenum target);
typedef void (GL_APIENTRY *PFNGLGETQUERYOBJECTUIVPROC_LZ)(GLuint id, GLenum pname, GLuint* params);
typedef void (GL_APIENTRY *PFNGLGETQUERYOBJECTUI64VPROC_LZ)(GLuint id, GLenum pname, GLuint64* params);
extern PFNGLGENQUERIESPROC_LZ glGenQueries;
extern PFNGLDELETEQUERIESPROC_LZ glDeleteQueries;
extern PFNGLBEGINQUERYPROC_LZ glBeginQuery;
extern PFNGLENDQUERYPROC_LZ glEndQuery;
extern PFNGLGETQUERYOBJECTUIVPROC_LZ glGetQueryObjectuiv;
extern PFNGLGETQUERYOBJECTUI64VPROC_LZ glGetQueryObjectui64v;

typedef void (GL_APIENTRY *PFNGLMULTIDRAWELEMENTSPROC)(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei drawcount);
extern PFNGLMULTIDRAWELEMENTSPROC glMultiDrawElements;

typedef void (GL_APIENTRY *PFNGLUNIFORM1UIPROC)(GLint location, GLuint v0);
typedef void (GL_APIENTRY *PFNGLBINDBUFFERBASEPROC)(GLenum target, GLuint index, GLuint buffer);
typedef void (GL_APIENTRY *PFNGLDISPATCHCOMPUTEPROC)(GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z);
typedef void (GL_APIENTRY *PFNGLMEMORYBARRIERPROC)(GLbitfield barriers);
typedef void (GL_APIENTRY *PFNGLMULTIDRAWELEMENTSINDIRECTPROC)(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride);

extern PFNGLUNIFORM1UIPROC glUniform1ui;
extern PFNGLBINDBUFFERBASEPROC glBindBufferBase;
extern PFNGLDISPATCHCOMPUTEPROC glDispatchCompute;
extern PFNGLMEMORYBARRIERPROC glMemoryBarrier;
extern PFNGLMULTIDRAWELEMENTSINDIRECTPROC glMultiDrawElementsIndirect;

extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
// Integer attribute variant, needed by the skinning stream: bone indices must
// reach the shader as exact ints, and the float entry point would convert them.
extern PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer;
// Instanced drawing. The divisor is what makes an attribute advance once per
// instance instead of once per vertex, which is how a per-instance transform is
// fed to a shader without a uniform update per object.
extern PFNGLUNIFORM4FPROC glUniform4f;
extern PFNGLBUFFERSUBDATAPROC glBufferSubData;
extern PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor;
extern PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced;
extern PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced;

extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM2FPROC glUniform2f;
extern PFNGLUNIFORM4FVPROC glUniform4fv;
extern PFNGLTEXBUFFERPROC glTexBuffer;
extern PFNGLUNIFORM3FPROC glUniform3f;

extern PFNGLGETPROGRAMBINARYPROC glGetProgramBinary;
extern PFNGLPROGRAMBINARYPROC   glProgramBinary;
extern PFNGLPROGRAMPARAMETERIPROC glProgramParameteri;
extern PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;

extern PFNGLTEXIMAGE2DMULTISAMPLEPROC glTexImage2DMultisample;
extern PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC glRenderbufferStorageMultisample;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;

inline bool load_gl_functions() {
    #define LOAD_PROC(type, name) \
        name = (type)SDL_GL_GetProcAddress(#name); \
        if (!name) { \
            std::cerr << "Failed to load OpenGL function: " << #name << std::endl; \
            return false; \
        }

    LOAD_PROC(PFNGLCREATESHADERPROC, glCreateShader);
    LOAD_PROC(PFNGLSHADERSOURCEPROC, glShaderSource);
    LOAD_PROC(PFNGLCOMPILESHADERPROC, glCompileShader);
    LOAD_PROC(PFNGLGETSHADERIVPROC, glGetShaderiv);
    LOAD_PROC(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
    LOAD_PROC(PFNGLCREATEPROGRAMPROC, glCreateProgram);
    LOAD_PROC(PFNGLATTACHSHADERPROC, glAttachShader);
    LOAD_PROC(PFNGLLINKPROGRAMPROC, glLinkProgram);
    LOAD_PROC(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
    LOAD_PROC(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
    LOAD_PROC(PFNGLUSEPROGRAMPROC, glUseProgram);
    LOAD_PROC(PFNGLDELETESHADERPROC, glDeleteShader);
    LOAD_PROC(PFNGLDELETEPROGRAMPROC, glDeleteProgram);

    LOAD_PROC(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    LOAD_PROC(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    LOAD_PROC(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);

    LOAD_PROC(PFNGLGENBUFFERSPROC, glGenBuffers);
    LOAD_PROC(PFNGLBINDBUFFERPROC, glBindBuffer);
    LOAD_PROC(PFNGLBUFFERDATAPROC, glBufferData);
    LOAD_PROC(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);

    LOAD_PROC(PFNGLGENQUERIESPROC_LZ, glGenQueries);
    LOAD_PROC(PFNGLDELETEQUERIESPROC_LZ, glDeleteQueries);
    LOAD_PROC(PFNGLBEGINQUERYPROC_LZ, glBeginQuery);
    LOAD_PROC(PFNGLENDQUERYPROC_LZ, glEndQuery);
    LOAD_PROC(PFNGLGETQUERYOBJECTUIVPROC_LZ, glGetQueryObjectuiv);
    LOAD_PROC(PFNGLGETQUERYOBJECTUI64VPROC_LZ, glGetQueryObjectui64v);
    
    LOAD_PROC(PFNGLMULTIDRAWELEMENTSPROC, glMultiDrawElements);
    
    LOAD_PROC(PFNGLUNIFORM1UIPROC, glUniform1ui);
    LOAD_PROC(PFNGLBINDBUFFERBASEPROC, glBindBufferBase);
    LOAD_PROC(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute);
    LOAD_PROC(PFNGLMEMORYBARRIERPROC, glMemoryBarrier);
    LOAD_PROC(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect);

    LOAD_PROC(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    LOAD_PROC(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
    LOAD_PROC(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer);
    LOAD_PROC(PFNGLUNIFORM4FPROC, glUniform4f);
    LOAD_PROC(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
    LOAD_PROC(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor);
    LOAD_PROC(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
    LOAD_PROC(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced);

    LOAD_PROC(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    LOAD_PROC(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv);
    LOAD_PROC(PFNGLUNIFORM3FVPROC, glUniform3fv);
    LOAD_PROC(PFNGLUNIFORM1IPROC, glUniform1i);
    LOAD_PROC(PFNGLUNIFORM1FPROC, glUniform1f);
    LOAD_PROC(PFNGLUNIFORM2FPROC, glUniform2f);
    LOAD_PROC(PFNGLUNIFORM4FVPROC, glUniform4fv);
    LOAD_PROC(PFNGLTEXBUFFERPROC, glTexBuffer);
    LOAD_PROC(PFNGLUNIFORM3FPROC, glUniform3f);

#ifdef _WIN32
    // See the note above: GL 1.2+ is not exported by opengl32.dll.
    LOAD_PROC(PFNGLACTIVETEXTURELITHPROC, lithium_glActiveTexture);
    LOAD_PROC(PFNGLDRAWBUFFERSLITHPROC, lithium_glDrawBuffers);
    LOAD_PROC(PFNGLTEXIMAGE3DLITHPROC, lithium_glTexImage3D);
    LOAD_PROC(PFNGLTEXSUBIMAGE3DLITHPROC, lithium_glTexSubImage3D);
#endif
    LOAD_PROC(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary);
    LOAD_PROC(PFNGLPROGRAMBINARYPROC, glProgramBinary);
    LOAD_PROC(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri);
    LOAD_PROC(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate);
    LOAD_PROC(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
    LOAD_PROC(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    LOAD_PROC(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    LOAD_PROC(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    LOAD_PROC(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    LOAD_PROC(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers);
    LOAD_PROC(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer);
    LOAD_PROC(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
    glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    
    glTexImage2DMultisample = (PFNGLTEXIMAGE2DMULTISAMPLEPROC)SDL_GL_GetProcAddress("glTexImage2DMultisample");
    LOAD_PROC(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample);
    LOAD_PROC(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer);
    LOAD_PROC(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap);

    #undef LOAD_PROC
    return true;
}
