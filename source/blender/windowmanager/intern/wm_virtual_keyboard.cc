/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * On-screen keyboard for touch devices.
 *
 * Compact, configurable 14-slot touch shortcut pad for Blender Android.
 * Tapping a slot toggles its assigned key(s) down/up; slots may share held keys.
 *
 * Why it lives here rather than in an add-on
 * ------------------------------------------
 * A key has to arrive as a key. If the tap that presses it reaches the interface layer as a
 * pointer event, whatever text field was being edited ends there and then: the field installs a UI
 * handler that outranks every modal operator, so it sees the click first and commits. No overlay
 * above it can prevent that, which is why the Python version could never type into a native field.
 *
 * So the tap is answered in #wm_virtual_keyboard_ghost_event, called from `ghost_event_proc()`
 * before the event reaches the queue at all. While the keyboard is open, a pointer event that
 * lands on it is consumed there and a key event is fed back in its place, built the same way
 * #wm_window_update_eventstate_modifiers builds the modifier events it injects. Nothing downstream
 * can tell the difference between this and a hardware keyboard, which is the point: text editing,
 * the keymap, operator dispatch and the search fields all keep working with no special case
 * anywhere.
 *
 * Drawing goes through #WM_draw_cb_activate, which paints in window coordinates after every region
 * and after the editor outlines. One callback covers the whole window, so the keyboard works in
 * every workspace and every editor without knowing anything about either.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "GHOST_ISystem.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_time.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BLF_api.hh"

#include "BKE_appdir.hh"
#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm.hh"
#include "wm_event_system.hh"
#include "wm_window.hh"
#include "wm_window_private.hh"

