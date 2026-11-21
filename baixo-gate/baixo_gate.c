/*
 * Baixo Gate: noise gate + multiband hiss reduction for bass guitar.
 *
 * Gate
 * - Detector only looks at 40 Hz..1.2 kHz (where bass notes live), so hiss
 *   cannot hold the gate open or make it chatter.
 * - Hysteresis + hold: opens at `threshold`, closes only after the level stays
 *   below `threshold - hysteresis` for `hold` ms.
 * - Gain moves in linear dB ramps (attack/release) down to `range`.
 *
 * Hiss reduction while playing (multiband)
 * - Linkwitz-Riley 4 crossover tree, half-octave bands from 300 Hz, with
 *   allpass compensation: with all band gains at 1 the bands sum back to a
 *   magnitude-flat (allpassed) copy of the input.
 * - Band 0 (< 300 Hz, the bass fundamental) is never touched.
 * - Each upper band gets a Wiener-style gain g = sqrt(1 - strength * N / E),
 *   floored at `nr_reduction`, where E is the band power and N the band noise
 *   power. Where the note has energy the band passes; where only hiss is left
 *   the band is turned down.
 * - N is learned automatically while the gate is fully closed (you are not
 *   playing), starting from values measured on the HD-II.
 *
 * Extra: dynamic low-pass on the note tail (`hiss`, `hiss_freq`), and 1 ms
 * lookahead so neither the gate nor the bands smear the attack.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#define BG_URI "urn:zegkreist:lv2:baixo-gate"

#define LOOKAHEAD_MS 1.0
#define MAX_LOOKAHEAD 1024
#define COEF_EVERY 16
#define DETECT_HP_HZ 40.0
#define DETECT_LP_HZ 1200.0
#define ENV_RELEASE_MS 60.0
#define HISS_SPAN_DB 24.0
#define HISS_OPEN_MS 2.0
#define HISS_CLOSE_MS 120.0
#define HISS_MAX_HZ 18000.0

#define NX 12          /* crossovers */
#define NB (NX + 1)    /* bands */
/* Per-band timing: high bands react and close fast so a string attack does
 * not drag hiss along for ~200 ms; low bands are slower because their power
 * fluctuates more on pure noise (fewer cycles per ms) and would "bubble". */
#define NR_G_UP_MS 1.0
#define NR_G_DOWN_FAST_MS 15.0 /* bands >= 1200 Hz */
#define NR_G_DOWN_SLOW_MS 30.0 /* 300..1200 Hz */
#define NR_LEARN_MS 500.0

/* half-octave bands: a harmonic only opens its own narrow band, so the hiss
 * next to it stays down */
static const double XOVER_HZ[NX] = {300.0, 425.0, 600.0, 850.0, 1200.0, 1700.0,
                                    2400.0, 3400.0, 4800.0, 6800.0, 9600.0, 13600.0};

/* Initial band noise power (mean square, linear), measured on the HD-II with
 * Mic gain at 100% (2026-11-21). Replaced by the learned value at the first
 * pause. Band 0 is unused. */
static const float NOISE_INIT[NB] = {
	0.0f, 4.70e-09f, 4.64e-09f, 4.75e-09f, 5.99e-09f, 7.82e-09f, 9.39e-09f,
	1.15e-08f, 1.36e-08f, 1.62e-08f, 1.80e-08f, 2.07e-08f, 6.46e-08f};

typedef enum {
	P_IN_L = 0,
	P_IN_R,
	P_OUT_L,
	P_OUT_R,
	P_THRESHOLD,
	P_HYSTERESIS,
	P_ATTACK,
	P_HOLD,
	P_RELEASE,
	P_RANGE,
	P_HISS,
	P_HISS_FREQ,
	P_LEVEL,
	P_GAIN,
	P_LATENCY,
	P_NR_REDUCTION,
	P_NR_STRENGTH,
	P_NR_METER,
	P_COUNT
} PortIndex;

typedef struct {
	float b0, b1, b2, a1, a2;
} BqC;

typedef struct {
	float z1, z2;
} BqS;

typedef struct {
	float ic1, ic2;
} Svf;

