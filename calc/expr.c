/*
 * retro-calc expression engine: a recursive-descent parser over doubles.
 *
 *   assignment   name = expr
 *   bitwise      |  xor  &  <<  >>          (on whole numbers)
 *   arithmetic   +  -  *  /  %  mod         2pi and 3(4+1) multiply
 *   power        ^ or **  (right to left; -2^2 is -4)
 *   postfix      !  (factorial)
 *   numbers      1.5e-3  0xff  0b1010  0o17
 *   names        pi e tau phi ans mem, your variables, and the functions
 */
#include "expr.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif
#ifndef M_E
# define M_E 2.7182818284590452354
#endif

typedef struct Parser {
	const char *p;
	CalcState *st;
	char err[96];
	int depth;
} Parser;

static double p_expr(Parser *ps);
static double p_unary(Parser *ps);

static void
fail(Parser *ps, const char *msg)
{
	if (!ps->err[0])
		snprintf(ps->err, sizeof ps->err, "%s", msg);
}

static void
skip(Parser *ps)
{
	while (isspace((unsigned char)*ps->p))
		ps->p++;
}

static int
eat(Parser *ps, const char *tok)
{
	size_t n = strlen(tok);

	skip(ps);
	if (strncmp(ps->p, tok, n))
		return 0;
	ps->p += n;
	return 1;
}

/* a word operator ("mod", "xor") must not be the start of a longer name */
static int
eat_word(Parser *ps, const char *w)
{
	size_t n = strlen(w);

	skip(ps);
	if (strncmp(ps->p, w, n) || isalnum((unsigned char)ps->p[n]) ||
	    ps->p[n] == '_')
		return 0;
	ps->p += n;
	return 1;
}

/* ------------------------------------------------------------- functions */

static double
to_rad(const Parser *ps, double x)
{
	return ps->st->degrees ? x * M_PI / 180.0 : x;
}

static double
from_rad(const Parser *ps, double x)
{
	return ps->st->degrees ? x * 180.0 / M_PI : x;
}

static double
factorial(Parser *ps, double x)
{
	if (x < 0 || x != floor(x)) {
		if (x < 0 && x == floor(x)) {
			fail(ps, "factorial of a negative whole number");
			return 0;
		}
		return tgamma(x + 1.0);            /* 0.5! and friends */
	}
	if (x > 170) {
		fail(ps, "too large");
		return 0;
	}
	return floor(tgamma(x + 1.0) + 0.5);
}

static double
gcd(double a, double b)
{
	a = fabs(floor(a));
	b = fabs(floor(b));
	while (b > 0) {
		double t = fmod(a, b);

		a = b;
		b = t;
	}
	return a;
}

static double
ncr(Parser *ps, double n, double r)
{
	double k, v = 1;

	if (n < 0 || r < 0 || n != floor(n) || r != floor(r)) {
		fail(ps, "ncr needs whole numbers >= 0");
		return 0;
	}
	if (r > n)
		return 0;
	if (r > n - r)
		r = n - r;
	for (k = 1; k <= r; k++)
		v = v * (n - r + k) / k;
	return floor(v + 0.5);
}

static double
whole(Parser *ps, double x)
{
	if (x != floor(x) || fabs(x) > 9007199254740992.0) {
		fail(ps, "bit operations need whole numbers");
		return 0;
	}
	return x;
}

enum {
	F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH,
	F_ASINH, F_ACOSH, F_ATANH, F_EXP, F_LN, F_LOG, F_LOG2, F_SQRT, F_CBRT,
	F_ABS, F_FLOOR, F_CEIL, F_ROUND, F_TRUNC, F_SIGN, F_FACT, F_GAMMA,
	F_DEG, F_RAD, F_FRAC, F_NOT,
	F_TWO,                             /* two arguments from here on */
	F_POW = F_TWO, F_ATAN2, F_HYPOT, F_MOD, F_MIN, F_MAX, F_GCD, F_LCM,
	F_NCR, F_NPR, F_ROOT, F_LOGB
};

const char *const calc_functions[] = {
	"sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
	"asinh", "acosh", "atanh", "exp", "ln", "log", "log2", "sqrt", "cbrt",
	"abs", "floor", "ceil", "round", "trunc", "sign", "fact", "gamma",
	"deg", "rad", "frac", "not",
	"pow", "atan2", "hypot", "mod", "min", "max", "gcd", "lcm", "ncr",
	"npr", "root", "logb", NULL
};

