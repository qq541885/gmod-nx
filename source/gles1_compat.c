/* gles1_compat.c -- weak no-op fallbacks for the GLES1 entry points.
 *
 * Only compiled in when switch-mesa did not provide <GLES/gl.h>. Everything is
 * __attribute__((weak)) so that if the toolchain's GLES library does export
 * these symbols, its versions win and these are discarded by the linker.
 *
 * togl reaches the fixed-function path only for legacy UI blits; the 3D scene
 * goes through the GLES2/GLES3 entry points that switch-mesa does provide.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#include "gles1_compat.h"

#if !GMOD_HAVE_GLES1

#define WEAK __attribute__((weak))

WEAK void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  (void)r; (void)g; (void)b; (void)a;
}

WEAK void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
  (void)size; (void)type; (void)stride; (void)pointer;
}

WEAK void glDisableClientState(GLenum cap) { (void)cap; }

WEAK void glEnableClientState(GLenum cap) { (void)cap; }

WEAK void glLoadIdentity(void) { }

WEAK void glMatrixMode(GLenum mode) { (void)mode; }

WEAK void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  (void)l; (void)r; (void)b; (void)t; (void)n; (void)f;
}

WEAK void glPopMatrix(void) { }

WEAK void glPushMatrix(void) { }

WEAK void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
  (void)angle; (void)x; (void)y; (void)z;
}

WEAK void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  (void)x; (void)y; (void)z;
}

WEAK void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
  (void)size; (void)type; (void)stride; (void)pointer;
}

WEAK void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *pointer) {
  (void)size; (void)type; (void)stride; (void)pointer;
}

WEAK void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
  (void)target; (void)pname; (void)param;
}

WEAK void glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat width, GLfloat height) {
  (void)x; (void)y; (void)z; (void)width; (void)height;
}

#endif /* !GMOD_HAVE_GLES1 */