namespace blender {

/* Compact Android shortcut pad. All catalog keys can be assigned to any of 14 slots. */
static constexpr int VK_SLOT_COUNT = 14;
static constexpr int VK_PICK_PAGE_SIZE = 14;

enum class VKKind {
  Slot,
  Pick,
  Close,
  Move,
  Edit,
  Done,
  Save,
  Cancel,
  Clear,
  Reset,
  Prev,
  Next,
};

struct VKKeySpec {
  const char *label;
  VKKind kind;
  int code;
};

struct VKChoice {
  const char *label;
  GHOST_TKey key;
  const char *utf8;
  const char *shifted;
};

/* Display labels are short so that combinations fit on narrow touch keys. */
static const VKChoice vk_choices[] = {
    {"Ctrl", GHOST_kKeyLeftControl, nullptr, nullptr},
    {"Shift", GHOST_kKeyLeftShift, nullptr, nullptr},
    {"Alt", GHOST_kKeyLeftAlt, nullptr, nullptr},
    {"Esc", GHOST_kKeyEsc, nullptr, nullptr},
    {"Tab", GHOST_kKeyTab, nullptr, nullptr},
    {"Space", GHOST_kKeySpace, " ", " "},
    {"Enter", GHOST_kKeyEnter, nullptr, nullptr},
    {"Bksp", GHOST_kKeyBackSpace, nullptr, nullptr},
    {"Del", GHOST_kKeyDelete, nullptr, nullptr},
    {"Left", GHOST_kKeyLeftArrow, nullptr, nullptr},
    {"Right", GHOST_kKeyRightArrow, nullptr, nullptr},
    {"Up", GHOST_kKeyUpArrow, nullptr, nullptr},
    {"Down", GHOST_kKeyDownArrow, nullptr, nullptr},
    {"Home", GHOST_kKeyHome, nullptr, nullptr},
    {"End", GHOST_kKeyEnd, nullptr, nullptr},
    {"PgUp", GHOST_kKeyUpPage, nullptr, nullptr},
    {"PgDn", GHOST_kKeyDownPage, nullptr, nullptr},
    {"Ins", GHOST_kKeyInsert, nullptr, nullptr},
    {"A", GHOST_kKeyA, "a", "A"},
    {"B", GHOST_kKeyB, "b", "B"},
    {"C", GHOST_kKeyC, "c", "C"},
    {"D", GHOST_kKeyD, "d", "D"},
    {"E", GHOST_kKeyE, "e", "E"},
    {"F", GHOST_kKeyF, "f", "F"},
    {"G", GHOST_kKeyG, "g", "G"},
    {"H", GHOST_kKeyH, "h", "H"},
    {"I", GHOST_kKeyI, "i", "I"},
    {"J", GHOST_kKeyJ, "j", "J"},
    {"K", GHOST_kKeyK, "k", "K"},
    {"L", GHOST_kKeyL, "l", "L"},
    {"M", GHOST_kKeyM, "m", "M"},
    {"N", GHOST_kKeyN, "n", "N"},
    {"O", GHOST_kKeyO, "o", "O"},
    {"P", GHOST_kKeyP, "p", "P"},
    {"Q", GHOST_kKeyQ, "q", "Q"},
    {"R", GHOST_kKeyR, "r", "R"},
    {"S", GHOST_kKeyS, "s", "S"},
    {"T", GHOST_kKeyT, "t", "T"},
    {"U", GHOST_kKeyU, "u", "U"},
    {"V", GHOST_kKeyV, "v", "V"},
    {"W", GHOST_kKeyW, "w", "W"},
    {"X", GHOST_kKeyX, "x", "X"},
    {"Y", GHOST_kKeyY, "y", "Y"},
    {"Z", GHOST_kKeyZ, "z", "Z"},
    {"0", GHOST_kKey0, "0", ")"},
    {"1", GHOST_kKey1, "1", "!"},
    {"2", GHOST_kKey2, "2", "@"},
    {"3", GHOST_kKey3, "3", "#"},
    {"4", GHOST_kKey4, "4", "$"},
    {"5", GHOST_kKey5, "5", "%"},
    {"6", GHOST_kKey6, "6", "^"},
    {"7", GHOST_kKey7, "7", "&"},
    {"8", GHOST_kKey8, "8", "*"},
    {"9", GHOST_kKey9, "9", "("},
    {"F1", GHOST_kKeyF1, nullptr, nullptr},
    {"F2", GHOST_kKeyF2, nullptr, nullptr},
    {"F3", GHOST_kKeyF3, nullptr, nullptr},
    {"F4", GHOST_kKeyF4, nullptr, nullptr},
    {"F5", GHOST_kKeyF5, nullptr, nullptr},
    {"F6", GHOST_kKeyF6, nullptr, nullptr},
    {"F7", GHOST_kKeyF7, nullptr, nullptr},
    {"F8", GHOST_kKeyF8, nullptr, nullptr},
    {"F9", GHOST_kKeyF9, nullptr, nullptr},
    {"F10", GHOST_kKeyF10, nullptr, nullptr},
    {"F11", GHOST_kKeyF11, nullptr, nullptr},
    {"F12", GHOST_kKeyF12, nullptr, nullptr},
    {"Num0", GHOST_kKeyNumpad0, "0", "0"},
    {"Num1", GHOST_kKeyNumpad1, "1", "1"},
    {"Num2", GHOST_kKeyNumpad2, "2", "2"},
    {"Num3", GHOST_kKeyNumpad3, "3", "3"},
    {"Num4", GHOST_kKeyNumpad4, "4", "4"},
    {"Num5", GHOST_kKeyNumpad5, "5", "5"},
    {"Num6", GHOST_kKeyNumpad6, "6", "6"},
    {"Num7", GHOST_kKeyNumpad7, "7", "7"},
    {"Num8", GHOST_kKeyNumpad8, "8", "8"},
    {"Num9", GHOST_kKeyNumpad9, "9", "9"},
    {"Num+", GHOST_kKeyNumpadPlus, "+", "+"},
    {"Num-", GHOST_kKeyNumpadMinus, "-", "-"},
    {"Minus", GHOST_kKeyMinus, "-", "_"},
    {"Equal", GHOST_kKeyEqual, "=", "+"},
    {"Comma", GHOST_kKeyComma, ",", "<"},
    {"Period", GHOST_kKeyPeriod, ".", ">"},
    {"Slash", GHOST_kKeySlash, "/", "?"},
    {"Semi", GHOST_kKeySemicolon, ";", ":"},
    {"Quote", GHOST_kKeyQuote, "'", "\""},
    {"[", GHOST_kKeyLeftBracket, "[", "{"},
    {"]", GHOST_kKeyRightBracket, "]", "}"},
    {"Backslash", GHOST_kKeyBackslash, "\\", "|"},
};

static const VKChoice *vk_choice_for_key(const GHOST_TKey key)
{
  for (const VKChoice &choice : vk_choices) {
    if (choice.key == key) {
      return &choice;
    }
  }
  return nullptr;
}

struct VKSlot {
  GHOST_TKey keys[2] = {GHOST_kKeyUnknown, GHOST_kKeyUnknown};
  int count = 0;
  bool active = false;
  char label[40] = "";
};

/* Placed keys point into the stable spec arrays owned by the singleton. */
struct VKPlacedKey {
  const VKKeySpec *spec;
  rcti rect;
};

static const float VK_MIN_HEIGHT = 108.0f;

struct VirtualKeyboard {
  bool open = false;
  wmWindow *win = nullptr;
  void *draw_handle = nullptr;
  rcti rect = {0, 0, 0, 0};
  float offset = 0.0f;
  VKSlot slots[VK_SLOT_COUNT];
  VKKeySpec slot_specs[VK_SLOT_COUNT] = {};
  VKKeySpec picker_specs[VK_PICK_PAGE_SIZE] = {};
  bool loaded = false;
  bool editing = false;
  bool picking = false;
  int selected_slot = -1;
  int picker_page = 0;
  GHOST_TKey pending[2] = {GHOST_kKeyUnknown, GHOST_kKeyUnknown};
  int pending_count = 0;
  bool save_failed = false;
  Vector<VKPlacedKey> keys;
  const char *const *text_edit = nullptr;
  char last_key[64] = "";
  double last_key_time = 0.0;
  int hover = -1;
  int pressed = -1;
  bool moving = false;
  int cursor[2] = {0, 0};
  int pinned[2] = {0, 0};
  int drag_prev[2] = {0, 0};
};

static VirtualKeyboard g_vk;

static void vk_slot_label(VKSlot &slot)
{
  if (slot.count == 0) {
    BLI_strncpy(slot.label, "Empty", sizeof(slot.label));
    return;
  }
  const VKChoice *first = vk_choice_for_key(slot.keys[0]);
  const VKChoice *second = (slot.count == 2) ? vk_choice_for_key(slot.keys[1]) : nullptr;
  if (first == nullptr || (slot.count == 2 && second == nullptr)) {
    BLI_strncpy(slot.label, "Invalid", sizeof(slot.label));
    return;
  }
  if (second != nullptr) {
    BLI_snprintf(slot.label, sizeof(slot.label), "%s+%s", first->label, second->label);
  }
  else {
    BLI_strncpy(slot.label, first->label, sizeof(slot.label));
  }
}

static void vk_default_slots(VirtualKeyboard &vk)
{
  const GHOST_TKey defaults[VK_SLOT_COUNT][2] = {
      {GHOST_kKeyLeftShift, GHOST_kKeyUnknown},
      {GHOST_kKeyLeftControl, GHOST_kKeyUnknown},
      {GHOST_kKeyLeftAlt, GHOST_kKeyUnknown},
      {GHOST_kKeyG, GHOST_kKeyUnknown},
      {GHOST_kKeyR, GHOST_kKeyUnknown},
      {GHOST_kKeyS, GHOST_kKeyUnknown},
      {GHOST_kKeyE, GHOST_kKeyUnknown},
      {GHOST_kKeyF, GHOST_kKeyUnknown},
      {GHOST_kKeyTab, GHOST_kKeyUnknown},
      {GHOST_kKeyLeftControl, GHOST_kKeyD},
      {GHOST_kKeyLeftShift, GHOST_kKeyD},
      {GHOST_kKeyLeftControl, GHOST_kKeyZ},
      {GHOST_kKeyEsc, GHOST_kKeyUnknown},
      {GHOST_kKeyX, GHOST_kKeyUnknown},
  };
  for (int i = 0; i < VK_SLOT_COUNT; i++) {
    vk.slots[i].keys[0] = defaults[i][0];
    vk.slots[i].keys[1] = defaults[i][1];
    vk.slots[i].count = (defaults[i][1] == GHOST_kKeyUnknown) ? 1 : 2;
    vk.slots[i].active = false;
    vk_slot_label(vk.slots[i]);
  }
}

/* Blender's existing user-config location is app-private on Android. */
static std::optional<std::string> vk_config_path(const bool create)
{
  const std::optional<std::string> directory = create ?
      BKE_appdir_folder_id_create(BLENDER_USER_CONFIG, nullptr) :
      BKE_appdir_folder_id(BLENDER_USER_CONFIG, nullptr);
  if (!directory) {
    return std::nullopt;
  }
  return *directory + "/android_custom_keyboard_v1.txt";
}

static bool vk_save(VirtualKeyboard &vk)
{
  const std::optional<std::string> path = vk_config_path(true);
  if (!path) {
    return false;
  }
  const std::string temp_path = *path + ".tmp";
  FILE *file = std::fopen(temp_path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  bool ok = std::fprintf(file, "VK14 1\n") > 0;
  for (const VKSlot &slot : vk.slots) {
    ok &= std::fprintf(file, "%d %d %d\n", slot.count, int(slot.keys[0]), int(slot.keys[1])) > 0;
  }
  ok &= (std::fclose(file) == 0);
  if (!ok || std::rename(temp_path.c_str(), path->c_str()) != 0) {
    std::remove(temp_path.c_str());
    return false;
  }
  return true;
}

static void vk_load(VirtualKeyboard &vk)
{
  vk_default_slots(vk);
  vk.loaded = true;
  const std::optional<std::string> path = vk_config_path(false);
  if (!path) {
    return;
  }
  FILE *file = std::fopen(path->c_str(), "rb");
  if (file == nullptr) {
    return;
  }
  char signature[16] = "";
  int version = 0;
  VKSlot loaded[VK_SLOT_COUNT] = {};
  bool ok = std::fscanf(file, "%15s %d", signature, &version) == 2 &&
            STREQ(signature, "VK14") && version == 1;
  for (int i = 0; ok && i < VK_SLOT_COUNT; i++) {
    int n = -1, a = -1, b = -1;
    if (std::fscanf(file, "%d %d %d", &n, &a, &b) != 3 || n < 0 || n > 2) {
      ok = false;
      break;
    }
    const GHOST_TKey ka = GHOST_TKey(a);
    const GHOST_TKey kb = GHOST_TKey(b);
    if ((n >= 1 && vk_choice_for_key(ka) == nullptr) ||
        (n == 2 && (ka == kb || vk_choice_for_key(kb) == nullptr)))
    {
      ok = false;
      break;
    }
    loaded[i].count = n;
    loaded[i].keys[0] = n >= 1 ? ka : GHOST_kKeyUnknown;
    loaded[i].keys[1] = n == 2 ? kb : GHOST_kKeyUnknown;
    vk_slot_label(loaded[i]);
  }
  std::fclose(file);
  if (ok) {
    for (int i = 0; i < VK_SLOT_COUNT; i++) {
      vk.slots[i] = loaded[i];
    }
  }
}

static float vk_scale()
{
  return (U.scale_factor > 0.0f) ? U.scale_factor : 1.0f;
}

static void vk_tag_redraw(wmWindow *win)
{
  bScreen *screen = WM_window_get_active_screen(win);
  if (screen) {
    screen->do_draw = true;
  }
}

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

/**
 * The window rows the keyboard is allowed to occupy.
 *
 * The top bar and the status bar are global areas: the keyboard is kept clear of both, so the
 * button that opens it and the editor menus stay reachable while it is up.
 */
static void vk_drawable_band(const wmWindow *win, int *r_bottom, int *r_top)
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  int bottom = 0;
  int top = win->sizey;
  bool found = false;

  if (screen) {
    for (const ScrArea &area : screen->areabase) {
      if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
        continue;
      }
      const int area_bottom = area.totrct.ymin;
      const int area_top = area.totrct.ymax;
      if (!found) {
        bottom = area_bottom;
        top = area_top;
        found = true;
      }
      else {
        bottom = min_ii(bottom, area_bottom);
        top = max_ii(top, area_top);
      }
    }
  }

