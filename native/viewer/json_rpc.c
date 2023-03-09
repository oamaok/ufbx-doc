#include "json_rpc.h"
#include "external/json_input.h"
#include "external/json_output.h"
#include "external/cputime.h"
#include "arena.h"
#include "viewer.h"
#include "serialization.h"
#include "ufbx.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
	#define NOMINMAX
	#include <Windows.h>
#endif

static bool g_pretty = false;
static bool g_verbose = false;
static uint64_t g_start_cpu_tick = 0;

void log_printf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
#if defined(_WIN32)
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, args);
	OutputDebugStringA(buf);

	va_list args2;
	va_copy(args2, args);
	vfprintf(stderr, fmt, args2);
	va_end(args2);
#else
	vfprintf(stderr, fmt, args);
#endif
	va_end(args);
}

jso_stream begin_response()
{
	uint64_t cpu_tick = cputime_cpu_tick();
	cputime_end_init();
	jso_stream s;
	jso_init_growable(&s);
	s.pretty = g_pretty;
	jso_object(&s);
	jso_single_line(&s);
	jso_prop_object(&s, "rpc");
	double sec = cputime_cpu_delta_to_sec(NULL, cpu_tick - g_start_cpu_tick);
	jso_prop_double(&s, "duration", sec);
	jso_end_object(&s);
	return s;
}

char *end_response(jso_stream *s)
{
	jso_end_object(s);
	return jso_close_growable(s);
}

char *fmt_error(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char buf[512];
	int len = vsnprintf(buf, sizeof(buf), fmt, args);
	if (len < 0) len = 0;
	va_end(args);

	jso_stream s = begin_response();
	jso_prop_string_len(&s, "error", buf, len);
	return end_response(&s);
}

enum {
	MAX_NAME_LEN = 64,
	MAX_LODAED_SCENES = 16,
};

typedef struct {
	arena_t *arena;
	const char *name;
	ufbx_scene *fbx_scene;
	vi_scene *vi_scene;
} rpc_scene;

typedef alist_t(rpc_scene) rpc_scene_list;

typedef struct {
	rpc_scene_list scenes;
	void *pixel_buffer;
} rpc_globals;

static rpc_globals rpcg;

char *rpc_cmd_init(arena_t *tmp, jsi_obj *args)
{
	g_pretty = jsi_get_bool(args, "pretty", g_pretty);
	g_verbose = jsi_get_bool(args, "verbose", g_verbose);

	vi_setup();

	jso_stream s = begin_response();
	jso_prop_boolean(&s, "pretty", g_pretty);
	jso_prop_boolean(&s, "verbose", g_verbose);
	return end_response(&s);
}

char *rpc_cmd_load_scene(arena_t *tmp, jsi_obj *args)
{
	const char *name = jsi_get_str(args, "name", NULL);
	if (!name) return fmt_error("Missing field: 'name'");
	const void *data = (const void*)jsi_get_int64(args, "dataPointer", 0);
	size_t size = (size_t)jsi_get_int64(args, "size", 0);
	if (!data || !size) return fmt_error("Bad data range: { %p, %zu }", data, size);

	rpc_scene *scene = NULL;
	for (size_t i = 0; i < rpcg.scenes.count; i++) {
		if (!strcmp(rpcg.scenes.data[i].name, name)) {
			scene = &rpcg.scenes.data[i];
			break;
		}
	}
	if (!scene) {
		scene = alist_push(NULL, rpc_scene, &rpcg.scenes);
		scene->arena = arena_create(NULL);
		scene->name = aalloc_copy_str(scene->arena, name);
	}

	ufbx_load_opts opts = {
		.allow_null_material = true,
		.target_axes = ufbx_axes_right_handed_y_up,
		.target_unit_meters = 1.0f,
	};
	ufbx_error error;
	ufbx_scene *fbx_scene = ufbx_load_memory(data, size, &opts, &error);
	if (!fbx_scene) {
		char *buf = aalloc(tmp, char, 4096);
		ufbx_format_error(buf, sizeof(buf), &error);
		return fmt_error("Failed to load scene:\n%s", buf);
	}

	scene->fbx_scene = fbx_scene;

	jso_stream s = begin_response();
	jso_prop(&s, "scene");
	serialize_scene(&s, fbx_scene);
	return end_response(&s);
}

static um_vec3 get_vec3(jsi_obj *parent, const char *name, um_vec3 def)
{
	jsi_obj *obj = jsi_get_obj(parent, name);
	if (obj) {
		return um_v3(
			(float)jsi_get_double(obj, "x", def.x),
			(float)jsi_get_double(obj, "y", def.y),
			(float)jsi_get_double(obj, "z", def.z));
	} else {
		return def;
	}
}

static rpc_scene *find_scene(const char *name)
{
	for (size_t i = 0; i < rpcg.scenes.count; i++) {
		if (!strcmp(rpcg.scenes.data[i].name, name)) {
			return &rpcg.scenes.data[i];
		}
	}
	return NULL;
}

