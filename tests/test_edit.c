/*
 * Tests for the editing core.
 *
 * Because `edit.c` does no I/O, every keybinding is exercised by feeding bytes
 * and reading the buffer back. No pty, no terminal, no timing.
 */
#include "edit.h"
#include "minunit.h"
#include "suites.h"

#include <string.h>

#define ESC "\x1b"

/* Feed a string byte by byte, discarding the actions. */
static void feed(Edit *ed, const char *bytes)
{
    size_t i;

    for (i = 0u; bytes[i] != '\0'; i++) {
        (void)edit_feed(ed, (unsigned char)bytes[i]);
    }
}

/* Feed a string, then tell the core no further byte followed — what the
 * terminal layer does after its escape timeout. Needed whenever the input ends
 * in Esc, which is most of vi mode. */
static void feed_esc(Edit *ed, const char *bytes)
{
    feed(ed, bytes);
    (void)edit_timeout(ed);
}

/* Feed a string and return the action the last byte produced. */
static EditAction feed_last(Edit *ed, const char *bytes)
{
    EditAction act = EDIT_NONE;
    size_t i;

    for (i = 0u; bytes[i] != '\0'; i++) {
        act = edit_feed(ed, (unsigned char)bytes[i]);
    }
    return act;
}

static const char *test_insert_and_cursor(void)
{
    Edit *ed = edit_new();
    bool ok;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "select 1");
    ok = strcmp(edit_buffer(ed), "select 1") == 0 && edit_cursor(ed) == 8u && edit_len(ed) == 8u;
    edit_free(ed);
    mu_assert("plain insertion wrong", ok);
    return NULL;
}

static const char *test_emacs_motion(void)
{
    Edit *ed = edit_new();
    bool home;
    bool right;
    bool end;
    bool arrow;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "abc");
    feed(ed, "\x01"); /* Ctrl-A */
    home = edit_cursor(ed) == 0u;
    feed(ed, "\x06"); /* Ctrl-F */
    right = edit_cursor(ed) == 1u;
    feed(ed, "\x05"); /* Ctrl-E */
    end = edit_cursor(ed) == 3u;
    feed(ed, ESC "[D" ESC "[D"); /* two left arrows */
    arrow = edit_cursor(ed) == 1u;
    edit_free(ed);

    mu_assert("Ctrl-A should go home", home);
    mu_assert("Ctrl-F should step right", right);
    mu_assert("Ctrl-E should go to end", end);
    mu_assert("arrow keys should move the cursor", arrow);
    return NULL;
}

static const char *test_emacs_edit(void)
{
    Edit *ed = edit_new();
    bool backspace;
    bool kill_end;
    bool kill_word;
    bool transpose;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "abcd\x7f"); /* backspace */
    backspace = strcmp(edit_buffer(ed), "abc") == 0;

    edit_reset(ed);
    feed(ed, "hello world\x01\x0b"); /* home, then Ctrl-K */
    kill_end = edit_len(ed) == 0u;

    edit_reset(ed);
    feed(ed, "select from t\x17"); /* Ctrl-W */
    kill_word = strcmp(edit_buffer(ed), "select from ") == 0;

    edit_reset(ed);
    feed(ed, "ab\x02\x14"); /* left, then Ctrl-T */
    transpose = strcmp(edit_buffer(ed), "ba") == 0;

    edit_free(ed);
    mu_assert("backspace wrong", backspace);
    mu_assert("Ctrl-K should kill to end", kill_end);
    mu_assert("Ctrl-W should kill the previous word", kill_word);
    mu_assert("Ctrl-T should transpose", transpose);
    return NULL;
}

/* Regression: backspace at end-of-line used to double-decrement the cursor
 * (buf_delete()'s own clamp moved it back, then the caller moved it again),
 * so every backspace after the first deleted the character before the one
 * intended and the true last character was never reached. Repeated
 * backspaces must fully clear the buffer, one character at a time, with the
 * cursor always tracking the length. */
