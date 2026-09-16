#include "minunit.h"
#include "suites.h"

#include <stdio.h>

int tests_run = 0;

/* Named so a failure report says which suite stopped, since minunit aborts the
 * whole run on the first failing assertion. */
static const char *all_tests(void)
{
    const char *msg = db_suite();

    if (msg == NULL) {
        msg = edit_suite();
    }
    if (msg == NULL) {
        msg = sqlctx_suite();
    }
    if (msg == NULL) {
        msg = comp_suite();
    }
    if (msg == NULL) {
        msg = menu_suite();
    }
    if (msg == NULL) {
        msg = line_suite();
    }
    if (msg == NULL) {
        msg = out_suite();
    }
    if (msg == NULL) {
        msg = width_suite();
    }
    return msg;
}

int main(void)
{
    const char *result = all_tests();

    if (result != NULL) {
        printf("FAIL: %s\n", result);
    } else {
        printf("All tests passed.\n");
    }
    printf("Tests run: %d\n", tests_run);
    return result != NULL ? 1 : 0;
}
