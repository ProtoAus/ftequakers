//This is my attempt at wayland support for both opengl and vulkan.
//Note that this is sorely under-tested - I haven't tested vulkan-on-wayland at all as none of the drivers for nvidia support it.

//in no particular order...
//TODO: leaks on shutdown
//TODO: primary selection (zwp_primary_selection_device_manager_v1), so CBT_SELECTION stays internal
//TODO: drag+drop

/*
kwin unstable protocools:
 zwp_relative_pointer_manager_v1, since 5.28
 zwp_pointer_constraints_v1, since 5.29
 xdg_wm_base, since 5.48
 zxdg_decoration_manager_v1, since 5.54
*/

#include "bothdefs.h"
#ifdef WAYLANDQUAKE
#include "gl_videgl.h"	//define this BEFORE the wayland stuff. This means the EGL types will have their (x11) defaults instead of getting mixed up with wayland. we expect to be able to use the void* verions instead for wayland anyway.
#include <wayland-client.h>
#include <wayland-egl.h>
#include <linux/input.h>	//this is shite.
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#if defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>	//getrandom: glibc 2.25+, musl 1.1.20+
#define HAVE_GETRANDOM
#endif
#endif
#include <xkbcommon/xkbcommon.h>

#include "quakedef.h"
#if defined(GLQUAKE) && defined(USE_EGL)
#include "gl_draw.h"
#endif
#if defined(VKQUAKE)
#include "vk/vkrenderer.h"
#endif

#if WAYLAND_VERSION_MAJOR < 1
#error "wayland headers are too old"
#endif

#include "glquake.h"
#include "shader.h"

extern cvar_t vid_conautoscale;

//protocol names...
#define WP_POINTER_CONSTRAINTS_NAME "zwp_pointer_constraints_v1"
#define WP_RELATIVE_POINTER_MANAGER_NAME "zwp_relative_pointer_manager_v1"
#define ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME "org_kde_kwin_server_decoration_manager"	//used only when the XDG one is missing
#define XDG_DECORATION_MANAGER_NAME "zxdg_decoration_manager_v1"
#define WP_FRACTIONAL_SCALE_MANAGER_NAME "wp_fractional_scale_manager_v1"
#define WP_VIEWPORTER_NAME "wp_viewporter"
#define WP_TEARING_CONTROL_MANAGER_NAME "wp_tearing_control_manager_v1"
#define WP_CURSOR_SHAPE_MANAGER_NAME "wp_cursor_shape_manager_v1"

//#define XDG_SHELL_UNSTABLE
#ifdef XDG_SHELL_UNSTABLE
#define XDG_SHELL_NAME "zxdg_shell_v6"
#else
#define XDG_SHELL_NAME "xdg_wm_base"
#endif
#define WL_SHELL_NAME "wl_shell"	//fall back on this if xdg_shell is missing

#ifndef STATIC_WAYLAND
	#define DYNAMIC_WAYLAND
#endif
#ifdef DYNAMIC_WAYLAND

static struct wl_display *(*pwl_display_connect)(const char *name);
static void (*pwl_display_disconnect)(struct wl_display *display);
static int (*pwl_display_dispatch)(struct wl_display *display);
static int (*pwl_display_dispatch_pending)(struct wl_display *display);
static int (*pwl_display_roundtrip)(struct wl_display *display);
static int (*pwl_display_flush)(struct wl_display *display);
static int (*pwl_display_prepare_read)(struct wl_display *display);
static int (*pwl_display_read_events)(struct wl_display *display);
static void (*pwl_display_cancel_read)(struct wl_display *display);
static int (*pwl_display_get_fd)(struct wl_display *display);

static struct wl_proxy *(*pwl_proxy_marshal_constructor)(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, ...);
static struct wl_proxy *(*pwl_proxy_marshal_constructor_versioned)(struct wl_proxy *proxy, uint32_t opcode, const struct wl_interface *interface, uint32_t version, ...);
static void (*pwl_proxy_destroy)(struct wl_proxy *proxy);
static void (*pwl_proxy_marshal)(struct wl_proxy *p, uint32_t opcode, ...);
static int (*pwl_proxy_add_listener)(struct wl_proxy *proxy, void (**implementation)(void), void *data);

static const struct wl_interface		*pwl_keyboard_interface;
static const struct wl_interface		*pwl_pointer_interface;
static const struct wl_interface		*pwl_compositor_interface;
static const struct wl_interface		*pwl_region_interface;
static const struct wl_interface		*pwl_surface_interface;
#ifdef WL_SHELL_NAME
static const struct wl_interface		*pwl_shell_surface_interface;
static const struct wl_interface		*pwl_shell_interface;
#endif
static const struct wl_interface		*pwl_seat_interface;
static const struct wl_interface		*pwl_registry_interface;
static const struct wl_interface		*pwl_output_interface;
static const struct wl_interface		*pwl_shm_interface;
static const struct wl_interface		*pwl_shm_pool_interface;
static const struct wl_interface		*pwl_buffer_interface;
static const struct wl_interface		*pwl_callback_interface;
static const struct wl_interface		*pwl_data_device_manager_interface;
static const struct wl_interface		*pwl_data_device_interface;
static const struct wl_interface		*pwl_data_source_interface;

static dllfunction_t waylandexports_wl[] =
{
	{(void**)&pwl_display_connect,						"wl_display_connect"},
	{(void**)&pwl_display_disconnect,					"wl_display_disconnect"},
	{(void**)&pwl_display_dispatch,						"wl_display_dispatch"},
	{(void**)&pwl_display_dispatch_pending,				"wl_display_dispatch_pending"},
	{(void**)&pwl_display_roundtrip,					"wl_display_roundtrip"},
	{(void**)&pwl_display_flush,						"wl_display_flush"},
	{(void**)&pwl_display_prepare_read,				"wl_display_prepare_read"},
	{(void**)&pwl_display_read_events,				"wl_display_read_events"},
	{(void**)&pwl_display_cancel_read,				"wl_display_cancel_read"},
	{(void**)&pwl_display_get_fd,					"wl_display_get_fd"},
	{(void**)&pwl_proxy_marshal_constructor,			"wl_proxy_marshal_constructor"},
	{(void**)&pwl_proxy_marshal_constructor_versioned,	"wl_proxy_marshal_constructor_versioned"},
	{(void**)&pwl_proxy_destroy,						"wl_proxy_destroy"},
	{(void**)&pwl_proxy_marshal,						"wl_proxy_marshal"},
	{(void**)&pwl_proxy_add_listener,					"wl_proxy_add_listener"},
	{(void**)&pwl_keyboard_interface,					"wl_keyboard_interface"},
	{(void**)&pwl_pointer_interface,					"wl_pointer_interface"},
	{(void**)&pwl_compositor_interface,					"wl_compositor_interface"},
	{(void**)&pwl_region_interface,						"wl_region_interface"},
	{(void**)&pwl_surface_interface,					"wl_surface_interface"},
#ifdef WL_SHELL_NAME
	{(void**)&pwl_shell_surface_interface,				"wl_shell_surface_interface"},
	{(void**)&pwl_shell_interface,						"wl_shell_interface"},
#endif
	{(void**)&pwl_seat_interface,						"wl_seat_interface"},
	{(void**)&pwl_registry_interface,					"wl_registry_interface"},
	{(void**)&pwl_output_interface,						"wl_output_interface"},
	{(void**)&pwl_shm_interface,						"wl_shm_interface"},
	{(void**)&pwl_shm_pool_interface,					"wl_shm_pool_interface"},
	{(void**)&pwl_buffer_interface,						"wl_buffer_interface"},
	{(void**)&pwl_callback_interface,					"wl_callback_interface"},
	{(void**)&pwl_data_device_manager_interface,		"wl_data_device_manager_interface"},
	{(void**)&pwl_data_device_interface,				"wl_data_device_interface"},
	{(void**)&pwl_data_source_interface,				"wl_data_source_interface"},
	{NULL, NULL}
};
static dllhandle_t *lib_wayland_wl;
static qboolean WL_InitLibrary(void)
{
	lib_wayland_wl = Sys_LoadLibrary("libwayland-client.so.0", waylandexports_wl);
	if (!lib_wayland_wl)
		return false;
	return true;
}

static dllhandle_t *lib_xkb;
static struct xkb_context *(*pxkb_context_new)(enum xkb_context_flags flags);
static struct xkb_keymap *(*pxkb_keymap_new_from_string)(struct xkb_context *context, const char *string, enum xkb_keymap_format format, enum xkb_keymap_compile_flags flags);
static struct xkb_state *(*pxkb_state_new)(struct xkb_keymap *keymap);
static xkb_keysym_t (*pxkb_state_key_get_one_sym)(struct xkb_state *state, xkb_keycode_t key);
static uint32_t (*pxkb_keysym_to_utf32)(xkb_keysym_t keysym);
static void (*pxkb_state_unref)(struct xkb_state *state);
static void (*pxkb_keymap_unref)(struct xkb_keymap *keymap);
static enum xkb_state_component (*pxkb_state_update_mask)(struct xkb_state *state, xkb_mod_mask_t depressed_mods, xkb_mod_mask_t latched_mods, xkb_mod_mask_t locked_mods, xkb_layout_index_t depressed_layout, xkb_layout_index_t latched_layout, xkb_layout_index_t locked_layout);
static struct xkb_keymap *(*pxkb_state_get_keymap)(struct xkb_state *state);
static int (*pxkb_keymap_key_repeats)(struct xkb_keymap *keymap, xkb_keycode_t key);
static qboolean WL_InitLibraryXKB(void)
{
	static dllfunction_t tab[] =
	{
		{(void**)&pxkb_context_new,						"xkb_context_new"},
		{(void**)&pxkb_keymap_new_from_string,			"xkb_keymap_new_from_string"},
		{(void**)&pxkb_state_new,						"xkb_state_new"},
		{(void**)&pxkb_state_update_mask,				"xkb_state_update_mask"},
		{(void**)&pxkb_state_key_get_one_sym,			"xkb_state_key_get_one_sym"},
		{(void**)&pxkb_keysym_to_utf32,					"xkb_keysym_to_utf32"},
		{(void**)&pxkb_state_unref,						"xkb_state_unref"},
		{(void**)&pxkb_keymap_unref,					"xkb_keymap_unref"},
		{(void**)&pxkb_state_get_keymap,				"xkb_state_get_keymap"},
		{(void**)&pxkb_keymap_key_repeats,				"xkb_keymap_key_repeats"},
		{NULL, NULL}
	};
	lib_xkb = Sys_LoadLibrary("libxkbcommon.so.0", tab);
	return !!lib_xkb;
}

#if defined(GLQUAKE) && defined(USE_EGL)
static struct wl_egl_window *(*pwl_egl_window_create)(struct wl_surface *surface, int width, int height);
static void (*pwl_egl_window_destroy)(struct wl_egl_window *egl_window);
static void (*pwl_egl_window_resize)(struct wl_egl_window *egl_window, int width, int height, int dx, int dy);
//static void (*pwl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width, int *height);
static dllfunction_t waylandexports_egl[] =
{
	{(void**)&pwl_egl_window_create,		"wl_egl_window_create"},
	{(void**)&pwl_egl_window_destroy,		"wl_egl_window_destroy"},
	{(void**)&pwl_egl_window_resize,		"wl_egl_window_resize"},
//	{(void**)&pwl_egl_window_get_attached_size,	"wl_egl_window_get_attached_size"},
	{NULL, NULL}
};
static dllhandle_t *lib_wayland_egl;
#endif


//I hate wayland.
static inline struct wl_region *pwl_compositor_create_region(struct wl_compositor *wl_compositor)				{return (struct wl_region*)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_compositor, WL_COMPOSITOR_CREATE_REGION, pwl_region_interface, NULL);}
static inline struct wl_surface *pwl_compositor_create_surface(struct wl_compositor *wl_compositor)				{return (struct wl_surface *)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_compositor, WL_COMPOSITOR_CREATE_SURFACE, pwl_surface_interface, NULL);}
static inline void pwl_surface_set_opaque_region(struct wl_surface *wl_surface, struct wl_region *region)		{pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_SET_OPAQUE_REGION, region);}
static inline void pwl_region_add(struct wl_region *wl_region, int32_t x, int32_t y, int32_t width, int32_t height)	{pwl_proxy_marshal((struct wl_proxy *) wl_region, WL_REGION_ADD, x, y, width, height);}
#ifdef WL_SHELL_NAME
static inline struct wl_shell_surface *pwl_shell_get_shell_surface(struct wl_shell *wl_shell, struct wl_surface *surface)	{return (struct wl_shell_surface *)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_shell, WL_SHELL_GET_SHELL_SURFACE, pwl_shell_surface_interface, NULL, surface);}
static inline void pwl_shell_surface_set_toplevel(struct wl_shell_surface *wl_shell_surface)					{pwl_proxy_marshal((struct wl_proxy *) wl_shell_surface, WL_SHELL_SURFACE_SET_TOPLEVEL);}
static inline void pwl_shell_surface_set_fullscreen(struct wl_shell_surface *wl_shell_surface, uint32_t method, uint32_t framerate, struct wl_output *output)	{pwl_proxy_marshal((struct wl_proxy *) wl_shell_surface, WL_SHELL_SURFACE_SET_FULLSCREEN, method, framerate, output);}
static inline int pwl_shell_surface_add_listener(struct wl_shell_surface *wl_shell_surface, const struct wl_shell_surface_listener *listener, void *data)		{return pwl_proxy_add_listener((struct wl_proxy *) wl_shell_surface, (void (**)(void)) listener, data);}
static inline void pwl_shell_surface_pong(struct wl_shell_surface *wl_shell_surface, uint32_t serial)			{pwl_proxy_marshal((struct wl_proxy *) wl_shell_surface, WL_SHELL_SURFACE_PONG, serial);}
static inline void pwl_shell_surface_set_title(struct wl_shell_surface *wl_shell_surface, const char *title)	{pwl_proxy_marshal((struct wl_proxy *) wl_shell_surface, WL_SHELL_SURFACE_SET_TITLE, title);}
#endif
static inline struct wl_registry *pwl_display_get_registry(struct wl_display *wl_display)	{return (struct wl_registry *)pwl_proxy_marshal_constructor((struct wl_proxy *) wl_display, WL_DISPLAY_GET_REGISTRY, pwl_registry_interface, NULL);}
static inline void *pwl_registry_bind(struct wl_registry *wl_registry, uint32_t name, const struct wl_interface *interface, uint32_t version)	{return (void*)pwl_proxy_marshal_constructor_versioned((struct wl_proxy *) wl_registry, WL_REGISTRY_BIND, interface, version, name, interface->name, version, NULL);}
static inline int pwl_registry_add_listener(struct wl_registry *wl_registry, const struct wl_registry_listener *listener, void *data)			{return pwl_proxy_add_listener((struct wl_proxy *) wl_registry, (void (**)(void)) listener, data);}
static inline void pwl_keyboard_destroy(struct wl_keyboard *wl_keyboard)			{pwl_proxy_destroy((struct wl_proxy *) wl_keyboard);}
static inline int pwl_keyboard_add_listener(struct wl_keyboard *wl_keyboard, const struct wl_keyboard_listener *listener, void *data)			{return pwl_proxy_add_listener((struct wl_proxy *) wl_keyboard, (void (**)(void)) listener, data);}
static inline void pwl_pointer_destroy(struct wl_pointer *wl_pointer)				{pwl_proxy_destroy((struct wl_proxy *) wl_pointer);}
static inline int pwl_pointer_add_listener(struct wl_pointer *wl_pointer, const struct wl_pointer_listener *listener, void *data)				{return pwl_proxy_add_listener((struct wl_proxy *) wl_pointer, (void (**)(void)) listener, data);}
static inline struct wl_pointer *pwl_seat_get_pointer(struct wl_seat *wl_seat)		{return (struct wl_pointer *)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_seat, WL_SEAT_GET_POINTER, pwl_pointer_interface, NULL);}
static inline struct wl_keyboard *pwl_seat_get_keyboard(struct wl_seat *wl_seat)	{return (struct wl_keyboard *)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_seat, WL_SEAT_GET_KEYBOARD, pwl_keyboard_interface, NULL);}
static inline int pwl_seat_add_listener(struct wl_seat *wl_seat, const struct wl_seat_listener *listener, void *data)	{return pwl_proxy_add_listener((struct wl_proxy *) wl_seat, (void (**)(void)) listener, data);}