static const char *test_backspace_repeated(void)
{
    Edit *ed = edit_new();
    const char *text = "positions";
    size_t n = strlen(text);
    size_t i;
    bool ok = true;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, text);
    for (i = 0u; i < n; i++) {
        size_t before = edit_len(ed);

        (void)edit_feed(ed, 0x7f);
        if (edit_len(ed) != before - 1u || edit_cursor(ed) != edit_len(ed)) {
            ok = false;
        }
    }
    mu_assert("repeated backspace should fully and correctly clear the buffer", ok);
    mu_assert("buffer should be empty", edit_len(ed) == 0u);
    mu_assert("buffer text should be empty", strcmp(edit_buffer(ed), "") == 0);

    edit_free(ed);
    return NULL;
}

static const char *test_actions(void)
{
    Edit *ed = edit_new();
    EditAction accept;
    EditAction eof;
    EditAction intr;
    EditAction clear;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "x");
    accept = edit_feed(ed, '\r');
    edit_reset(ed);
    eof = edit_feed(ed, 0x04); /* Ctrl-D on an empty buffer */
    intr = edit_feed(ed, 0x03);
    clear = edit_feed(ed, 0x0c);
    edit_free(ed);

    mu_assert("Enter should accept", accept == EDIT_ACCEPT);
    mu_assert("Ctrl-D on empty should be EOF", eof == EDIT_EOF);
    mu_assert("Ctrl-C should interrupt", intr == EDIT_INTR);
    mu_assert("Ctrl-L should clear", clear == EDIT_CLEAR);
    return NULL;
}

/* An unrecognised escape sequence must be swallowed whole; the failure this
 * guards against is a stray "[5~" appearing in the SQL. */
static const char *test_escape_swallowed(void)
{
    Edit *ed = edit_new();
    bool ok;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "a" ESC "[5~"
             "b");
    ok = strcmp(edit_buffer(ed), "ab") == 0;
    edit_free(ed);
    mu_assert("unknown escape leaked into the buffer", ok);
    return NULL;
}

static const char *test_history(void)
{
    Edit *ed = edit_new();
    bool added;
    bool dup;
    bool blank;
    bool prev;
    bool back;
    bool stash;

    mu_assert("edit_new failed", ed != NULL);
    added = edit_history_add(ed, "select 1;") && edit_history_count(ed) == 1u;
    dup = !edit_history_add(ed, "select 1;");
    blank = !edit_history_add(ed, "   ");
    (void)edit_history_add(ed, "select 2;");

    edit_reset(ed);
    feed(ed, "partial");
    feed(ed, ESC "[A"); /* up */
    prev = strcmp(edit_buffer(ed), "select 2;") == 0;
    feed(ed, ESC "[A");
    back = strcmp(edit_buffer(ed), "select 1;") == 0;
    feed(ed, ESC "[B" ESC "[B"); /* down twice, back to the typed line */
    stash = strcmp(edit_buffer(ed), "partial") == 0;
    edit_free(ed);

    mu_assert("history add failed", added);
    mu_assert("consecutive duplicate should be dropped", dup);
    mu_assert("blank entry should be dropped", blank);
    mu_assert("up should recall the newest entry", prev);
    mu_assert("up twice should recall the older entry", back);
    mu_assert("down should restore the typed line", stash);
    return NULL;
}

static const char *test_history_cap(void)
{
    Edit *ed = edit_new();
    bool capped;
    bool oldest;

    mu_assert("edit_new failed", ed != NULL);
    edit_history_set_max(ed, 2u);
    (void)edit_history_add(ed, "one");
    (void)edit_history_add(ed, "two");
    (void)edit_history_add(ed, "three");
    capped = edit_history_count(ed) == 2u;
    oldest = edit_history_at(ed, 0u) != NULL && strcmp(edit_history_at(ed, 0u), "two") == 0;
    edit_history_clear(ed);
    capped = capped && edit_history_count(ed) == 0u;
    edit_free(ed);

    mu_assert("history should be capped", capped);
    mu_assert("the oldest entry should be dropped first", oldest);
    return NULL;
}

/* --- vi mode ----------------------------------------------------------- */

