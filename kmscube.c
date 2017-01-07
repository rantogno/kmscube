/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include "esUtil.h"
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>


#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Exit after rendering the given number of frames. If 0, then continue
 * rendering forever.
 */
static uint64_t arg_frames = 0;

struct frame {
	struct gbm_bo *gbm_bo;
	EGLImageKHR egl_image;
	GLuint gl_renderbuffer;
};

static struct frame frames[2];

static struct {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	GLuint fb;
	GLuint program;
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLuint vbo;
	GLuint positionsoffset, colorsoffset, normalsoffset;

	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES;
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR;
	PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR;
	PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR;
	PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;
} gl;

static struct {
	struct gbm_device *dev;
} gbm;

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

struct encoder {
	drmModeEncoder *encoder;
};

struct connector {
	drmModeConnector *connector;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct fb {
	drmModeFB *fb;
};

struct plane {
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

static struct {
	int fd;
	drmModeRes *res;
	drmModePlaneRes *plane_res;

	struct crtc *crtcs;
	struct encoder *encoders;
	struct connector *connectors;
	struct fb *fbs;
	struct plane *planes;
	drmModeAtomicReq *req;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

static drmModeEncoder *get_encoder_by_id(uint32_t id)
{
	drmModeEncoder *encoder;
	int i;

	for (i = 0; i < drm.res->count_encoders; i++) {
		encoder = drm.encoders[i].encoder;
		if (encoder && encoder->encoder_id == id)
			return encoder;
	}

	return NULL;
}

static int get_crtc_index(uint32_t id)
{
	int i;

	for (i = 0; i < drm.res->count_crtcs; ++i) {
		drmModeCrtc *crtc = drm.crtcs[i].crtc;
		if (crtc && crtc->crtc_id == id)
			return i;
	}

	return -1;
}

static int prepare_connector()
{
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder;
	uint32_t possible_crtcs = ~0;
	uint32_t active_crtcs = 0;
	unsigned int crtc_idx, idx, i, area;

	for (i = 0; i < drm.res->count_connectors; i++) {
		connector = drm.connectors[i].connector;
		if (connector->connection == DRM_MODE_CONNECTED)
			break;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

	drm.connector_id = connector->connector_id;

	for (i = 0; i < connector->count_encoders; ++i) {
		encoder = get_encoder_by_id(connector->encoders[i]);
		if (!encoder)
			continue;

		possible_crtcs |= encoder->possible_crtcs;

		idx = get_crtc_index(encoder->crtc_id);
		if (idx >= 0)
			active_crtcs |= 1 << idx;
	}

	if (!possible_crtcs)
		return -1;

	/* Return the first possible and active CRTC if one exists, or the first
	 * possible CRTC otherwise.
	 */
	if (possible_crtcs & active_crtcs)
		crtc_idx = ffs(possible_crtcs & active_crtcs);
	else
		crtc_idx = ffs(possible_crtcs);

	drm.crtc_id = drm.crtcs[crtc_idx -1 ].crtc->crtc_id;

	/* find prefered mode or the highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			drm.mode = current_mode;
		}

		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		printf("could not find mode!\n");
		return -1;
	}

	return 0;
}

static void free_drm()
{
	int i;

#define free_resource(_drm, __res, type, Type)					\
	do {									\
		if (!(_drm).type##s)						\
			break;							\
		for (i = 0; i < (int)(_drm).__res->count_##type##s; ++i) {	\
			if (!(_drm).type##s[i].type)				\
				break;						\
			drmModeFree##Type((_drm).type##s[i].type);		\
		}								\
		free((_drm).type##s);						\
	} while (0)

#define free_properties(_drm, __res, type)					\
	do {									\
		for (i = 0; i < (int)(_drm).__res->count_##type##s; ++i) {	\
			drmModeFreeObjectProperties(_drm.type##s[i].props);	\
			free(_drm.type##s[i].props_info);			\
		}								\
	} while (0)

	if (drm.res) {
		free_properties(drm, res, crtc);

		free_resource(drm, res, crtc, Crtc);
		free_resource(drm, res, encoder, Encoder);
		free_resource(drm, res, connector, Connector);
		free_resource(drm, res, fb, FB);

		drmModeFreeResources(drm.res);
	}

	if (drm.plane_res) {
		free_properties(drm, plane_res, plane);

		free_resource(drm, plane_res, plane, Plane);

		drmModeFreePlaneResources(drm.plane_res);
	}
}

static int init_drm(void)
{
	static const char *modules[] = {
			"i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos", "msm", "tegra", "virtio_gpu"
	};
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(modules); i++) {
		printf("trying to load module %s...", modules[i]);
		drm.fd = drmOpen(modules[i], NULL);
		if (drm.fd < 0) {
			printf("failed.\n");
		} else {
			printf("success.\n");
			break;
		}
	}

	if (drm.fd < 0) {
		printf("could not open drm device\n");
		return -1;
	}

	ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		printf("no atomic modesetting support: %s\n", strerror(errno));
		return -1;
	}

	drm.res = drmModeGetResources(drm.fd);
	if (!drm.res) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	drm.crtcs = calloc(drm.res->count_crtcs, sizeof(*drm.crtcs));
	drm.encoders = calloc(drm.res->count_encoders, sizeof(*drm.encoders));
	drm.connectors = calloc(drm.res->count_connectors, sizeof(*drm.connectors));
	drm.fbs = calloc(drm.res->count_fbs, sizeof(*drm.fbs));

	if (!drm.crtcs || !drm.encoders || !drm.connectors || !drm.fbs)
		goto error;

#define get_resource(_drm, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_drm).__res->count_##type##s; ++i) {	\
			(_drm).type##s[i].type =				\
				drmModeGet##Type((_drm).fd, (_drm).__res->type##s[i]); \
			if (!(_drm).type##s[i].type)				\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, (_drm).__res->type##s[i],	\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(drm, res, crtc, Crtc);
	get_resource(drm, res, encoder, Encoder);
	get_resource(drm, res, connector, Connector);
	get_resource(drm, res, fb, FB);

#define get_properties(_drm, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_drm).__res->count_##type##s; ++i) {	\
			struct type *obj = &_drm.type##s[i];			\
			unsigned int j;						\
			obj->props =						\
				drmModeObjectGetProperties(_drm.fd, obj->type->type##_id, \
							   DRM_MODE_OBJECT_##Type); \
			if (!obj->props) {					\
				fprintf(stderr,					\
					"could not get %s %i properties: %s\n", \
					#type, obj->type->type##_id,		\
					strerror(errno));			\
				continue;					\
			}							\
			obj->props_info = calloc(obj->props->count_props,	\
						 sizeof(*obj->props_info));	\
			if (!obj->props_info)					\
				continue;					\
			for (j = 0; j < obj->props->count_props; ++j)		\
				obj->props_info[j] =				\
					drmModeGetProperty(_drm.fd, obj->props->props[j]); \
		}								\
	} while (0)

	get_properties(drm, res, crtc, CRTC);
	get_properties(drm, res, connector, CONNECTOR);

	for (i = 0; i < drm.res->count_crtcs; ++i)
		drm.crtcs[i].mode = &drm.crtcs[i].crtc->mode;

	drm.plane_res = drmModeGetPlaneResources(drm.fd);
	if (!drm.plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
			strerror(errno));
		return 0;
	}

	drm.planes = calloc(drm.plane_res->count_planes, sizeof(*drm.planes));
	if (!drm.planes)
		goto error;

	get_resource(drm, plane_res, plane, Plane);
	get_properties(drm, plane_res, plane, PLANE);

	return prepare_connector();

error:
	free_drm();
	return -1;
}

static int init_gbm(void)
{
	gbm.dev = gbm_create_device(drm.fd);
	if (!gbm.dev) {
		printf("failed to create gbm device\n");
		return -1;
	}

	return 0;
}

static int init_gl(void)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;

	static const GLfloat vVertices[] = {
			// front
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f, // point magenta
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			// back
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, +1.0f, -1.0f, // point yellow
			-1.0f, +1.0f, -1.0f, // point green
			// right
			+1.0f, -1.0f, +1.0f, // point magenta
			+1.0f, -1.0f, -1.0f, // point red
			+1.0f, +1.0f, +1.0f, // point white
			+1.0f, +1.0f, -1.0f, // point yellow
			// left
			-1.0f, -1.0f, -1.0f, // point black
			-1.0f, -1.0f, +1.0f, // point blue
			-1.0f, +1.0f, -1.0f, // point green
			-1.0f, +1.0f, +1.0f, // point cyan
			// top
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			-1.0f, +1.0f, -1.0f, // point green
			+1.0f, +1.0f, -1.0f, // point yellow
			// bottom
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f  // point magenta
	};

	static const GLfloat vColors[] = {
			// front
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f, // magenta
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			// back
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  0.0f, // black
			1.0f,  1.0f,  0.0f, // yellow
			0.0f,  1.0f,  0.0f, // green
			// right
			1.0f,  0.0f,  1.0f, // magenta
			1.0f,  0.0f,  0.0f, // red
			1.0f,  1.0f,  1.0f, // white
			1.0f,  1.0f,  0.0f, // yellow
			// left
			0.0f,  0.0f,  0.0f, // black
			0.0f,  0.0f,  1.0f, // blue
			0.0f,  1.0f,  0.0f, // green
			0.0f,  1.0f,  1.0f, // cyan
			// top
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			0.0f,  1.0f,  0.0f, // green
			1.0f,  1.0f,  0.0f, // yellow
			// bottom
			0.0f,  0.0f,  0.0f, // black
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f  // magenta
	};

	static const GLfloat vNormals[] = {
			// front
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			// back
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			// right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			// left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			// top
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			// bottom
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f  // down
	};

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	static const char *vertex_shader_source =
			"uniform mat4 modelviewMatrix;      \n"
			"uniform mat4 modelviewprojectionMatrix;\n"
			"uniform mat3 normalMatrix;         \n"
			"                                   \n"
			"attribute vec4 in_position;        \n"
			"attribute vec3 in_normal;          \n"
			"attribute vec4 in_color;           \n"
			"\n"
			"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_Position = modelviewprojectionMatrix * in_position;\n"
			"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
			"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
			"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
			"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
			"    float diff = max(0.0, dot(vEyeNormal, vLightDir));\n"
			"    vVaryingColor = vec4(diff * in_color.rgb, 1.0);\n"
			"}                                  \n";

	static const char *fragment_shader_source =
			"precision mediump float;           \n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_FragColor = vVaryingColor;  \n"
			"}                                  \n";

	gl.display = eglGetDisplay(gbm.dev);

	if (!eglInitialize(gl.display, &major, &minor)) {
		printf("failed to initialize\n");
		return -1;
	}

#define get_proc(name) do { \
		gl.name = (void *)eglGetProcAddress(#name); \
		assert(gl.name); \
	} while (0)

	get_proc(glEGLImageTargetRenderbufferStorageOES);
	get_proc(eglCreateImageKHR);
	get_proc(eglCreateSyncKHR);
	get_proc(eglDestroySyncKHR);
	get_proc(eglWaitSyncKHR);
	get_proc(eglClientWaitSyncKHR);
	get_proc(eglDupNativeFenceFDANDROID);

	printf("Using display %p with EGL version %d.%d\n",
			gl.display, major, minor);

	printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
	printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
	printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("failed to bind api EGL_OPENGL_ES_API\n");
		return -1;
	}

	if (!eglChooseConfig(gl.display, config_attribs, &gl.config, 1, &n) || n != 1) {
		printf("failed to choose config: %d\n", n);
		return -1;
	}

	gl.context = eglCreateContext(gl.display, gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (gl.context == NULL) {
		printf("failed to create context\n");
		return -1;
	}

	eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl.context);

	glGenFramebuffers(1, &gl.fb);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.fb);

	vertex_shader = glCreateShader(GL_VERTEX_SHADER);

	glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertex_shader);

	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("vertex shader compilation failed!:\n");
		glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &ret);
		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(vertex_shader, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragment_shader);

	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("fragment shader compilation failed!:\n");
		glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(fragment_shader, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	gl.program = glCreateProgram();

	glAttachShader(gl.program, vertex_shader);
	glAttachShader(gl.program, fragment_shader);

	glBindAttribLocation(gl.program, 0, "in_position");
	glBindAttribLocation(gl.program, 1, "in_normal");
	glBindAttribLocation(gl.program, 2, "in_color");

	glLinkProgram(gl.program);

	glGetProgramiv(gl.program, GL_LINK_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("program linking failed!:\n");
		glGetProgramiv(gl.program, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetProgramInfoLog(gl.program, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	glUseProgram(gl.program);

	gl.modelviewmatrix = glGetUniformLocation(gl.program, "modelviewMatrix");
	gl.modelviewprojectionmatrix = glGetUniformLocation(gl.program, "modelviewprojectionMatrix");
	gl.normalmatrix = glGetUniformLocation(gl.program, "normalMatrix");

	glViewport(0, 0, drm.mode->hdisplay, drm.mode->vdisplay);
	glEnable(GL_CULL_FACE);

	gl.positionsoffset = 0;
	gl.colorsoffset = sizeof(vVertices);
	gl.normalsoffset = sizeof(vVertices) + sizeof(vColors);
	glGenBuffers(1, &gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices) + sizeof(vColors) + sizeof(vNormals), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, gl.positionsoffset, sizeof(vVertices), &vVertices[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.colorsoffset, sizeof(vColors), &vColors[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.normalsoffset, sizeof(vNormals), &vNormals[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)(uintptr_t) gl.positionsoffset);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)(uintptr_t) gl.normalsoffset);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)(uintptr_t) gl.colorsoffset);
	glEnableVertexAttribArray(2);

	return 0;
}

static void init_frames(void)
{
	const int width = drm.mode->hdisplay;
	const int height = drm.mode->vdisplay;

	for (int i = 0; i < ARRAY_SIZE(frames); ++i) {
		struct frame *frame = &frames[i];

		frame->gbm_bo = gbm_bo_create(gbm.dev, width, height,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (!frame->gbm_bo) {
			printf("failed to create gbm_bo\n");
			exit(EXIT_FAILURE);
		}

		int gbm_bo_fd = gbm_bo_get_fd(frame->gbm_bo);
		if (gbm_bo_fd == -1) {
			printf("gbm_bo_get_fd() failed\n");
			exit(EXIT_FAILURE);
		}

		const EGLint image_attrs[] = {
			EGL_WIDTH,			width,
			EGL_HEIGHT,			height,
			EGL_LINUX_DRM_FOURCC_EXT,	DRM_FORMAT_XRGB8888,
			EGL_DMA_BUF_PLANE0_FD_EXT,	gbm_bo_fd,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,	gbm_bo_get_stride(frame->gbm_bo),
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,	0,
			EGL_NONE,
		};

		frame->egl_image = gl.eglCreateImageKHR(gl.display,
			EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
			(EGLClientBuffer) NULL, image_attrs);
		if (!frame->egl_image) {
			printf("failed to create EGLImage from gbm_bo\n");
			exit(EXIT_FAILURE);
		}

		glGenRenderbuffers(1, &frame->gl_renderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, frame->gl_renderbuffer);
		gl.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
							  frame->egl_image);
		if (glGetError() != GL_NO_ERROR) {
			printf("failed to create GL renderbuffer from EGLImage\n");
			exit(EXIT_FAILURE);
		}
	}
}

static void draw(uint32_t i)
{
	ESMatrix modelview;

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -8.0f);
	esRotate(&modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

	GLfloat aspect = (GLfloat)(drm.mode->vdisplay) / (GLfloat)(drm.mode->hdisplay);

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 6.0f, 10.0f);

	ESMatrix modelviewprojection;
	esMatrixLoadIdentity(&modelviewprojection);
	esMatrixMultiply(&modelviewprojection, &modelview, &projection);

	float normal[9];
	normal[0] = modelview.m[0][0];
	normal[1] = modelview.m[0][1];
	normal[2] = modelview.m[0][2];
	normal[3] = modelview.m[1][0];
	normal[4] = modelview.m[1][1];
	normal[5] = modelview.m[1][2];
	normal[6] = modelview.m[2][0];
	normal[7] = modelview.m[2][1];
	normal[8] = modelview.m[2][2];

	glUniformMatrix4fv(gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniformMatrix3fv(gl.normalmatrix, 1, GL_FALSE, normal);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static int add_connector_property(drmModeAtomicReq *req, uint32_t obj_id,
					const char *name, uint64_t value)
{
	struct connector *obj = NULL;
	unsigned int i;
	int prop_id = 0;

	for (i = 0; i < (unsigned int) drm.res->count_connectors ;
			i++) {
		if (obj_id == drm.res->connectors[i]) {
			obj = &drm.connectors[i];
			break;
		}
	}

	if (!obj)
		return -EINVAL;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0)
		return -EINVAL;

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct crtc *obj = NULL;
	unsigned int i;
	int prop_id = -1;

	for (i = 0; i < (unsigned int) drm.res->count_crtcs ; i++) {
		if (obj_id == drm.res->crtcs[i]) {
			obj = &drm.crtcs[i];
			break;
		}
	}

	if (!obj)
		return -EINVAL;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0)
		return -EINVAL;

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_plane_property(drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct plane *obj = NULL;
	unsigned int i;
	int prop_id = -1;

	for (i = 0; i < (unsigned int) drm.plane_res->count_planes ; i++) {
		if (obj_id == drm.plane_res->planes[i]) {
			obj = &drm.planes[i];
			break;
		}
	}

	if (!obj)
		return -EINVAL;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0)
		return -EINVAL;

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}


static int get_primary_plane_id()
{
	struct plane *obj = NULL;
	unsigned int i, j;

	for (i = 0; i < (unsigned int) drm.plane_res->count_planes ; i++) {
		obj = &drm.planes[i];
		if (!(obj->plane->possible_crtcs & (1 << get_crtc_index(drm.crtc_id))))
			continue;
		for (j = 0 ; j < obj->props->count_props ; j++) {
			if (strcmp(obj->props_info[j]->name, "type") != 0)
				continue;

			if (obj->props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
				return drm.plane_res->planes[i];
		}
	}

	return -EINVAL;
}

static int drm_atomic_commit(uint32_t fb_id, uint32_t flags,
			     int64_t in_fence_fd, int64_t *out_fence_fd)
{
	drmModeAtomicReq *req;
	uint32_t blob_id;
	int plane_id, ret;

	req = drmModeAtomicAlloc();

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		if (add_connector_property(req, drm.connector_id, "CRTC_ID",
						drm.crtc_id) < 0)
				return -1;

		if (drmModeCreatePropertyBlob(drm.fd, drm.mode, sizeof(*drm.mode),
					      &blob_id) != 0)
			return -1;

		if (add_crtc_property(req, drm.crtc_id, "MODE_ID", blob_id) < 0)
			return -1;

		if (add_crtc_property(req, drm.crtc_id, "ACTIVE", 1) < 0)
			return -1;
	}

	plane_id = get_primary_plane_id();
	if (plane_id < 0)
		return -1;

	add_plane_property(req, plane_id, "FB_ID", fb_id);
	add_plane_property(req, plane_id, "CRTC_ID", drm.crtc_id);
	add_plane_property(req, plane_id, "SRC_X", 0);
	add_plane_property(req, plane_id, "SRC_Y", 0);
	add_plane_property(req, plane_id, "SRC_W", drm.mode->hdisplay << 16);
	add_plane_property(req, plane_id, "SRC_H", drm.mode->vdisplay << 16);
	add_plane_property(req, plane_id, "CRTC_X", 0);
	add_plane_property(req, plane_id, "CRTC_Y", 0);
	add_plane_property(req, plane_id, "CRTC_W", drm.mode->hdisplay);
	add_plane_property(req, plane_id, "CRTC_H", drm.mode->vdisplay);

	if (in_fence_fd != -1)
		add_plane_property(req, plane_id, "FENCE_FD", in_fence_fd);

	if (out_fence_fd != NULL) {
		add_crtc_property(req, drm.crtc_id, "OUT_FENCE_PTR",
				  (uintptr_t) out_fence_fd);
	}

	printf("--- FLAGS: 0x%x\n", flags);
	ret = drmModeAtomicCommit(drm.fd, req, flags, NULL);
	if (ret)
		goto out;

	drm.req = req;

	return 0;

out:
	drmModeAtomicFree(req);

	return ret;
}

static EGLSyncKHR create_fence(int fd)
{
	EGLint attrib_list[] = {
		EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
		EGL_NONE,
	};
	EGLSyncKHR fence = gl.eglCreateSyncKHR(gl.display,
			EGL_SYNC_NATIVE_FENCE_ANDROID, attrib_list);
	assert(fence);
	return fence;
}

static void
print_help(void)
{
	printf("usage: kmscube [-h|--help] [-f|--frames <frames>]\n");
}

static void
parse_args(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "help",       no_argument,            NULL, 'h' },
		{ "frames",     required_argument,      NULL, 'f' },
		{ 0 },
	};

	char *endptr;
	int opt;
	int longindex = 0;

	/* Suppress getopt's poor error messages */
	opterr = 0;

	while ((opt = getopt_long(argc, argv, "+:hf:", longopts,
				 /*longindex*/ &longindex)) != -1) {
		switch (opt) {
		case 'h':
			print_help();
			exit(0);
			break;
		case 'f':
			errno = 0;
			arg_frames = strtoul(optarg, &endptr, 0);
			if (errno || *endptr != '\0') {
				fprintf(stderr, "usage error: invalid value for <frames>\n");
				exit(EXIT_FAILURE);
			}
			break;
		case ':':
			fprintf(stderr, "usage error: %s requires an argument\n", argv[optind - 1]);
			exit(EXIT_FAILURE);
			break;
		case '?':
		default:
			assert(opt == '?');
			fprintf(stderr, "usage error: unknown option '%s'\n", argv[optind - 1]);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "usage error: trailing args\n");
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char *argv[])
{
	struct drm_fb *fb;
	int ret;

	parse_args(argc, argv);

	ret = init_drm();
	if (ret) {
		printf("failed to initialize DRM\n");
		return ret;
	}

	ret = init_gbm();
	if (ret) {
		printf("failed to initialize GBM\n");
		return ret;
	}

	ret = init_gl();
	if (ret) {
		printf("failed to initialize EGL\n");
		return ret;
	}

	init_frames();

	/* clear the color buffer */
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, frames[0].gl_renderbuffer);
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	fb = drm_fb_get_from_bo(frames[0].gbm_bo);

	/* set mode: */
	ret = drm_atomic_commit(fb->fb_id, DRM_MODE_ATOMIC_ALLOW_MODESET, -1, NULL);
	if (ret) {
		printf("failed to commit modeset: %s\n", strerror(errno));
		return ret;
	}

	/* We use int64_t for fds instead of int to workaround a bug in the
	 * kernel's API for OUT_FENCE_PTR, as of Linux 4.10-rc2. The
	 * OUT_FENCE_PTR property must point to an int64_t.
	 */
	int64_t gpu_fence_fd = -1; /* out-fence from gpu, in-fence to kms */
	int64_t kms_fence_fd = -1; /* in-fence to gpu, out-fence from kms */

	for (uint64_t i = 1; arg_frames == 0 || i < arg_frames; ++i) {
		struct frame *frame = &frames[i % ARRAY_SIZE(frames)];

		EGLSyncKHR gpu_fence = NULL;   /* out-fence from gpu, in-fence to kms */
		EGLSyncKHR kms_fence = NULL;   /* in-fence to gpu, out-fence from kms */

		if (kms_fence_fd != -1) {
			kms_fence = create_fence(kms_fence_fd);

			/* driver now has ownership of the fence fd: */
			kms_fence_fd = -1;

			/* wait "on the gpu" (ie. this won't necessarily block, but
			 * will block the rendering until fence is signaled), until
			 * the previous pageflip completes so we don't render into
			 * the buffer that is still on screen.
			 */
			/* gl.eglWaitSyncKHR(gl.display, kms_fence, 0); */
			gl.eglClientWaitSyncKHR(gl.display, kms_fence,
						/*flags*/ EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
						/*timeout*/ EGL_FOREVER_KHR);
			gl.eglDestroySyncKHR(gl.display, kms_fence);
		}

		draw(i);

		/* insert fence to be singled in cmdstream.. this fence will be
		 * signaled when gpu rendering done
		 */
		gpu_fence = create_fence(EGL_NO_NATIVE_FENCE_FD_ANDROID);
		gpu_fence_fd = gl.eglDupNativeFenceFDANDROID(gl.display, gpu_fence);
		gl.eglDestroySyncKHR(gl.display, gpu_fence);
		assert(gpu_fence_fd != -1);

		fb = drm_fb_get_from_bo(frame->gbm_bo);

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */
		ret = drm_atomic_commit(fb->fb_id, DRM_MODE_ATOMIC_NONBLOCK,
					gpu_fence_fd, &kms_fence_fd);
		printf("commit: gpu_fence_fd=%ld, kms_fence_fd=%ld, ret=%d\n",
				gpu_fence_fd, kms_fence_fd, ret);
		if (ret) {
			printf("failed to commit: %s\n", strerror(errno));
			return -1;
		}

		if (gpu_fence_fd != -1) {
			close(gpu_fence_fd);
			gpu_fence_fd = -1;
		}
	}

	return ret;
}