  if (!found || (top - bottom) < 80) {
    bottom = 0;
    top = win->sizey;
  }
  *r_bottom = bottom;
  *r_top = top;
}

/**
 * Whether this point is inside an editor, and so worth remembering as where injected keys go.
 *
 * A stylus that leaves proximity parks the cursor outside the window, and a shortcut attributed to
 * that point reaches no editor at all: the key is delivered and nothing happens. Keeping the last
 * position that was actually over an editor is what makes a shortcut land where the user last
 * worked rather than wherever the pen happened to leave the screen.
 */
static bool vk_position_in_area(const wmWindow *win, const int xy[2])
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen == nullptr) {
    return false;
  }
  for (const ScrArea &area : screen->areabase) {
    if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
      continue;
    }
    if (BLI_rcti_isect_pt_v(&area.totrct, xy)) {
      return true;
    }
  }
  return false;
}

/**
 * Where an injected key should be aimed.
 *
 * The remembered position is preferred, but it cannot be trusted on its own: rotating the device
 * rearranges every area, and a point remembered in portrait can land outside the window entirely,
 * which delivers the key to no editor at all. When it no longer points at anything, the largest
 * editor is used instead, and a 3D viewport outranks a bigger flat one since that is what a
 * shortcut is usually meant for.
 */
static void vk_target_position(const VirtualKeyboard &vk, const wmWindow *win, int r_xy[2])
{
  if (vk_position_in_area(win, vk.pinned)) {
    copy_v2_v2_int(r_xy, vk.pinned);
    return;
  }

  r_xy[0] = win->sizex / 2;
  r_xy[1] = win->sizey / 2;

  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen == nullptr) {
    return;
  }
  const ScrArea *best = nullptr;
  float best_score = -1.0f;
  for (const ScrArea &area : screen->areabase) {
    if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
      continue;
    }
    float score = float(BLI_rcti_size_x(&area.totrct)) * float(BLI_rcti_size_y(&area.totrct));
    if (area.spacetype == SPACE_VIEW3D) {
      score *= 4.0f;
    }
    if (score > best_score) {
      best = &area;
      best_score = score;
    }
  }
  if (best != nullptr) {
    r_xy[0] = BLI_rcti_cent_x(&best->totrct);
    r_xy[1] = BLI_rcti_cent_y(&best->totrct);
  }
}

static const VKKeySpec vk_spec_close = {"X", VKKind::Close, 0};
static const VKKeySpec vk_spec_move = {"Move", VKKind::Move, 0};
static const VKKeySpec vk_spec_edit = {"Edit", VKKind::Edit, 0};
static const VKKeySpec vk_spec_done = {"Done", VKKind::Done, 0};
static const VKKeySpec vk_spec_save = {"Save", VKKind::Save, 0};
static const VKKeySpec vk_spec_cancel = {"Back", VKKind::Cancel, 0};
static const VKKeySpec vk_spec_clear = {"Clear", VKKind::Clear, 0};
static const VKKeySpec vk_spec_reset = {"Reset", VKKind::Reset, 0};
static const VKKeySpec vk_spec_prev = {"<", VKKind::Prev, 0};
static const VKKeySpec vk_spec_next = {">", VKKind::Next, 0};

static void vk_place(VirtualKeyboard &vk,
                     const VKKeySpec *spec,
                     const float x,
                     const float y,
                     const float w,
                     const float h)
{
  VKPlacedKey placed;
  placed.spec = spec;
  placed.rect = {int(x), int(x + w), int(y), int(y + h)};
  vk.keys.append(placed);
}