typedef struct {
	BqS lp[NX][2], hp[NX][2];
	BqS ap[NB][NX]; /* ap[band][crossover] */
	float band_delay[NB][MAX_LOOKAHEAD];
	float in_delay[MAX_LOOKAHEAD];
} Channel;

typedef struct {
	void* port[P_COUNT];
	double sr;

	/* gate detector */
	float hp_a, lp_a;
	float hp_x1[2], hp_y1[2], lp1[2], lp2[2];
	float env, env_dec;
	int open;
	uint32_t hold_left;
	float gain_db;

	/* tail low-pass */
	Svf svf[2];
	float hiss_t, hiss_up, hiss_down;
	float g, a1, a2, a3;
	uint32_t coef_count;

	/* multiband */
	BqC lpc[NX], hpc[NX], apc[NX];
	Channel ch[2];
	float band_e[NB];
	float band_n[NB];
	float band_g[NB];
	float e_att[NB], e_rel[NB], g_down[NB], g_up, learn;

	uint32_t la, widx;
} BaixoGate;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float to_db(float v)
{
	return v > 1e-9f ? 20.0f * log10f(v) : -180.0f;
}

static float onepole_coef(double sr, double hz)
{
	return (float)exp(-2.0 * M_PI * hz / sr);
}

static float smooth_coef(double sr, double ms)
{
	return (float)(1.0 - exp(-1.0 / (ms * 0.001 * sr)));
}

/* RBJ cookbook biquads (all share the same bilinear mapping, so
 * LR4 LP + LR4 HP == AP exactly in the digital domain too). */