static inline struct wl_callback *pwl_surface_frame(struct wl_surface *wl_surface)	{return (struct wl_callback*)(struct wl_proxy *) pwl_proxy_marshal_constructor((struct wl_proxy *) wl_surface, WL_SURFACE_FRAME, pwl_callback_interface, NULL);}
static inline int pwl_callback_add_listener(struct wl_callback *wl_callback, const struct wl_callback_listener *listener, void *data) {return pwl_proxy_add_listener((struct wl_proxy *) wl_callback, (void (**)(void)) listener, data);}

static inline void pwl_pointer_set_cursor(struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, int32_t hotspot_x, int32_t hotspot_y)		{ pwl_proxy_marshal((struct wl_proxy *) wl_pointer, WL_POINTER_SET_CURSOR, serial, surface, hotspot_x, hotspot_y); }
static inline struct wl_shm_pool *pwl_shm_create_pool(struct wl_shm *wl_shm, int32_t fd, int32_t size)		{ return (struct wl_shm_pool *)pwl_proxy_marshal_constructor((struct wl_proxy *) wl_shm, WL_SHM_CREATE_POOL, pwl_shm_pool_interface, NULL, fd, size); }
static inline struct wl_buffer *pwl_shm_pool_create_buffer(struct wl_shm_pool *wl_shm_pool, int32_t offset, int32_t width, int32_t height, int32_t stride, uint32_t format)		{ return (struct wl_buffer *)pwl_proxy_marshal_constructor((struct wl_proxy *) wl_shm_pool, WL_SHM_POOL_CREATE_BUFFER, pwl_buffer_interface, NULL, offset, width, height, stride, format); }
static inline void pwl_shm_pool_destroy(struct wl_shm_pool *wl_shm_pool)		{ pwl_proxy_marshal((struct wl_proxy *) wl_shm_pool, WL_SHM_POOL_DESTROY); pwl_proxy_destroy((struct wl_proxy *) wl_shm_pool); }
static inline void pwl_surface_commit(struct wl_surface *wl_surface)		{ pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_COMMIT); }
static inline void pwl_buffer_destroy(struct wl_buffer *wl_buffer)		{ pwl_proxy_marshal((struct wl_proxy *) wl_buffer, WL_BUFFER_DESTROY); pwl_proxy_destroy((struct wl_proxy *) wl_buffer); }
static inline void pwl_surface_attach(struct wl_surface *wl_surface, struct wl_buffer *buffer, int32_t x, int32_t y)		{ pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_ATTACH, buffer, x, y); }
static inline void pwl_surface_damage(struct wl_surface *wl_surface, int32_t x, int32_t y, int32_t width, int32_t height)		{ pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_DAMAGE, x, y, width, height); }
static inline void pwl_surface_destroy(struct wl_surface *wl_surface)		{ pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_DESTROY); pwl_proxy_destroy((struct wl_proxy *) wl_surface); }
static inline void pwl_surface_set_buffer_scale(struct wl_surface *wl_surface, int32_t scale)		{ pwl_proxy_marshal((struct wl_proxy *) wl_surface, WL_SURFACE_SET_BUFFER_SCALE, scale); }

#else
#define pwl_keyboard_interface		&wl_keyboard_interface
#define pwl_pointer_interface		&wl_pointer_interface
#define pwl_compositor_interface	&wl_compositor_interface
#define pwl_registry_interface		&wl_registry_interface
#define pwl_region_interface		&wl_region_interface
#define pwl_surface_interface		&wl_surface_interface
#ifdef WL_SHELL_NAME
#define pwl_shell_surface_interface	&wl_shell_surface_interface
#define pwl_shell_interface			&wl_shell_interface
#endif
#define pwl_seat_interface			&wl_seat_interface
#define pwl_output_interface		&wl_output_interface
#define pwl_shm_interface			&wl_shm_interface
#define pwl_data_device_manager_interface	&wl_data_device_manager_interface
#define pwl_data_device_interface	&wl_data_device_interface
#define pwl_data_source_interface	&wl_data_source_interface

#define pwl_display_connect					wl_display_connect
#define pwl_display_disconnect				wl_display_disconnect
#define pwl_display_dispatch				wl_display_dispatch
#define pwl_display_dispatch_pending		wl_display_dispatch_pending
#define pwl_display_roundtrip				wl_display_roundtrip
#define pwl_display_flush					wl_display_flush
#define pwl_display_prepare_read			wl_display_prepare_read
#define pwl_display_read_events			wl_display_read_events
#define pwl_display_cancel_read			wl_display_cancel_read
#define pwl_display_get_fd					wl_display_get_fd

#define pwl_proxy_marshal					wl_proxy_marshal
#define pwl_proxy_marshal_constructor		wl_proxy_marshal_constructor
#define pwl_proxy_marshal_constructor_versioned		wl_proxy_marshal_constructor_versioned
#define pwl_proxy_destroy					wl_proxy_destroy
#define pwl_proxy_add_listener				wl_proxy_add_listener

#define pwl_compositor_create_region		wl_compositor_create_region
#define pwl_compositor_create_surface		wl_compositor_create_surface
#define pwl_surface_set_opaque_region		wl_surface_set_opaque_region
#define pwl_region_add						wl_region_add
#define pwl_shell_get_shell_surface			wl_shell_get_shell_surface
#define pwl_shell_surface_set_toplevel		wl_shell_surface_set_toplevel
#define pwl_shell_surface_set_fullscreen	wl_shell_surface_set_fullscreen
#define pwl_shell_surface_add_listener		wl_shell_surface_add_listener
#define pwl_shell_surface_pong				wl_shell_surface_pong
#define pwl_shell_surface_set_title			wl_shell_surface_set_title
#define pwl_display_get_registry			wl_display_get_registry
#define pwl_registry_bind					wl_registry_bind
#define pwl_registry_add_listener			wl_registry_add_listener
#define pwl_keyboard_destroy				wl_keyboard_destroy
#define pwl_keyboard_add_listener			wl_keyboard_add_listener
#define pwl_pointer_destroy					wl_pointer_destroy
#define pwl_pointer_add_listener			wl_pointer_add_listener
#define pwl_seat_get_pointer				wl_seat_get_pointer
#define pwl_seat_get_keyboard				wl_seat_get_keyboard
#define pwl_seat_add_listener				wl_seat_add_listener
#define pwl_surface_frame					wl_surface_frame
#define pwl_callback_add_listener			wl_callback_add_listener
#define pwl_pointer_set_cursor				wl_pointer_set_cursor
#define pwl_shm_create_pool					wl_shm_create_pool
#define pwl_shm_pool_create_buffer			wl_shm_pool_create_buffer
#define pwl_shm_pool_destroy				wl_shm_pool_destroy
#define pwl_surface_commit					wl_surface_commit
#define pwl_buffer_destroy					wl_buffer_destroy
#define pwl_surface_attach					wl_surface_attach
#define pwl_surface_damage					wl_surface_damage
#define pwl_surface_destroy					wl_surface_destroy
#define pwl_surface_set_buffer_scale		wl_surface_set_buffer_scale

#define pwl_egl_window_create				wl_egl_window_create
#define pwl_egl_window_destroy				wl_egl_window_destroy
#define pwl_egl_window_resize				wl_egl_window_resize

#define pxkb_context_new					xkb_context_new
#define pxkb_keymap_new_from_string			xkb_keymap_new_from_string
#define pxkb_state_new						xkb_state_new
#define pxkb_state_key_get_one_sym			xkb_state_key_get_one_sym
#define pxkb_keysym_to_utf32				xkb_keysym_to_utf32
#define pxkb_state_unref					xkb_state_unref
#define pxkb_keymap_unref					xkb_keymap_unref
#define pxkb_state_update_mask				xkb_state_update_mask
#define pxkb_state_get_keymap				xkb_state_get_keymap
#define pxkb_keymap_key_repeats				xkb_keymap_key_repeats

#endif
static const struct wl_interface *pzwp_relative_pointer_v1_interface;
static const struct wl_interface *pzwp_locked_pointer_v1_interface;

struct cursorinfo_s
{
	struct wl_surface *surf;	//the surface used as a cursor.
	struct wl_buffer *buf;		//can't destroy this too early, or the server bugs out.
	int hot_x, hot_y;			//hotspot stuff
	int width, height;			//for invalidating the surface to avoid server bugs.
};

static struct wdisplay_s
{
	//display stuff
	struct wl_display *display;	//manager
	struct wl_registry *registry;	//manager
	struct wl_compositor *compositor;	//manager
	struct wl_shm *shm;	//manager

	//seat stuff
	void *pointer;
	qboolean cursorfocus;	//says that we 'own' the hardware cursor
	uint32_t cursorserial;	//required for updating the cursor image
	struct cursorinfo_s *cursor;
	struct wl_keyboard *keyboard;
	struct wl_seat *seat;
	uint32_t inputserial;	//latest key/button/enter serial; set_selection needs one the compositor gave us

	//wl_keyboard.repeat_info: the client repeats keys itself
	int repeat_rate;	//per second, 0 = off
	int repeat_delay;	//ms
	qboolean repeat_held;
	uint32_t repeat_scancode, repeat_qkey, repeat_ukey;
	double repeat_next;

	//clipboard (CBT_CLIPBOARD only)
	struct wl_data_device_manager *data_device_manager;
	struct wl_data_device *data_device;
	struct wl_data_offer *newoffer;	//offers announce their mime types before selection/enter names them
	int newoffer_mime;				//1-based index into wl_textmimes, 0 = no text
	qboolean newoffer_self;			//it offers WL_SelfMime: this process's own selection
	struct wl_data_offer *selection_offer;
	int selection_mime;
	qboolean selection_self;
	struct wl_data_offer *dnd_offer;
	struct wl_data_source *data_source;	//our newest source, until it is cancelled
	char *cliptext;					//what data_source serves

	//because wayland really doesn't help when it comes to various keymaps
	struct xkb_context *xkb_context;
	struct xkb_state *xkb_state;
	//to try to fix mouse stuff
	qboolean relative_mouse_active;
#ifdef WP_POINTER_CONSTRAINTS_NAME
	struct zwp_pointer_constraints_v1 *pointer_constraints;
	struct zwp_locked_pointer_v1 *locked_pointer;
#endif
#ifdef WP_RELATIVE_POINTER_MANAGER_NAME
	struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
	struct zwp_relative_pointer_v1 *relative_pointer;
#endif

	//stupid csd crap to work around shitty wayland servers
	int truewidth;	//not really needed, but present for consistency
	int trueheight;
	int csdsize;
	char *csdcaption;
	qboolean hasssd;	//probably false on gnome.
	qboolean fullscreen;	//P1611: the compositor's last xdg_toplevel.configure carried the fullscreen state
	qboolean statesynced;	//P1611: currentrendererstate.fullscreen re-read from `fullscreen` after the restart's memcpy
	int winwidth, winheight;	//P1611: an exact windowed size asked for by a live toggle, until a windowed configure
	int mousex,mousey;

	//window stuff
#if defined(GLQUAKE) && defined(USE_EGL)
	struct wl_egl_window *enwindow;
#endif
	struct wl_surface *surface;

	//HiDPI: truewidth/trueheight are surface (logical) units, the buffer and vid.pixel* are device pixels
	int compositor_version;
	struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
	struct wp_fractional_scale_v1 *fractional_scale;
	int scale120;				//wp_fractional_scale_v1.preferred_scale (scale*120), 0 until sent
	struct wp_viewporter *viewporter;
	struct wp_viewport *viewport;
	int preferred_buffer_scale;	//wl_surface v6, 0 until sent
	struct wloutput_s
	{
		struct wl_output *output;
		uint32_t id;
		int scale;
		qboolean entered;
	} outputs[8];				//for wl_output.scale, the last fallback
	int intscale;				//integer scale when there is no fractional scale
	int bufscale120;			//the scale the buffer was last sized for
	int bufwidth, bufheight;
	int destwidth, destheight;	//the logical size they were made for (the viewport destination)

	struct wp_tearing_control_manager_v1 *tearing_control_manager;
	struct wp_tearing_control_v1 *tearing_control;
	int tearing_hint;			//last hint sent, -1 = none

	struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
	struct wp_cursor_shape_device_v1 *cursor_shape_device;	//belongs to w.pointer; dies with it

#ifdef XDG_SHELL_NAME
	struct xdg_toplevel *xdg_toplevel;
	struct xdg_surface *xdg_surface;
	struct xdg_wm_base *xdg_wm_base;	//manager
#endif
#ifdef WL_SHELL_NAME
	struct wl_shell_surface *ssurface;
	struct wl_shell *shell;	//manager
#endif
	qboolean wait_for_configure;	//WL is being slow and won't let us redraw yet.
	qboolean waylandisblocking;
	struct wl_callback *framecb;	//P1602: at most one outstanding frame callback
	double frameasked;

	//belated sanity stuff
#ifdef XDG_DECORATION_MANAGER_NAME
	struct zxdg_toplevel_decoration_v1 *xdg_decoration;
	struct zxdg_decoration_manager_v1 *xdg_decoration_manager;
#endif
#ifdef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME
	struct org_kde_kwin_server_decoration *kwin_decoration;
	struct org_kde_kwin_server_decoration_manager *kwin_decoration_manager;
#endif
} w;
static char wl_caption[256];	//P1611: the last title, kept across WL_DeInit's memset of `w`
static void WL_SetCaption(const char *text);

//P1602: an interface table from the loaded libwayland-client bounds the version too: an older library cannot decode newer events
static uint32_t WL_BindVersion(const struct wl_interface *iface, uint32_t advertised, uint32_t ours)
{
	return min(min(advertised, ours), (uint32_t)iface->version);
}

//Patch 1602: HiDPI. The buffer is round(logical size * scale); wp_viewport maps it back onto the logical size.
#define WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE 1
#define WP_VIEWPORTER_GET_VIEWPORT 1
#define WP_VIEWPORT_SET_DESTINATION 2
#define WP_TEARING_CONTROL_MANAGER_V1_GET_TEARING_CONTROL 1
#define WP_TEARING_CONTROL_V1_SET_PRESENTATION_HINT 0
#define WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC 0
#define WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC 1
static const struct wl_interface *pwp_fractional_scale_v1_interface;
static const struct wl_interface *pwp_viewport_interface;
static const struct wl_interface *pwp_tearing_control_v1_interface;
static const struct wl_interface *pwp_cursor_shape_device_v1_interface;
#define WP_CURSOR_SHAPE_MANAGER_V1_GET_POINTER 1
#define WP_CURSOR_SHAPE_DEVICE_V1_DESTROY 0
#define WP_CURSOR_SHAPE_DEVICE_V1_SET_SHAPE 1
#define WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT 1