char *rpc_cmd_render(arena_t *tmp, jsi_obj *args)
{
	jsi_obj *target = jsi_get_obj(args, "target");
	jsi_obj *desc = jsi_get_obj(args, "desc");
	if (!target) return fmt_error("Missing field: 'target'");
	if (!desc) return fmt_error("Missing field: 'desc'");

	vi_target vtarget = {
		.target_index = (uint32_t)jsi_get_int(target, "targetIndex", 0),
		.width = (uint32_t)jsi_get_int(target, "width", 256),
		.height = (uint32_t)jsi_get_int(target, "height", 256),
		.samples = (uint32_t)jsi_get_int(target, "samples", 1),
		.pixel_scale = (float)jsi_get_double(target, "pixelScale", 1.0),
	};

	vi_setup();

	const char *name = jsi_get_str(desc, "sceneName", NULL);
	if (!name) return fmt_error("Missing field: 'name'");
	rpc_scene *scene = find_scene(name);
	if (!scene) return fmt_error("Scene not found: '%s'", name);

	if (!scene->vi_scene) {
		scene->vi_scene = vi_make_scene(scene->fbx_scene);
	}

	ufbx_prop_override_desc *overrides = NULL;
	size_t num_overrides = 0;
	jsi_arr *js_overrides = jsi_get_arr(desc, "overrides");
	if (js_overrides) {
		num_overrides = js_overrides->num_values;
		overrides = aalloc(tmp, ufbx_prop_override_desc, num_overrides);

		for (size_t i = 0; i < num_overrides; i++) {
			jsi_obj *obj = jsi_as_obj(&js_overrides->values[i]);
			jsi_value *val = jsi_get(obj, "value");
			if (!obj || !val) continue;

			overrides[i].element_id = (uint32_t)jsi_get_int(obj, "elementId", 0);
			overrides[i].prop_name.data = jsi_get_str(obj, "name", 0);
			overrides[i].prop_name.length = SIZE_MAX;
			if (val->type == jsi_type_array) {
				for (size_t ci = 0; ci < 3; ci++) {
					if (ci < val->array->num_values) {
						overrides[i].value.v[ci] = jsi_as_double(&val->array->values[ci], 0.0);
					}
				}
			} else if (val->type == jsi_type_number) {
				overrides[i].value.x = jsi_as_double(val, 0.0);
			}
		}
	}

	jsi_obj *camera = jsi_get_obj(desc, "camera");
	jsi_obj *animation = jsi_get_obj(desc, "animation");
	vi_desc vdesc = {
		.camera_pos = get_vec3(camera, "position", um_v3(4.0f, 4.0f, 4.0f)),
		.camera_target = get_vec3(camera, "target", um_zero3),
		.field_of_view = (float)jsi_get_double(camera, "fieldOfView", 50.0f),
		.near_plane = (float)jsi_get_double(camera, "nearPlane", 0.01f),
		.far_plane = (float)jsi_get_double(camera, "farPlane", 100.0f),
		.selected_element_id = (uint32_t)jsi_get_int(desc, "selectedElement", -1),
		.highlight_vertex_index = (uint32_t)jsi_get_int(desc, "highlightVertexIndex", -1),
		.highlight_face_index = (uint32_t)jsi_get_int(desc, "highlightFaceIndex", -1),
		.time = jsi_get_double(animation, "time", 0.0),
		.overrides = overrides,
		.num_overrides = num_overrides,
	};

	vi_render(scene->vi_scene, &vtarget, &vdesc);

	jso_stream s = begin_response();
	return end_response(&s);
}

char *rpc_cmd_present(arena_t *tmp, jsi_obj *args)
{
	uint32_t target = (uint32_t)jsi_get_int(args, "targetIndex", 0);
	uint32_t width = (uint32_t)jsi_get_int(args, "width", 0);
	uint32_t height = (uint32_t)jsi_get_int(args, "height", 0);

	vi_setup();

	vi_present(target, width, height);

	jso_stream s = begin_response();
	return end_response(&s);
}

char *rpc_cmd_get_pixels(arena_t *tmp, jsi_obj *args)
{
	uint32_t target = (uint32_t)jsi_get_int(args, "targetIndex", 0);
	uint32_t width = (uint32_t)jsi_get_int(args, "width", 0);
	uint32_t height = (uint32_t)jsi_get_int(args, "height", 0);

	size_t required_size = (size_t)width * (size_t)height * 4;
	size_t capacity = aalloc_capacity_bytes(rpcg.pixel_buffer);
	if (capacity < required_size) {
		afree(NULL, rpcg.pixel_buffer);
		rpcg.pixel_buffer = aalloc(NULL, char, required_size);
	}

	vi_setup();

	if (!vi_get_pixels(target, width, height, rpcg.pixel_buffer)) {
		return fmt_error("Failed to get pixels");
	}

	jso_stream s = begin_response();
	jso_prop_int64(&s, "dataPointer", (int64_t)(uintptr_t)rpcg.pixel_buffer);
	return end_response(&s);
}

