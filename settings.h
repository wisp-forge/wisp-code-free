// language: C, file: wisp-code/settings.h
#ifndef SETTINGS_H
#define SETTINGS_H

/* Modal settings overlay + the effective runtime configuration.
 *
 * The overlay is defined in settings.c. Values are persisted to
 * <project_root>/wisp-code.conf on every commit (Enter in the editor,
 * ←/→ on a cycler, or after a model fetch). Loading happens once at
 * startup via settings_load().
 *
 * The settings_* accessors are the single source of truth for the
 * values http.c and console.c read at request time. No compile-time
 * macro duplicates them.
 *
 * Usage from the main loop:
 *
 *   if (settings_is_open()) {
 *       if (settings_handle_key(ch)) g_redraw = 1;
 *       // skip the normal key handling while open
 *   } else if (ch == 27) { ... Alt-S opens it ... }
 *
 * And once per frame, after the base layout is drawn:
 *
 *   if (settings_is_open()) settings_render();
 */

/* --- Overlay interface -------------------------------------------- */

/* Open the overlay with the selection on the first row. Idempotent. */
void settings_open(void);

/* Close the overlay. Idempotent. */
void settings_close(void);

/* Return 1 if the overlay is currently open. */
int  settings_is_open(void);

/* Handle a key press while the overlay is open.
 * Returns 1 if the key was consumed (redraw needed), 0 otherwise. */
int  settings_handle_key(int ch);

/* Draw the overlay on top of the current frame. Assumes the base
 * layout has already been rendered this frame. */
void settings_render(void);

/* --- Persistence -------------------------------------------------- */

/* Load persisted settings from <project_root>/wisp-code.conf into the
 * row model. Missing keys leave the compiled-in defaults in place.
 * Malformed values (bad endpoint, bad osc8) are ignored. Called once
 * at startup. */
void settings_load(const char *project_root);

/* Persist the current row values to <project_root>/wisp-code.conf.
 * Keys owned by other modules (build, lang, theme) are preserved
 * verbatim. Called from the overlay after each commit. */
void settings_save(const char *project_root);

/* --- Effective runtime values ------------------------------------- */
/*                                                                    */
/* Each accessor returns a pointer into the row model's own storage.  */
/* The pointer is valid until the next commit to that row. Callers    */
/* that hold the value across an await point (e.g. a network round-   */
/* trip) must copy it into a local buffer first.                      */

/* The URL the HTTP client posts to. Derived from the api_url row. */
const char *settings_api_url(void);

/* The bearer token. Empty when the user has cleared it. */
const char *settings_api_key(void);

/* The model identifier sent in the request body. */
const char *settings_model(void);

/* Request timeout in milliseconds. Returns the compiled-in default
 * (180000) when the row is empty or not parseable. */
int settings_timeout_ms(void);

/* Maximum auto-continues per user turn. 0 = none; failed %%%EDITs
 * land as [FAILED] sections and the turn ends. 1 = one shot shared
 * between edit-retry and read-continue. 2 = the default, matching
 * the compiled-in AUTO_CONTINUE_MAX. */
int settings_auto_retry(void);

/* OSC 8 hyperlink mode. 0 = auto (use the terminal heuristic in
 * console.c), 1 = force on, 2 = force off. */
int settings_osc8_mode(void);

/* Filename of the project-rules Markdown file, resolved relative to
 * the project root. Empty disables the feature. Read once at startup
 * by console.c and appended to the system prefix inside history[0],
 * between the tool prompt and the operator block. Byte-stable for the
 * whole session, so it rides the prompt cache on every turn. */
const char *settings_rules_file(void);

/* Filename of the operator-instructions Markdown file, resolved
 * relative to the project root. Empty disables the feature. Read once
 * at startup by console.c and appended to the system prefix inside
 * history[0], between the project rules and the [ENV] line.
 * Byte-stable for the whole session, so it rides the prompt cache on
 * every turn. Protected from pruning: history_prune_locked() never
 * drops index 0. */
const char *settings_operator_file(void);

/* Return 1 if the overlay is open AND the current row is in edit
 * mode. Used by console.c to avoid hiding the cursor mid-edit. */
int settings_is_editing(void);

#endif