static void vk_build_layout(VirtualKeyboard &vk, wmWindow *win)
{
  const int win_w = win->sizex, win_h = win->sizey;
  if (win_w <= 0 || win_h <= 0) {
    return;
  }
  int band_bottom, band_top;
  vk_drawable_band(win, &band_bottom, &band_top);
  const float scale = vk_scale();
  const bool portrait = win_w < win_h;
  const int columns = portrait ? 4 : 7;
  const int rows = (VK_SLOT_COUNT + columns - 1) / columns;
  const float band_h = float(band_top - band_bottom);
  const float panel_width = min_ff(float(win_w) * (portrait ? 0.94f : 0.80f),
                                   (portrait ? 480.0f : 840.0f) * scale);
  const float height = min_ff(
      band_h,
      max_ff(VK_MIN_HEIGHT * scale,
             min_ff(float(win_h) * (portrait ? 0.28f : 0.36f),
                    (portrait ? 300.0f : 194.0f) * scale)));
  const float room = max_ff(0.0f, band_h - height);
  vk.offset = clamp_f(vk.offset, 0.0f, room);
  const float x0 = (float(win_w) - panel_width) * 0.5f;
  const float y0 = float(band_bottom) + vk.offset;
  vk.rect = {int(x0), int(x0 + panel_width), int(y0), int(y0 + height)};
  vk.keys.clear();

  const float pad = max_ff(3.0f, 5.0f * scale);
  const float gap = max_ff(2.0f, 4.0f * scale);
  const float header_h = clamp_f(height * 0.22f, 26.0f * scale, 38.0f * scale);
  const float header_y = y0 + height - header_h - pad;
  const float header_w = clamp_f(header_h * 1.25f, 32.0f * scale, 55.0f * scale);

  /* Keep the original indices of Close [0] and Move [1] for the status text. */
  vk_place(vk, &vk_spec_close, x0 + panel_width - pad - header_w, header_y, header_w, header_h);
  vk_place(vk, &vk_spec_move, x0 + pad, header_y, header_w, header_h);
  float right = x0 + panel_width - pad - header_w - gap;
  auto add_button = [&](const VKKeySpec *spec) {
    right -= header_w;
    vk_place(vk, spec, right, header_y, header_w, header_h);
    right -= gap;
  };
  if (vk.picking) {
    add_button(&vk_spec_save);
    add_button(&vk_spec_cancel);
    add_button(&vk_spec_next);
    add_button(&vk_spec_prev);
    add_button(&vk_spec_clear);
  }
  else if (vk.editing) {
    add_button(&vk_spec_done);
    add_button(&vk_spec_reset);
  }
  else {
    add_button(&vk_spec_edit);
  }

  const float main_y = y0 + pad;
  const float main_h = max_ff(20.0f, header_y - gap - main_y);
  const float key_width = (panel_width - 2.0f * pad - float(columns - 1) * gap) /
                          float(columns);
  const float key_height = (main_h - float(rows - 1) * gap) / float(rows);

  for (int index = 0; index < VK_SLOT_COUNT; index++) {
    VKKeySpec *spec;
    if (vk.picking) {
      const int choice_index = vk.picker_page * VK_PICK_PAGE_SIZE + index;
      if (choice_index >= int(ARRAY_SIZE(vk_choices))) {
        break;
      }
      vk.picker_specs[index] = {vk_choices[choice_index].label, VKKind::Pick, choice_index};
      spec = &vk.picker_specs[index];
    }
    else {
      vk.slot_specs[index] = {vk.slots[index].label, VKKind::Slot, index};
      spec = &vk.slot_specs[index];
    }
    const int col = index % columns;
    const int row = index / columns;
    const float x = x0 + pad + float(col) * (key_width + gap);
    const float y = main_y + float(rows - 1 - row) * (key_height + gap);
    vk_place(vk, spec, x, y, key_width, key_height);
  }
}

static void vk_ensure_layout(VirtualKeyboard &vk, wmWindow *win)
{
  /* Rebuilt rather than cached against the window size. Rotation changes the size, the areas and
   * therefore the band all at once, and a layout that survives any of that keeps the proportion of
   * the orientation it was built in. The arithmetic is a few dozen multiplications. */
  vk_build_layout(vk, win);
}

