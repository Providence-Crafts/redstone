/*
 * import.h - reading tabular text in, and handing tabular text out.
 *
 * `.import` and the two commands that are an output mode plus a program:
 * `.excel`, which writes CSV and opens it, and `.www`, which writes HTML and
 * opens it. They share the temp-file-and-launch machinery.
 */
#ifndef SQLSH_IMPORT_H
#define SQLSH_IMPORT_H

#include "dot.h"

bool import_cmd_import(Shell *sh, int argc, char **argv);
bool import_cmd_excel(Shell *sh, int argc, char **argv);
bool import_cmd_www(Shell *sh, int argc, char **argv);

#endif /* SQLSH_IMPORT_H */