static double
call(Parser *ps, int f, double a, double b)
{
	switch (f) {
	case F_SIN: return sin(to_rad(ps, a));
	case F_COS: return cos(to_rad(ps, a));
	case F_TAN: return tan(to_rad(ps, a));
	case F_ASIN: return from_rad(ps, asin(a));
	case F_ACOS: return from_rad(ps, acos(a));
	case F_ATAN: return from_rad(ps, atan(a));
	case F_SINH: return sinh(a);
	case F_COSH: return cosh(a);
	case F_TANH: return tanh(a);
	case F_ASINH: return asinh(a);
	case F_ACOSH: return acosh(a);
	case F_ATANH: return atanh(a);
	case F_EXP: return exp(a);
	case F_LN: return log(a);
	case F_LOG: return log10(a);
	case F_LOG2: return log2(a);
	case F_SQRT: return sqrt(a);
	case F_CBRT: return cbrt(a);
	case F_ABS: return fabs(a);
	case F_FLOOR: return floor(a);
	case F_CEIL: return ceil(a);
	case F_ROUND: return round(a);
	case F_TRUNC: return trunc(a);
	case F_SIGN: return a > 0 ? 1 : a < 0 ? -1 : 0;
	case F_FACT: return factorial(ps, a);
	case F_GAMMA: return tgamma(a);
	case F_DEG: return a * 180.0 / M_PI;
	case F_RAD: return a * M_PI / 180.0;
	case F_FRAC: return a - trunc(a);
	case F_NOT: return (double)~(long long)whole(ps, a);
	case F_POW: return pow(a, b);
	case F_ATAN2: return from_rad(ps, atan2(a, b));
	case F_HYPOT: return hypot(a, b);
	case F_MOD:
		if (b == 0) {
			fail(ps, "division by zero");
			return 0;
		}
		return fmod(a, b);
	case F_MIN: return a < b ? a : b;
	case F_MAX: return a > b ? a : b;
	case F_GCD: return gcd(a, b);
	case F_LCM: return gcd(a, b) == 0 ? 0 : fabs(floor(a) * floor(b)) /
	    gcd(a, b);
	case F_NCR: return ncr(ps, a, b);
	case F_NPR: return ncr(ps, a, b) * factorial(ps, b);
	case F_ROOT:
		/* odd roots of negative numbers are real */
		if (a < 0 && b == floor(b) && fmod(fabs(b), 2.0) == 1.0)
			return -pow(-a, 1.0 / b);
		return pow(a, 1.0 / b);
	case F_LOGB: return log(a) / log(b);
	default: return 0;
	}
}

/* ----------------------------------------------------------------- parser */

static double
p_number(Parser *ps)
{
	const char *s = ps->p;
	char *end;
	double v;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' ||
	    s[1] == 'B' || s[1] == 'o' || s[1] == 'O')) {
		int base = tolower((unsigned char)s[1]) == 'x' ? 16 :
		    tolower((unsigned char)s[1]) == 'b' ? 2 : 8;
		unsigned long long u = strtoull(s + 2, &end, base);

		if (end == s + 2) {
			fail(ps, "digits expected after the base prefix");
			return 0;
		}
		ps->p = end;
		return (double)u;
	}
	v = strtod(s, &end);
	if (end == s) {
		fail(ps, "number expected");
		return 0;
	}
	ps->p = end;
	return v;
}

static double *
var_find(CalcState *st, const char *name)
{
	int i;

	for (i = 0; i < st->nvar; i++)
		if (!strcmp(st->var[i].name, name))
			return &st->var[i].val;
	return NULL;
}

