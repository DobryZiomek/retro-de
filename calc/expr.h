/* retro-calc expression engine: no X, so the unit tests can drive it. */
#ifndef RETRO_EXPR_H
#define RETRO_EXPR_H

#include <stddef.h>

#define CALC_VARS 32

typedef struct CalcState {
	int degrees;                       /* trigonometry in degrees         */
	double ans;                        /* the last result                 */
	double mem;                        /* M+ / M- / MR                    */
	struct {
		char name[16];
		double val;
	} var[CALC_VARS];
	int nvar;
} CalcState;

/* Evaluates one expression ("x = 3", "2sin(pi/4)^2", "0xff & 0b1010",
 * "5!", "ncr(52, 5)").  0 and *out on success (which also becomes `ans`);
 * -1 and a message in err otherwise. */
int  calc_eval(CalcState *st, const char *expr, double *out, char *err,
               size_t n);
/* base 10, 16, 2 or 8; other bases show whole numbers only */
void calc_format(double v, int base, char *buf, size_t n);
/* names for completion and the help text; NULL-terminated */
extern const char *const calc_functions[];

#endif