static int vk_key_at(const VirtualKeyboard &vk, const int xy[2])
{
  for (const int i : vk.keys.index_range()) {
    if (BLI_rcti_isect_pt_v(&vk.keys[i].rect, xy)) {
      return i;
    }
  }
  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void vk_rect_verts(uint pos, const rctf &rect, float radius, int segments)
{
  /* A rounded rectangle as a triangle fan from the middle. The corners are the only curved part,
   * so a handful of segments each is enough at any size a finger can hit. */
  radius = min_ff(radius, min_ff(BLI_rctf_size_x(&rect), BLI_rctf_size_y(&rect)) * 0.5f);
  const float cx = BLI_rctf_cent_x(&rect);
  const float cy = BLI_rctf_cent_y(&rect);

  const float corner_x[4] = {rect.xmax - radius, rect.xmax - radius, rect.xmin + radius, rect.xmin + radius};
  const float corner_y[4] = {rect.ymin + radius, rect.ymax - radius, rect.ymax - radius, rect.ymin + radius};
  const float corner_start[4] = {-float(M_PI) * 0.5f, 0.0f, float(M_PI) * 0.5f, float(M_PI)};

  float prev_x = 0.0f, prev_y = 0.0f;
  bool has_prev = false;
  float first_x = 0.0f, first_y = 0.0f;

  for (int corner = 0; corner < 4; corner++) {
    for (int i = 0; i <= segments; i++) {
      const float angle = corner_start[corner] + (float(M_PI) * 0.5f) * (float(i) / float(segments));
      const float x = corner_x[corner] + cosf(angle) * radius;
      const float y = corner_y[corner] + sinf(angle) * radius;
      if (has_prev) {
        immVertex2f(pos, cx, cy);
        immVertex2f(pos, prev_x, prev_y);
        immVertex2f(pos, x, y);
      }
      else {
        first_x = x;
        first_y = y;
        has_prev = true;
      }
      prev_x = x;
      prev_y = y;
    }
  }
  immVertex2f(pos, cx, cy);
  immVertex2f(pos, prev_x, prev_y);
  immVertex2f(pos, first_x, first_y);
}

static int vk_round_rect_tris(int segments)
{
  /* One triangle per outline point, the closing one included, over four corners of `segments + 1`
   * points each. Promising fewer than are emitted is what left every key with a torn notch along
   * its bottom edge, which is where the fan closes. */
  return 4 * (segments + 1) * 3;
}

static void vk_draw_round_rect(uint pos, const rctf &rect, float radius, const float color[4])
{
  const int segments = 4;
  immUniformColor4fv(color);
  immBegin(GPU_PRIM_TRIS, uint(vk_round_rect_tris(segments)));
  vk_rect_verts(pos, rect, radius, segments);
  immEnd();
}

static void vk_draw_rect(uint pos, const rctf &rect, const float color[4])
{
  immUniformColor4fv(color);
  immBegin(GPU_PRIM_TRIS, 6);
  immVertex2f(pos, rect.xmin, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymax);
  immVertex2f(pos, rect.xmin, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymax);
  immVertex2f(pos, rect.xmin, rect.ymax);
  immEnd();
}

/* One palette rather than the theme: the keyboard sits above every editor, so it has to read the
 * same whatever theme is behind it. These are Blender's own interface greys and its selection
 * blue. */
static const float VK_COL_PANEL[4] = {0.106f, 0.106f, 0.106f, 0.96f};
static const float VK_COL_PANEL_EDGE[4] = {0.24f, 0.24f, 0.24f, 1.0f};
/* Flat: one colour per cap, and it is the lighter one the old top half carried rather than the
 * darker body underneath it. The keys used to be drawn in three passes -- a dropped shadow, the
 * cap, then a white wash over the top 45% -- which gave them a moulded look that nothing else in
 * Blender has. These values are each of those caps with the wash already folded in, so a key
 * reads at the brightness it always did, in one pass. */
static const float VK_COL_CAP[4] = {0.26f, 0.26f, 0.26f, 1.0f};
static const float VK_COL_CAP_MOD[4] = {0.20f, 0.20f, 0.20f, 1.0f};
static const float VK_COL_CAP_PRESS[4] = {0.314f, 0.475f, 0.717f, 1.0f};
static const float VK_COL_CAP_LOCK[4] = {0.383f, 0.573f, 0.839f, 1.0f};
static const float VK_COL_CAP_CLOSE[4] = {0.478f, 0.202f, 0.183f, 1.0f};
static const float VK_COL_CAP_MOVE[4] = {0.32f, 0.32f, 0.32f, 1.0f};
static const float VK_COL_TEXT[4] = {0.85f, 0.85f, 0.85f, 1.0f};
static const float VK_COL_TEXT_ON[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static const float VK_COL_TEXT_DIM[4] = {0.6f, 0.6f, 0.6f, 1.0f};
/** The character Shift is offering, shown in place of the usual one. */
static const float VK_COL_TEXT_SHIFT[4] = {0.45f, 0.68f, 1.0f, 1.0f};

static bool vk_picker_is_pending(const VirtualKeyboard &vk, const GHOST_TKey key)
{
  for (int i = 0; i < vk.pending_count; i++) {
    if (vk.pending[i] == key) {
      return true;
    }
  }
  return false;
}

static const float *vk_cap_color(const VirtualKeyboard &vk, int index)
{
  const VKKeySpec *spec = vk.keys[index].spec;
  if (index == vk.pressed) {
    return VK_COL_CAP_PRESS;
  }
  switch (spec->kind) {
    case VKKind::Close:
      return VK_COL_CAP_CLOSE;
    case VKKind::Move:
      return vk.moving ? VK_COL_CAP_PRESS : VK_COL_CAP_MOVE;
    case VKKind::Slot:
      return (vk.slots[spec->code].active ||
              (vk.editing && !vk.picking && vk.selected_slot == spec->code)) ?
                 VK_COL_CAP_LOCK : VK_COL_CAP;
    case VKKind::Pick:
      return vk_picker_is_pending(vk, vk_choices[spec->code].key) ?
                 VK_COL_CAP_LOCK : VK_COL_CAP;
    default:
      return VK_COL_CAP_MOD;
  }
}

static const char *vk_key_label(const VirtualKeyboard & /*vk*/,
                                const VKKeySpec &spec,
                                bool *r_shifted)
{
  *r_shifted = false;
  return spec.label;
}

static void vk_status_text(const VirtualKeyboard &vk,
                           const wmWindow * /*win*/,
                           char *buf,
                           size_t buf_size)
{
  if (vk.picking) {
    BLI_snprintf(buf, buf_size, "Slot %d: choose 1 or 2, Save", vk.selected_slot + 1);
  }
  else if (vk.editing) {
    BLI_strncpy(buf, vk.save_failed ? "Save failed" : "Tap a slot to edit", buf_size);
  }
  else if (vk.text_edit != nullptr && *vk.text_edit != nullptr) {
    BLI_snprintf(buf, buf_size, "%s|", *vk.text_edit);
  }
  else if (vk.last_key[0] != '\0' && (BLI_time_now_seconds() - vk.last_key_time) < 1.5) {
    BLI_strncpy(buf, vk.last_key, buf_size);
  }
  else {
    int active = 0;
    for (const VKSlot &slot : vk.slots) {
      active += slot.active ? 1 : 0;
    }
    BLI_snprintf(buf, buf_size, "%d held - Edit to assign keys", active);
  }
}

static void vk_draw_cb(const wmWindow *win, void * /*customdata*/)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open || vk.win != win) {
    return;
  }
  vk_ensure_layout(vk, const_cast<wmWindow *>(win));
  if (vk.keys.is_empty()) {
    return;
  }

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

  GPU_blend(GPU_BLEND_ALPHA);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  rctf panel;
  BLI_rctf_rcti_copy(&panel, &vk.rect);
  vk_draw_rect(pos, panel, VK_COL_PANEL);

  rctf edge = panel;
  edge.ymin = edge.ymax - 2.0f;
  vk_draw_rect(pos, edge, VK_COL_PANEL_EDGE);

  for (const int i : vk.keys.index_range()) {
    rctf cap;
    BLI_rctf_rcti_copy(&cap, &vk.keys[i].rect);
    const float cap_h = BLI_rctf_size_y(&cap);
    const float radius = cap_h * 0.18f;

    vk_draw_round_rect(pos, cap, radius, vk_cap_color(vk, i));
  }

  immUnbindProgram();

  /* Labels. */
  const int font_id = BLF_default();
  for (const int i : vk.keys.index_range()) {
    const VKPlacedKey &key = vk.keys[i];
    if (key.spec->label == nullptr) {
      continue;
    }
    const float cap_h = float(BLI_rcti_size_y(&key.rect));
    /* Bounded above as well as below: a tall portrait key would otherwise carry a label larger
     * than anything else on screen. */
    const float size = clamp_f(cap_h * 0.34f, 9.0f, 16.0f * vk_scale());
    BLF_size(font_id, size);

    const bool on = (i == vk.pressed) ||
                    (key.spec->kind == VKKind::Slot && vk.slots[key.spec->code].active) ||
                    (key.spec->kind == VKKind::Pick &&
                     vk_picker_is_pending(vk, vk_choices[key.spec->code].key));

    bool shifted = false;
    const char *label = vk_key_label(vk, *key.spec, &shifted);
    /* Held keys use the highlighted colour; configured labels remain unchanged. */
    BLF_color4fv(font_id,
                 on          ? VK_COL_TEXT_ON :
                 shifted     ? VK_COL_TEXT_SHIFT :
                               VK_COL_TEXT);

    const size_t label_len = strlen(label);
    const float available_w = float(BLI_rcti_size_x(&key.rect)) - 6.0f;
    float text_w = BLF_width(font_id, label, label_len);
    if (text_w > available_w && text_w > 0.0f) {
      BLF_size(font_id, max_ff(8.0f, size * available_w / text_w));
      text_w = BLF_width(font_id, label, label_len);
    }
    const float x = float(key.rect.xmin) + (float(BLI_rcti_size_x(&key.rect)) - text_w) * 0.5f;
    const float y = float(key.rect.ymin) + (cap_h - size) * 0.5f + size * 0.12f;
    BLF_position(font_id, x, y, 0.0f);
    BLF_draw(font_id, label, label_len);
  }

  /* Draw the action status between the drag handle and menu buttons. */
  {
    char status[512];
    vk_status_text(vk, win, status, sizeof(status));

    const VKPlacedKey &handle = vk.keys[1];
    const VKPlacedKey &close = vk.keys[0];
    const float bar_h = float(BLI_rcti_size_y(&handle.rect));
    /* Small on purpose: this is a hint about where the keys are going, not a heading. */
    const float size = min_ff(bar_h * 0.40f, 11.0f * vk_scale());
    BLF_size(font_id, size);
    BLF_color4fv(font_id, (vk.text_edit != nullptr) ? VK_COL_TEXT_ON : VK_COL_TEXT_DIM);

    const float text_x = float(handle.rect.xmax) + bar_h * 0.25f;
    float status_right = float(close.rect.xmin) - bar_h * 0.3f;
    for (const VKPlacedKey &button : vk.keys) {
      if (button.spec->kind != VKKind::Slot && button.spec->kind != VKKind::Pick &&
          button.spec->kind != VKKind::Move && button.spec->kind != VKKind::Close)
      {
        status_right = min_ff(status_right, float(button.rect.xmin) - 3.0f);
      }
    }
    const float avail = max_ff(0.0f, status_right - text_x);

    /* Follow the end of the text once it no longer fits, so what was just typed stays visible and
     * nothing ever runs under the close button. */
    const char *text = status;
    while (*text != '\0' && BLF_width(font_id, text, strlen(text)) > avail) {
      text += BLI_str_utf8_size_safe(text);
    }

    BLF_position(font_id,
                 text_x,
                 float(handle.rect.ymin) + (bar_h - size) * 0.5f + size * 0.12f,
                 0.0f);
    BLF_draw(font_id, text, strlen(text));
  }

  BLF_batch_draw_flush();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sending keys
 * \{ */

static uint64_t vk_event_time_ms()
{
  static uint64_t clock = 0;
  clock += uint64_t(U.dbl_click_time) + 100;
  return clock;
}

static void vk_send_ghost_key(wmWindowManager *wm,
                              wmWindow *win,
                              GHOST_TKey key,
                              const char *utf8,
                              bool down)
{
  if (wm == nullptr || win == nullptr) {
    return;
  }
  GHOST_TEventKeyData kdata = {};
  kdata.key = key;
  kdata.is_repeat = false;
  if (down && utf8 != nullptr) {
    BLI_strncpy(kdata.utf8_buf, utf8, sizeof(kdata.utf8_buf));
  }
  wm_event_add_ghostevent(
      wm, win, down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp, &kdata, vk_event_time_ms());
}

static bool vk_is_modifier(const GHOST_TKey key)
{
  return ELEM(key, GHOST_kKeyLeftControl, GHOST_kKeyLeftShift, GHOST_kKeyLeftAlt);
}

static int vk_key_owners(const VirtualKeyboard &vk, const GHOST_TKey key)
{
  int owners = 0;
  for (const VKSlot &slot : vk.slots) {
    if (!slot.active) {
      continue;
    }
    for (int i = 0; i < slot.count; i++) {
      if (slot.keys[i] == key) {
        owners++;
      }
    }
  }
  return owners;
}

static void vk_send_key_down(VirtualKeyboard &vk,
                             wmWindowManager *wm,
                             wmWindow *win,
                             GHOST_TKey key,
                             const VKSlot &activating_slot)
{
  int target[2];
  vk_target_position(vk, win, target);
  if (win->runtime->eventstate != nullptr) {
    copy_v2_v2_int(win->runtime->eventstate->xy, target);
  }
  const VKChoice *choice = vk_choice_for_key(key);
  const char *utf8 = choice ? choice->utf8 : nullptr;
  auto modifier_held = [&](const GHOST_TKey modifier) {
    if (vk_key_owners(vk, modifier) > 0) {
      return true;
    }
    for (int i = 0; i < activating_slot.count; i++) {
      if (activating_slot.keys[i] == modifier) {
        return true;
      }
    }
    return false;
  };
  if (choice != nullptr && choice->shifted != nullptr && modifier_held(GHOST_kKeyLeftShift)) {
    utf8 = choice->shifted;
  }
  if (modifier_held(GHOST_kKeyLeftControl) || modifier_held(GHOST_kKeyLeftAlt)) {
    utf8 = nullptr;
  }
  vk_send_ghost_key(wm, win, key, utf8, true);
}

static void vk_set_slot_active(VirtualKeyboard &vk,
                               wmWindowManager *wm,
                               wmWindow *win,
                               const int index,
                               const bool active)
{
  VKSlot &slot = vk.slots[index];
  if (slot.active == active || slot.count == 0) {
    return;
  }
  if (active) {
    /* Put modifiers first, even if they were assigned as the second key. */
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < slot.count; i++) {
        const GHOST_TKey key = slot.keys[i];
        if (vk_is_modifier(key) == (pass == 0) && vk_key_owners(vk, key) == 0) {
          vk_send_key_down(vk, wm, win, key, slot);
        }
      }
    }
    slot.active = true;
  }
  else {
    /* Drop non-modifiers first, and keep shared keys until their final owner releases. */
    slot.active = false;
    for (int pass = 0; pass < 2; pass++) {
      for (int i = slot.count - 1; i >= 0; i--) {
        const GHOST_TKey key = slot.keys[i];
        if (vk_is_modifier(key) == (pass == 1) && vk_key_owners(vk, key) == 0) {
          vk_send_ghost_key(wm, win, key, nullptr, false);
        }
      }
    }
  }
  BLI_strncpy(vk.last_key, slot.label, sizeof(vk.last_key));
  vk.last_key_time = BLI_time_now_seconds();
}

