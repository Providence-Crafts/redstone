/*
 * Test suites. One runner links every suite, so each test file exports a
 * single entry point and none of them define main.
 */
#ifndef SQLSH_TEST_SUITES_H
#define SQLSH_TEST_SUITES_H

const char *db_suite(void);
const char *edit_suite(void);
const char *line_suite(void);

#endif /* SQLSH_TEST_SUITES_H */