static double
p_name(Parser *ps)
{
	char name[16];
	size_t n = 0;
	double *v;
	int i;

	while (isalnum((unsigned char)*ps->p) || *ps->p == '_') {
		if (n + 1 < sizeof name)
			name[n++] = *ps->p;
		ps->p++;
	}
	name[n] = '\0';
	skip(ps);
	if (*ps->p == '(') {
		double a, b = 0;

		for (i = 0; calc_functions[i]; i++)
			if (!strcmp(calc_functions[i], name))
				break;
		if (calc_functions[i]) {
			ps->p++;
			a = p_expr(ps);
			if (i >= F_TWO) {
				if (!eat(ps, ",")) {
					fail(ps, "this function takes two arguments");
					return 0;
				}
				b = p_expr(ps);
			}
			if (!eat(ps, ")"))
				fail(ps, "')' expected");
			return call(ps, i, a, b);
		}
	}
	if ((v = var_find(ps->st, name)))      /* your own names come first */
		return *v;
	if (!strcmp(name, "pi"))
		return M_PI;
	if (!strcmp(name, "e"))
		return M_E;
	if (!strcmp(name, "tau"))
		return 2 * M_PI;
	if (!strcmp(name, "phi"))
		return 1.6180339887498948482;
	if (!strcmp(name, "ans"))
		return ps->st->ans;
	if (!strcmp(name, "mem"))
		return ps->st->mem;
	for (i = 0; calc_functions[i]; i++) {
		if (!strcmp(calc_functions[i], name)) {
			fail(ps, "'(' expected after a function");
			return 0;
		}
	}
	snprintf(ps->err, sizeof ps->err, "unknown name '%s'", name);
	return 0;
}

static double
p_primary(Parser *ps)
{
	double v;

	skip(ps);
	if (*ps->p == '(') {
		ps->p++;
		if (++ps->depth > 200) {
			fail(ps, "nested too deeply");
			return 0;
		}
		v = p_expr(ps);
		ps->depth--;
		if (!eat(ps, ")"))
			fail(ps, "')' expected");
		return v;
	}
	if (isdigit((unsigned char)*ps->p) || *ps->p == '.')
		return p_number(ps);
	if (isalpha((unsigned char)*ps->p) || *ps->p == '_')
		return p_name(ps);
	fail(ps, *ps->p ? "unexpected character" : "the expression is incomplete");
	return 0;
}

static double
p_postfix(Parser *ps)
{
	double v = p_primary(ps);

	for (;;) {
		skip(ps);
		if (*ps->p == '!' && ps->p[1] != '=') {
			ps->p++;
			v = factorial(ps, v);
		} else {
			return v;
		}
	}
}

static double
p_power(Parser *ps)
{
	double base = p_postfix(ps);

	if (eat(ps, "**") || eat(ps, "^"))
		return pow(base, p_unary(ps));     /* right to left */
	return base;
}

static double
p_unary(Parser *ps)
{
	skip(ps);
	if (++ps->depth > 200) {
		fail(ps, "nested too deeply");
		return 0;
	}
	if (eat(ps, "-")) {
		double v = -p_unary(ps);

		ps->depth--;
		return v;
	}
	if (eat(ps, "+")) {
		double v = p_unary(ps);

		ps->depth--;
		return v;
	}
	if (eat(ps, "~")) {
		double v = (double)~(long long)whole(ps, p_unary(ps));

		ps->depth--;
		return v;
	}
	ps->depth--;
	return p_power(ps);
}

static double
p_term(Parser *ps)
{
	double v = p_unary(ps);

	for (;;) {
		skip(ps);
		if (*ps->p == '*' && ps->p[1] != '*') {
			ps->p++;
			v *= p_unary(ps);
		} else if (*ps->p == '/') {
			double d;

			ps->p++;
			d = p_unary(ps);
			if (d == 0) {
				fail(ps, "division by zero");
				return 0;
			}
			v /= d;
		} else if (*ps->p == '%' || eat_word(ps, "mod")) {
			double d;

			if (*ps->p == '%')
				ps->p++;
			d = p_unary(ps);
			if (d == 0) {
				fail(ps, "division by zero");
				return 0;
			}
			v = fmod(v, d);
		} else if (*ps->p == '(' || isalpha((unsigned char)*ps->p) ||
		    *ps->p == '_') {
			/* 2pi, 3(4+1), 2sin(x): multiplication without a sign -
			 * but "xor" and "mod" are operators, not names */
			if (!strncmp(ps->p, "xor", 3) &&
			    !isalnum((unsigned char)ps->p[3]))
				return v;
			v *= p_power(ps);
		} else {
			return v;
		}
		if (ps->err[0])
			return 0;
	}
}

static double
p_sum(Parser *ps)
{
	double v = p_term(ps);

	for (;;) {
		if (ps->err[0])
			return 0;
		if (eat(ps, "+"))
			v += p_term(ps);
		else if (eat(ps, "-"))
			v -= p_term(ps);
		else
			return v;
	}
}

