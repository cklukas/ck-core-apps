#include "formula_eval.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void skip_spaces(const char **ptr)
{
    if (!ptr || !*ptr) return;
    while (**ptr && isspace((unsigned char)**ptr)) {
        (*ptr)++;
    }
}

static double parse_expression(const char **ptr, bool *ok);
static double parse_term(const char **ptr, bool *ok);
static double parse_factor(const char **ptr, bool *ok);

static double parse_number(const char **ptr, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    char *end = NULL;
    double value = strtod(*ptr, &end);
    if (end == *ptr) {
        *ok = false;
        return 0.0;
    }
    *ptr = end;
    return value;
}

static double parse_factor(const char **ptr, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    skip_spaces(ptr);
    if (!**ptr) {
        *ok = false;
        return 0.0;
    }

    if (**ptr == '+' || **ptr == '-') {
        char sign = **ptr;
        (*ptr)++;
        double value = parse_factor(ptr, ok);
        return (sign == '-') ? -value : value;
    }

    double value = 0.0;
    if (**ptr == '(') {
        (*ptr)++;
        value = parse_expression(ptr, ok);
        skip_spaces(ptr);
        if (**ptr == ')') {
            (*ptr)++;
        } else {
            *ok = false;
        }
    } else {
        value = parse_number(ptr, ok);
    }
    return value;
}

static double parse_term(const char **ptr, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    double value = parse_factor(ptr, ok);
    while (*ok) {
        skip_spaces(ptr);
        char op = **ptr;
        if (op != '*' && op != '/') break;
        (*ptr)++;
        double rhs = parse_factor(ptr, ok);
        if (!*ok) break;
        if (op == '*') {
            value *= rhs;
        } else {
            if (rhs == 0.0) {
                *ok = false;
                return 0.0;
            }
            value /= rhs;
        }
    }
    return value;
}

static double parse_expression(const char **ptr, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    double value = parse_term(ptr, ok);
    while (*ok) {
        skip_spaces(ptr);
        char op = **ptr;
        if (op != '+' && op != '-') break;
        (*ptr)++;
        double rhs = parse_term(ptr, ok);
        if (!*ok) break;
        if (op == '+') {
            value += rhs;
        } else {
            value -= rhs;
        }
    }
    return value;
}

void formula_init(FormulaCtx *ctx)
{
    if (!ctx) return;
    ctx->len = 0;
    ctx->buffer[0] = '\0';
}

void formula_clear(FormulaCtx *ctx)
{
    if (!ctx) return;
    ctx->len = 0;
    ctx->buffer[0] = '\0';
}

const char *formula_text(const FormulaCtx *ctx)
{
    if (!ctx) return "";
    return ctx->buffer;
}

bool formula_is_empty(const FormulaCtx *ctx)
{
    if (!ctx) return true;
    return ctx->len == 0;
}

bool formula_append_char(FormulaCtx *ctx, char c)
{
    if (!ctx || ctx->len + 1 >= sizeof(ctx->buffer)) return false;
    ctx->buffer[ctx->len++] = c;
    ctx->buffer[ctx->len] = '\0';
    return true;
}

bool formula_append_str(FormulaCtx *ctx, const char *str)
{
    if (!ctx || !str) return false;
    while (*str) {
        if (!formula_append_char(ctx, *str++)) {
            return false;
        }
    }
    return true;
}

bool formula_backspace(FormulaCtx *ctx)
{
    if (!ctx || ctx->len == 0) return false;
    ctx->buffer[--ctx->len] = '\0';
    return true;
}

bool formula_evaluate(const FormulaCtx *ctx, double *out_value)
{
    if (!ctx || !out_value) return false;
    if (ctx->len == 0) return false;
    const char *ptr = ctx->buffer;
    bool ok = true;
    double value = parse_expression(&ptr, &ok);
    skip_spaces(&ptr);
    if (!ok || *ptr != '\0') return false;
    *out_value = value;
    return true;
}