char *rpc_cmd_free_resources(arena_t *tmp, jsi_obj *args)
{
	bool scenes = jsi_get_bool(args, "scenes", false);
	bool targets = jsi_get_bool(args, "targets", false);
	bool globals = jsi_get_bool(args, "globals", false);

	if (scenes) {
		for (size_t i = 0; i < rpcg.scenes.count; i++) {
			rpc_scene *scene = &rpcg.scenes.data[i];
			vi_free_scene(scene->vi_scene);
			scene->vi_scene = NULL;
		}
	}

	if (targets) {
		vi_free_targets();
	}

	if (globals) {
		vi_shutdown();
	}

	jso_stream s = begin_response();
	return end_response(&s);
}

char *rpc_cmd_get_vertex(arena_t *tmp, jsi_obj *args)
{
	const char *scene_name = jsi_get_str(args, "sceneName", NULL);
	size_t element_id = (size_t)jsi_get_int(args, "elementId", SIZE_MAX);
	size_t index = (size_t)jsi_get_int(args, "index", SIZE_MAX);

	const char *name = jsi_get_str(args, "sceneName", NULL);
	if (!name) return fmt_error("Missing field: 'name'");
	rpc_scene *scene = find_scene(name);
	if (!scene) return fmt_error("Scene not found: '%s'", name);
	if (!scene->fbx_scene) return fmt_error("Scene not loaded");

	ufbx_scene *fbx_scene = scene->fbx_scene;
	if (element_id >= fbx_scene->elements.count) return fmt_error("Bad element id: %zu", element_id);
	ufbx_element *element = fbx_scene->elements.data[element_id];
	if (element->type != UFBX_ELEMENT_MESH) return fmt_error("Element is not a mesh");
	ufbx_mesh *mesh = (ufbx_mesh*)element;
	if (index >= mesh->num_indices) return fmt_error("Index out of bounds: %zu", index);

	jso_stream s = begin_response();

	jso_prop_int(&s, "vertexIndex", (int)mesh->vertex_indices.data[index]);

	jso_prop_vec3(&s, "position", ufbx_get_vertex_vec3(&mesh->vertex_position, index));
	if (mesh->vertex_normal.exists) {
		jso_prop_vec3(&s, "normal", ufbx_get_vertex_vec3(&mesh->vertex_normal, index));
	}
	if (mesh->vertex_uv.exists) {
		jso_prop_vec2(&s, "uv", ufbx_get_vertex_vec2(&mesh->vertex_uv, index));
	}

	uint32_t face_ix = ufbx_find_face_index(mesh, index);
	jso_prop_int(&s, "face", (int)face_ix);

	return end_response(&s);
}

char *rpc_handle(arena_t *tmp, jsi_value *value)
{
	jsi_obj *obj = jsi_as_obj(value);
	if (!obj) return fmt_error("Expected a top-level object");

	const char *cmd = jsi_get_str(obj, "cmd", "(missing)");
	if (!strcmp(cmd, "init")) {
		return rpc_cmd_init(tmp, obj);
	} else if (!strcmp(cmd, "loadScene")) {
		return rpc_cmd_load_scene(tmp, obj);
	} else if (!strcmp(cmd, "render")) {
		return rpc_cmd_render(tmp, obj);
	} else if (!strcmp(cmd, "present")) {
		return rpc_cmd_present(tmp, obj);
	} else if (!strcmp(cmd, "getPixels")) {
		return rpc_cmd_get_pixels(tmp, obj);
	} else if (!strcmp(cmd, "freeResources")) {
		return rpc_cmd_free_resources(tmp, obj);
	} else if (!strcmp(cmd, "getVertex")) {
		return rpc_cmd_get_vertex(tmp, obj);
	} else {
		return fmt_error("Unknown cmd: '%s'\n", cmd);
	}
}

char *rpc_call(char *input)
{
	if (g_verbose) {
		log_printf("RPC request: %s\n", input);
	}

	jsi_args args = {
		.store_integers_as_int64 = true,
	};
	jsi_value *value = jsi_parse_string(input, &args);
	free(input);

	cputime_begin_init();
	g_start_cpu_tick = cputime_cpu_tick();

	if (!value) {
		return fmt_error("Failed to parse JSON: %zu:%zu: %s",
			args.error.line, args.error.column, args.error.description);
	}

	arena_t tmp;
	arena_init(&tmp, NULL);
	char *result = rpc_handle(&tmp, value);
	arena_free(&tmp);

	if (g_verbose) {
		log_printf("RPC response: %s\n", result);
	}

	jsi_free(value);
	return result;
}
