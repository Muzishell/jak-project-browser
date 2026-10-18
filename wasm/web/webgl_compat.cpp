/*!
 * @file webgl_compat.cpp
 * The desktop GL calls the renderer makes that WebGL2 (OpenGL ES 3.0) doesn't have, as adapters
 * installed over glad's function pointers (CLAUDE.MD, decision 8). The renderer code is unchanged.
 *  - 1D textures are 2D textures of height 1 (the shaders read them with texelFetch(s, ivec2(i, 0)))
 *  - GL_UNSIGNED_INT_8_8_8_8_REV with RGBA is the byte order of GL_UNSIGNED_BYTE on little endian
 *  - primitive restart is always on in WebGL2, with the index the renderer sets (0xFFFFFFFF)
 *  - multi-draw is a loop of draws (WEBGL_multi_draw later, if profiling asks)
 *  - polygon mode (debug wireframe) and the debug output do nothing
 *  - texture size queries and readback come from a size table and a framebuffer read
 */

#include <unordered_map>

#include "common/log/log.h"

#include "third-party/glad/include/glad/glad.h"

namespace webgl_compat {
namespace {

// the real WebGL2 entry points the adapters forward to
PFNGLBINDTEXTUREPROC real_bind_texture;
PFNGLTEXPARAMETERIPROC real_tex_parameteri;
PFNGLTEXPARAMETERFPROC real_tex_parameterf;
PFNGLTEXIMAGE2DPROC real_tex_image_2d;
PFNGLTEXSUBIMAGE2DPROC real_tex_sub_image_2d;
PFNGLENABLEPROC real_enable;
PFNGLDISABLEPROC real_disable;

GLenum map_target(GLenum target) {
  return target == GL_TEXTURE_1D ? GL_TEXTURE_2D : target;
}

GLenum map_type(GLenum format, GLenum type) {
  if (type == GL_UNSIGNED_INT_8_8_8_8_REV && format == GL_RGBA) {
    return GL_UNSIGNED_BYTE;
  }
  return type;
}

// level-0 sizes of 2D textures, for glGetTexLevelParameteriv
std::unordered_map<GLuint, std::pair<GLint, GLint>> texture_sizes;

GLuint bound_texture_2d() {
  GLint tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  return GLuint(tex);
}

void APIENTRY bind_texture(GLenum target, GLuint texture) {
  real_bind_texture(map_target(target), texture);
}

void APIENTRY tex_parameteri(GLenum target, GLenum pname, GLint param) {
  real_tex_parameteri(map_target(target), pname, param);
}

void APIENTRY tex_parameterf(GLenum target, GLenum pname, GLfloat param) {
  real_tex_parameterf(map_target(target), pname, param);
}

void APIENTRY tex_image_2d(GLenum target,
                           GLint level,
                           GLint internalformat,
                           GLsizei width,
                           GLsizei height,
                           GLint border,
                           GLenum format,
                           GLenum type,
                           const void* pixels) {
  real_tex_image_2d(target, level, internalformat, width, height, border, format,
                    map_type(format, type), pixels);
  if (target == GL_TEXTURE_2D && level == 0) {
    texture_sizes[bound_texture_2d()] = {width, height};
  }
}

void APIENTRY tex_sub_image_2d(GLenum target,
                               GLint level,
                               GLint xoffset,
                               GLint yoffset,
                               GLsizei width,
                               GLsizei height,
                               GLenum format,
                               GLenum type,
                               const void* pixels) {
  real_tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format,
                        map_type(format, type), pixels);
}

void APIENTRY tex_image_1d(GLenum target,
                           GLint level,
                           GLint internalformat,
                           GLsizei width,
                           GLint border,
                           GLenum format,
                           GLenum type,
                           const void* pixels) {
  tex_image_2d(map_target(target), level, internalformat, width, 1, border, format, type, pixels);
}

void APIENTRY tex_sub_image_1d(GLenum target,
                               GLint level,
                               GLint xoffset,
                               GLsizei width,
                               GLenum format,
                               GLenum type,
                               const void* pixels) {
  tex_sub_image_2d(map_target(target), level, xoffset, 0, width, 1, format, type, pixels);
}

bool is_desktop_only_cap(GLenum cap) {
  return cap == GL_PRIMITIVE_RESTART || cap == GL_DEBUG_OUTPUT || cap == GL_TEXTURE_1D ||
         cap == GL_PROGRAM_POINT_SIZE || cap == GL_MULTISAMPLE;
}

void APIENTRY enable(GLenum cap) {
  if (!is_desktop_only_cap(cap)) {
    real_enable(cap);
  }
}

void APIENTRY disable(GLenum cap) {
  if (!is_desktop_only_cap(cap)) {
    real_disable(cap);
  }
}

void APIENTRY primitive_restart_index(GLuint index) {
  if (index != 0xFFFFFFFF) {
    lg::warn("WebGL2: primitive restart index is always 0xFFFFFFFF, not 0x{:x}", index);
  }
}

void APIENTRY multi_draw_elements(GLenum mode,
                                  const GLsizei* count,
                                  GLenum type,
                                  const void* const* indices,
                                  GLsizei drawcount) {
  for (GLsizei i = 0; i < drawcount; i++) {
    if (count[i] > 0) {
      glDrawElements(mode, count[i], type, indices[i]);
    }
  }
}

void APIENTRY color_mask_i(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  // only one draw buffer is ever masked
  if (index == 0) {
    glColorMask(r, g, b, a);
  }
}

void APIENTRY framebuffer_texture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
  glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture, level);
}