static const char *test_vi_modes(void)
{
    Edit *ed = edit_new();
    bool normal;
    bool moved;
    bool appended;

    mu_assert("edit_new failed", ed != NULL);
    edit_set_keymap(ed, EDIT_VI);
    mu_assert("vi should start in insert", edit_vi_state(ed) == EDIT_VI_INSERT);

    feed_esc(ed, "abc" ESC);
    normal = edit_vi_state(ed) == EDIT_VI_NORMAL && edit_cursor(ed) == 2u;
    feed(ed, "0");
    moved = edit_cursor(ed) == 0u;
    feed(ed, "Axy");
    appended = strcmp(edit_buffer(ed), "abcxy") == 0 && edit_vi_state(ed) == EDIT_VI_INSERT;
    edit_free(ed);

    mu_assert("Esc should enter normal mode and step left", normal);
    mu_assert("0 should go to column 0", moved);
    mu_assert("A should append at end of line", appended);
    return NULL;
}

static const char *test_vi_operators(void)
{
    Edit *ed = edit_new();
    bool dw;
    bool dd;
    bool cw;
    bool xx;
    bool replaced;

    mu_assert("edit_new failed", ed != NULL);
    edit_set_keymap(ed, EDIT_VI);

    feed(ed, "select from t" ESC "0dw");
    dw = strcmp(edit_buffer(ed), "from t") == 0;

    edit_reset(ed);
    edit_set_keymap(ed, EDIT_VI);
    feed(ed, "throw away" ESC "dd");
    dd = edit_len(ed) == 0u;

    edit_reset(ed);
    edit_set_keymap(ed, EDIT_VI);
    feed(ed, "alpha beta" ESC "0cwgamma ");
    cw = strcmp(edit_buffer(ed), "gamma beta") == 0;

    edit_reset(ed);
    edit_set_keymap(ed, EDIT_VI);
    feed(ed, "abcdef" ESC "03x");
    xx = strcmp(edit_buffer(ed), "def") == 0;

    edit_reset(ed);
    edit_set_keymap(ed, EDIT_VI);
    feed(ed, "cat" ESC "0rb");
    replaced = strcmp(edit_buffer(ed), "bat") == 0;

    edit_free(ed);
    mu_assert("dw should delete a word", dw);
    mu_assert("dd should delete the line", dd);
    mu_assert("cw should change a word and enter insert", cw);
    mu_assert("3x should delete three characters", xx);
    mu_assert("r should replace one character", replaced);
    return NULL;
}

static const char *test_vi_counts_and_undo(void)
{
    Edit *ed = edit_new();
    bool counted;
    bool undone;
    EditAction accepted;

    mu_assert("edit_new failed", ed != NULL);
    edit_set_keymap(ed, EDIT_VI);

    feed(ed, "one two three" ESC "02w");
    counted = edit_cursor(ed) == 8u;

    feed(ed, "dw");
    undone = strcmp(edit_buffer(ed), "one two ") == 0;
    feed(ed, "u");
    undone = undone && strcmp(edit_buffer(ed), "one two three") == 0;

    accepted = feed_last(ed, "\r");
    edit_free(ed);

    mu_assert("2w should skip two words", counted);
    mu_assert("u should undo the last change", undone);
    mu_assert("Enter in normal mode should accept", accepted == EDIT_ACCEPT);
    return NULL;
}

/* Unbound keys in normal mode must never reach the buffer: typing `z` at the
 * wrong moment silently corrupting a statement is the classic vi-mode bug. */
static const char *test_vi_normal_is_not_insert(void)
{
    Edit *ed = edit_new();
    bool ok;

    mu_assert("edit_new failed", ed != NULL);
    edit_set_keymap(ed, EDIT_VI);
    feed(ed, "abc" ESC "zqp");
    ok = strcmp(edit_buffer(ed), "abc") == 0;
    edit_free(ed);
    mu_assert("normal mode inserted text", ok);
    return NULL;
}

/* --- completion keys ---------------------------------------------------- */

