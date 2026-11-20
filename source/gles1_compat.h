/* gles1_compat.h -- GLES1 fixed-function entry points that libtogl imports.
 *
 * GDroid's togl still references the GLES1 client-state / matrix stack calls.
 * devkitPro's switch-mesa ships EGL + GLES2/GLES3 and, depending on the build,
 * may or may not ship the GLES1 compatibility layer. If <GLES/gl.h> is present
 * we use it; otherwise we declare the symbols here and provide weak no-op
 * definitions in gles1_compat.c so the module still links.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#ifndef __GLES1_COMPAT_H__
#define __GLES1_COMPAT_H__

#include <GLES2/gl2.h>

#if defined(__has_include)
#  if __has_include(<GLES/gl.h>)
#    include <GLES/gl.h>
#    define GMOD_HAVE_GLES1 1
#  endif
#endif

#ifndef GMOD_HAVE_GLES1
#define GMOD_HAVE_GLES1 0

/* Constants used by the callers (values from the GLES1 spec). */
#ifndef GL_MODELVIEW
#define GL_MODELVIEW  0x1700
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION 0x1701
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY 0x8076
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
void glDisableClientState(GLenum cap);
void glEnableClientState(GLenum cap);
void glLoadIdentity(void);
void glMatrixMode(GLenum mode);
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void glPopMatrix(void);
void glPushMatrix(void);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);

/* glDrawTexfOES lives in GLES1's glext, not gl.h: always declare it. */
#if defined(__has_include)
#  if !__has_include(<GLES/glext.h>)
void glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat width, GLfloat height);
#  endif
#else
void glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat width, GLfloat height);
#endif

#endif /* !GMOD_HAVE_GLES1 */


#endif