static BqC biquad(int type, double sr, double fc, double q)
{
	const double w0 = 2.0 * M_PI * fc / sr;
	const double cw = cos(w0), alpha = sin(w0) / (2.0 * q);
	double b0, b1, b2;
	const double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
	if (type == 0) { /* low-pass */
		b0 = (1.0 - cw) / 2.0;
		b1 = 1.0 - cw;
		b2 = b0;
	} else if (type == 1) { /* high-pass */
		b0 = (1.0 + cw) / 2.0;
		b1 = -(1.0 + cw);
		b2 = b0;
	} else { /* all-pass */
		b0 = 1.0 - alpha;
		b1 = -2.0 * cw;
		b2 = 1.0 + alpha;
	}
	BqC c = {(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(a1 / a0), (float)(a2 / a0)};
	return c;
}

static inline float bq(const BqC* c, BqS* s, float x)
{
	const float y = c->b0 * x + s->z1;
	s->z1 = c->b1 * x - c->a1 * y + s->z2;
	s->z2 = c->b2 * x - c->a2 * y;
	return y;
}

static void flush_bq(BqS* p, int count)
{
	for (int i = 0; i < count; ++i) {
		if (fabsf(p[i].z1) < 1e-20f) p[i].z1 = 0.0f;
		if (fabsf(p[i].z2) < 1e-20f) p[i].z2 = 0.0f;
	}
}

static void svf_coefs(BaixoGate* s, float fc)
{
	const float k = 1.41421356f;
	fc = clampf(fc, 20.0f, (float)(s->sr * 0.45));
	s->g = tanf((float)M_PI * fc / (float)s->sr);
	s->a1 = 1.0f / (1.0f + s->g * (s->g + k));
	s->a2 = s->g * s->a1;
	s->a3 = s->g * s->a2;
}

static float svf_lp(BaixoGate* s, Svf* f, float v0)
{
	const float v3 = v0 - f->ic2;
	const float v1 = s->a1 * f->ic1 + s->a2 * v3;
	const float v2 = f->ic2 + s->a2 * f->ic1 + s->a3 * v3;
	f->ic1 = 2.0f * v1 - f->ic1;
	f->ic2 = 2.0f * v2 - f->ic2;
	return v2;
}

/* Split x into NB magnitude-complementary bands. */
static void split(const BaixoGate* s, Channel* c, float x, float* band)
{
	float rest = x;
	for (int k = 0; k < NX; ++k) {
		band[k] = bq(&s->lpc[k], &c->lp[k][1], bq(&s->lpc[k], &c->lp[k][0], rest));
		rest = bq(&s->hpc[k], &c->hp[k][1], bq(&s->hpc[k], &c->hp[k][0], rest));
	}
	band[NX] = rest;
	for (int k = 0; k < NX - 1; ++k) {
		for (int j = k + 1; j < NX; ++j) {
			band[k] = bq(&s->apc[j], &c->ap[k][j], band[k]);
		}
	}
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
                              const char* path, const LV2_Feature* const* f)
{
	(void)d;
	(void)path;
	(void)f;
	BaixoGate* s = (BaixoGate*)calloc(1, sizeof(BaixoGate));
	if (!s) {
		return NULL;
	}
	s->sr = rate;
	s->hp_a = onepole_coef(rate, DETECT_HP_HZ);
	s->lp_a = onepole_coef(rate, DETECT_LP_HZ);
	s->env_dec = (float)exp(-1.0 / (ENV_RELEASE_MS * 0.001 * rate));
	s->hiss_up = smooth_coef(rate, HISS_OPEN_MS);
	s->hiss_down = smooth_coef(rate, HISS_CLOSE_MS);
	s->g_up = smooth_coef(rate, NR_G_UP_MS);
	for (int b = 1; b < NB; ++b) {
		const double f_lo = XOVER_HZ[b - 1];
		/* symmetric averaging = unbiased mean power. A fast-attack peak
		 * follower overestimates noise power ~3-5x and lets hiss through. */
		double avg_ms = 4000.0 / f_lo; /* ~4 cycles of the band's lowest freq */
		avg_ms = avg_ms < 3.0 ? 3.0 : (avg_ms > 15.0 ? 15.0 : avg_ms);
		s->e_att[b] = smooth_coef(rate, avg_ms);
		s->e_rel[b] = s->e_att[b];
		s->g_down[b] = smooth_coef(rate, f_lo >= 1200.0 ? NR_G_DOWN_FAST_MS : NR_G_DOWN_SLOW_MS);
	}
	s->learn = smooth_coef(rate, NR_LEARN_MS);
	for (int k = 0; k < NX; ++k) {
		const double fc = XOVER_HZ[k] < rate * 0.45 ? XOVER_HZ[k] : rate * 0.45;
		s->lpc[k] = biquad(0, rate, fc, M_SQRT1_2);
		s->hpc[k] = biquad(1, rate, fc, M_SQRT1_2);
		s->apc[k] = biquad(2, rate, fc, M_SQRT1_2);
	}
	s->la = (uint32_t)lround(LOOKAHEAD_MS * 0.001 * rate);
	if (s->la >= MAX_LOOKAHEAD) {
		s->la = MAX_LOOKAHEAD - 1;
	}
	/* noise init values were measured at 48 kHz; band powers of white-ish
	 * noise scale ~1/sr per Hz but bands are fixed in Hz, so they hold. */
	memcpy(s->band_n, NOISE_INIT, sizeof(s->band_n));
	return (LV2_Handle)s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	BaixoGate* s = (BaixoGate*)h;
	if (port < P_COUNT) {
		s->port[port] = data;
	}
}

static void activate(LV2_Handle h)
{
	BaixoGate* s = (BaixoGate*)h;
	memset(s->hp_x1, 0, sizeof(s->hp_x1));
	memset(s->hp_y1, 0, sizeof(s->hp_y1));
	memset(s->lp1, 0, sizeof(s->lp1));
	memset(s->lp2, 0, sizeof(s->lp2));
	memset(s->svf, 0, sizeof(s->svf));
	memset(s->ch, 0, sizeof(s->ch));
	memset(s->band_e, 0, sizeof(s->band_e));
	for (int b = 0; b < NB; ++b) {
		s->band_g[b] = 1.0f;
	}
	s->env = 0.0f;
	s->open = 0;
	s->hold_left = 0;
	s->gain_db = -90.0f;
	s->hiss_t = 0.0f;
	s->coef_count = 0;
	s->widx = 0;
}

#define CTRL(i) (*(const float*)s->port[i])
#define CTRL_OR(i, def) (s->port[i] ? *(const float*)s->port[i] : (def))

static void run(LV2_Handle h, uint32_t n)
{
	BaixoGate* s = (BaixoGate*)h;
	const float* in[2] = {(const float*)s->port[P_IN_L], (const float*)s->port[P_IN_R]};
	float* out[2] = {(float*)s->port[P_OUT_L], (float*)s->port[P_OUT_R]};

	const float thr = clampf(CTRL(P_THRESHOLD), -100.0f, 0.0f);
	const float close_thr = thr - clampf(CTRL(P_HYSTERESIS), 0.0f, 40.0f);
	const float range = clampf(CTRL(P_RANGE), -90.0f, 0.0f);
	const float span = range < -1.0f ? -range : 1.0f;
	const float att_rate = span / (float)(clampf(CTRL(P_ATTACK), 0.05f, 100.0f) * 0.001 * s->sr);
	const float rel_rate = span / (float)(clampf(CTRL(P_RELEASE), 1.0f, 5000.0f) * 0.001 * s->sr);
	const uint32_t hold = (uint32_t)(clampf(CTRL(P_HOLD), 0.0f, 2000.0f) * 0.001 * s->sr);
	const float hiss_amt = clampf(CTRL(P_HISS), 0.0f, 1.0f);
	const float hiss_min = clampf(CTRL(P_HISS_FREQ), 200.0f, 12000.0f);
	const float hiss_ratio = (float)HISS_MAX_HZ / hiss_min;
	const float nr_red_db = clampf(CTRL_OR(P_NR_REDUCTION, -30.0f), -60.0f, 0.0f);
	const float nr_gmin = exp2f(nr_red_db * 0.16609640474f);
	const float nr_str = clampf(CTRL_OR(P_NR_STRENGTH, 4.0f), 0.0f, 20.0f);
	const int nr_on = nr_red_db < -0.01f && nr_str > 0.0f;

	float level_max = 0.0f;

	for (uint32_t i = 0; i < n; ++i) {
		/* ---- gate detector (undelayed) ---- */
		float peak = 0.0f;
		for (int c = 0; c < 2; ++c) {
			const float x = in[c][i];
			const float hp = s->hp_a * (s->hp_y1[c] + x - s->hp_x1[c]);
			s->hp_x1[c] = x;
			s->hp_y1[c] = hp;
			s->lp1[c] = hp + s->lp_a * (s->lp1[c] - hp);
			s->lp2[c] = s->lp1[c] + s->lp_a * (s->lp2[c] - s->lp1[c]);
			const float a = fabsf(s->lp2[c]);
			if (a > peak) {
				peak = a;
			}
		}
		s->env = peak > s->env ? peak : s->env * s->env_dec;
		if (s->env > level_max) {
			level_max = s->env;
		}
		const float env_db = to_db(s->env);

		if (env_db >= thr) {
			s->open = 1;
			s->hold_left = hold;
		} else if (env_db >= close_thr) {
			if (s->open) {
				s->hold_left = hold;
			}
		} else if (s->open) {
			if (s->hold_left > 0) {
				--s->hold_left;
			} else {
				s->open = 0;
			}
		}
		const float target = s->open ? 0.0f : range;
		if (s->gain_db < target) {
			s->gain_db = fminf(target, s->gain_db + att_rate);
		} else if (s->gain_db > target) {
			s->gain_db = fmaxf(target, s->gain_db - rel_rate);
		}
		const float gain = exp2f(s->gain_db * 0.16609640474f);
		/* fully closed and not playing: what we hear now is pure noise */
		const int learning = !s->open && range < -6.0f && s->gain_db <= range + 0.5f;

		/* ---- tail low-pass cutoff ---- */
		const float t_target = clampf((env_db - close_thr) / (float)HISS_SPAN_DB, 0.0f, 1.0f);
		s->hiss_t += (t_target - s->hiss_t) * (t_target > s->hiss_t ? s->hiss_up : s->hiss_down);
		if (s->coef_count == 0) {
			svf_coefs(s, hiss_min * powf(hiss_ratio, s->hiss_t));
		}
		s->coef_count = (s->coef_count + 1) % COEF_EVERY;
		const float wet = hiss_amt * (1.0f - s->hiss_t * s->hiss_t);

		const uint32_t ridx = (s->widx + MAX_LOOKAHEAD - s->la) % MAX_LOOKAHEAD;

		/* ---- multiband: split undelayed input, gains from undelayed bands,
		 *      applied to delayed bands ---- */
		float band[2][NB];
		if (nr_on) {
			split(s, &s->ch[0], in[0][i], band[0]);
			split(s, &s->ch[1], in[1][i], band[1]);
			for (int b = 1; b < NB; ++b) {
				const float p = 0.5f * (band[0][b] * band[0][b] + band[1][b] * band[1][b]);
				s->band_e[b] += (p - s->band_e[b]) * (p > s->band_e[b] ? s->e_att[b] : s->e_rel[b]);
				if (learning) {
					s->band_n[b] += (p - s->band_n[b]) * s->learn;
				}
				float g2 = 1.0f - nr_str * s->band_n[b] / (s->band_e[b] + 1e-20f);
				float g = g2 > nr_gmin * nr_gmin ? sqrtf(g2) : nr_gmin;
				s->band_g[b] += (g - s->band_g[b]) * (g > s->band_g[b] ? s->g_up : s->g_down[b]);
			}
		}

		for (int c = 0; c < 2; ++c) {
			Channel* ch = &s->ch[c];
			float d;
			ch->in_delay[s->widx] = in[c][i];
			if (nr_on) {
				d = 0.0f;
				for (int b = 0; b < NB; ++b) {
					ch->band_delay[b][s->widx] = band[c][b];
					d += ch->band_delay[b][ridx] * (b == 0 ? 1.0f : s->band_g[b]);
				}
			} else {
				d = ch->in_delay[ridx];
			}
			const float lp = svf_lp(s, &s->svf[c], d);
			out[c][i] = (d + wet * (lp - d)) * gain;
		}
		s->widx = (s->widx + 1) % MAX_LOOKAHEAD;
	}

	/* flush denormals in recursive states */
	for (int c = 0; c < 2; ++c) {
		if (fabsf(s->hp_y1[c]) < 1e-20f) s->hp_y1[c] = 0.0f;
		if (fabsf(s->lp1[c]) < 1e-20f) s->lp1[c] = 0.0f;
		if (fabsf(s->lp2[c]) < 1e-20f) s->lp2[c] = 0.0f;
		if (fabsf(s->svf[c].ic1) < 1e-20f) s->svf[c].ic1 = 0.0f;
		if (fabsf(s->svf[c].ic2) < 1e-20f) s->svf[c].ic2 = 0.0f;
		flush_bq(&s->ch[c].lp[0][0], NX * 2);
		flush_bq(&s->ch[c].hp[0][0], NX * 2);
		flush_bq(&s->ch[c].ap[0][0], NB * NX);
	}
	if (s->env < 1e-20f) s->env = 0.0f;

	if (s->port[P_LEVEL]) *(float*)s->port[P_LEVEL] = clampf(to_db(level_max), -100.0f, 0.0f);
	if (s->port[P_GAIN]) *(float*)s->port[P_GAIN] = s->gain_db;
	if (s->port[P_LATENCY]) *(float*)s->port[P_LATENCY] = (float)s->la;
	if (s->port[P_NR_METER]) {
		float sum = 0.0f;
		for (int b = 1; b < NB; ++b) {
			sum += to_db(s->band_g[b]);
		}
		*(float*)s->port[P_NR_METER] = nr_on ? clampf(sum / (NB - 1), -60.0f, 0.0f) : 0.0f;
	}
}

#ifdef BG_DEBUG
LV2_SYMBOL_EXPORT void bg_debug_noise(LV2_Handle h, float* out)
{
	memcpy(out, ((BaixoGate*)h)->band_n, sizeof(float) * NB);
}
#endif

static void deactivate(LV2_Handle h)
{
	(void)h;
}

static void cleanup(LV2_Handle h)
{
	free(h);
}

static const void* extension_data(const char* uri)
{
	(void)uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	BG_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