static double
p_shift(Parser *ps)
{
	double v = p_sum(ps);

	for (;;) {
		if (ps->err[0])
			return 0;
		if (eat(ps, "<<")) {
			double n = whole(ps, p_sum(ps));

			v = whole(ps, v) * pow(2.0, n);
		} else if (eat(ps, ">>")) {
			double n = whole(ps, p_sum(ps));

			v = floor(whole(ps, v) / pow(2.0, n));
		} else {
			return v;
		}
	}
}

static double
p_and(Parser *ps)
{
	double v = p_shift(ps);

	while (!ps->err[0] && eat(ps, "&"))
		v = (double)((long long)whole(ps, v) &
		    (long long)whole(ps, p_shift(ps)));
	return v;
}

static double
p_xor(Parser *ps)
{
	double v = p_and(ps);

	while (!ps->err[0] && eat_word(ps, "xor"))
		v = (double)((long long)whole(ps, v) ^
		    (long long)whole(ps, p_and(ps)));
	return v;
}

static double
p_expr(Parser *ps)
{
	double v = p_xor(ps);

	while (!ps->err[0] && eat(ps, "|"))
		v = (double)((long long)whole(ps, v) |
		    (long long)whole(ps, p_xor(ps)));
	return v;
}

int
calc_eval(CalcState *st, const char *expr, double *out, char *err, size_t n)
{
	Parser ps;
	char name[16] = "";
	const char *s = expr, *eq;
	double v;
	size_t len = 0;
	int i;

	memset(&ps, 0, sizeof ps);
	ps.st = st;
	while (isspace((unsigned char)*s))
		s++;
	/* "name = expr" */
	if (isalpha((unsigned char)*s) || *s == '_') {
		const char *q = s;

		while (isalnum((unsigned char)*q) || *q == '_')
			q++;
		len = (size_t)(q - s);
		eq = q;
		while (isspace((unsigned char)*eq))
			eq++;
		if (*eq == '=' && eq[1] != '=' && len < sizeof name) {
			memcpy(name, s, len);
			name[len] = '\0';
			s = eq + 1;
		}
	}
	ps.p = s;
	v = p_expr(&ps);
	skip(&ps);
	if (!ps.err[0] && *ps.p)
		fail(&ps, *ps.p == ')' ? "')' without '('" : "unexpected character");
	if (!ps.err[0] && isnan(v))
		fail(&ps, "not a real number");
	if (!ps.err[0] && isinf(v))
		fail(&ps, "too large (overflow)");
	if (!ps.err[0] && name[0]) {
		static const char *const reserved[] = {
			"pi", "e", "tau", "phi", "ans", "mem", "mod", "xor", NULL
		};
		double *slot = var_find(st, name);

		for (i = 0; reserved[i]; i++)
			if (!strcmp(reserved[i], name))
				fail(&ps, "that name is taken");
		for (i = 0; calc_functions[i]; i++)
			if (!strcmp(calc_functions[i], name))
				fail(&ps, "that name is a function");
		if (!ps.err[0] && !slot) {
			if (st->nvar == CALC_VARS) {
				fail(&ps, "too many variables");
			} else {
				snprintf(st->var[st->nvar].name,
				    sizeof st->var[st->nvar].name, "%s", name);
				slot = &st->var[st->nvar++].val;
			}
		}
		if (!ps.err[0] && slot)
			*slot = v;
	}
	if (ps.err[0]) {
		snprintf(err, n, "%s", ps.err);
		return -1;
	}
	if (v == 0)
		v = 0;                             /* no "-0" */
	st->ans = v;
	*out = v;
	if (n)
		err[0] = '\0';
	return 0;
}

void
calc_format(double v, int base, char *buf, size_t n)
{
	static const char digits[] = "0123456789ABCDEF";
	char tmp[80];
	unsigned long long u;
	int i = 0, neg = v < 0;

	if (base == 10 || v != floor(v) || fabs(v) >= 18446744073709551615.0) {
		/* 12 significant digits hide binary rounding (0.1 + 0.2) */
		snprintf(buf, n, "%.12g", v);
		return;
	}
	u = (unsigned long long)fabs(v);
	do {
		tmp[i++] = digits[u % (unsigned)base];
		u /= (unsigned)base;
	} while (u && i < 70);
	snprintf(buf, n, "%s%s", neg ? "-" : "", base == 16 ? "0x" : base == 2 ?
	    "0b" : "0o");
	while (i > 0 && strlen(buf) + 2 < n) {
		size_t l = strlen(buf);

		buf[l] = tmp[--i];
		buf[l + 1] = '\0';
	}
}