static void WL_BindFractionalScaleManager(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[3];
	static const struct wl_message wp_fractional_scale_manager_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_fractional_scale", "no", types + 1 },
	};
	static const struct wl_interface wp_fractional_scale_manager_v1_interface = {
		"wp_fractional_scale_manager_v1", 1,
		2, wp_fractional_scale_manager_v1_requests,
		0, NULL,
	};
	static const struct wl_message wp_fractional_scale_v1_requests[] = {
		{ "destroy", "", types + 0 },
	};
	static const struct wl_message wp_fractional_scale_v1_events[] = {
		{ "preferred_scale", "u", types + 0 },
	};
	static const struct wl_interface wp_fractional_scale_v1_interface = {
		"wp_fractional_scale_v1", 1,
		1, wp_fractional_scale_v1_requests,
		1, wp_fractional_scale_v1_events,
	};

	types[1] = &wp_fractional_scale_v1_interface;
	types[2] = pwl_surface_interface;

	pwp_fractional_scale_v1_interface = &wp_fractional_scale_v1_interface;
	w.fractional_scale_manager = pwl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
}
static void WL_BindViewporter(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[6];	//set_source's four fixed args read types[0..3]
	static const struct wl_message wp_viewporter_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_viewport", "no", types + 4 },
	};
	static const struct wl_interface wp_viewporter_interface = {
		"wp_viewporter", 1,
		2, wp_viewporter_requests,
		0, NULL,
	};
	static const struct wl_message wp_viewport_requests[] = {
		{ "destroy", "", types + 0 },
		{ "set_source", "ffff", types + 0 },
		{ "set_destination", "ii", types + 0 },
	};
	static const struct wl_interface wp_viewport_interface = {
		"wp_viewport", 1,
		3, wp_viewport_requests,
		0, NULL,
	};

	types[4] = &wp_viewport_interface;
	types[5] = pwl_surface_interface;

	pwp_viewport_interface = &wp_viewport_interface;
	w.viewporter = pwl_registry_bind(registry, id, &wp_viewporter_interface, 1);
}
static void WL_BindTearingControlManager(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[3];
	static const struct wl_message wp_tearing_control_manager_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_tearing_control", "no", types + 1 },
	};
	static const struct wl_interface wp_tearing_control_manager_v1_interface = {
		"wp_tearing_control_manager_v1", 1,
		2, wp_tearing_control_manager_v1_requests,
		0, NULL,
	};
	static const struct wl_message wp_tearing_control_v1_requests[] = {
		{ "set_presentation_hint", "u", types + 0 },
		{ "destroy", "", types + 0 },
	};
	static const struct wl_interface wp_tearing_control_v1_interface = {
		"wp_tearing_control_v1", 1,
		2, wp_tearing_control_v1_requests,
		0, NULL,
	};

	types[1] = &wp_tearing_control_v1_interface;
	types[2] = pwl_surface_interface;

	pwp_tearing_control_v1_interface = &wp_tearing_control_v1_interface;
	w.tearing_control_manager = pwl_registry_bind(registry, id, &wp_tearing_control_manager_v1_interface, 1);
}
//P1602: wp_cursor_shape_v1 (staging) -- the compositor draws its own arrow, at the output's scale
static void WL_BindCursorShapeManager(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[3];
	static const struct wl_message wp_cursor_shape_manager_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_pointer", "no", types + 1 },
	};
	static const struct wl_interface wp_cursor_shape_manager_v1_interface = {
		"wp_cursor_shape_manager_v1", 1,
		2, wp_cursor_shape_manager_v1_requests,
		0, NULL,
	};
	static const struct wl_message wp_cursor_shape_device_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "set_shape", "uu", types + 0 },
	};
	static const struct wl_interface wp_cursor_shape_device_v1_interface = {
		"wp_cursor_shape_device_v1", 1,
		2, wp_cursor_shape_device_v1_requests,
		0, NULL,
	};

	types[1] = &wp_cursor_shape_device_v1_interface;
	types[2] = pwl_pointer_interface;

	pwp_cursor_shape_device_v1_interface = &wp_cursor_shape_device_v1_interface;
	w.cursor_shape_manager = pwl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface, 1);
}
static void WL_DropCursorShapeDevice(void)
{
	if (!w.cursor_shape_device)
		return;
	pwl_proxy_marshal((struct wl_proxy *) w.cursor_shape_device, WP_CURSOR_SHAPE_DEVICE_V1_DESTROY);
	pwl_proxy_destroy((struct wl_proxy *) w.cursor_shape_device);
	w.cursor_shape_device = NULL;
}

static int WL_Scale120(void)
{
	if (w.fractional_scale)
		return w.scale120?w.scale120:120;	//wl_surface's integer hint is the ceiling, so it is not used meanwhile
	if (!w.viewport && w.compositor_version < 3)
		return 120;	//no wl_surface.set_buffer_scale either
	return 120*max(1, w.intscale);
}
//buffer pixels per surface unit, for the buffer on screen now
static float WL_BufferScale(void)
{
	return w.bufscale120?w.bufscale120/120.f:1;
}
//(re)size the buffer for the current logical size and scale; called whenever either changes
static void WL_ApplySize(void)
{
	extern cvar_t vid_dpi_x, vid_dpi_y;
	int scale = WL_Scale120();
	int bw = (w.truewidth*scale + 60)/120;
	int bh = (w.trueheight*scale + 60)/120;

	if (!w.surface || !w.truewidth || !w.trueheight)
		return;
	if (bw == w.bufwidth && bh == w.bufheight && scale == w.bufscale120 && w.truewidth == w.destwidth && w.trueheight == w.destheight)
		return;

	if (w.truewidth != w.destwidth || w.trueheight != w.destheight)
	{	//P1611: the opaque region follows the size. It was set once, at the 640x480 the window was created at, so a
		//compositor that honours it saw everything past that corner as translucent (only an XRGB buffer hid it).
		struct wl_region *region = pwl_compositor_create_region(w.compositor);
		pwl_region_add(region, 0, 0, w.truewidth, w.trueheight);
		pwl_surface_set_opaque_region(w.surface, region);
		pwl_proxy_marshal((struct wl_proxy *) region, WL_REGION_DESTROY);	//the surface keeps a copy
		pwl_proxy_destroy((struct wl_proxy *) region);
	}
	if (w.viewport)
		pwl_proxy_marshal((struct wl_proxy *) w.viewport, WP_VIEWPORT_SET_DESTINATION, w.truewidth, w.trueheight);
	else if (scale != w.bufscale120 && w.compositor_version >= 3)
		pwl_surface_set_buffer_scale(w.surface, scale/120);
	if (scale != w.bufscale120)
	{	//for the code that sizes things in physical units
		vid.dpi_x = vid.dpi_y = 96*scale/120.f;
		Cvar_ForceSetValue(&vid_dpi_x, vid.dpi_x);
		Cvar_ForceSetValue(&vid_dpi_y, vid.dpi_y);
	}
	w.bufwidth = bw;
	w.bufheight = bh;
	w.bufscale120 = scale;
	w.destwidth = w.truewidth;
	w.destheight = w.trueheight;
	Con_DPrintf("Wayland: %ix%i logical, scale %g, %ix%i buffer\n", w.truewidth, w.trueheight, scale/120.0, bw, bh);

#if defined(GLQUAKE) && defined(USE_EGL)
	if (w.enwindow)
		pwl_egl_window_resize(w.enwindow, bw, bh, 0, 0);
#endif
	vid.pixelwidth = bw;
	vid.pixelheight = bh-w.csdsize;
	Cvar_ForceCallback(&vid_conautoscale);
}

static void WL_fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
	Con_DPrintf("Wayland: preferred scale %u/120 (%g)\n", scale, scale/120.0);
	w.scale120 = scale;
	WL_ApplySize();
}
static const struct wp_fractional_scale_v1_listener
{
	void (*preferred_scale)(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale);
} fractional_scale_listener =
{
	WL_fractional_scale_handle_preferred_scale
};

//without fractional scale: wl_surface.preferred_buffer_scale, else the biggest wl_output.scale the surface is on
static void WL_UpdateIntScale(void)
{
	int i, s = w.preferred_buffer_scale;
	if (!s)
	{
		for (i = 0; i < countof(w.outputs); i++)
			if (w.outputs[i].output && w.outputs[i].entered)
				s = max(s, w.outputs[i].scale);
	}
	w.intscale = max(1, s);
	WL_ApplySize();
}
static void WL_output_handle_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
}
static void WL_output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
}
static void WL_output_handle_done(void *data, struct wl_output *output)
{
}
static void WL_output_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
	struct wloutput_s *o = data;
	o->scale = factor;
	if (o->entered)
		WL_UpdateIntScale();
}
static const struct wl_output_listener output_listener =
{	//bound at version 2 at most, so name and description never arrive
	WL_output_handle_geometry,
	WL_output_handle_mode,
	WL_output_handle_done,
	WL_output_handle_scale,
};
static void WL_BindOutput(struct wl_registry *registry, uint32_t id, uint32_t version)
{
	int i;
	for (i = 0; i < countof(w.outputs); i++)
	{
		if (!w.outputs[i].output)
		{
			w.outputs[i].id = id;
			w.outputs[i].scale = 1;
			w.outputs[i].entered = false;
			w.outputs[i].output = pwl_registry_bind(registry, id, pwl_output_interface, WL_BindVersion(pwl_output_interface, version, 2));
			pwl_proxy_add_listener((struct wl_proxy *) w.outputs[i].output, (void (**)(void)) &output_listener, &w.outputs[i]);
			return;
		}
	}
}
static void WL_surface_handle_output(struct wl_output *output, qboolean entered)
{
	int i;
	for (i = 0; i < countof(w.outputs); i++)
		if (output && w.outputs[i].output == output)	//NULL: an output we already destroyed
			w.outputs[i].entered = entered;
	WL_UpdateIntScale();
}
static void WL_surface_handle_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	WL_surface_handle_output(output, true);
}
static void WL_surface_handle_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
	WL_surface_handle_output(output, false);
}
static void WL_surface_handle_preferred_buffer_scale(void *data, struct wl_surface *surface, int32_t factor)
{
	Con_DPrintf("Wayland: preferred buffer scale %i\n", factor);
	w.preferred_buffer_scale = factor;
	WL_UpdateIntScale();
}
static void WL_surface_handle_preferred_buffer_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
}
//Patch 1667: wl_surface events through v6 as a fixed table, not the header's struct. Debian 11's wayland 1.18 header
//(the release build) has a two-member wl_surface_listener, so a struct initialiser dropped the two v6 handlers while
//WL_BindVersion still bound v6 from the system library, and libwayland aborted on preferred_buffer_scale.
static void (*const surface_listener[4])(void) =
{
	(void (*)(void))WL_surface_handle_enter,
	(void (*)(void))WL_surface_handle_leave,
	(void (*)(void))WL_surface_handle_preferred_buffer_scale,
	(void (*)(void))WL_surface_handle_preferred_buffer_transform,
};

//wp_tearing_control_v1: let the compositor tear (when it allows that at all) only while vsync is off
static void WL_UpdateTearing(void)
{
	extern cvar_t vid_vsync;
	int hint = (*vid_vsync.string && vid_vsync.ival <= 0)?WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC:WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC;	//EGL_UpdateSwapInterval's reading
	if (!w.tearing_control || hint == w.tearing_hint)
		return;
	w.tearing_hint = hint;
	pwl_proxy_marshal((struct wl_proxy *) w.tearing_control, WP_TEARING_CONTROL_V1_SET_PRESENTATION_HINT, hint);
	Con_DPrintf("Wayland: presentation hint %s\n", (hint==WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC)?"async":"vsync");
}



#ifdef XDG_SHELL_NAME
static const struct wl_interface *pxdg_surface_interface;
static const struct wl_interface *pxdg_toplevel_interface;
static const struct wl_interface *pxdg_wm_base_interface;
static void WL_Setup_XDG_Shell(void)
{
	static const struct wl_interface *types[24];

	static const struct wl_message xdg_wm_base_requests[] = {
		{ "destroy", "", types + 0 },
		{ "create_positioner", "n", types + 4 },
		{ "get_xdg_surface", "no", types + 5 },
		{ "pong", "u", types + 0 },
	};
	static const struct wl_message xdg_wm_base_events[] = {
		{ "ping", "u", types + 0 },
	};

	static const struct wl_interface xdg_wm_base_interface = {
#ifdef XDG_SHELL_UNSTABLE
		"zxdg_wm_base_v6", 1,
#else
		"xdg_wm_base", 2,
#endif
		4, xdg_wm_base_requests,
		1, xdg_wm_base_events,
	};

	static const struct wl_message xdg_positioner_requests[] = {
		{ "destroy", "", types + 0 },
		{ "set_size", "ii", types + 0 },
		{ "set_anchor_rect", "iiii", types + 0 },
		{ "set_anchor", "u", types + 0 },
		{ "set_gravity", "u", types + 0 },
		{ "set_constraint_adjustment", "u", types + 0 },
		{ "set_offset", "ii", types + 0 },
	};

	static const struct wl_interface xdg_positioner_interface = {
#ifdef XDG_SHELL_UNSTABLE
		"zxdg_positioner_v6", 1,
#else
		"xdg_positioner", 2,
#endif
		7, xdg_positioner_requests,
		0, NULL,
	};

	static const struct wl_message xdg_surface_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_toplevel", "n", types + 7 },
#ifdef XDG_SHELL_UNSTABLE
		{ "get_popup", "noo", types + 8 },
#else
		{ "get_popup", "n?oo", types + 8 },
#endif
		{ "set_window_geometry", "iiii", types + 0 },
		{ "ack_configure", "u", types + 0 },
	};

	static const struct wl_message xdg_surface_events[] = {
		{ "configure", "u", types + 0 },
	};

	static const struct wl_interface xdg_surface_interface = {
#ifdef XDG_SHELL_UNSTABLE
		"zxdg_surface_v6", 1,
#else
		"xdg_surface", 2,
#endif
		5, xdg_surface_requests,
		1, xdg_surface_events,
	};

	static const struct wl_message xdg_toplevel_requests[] = {
		{ "destroy", "", types + 0 },
		{ "set_parent", "?o", types + 11 },
		{ "set_title", "s", types + 0 },
		{ "set_app_id", "s", types + 0 },
		{ "show_window_menu", "ouii", types + 12 },
		{ "move", "ou", types + 16 },
		{ "resize", "ouu", types + 18 },
		{ "set_max_size", "ii", types + 0 },
		{ "set_min_size", "ii", types + 0 },
		{ "set_maximized", "", types + 0 },
		{ "unset_maximized", "", types + 0 },
		{ "set_fullscreen", "?o", types + 21 },
		{ "unset_fullscreen", "", types + 0 },
		{ "set_minimized", "", types + 0 },
	};

	static const struct wl_message xdg_toplevel_events[] = {
		{ "configure", "iia", types + 0 },
		{ "close", "", types + 0 },
	};

	static const struct wl_interface xdg_toplevel_interface = {
#ifdef XDG_SHELL_UNSTABLE
		"zxdg_toplevel_v6", 1,
#else
		"xdg_toplevel", 2,
#endif
		14, xdg_toplevel_requests,
		2, xdg_toplevel_events,
	};

	static const struct wl_message xdg_popup_requests[] = {
		{ "destroy", "", types + 0 },
		{ "grab", "ou", types + 22 },
	};
	static const struct wl_message xdg_popup_events[] = {
		{ "configure", "iiii", types + 0 },
		{ "popup_done", "", types + 0 },
	};

	static const struct wl_interface xdg_popup_interface = {
#ifdef UNSTABLE
		"zxdg_popup_v6", 1,
#else
		"xdg_popup", 2,
#endif
		2, xdg_popup_requests,
		2, xdg_popup_events,
	};

	types[4] = &xdg_positioner_interface;
	types[5] = &xdg_surface_interface;
	types[6] = pwl_surface_interface;
	types[7] = &xdg_toplevel_interface;
	types[8] = &xdg_popup_interface;
	types[9] = &xdg_surface_interface;
	types[10] = &xdg_positioner_interface;
	types[11] = &xdg_toplevel_interface;
	types[12] = pwl_seat_interface;
	types[16] = pwl_seat_interface;
	types[18] = pwl_seat_interface;
	types[21] = pwl_output_interface;
	types[22] = pwl_seat_interface;

	pxdg_surface_interface = &xdg_surface_interface;
	pxdg_toplevel_interface = &xdg_toplevel_interface;
	pxdg_wm_base_interface = &xdg_wm_base_interface;
}
#define XDG_WM_BASE_DESTROY 0
#define XDG_WM_BASE_CREATE_POSITIONER 1
#define XDG_WM_BASE_GET_XDG_SURFACE 2
#define XDG_WM_BASE_PONG 3
struct xdg_wm_base_listener {
    void (*ping)(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial);
};
static inline int xdg_wm_base_add_listener(struct xdg_wm_base *xdg_wm_base, const struct xdg_wm_base_listener *listener, void *data)		{ return pwl_proxy_add_listener((struct wl_proxy *) xdg_wm_base, (void (**)(void)) listener, data); }
static inline struct xdg_surface *xdg_wm_base_get_xdg_surface(struct xdg_wm_base *xdg_wm_base, struct wl_surface *surface)		{ return (struct xdg_surface *)pwl_proxy_marshal_constructor((struct wl_proxy *) xdg_wm_base, XDG_WM_BASE_GET_XDG_SURFACE, pxdg_surface_interface, NULL, surface); }
static inline void xdg_wm_base_pong(struct xdg_wm_base *xdg_wm_base, uint32_t serial)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_wm_base, XDG_WM_BASE_PONG, serial); }

#define XDG_SURFACE_DESTROY 0
#define XDG_SURFACE_GET_TOPLEVEL 1
#define XDG_SURFACE_GET_POPUP 2
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define XDG_SURFACE_ACK_CONFIGURE 4
struct xdg_surface_listener {
    void (*configure)(void *data, struct xdg_surface *xdg_surface, uint32_t serial);
};
static inline int xdg_surface_add_listener(struct xdg_surface *xdg_surface, const struct xdg_surface_listener *listener, void *data) { return pwl_proxy_add_listener((struct wl_proxy *) xdg_surface, (void (**)(void)) listener, data); }
static inline struct xdg_toplevel *xdg_surface_get_toplevel(struct xdg_surface *xdg_surface)		{ return (struct xdg_toplevel *)pwl_proxy_marshal_constructor((struct wl_proxy *) xdg_surface, XDG_SURFACE_GET_TOPLEVEL, pxdg_toplevel_interface, NULL); }
static inline void xdg_surface_ack_configure(struct xdg_surface *xdg_surface, uint32_t serial)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_surface, XDG_SURFACE_ACK_CONFIGURE, serial); }

