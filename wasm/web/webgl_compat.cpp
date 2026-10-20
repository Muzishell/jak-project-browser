/*!
 * @file webgl_compat.cpp
 * The desktop GL calls the renderer makes that WebGL2 (OpenGL ES 3.0) doesn't have, as adapters
 * installed over glad's function pointers (CLAUDE.MD, decision 8). The renderer code is unchanged.
 *  - 1D textures are 2D textures of height 1 (the shaders read them with texelFetch(s, ivec2(i,
 * 0)))
 *  - GL_UNSIGNED_INT_8_8_8_8_REV with RGBA is the byte order of GL_UNSIGNED_BYTE on little endian;
 *    GL_UNSIGNED_INT_8_8_8_8 is the reverse (pixels byte-swapped on upload)
 *  - primitive restart is always on in WebGL2, with the index the renderer sets (0xFFFFFFFF)
 *  - multi-draw is a loop of draws (WEBGL_multi_draw later, if profiling asks)
 *  - polygon mode (debug wireframe) and the debug output do nothing
 *  - texture size queries and readback come from a size table and a framebuffer read
 *  - one texture level is never attached at two color attachments of a framebuffer (WebGL makes
 *    such a framebuffer unsupported)
 *  - WebGL errors are logged per frame; kLocateErrors finds the call that made one
 */

#include <emscripten.h>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
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
  if ((type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_INT_8_8_8_8) &&
      format == GL_RGBA) {
    return GL_UNSIGNED_BYTE;
  }
  return type;
}

// GL_UNSIGNED_INT_8_8_8_8 packs R in the high byte of each u32: the bytes of each pixel reversed
// compared to GL_UNSIGNED_BYTE. Returns the pixels to upload as UNSIGNED_BYTE.
const void* map_pixels(GLenum format,
                       GLenum type,
                       const void* pixels,
                       size_t pixel_count,
                       std::vector<u32>* scratch) {
  if (!pixels || type != GL_UNSIGNED_INT_8_8_8_8 || format != GL_RGBA) {
    return pixels;
  }
  scratch->resize(pixel_count);
  const u32* in = (const u32*)pixels;
  for (size_t i = 0; i < pixel_count; i++) {
    (*scratch)[i] = __builtin_bswap32(in[i]);
  }
  return scratch->data();
}

// level-0 sizes of 2D textures, for glGetTexLevelParameteriv
std::unordered_map<GLuint, std::pair<GLint, GLint>> texture_sizes;

// WebGL errors are logged once per frame (log_errors), a limited number. To find the call that
// makes one, set kLocateErrors: draws, clears and texture uploads then check the error state and
// the framebuffer before and after, with a JS stack. Off by default: the checks are synchronous
// GL queries on every call.
constexpr bool kLocateErrors = false;
int logged_errors = 0;
constexpr int kMaxLoggedErrors = 40;

void check_error(const char* what,
                 GLenum target,
                 GLint internalformat,
                 GLenum format,
                 GLenum type) {
  GLenum err = glGetError();
  if (err != GL_NO_ERROR && logged_errors < kMaxLoggedErrors) {
    logged_errors++;
    lg::error("WebGL error 0x{:x} in {} (target 0x{:x} internal 0x{:x} format 0x{:x} type 0x{:x})",
              err, what, target, internalformat, format, type);
    // where it came from (function names: --profiling-funcs)
    EM_ASM({ console.error("WebGL error stack: " + new Error().stack); });
  }
}

// kLocateErrors: draws and clears into an incomplete framebuffer report the framebuffer (once each)
PFNGLCLEARPROC real_clear;
PFNGLDRAWELEMENTSPROC real_draw_elements;
PFNGLDRAWARRAYSPROC real_draw_arrays;
std::unordered_map<GLint, bool> reported_fbos;

void check_framebuffer(const char* what, GLenum which = GL_DRAW_FRAMEBUFFER) {
  GLint fbo = 0;
  glGetIntegerv(
      which == GL_DRAW_FRAMEBUFFER ? GL_DRAW_FRAMEBUFFER_BINDING : GL_READ_FRAMEBUFFER_BINDING,
      &fbo);
  GLenum status = glCheckFramebufferStatus(which);
  if (status != GL_FRAMEBUFFER_COMPLETE && !reported_fbos[fbo]) {
    reported_fbos[fbo] = true;
    GLint type = 0, name = 0;
    glGetFramebufferAttachmentParameteriv(which, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type != GL_NONE) {
      glGetFramebufferAttachmentParameteriv(which, GL_COLOR_ATTACHMENT0,
                                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    }
    lg::error("WebGL: {} with incomplete {} framebuffer {} (status 0x{:x}, color 0x{:x} #{})", what,
              which == GL_DRAW_FRAMEBUFFER ? "draw" : "read", fbo, status, type, name);
  }
}

PFNGLBLITFRAMEBUFFERPROC real_blit_framebuffer;
PFNGLCLEARBUFFERFVPROC real_clear_bufferfv;
PFNGLREADPIXELSPROC real_read_pixels;

void APIENTRY blit_framebuffer(GLint sx0,
                               GLint sy0,
                               GLint sx1,
                               GLint sy1,
                               GLint dx0,
                               GLint dy0,
                               GLint dx1,
                               GLint dy1,
                               GLbitfield mask,
                               GLenum filter) {
  check_framebuffer("glBlitFramebuffer", GL_READ_FRAMEBUFFER);
  check_framebuffer("glBlitFramebuffer");
  real_blit_framebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
  check_error("glBlitFramebuffer", 0, 0, mask, filter);
}

void APIENTRY clear_bufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
  check_framebuffer("glClearBufferfv");
  real_clear_bufferfv(buffer, drawbuffer, value);
}