static void vk_release_all_slots(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win)
{
  for (int i = 0; i < VK_SLOT_COUNT; i++) {
    vk_set_slot_active(vk, wm, win, i, false);
  }
}

/* On reopening, clear modifiers stranded by an interrupted Android gesture. */
static void vk_release_stuck_mods(wmWindowManager *wm, wmWindow *win)
{
  const int held = (win->runtime->eventstate != nullptr) ?
                       win->runtime->eventstate->modifier : 0;
  if (held & KM_CTRL) {
    vk_send_ghost_key(wm, win, GHOST_kKeyLeftControl, nullptr, false);
  }
  if (held & KM_SHIFT) {
    vk_send_ghost_key(wm, win, GHOST_kKeyLeftShift, nullptr, false);
  }
  if (held & KM_ALT) {
    vk_send_ghost_key(wm, win, GHOST_kKeyLeftAlt, nullptr, false);
  }
}

/* -------------------------------------------------------------------- */
/** \name Open and close
 * \{ */

static void vk_close(wmWindowManager *wm, wmWindow *win)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open) {
    return;
  }
  vk_release_all_slots(vk, wm, win);
  if (vk.draw_handle != nullptr) {
    WM_draw_cb_exit(vk.win, vk.draw_handle);
    vk.draw_handle = nullptr;
  }
  vk.open = false;
  vk.win = nullptr;
  vk.editing = false;
  vk.picking = false;
  vk.selected_slot = -1;
  vk.pending_count = 0;
  vk.keys.clear();
  vk.text_edit = nullptr;
  vk.last_key[0] = '\0';
  vk.hover = -1;
  vk.pressed = -1;
  vk.moving = false;
  vk_tag_redraw(win);
}