#define XDG_TOPLEVEL_DESTROY 0
#define XDG_TOPLEVEL_SET_PARENT 1
#define XDG_TOPLEVEL_SET_TITLE 2
#define XDG_TOPLEVEL_SET_APP_ID 3
#define XDG_TOPLEVEL_SHOW_WINDOW_MENU 4
#define XDG_TOPLEVEL_MOVE 5
#define XDG_TOPLEVEL_RESIZE 6
#define XDG_TOPLEVEL_SET_MAX_SIZE 7
#define XDG_TOPLEVEL_SET_MIN_SIZE 8
#define XDG_TOPLEVEL_SET_MAXIMIZED 9
#define XDG_TOPLEVEL_UNSET_MAXIMIZED 10
#define XDG_TOPLEVEL_SET_FULLSCREEN 11
#define XDG_TOPLEVEL_UNSET_FULLSCREEN 12
#define XDG_TOPLEVEL_SET_MINIMIZED 13
struct xdg_toplevel_listener;
static inline void xdg_toplevel_set_title(struct xdg_toplevel *xdg_toplevel, const char *title)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SET_TITLE, title); }
static inline void xdg_toplevel_set_app_id(struct xdg_toplevel *xdg_toplevel, const char *app_id)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SET_APP_ID, app_id); }
static inline int xdg_toplevel_add_listener(struct xdg_toplevel *xdg_toplevel, const struct xdg_toplevel_listener *listener, void *data)		{ return pwl_proxy_add_listener((struct wl_proxy *) xdg_toplevel, (void (**)(void)) listener, data); }
static inline void xdg_toplevel_set_min_size(struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SET_MIN_SIZE, width, height); }
static inline void xdg_toplevel_move(struct xdg_toplevel *xdg_toplevel, struct wl_seat *seat, uint32_t serial)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_MOVE, seat, serial); }
static inline void xdg_toplevel_show_window_menu(struct xdg_toplevel *xdg_toplevel, struct wl_seat *seat, uint32_t serial, int32_t x, int32_t y)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SHOW_WINDOW_MENU, seat, serial, x, y); }
static inline void xdg_toplevel_set_fullscreen(struct xdg_toplevel *xdg_toplevel, struct wl_output *output)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SET_FULLSCREEN, output); }
static inline void xdg_toplevel_unset_fullscreen(struct xdg_toplevel *xdg_toplevel)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_UNSET_FULLSCREEN); }
static inline void xdg_toplevel_set_maximized(struct xdg_toplevel *xdg_toplevel)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_SET_MAXIMIZED); }
static inline void xdg_toplevel_unset_maximized(struct xdg_toplevel *xdg_toplevel)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_toplevel, XDG_TOPLEVEL_UNSET_MAXIMIZED); }
#define XDG_TOPLEVEL_STATE_FULLSCREEN 2	//xdg_toplevel.state, the same value in zxdg_toplevel_v6

static void xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	xdg_wm_base_pong(xdg_wm_base, serial);
}
static const struct xdg_wm_base_listener myxdg_wm_base_listener =
{
	xdg_wm_base_handle_ping,
};

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	xdg_surface_ack_configure(xdg_surface, serial);

	w.wait_for_configure = false; //all good now
}
static const struct xdg_surface_listener myxdg_surface_listener =
{
	xdg_surface_handle_configure,
};

void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states)
{
	const uint32_t *st;
	qboolean fs = false;
	for (st = states->data; (const char *)(st+1) <= (const char *)states->data + states->size; st++)
		if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN)
			fs = true;
	if (fs != w.fullscreen)
		Con_DPrintf("Wayland: %s\n", fs?"fullscreen":"not fullscreen");
	w.fullscreen = fs;
	//P1611: keep the engine's idea in step when the compositor changes it on its own (labwc's ToggleFullscreen,
	//GNOME's keybinding), so F11's "fullscreen?windowed:fullscreen" decision reads what is on screen. Wayland has
	//no exclusive mode: the engine's 1 and 2 are the same window state, so anything fullscreen counts.
	if (fs && !currentrendererstate.fullscreen)
		currentrendererstate.fullscreen = 2;	//internal 2 = borderless, what fullscreen_toggle asks for
	else if (!fs && currentrendererstate.fullscreen)
		currentrendererstate.fullscreen = 0;

	if ((!width || !height) && !fs && w.winwidth)
	{	//P1611: the 0x0 answer to an exact-size unset_fullscreen. Kept apart from truewidth, so a configure still
		//in flight (fullscreen-sized) cannot overwrite it first.
		width = w.winwidth;
		height = w.winheight;
	}
	if (!fs)
		w.winwidth = w.winheight = 0;
	if (!width || !height)
	{
		width = w.truewidth;
		height = w.trueheight;
	}

	w.truewidth = width;
	w.trueheight = height;
	WL_ApplySize();
}
void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	//fixme: steal focus, bring to top, or just hope the compositor is doing that for us when the user right-clicked the task bar of our minimised app or whatever.
	M_Window_ClosePrompt();
}
static struct xdg_toplevel_listener {
    void (*configure)(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states);
    void (*close)(void *data, struct xdg_toplevel *xdg_toplevel);
} myxdg_toplevel_listener =
{
	xdg_toplevel_handle_configure,
	xdg_toplevel_handle_close
};

#endif

#ifdef WL_SHELL_NAME
static void WL_shell_handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
	pwl_shell_surface_pong(shell_surface, serial);
}
static void WL_shell_handle_configure(void *data, struct wl_shell_surface *shell_surface, uint32_t edges, int32_t width, int32_t height)
{
	w.truewidth = width;
	w.trueheight = height;
	WL_ApplySize();

	w.wait_for_configure = false; //all good now
}
static void WL_shell_handle_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener =
{
	WL_shell_handle_ping,
	WL_shell_handle_configure,
	WL_shell_handle_popup_done
};
#endif

//qkeys are ascii-compatible for the most part.
static unsigned short waylandinputsucksbighairydonkeyballs[] =
{
	0, 	K_ESCAPE,'1','2','3','4','5','6',	//0x
	'7','8','9','0','-','=',K_BACKSPACE,K_TAB,
	'q','w','e','r','t','y','u','i',		//1x
	'o','p','[',']',K_ENTER,K_LCTRL,'a', 's',
	'd','f','g','h','j','k','l',';',		//2x
	'\'','`',K_LSHIFT,'#','z','x','c','v',
	'b','n','m',',','.','/',K_RSHIFT,K_KP_STAR,	//3x
	K_LALT,' ',K_CAPSLOCK,K_F1,K_F2,K_F3,K_F4,K_F5,
	K_F6,K_F7,K_F8,K_F9,K_F10,K_KP_NUMLOCK,K_SCRLCK,K_KP_HOME,//4x
	K_KP_UPARROW,K_KP_PGUP,K_KP_MINUS,K_KP_LEFTARROW,K_KP_5,K_KP_RIGHTARROW,K_KP_PLUS,K_KP_END,
	K_KP_DOWNARROW,K_KP_PGDN,K_KP_INS,K_KP_DEL,0,0,'\\',K_F11,	//5x
	K_F12,0,0,0,0,0,0,0,
	K_KP_ENTER,K_RCTRL,K_KP_SLASH,K_PRINTSCREEN,K_RALT,0,K_HOME,K_UPARROW,	//6x
	K_PGUP,K_LEFTARROW,K_RIGHTARROW,K_END,K_DOWNARROW,K_PGDN,K_INS,K_DEL,
	0,0,0,0,0,0,0,K_PAUSE,	//7x
	0,0,0,0,0,K_LWIN,K_RWIN,K_APP
};
static unsigned short waylandinputsucksbighairydonkeyballsshift[] =
{
	0, 	K_ESCAPE,'!','\"','3','$','%','^',	//0x
	'&','*','(',')','_','+',K_BACKSPACE,K_TAB,
	'Q','W','E','R','T','Y','U','I',		//1x
	'O','P','{','}',K_ENTER,K_LCTRL,'A', 'S',
	'D','F','G','H','J','K','L',':',		//2x
	'@','`',K_LSHIFT,'~','Z','X','C','V',
	'B','N','M','<','>','?',K_RSHIFT,K_KP_STAR,	//3x
	K_LALT,' ',K_CAPSLOCK,K_F1,K_F2,K_F3,K_F4,K_F5,
	K_F6,K_F7,K_F8,K_F9,K_F10,K_KP_NUMLOCK,K_SCRLCK,K_KP_HOME,//4x
	K_KP_UPARROW,K_KP_PGUP,K_KP_MINUS,K_KP_LEFTARROW,K_KP_5,K_KP_RIGHTARROW,K_KP_PLUS,K_KP_END,
	K_KP_DOWNARROW,K_KP_PGDN,K_KP_INS,K_KP_DEL,0,0,'|',K_F11,	//5x
	K_F12,0,0,0,0,0,0,0,
	K_KP_ENTER,K_RCTRL,K_KP_SLASH,K_PRINTSCREEN,K_RALT,0,K_HOME,K_UPARROW,	//6x
	K_PGUP,K_LEFTARROW,K_RIGHTARROW,K_END,K_DOWNARROW,K_PGDN,K_INS,K_DEL,
	0,0,0,0,0,0,0,K_PAUSE,	//7x
	0,0,0,0,0,K_LWIN,K_RWIN,K_APP	
};

static void WL_SetHWCursor(void)
{	//called when the hardware cursor needs to be re-applied

//	struct wl_buffer *buffer;
//	struct wl_cursor *cursor = s->default_cursor;
//	struct wl_cursor_image *image;

	if (!w.cursorfocus)
		return;	//nope, we don't own the cursor, don't try changing it.

	/*'If surface is NULL, the pointer image is hidden'*/
	if (!w.relative_mouse_active && !w.locked_pointer && w.cursor)
	{
		struct cursorinfo_s *c = w.cursor;
		//weston bugs out if we don't redo this junk
		pwl_surface_attach(c->surf, c->buf, 0, 0);
		pwl_surface_damage(c->surf, 0, 0, c->width, c->height);
		pwl_surface_commit(c->surf);

		//now actually set it.
		pwl_pointer_set_cursor(w.pointer, w.cursorserial, c->surf, c->hot_x, c->hot_y);
	}
	else if (!w.relative_mouse_active && !w.locked_pointer && w.cursor_shape_manager && w.pointer)
	{	//P1602: no custom image means the desktop's arrow, as on X11; NULL would hide the pointer in every menu
		if (!w.cursor_shape_device)
			w.cursor_shape_device = (struct wp_cursor_shape_device_v1 *)pwl_proxy_marshal_constructor((struct wl_proxy *) w.cursor_shape_manager, WP_CURSOR_SHAPE_MANAGER_V1_GET_POINTER, pwp_cursor_shape_device_v1_interface, NULL, w.pointer);
		pwl_proxy_marshal((struct wl_proxy *) w.cursor_shape_device, WP_CURSOR_SHAPE_DEVICE_V1_SET_SHAPE, w.cursorserial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	}
	else
		pwl_pointer_set_cursor(w.pointer, w.cursorserial, NULL, 0, 0);
}
void *WL_CreateCursor(const qbyte *imagedata, int width, int height, uploadfmt_t format, float hotx, float hoty, float scale)
{
	struct wl_surface *surf = NULL;
	enum wl_shm_format shmfmt;
	int pbytes;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer = NULL;
	qbyte *outdata;
	size_t size;
	const char *xdg_runtime_dir = getenv ("XDG_RUNTIME_DIR");
	int fd;
	struct cursorinfo_s *c;
	static qboolean allowfmts[PTI_MAX] = {[PTI_BGRX8]=true, [PTI_BGRA8]=true};	//FIXME: populate via wl_shm_add_listener instead of using formats that we THINK will work.
	struct pendingtextureinfo mips = {};

	if (!w.shm)
		return NULL;	//can't shm the image to the server

	mips.mipcount = 1;
	mips.type = PTI_2D;
	mips.encoding = format;
	mips.extrafree = NULL;

	mips.mip[0].width = width*scale;
	mips.mip[0].height = height*scale;
	mips.mip[0].depth = 1;
	mips.mip[0].data = Image_ResampleTexture(format, imagedata, width, height, NULL, mips.mip[0].width, mips.mip[0].height);
	mips.mip[0].needfree = true;
	mips.mip[0].datasize = 0;
	if (!allowfmts[mips.encoding])
		Image_ChangeFormat(&mips, allowfmts, format, "cursor");
	if (!allowfmts[mips.encoding])
		return NULL;	//failure...

	switch(mips.encoding)
	{	//fte favours byte orders, while packed formats are ordered as hex numbers would be
		//wayland formats are as hex (explicitly little-endian, so byteswapped)
	case PTI_RGBA8:	pbytes = 4; shmfmt = WL_SHM_FORMAT_ABGR8888;	break;
	case PTI_RGBX8:	pbytes = 4; shmfmt = WL_SHM_FORMAT_XBGR8888;	break;
	case PTI_LLLA8: //just fall through
	case PTI_BGRA8:	pbytes = 4; shmfmt = WL_SHM_FORMAT_ARGB8888;	break;
	case PTI_LLLX8: //just fall through
	case PTI_BGRX8:	pbytes = 4; shmfmt = WL_SHM_FORMAT_XRGB8888;	break;
	case PTI_RGB8:	pbytes = 3; shmfmt = WL_SHM_FORMAT_BGR888;		break;
	case PTI_BGR8:	pbytes = 3; shmfmt = WL_SHM_FORMAT_RGB888;		break;

	case PTI_RGB565:pbytes = 2; shmfmt = WL_SHM_FORMAT_RGB565;		break;
	case PTI_A2BGR10:pbytes= 4; shmfmt = WL_SHM_FORMAT_ABGR2101010;	break;
	case PTI_L8:
	default:
		Sys_Error("WL_CreateCursor: converted to unsupported pixel format %i", format);
		return NULL;	//failure. can't convert from that format.
	}

	Image_Premultiply(&mips);

	fd = open (xdg_runtime_dir, __O_TMPFILE|O_RDWR|O_EXCL, 0600);
	size = mips.mip[0].width * mips.mip[0].height * pbytes;
	if (fd >= 0)
	{
		ftruncate (fd, size);
		outdata = mmap (NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (outdata)
		{
			memcpy(outdata, mips.mip[0].data, mips.mip[0].width*mips.mip[0].height*pbytes);
			pool = pwl_shm_create_pool (w.shm, fd, size);
			if (pool)
			{
				buffer = pwl_shm_pool_create_buffer (pool, 0, mips.mip[0].width, mips.mip[0].height, mips.mip[0].width*pbytes, shmfmt);
				if (buffer)
				{
					surf = pwl_compositor_create_surface(w.compositor);
					if (surf)
					{	//yay! something worked for once!
						pwl_surface_attach(surf, buffer, 0, 0);
						pwl_surface_damage(surf, 0, 0, mips.mip[0].width, mips.mip[0].height);
						pwl_surface_commit(surf);
					}
					//can't destroy the buffer too early
				}
				pwl_shm_pool_destroy (pool);	//nor that
			}
			munmap(outdata, size);	//should be serverside by now
		}
		close (fd);	//nor that...
	}
	if (surf)
	{
		c = Z_Malloc(sizeof(*c));
		c->surf = surf;
		c->buf = buffer;
		c->hot_x = hotx;
		c->hot_y = hoty;
		c->width = mips.mip[0].width;
		c->height = mips.mip[0].height;
		return c;
	}
	if (buffer)
		pwl_buffer_destroy(buffer);	//don't need to track that any more

	//free it conversions required it.
	if (mips.mip[0].needfree)
		Z_Free(mips.mip[0].data);
	return NULL;
}
qboolean WL_SetCursor(void *cursor)
{
	struct cursorinfo_s *c = cursor;
	w.cursor = c;

	WL_SetHWCursor();	//update it
	return true;
}
void	 WL_DestroyCursor(void *cursor)
{
	struct cursorinfo_s *c = cursor;

	if (c == w.cursor)
	{	//unset the cursor if its active
		w.cursor = NULL;
		WL_SetHWCursor();
	}
	pwl_surface_destroy(c->surf);
	pwl_buffer_destroy(c->buf);
}

static void WL_pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	struct wdisplay_s *s = data;

	s->cursorfocus = true;
	s->cursorserial = serial;
	s->inputserial = serial;
	WL_SetHWCursor();
}
static void WL_pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
	struct wdisplay_s *s = data;
	s->cursorfocus = false;
	WL_SetHWCursor();
}

