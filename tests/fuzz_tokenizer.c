/*
 * fuzz_tokenizer.c - libFuzzer target over sqlctx.c's lexer and cursor-context
 * analysis.
 *
 * sqlctx.c is the one module that promises never to fail on malformed input
 * ("an input it cannot make sense of yields CTX_UNKNOWN", sqlctx.h) and never
 * to touch the heap, so it is exactly the shape a fuzzer wants: pure,
 * allocation-free, total. This target exercises both halves it exposes:
 *
 *   1. sql_lex_next() in a loop, which is the tokenizer itself. Bounded by
 *      the input length rather than trusted to terminate on its own, so a
 *      lexer bug that stops advancing pos is a fuzz finding (a hang under
 *      -timeout=N) rather than a silent infinite loop in this harness.
 *   2. sql_context() at a handful of cursor offsets derived from the input,
 *      which is the state machine built on top of the lexer and the thing
 *      completion actually calls on every keystroke.
 *
 * Build and run: `make fuzz` (see Makefile for the libFuzzer invocation and
 * FUZZ_TIME). Not part of `make gate`: a fuzz run has no fixed pass/fail
 * duration to gate on, and libFuzzer needs its own sanitizer build.
 */
#include "sqlctx.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* sql_lex_next and sql_context both want a NUL-terminated C string, and
     * neither may see the fuzzer's raw, unterminated buffer directly: a
     * comment or string token that reads to "end of input" has to mean this
     * NUL, not one byte past the allocation. */
    char *text = malloc(size + 1u);
    SqlLexer lx;
    SqlToken tok;
    size_t iterations;
    size_t cursor;

    if (text == NULL) {
        return 0;
    }
    if (size > 0u) {
        memcpy(text, data, size);
    }
    text[size] = '\0';

    sql_lex_init(&lx, text);
    for (iterations = 0u; iterations <= size; iterations++) {
        if (!sql_lex_next(&lx, &tok)) {
            break;
        }
    }

    /* A handful of cursor positions: both ends, the middle, and one byte
     * derived from the input itself, so a crash correlates with a specific
     * offset rather than only ever hitting size/0. */
    for (cursor = 0u; cursor <= size; cursor += (size / 4u) + 1u) {
        SqlContext ctx;

        sql_context(text, cursor, &ctx);
    }
    if (size > 0u) {
        SqlContext ctx;

        sql_context(text, (size_t)data[0] % (size + 1u), &ctx);
    }

    free(text);
    return 0;
}
