#ifndef VOLT_PARSE_H
#define VOLT_PARSE_H

#include "ast.h"

/* Parse one source file into a Module (name = file stem, mangled-safe). */
Module *parse_module(const char *path, const char *modname, const char *src);

#endif
