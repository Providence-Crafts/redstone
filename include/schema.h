/*
 * schema.h - the dot commands that render the database as text.
 *
 * `.schema`, `.dump`, `.fullschema`, `.indexes`, `.databases`, `.dbinfo` and
 * `.dbtotxt`. They are together because they share one job: producing text
 * that sqlite3(1) produces byte for byte, which is a different discipline
 * from the settings commands in dot.c.
 */
#ifndef SQLSH_SCHEMA_H
#define SQLSH_SCHEMA_H

#include "dot.h"

bool schema_cmd_schema(Shell *sh, int argc, char **argv);
bool schema_cmd_fullschema(Shell *sh, int argc, char **argv);
bool schema_cmd_tables(Shell *sh, int argc, char **argv);
bool schema_cmd_indexes(Shell *sh, int argc, char **argv);
bool schema_cmd_databases(Shell *sh, int argc, char **argv);
bool schema_cmd_dump(Shell *sh, int argc, char **argv);
bool schema_cmd_dbinfo(Shell *sh, int argc, char **argv);
bool schema_cmd_dbtotxt(Shell *sh, int argc, char **argv);
bool schema_cmd_clone(Shell *sh, int argc, char **argv);
bool schema_cmd_lint(Shell *sh, int argc, char **argv);

#endif /* SQLSH_SCHEMA_H */
