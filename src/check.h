#ifndef VOLT_CHECK_H
#define VOLT_CHECK_H

#include "ast.h"

struct Local {
    const char *name;
    Type *type;
    const char *cname; /* unique C identifier within the function */
    bool is_param;
    bool is_this;
    bool assigned; /* body assigns to it; ref-typed params need a defensive retain */
    struct Local *next_in_fn;
};

/* Check every module in the list (entry must define fn main()). Annotates the AST. */
void check_all(Module *mods, Module *entry);

/* shared type helpers (used by emitter too) */
bool type_identical(const Type *a, const Type *b);
ClassDecl *class_find_field(ClassDecl *cd, const char *name, Field **out);
FnDecl *class_find_method(ClassDecl *cd, const char *name);
bool class_derives(const ClassDecl *sub, const ClassDecl *base);

#endif
