/*
 * Shared building blocks for the distortion plugins:
 *
 * - Linkwitz-Riley 4 crossover: splits the bass into a clean low band and a
 *   high band that goes to the distortion. LR4 low + LR4 high sum to an
 *   allpass, so with no distortion the result is magnitude-flat.
 * - 4x linear-phase oversampler (windowed-sinc FIR, polyphase). Running the
 *   nonlinearity at 4x keeps aliasing (harsh digital fizz) out of the audio.
 *   Its fixed delay, SPLIT_OS_LATENCY samples, must be applied to the clean
 *   path so both paths line up.
 *
 * Header-only, no allocation, real-time safe.
 */
#ifndef PLUGINS_LV2_BASS_SPLIT_H
#define PLUGINS_LV2_BASS_SPLIT_H

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- biquad */

typedef struct {
	float b0, b1, b2, a1, a2;
} SplitBq;

typedef struct {
	float z1, z2;
} SplitBqState;

/* RBJ cookbook. type: 0 low-pass, 1 high-pass, 2 peaking (gain_db used). */
static inline SplitBq split_bq_design(int type, double sr, double fc, double q, double gain_db)
{
	if (fc > sr * 0.45) {
		fc = sr * 0.45;
	}
	const double w0 = 2.0 * M_PI * fc / sr;
	const double cw = cos(w0), alpha = sin(w0) / (2.0 * q);
	double b0, b1, b2, a0, a1, a2;
	if (type == 2) {
		const double A = pow(10.0, gain_db / 40.0);
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cw;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cw;
		a2 = 1.0 - alpha / A;
	} else {
		a0 = 1.0 + alpha;
		a1 = -2.0 * cw;
		a2 = 1.0 - alpha;
		if (type == 0) {
			b0 = (1.0 - cw) / 2.0;
			b1 = 1.0 - cw;
		} else {
			b0 = (1.0 + cw) / 2.0;
			b1 = -(1.0 + cw);
		}
		b2 = b0;
	}
	SplitBq c = {(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(a1 / a0), (float)(a2 / a0)};
	return c;
}

static inline float split_bq_run(const SplitBq* c, SplitBqState* s, float x)
{
	const float y = c->b0 * x + s->z1;
	s->z1 = c->b1 * x - c->a1 * y + s->z2;
	s->z2 = c->b2 * x - c->a2 * y;
	return y;
}

static inline void split_bq_flush(SplitBqState* s)
{
	if (fabsf(s->z1) < 1e-20f) s->z1 = 0.0f;
	if (fabsf(s->z2) < 1e-20f) s->z2 = 0.0f;
}

/* ------------------------------------------------------ LR4 crossover */

typedef struct {
	SplitBq lp, hp;
} SplitXover;

typedef struct {
	SplitBqState lp[2], hp[2];
} SplitXoverState;

static inline void split_xover_design(SplitXover* x, double sr, double fc)
{
	x->lp = split_bq_design(0, sr, fc, M_SQRT1_2, 0.0);
	x->hp = split_bq_design(1, sr, fc, M_SQRT1_2, 0.0);
}

static inline void split_xover_run(const SplitXover* x, SplitXoverState* s, float in,
                                   float* low, float* high)
{
	*low = split_bq_run(&x->lp, &s->lp[1], split_bq_run(&x->lp, &s->lp[0], in));
	*high = split_bq_run(&x->hp, &s->hp[1], split_bq_run(&x->hp, &s->hp[0], in));
}

static inline void split_xover_flush(SplitXoverState* s)
{
	for (int i = 0; i < 2; ++i) {
		split_bq_flush(&s->lp[i]);
		split_bq_flush(&s->hp[i]);
	}
}

/* ------------------------------------------------- 4x oversampler */

#define SPLIT_OS 4
#define SPLIT_OS_TAPS 65                     /* odd: integer group delay */
#define SPLIT_OS_UP_HIST ((SPLIT_OS_TAPS + SPLIT_OS - 1) / SPLIT_OS)
#define SPLIT_OS_DOWN_HIST (SPLIT_OS_TAPS + SPLIT_OS - 1)
/* (TAPS-1)/2 at 4x for the up filter + the same for the down filter */
#define SPLIT_OS_LATENCY ((SPLIT_OS_TAPS - 1) / SPLIT_OS)

typedef struct {
	float h[SPLIT_OS_TAPS];
} SplitOs;

typedef struct {
	float up[SPLIT_OS_UP_HIST];     /* base-rate input history, [0] newest */
	float down[SPLIT_OS_DOWN_HIST]; /* 4x-rate history, [0] newest */
} SplitOsState;

/* Blackman-windowed sinc, cutoff 12 kHz at a 48 kHz base rate (0.0625 of the
 * 4x rate). Flat to ~8 kHz, stopband well below -70 dB from 24 kHz on: plenty
 * for a bass distortion whose tone control tops out at 8 kHz. */
static inline void split_os_design(SplitOs* os)
{
	const double fc = 0.0625;
	const int m = SPLIT_OS_TAPS - 1;
	double sum = 0.0;
	for (int i = 0; i <= m; ++i) {
		const double t = i - m / 2.0;
		const double sinc = t == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * t) / (M_PI * t);
		const double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / m) + 0.08 * cos(4.0 * M_PI * i / m);
		os->h[i] = (float)(sinc * w);
		sum += os->h[i];
	}
	for (int i = 0; i <= m; ++i) {
		os->h[i] = (float)(os->h[i] / sum);
	}
}

/* one base-rate sample in, SPLIT_OS samples out (oldest first) */
static inline void split_os_up(const SplitOs* os, SplitOsState* s, float x, float out[SPLIT_OS])
{
	memmove(s->up + 1, s->up, sizeof(float) * (SPLIT_OS_UP_HIST - 1));
	s->up[0] = x;
	for (int p = 0; p < SPLIT_OS; ++p) {
		float acc = 0.0f;
		for (int k = 0; SPLIT_OS * k + p < SPLIT_OS_TAPS; ++k) {
			acc += os->h[SPLIT_OS * k + p] * s->up[k];
		}
		out[p] = acc * (float)SPLIT_OS;
	}
}

/* SPLIT_OS samples in (oldest first), one base-rate sample out */
static inline float split_os_down(const SplitOs* os, SplitOsState* s, const float in[SPLIT_OS])
{
	memmove(s->down + SPLIT_OS, s->down, sizeof(float) * (SPLIT_OS_DOWN_HIST - SPLIT_OS));
	for (int p = 0; p < SPLIT_OS; ++p) {
		s->down[SPLIT_OS - 1 - p] = in[p];
	}
	/* evaluate at the first of the 4 new samples so the total delay is an
	 * integer number of base-rate samples */
	float acc = 0.0f;
	for (int j = 0; j < SPLIT_OS_TAPS; ++j) {
		acc += os->h[j] * s->down[j + SPLIT_OS - 1];
	}
	return acc;
}

/* ------------------------------------------- clean path delay line */

typedef struct {
	float buf[SPLIT_OS_LATENCY + 1];
	int pos;
} SplitDelay;

static inline float split_delay_run(SplitDelay* d, float x)
{
	d->buf[d->pos] = x;
	d->pos = (d->pos + 1) % (SPLIT_OS_LATENCY + 1);
	return d->buf[d->pos]; /* oldest = written SPLIT_OS_LATENCY samples ago */
}

#endif