void APIENTRY clear_depth(GLdouble depth) {
  glClearDepthf(GLfloat(depth));
}

void APIENTRY polygon_mode(GLenum, GLenum) {}

void APIENTRY debug_message_callback(GLDEBUGPROC, const void*) {}

void APIENTRY debug_message_control(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean) {}

void APIENTRY get_tex_level_parameteriv(GLenum target, GLint level, GLenum pname, GLint* params) {
  auto it = texture_sizes.find(bound_texture_2d());
  if (target != GL_TEXTURE_2D || level != 0 || it == texture_sizes.end()) {
    lg::warn("WebGL2: no size known for texture level parameter 0x{:x}", pname);
    *params = 0;
    return;
  }
  *params = pname == GL_TEXTURE_WIDTH ? it->second.first : it->second.second;
}

void APIENTRY get_tex_image(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
  // read the bound 2D texture through a temporary framebuffer
  auto it = texture_sizes.find(bound_texture_2d());
  if (target != GL_TEXTURE_2D || it == texture_sizes.end()) {
    lg::warn("WebGL2: glGetTexImage of an unknown texture");
    return;
  }
  GLint old_fbo = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_fbo);
  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->first,
                         level);
  glReadPixels(0, 0, it->second.first, it->second.second, format, map_type(format, type), pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_fbo);
  glDeleteFramebuffers(1, &fbo);
}

void APIENTRY tex_image_2d_multisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean) {
  lg::error("WebGL2: multisample textures are not supported");
}

}  // namespace

/*!
 * Install the adapters over glad's function pointers. Call right after gladLoadGLLoader.
 */
void install() {
  real_bind_texture = glad_glBindTexture;
  real_tex_parameteri = glad_glTexParameteri;
  real_tex_parameterf = glad_glTexParameterf;
  real_tex_image_2d = glad_glTexImage2D;
  real_tex_sub_image_2d = glad_glTexSubImage2D;
  real_enable = glad_glEnable;
  real_disable = glad_glDisable;

  glad_glBindTexture = bind_texture;
  glad_glTexParameteri = tex_parameteri;
  glad_glTexParameterf = tex_parameterf;
  glad_glTexImage2D = tex_image_2d;
  glad_glTexSubImage2D = tex_sub_image_2d;
  glad_glTexImage1D = tex_image_1d;
  glad_glTexSubImage1D = tex_sub_image_1d;
  glad_glEnable = enable;
  glad_glDisable = disable;
  glad_glPrimitiveRestartIndex = primitive_restart_index;
  glad_glMultiDrawElements = multi_draw_elements;
  glad_glColorMaski = color_mask_i;
  glad_glFramebufferTexture = framebuffer_texture;
  glad_glClearDepth = clear_depth;
  glad_glPolygonMode = polygon_mode;
  glad_glDebugMessageCallback = debug_message_callback;
  glad_glDebugMessageControl = debug_message_control;
  glad_glGetTexLevelParameteriv = get_tex_level_parameteriv;
  glad_glGetTexImage = get_tex_image;
  glad_glTexImage2DMultisample = tex_image_2d_multisample;
}

}  // namespace webgl_compat