static const char *test_completion_actions(void)
{
    Edit *ed = edit_new();
    bool tab;
    bool nav;
    bool sideways;
    bool accept;
    bool cancel;
    bool inert;

    mu_assert("edit_new failed", ed != NULL);
    tab = feed_last(ed, "sel\t") == EDIT_COMPLETE && !edit_is_completing(ed);

    /* Until the caller says a menu is open, the navigation keys keep their
     * ordinary meanings -- Ctrl-P must still walk history, and the arrows
     * still move the cursor. */
    inert = feed_last(ed, "\x10") != EDIT_COMP_PREV && feed_last(ed, "\x1b[D") != EDIT_COMP_PREV &&
            feed_last(ed, "\x1b[C") != EDIT_COMP_NEXT;

    edit_completing(ed, true);
    nav = feed_last(ed, "\t") == EDIT_COMP_NEXT && feed_last(ed, "\x1b[Z") == EDIT_COMP_PREV &&
          feed_last(ed, "\x0e") == EDIT_COMP_NEXT && feed_last(ed, "\x10") == EDIT_COMP_PREV &&
          feed_last(ed, "\x1b[B") == EDIT_COMP_DOWN && feed_last(ed, "\x1b[A") == EDIT_COMP_UP;
    /* With a menu open, left/right drive the selection too -- the edit
     * buffer's cursor never moves while a choice is being made. */
    sideways =
        feed_last(ed, "\x1b[D") == EDIT_COMP_PREV && feed_last(ed, "\x1b[C") == EDIT_COMP_NEXT;
    accept = feed_last(ed, "\r") == EDIT_COMP_ACCEPT;
    cancel = feed_last(ed, "\x07") == EDIT_COMP_CANCEL;
    edit_free(ed);

    mu_assert("Tab should ask for completion", tab);
    mu_assert("navigation keys should be ordinary keys with no menu open", inert);
    mu_assert("a navigation key was not routed to the menu", nav);
    mu_assert("left and right should drive the menu selection while it is open", sideways);
    mu_assert("Enter should accept the selection", accept);
    mu_assert("Ctrl-G should dismiss the menu", cancel);
    return NULL;
}

/* Anything that is not a menu key must still edit, so that typing narrows. */
static const char *test_completion_lets_typing_through(void)
{
    Edit *ed = edit_new();
    bool typed;
    bool erased;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "sel");
    edit_completing(ed, true);
    typed = feed_last(ed, "e") == EDIT_REDRAW && strcmp(edit_buffer(ed), "sele") == 0;
    erased = feed_last(ed, "\x7f") == EDIT_REDRAW && strcmp(edit_buffer(ed), "sel") == 0;
    edit_free(ed);

    mu_assert("an ordinary key should still insert while completing", typed);
    mu_assert("backspace should still delete while completing", erased);
    return NULL;
}

static const char *test_replace_range(void)
{
    Edit *ed = edit_new();
    bool replaced;
    bool cursor;
    bool rejected;

    mu_assert("edit_new failed", ed != NULL);
    feed(ed, "select * from emp");
    replaced = edit_replace_range(ed, 14u, 17u, "employees") &&
               strcmp(edit_buffer(ed), "select * from employees") == 0;
    cursor = edit_cursor(ed) == 23u;
    rejected = !edit_replace_range(ed, 5u, 2u, "x") && !edit_replace_range(ed, 0u, 999u, "x");
    edit_free(ed);

    mu_assert("replace_range produced the wrong buffer", replaced);
    mu_assert("the cursor should land after the inserted text", cursor);
    mu_assert("an inverted or out-of-range span should be refused", rejected);
    return NULL;
}

const char *edit_suite(void)
{
    mu_run_test(test_insert_and_cursor);
    mu_run_test(test_emacs_motion);
    mu_run_test(test_emacs_edit);
    mu_run_test(test_backspace_repeated);
    mu_run_test(test_actions);
    mu_run_test(test_escape_swallowed);
    mu_run_test(test_history);
    mu_run_test(test_history_cap);
    mu_run_test(test_vi_modes);
    mu_run_test(test_vi_operators);
    mu_run_test(test_vi_counts_and_undo);
    mu_run_test(test_vi_normal_is_not_insert);
    mu_run_test(test_completion_actions);
    mu_run_test(test_completion_lets_typing_through);
    mu_run_test(test_replace_range);
    return NULL;
}