static void WL_pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct wdisplay_s *s = data;
	double x = wl_fixed_to_double(sx);
	double y = wl_fixed_to_double(sy);
	//wayland is shite shite shite.
	//1.4 still has no relative mouse motion.

	//track this for csd (surface units, as xdg_toplevel_show_window_menu wants them)
	s->mousex = x;
	s->mousey = y;

	//and let the game know, in buffer pixels.
	if (!s->relative_mouse_active)
		IN_MouseMove(0, true, x*WL_BufferScale(), y*WL_BufferScale() - s->csdsize, 0, 0);
}
static qboolean WL_PointerOnCaption(struct wdisplay_s *s)
{	//csdsize is buffer pixels
	return s->mousey*WL_BufferScale() < s->csdsize;
}

static void WL_pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	struct wdisplay_s *s = data;
	int qkey;

	s->inputserial = serial;
	switch(button)
	{
	default:
		return;	//blurgh.
	case BTN_LEFT:
		qkey = K_MOUSE1;
		if ((!s->relative_mouse_active||!vid.activeapp) && WL_PointerOnCaption(s))
		{
#ifdef XDG_SHELL_NAME
			if (s->xdg_toplevel)
				xdg_toplevel_move(s->xdg_toplevel, s->seat, serial);
#endif
#ifdef WL_SHELL_NAME
			if (s->ssurface)
				pwl_proxy_marshal((struct wl_proxy *) s->ssurface, WL_SHELL_SURFACE_MOVE, s->seat, serial);
#endif
			return;
		}
		break;
	case BTN_RIGHT:
		qkey = K_MOUSE2;
		if ((!s->relative_mouse_active||!vid.activeapp) && WL_PointerOnCaption(s))
		{
#ifdef XDG_SHELL_NAME
			if (s->xdg_toplevel)
				xdg_toplevel_show_window_menu(s->xdg_toplevel, s->seat, serial, s->mousex, s->mousey);
#endif
#ifdef WL_SHELL_NAME
			if (s->ssurface)
				pwl_proxy_marshal((struct wl_proxy *) s->ssurface, WL_SHELL_SURFACE_SET_MAXIMIZED, NULL);
#endif
			return;
		}
		break;
	case BTN_MIDDLE:
		qkey = K_MOUSE3;
		break;
	case BTN_SIDE:
		qkey = K_MOUSE4;
		break;
	case BTN_EXTRA:
		qkey = K_MOUSE5;
		break;
	case BTN_FORWARD:
		qkey = K_MOUSE6;
		break;
	case BTN_BACK:
		qkey = K_MOUSE7;
		break;
	case BTN_TASK:
		qkey = K_MOUSE8;
		break;
	}
	IN_KeyEvent(0, !!state, qkey, 0);
//		pwl_shell_surface_move(display->window->shell_surface, display->seat, serial);
}

static void WL_pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	if (value < 0)
	{
		IN_KeyEvent(0, 1, K_MWHEELUP, 0);
		IN_KeyEvent(0, 0, K_MWHEELUP, 0);
	}
	else
	{
		IN_KeyEvent(0, 1, K_MWHEELDOWN, 0);
		IN_KeyEvent(0, 0, K_MWHEELDOWN, 0);
	}
}

static const struct wl_pointer_listener pointer_listener =
{
	WL_pointer_handle_enter,
	WL_pointer_handle_leave,
	WL_pointer_handle_motion,
	WL_pointer_handle_button,
	WL_pointer_handle_axis,
};

#ifdef WP_RELATIVE_POINTER_MANAGER_NAME
struct zwp_relative_pointer_v1;
#define ZWP_RELATIVE_POINTER_MANAGER_V1_DESTROY 0
#define ZWP_RELATIVE_POINTER_MANAGER_V1_GET_RELATIVE_POINTER 1
#define ZWP_RELATIVE_POINTER_V1_DESTROY 0

static void WL_pointer_handle_delta(void *data, struct zwp_relative_pointer_v1 *pointer, uint32_t time_hi, uint32_t time_lo, wl_fixed_t dx_w, wl_fixed_t dy_w, wl_fixed_t dx_raw_w, wl_fixed_t dy_raw_w)
{
	if (w.relative_mouse_active)
	{
		double xmove = wl_fixed_to_double(dx_raw_w);
		double ymove = wl_fixed_to_double(dy_raw_w);
		IN_MouseMove(0, false, xmove, ymove, 0, 0);
	}
}
struct zwp_relative_pointer_v1_listener
{
	void (*delta)(void *data, struct zwp_relative_pointer_v1 *pointer, uint32_t time_hi, uint32_t time_lo, wl_fixed_t dx_w, wl_fixed_t dy_w, wl_fixed_t dx_raw_w, wl_fixed_t dy_raw_w);
};
static const struct zwp_relative_pointer_v1_listener relative_pointer_listener =
{
	WL_pointer_handle_delta,
};

static void WL_BindRelativePointerManager(struct wl_registry *registry, uint32_t id)
{	/*oh hey, I wrote lots of code! pay me more! fuck that shit.*/

	static const struct wl_interface *types[8];
	static const struct wl_message zwp_relative_pointer_manager_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_relative_pointer", "no", types + 6 },
	};
	static const struct wl_interface zwp_relative_pointer_manager_v1_interface = {
		"zwp_relative_pointer_manager_v1", 1,
		2, zwp_relative_pointer_manager_v1_requests,
		0, NULL,
	};
	static const struct wl_message zwp_relative_pointer_v1_requests[] = {
		{ "destroy", "", types + 0 },
	};
	static const struct wl_message zwp_relative_pointer_v1_events[] = {
		{ "relative_motion", "uuffff", types + 0 },
	};
	static const struct wl_interface zwp_relative_pointer_v1_interface = {
		"zwp_relative_pointer_v1", 1,
		1, zwp_relative_pointer_v1_requests,
		1, zwp_relative_pointer_v1_events,
	};

	//fix up types...
	types[6] = &zwp_relative_pointer_v1_interface;
	types[7] = pwl_pointer_interface;

	pzwp_relative_pointer_v1_interface = &zwp_relative_pointer_v1_interface;
	w.relative_pointer_manager = pwl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
}
#endif

#ifdef WP_POINTER_CONSTRAINTS_NAME
#define ZWP_POINTER_CONSTRAINTS_V1_DESTROY 0
#define ZWP_POINTER_CONSTRAINTS_V1_LOCK_POINTER 1
#define ZWP_POINTER_CONSTRAINTS_V1_CONFINE_POINTER 2
#define ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT 1
#define ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT 2
#define ZWP_LOCKED_POINTER_V1_DESTROY 0
#define ZWP_LOCKED_POINTER_V1_SET_CURSOR_POSITION_HINT 1
#define ZWP_LOCKED_POINTER_V1_SET_REGION 2
static void WL_pointer_handle_locked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
	struct wdisplay_s *s = data;
	s->relative_mouse_active = true;	//yay, everything works!...
	WL_SetHWCursor();
}
static void WL_pointer_handle_unlocked(void *data, struct zwp_locked_pointer_v1 *pointer)
{	//this is a one-shot. it gets destroyed automatically, but we still need to null it
	struct wdisplay_s *s = data;
	s->relative_mouse_active = false;	//probably the compositor killed it.
	WL_SetHWCursor();

//	s->locked_pointer = NULL;
}
struct zwp_locked_pointer_v1_listener
{
	void (*locked)(void *data, struct zwp_locked_pointer_v1 *pointer);
	void (*unlocked)(void *data, struct zwp_locked_pointer_v1 *pointer);
};
static const struct zwp_locked_pointer_v1_listener locked_pointer_listener =
{
	WL_pointer_handle_locked,
	WL_pointer_handle_unlocked,
};
static void WL_BindPointerConstraintsManager(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[14];
	static const struct wl_message zwp_pointer_constraints_v1_requests[] = {
		{"destroy", "", types+0},
		{"lock_pointer", "noo?ou", types+2},
		{"confine_pointer", "noo?ou", types+7},
	};
	static const struct wl_interface zwp_pointer_constraints_v1_interface = {
		"zwp_pointer_constraints_v1", 1,
		3, zwp_pointer_constraints_v1_requests,
		0, NULL,
	};

	static const struct wl_message zwp_locked_pointer_v1_requests[] = {
		{"destroy", "", types + 0},
		{"set_cursor_position_hint", "ff", types + 0},
		{"set_region", "?o", types + 12},
	};

	static const struct wl_message zwp_locked_pointer_v1_events[] = {
		{"locked", "", types + 0},
		{"unlocked", "", types + 0},
	};

	static const struct wl_interface zwp_locked_pointer_v1_interface = {
		"zwp_locked_pointer_v1", 1,
		3, zwp_locked_pointer_v1_requests,
		2, zwp_locked_pointer_v1_events,
	};

	types[2] = &zwp_locked_pointer_v1_interface;
	types[3] = pwl_surface_interface;
	types[4] = pwl_pointer_interface;
	types[5] = pwl_region_interface;
//	types[7] = &zwp_confined_pointer_v1_interfae;
	types[8] = pwl_surface_interface;
	types[9] = pwl_pointer_interface;
	types[10] = pwl_region_interface;
	types[12] = pwl_region_interface;
	types[13] = pwl_region_interface;

	pzwp_locked_pointer_v1_interface = &zwp_locked_pointer_v1_interface;
	w.pointer_constraints = pwl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
}
static inline int zwp_locked_pointer_v1_add_listener(struct zwp_locked_pointer_v1 *zwp_locked_pointer_v1, const struct zwp_locked_pointer_v1_listener *listener, void *data)		{return pwl_proxy_add_listener((struct wl_proxy *) zwp_locked_pointer_v1, (void (**)(void)) listener, data);}
static inline struct zwp_locked_pointer_v1 *zwp_pointer_constraints_v1_lock_pointer(struct zwp_pointer_constraints_v1 *zwp_pointer_constraints_v1, struct wl_surface *surface, struct wl_pointer *pointer, struct wl_region *region, uint32_t lifetime)		{return (struct zwp_locked_pointer_v1 *) pwl_proxy_marshal_constructor((struct wl_proxy *) zwp_pointer_constraints_v1, ZWP_POINTER_CONSTRAINTS_V1_LOCK_POINTER, pzwp_locked_pointer_v1_interface, NULL, surface, pointer, region, lifetime);}
#endif

static void WL_keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size)
{
	struct wdisplay_s *d = data;
	if (d->xkb_context)
	{
		struct xkb_keymap *keymap;
		char *keymap_string = mmap (NULL, size, PROT_READ, MAP_SHARED, fd, 0);
		if (keymap_string != MAP_FAILED)
		{
			keymap = pxkb_keymap_new_from_string (d->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
			munmap (keymap_string, size);
			pxkb_state_unref (d->xkb_state);
			d->xkb_state = pxkb_state_new (keymap);
			pxkb_keymap_unref (keymap);
		}
	}

	close(fd);
}

static void WL_keyboard_handle_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
	struct wdisplay_s *d = data;
	Con_DPrintf("Wayland: keyboard focus in\n");
	d->inputserial = serial;
	vid.activeapp = true;
}

static void WL_keyboard_handle_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
	struct wdisplay_s *d = data;
	Con_DPrintf("Wayland: keyboard focus out\n");
	d->repeat_held = false;
	vid.activeapp = false;
	IN_KeyEvent(0, false, -1, 0);	//aka: release all, so we're not left with alt held, etc.
}

static qboolean WL_KeyRepeats(struct wdisplay_s *d, uint32_t key, uint32_t qkey)
{
	if (d->xkb_state)
		return pxkb_keymap_key_repeats(pxkb_state_get_keymap(d->xkb_state), key+8);
	switch(qkey)
	{	//no keymap: everything but the modifiers and locks
	case 0:
	case K_LSHIFT: case K_RSHIFT: case K_LCTRL: case K_RCTRL: case K_LALT: case K_RALT:
	case K_LWIN: case K_RWIN: case K_CAPSLOCK: case K_KP_NUMLOCK: case K_SCRLCK:
		return false;
	default:
		return true;
	}
}
//the compositor sends no repeats: a held key repeats as further presses with no release between, like X11's autorepeat
static void WL_KeyRepeat(void)
{
	double now;
	int n;
	if (!w.repeat_held || w.repeat_rate <= 0)
		return;
	now = Sys_DoubleTime();
	for (n = 0; now >= w.repeat_next && n < 8; n++)
	{
		IN_KeyEvent(0, 1, w.repeat_qkey, w.repeat_ukey);
		w.repeat_next += 1.0/w.repeat_rate;
	}
	if (now >= w.repeat_next)
		w.repeat_next = now + 1.0/w.repeat_rate;	//we stalled (no frames while hidden): don't burst
}
static void WL_keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	struct wdisplay_s *d = data;
	Con_DPrintf("Wayland: key repeat %i/s after %ims\n", rate, delay);
	d->repeat_rate = rate;
	d->repeat_delay = delay;
	if (rate <= 0)
		d->repeat_held = false;
}

static void WL_keyboard_handle_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	extern int		shift_down;
	struct wdisplay_s *d = data;
	uint32_t qkey;
	uint32_t ukey;

	if (key < sizeof(waylandinputsucksbighairydonkeyballs)/sizeof(waylandinputsucksbighairydonkeyballs[0]))
		qkey = waylandinputsucksbighairydonkeyballs[key];
	else
		qkey = 0;
	if (!qkey)
		Con_DPrintf("WLScancode %#x has no mapping\n", key);

	if (d->xkb_context)
	{
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		{
			xkb_keysym_t keysym = pxkb_state_key_get_one_sym (d->xkb_state, key+8);
			ukey = pxkb_keysym_to_utf32 (keysym);
		}
		else
			ukey = 0;
	}
	else
	{
		//FIXME: this stuff is fucked, especially the ukey stuff.
		if (key < sizeof(waylandinputsucksbighairydonkeyballs)/sizeof(waylandinputsucksbighairydonkeyballs[0]))
		{
			if (shift_down)
				ukey = waylandinputsucksbighairydonkeyballsshift[key];
			else
				ukey = waylandinputsucksbighairydonkeyballs[key];
		}
		else
			ukey = 0;
		if (ukey < ' ' || ukey > 127)
			ukey = 0;
	}

	d->inputserial = serial;
	IN_KeyEvent(0, (state==WL_KEYBOARD_KEY_STATE_PRESSED)?1:0, qkey, ukey);

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
	{
		if (d->repeat_rate > 0 && (qkey || ukey) && WL_KeyRepeats(d, key, qkey))
		{	//a newer press takes over; a modifier press leaves the repeat running
			d->repeat_held = true;
			d->repeat_scancode = key;
			d->repeat_qkey = qkey;
			d->repeat_ukey = ukey;
			d->repeat_next = Sys_DoubleTime() + d->repeat_delay/1000.0;
		}
	}
	else if (key == d->repeat_scancode)
		d->repeat_held = false;
}

static void WL_keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
	struct wdisplay_s *s = data;
	pxkb_state_update_mask (s->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener =
{
	WL_keyboard_handle_keymap,
	WL_keyboard_handle_enter,
	WL_keyboard_handle_leave,
	WL_keyboard_handle_key,
	WL_keyboard_handle_modifiers,
	WL_keyboard_handle_repeat_info	//wl_seat v4
};
/*Patch 387: Wayland's grant (in_generic.c Patch 301/326). 1 = relative-pointer and pointer-constraints
  exist, so a LOCKED pointer reads unaccelerated deltas. Unlocked (menus, in_windowed_mouse 0, a refused
  lock) the view takes absolute, compositor-accelerated drag-look -- the same caveat as in_windowed_mouse 0
  on X11 and Windows. One merged seat: injection cannot be told apart, so the counters stay -1.*/
qboolean WL_PublishGrant(void)
{
	if (!w.display)
		return false;
	in_rawmice_live = (w.pointer && w.relative_pointer && w.pointer_constraints)?1:0;
	in_rawkbd_live = 0;
	return true;
}
qboolean WL_EnumerateDevices(void *ctx, void(*callback)(void *ctx, const char *type, const char *devicename, unsigned int *qdevid))
{
	if (!w.display)
		return false;
	callback(ctx, "keyboard", "wayland", NULL);
	if (w.pointer)	//NULL: every Wayland pointer event is sent as devid 0, so in_deviceids must say it cannot be remapped
		callback(ctx, "mouse", (in_rawmice_live > 0)?"wayland-relative-pointer":"wayland", NULL);
	return true;
}
static void WL_seat_handle_capabilities(void *data, struct wl_seat *seat, enum wl_seat_capability caps)
{
	struct wdisplay_s *s = data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s->pointer)
	{
		s->pointer = pwl_seat_get_pointer(seat);
		pwl_pointer_add_listener(s->pointer, &pointer_listener, s);

		if (w.relative_pointer_manager)
		{	//and try and get relative pointer events too. so much fucking boilerplate.
			w.relative_pointer = (struct zwp_relative_pointer_v1 *)pwl_proxy_marshal_constructor((struct wl_proxy *) w.relative_pointer_manager, ZWP_RELATIVE_POINTER_MANAGER_V1_GET_RELATIVE_POINTER, pzwp_relative_pointer_v1_interface, NULL, w.pointer);
			pwl_proxy_add_listener((struct wl_proxy *) w.relative_pointer, (void (**)(void)) &relative_pointer_listener, &w);
		}
	}
	else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && s->pointer)
	{
		WL_DropCursorShapeDevice();
		s->cursorfocus = false;
		pwl_pointer_destroy(s->pointer);
		s->pointer = NULL;

		if (w.relative_pointer)
		{
			pwl_proxy_marshal((struct wl_proxy *) w.relative_pointer, ZWP_RELATIVE_POINTER_V1_DESTROY);
			pwl_proxy_destroy((struct wl_proxy *) w.relative_pointer);
			w.relative_pointer = NULL;	//Patch 387: was left dangling
		}
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s->keyboard)
	{
		s->keyboard = pwl_seat_get_keyboard(seat);
		pwl_keyboard_add_listener(s->keyboard, &keyboard_listener, s);
	}
	else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && s->keyboard)
	{
		pwl_keyboard_destroy(s->keyboard);
		s->keyboard = NULL;
		s->repeat_held = false;
	}
	WL_PublishGrant();	//Patch 387
}
static void WL_seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
}
static const struct wl_seat_listener seat_listener =
{
	WL_seat_handle_capabilities,
	WL_seat_handle_name	//wl_seat v2
};

//clipboard: wl_data_device's selection, CBT_CLIPBOARD only (gl_vidlinuxglx.c's Sys_Clipboard_PasteText/Sys_SaveClipboard call in)
static const char *wl_textmimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING"};	//best first
static const char *WL_SelfMime(void)
{	//our sources also offer this, so a selection is known to be ours (not another copy of the game's) by its offer alone.
	//A random id, not the pid: PID namespaces (Flatpak) repeat pids, and clipboard managers keep a dead instance's offer.
	static char mime[64];
	if (!*mime)
	{
		unsigned int id[2];
#ifdef HAVE_GETRANDOM
		if (getrandom(id, sizeof(id), GRND_NONBLOCK) != sizeof(id))
#endif
		{
			id[0] = (unsigned int)time(NULL) ^ (unsigned int)getpid();
			id[1] = (unsigned int)(Sys_DoubleTime()*1000000);
		}
		Q_snprintfz(mime, sizeof(mime), "application/x-ftesurf-self;id=%08x%08x", id[0], id[1]);
	}
	return mime;
}
static void WL_DestroyOffer(struct wl_data_offer **offer)
{
	if (!*offer)
		return;
	if (*offer == w.newoffer)
		w.newoffer = NULL;
	pwl_proxy_marshal((struct wl_proxy *) *offer, WL_DATA_OFFER_DESTROY);
	pwl_proxy_destroy((struct wl_proxy *) *offer);
	*offer = NULL;
}
static void WL_data_offer_handle_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
	int i;
	if (offer != w.newoffer)
		return;
	if (!strcmp(mime, WL_SelfMime()))
		w.newoffer_self = true;
	for (i = 0; i < countof(wl_textmimes); i++)
		if (!strcmp(mime, wl_textmimes[i]) && (!w.newoffer_mime || i+1 < w.newoffer_mime))
			w.newoffer_mime = i+1;
}
static const struct wl_data_offer_listener data_offer_listener =
{	//wl_data_device_manager is bound at version 1
	WL_data_offer_handle_offer,
};
static void WL_data_device_handle_data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
	w.newoffer = offer;
	w.newoffer_mime = 0;
	w.newoffer_self = false;
	pwl_proxy_add_listener((struct wl_proxy *) offer, (void (**)(void)) &data_offer_listener, &w);
}
static void WL_data_device_handle_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer)
{	//we take no drops; the offer is only kept to be destroyed
	WL_DestroyOffer(&w.dnd_offer);
	w.dnd_offer = offer;
}
static void WL_data_device_handle_leave(void *data, struct wl_data_device *device)
{
	WL_DestroyOffer(&w.dnd_offer);
}
static void WL_data_device_handle_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}
static void WL_data_device_handle_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
	if (w.selection_offer != offer)
		WL_DestroyOffer(&w.selection_offer);
	w.selection_offer = offer;
	w.selection_mime = (offer && offer == w.newoffer)?w.newoffer_mime:0;
	w.selection_self = (offer && offer == w.newoffer)?w.newoffer_self:false;
}
static const struct wl_data_device_listener data_device_listener =
{
	WL_data_device_handle_data_offer,
	WL_data_device_handle_enter,
	WL_data_device_handle_leave,
	WL_data_device_handle_motion,
	WL_data_device_handle_leave,	//drop: no leave follows it
	WL_data_device_handle_selection,
};
static void WL_data_source_handle_target(void *data, struct wl_data_source *source, const char *mime)
{
}
static void WL_data_source_handle_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{	//non-blocking with 100 ms in all: a reader that stops reading must not stall the game
	const char *t = w.cliptext?w.cliptext:"";
	size_t len = strcmp(mime, WL_SelfMime())?strlen(t):0;
	double deadline = Sys_DoubleTime() + 0.1;
	struct pollfd p;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	while (len)
	{
		ssize_t r = write(fd, t, len);
		int ms = (deadline - Sys_DoubleTime())*1000;
		if (r > 0)
		{
			t += r;
			len -= r;
			continue;
		}
		if (ms <= 0)
		{
			Con_DPrintf("Wayland: clipboard send cut short by its 100 ms budget, %u bytes unsent\n", (unsigned int)len);
			break;
		}
		if (r < 0 && errno == EINTR)
			continue;
		p.fd = fd;
		p.events = POLLOUT;
		if (r < 0 && errno == EAGAIN && (poll(&p, 1, ms) >= 0 || errno == EINTR))
			continue;	//writable, interrupted, or timed out (and the next pass says so)
		break;	//an error, such as the reader closing
	}
	close(fd);
}
static void WL_data_source_handle_cancelled(void *data, struct wl_data_source *source)
{	//someone else took the selection (or our replacement did)
	if (source == w.data_source)
	{
		w.data_source = NULL;
		Z_Free(w.cliptext);
		w.cliptext = NULL;
	}
	pwl_proxy_marshal((struct wl_proxy *) source, WL_DATA_SOURCE_DESTROY);
	pwl_proxy_destroy((struct wl_proxy *) source);
}
static const struct wl_data_source_listener data_source_listener =
{
	WL_data_source_handle_target,
	WL_data_source_handle_send,
	WL_data_source_handle_cancelled,
};
//false: not ours to answer (no Wayland clipboard, the primary selection, or the selection is this process's), so the caller uses its own copy
qboolean WL_Clipboard_PasteText(clipboardtype_t cbt, void (*callback)(void *ctx, const char *utf8), void *ctx)
{
	int fds[2];
	char *text = NULL;
	size_t len = 0, cap = 0;
	ssize_t r;
	struct pollfd p;
	double deadline;
	int ms;

	if (!w.display || cbt != CBT_CLIPBOARD || !w.data_device || w.selection_self)
		return false;	//(reading our own source would wait on ourselves)
	if (!w.selection_offer || !w.selection_mime || pipe(fds) < 0)
	{
		callback(ctx, NULL);
		return true;
	}
	pwl_proxy_marshal((struct wl_proxy *) w.selection_offer, WL_DATA_OFFER_RECEIVE, wl_textmimes[w.selection_mime-1], fds[1]);
	close(fds[1]);	//the request carries its own dup
	pwl_display_flush(w.display);

	p.fd = fds[0];
	p.events = POLLIN;
	deadline = Sys_DoubleTime() + 1;	//a source that never finishes costs a second in all, not a hang
	while (len < (1u<<20) && (ms = (deadline - Sys_DoubleTime())*1000) > 0)
	{
		int n = poll(&p, 1, ms);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (len+4096 >= cap)
		{
			char *grown = realloc(text, cap = len+4096+1);
			if (!grown)
				break;
			text = grown;
		}
		r = read(fds[0], text+len, 4096);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += r;
	}
	close(fds[0]);
	if (text)
		text[len] = 0;
	callback(ctx, text);
	free(text);
	return true;
}
void WL_SaveClipboard(clipboardtype_t cbt, const char *text)
{
	int i;
	struct wl_data_source *source;
	if (!w.display || cbt != CBT_CLIPBOARD || !w.data_device || !w.inputserial)
		return;	//no input serial yet: set_selection would be refused, so only the caller's own copy changes
	Z_StrDupPtr(&w.cliptext, text);
	source = (struct wl_data_source *) pwl_proxy_marshal_constructor((struct wl_proxy *) w.data_device_manager, WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE, pwl_data_source_interface, NULL);
	for (i = 0; i < countof(wl_textmimes); i++)
		pwl_proxy_marshal((struct wl_proxy *) source, WL_DATA_SOURCE_OFFER, wl_textmimes[i]);
	pwl_proxy_marshal((struct wl_proxy *) source, WL_DATA_SOURCE_OFFER, WL_SelfMime());
	pwl_proxy_add_listener((struct wl_proxy *) source, (void (**)(void)) &data_source_listener, &w);
	pwl_proxy_marshal((struct wl_proxy *) w.data_device, WL_DATA_DEVICE_SET_SELECTION, source, w.inputserial);
	w.data_source = source;	//the previous one, if any, is told 'cancelled'
}


#ifdef XDG_DECORATION_MANAGER_NAME
static const struct wl_interface *pzxdg_toplevel_decoration_v1_interface;
static void WL_BindDecorationManager(struct wl_registry *registry, uint32_t id)
{	/*oh hey, I wrote lots of code! pay me more! fuck that shit.*/
	static const struct wl_interface *types[3];
	static const struct wl_message zxdg_decoration_manager_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "get_toplevel_decoration", "no", types + 1 },
	};
	static const struct wl_interface zxdg_decoration_manager_v1_interface = {
		"zxdg_decoration_manager_v1", 1,
		2, zxdg_decoration_manager_v1_requests,
		0, NULL,
	};
	static const struct wl_message zxdg_toplevel_decoration_v1_requests[] = {
		{ "destroy", "", types + 0 },
		{ "set_mode", "u", types + 0 },
		{ "unset_mode", "", types + 0 },
	};
	static const struct wl_message zxdg_toplevel_decoration_v1_events[] = {
		{ "configure", "u", types + 0 },
	};
	static const struct wl_interface zxdg_toplevel_decoration_v1_interface = {
		"zxdg_toplevel_decoration_v1", 1,
		3, zxdg_toplevel_decoration_v1_requests,
		1, zxdg_toplevel_decoration_v1_events,
	};

	//fix up types...
	types[1] = &zxdg_toplevel_decoration_v1_interface;
	types[2] = pxdg_toplevel_interface;

	pzxdg_toplevel_decoration_v1_interface = &zxdg_toplevel_decoration_v1_interface;
	w.xdg_decoration_manager = pwl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
}

enum xdg_decoration_manager_mode {
    ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE = 1,
    ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE = 2,
};
#define ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION 1
static inline struct zxdg_toplevel_decoration_v1 *zxdg_decoration_manager_v1_get_toplevel_decoration(struct zxdg_decoration_manager_v1 *zxdg_decoration_manager_v1, struct xdg_toplevel *toplevel)		{ return (struct zxdg_toplevel_decoration_v1 *) pwl_proxy_marshal_constructor((struct wl_proxy *) zxdg_decoration_manager_v1, ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION, pzxdg_toplevel_decoration_v1_interface, NULL, toplevel); }

#define ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE 1
struct zxdg_toplevel_decoration_v1_listener {
    void (*configure)(void *data, struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1, uint32_t mode);
};
static inline void zxdg_toplevel_decoration_v1_set_mode(struct zxdg_toplevel_decoration_v1 *xdg_decoration, uint32_t mode)		{ pwl_proxy_marshal((struct wl_proxy *) xdg_decoration, ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE, mode); }
static inline int zxdg_toplevel_decoration_v1_add_listener(struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1, const struct zxdg_toplevel_decoration_v1_listener *listener, void *data)	{ return pwl_proxy_add_listener((struct wl_proxy *) zxdg_toplevel_decoration_v1, (void (**)(void)) listener, data); }

static void xdg_decoration_mode(void *data, struct zxdg_toplevel_decoration_v1 *xdg_decoration, uint32_t mode)
{
	struct wdisplay_s *d = data;
	Con_DPrintf("Wayland: xdg-decoration mode %s\n", (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)?"server-side":"client-side");
	d->hasssd = (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}
static const struct zxdg_toplevel_decoration_v1_listener myxdg_decoration_listener =
{
	xdg_decoration_mode
};

#endif

#ifdef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME
static const struct wl_interface *porg_kde_kwin_server_decoration_interface;
static void WL_BindKWinDecorationManager(struct wl_registry *registry, uint32_t id)
{
	static const struct wl_interface *types[3];
	static const struct wl_message org_kde_kwin_server_decoration_manager_requests[] = {
		{ "create", "no", types + 1 },
	};
	static const struct wl_message org_kde_kwin_server_decoration_manager_events[] = {
		{ "default_mode", "u", types + 0 },
	};
	static const struct wl_interface org_kde_kwin_server_decoration_manager_interface = {
		"org_kde_kwin_server_decoration_manager", 1,
		1, org_kde_kwin_server_decoration_manager_requests,
		1, org_kde_kwin_server_decoration_manager_events,
	};
	static const struct wl_message org_kde_kwin_server_decoration_requests[] = {
		{ "release", "", types + 0 },
		{ "request_mode", "u", types + 0 },
	};
	static const struct wl_message org_kde_kwin_server_decoration_events[] = {
		{ "mode", "u", types + 0 },
	};
	static const struct wl_interface org_kde_kwin_server_decoration_interface = {
		"org_kde_kwin_server_decoration", 1,
		2, org_kde_kwin_server_decoration_requests,
		1, org_kde_kwin_server_decoration_events,
	};

	//fix up types...
	types[1] = &org_kde_kwin_server_decoration_interface;
	types[2] = pwl_surface_interface;

	porg_kde_kwin_server_decoration_interface = &org_kde_kwin_server_decoration_interface;
	w.kwin_decoration_manager = pwl_registry_bind(registry, id, &org_kde_kwin_server_decoration_manager_interface, 1);
}
enum org_kde_kwin_server_decoration_manager_mode {
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_NONE = 0,
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT = 1,
    ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER = 2,
};
#define ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_CREATE 0
static inline struct org_kde_kwin_server_decoration *org_kde_kwin_server_decoration_manager_create(struct org_kde_kwin_server_decoration_manager *org_kde_kwin_server_decoration_manager, struct wl_surface *surface)		{ return (struct org_kde_kwin_server_decoration *) pwl_proxy_marshal_constructor((struct wl_proxy *) org_kde_kwin_server_decoration_manager, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_CREATE, porg_kde_kwin_server_decoration_interface, NULL, surface); }
#define ORG_KDE_KWIN_SERVER_DECORATION_REQUEST_MODE 1
struct org_kde_kwin_server_decoration_listener {
    void (*mode)(void *data, struct org_kde_kwin_server_decoration *org_kde_kwin_server_decoration, uint32_t mode);
};
static inline void org_kde_kwin_server_decoration_request_mode(struct org_kde_kwin_server_decoration *org_kde_kwin_server_decoration, uint32_t mode)		{ pwl_proxy_marshal((struct wl_proxy *) org_kde_kwin_server_decoration, ORG_KDE_KWIN_SERVER_DECORATION_REQUEST_MODE, mode); }
static inline int org_kde_kwin_server_decoration_add_listener(struct org_kde_kwin_server_decoration *org_kde_kwin_server_decoration, const struct org_kde_kwin_server_decoration_listener *listener, void *data)		{ return pwl_proxy_add_listener((struct wl_proxy *) org_kde_kwin_server_decoration, (void (**)(void)) listener, data); }

static void kwin_decoration_mode(void *data, struct org_kde_kwin_server_decoration *org_kde_kwin_server_decoration, uint32_t mode)
{
	struct wdisplay_s *d = data;
	d->hasssd = (mode != ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_CLIENT);
}
static struct org_kde_kwin_server_decoration_listener myorg_kde_kwin_server_decoration_listener =
{
	kwin_decoration_mode
};
#endif

static void WL_handle_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
{
	struct wdisplay_s *d = data;
Con_DLPrintf(0, "Wayland Interface %s id %u version %u\n", interface, id, version);
	if (strcmp(interface, "wl_compositor") == 0)
	{	//v3: wl_surface.set_buffer_scale, v6: wl_surface.preferred_buffer_scale
		d->compositor_version = WL_BindVersion(pwl_compositor_interface, version, 6);
		d->compositor = pwl_registry_bind(registry, id, pwl_compositor_interface, d->compositor_version);
	}
	else if (strcmp(interface, "wl_shm") == 0)
		d->shm = pwl_registry_bind(registry, id, pwl_shm_interface, WL_BindVersion(pwl_shm_interface, version, 1));
#ifdef WL_SHELL_NAME
	else if (strcmp(interface, WL_SHELL_NAME) == 0)
		d->shell = pwl_registry_bind(registry, id, pwl_shell_interface, WL_BindVersion(pwl_shell_interface, version, 1));
#endif
#ifdef XDG_SHELL_NAME
	else if (strcmp(interface, XDG_SHELL_NAME) == 0 && version > 1)	//old versions of kwin are buggy, sidestep it.
		d->xdg_wm_base = pwl_registry_bind(registry, id, pxdg_wm_base_interface, 2);
#endif
	else if (strcmp(interface, "wl_seat") == 0 && !d->seat)
	{	//v4 for wl_keyboard.repeat_info; v5 would add wl_pointer events the listener lacks
		d->seat = pwl_registry_bind(registry, id, pwl_seat_interface, WL_BindVersion(pwl_seat_interface, version, 4));
		pwl_seat_add_listener(d->seat, &seat_listener, d);
	}
	else if (strcmp(interface, "wl_output") == 0)
		WL_BindOutput(registry, id, version);
	else if (strcmp(interface, "wl_data_device_manager") == 0)
		d->data_device_manager = pwl_registry_bind(registry, id, pwl_data_device_manager_interface, WL_BindVersion(pwl_data_device_manager_interface, version, 1));
	else if (strcmp(interface, WP_FRACTIONAL_SCALE_MANAGER_NAME) == 0)
		WL_BindFractionalScaleManager(registry, id);
	else if (strcmp(interface, WP_VIEWPORTER_NAME) == 0)
		WL_BindViewporter(registry, id);
	else if (strcmp(interface, WP_TEARING_CONTROL_MANAGER_NAME) == 0)
		WL_BindTearingControlManager(registry, id);
	else if (strcmp(interface, WP_CURSOR_SHAPE_MANAGER_NAME) == 0)
		WL_BindCursorShapeManager(registry, id);
#ifdef XDG_DECORATION_MANAGER_NAME
	else if (strcmp(interface, XDG_DECORATION_MANAGER_NAME) == 0)
		WL_BindDecorationManager(registry, id);
#endif
#ifdef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME
	else if (strcmp(interface, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME) == 0)
		WL_BindKWinDecorationManager(registry, id);
#endif
#ifdef WP_RELATIVE_POINTER_MANAGER_NAME
	else if (strcmp(interface, WP_RELATIVE_POINTER_MANAGER_NAME) == 0)
		WL_BindRelativePointerManager(registry, id);
#endif
#ifdef WP_POINTER_CONSTRAINTS_NAME
	else if (strcmp(interface, WP_POINTER_CONSTRAINTS_NAME) == 0)
		WL_BindPointerConstraintsManager(registry, id);
#endif
/*	else if (!strcmp(interface, "input_device"))
		display_add_input(id);
*/
}

static void WL_handle_global_remove(void *data, struct wl_registry *wl_registry, uint32_t id)
{
	int i;
	for (i = 0; i < countof(w.outputs); i++)
	{
		if (w.outputs[i].output && w.outputs[i].id == id)
		{	//a monitor went away
			pwl_proxy_destroy((struct wl_proxy *) w.outputs[i].output);
			memset(&w.outputs[i], 0, sizeof(w.outputs[i]));
			WL_UpdateIntScale();
			return;
		}
	}
	Con_Printf("WL_handle_global_remove: %u\n", id);
}

static const struct wl_registry_listener WL_registry_listener = {
	WL_handle_global,
	WL_handle_global_remove
};

//P1602: dispatch what arrives within timeoutms. wl_display_dispatch blocks until an event comes, which stalled
//the whole host (network included) while a hidden surface got no frame callbacks.
static qboolean WL_PumpEvents(int timeoutms)
{
	struct pollfd pfd;
	while (pwl_display_prepare_read(w.display) != 0)
		if (pwl_display_dispatch_pending(w.display) < 0)
			return false;
	pwl_display_flush(w.display);
	pfd.fd = pwl_display_get_fd(w.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeoutms) > 0)
	{
		if (pwl_display_read_events(w.display) < 0)
			return false;
	}
	else
		pwl_display_cancel_read(w.display);
	return pwl_display_dispatch_pending(w.display) >= 0;
}
qboolean WL_MayRefresh(void)
{
	if (w.wait_for_configure || w.waylandisblocking)
	{
		if (!WL_PumpEvents(1))
		{
			Sys_Sleep(2);
			Con_Printf(CON_ERROR "wayland connection was lost. Restarting video\n");
			Cbuf_InsertText("vid_restart", RESTRICT_LOCAL, true);
			return false;
		}
		//wlroots sends frame callbacks only to visible surfaces: past 100 ms draw anyway (~10 fps while hidden)
		if (w.waylandisblocking && Sys_DoubleTime() - w.frameasked > 0.1)
			w.waylandisblocking = false;
		return !w.wait_for_configure && !w.waylandisblocking;
	}
	return true;
}
static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t time)
{
	pwl_proxy_destroy((struct wl_proxy *)callback);
	if (w.framecb == callback)
		w.framecb = NULL;
	w.waylandisblocking = false;
}
static const struct wl_callback_listener frame_listener = {
	.done = frame_handle_done,
};

static void WL_SwapBuffers(void)
{
	extern cvar_t vid_vsync;
	qboolean wantabs;
	TRACE(("WL_SwapBuffers\n"));

	WL_UpdateTearing();	//rides on the swap's commit
	if (!w.statesynced && w.xdg_toplevel)
	{	//P1611: R_ApplyRenderer copies the requested state over currentrendererstate after WL_Init, wiping what an
		//init-time configure said (a window rule forcing fullscreen); read it back once, at the first swap.
		w.statesynced = true;
		if (w.fullscreen != !!currentrendererstate.fullscreen)
			currentrendererstate.fullscreen = w.fullscreen?2:0;
	}
	switch(qrenderer)
	{
#if defined(GLQUAKE) && defined(USE_EGL)
	case QR_OPENGL:
		if (w.csdsize && font_default)
		{
			unsigned int vw=vid.width;
			unsigned int vh=vid.height;
			unsigned int rw=vid.rotpixelwidth;
			unsigned int rh=vid.rotpixelheight;
			Matrix4x4_CM_Orthographic(r_refdef.m_projection_std, 0, vid.pixelwidth, w.csdsize, 0, -99999, 99999);
			Matrix4x4_Identity(r_refdef.m_view);
			qglViewport(0, vid.pixelheight, vid.pixelwidth, w.csdsize);
			GL_SetShaderState2D(true);	//responsible for loading the new projection matrix
			vid.rotpixelwidth = vid.width = vid.pixelwidth;
			vid.rotpixelheight = vid.height = w.csdsize;
			R2D_ImageColours(0.05, 0.05, 0.1, 1);
			R2D_FillBlock(0, 0, vid.pixelwidth, w.csdsize);
			R2D_ImageColours(1, 1, 1, 1);
			Draw_FunStringWidth(0, 4, w.csdcaption, vid.pixelwidth, 2, vid.activeapp);
			vid.width=vw;
			vid.height=vh;
			vid.rotpixelwidth=rw;
			vid.rotpixelheight=rh;
		}
		if (R2D_Flush)
			R2D_Flush();

		if (!*vid_vsync.string || vid_vsync.ival > 0)	//P1602: vsync off draws unthrottled, as GLX does; the frame callback capped it at the refresh
		{
			if (!w.framecb)	//a hidden surface would otherwise queue one per frame
			{
				// Register a frame callback to know when we need to draw the next frame
				w.framecb = pwl_surface_frame(w.surface);
				pwl_callback_add_listener(w.framecb, &frame_listener, NULL);
			}
			w.waylandisblocking = true;
			w.frameasked = Sys_DoubleTime();
		}

		EGL_SwapBuffers();
		//wl_surface_damage(w.surface, 0, 0, vid.pixelwidth, vid.pixelheight);
		if (!WL_PumpEvents(0))	//read too: with vsync off WL_MayRefresh never waits, so nothing else reads the socket
		{
			Con_Printf(CON_ERROR "wayland connection was lost. Restarting video\n");
			Cbuf_InsertText("vid_restart", RESTRICT_LOCAL, true);
			return;
		}
		if (w.hasssd)
			w.csdsize = 0;	//kde's implementation-specific/legacy extension doesn't need us to draw crappy lame CSD junk
		else if (w.fullscreen)
			w.csdsize = 0;	//P1611: a fullscreen window has no title bar (GNOME drew ours across the top of the menus)
		else if (vid.activeapp && w.relative_mouse_active)
			w.csdsize = 0;	//kill the csd while using relative mouse coords.
		else
			w.csdsize = Font_CharPHeight(font_default)+8;

		if (w.bufheight && vid.pixelheight != w.bufheight-w.csdsize)
		{
			vid.pixelheight = w.bufheight-w.csdsize;
			Cvar_ForceCallback(&vid_conautoscale);
		}
		break;
#endif
	case QR_VULKAN:
		if (!WL_PumpEvents(0))
		{
			Con_Printf(CON_ERROR "wayland connection was lost. Restarting video\n");
			Cbuf_InsertText("vid_restart", RESTRICT_LOCAL, true);
			return;
		}
		break;
	default:
		break;
	}
	WL_KeyRepeat();

	//if the game wants absolute mouse positions...
	wantabs = !vid.activeapp || (!vrui.enabled && Key_MouseShouldBeFree()) || !in_windowed_mouse.value;
	//and force it on if we're lacking one of the plethora of extensions that were needed to get the damn thing actually usable.
	wantabs |= !w.relative_pointer || !w.pointer_constraints;
	if (!wantabs && w.cursorfocus && !w.locked_pointer && w.pointer_constraints)
	{
		w.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(w.pointer_constraints, w.surface, w.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(w.locked_pointer, &locked_pointer_listener, &w);
	}
	if (wantabs && w.locked_pointer)
	{
		pwl_proxy_marshal((struct wl_proxy *) w.locked_pointer, ZWP_LOCKED_POINTER_V1_DESTROY);
		pwl_proxy_destroy((struct wl_proxy *) w.locked_pointer);
		w.locked_pointer = NULL;
		//P1602: destroying the lock sends no `unlocked`, so this is the only place relative mode ends; the old
		//test here was inverted and the cursor stayed hidden after mouselook until the pointer left and re-entered.
		w.relative_mouse_active = false;
		WL_SetHWCursor();
	}
}

#ifdef VKQUAKE
static qboolean WLVK_SetupSurface(void)
{
	VkWaylandSurfaceCreateInfoKHR inf = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    inf.flags = 0;
    inf.display = w.display;
    inf.surface = w.surface;

    if (VK_SUCCESS == vkCreateWaylandSurfaceKHR(vk.instance, &inf, vkallocationcb, &vk.surface))
        return true;
    return false;
}
#endif

extern qboolean (*VID_SetFullscreenLive)(int fullscreen, int width, int height);	//renderer.c, P1611
extern int VID_WindowMaximizePolicy(void);
//P1611: F11 / Alt+Enter / vid_toggle on the live window: Wayland fullscreen is a window state, not a video mode,
//so the compositor's configure resizes us (WL_ApplySize) and nothing restarts. OpenGL only: the Vulkan path never
//rebuilds its swapchain on a resize. Why and measurements: ENGINE_PATCHES.md Patch 1611.
static qboolean WL_SetFullscreenLive(int fullscreen, int width, int height)
{
#ifdef XDG_SHELL_NAME
	if (!w.xdg_toplevel || qrenderer != QR_OPENGL)
		return false;
	w.winwidth = w.winheight = 0;
	if (fullscreen)
		xdg_toplevel_set_fullscreen(w.xdg_toplevel, NULL);
	else
	{
		xdg_toplevel_unset_fullscreen(w.xdg_toplevel);
		if (VID_WindowMaximizePolicy() == 2)
			xdg_toplevel_set_maximized(w.xdg_toplevel);	//windowed means maximized, as on Windows (Patch 268)
		else
		{	//exact size, in the same units WL_Init takes info->width in
			xdg_toplevel_unset_maximized(w.xdg_toplevel);
			w.winwidth = width;
			w.winheight = height;
		}
	}
	pwl_display_flush(w.display);
	return true;
#else
	return false;
#endif
}

static qboolean WL_NameAndShame(void)
{
	VID_SetFullscreenLive = WL_SetFullscreenLive;	//P1611: the renderer is up; WL_DeInit takes it away again
	WL_PublishGrant();	//Patch 387
	//called after the renderer has been initialised, so these messages should be more prominant.
	if (!w.relative_pointer)
		Con_Printf(CON_WARNING "WARNING: Wayland server does not support %s, "CON_ERROR"mouse grabs are not supported\n", WP_RELATIVE_POINTER_MANAGER_NAME);
	else if (!w.pointer_constraints)
		Con_Printf(CON_WARNING "WARNING: Wayland server does not support %s, "CON_ERROR"mouse grabs are not supported\n", WP_POINTER_CONSTRAINTS_NAME);

	if (!w.hasssd)
		Con_Printf(CON_WARNING "WARNING: Wayland server does not support window decorations\n");
	return true;
}

static qboolean WL_Init (rendererstate_t *info, unsigned char *palette)
{
#ifdef DYNAMIC_WAYLAND
	if (!WL_InitLibrary())
	{
		Con_Printf("couldn't load wayland client libraries\n");
		return false;
	}
#endif
	WL_Setup_XDG_Shell();

	switch(qrenderer)
	{
#if defined(GLQUAKE) && defined(USE_EGL)
	case QR_OPENGL:
#ifdef DYNAMIC_WAYLAND
		lib_wayland_egl = Sys_LoadLibrary("libwayland-egl.so.1", waylandexports_egl);
		if (!lib_wayland_egl)
		{
			Con_Printf("couldn't load libwayland-egl.so.1 library\n");
			return false;
		}
#endif

//		setenv("EGL_PLATFORM", "wayland", 1);	//if this actually matters then we're kinda screwed
		if (!EGL_LoadLibrary(info->subrenderer))
		{
			Con_Printf("couldn't load EGL library\n");
			return false;
		}

		break;
#endif
#ifdef VKQUAKE
	case QR_VULKAN:
		#ifdef VK_NO_PROTOTYPES
		{	//vulkan requires that vkGetInstanceProcAddr is set in advance.
			dllfunction_t func[] =
			{
				{(void*)&vkGetInstanceProcAddr,	"vkGetInstanceProcAddr"},
				{NULL,							NULL}
			};

			if (!Sys_LoadLibrary("libvulkan.so.1", func))
			{
				if (!Sys_LoadLibrary("libvulkan.so", func))
				{
					Con_Printf("Couldn't intialise libvulkan.so\nvulkan loader is not installed\n");
					return false;
				}
			}
		}
		#endif
		break;
#endif
	default:
		return false;	//not supported dude...
	}

	memset(&w, 0, sizeof(w));
	w.tearing_hint = -1;

#ifdef DYNAMIC_WAYLAND
	if (WL_InitLibraryXKB())
#endif
		w.xkb_context = pxkb_context_new(XKB_CONTEXT_NO_FLAGS);

	w.csdsize = 0;
	w.display = pwl_display_connect(NULL);
	if (!w.display)
	{
		Con_Printf("couldn't connect to wayland server\n");
		return false;
	}
	w.registry = pwl_display_get_registry(w.display);
	pwl_registry_add_listener(w.registry, &WL_registry_listener, &w);
	pwl_display_dispatch(w.display);
	pwl_display_roundtrip(w.display);
	if (!w.compositor)
	{
		Con_Printf("no compositor running, apparently\n");
		return false;
	}

	w.surface = pwl_compositor_create_surface(w.compositor);
	if (!w.surface)
	{
		Con_Printf("no compositor running, apparently\n");
		return false;
	}
	pwl_proxy_add_listener((struct wl_proxy *) w.surface, (void (**)(void)) surface_listener, &w);
	if (w.fractional_scale_manager && w.viewporter)
	{	//preferred_scale only works with a viewport to scale the buffer back down
		w.fractional_scale = (struct wp_fractional_scale_v1 *) pwl_proxy_marshal_constructor((struct wl_proxy *) w.fractional_scale_manager, WP_FRACTIONAL_SCALE_MANAGER_V1_GET_FRACTIONAL_SCALE, pwp_fractional_scale_v1_interface, NULL, w.surface);
		pwl_proxy_add_listener((struct wl_proxy *) w.fractional_scale, (void (**)(void)) &fractional_scale_listener, &w);
	}
	if (w.viewporter)
		w.viewport = (struct wp_viewport *) pwl_proxy_marshal_constructor((struct wl_proxy *) w.viewporter, WP_VIEWPORTER_GET_VIEWPORT, pwp_viewport_interface, NULL, w.surface);
	if (w.tearing_control_manager)
		w.tearing_control = (struct wp_tearing_control_v1 *) pwl_proxy_marshal_constructor((struct wl_proxy *) w.tearing_control_manager, WP_TEARING_CONTROL_MANAGER_V1_GET_TEARING_CONTROL, pwp_tearing_control_v1_interface, NULL, w.surface);
	if (w.data_device_manager && w.seat)
	{
		w.data_device = (struct wl_data_device *) pwl_proxy_marshal_constructor((struct wl_proxy *) w.data_device_manager, WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE, pwl_data_device_interface, NULL, w.seat);
		pwl_proxy_add_listener((struct wl_proxy *) w.data_device, (void (**)(void)) &data_device_listener, &w);
	}

	if (0)
		;
#ifdef XDG_SHELL_NAME
	else if (w.xdg_wm_base)
	{
		xdg_wm_base_add_listener(w.xdg_wm_base, &myxdg_wm_base_listener, &w);
		w.xdg_surface = xdg_wm_base_get_xdg_surface(w.xdg_wm_base, w.surface);
		if (!w.xdg_surface)
			return false;
		xdg_surface_add_listener(w.xdg_surface, &myxdg_surface_listener, &w);
		w.xdg_toplevel = xdg_surface_get_toplevel(w.xdg_surface);
		if (!w.xdg_toplevel)
			return false;
		xdg_toplevel_add_listener(w.xdg_toplevel, &myxdg_toplevel_listener, &w);
		//FTESurf Patch 1633: taskbars find the icon through the .desktop file named by app_id (ftesurf.sh writes
		//worldofsurf.desktop); unset, labwc and KWin show a generic one. Set before the first commit.
		xdg_toplevel_set_app_id(w.xdg_toplevel, "worldofsurf");
		xdg_toplevel_set_min_size(w.xdg_toplevel, 320, 200);
	}
#endif
#ifdef WL_SHELL_NAME
	else if (w.shell)
	{
		w.ssurface = pwl_shell_get_shell_surface(w.shell, w.surface);
		if (w.ssurface)
			pwl_shell_surface_add_listener(w.ssurface, &shell_surface_listener, &w);
	}
#endif
	else
	{
		Con_Printf("no compositor shell running, apparently\n");
		return false;	//no way to make it fullscreen/top-level means it'll probably stay hidden, so make this fatal.
	}

	{	//P1611: a restart keeps the title it had. It used to come back as "FTE Quake" (alt-tab's label) until the
		//client's state next changed.
		char cap[sizeof(wl_caption)];
		Q_strncpyz(cap, *wl_caption?wl_caption:(*fs_gamename.string?fs_gamename.string:"FTE Quake"), sizeof(cap));
		WL_SetCaption(cap);
	}
#ifdef XDG_DECORATION_MANAGER_NAME
	if (w.xdg_decoration_manager && w.xdg_toplevel)
	{	//decorate it! (before the first commit, as the protocol requires)
		w.xdg_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(w.xdg_decoration_manager, w.xdg_toplevel);
		if (w.xdg_decoration)
		{
			zxdg_toplevel_decoration_v1_add_listener(w.xdg_decoration, &myxdg_decoration_listener, &w);
			zxdg_toplevel_decoration_v1_set_mode(w.xdg_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}
#endif
#if defined(XDG_DECORATION_MANAGER_NAME) && defined(ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME)
	else
#endif
#ifdef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME
	if (w.kwin_decoration_manager)
	{	//decorate it!
		w.kwin_decoration = org_kde_kwin_server_decoration_manager_create(w.kwin_decoration_manager, w.surface);
		if (w.kwin_decoration)
		{
			org_kde_kwin_server_decoration_add_listener(w.kwin_decoration, &myorg_kde_kwin_server_decoration_listener, &w);
			org_kde_kwin_server_decoration_request_mode(w.kwin_decoration, ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_MODE_SERVER);
		}
	}
#endif
	
#ifdef XDG_SHELL_NAME
	if (w.xdg_wm_base)
	{
		if (info->fullscreen)
			xdg_toplevel_set_fullscreen(w.xdg_toplevel, NULL);
		else if (qrenderer == QR_OPENGL && VID_WindowMaximizePolicy() == 2)
			xdg_toplevel_set_maximized(w.xdg_toplevel);	//P1611: vid_winmaximize, as Windows does (it opened at 640x480); not Vulkan, which cannot resize
	}
#endif
#ifdef WL_SHELL_NAME
	if (w.ssurface)
	{
		if (info->fullscreen)
			pwl_shell_surface_set_fullscreen(w.ssurface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 60, NULL);
		else
			pwl_shell_surface_set_toplevel(w.ssurface);
	}
#endif

	w.truewidth = info->width?info->width:640;
	w.trueheight = info->height?info->height:480;
pwl_display_roundtrip(w.display);
	WL_ApplySize();	//sets vid.pixelwidth/pixelheight, and (P1611) the opaque region, on every resize from here on

	vid.activeapp = true;

	//window_set_keyboard_focus_handler(window, WL_handler_keyfocus);
	//window_set_resize_handler(w.surface, WL_handler_resize);

	switch(qrenderer)
	{
	default:
		return false;
#ifdef VKQUAKE
	case QR_VULKAN:
		{
			const char *extnames[] = {VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
			if (VK_Init(info, extnames, countof(extnames), WLVK_SetupSurface, NULL))
				return WL_NameAndShame();
			Con_Printf(CON_ERROR "Unable to initialise vulkan-on-wayland.\n");
			return false;
		}
		break;
#endif
#if defined(GLQUAKE) && defined(USE_EGL)
	case QR_OPENGL:
		{
			EGLConfig cfg;
			w.enwindow = pwl_egl_window_create(w.surface, w.bufwidth, w.bufheight);
			if (!EGL_InitDisplay(info, EGL_PLATFORM_WAYLAND_KHR, w.display, (EGLNativeDisplayType)w.display, &cfg))
			{
				Con_Printf("couldn't find suitable EGL config\n");
				return false;
			}
			#ifdef XDG_SHELL_NAME
				if (w.xdg_toplevel)
				{
					w.wait_for_configure = true;
					pwl_surface_commit(w.surface);
				}
			#endif
			egl_backendpacing = true;	//WL_SwapBuffers' frame callback paces vsync; EGL must not block on its own
			if (!EGL_InitWindow(info, EGL_PLATFORM_WAYLAND_KHR, w.enwindow, (EGLNativeWindowType)w.enwindow, cfg))
			{
				egl_backendpacing = false;
				Con_Printf("couldn't initialise EGL context\n");
				return false;
			}
		}

		pwl_display_dispatch_pending(w.display);

		if (GL_Init(info, &EGL_Proc))
			return WL_NameAndShame();
		Con_Printf(CON_ERROR "Unable to initialise opengl-on-wayland.\n");
		return false;
#endif
	}
	return true;
}
//destructor request (opcode) then the proxy; NULL is fine
static void WL_DestroyObject(void *obj, uint32_t destroyop)
{
	if (!obj)
		return;
	pwl_proxy_marshal((struct wl_proxy *) obj, destroyop);
	pwl_proxy_destroy((struct wl_proxy *) obj);
}
static void WL_DeInit(void)
{
	VID_SetFullscreenLive = NULL;	//P1611
	w.cursorfocus = false;	//a pointer.enter the final roundtrip delivers must not set a cursor again
#if defined(GLQUAKE) && defined(USE_EGL)
	EGL_Shutdown();
	if (w.enwindow)
		pwl_egl_window_destroy(w.enwindow);
	w.enwindow = NULL;	//the roundtrip below can still dispatch a configure/scale into WL_ApplySize
	EGL_UnloadLibrary();
	GL_ForgetPointers();
#endif
#ifdef WL_SHELL_NAME
	if (w.ssurface)		pwl_proxy_destroy((struct wl_proxy *) w.ssurface);
#endif
	//P1602: role objects before their surface, or wlroots answers "surface was destroyed before its role object"
	WL_DropCursorShapeDevice();
#ifdef XDG_DECORATION_MANAGER_NAME
	WL_DestroyObject(w.xdg_decoration, 0);	//zxdg_toplevel_decoration_v1.destroy, before its toplevel
#endif
#ifdef ORG_KDE_KWIN_SERVER_DECORATION_MANAGER_NAME
	WL_DestroyObject(w.kwin_decoration, 0);	//release
#endif
	WL_DestroyObject(w.tearing_control, 1);	//wp_tearing_control_v1.destroy is opcode 1
	WL_DestroyObject(w.fractional_scale, 0);
	WL_DestroyObject(w.viewport, 0);
#ifdef XDG_SHELL_NAME
	WL_DestroyObject(w.xdg_toplevel, 0);
	WL_DestroyObject(w.xdg_surface, 0);
#endif
	if (w.surface)		pwl_proxy_marshal((struct wl_proxy *) w.surface, WL_SURFACE_DESTROY);
	if (w.surface)		pwl_proxy_destroy((struct wl_proxy *) w.surface);
	w.surface = NULL;
	if (w.compositor)	pwl_proxy_destroy((struct wl_proxy *) w.compositor);
	if (w.registry)		pwl_proxy_destroy((struct wl_proxy *) w.registry);
	if (w.display)
	{	//let the server know everything before shutting it off (a roundtrip: a dispatch could wait forever for an event)
		pwl_display_roundtrip(w.display);
		pwl_display_disconnect(w.display);
	}
	Z_Free(w.csdcaption);
	Z_Free(w.cliptext);
	memset(&w, 0, sizeof(w));
	in_rawmice_live = 0;	//Patch 387: the grant dies with the display
	in_rawkbd_live = 0;
}
static qboolean WL_ApplyGammaRamps(unsigned int gammarampsize, unsigned short *ramps)
{
	//not supported
	return false;
}
static void WL_SetCaption(const char *text)
{
	if (text != wl_caption)
		Q_strncpyz(wl_caption, text, sizeof(wl_caption));	//P1611: outlives `w`, for the next WL_Init
#ifdef XDG_SHELL_NAME
	if (w.xdg_toplevel)
		xdg_toplevel_set_title(w.xdg_toplevel, text);
#endif
#ifdef WL_SHELL_NAME
	if (w.ssurface)
		pwl_shell_surface_set_title(w.ssurface, text);
#endif

	Z_StrDupPtr(&w.csdcaption, text);
}

static int WL_GetPriority(void)
{
	//2 = above x11, 0 = below x11.
	char *stype = getenv("XDG_SESSION_TYPE");
	char *dpyname = getenv("WAYLAND_DISPLAY");
	if (!dpyname || !*dpyname)
		return 0;	//P1602: no compositor to talk to (a tty, su), so GLX takes "gl"
	if (stype)
	{
		if (!strcmp(stype, "wayland"))
			return 2;
		if (!strcmp(stype, "x11"))
			return 0;
		if (!strcmp(stype, "tty"))	//FIXME: support this!
			return 0;
	}

	//otherwise if both WAYLAND_DISPLAY and DISPLAY are defined, then we assume that we were started from xwayland wrapper thing, and that the native/preferred windowing system is wayland.
	//(lets just hope our wayland support is comparable)

	return 2;	//something above X11.
}

#if defined(GLQUAKE) && defined(USE_EGL)
rendererinfo_t rendererinfo_wayland_gl =
{
	"OpenGL (Wayland)",
	{	//Patch 1602: gl/opengl too, so a saved `vid_renderer gl` takes this on a Wayland session (WL_GetPriority 2 beats GLX's 1) and GLX elsewhere (0)
		"wlgl",
		"wayland",
		"gl",
		"opengl",
	},
	QR_OPENGL,

	GLDraw_Init,
	GLDraw_DeInit,

	GL_UpdateFiltering,
	GL_LoadTextureMips,
	GL_DestroyTexture,

	GLR_Init,
	GLR_DeInit,
	GLR_RenderView,

	WL_Init,
	WL_DeInit,
	WL_SwapBuffers,
	WL_ApplyGammaRamps,
	WL_CreateCursor,
	WL_SetCursor,
	WL_DestroyCursor,
	WL_SetCaption,       //setcaption
	GLVID_GetRGBInfo,


	GLSCR_UpdateScreen,

	GLBE_SelectMode,
	GLBE_DrawMesh_List,
	GLBE_DrawMesh_Single,
	GLBE_SubmitBatch,
	GLBE_GetTempBatch,
	GLBE_DrawWorld,
	GLBE_Init,
	GLBE_GenBrushModelVBO,
	GLBE_ClearVBO,
	GLBE_UpdateLightmaps,
	GLBE_SelectEntity,
	GLBE_SelectDLight,
	GLBE_Scissor,
	GLBE_LightCullModel,

	GLBE_VBO_Begin,
	GLBE_VBO_Data,
	GLBE_VBO_Finish,
	GLBE_VBO_Destroy,

	GLBE_RenderToTextureUpdate2d,

	"",
	WL_GetPriority,
	NULL,
	NULL,
	WL_MayRefresh
};
#endif

#ifdef VKQUAKE
static qboolean WLVK_EnumerateDevices(void *usercontext, void(*callback)(void *context, const char *devicename, const char *outputname, const char *desc))
{
	qboolean ret = false;
#ifdef VK_NO_PROTOTYPES
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	void *lib = NULL;
	dllfunction_t func[] =
	{
		{(void*)&vkGetInstanceProcAddr,		"vkGetInstanceProcAddr"},
		{NULL,							NULL}
	};

	if (!lib)
		lib = Sys_LoadLibrary("libvulkan.so.1", func);
	if (!lib)
		lib = Sys_LoadLibrary("libvulkan.so", func);
	if (!lib)
		return false;
#endif
	ret = VK_EnumerateDevices(usercontext, callback, "Vulkan-Wayland-", vkGetInstanceProcAddr);
#ifdef VK_NO_PROTOTYPES
	Sys_CloseLibrary(lib);
#endif
	return ret;
}
rendererinfo_t rendererinfo_wayland_vk =
{
    "Vulkan (Wayland)",
    {
        "wlvk",
        "vk",
        "vulkan",
        "wayland"
    },
    QR_VULKAN,

    VK_Draw_Init,
    VK_Draw_Shutdown,

    VK_UpdateFiltering,
    VK_LoadTextureMips,
    VK_DestroyTexture,

    VK_R_Init,
    VK_R_DeInit,
    VK_R_RenderView,

    WL_Init,
    WL_DeInit,
    WL_SwapBuffers,
    WL_ApplyGammaRamps,

    WL_CreateCursor,
    WL_SetCursor,
    WL_DestroyCursor,
    WL_SetCaption,       //setcaption
    VKVID_GetRGBInfo,


    VK_SCR_UpdateScreen,

    VKBE_SelectMode,
    VKBE_DrawMesh_List,
    VKBE_DrawMesh_Single,
    VKBE_SubmitBatch,
    VKBE_GetTempBatch,
    VKBE_DrawWorld,
    VKBE_Init,
    VKBE_GenBrushModelVBO,
    VKBE_ClearVBO,
    VKBE_UploadAllLightmaps,
    VKBE_SelectEntity,
    VKBE_SelectDLight,
    VKBE_Scissor,
    VKBE_LightCullModel,

    VKBE_VBO_Begin,
    VKBE_VBO_Data,
    VKBE_VBO_Finish,
    VKBE_VBO_Destroy,

    VKBE_RenderToTextureUpdate2d,

    "",
	WL_GetPriority,
	NULL,
	WLVK_EnumerateDevices
};
#endif

#endif

