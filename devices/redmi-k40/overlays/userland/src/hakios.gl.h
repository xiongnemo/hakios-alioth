/*
 * What src/eagl.c asks of the OpenGL ES library behind it besides the gl* functions: see
 * src/gl_osmesa.c, the one implementation there is.
 */
#ifndef HAKIOS_GL_H
#define HAKIOS_GL_H

struct hakios_gl;

struct hakios_gl *hakios_gl_create(int version);              /* a context of OpenGL ES 1, 2 or 3; NULL: none to be had */
struct hakios_gl *hakios_gl_create_shared(int version, struct hakios_gl *share);  /* ... sharing share's objects */
int hakios_gl_make_current(struct hakios_gl *gl);             /* ... the calling thread's (NULL: none); 0 on failure */
int hakios_gl_has_current(void);                               /* (the K40's) a context is the calling thread's now */
int hakios_gl_is_current(const struct hakios_gl *gl);         /* (the K40's) ... and it is this one, by serial, not address */
void hakios_gl_destroy(struct hakios_gl *gl);
const char *hakios_gl_library(void);                           /* for the log */

/*
 * The texture bound to GL_TEXTURE_2D made of an IOSurface's own memory: what is drawn into the
 * texture is in the surface, what is in the surface is what the texture shows, and nobody copies.
 * For a library whose driver can do that (a GPU's) and a surface it can do it with. 0: not
 * done; the texture is to be filled and read back with OpenGL ES calls.
 */
int hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface);

/*
 * The picture of a renderbuffer of the current context copied by the GPU into a texture of it, turned
 * over or not, with nothing of the context's state touched -- in any version of OpenGL ES (version 1
 * has no glBlitFramebuffer). 0: not done (no such objects, or a library that cannot).
 */
int hakios_gl_copy(struct hakios_gl *gl, unsigned renderbuffer, unsigned texture, int flip);

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each); these
 * are the names its Metal renderer has for them, four characters a function, in their order.
 */
#define HAKIOS_CA_FUNCTIONS \
	"clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma bds bco bim c33 c3b c4b c4p c3a c3x c4x bia grl ds4 ds6 " \
	"ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrb sb3 axl rad rac con vcm vdk vlt map mpc crv hue clm hom opa met " \
	"srl ega sdr alt p3c edr hrm lut ovr ovl lub aup alm stl pc0 pc1 prc psl ps0 ps1 psm psr pcr pbl peo bib asg a75 " \
	"a50 atc ipc apl 3dl cst "

#endif