static void vk_open(wmWindowManager *wm, wmWindow *win)
{
  VirtualKeyboard &vk = g_vk;
  if (vk.open) {
    return;
  }
  if (!vk.loaded) {
    vk_load(vk);
  }
  vk_release_stuck_mods(wm, win);
  vk.open = true;
  vk.win = win;
  vk.offset = 0.0f;
  vk.editing = false;
  vk.picking = false;
  vk.selected_slot = -1;
  vk.pending_count = 0;
  vk.text_edit = nullptr;
  vk.last_key[0] = '\0';
  vk.hover = -1;
  vk.pressed = -1;
  vk.moving = false;
  copy_v2_v2_int(vk.pinned, win->runtime->eventstate->xy);
  vk_build_layout(vk, win);
  vk.draw_handle = WM_draw_cb_activate(win, vk_draw_cb, nullptr);
  vk_tag_redraw(win);
}

void WM_virtual_keyboard_text_edit_begin(const wmWindow *win, const char *const *string)
{
  if (g_vk.open && g_vk.win == win) {
    g_vk.text_edit = string;
    vk_tag_redraw(g_vk.win);
  }
}

void WM_virtual_keyboard_text_edit_end(const wmWindow *win)
{
  if (g_vk.text_edit != nullptr && g_vk.win == win) {
    g_vk.text_edit = nullptr;
    vk_tag_redraw(g_vk.win);
  }
}

bool WM_virtual_keyboard_is_open(const wmWindow *win)
{
  return g_vk.open && (win == nullptr || g_vk.win == win);
}

bool WM_virtual_keyboard_rect_get(const wmWindow *win, rcti *r_rect)
{
  if (!WM_virtual_keyboard_is_open(win)) {
    return false;
  }
  *r_rect = g_vk.rect;
  return true;
}

void WM_virtual_keyboard_toggle(wmWindowManager *wm, wmWindow *win)
{
  if (g_vk.open) {
    vk_close(wm, win);
  }
  else {
    vk_open(wm, win);
  }
}

