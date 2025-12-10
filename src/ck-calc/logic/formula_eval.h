#ifndef CK_CALC_FORMULA_EVAL_H
#define CK_CALC_FORMULA_EVAL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char buffer[512];
    size_t len;
} FormulaCtx;

void formula_init(FormulaCtx *ctx);
void formula_clear(FormulaCtx *ctx);
const char *formula_text(const FormulaCtx *ctx);
bool formula_is_empty(const FormulaCtx *ctx);
bool formula_append_char(FormulaCtx *ctx, char c);
bool formula_append_str(FormulaCtx *ctx, const char *str);
bool formula_backspace(FormulaCtx *ctx);
bool formula_evaluate(const FormulaCtx *ctx, double *out_value);

#endif /* CK_CALC_FORMULA_EVAL_H */
