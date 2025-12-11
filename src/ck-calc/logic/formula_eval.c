#include "formula_eval.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "trig_mode.h"

static void skip_spaces(const char **ptr)
{
    if (!ptr || !*ptr) return;
    while (**ptr && isspace((unsigned char)**ptr)) {
        (*ptr)++;
    }
}

static double parse_expression(const char **ptr, TrigMode mode, bool *ok);
static double parse_term(const char **ptr, TrigMode mode, bool *ok);
static double parse_power(const char **ptr, TrigMode mode, bool *ok);

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

static bool read_identifier(const char **ptr, char *out, size_t out_size)
{
    if (!ptr || !*ptr || !out || out_size == 0) return false;
    skip_spaces(ptr);
    const char *p = *ptr;
    size_t len = 0;
    if (!isalpha((unsigned char)*p)) return false;
    while (*p && (isalpha((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) {
        if (len + 1 < out_size) {
            out[len++] = *p;
        }
        p++;
    }
    if (len == 0) return false;
    out[len] = '\0';
    *ptr = p;
    return true;
}

static double angle_to_radians(double value, TrigMode mode)
{
    switch (mode) {
        case TRIG_MODE_DEG:  return value * M_PI / 180.0;
        case TRIG_MODE_GRAD: return value * M_PI / 200.0;
        case TRIG_MODE_TURN: return value * 2.0 * M_PI;
        case TRIG_MODE_RAD:
        default:             return value;
    }
}

static double radians_to_mode(double value, TrigMode mode)
{
    switch (mode) {
        case TRIG_MODE_DEG:  return value * 180.0 / M_PI;
        case TRIG_MODE_GRAD: return value * 200.0 / M_PI;
        case TRIG_MODE_TURN: return value / (2.0 * M_PI);
        case TRIG_MODE_RAD:
        default:             return value;
    }
}

static double apply_function(const char *name, double arg, TrigMode mode, bool *ok)
{
    if (!name || !ok) return 0.0;
    double result = 0.0;
    if (strcmp(name, "sqrt") == 0) {
        if (arg < 0.0) { *ok = false; return 0.0; }
        result = sqrt(arg);
    } else if (strcmp(name, "cbrt") == 0) {
        result = cbrt(arg);
    } else if (strcmp(name, "ln") == 0) {
        if (arg <= 0.0) { *ok = false; return 0.0; }
        result = log(arg);
    } else if (strcmp(name, "log10") == 0 || strcmp(name, "log") == 0) {
        if (arg <= 0.0) { *ok = false; return 0.0; }
        result = log10(arg);
    } else if (strcmp(name, "log2") == 0) {
        if (arg <= 0.0) { *ok = false; return 0.0; }
        result = log(arg) / log(2.0);
    } else if (strcmp(name, "exp") == 0) {
        result = exp(arg);
    } else if (strcmp(name, "sin") == 0) {
        result = sin(angle_to_radians(arg, mode));
    } else if (strcmp(name, "cos") == 0) {
        result = cos(angle_to_radians(arg, mode));
    } else if (strcmp(name, "tan") == 0) {
        result = tan(angle_to_radians(arg, mode));
    } else if (strcmp(name, "asin") == 0) {
        if (arg < -1.0 || arg > 1.0) { *ok = false; return 0.0; }
        result = radians_to_mode(asin(arg), mode);
    } else if (strcmp(name, "acos") == 0) {
        if (arg < -1.0 || arg > 1.0) { *ok = false; return 0.0; }
        result = radians_to_mode(acos(arg), mode);
    } else if (strcmp(name, "atan") == 0) {
        result = radians_to_mode(atan(arg), mode);
    } else if (strcmp(name, "sinh") == 0) {
        result = sinh(angle_to_radians(arg, mode));
    } else if (strcmp(name, "cosh") == 0) {
        result = cosh(angle_to_radians(arg, mode));
    } else if (strcmp(name, "tanh") == 0) {
        result = tanh(angle_to_radians(arg, mode));
    } else if (strcmp(name, "asinh") == 0) {
        result = radians_to_mode(asinh(arg), mode);
    } else if (strcmp(name, "acosh") == 0) {
        if (arg < 1.0) { *ok = false; return 0.0; }
        result = radians_to_mode(acosh(arg), mode);
    } else if (strcmp(name, "atanh") == 0) {
        if (arg <= -1.0 || arg >= 1.0) { *ok = false; return 0.0; }
        result = radians_to_mode(atanh(arg), mode);
    } else if (strcmp(name, "inv") == 0) {
        if (arg == 0.0) { *ok = false; return 0.0; }
        result = 1.0 / arg;
    } else {
        *ok = false;
        return 0.0;
    }
    if (!isfinite(result)) {
        *ok = false;
        return 0.0;
    }
    return result;
}

static double apply_factorial(double value, bool *ok)
{
    if (!ok) return 0.0;
    if (value < 0.0) { *ok = false; return 0.0; }
    double rounded = floor(value + 0.5);
    if (fabs(value - rounded) > 1e-9) { *ok = false; return 0.0; }
    if (rounded > 20.0) { *ok = false; return 0.0; }
    double result = 1.0;
    for (int i = 2; i <= (int)rounded; ++i) {
        result *= (double)i;
    }
    return result;
}

static double parse_primary(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    skip_spaces(ptr);
    if (!**ptr) {
        *ok = false;
        return 0.0;
    }

    if (**ptr == '(') {
        (*ptr)++;
        double value = parse_expression(ptr, mode, ok);
        skip_spaces(ptr);
        if (**ptr == ')') {
            (*ptr)++;
        } else {
            *ok = false;
        }
        return value;
    }

    if (isalpha((unsigned char)**ptr)) {
        char ident[32];
        if (!read_identifier(ptr, ident, sizeof(ident))) {
            *ok = false;
            return 0.0;
        }
        skip_spaces(ptr);
        if (**ptr == '(') {
            (*ptr)++;
            double arg = parse_expression(ptr, mode, ok);
            skip_spaces(ptr);
            if (**ptr == ')') {
                (*ptr)++;
            } else {
                *ok = false;
            }
            if (!*ok) return 0.0;
            return apply_function(ident, arg, mode, ok);
        }

        if (strcmp(ident, "pi") == 0) {
            return 3.14159265358979323846;
        } else if (strcmp(ident, "e") == 0) {
            return 2.71828182845904523536;
        } else {
            *ok = false;
            return 0.0;
        }
    }

    return parse_number(ptr, ok);
}

static double parse_postfix(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !ok) return 0.0;
    double value = parse_primary(ptr, mode, ok);
    while (*ok) {
        skip_spaces(ptr);
        if (**ptr == '!') {
            (*ptr)++;
            value = apply_factorial(value, ok);
            if (!*ok) return 0.0;
            continue;
        }
        break;
    }
    return value;
}

static double parse_unary(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    skip_spaces(ptr);
    if (**ptr == '+' || **ptr == '-') {
        char sign = **ptr;
        (*ptr)++;
        double value = parse_unary(ptr, mode, ok);
        return (sign == '-') ? -value : value;
    }
    return parse_postfix(ptr, mode, ok);
}

static double safe_pow(double base, double exp, bool *ok)
{
    if (!ok) return 0.0;
    if (base == 0.0 && exp < 0.0) {
        *ok = false;
        return 0.0;
    }
    double result = pow(base, exp);
    if (!isfinite(result)) {
        *ok = false;
        return 0.0;
    }
    return result;
}

static double parse_power(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    double base = parse_unary(ptr, mode, ok);
    if (!*ok) return 0.0;
    skip_spaces(ptr);
    if (**ptr == '^') {
        (*ptr)++;
        double exponent = parse_power(ptr, mode, ok);
        if (!*ok) return 0.0;
        base = safe_pow(base, exponent, ok);
    }
    return base;
}

static double parse_term(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    double value = parse_power(ptr, mode, ok);
    while (*ok) {
        skip_spaces(ptr);
        char op = **ptr;
        if (op != '*' && op != '/') break;
        (*ptr)++;
        double rhs = parse_power(ptr, mode, ok);
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

static double parse_expression(const char **ptr, TrigMode mode, bool *ok)
{
    if (!ptr || !*ptr || !ok) return 0.0;
    double value = parse_term(ptr, mode, ok);
    while (*ok) {
        skip_spaces(ptr);
        char op = **ptr;
        if (op != '+' && op != '-') break;
        (*ptr)++;
        double rhs = parse_term(ptr, mode, ok);
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

bool formula_evaluate(const FormulaCtx *ctx, double *out_value, TrigMode mode)
{
    if (!ctx || !out_value) return false;
    if (ctx->len == 0) return false;
    const char *ptr = ctx->buffer;
    bool ok = true;
    double value = parse_expression(&ptr, mode, &ok);
    skip_spaces(&ptr);
    if (!ok || *ptr != '\0') return false;
    *out_value = value;
    return true;
}