void wm_virtual_keyboard_window_close(wmWindow *win)
{
  if (g_vk.open && g_vk.win == win) {
    /* The draw callback belongs to a window that is going away, so drop it without touching it. */
    g_vk.draw_handle = nullptr;
    g_vk.open = false;
    g_vk.win = nullptr;
    /* The window is going away and takes its event state with it, so there is nothing to
     * release the modifiers to. */
    for (VKSlot &slot : g_vk.slots) {
      slot.active = false;
    }
    g_vk.editing = false;
    g_vk.picking = false;
    g_vk.keys.clear();
    g_vk.text_edit = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Input
 * \{ */

static void vk_press(VirtualKeyboard &vk, wmWindow *win, int index)
{
  vk.pressed = index;
  vk.moving = (index != -1) && (vk.keys[index].spec->kind == VKKind::Move);
  copy_v2_v2_int(vk.drag_prev, vk.cursor);
  vk_tag_redraw(win);
}

static void vk_toggle_pending(VirtualKeyboard &vk, const GHOST_TKey key)
{
  for (int i = 0; i < vk.pending_count; i++) {
    if (vk.pending[i] == key) {
      for (int j = i; j + 1 < vk.pending_count; j++) {
        vk.pending[j] = vk.pending[j + 1];
      }
      vk.pending_count--;
      return;
    }
  }
  if (vk.pending_count < 2) {
    vk.pending[vk.pending_count++] = key;
  }
}

static void vk_release(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win)
{
  const int index = vk.pressed;
  const bool was_moving = vk.moving;
  vk.pressed = -1;
  vk.moving = false;
  if (index == -1) {
    vk_tag_redraw(win);
    return;
  }
  const VKKeySpec &spec = *vk.keys[index].spec;
  if (!was_moving && BLI_rcti_isect_pt_v(&vk.keys[index].rect, vk.cursor)) {
    switch (spec.kind) {
      case VKKind::Close:
        vk_close(wm, win);
        return;
      case VKKind::Move:
        break;
      case VKKind::Edit:
        vk_release_all_slots(vk, wm, win);
        vk.editing = true;
        vk.picking = false;
        break;
      case VKKind::Done:
        vk.editing = false;
        vk.picking = false;
        break;
      case VKKind::Reset:
        vk_release_all_slots(vk, wm, win);
        vk_default_slots(vk);
        vk.save_failed = !vk_save(vk);
        break;
      case VKKind::Slot:
        if (!vk.editing) {
          vk_set_slot_active(vk, wm, win, spec.code, !vk.slots[spec.code].active);
        }
        else {
          vk.selected_slot = spec.code;
          vk.pending_count = vk.slots[spec.code].count;
          for (int i = 0; i < vk.pending_count; i++) {
            vk.pending[i] = vk.slots[spec.code].keys[i];
          }
          vk.picker_page = 0;
          vk.picking = true;
        }
        break;
      case VKKind::Pick:
        vk_toggle_pending(vk, vk_choices[spec.code].key);
        break;
      case VKKind::Prev:
        vk.picker_page = max_ii(0, vk.picker_page - 1);
        break;
      case VKKind::Next:
        if ((vk.picker_page + 1) * VK_PICK_PAGE_SIZE < int(ARRAY_SIZE(vk_choices))) {
          vk.picker_page++;
        }
        break;
      case VKKind::Clear:
        vk.pending_count = 0;
        break;
      case VKKind::Cancel:
        vk.picking = false;
        break;
      case VKKind::Save:
        if (vk.selected_slot >= 0 && vk.selected_slot < VK_SLOT_COUNT) {
          VKSlot &slot = vk.slots[vk.selected_slot];
          slot.count = vk.pending_count;
          for (int i = 0; i < 2; i++) {
            slot.keys[i] = i < slot.count ? vk.pending[i] : GHOST_kKeyUnknown;
          }
          vk_slot_label(slot);
          vk.save_failed = !vk_save(vk);
        }
        vk.picking = false;
        break;
    }
  }
  vk_tag_redraw(win);
}

/**
 * Whether a popup drawn over the keyboard owns this point.
 *
 * A menu or a pie opened while the keyboard is up is drawn above it, and the part of it that falls
 * over the panel has to stay reachable. Answered by position rather than by the mere presence of a
 * popup, so a search box opened while typing takes only the taps that land on it and the keys
 * around it keep working.
 *
 * A running modal operator deliberately does *not* count, which was learned the hard way. Standing
 * aside for one meant that starting, say, circle select from the keyboard left the whole interface
 * unusable: the operator holds every pointer event until it is confirmed or cancelled, and Esc is
 * on the keyboard that had just gone inert. On a device with no other keyboard the only way out
 * was to kill the application. Losing part of a control that draws under the panel is a nuisance;
 * being unable to leave it is not.
 *
 * Text editing is not included either. It runs as a UI handler rather than as an operator, which
 * is exactly why the keyboard can type into it.
 */
static bool vk_point_is_owned(const wmWindow *win, const int xy[2])
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen != nullptr) {
    for (const ARegion &region : screen->regionbase) {
      if (region.runtime->visible && BLI_rcti_isect_pt_v(&region.winrct, xy)) {
        return true;
      }
    }
  }
  return false;
}

bool wm_virtual_keyboard_ghost_event(wmWindowManager *wm,
                                     wmWindow *win,
                                     const int type,
                                     const void *customdata)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open || vk.win != win) {
    return false;
  }
  vk_ensure_layout(vk, win);

  switch (type) {
    case GHOST_kEventWindowDeactivate:
      vk_release_all_slots(vk, wm, win);
      vk_tag_redraw(win);
      return false;
    case GHOST_kEventCursorMove: {
      const GHOST_TEventCursorData *cd = static_cast<const GHOST_TEventCursorData *>(customdata);
      int xy[2] = {cd->x, cd->y};
      wm_cursor_position_from_ghost_screen_coords(win, &xy[0], &xy[1]);
      copy_v2_v2_int(vk.cursor, xy);

      if (vk.moving) {
        const int dy = xy[1] - vk.drag_prev[1];
        copy_v2_v2_int(vk.drag_prev, xy);
        if (dy != 0) {
          vk.offset += float(dy);
          vk_ensure_layout(vk, win);
          vk_tag_redraw(win);
        }
        return true;
      }

      if (!BLI_rcti_isect_pt_v(&vk.rect, xy)) {
        /* Outside: remember where, so an injected key lands in that editor, and let the move
         * through untouched. */
        if (vk_position_in_area(win, xy)) {
          copy_v2_v2_int(vk.pinned, xy);
        }
        return false;
      }
      if (vk_point_is_owned(win, xy)) {
        return false;
      }

      const int hover = vk_key_at(vk, xy);
      if (hover != vk.hover) {
        vk.hover = hover;
        vk_tag_redraw(win);
      }
      return true;
    }
    case GHOST_kEventButtonDown:
    case GHOST_kEventButtonUp: {
      const GHOST_TEventButtonData *bd = static_cast<const GHOST_TEventButtonData *>(customdata);
      const bool on_panel = BLI_rcti_isect_pt_v(&vk.rect, vk.cursor) &&
                            !vk_point_is_owned(win, vk.cursor);
      /* Touch: only the left button presses a key.
       *
       * A finger held still arrives as a right click instead --
       * GHOST_SystemAndroid::touchLongPressCheck() turns a stationary press into one at
       * TOUCH_LONG_PRESS_MS and cancels the pending left press to do it -- so a held key does
       * nothing here. That is deliberate. Taking the right click as a key press was tried and
       * removed: it made press-and-hold work and it also made the keyboard unpredictable enough
       * to be worse than not having the gesture. A tap toggles a modifier and it stays on, which
       * is all the gesture was ever reaching for. */
      if (bd->button != GHOST_kButtonMaskLeft) {
        return on_panel;
      }
      if (type == GHOST_kEventButtonDown) {
        if (!on_panel) {
          return false;
        }
        vk_press(vk, win, vk_key_at(vk, vk.cursor));
        return true;
      }
      /* A release always ends the press this keyboard owns, even if the finger has wandered off
       * the panel: dropping it would leave a key stuck down. Anything else is only ours if the
       * press was, which is what keeps a menu drawn over the panel usable: its items act on the
       * release, and swallowing that left them highlighted but never chosen. */
      if (vk.pressed == -1 && !vk.moving) {
        return on_panel;
      }
      vk_release(vk, wm, win);
      return true;
    }
    case GHOST_kEventTrackpad: {
      /* A two finger gesture that starts on the keyboard must not scroll the editor behind it. */
      return BLI_rcti_isect_pt_v(&vk.rect, vk.cursor) && !vk_point_is_owned(win, vk.cursor);
    }
    default:
      break;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static wmOperatorStatus wm_virtual_keyboard_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    return OPERATOR_CANCELLED;
  }
  WM_virtual_keyboard_toggle(CTX_wm_manager(C), win);
  return OPERATOR_FINISHED;
}

void WM_OT_virtual_keyboard_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Virtual Keyboard";
  ot->idname = "WM_OT_virtual_keyboard_toggle";
  ot->description =
      "Show or hide the 14-key customizable shortcut pad";

  ot->exec = wm_virtual_keyboard_toggle_exec;
  ot->poll = WM_operator_winactive;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender
