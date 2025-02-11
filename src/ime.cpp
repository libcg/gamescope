#include "ime.hpp"
#include "wlserver.hpp"
#include "log.hpp"

#include <assert.h>
#include <unistd.h>
#include <string.h>

#include <deque>
#include <unordered_map>
#include <vector>

#include <linux/input-event-codes.h>

extern "C" {
#define delete delete_
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#undef delete
}

#include "gamescope-input-method-protocol.h"

/* The C/C++ standard library doesn't expose a reliable way to decode UTF-8,
 * so we need to ship our own implementation. Yay for locales. */

static const uint32_t UTF8_INVALID = 0xFFFD;

static size_t utf8_size(const char *str)
{
	uint8_t u8 = (uint8_t)str[0];
	if (u8 == 0) {
		return 0;
	} else if ((u8 & 0x80) == 0) {
		return 1;
	} else if ((u8 & 0xE0) == 0xC0) {
		return 2;
	} else if ((u8 & 0xF0) == 0xE0) {
		return 3;
	} else if ((u8 & 0xF8) == 0xF0) {
		return 4;
	} else {
		return 0;
	}
}

static uint32_t utf8_decode(const char **str_ptr)
{
	const char *str = *str_ptr;
	size_t size = utf8_size(str);
	if (size == 0) {
		*str_ptr = &str[1];
		return UTF8_INVALID;
	}

	*str_ptr = &str[size];

	const uint32_t masks[] = { 0x7F, 0x1F, 0x0F, 0x07 };
	uint32_t ret = (uint32_t)str[0] & masks[size - 1];
	for (size_t i = 1; i < size; i++) {
		ret <<= 6;
		ret |= str[i] & 0x3F;
	}
	return ret;
}

#define IME_MANAGER_VERSION 1

/* Some clients assume keycodes are coming from evdev and interpret them. Only
 * use keys that would normally produce characters for our emulated events. */
static const uint32_t allow_keycodes[] = {
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
	KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE,
	KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_BACKSLASH,
	KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH,
};

static const size_t allow_keycodes_len = sizeof(allow_keycodes) / sizeof(allow_keycodes[0]);

struct wlserver_input_method_key {
	uint32_t keycode;
	xkb_keysym_t keysym;
};

static std::unordered_map<enum gamescope_input_method_action, struct wlserver_input_method_key> actions = {
	{ GAMESCOPE_INPUT_METHOD_ACTION_SUBMIT, { KEY_ENTER, XKB_KEY_Return } },
	{ GAMESCOPE_INPUT_METHOD_ACTION_DELETE_LEFT, { KEY_BACKSPACE, XKB_KEY_BackSpace } },
	{ GAMESCOPE_INPUT_METHOD_ACTION_DELETE_RIGHT, { KEY_DELETE, XKB_KEY_Delete } },
	{ GAMESCOPE_INPUT_METHOD_ACTION_MOVE_LEFT, { KEY_LEFT, XKB_KEY_Left } },
	{ GAMESCOPE_INPUT_METHOD_ACTION_MOVE_RIGHT, { KEY_RIGHT, XKB_KEY_Right } },
};

struct wlserver_input_method {
	struct wl_resource *resource;
	struct wlserver_input_method_manager *manager;
	uint32_t serial;

	struct {
		char *string;
		enum gamescope_input_method_action action;
	} pending;

	// Used to send emulated input events
	struct wlr_keyboard keyboard;
	struct wlr_input_device keyboard_device;
	std::deque<struct wlserver_input_method_key> keys;
	uint32_t next_keycode_index;

	struct wl_event_source *ime_reset_ime_keyboard_event_source;
};

struct wlserver_input_method_manager {
	struct wl_global *global;
	struct wlserver_t *server;

	struct wl_event_source *ime_reset_keyboard_event_source;
};

static LogScope ime_log("ime");

static struct wlserver_input_method *active_input_method = nullptr;

static xkb_keysym_t keysym_from_ch(uint32_t ch)
{
	// There's a bug in libxkbcommon where the EURO symbol doesn't map to the correct keysym
	if (ch == 0x20ac) {
		return XKB_KEY_EuroSign;
	}
	return xkb_utf32_to_keysym(ch);
}

static uint32_t keycode_from_ch(struct wlserver_input_method *ime, uint32_t ch)
{
	xkb_keysym_t keysym = keysym_from_ch(ch);
	if (keysym == XKB_KEY_NoSymbol) {
		return XKB_KEYCODE_INVALID;
	}

	// Repeated chars can re-use keycode
	if (!ime->keys.empty() && ime->keys.back().keysym == keysym)
	{
		return ime->keys.back().keycode;
	}

	if (ime->keys.size() >= allow_keycodes_len) {
		// TODO: maybe use keycodes above KEY_MAX?
		ime_log.errorf("Key codes wrapped within 100ms!");
		ime->keys.pop_front();
		// FALLTHROUGH and allow re-use (oldest key probably fine anyway)
	}

	uint32_t keycode = allow_keycodes[ime->next_keycode_index++ % allow_keycodes_len];
	ime->keys.push_back((struct wlserver_input_method_key){ keycode, keysym });
	return keycode;
}

static bool generate_keymap_key(FILE *f, uint32_t keycode, xkb_keysym_t keysym)
{
	char keysym_name[256];
	int ret = xkb_keysym_get_name(keysym, keysym_name, sizeof(keysym_name));
	if (ret <= 0) {
		ime_log.errorf("xkb_keysym_get_name failed for keysym %u", keysym);
		return false;
	}

	fprintf(f, "	key <K%u> {[ %s ]};\n", keycode, keysym_name);
	return true;
}

static struct xkb_keymap *generate_keymap(struct wlserver_input_method *ime)
{
	uint32_t keycode_offset = 8;

	char *str = NULL;
	size_t str_size = 0;
	FILE *f = open_memstream(&str, &str_size);

	// min/max from the set of all allow_keycodes and actions
	uint32_t min_keycode = KEY_1;
	uint32_t max_keycode = KEY_DELETE;
	fprintf(f,
		"xkb_keymap {\n"
		"\n"
		"xkb_keycodes \"(unnamed)\" {\n"
		"	minimum = %u;\n"
		"	maximum = %u;\n",
		keycode_offset + min_keycode,
		keycode_offset + max_keycode
	);

	for (const auto kk : ime->keys) {
		uint32_t keycode = kk.keycode;
		fprintf(f, "	<K%u> = %u;\n", keycode, keycode + keycode_offset);
	}
	for (const auto kv : actions) {
		uint32_t keycode = kv.second.keycode;
		fprintf(f, "	<K%u> = %u;\n", keycode, keycode + keycode_offset);
	}

	// TODO: should we really be including "complete" here? squeekboard seems
	// to get away with some other workarounds:
	// https://gitlab.gnome.org/World/Phosh/squeekboard/-/blob/fc411d680b0138042b95b8a630401607726113d4/src/keyboard.rs#L180
	fprintf(f,
		"};\n"
		"\n"
		"xkb_types \"(unnamed)\" { include \"complete\" };\n"
		"\n"
		"xkb_compatibility \"(unnamed)\" { include \"complete\" };\n"
		"\n"
		"xkb_symbols \"(unnamed)\" {\n"
	);

	for (const auto kk : ime->keys) {
		if (!generate_keymap_key(f, kk.keycode, kk.keysym))
		{
			fclose(f);
			free(str);
			return nullptr;
		}
	}
	for (const auto kv : actions) {
		const auto kk = kv.second;
		if (!generate_keymap_key(f, kk.keycode, kk.keysym))
		{
			fclose(f);
			free(str);
			return nullptr;
		}
	}

	fprintf(f,
		"};\n"
		"\n"
		"};\n"
	);

	fclose(f);

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(context, str, str_size, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(context);

	free(str);

	return keymap;
}

static bool try_type_keysym(struct wlserver_input_method *ime, xkb_keysym_t keysym)
{
	struct wlr_seat *seat = ime->manager->server->wlr.seat;
	struct wlr_input_device *device = ime->manager->server->wlr.virtual_keyboard_device;

	struct xkb_keymap *keymap = device->keyboard->keymap;
	xkb_keycode_t min_keycode = xkb_keymap_min_keycode(keymap);
	xkb_keycode_t max_keycode = xkb_keymap_max_keycode(keymap);
	for (xkb_keycode_t keycode = min_keycode; keycode <= max_keycode; keycode++) {
		xkb_layout_index_t num_layouts = xkb_keymap_num_layouts_for_key(keymap, keycode);
		for (xkb_layout_index_t layout = 0; layout < num_layouts; layout++) {
			xkb_level_index_t num_levels = xkb_keymap_num_levels_for_key(keymap, keycode, layout);
			for (xkb_level_index_t level = 0; level < num_levels; level++) {
				const xkb_keysym_t *syms = nullptr;
				int num_syms = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, level, &syms);
				if (num_syms != 1) {
					continue;
				}
				if (syms[0] != keysym) {
					continue;
				}

				xkb_mod_mask_t mask;
				size_t num_masks = xkb_keymap_key_get_mods_for_level(keymap, keycode, layout, level, &mask, 1);
				if (num_masks != 1) {
					continue;
				}

				xkb_mod_mask_t allowed = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
				if ((mask & allowed) != mask) {
					continue;
				}

				assert(keycode >= 8);

				uint32_t keycodes[8] = {0};
				size_t n = 0;
				if (mask & WLR_MODIFIER_SHIFT) {
					keycodes[n++] = KEY_LEFTSHIFT;
				}
				if (mask & WLR_MODIFIER_CTRL) {
					keycodes[n++] = KEY_LEFTCTRL;
				}
				if (mask & WLR_MODIFIER_ALT) {
					keycodes[n++] = KEY_LEFTALT;
				}
				keycodes[n++] = keycode - 8;

				struct wlr_keyboard_modifiers prev_mods = {0};
				if (seat->keyboard_state.keyboard != nullptr) {
					prev_mods = seat->keyboard_state.keyboard->modifiers;
				}
				struct wlr_keyboard_modifiers mods = {
					.depressed = mask,
				};
				wlr_seat_set_keyboard(seat, device);
				wlr_seat_keyboard_notify_modifiers(seat, &mods);
				for (size_t i = 0; i < n; i++) {
					wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_PRESSED);
				}
				for (ssize_t i = n - 1; i >= 0; i--) {
					wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_RELEASED);
				}
				wlr_seat_keyboard_notify_modifiers(seat, &prev_mods);

				return true;
			}
		}
	}

	return false;
}

static void type_text(struct wlserver_input_method *ime, const char *text)
{
	// If possible, try to type the character without switching the keymap
	// ...unless we're already using a fancy keymap
	if (utf8_size(text) == 1 && text[1] == '\0' && ime->keys.empty()) {
		xkb_keysym_t keysym = keysym_from_ch(text[0]);
		if (keysym != XKB_KEY_NoSymbol && try_type_keysym(ime, keysym)) {
			return;
		}
	}

	std::vector<xkb_keycode_t> keycodes;
	while (text[0] != '\0') {
		uint32_t ch = utf8_decode(&text);

		xkb_keycode_t keycode = keycode_from_ch(ime, ch);
		if (keycode == XKB_KEYCODE_INVALID) {
			ime_log.errorf("warning: cannot type character U+%X", ch);
			continue;
		}

		keycodes.push_back(keycode);
	}

	struct xkb_keymap *keymap = generate_keymap(ime);
	if (keymap == nullptr) {
		ime_log.errorf("failed to generate keymap");
		return;
	}
	wlr_keyboard_set_keymap(&ime->keyboard, keymap);
	xkb_keymap_unref(keymap);

	struct wlr_seat *seat = ime->manager->server->wlr.seat;
	wlr_seat_set_keyboard(seat, &ime->keyboard_device);

	// Note: Xwayland doesn't care about the time field of the events
	for (size_t i = 0; i < keycodes.size(); i++) {
		wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_PRESSED);
		wlr_seat_keyboard_notify_key(seat, 0, keycodes[i], WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	// Reset keymap when we're idle for a while
	wl_event_source_timer_update(ime->ime_reset_ime_keyboard_event_source, 100 /* ms */);
}

static void perform_action(struct wlserver_input_method *ime, enum gamescope_input_method_action action)
{
	if (actions.count(action) == 0) {
		ime_log.errorf("unsupported action %d", action);
		return;
	}

	const struct wlserver_input_method_key key = actions[action];
	if (try_type_keysym(ime, key.keysym)) {
		return;
	}

	// Keymap always contains all actions[]

	struct xkb_keymap *keymap = generate_keymap(ime);
	if (keymap == nullptr) {
		ime_log.errorf("failed to generate keymap");
		return;
	}
	wlr_keyboard_set_keymap(&ime->keyboard, keymap);
	xkb_keymap_unref(keymap);

	struct wlr_seat *seat = ime->manager->server->wlr.seat;
	wlr_seat_set_keyboard(seat, &ime->keyboard_device);

	// Note: Xwayland doesn't care about the time field of the events
	wlr_seat_keyboard_notify_key(seat, 0, key.keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(seat, 0, key.keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void ime_handle_commit(struct wl_client *client, struct wl_resource *ime_resource, uint32_t serial)
{
	struct wlserver_input_method *ime = (struct wlserver_input_method *)wl_resource_get_user_data(ime_resource);

	if (serial != ime->serial) {
		return;
	}

	if (ime->pending.string != nullptr) {
		type_text(ime, ime->pending.string);
	}
	if (ime->pending.action != GAMESCOPE_INPUT_METHOD_ACTION_NONE) {
		perform_action(ime, ime->pending.action);
	}

	free(ime->pending.string);
	ime->pending.string = nullptr;
	ime->pending.action = GAMESCOPE_INPUT_METHOD_ACTION_NONE;

	// Steam's virtual keyboard is based on XTest and relies on the keymap to
	// be reset. However, resetting it immediately is racy: clients will
	// interpret the keycodes we've just sent with the new keymap. To
	// workaround these issues, wait for a bit before resetting the keymap.
	wl_event_source_timer_update(ime->manager->ime_reset_keyboard_event_source, 100 /* ms */);
}

static void ime_handle_set_string(struct wl_client *client, struct wl_resource *ime_resource, const char *text)
{
	struct wlserver_input_method *ime = (struct wlserver_input_method *)wl_resource_get_user_data(ime_resource);
	free(ime->pending.string);
	ime->pending.string = strdup(text);
}

static void ime_handle_set_action(struct wl_client *client, struct wl_resource *ime_resource, uint32_t action)
{
	struct wlserver_input_method *ime = (struct wlserver_input_method *)wl_resource_get_user_data(ime_resource);
	ime->pending.action = (enum gamescope_input_method_action)action;
}

static void ime_handle_destroy(struct wl_client *client, struct wl_resource *ime_resource)
{
	wl_resource_destroy(ime_resource);
}

static const struct gamescope_input_method_interface ime_impl = {
	.destroy = ime_handle_destroy,
	.commit = ime_handle_commit,
	.set_string = ime_handle_set_string,
	.set_action = ime_handle_set_action,
};

static void ime_handle_resource_destroy(struct wl_resource *ime_resource)
{
	struct wlserver_input_method *ime = (struct wlserver_input_method *)wl_resource_get_user_data(ime_resource);
	if (ime == nullptr)
		return;

	active_input_method = nullptr;

	wlr_input_device_destroy(&ime->keyboard_device);

	delete ime;
}

static void keyboard_destroy(struct wlr_keyboard *kyeboard) {}

static const struct wlr_keyboard_impl keyboard_impl = {
	.destroy = keyboard_destroy,
};

static void keyboard_device_destroy(struct wlr_input_device *dev) {}

static const struct wlr_input_device_impl keyboard_device_impl = {
	.destroy = keyboard_device_destroy,
};

static int reset_ime_keyboard(void *data)
{
	struct wlserver_input_method *ime = (struct wlserver_input_method *)data;

	ime->keys.clear();
	ime->next_keycode_index = 0;  // preserve old behavior; could just let this keep going

	return 0;
}

static void manager_handle_create_input_method(struct wl_client *client, struct wl_resource *manager_resource, struct wl_resource *seat_resource, uint32_t id)
{
	struct wlserver_input_method_manager *manager = (struct wlserver_input_method_manager *)wl_resource_get_user_data(manager_resource);

	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *ime_resource = wl_resource_create(client, &gamescope_input_method_interface, version, id);
	wl_resource_set_implementation(ime_resource, &ime_impl, nullptr, ime_handle_resource_destroy);

	if (active_input_method != nullptr) {
		gamescope_input_method_send_unavailable(ime_resource);
		return;
	}

	struct wlserver_input_method *ime = new wlserver_input_method();
	ime->resource = ime_resource;
	ime->manager = manager;
	ime->serial = 1;
	ime->next_keycode_index = 0;

	wlr_keyboard_init(&ime->keyboard, &keyboard_impl);
	wlr_input_device_init(&ime->keyboard_device, WLR_INPUT_DEVICE_KEYBOARD, &keyboard_device_impl, "ime", 0, 0);
	ime->keyboard_device.keyboard = &ime->keyboard;

	wlr_keyboard_set_repeat_info(&ime->keyboard, 0, 0);

	wl_resource_set_user_data(ime->resource, ime);
	gamescope_input_method_send_done(ime->resource, ime->serial);

	ime->ime_reset_ime_keyboard_event_source = wl_event_loop_add_timer(manager->server->event_loop, reset_ime_keyboard, ime);

	active_input_method = ime;
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *manager_resource)
{
	wl_resource_destroy(manager_resource);
}

static const struct gamescope_input_method_manager_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.create_input_method = manager_handle_create_input_method,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wlserver_input_method_manager *manager = (struct wlserver_input_method_manager *)data;

	struct wl_resource *resource = wl_resource_create(client, &gamescope_input_method_manager_interface, version, id);
	wl_resource_set_implementation(resource, &manager_impl, manager, nullptr);
}

static int reset_keyboard(void *data)
{
	struct wlserver_t *wlserver = (struct wlserver_t *)data;

	// Reset the keyboard if it's not set or set to an IME's
	struct wlr_seat *seat = wlserver->wlr.seat;
	if (seat->keyboard_state.keyboard == nullptr || seat->keyboard_state.keyboard->data == nullptr) {
		wlr_seat_set_keyboard(seat, wlserver->wlr.virtual_keyboard_device);
	}

	return 0;
}

void create_ime_manager(struct wlserver_t *wlserver)
{
	struct wlserver_input_method_manager *manager = new wlserver_input_method_manager();
	manager->server = wlserver;
	manager->global = wl_global_create(wlserver->display, &gamescope_input_method_manager_interface, IME_MANAGER_VERSION, manager, manager_bind);
	manager->ime_reset_keyboard_event_source = wl_event_loop_add_timer(wlserver->event_loop, reset_keyboard, wlserver);
}