void APIENTRY read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* d) {
  check_framebuffer("glReadPixels", GL_READ_FRAMEBUFFER);
  real_read_pixels(x, y, w, h, f, t, d);
}

void APIENTRY clear(GLbitfield mask) {
  check_framebuffer("glClear");
  real_clear(mask);
}

void APIENTRY draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  check_framebuffer("glDrawElements");
  check_error("a call before glDrawElements", 0, 0, 0, 0);
  real_draw_elements(mode, count, type, indices);
  check_error("glDrawElements", mode, count, type, 0);
}

void APIENTRY draw_arrays(GLenum mode, GLint first, GLsizei count) {
  check_framebuffer("glDrawArrays");
  check_error("a call before glDrawArrays", 0, 0, 0, 0);
  real_draw_arrays(mode, first, count);
  check_error("glDrawArrays", mode, first, count, 0);
}

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
  std::vector<u32> scratch;
  if constexpr (kLocateErrors) {
    check_error("a call before glTexImage2D", 0, 0, 0, 0);
  }
  real_tex_image_2d(target, level, internalformat, width, height, border, format,
                    map_type(format, type),
                    map_pixels(format, type, pixels, size_t(width) * height, &scratch));
  if constexpr (kLocateErrors) {
    check_error("glTexImage2D", target, internalformat, format, type);
  }
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
  std::vector<u32> scratch;
  if constexpr (kLocateErrors) {
    check_error("a call before glTexSubImage2D", 0, 0, 0, 0);
  }
  real_tex_sub_image_2d(target, level, xoffset, yoffset, width, height, format,
                        map_type(format, type),
                        map_pixels(format, type, pixels, size_t(width) * height, &scratch));
  if constexpr (kLocateErrors) {
    check_error("glTexSubImage2D", target, 0, format, type);
  }
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

// The renderer only ever draws to one color attachment per framebuffer. FramebufferTexturePair
// puts mip level i at COLOR_ATTACHMENTi with glDrawBuffers({COLOR_ATTACHMENTi}), which WebGL
// refuses (entry k of glDrawBuffers must be NONE or COLOR_ATTACHMENTk; COLOR_ATTACHMENT8 and up
// may not exist at all), so level i would be drawn nowhere. Each level has its own framebuffer,
// so every color attachment and single draw buffer becomes COLOR_ATTACHMENT0.
GLenum color_attachment_0(GLenum attachment) {
  return attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT0 + 31
             ? GL_COLOR_ATTACHMENT0
             : attachment;
}

void APIENTRY framebuffer_texture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
  glFramebufferTexture2D(target, color_attachment_0(attachment), GL_TEXTURE_2D, texture, level);
}

PFNGLDRAWBUFFERSPROC real_draw_buffers;

void APIENTRY draw_buffers(GLsizei n, const GLenum* bufs) {
  if (n == 1) {
    GLenum buf = color_attachment_0(bufs[0]);
    real_draw_buffers(1, &buf);
  } else {
    real_draw_buffers(n, bufs);
  }
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
 * Log the WebGL errors of the frame (a limited number). Call after each frame.
 */
void log_errors() {
  check_error("frame", 0, 0, 0, 0);
  while (glGetError() != GL_NO_ERROR) {
  }
}

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
  if constexpr (kLocateErrors) {
    real_clear = glad_glClear;
    real_draw_elements = glad_glDrawElements;
    real_draw_arrays = glad_glDrawArrays;
    real_blit_framebuffer = glad_glBlitFramebuffer;
    real_clear_bufferfv = glad_glClearBufferfv;
    real_read_pixels = glad_glReadPixels;
    glad_glBlitFramebuffer = blit_framebuffer;
    glad_glClearBufferfv = clear_bufferfv;
    glad_glReadPixels = read_pixels;
    glad_glClear = clear;
    glad_glDrawElements = draw_elements;
    glad_glDrawArrays = draw_arrays;
  }

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
  real_draw_buffers = glad_glDrawBuffers;
  glad_glDrawBuffers = draw_buffers;
  glad_glClearDepth = clear_depth;
  glad_glPolygonMode = polygon_mode;
  glad_glDebugMessageCallback = debug_message_callback;
  glad_glDebugMessageControl = debug_message_control;
  glad_glGetTexLevelParameteriv = get_tex_level_parameteriv;
  glad_glGetTexImage = get_tex_image;
  glad_glTexImage2DMultisample = tex_image_2d_multisample;
}

}  // namespace webgl_compat
