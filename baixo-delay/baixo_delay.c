/*
 * Baixo Delay: psychedelic, airy delay for bass, with a dry low end.
 *
 *   in ─────────────────────────────────────────────────────────────► dry ──┐
 *    └─► mono ─► LR4 high-pass ("Divisão") ─► send ─┐                        │
 *         ┌──────────────────────────────────────────┘                       │
 *         ▼                                                                  │
 *       (+) ─► "Tom" LP ─► loop HP ─► soft sat ─► delay line (L, R) ─┐       │
 *        ▲                                                          │       │
 *        │          modulated read ("Modulação", "Velocidade")  ◄───┘       │
 *        │                │                                                  │
 *        │                ▼                                                  │
 *        │        allpass diffusion ("Difusão") ─► tap ─► "Nível eco" ─────(+)─► out
 *        │                                          │
 *        └── "Repetições" × L/R cross-feed ("Abertura") ◄┘
 *
 * - Only the signal above "Divisão" is repeated: the bass lows stay dry and
 *   tight, the repeats float on top of them without mud.
 * - Each repeat goes through the tone low-pass and a gentle high-pass again,
 *   so it gets darker and thinner like an analog / tape delay.
 * - The read point is modulated by two LFOs (L and R in quadrature): tape
 *   wow / chorus on the repeats, swirling across the stereo field.
 * - Four allpasses in the loop smear each repeat into a cloud. "Difusão"
 *   crossfades the crisp echo into the smeared one; both start right on
 *   "Tempo" (an allpass passes part of the signal with no delay, so the
 *   allpass delay must not be subtracted from the main delay: that would make
 *   a pre-echo).
 * - "Abertura" goes from mono repeats (0) to full ping-pong (100).
 * - Soft saturation in the loop: feedback can go to 100% (self-oscillation)
 *   without blowing up.
 * - "Tempo" changes glide like tape (pitch bend), no clicks.
 *
 * Zero latency. The dry signal passes untouched.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "bass_split.h"

#define BD_URI "urn:zegkreist:lv2:baixo-delay"

#define MAX_TIME_MS 2000.0
#define MAX_MOD_MS 3.5      /* "Modulação" 10 = ±3.5 ms swing of the read point */
#define LFO2_RATIO 0.37     /* slow second LFO: makes the wobble less mechanical */
#define LFO2_AMOUNT 0.3
#define LOOP_HP_RATIO 0.5   /* loop high-pass at Divisão / 2 */
#define GLIDE_MS 200.0      /* tape-style "Tempo" glide */
#define SMOOTH_MS 20.0
#define AP_N 4
#define AP_MAX 4096         /* >= longest allpass (13.13 ms) at 192 kHz */
#define AP_G 0.7f

/* allpass lengths (ms), different per side for stereo decorrelation */
static const double AP_MS[2][AP_N] = {{4.31, 6.73, 9.97, 13.13}, {4.79, 7.19, 10.61, 12.41}};

typedef enum {
	P_IN_L = 0,
	P_IN_R,
	P_OUT_L,
	P_OUT_R,
	P_TIME,
	P_FEEDBACK,
	P_LEVEL,
	P_TONE,
	P_SPLIT,
	P_MOD,
	P_RATE,
	P_DIFFUSION,
	P_WIDTH,
	P_COUNT
} PortIndex;

typedef struct {
	float* line;
	SplitBqState tone, hp;
	float ap[AP_N][AP_MAX];
	int ap_pos[AP_N];
	int ap_len[AP_N];
} Chan;

typedef struct {
	void* port[P_COUNT];
	double sr;

	uint32_t size, mask; /* delay line length (power of 2) */
	uint32_t wpos;

	SplitXover xover;           /* only the high side is used */
	SplitXoverState send_state; /* mono send high-pass */
	SplitBq tone, hp;
	float cur_split, cur_tone;

	double ph1, ph2; /* LFO phases, radians */
	float glide, smooth;

	/* smoothed values; `fresh` = jump to targets on the first run */
	float delay, feedback, level, depth, diffusion, width;
	int fresh;

	Chan ch[2];
} BaixoDelay;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float db_to_lin(float db)
{
	return powf(10.0f, db / 20.0f);
}

static void design_filters(BaixoDelay* s, float split, float tone)
{
	if (split != s->cur_split) {
		split_xover_design(&s->xover, s->sr, split);
		s->hp = split_bq_design(1, s->sr, split * LOOP_HP_RATIO, M_SQRT1_2, 0.0);
		s->cur_split = split;
	}
	if (tone != s->cur_tone) {
		s->tone = split_bq_design(0, s->sr, tone, M_SQRT1_2, 0.0);
		s->cur_tone = tone;
	}
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
                              const char* path, const LV2_Feature* const* f)
{
	(void)d;
	(void)path;
	(void)f;
	BaixoDelay* s = (BaixoDelay*)calloc(1, sizeof(BaixoDelay));
	if (!s) {
		return NULL;
	}
	s->sr = rate;

	/* max delay + modulation swing + interpolation margin */
	const double need = rate * (MAX_TIME_MS + 2.0 * MAX_MOD_MS) * 0.001 + 8.0;
	s->size = 1;
	while (s->size < need) {
		s->size <<= 1;
	}
	s->mask = s->size - 1;
	for (int c = 0; c < 2; ++c) {
		s->ch[c].line = (float*)calloc(s->size, sizeof(float));
		if (!s->ch[c].line) {
			free(s->ch[0].line);
			free(s);
			return NULL;
		}
		for (int k = 0; k < AP_N; ++k) {
			int len = (int)lround(AP_MS[c][k] * 0.001 * rate);
			s->ch[c].ap_len[k] = len < 1 ? 1 : (len > AP_MAX ? AP_MAX : len);
		}
	}

	s->glide = (float)(1.0 - exp(-1.0 / (GLIDE_MS * 0.001 * rate)));
	s->smooth = (float)(1.0 - exp(-1.0 / (SMOOTH_MS * 0.001 * rate)));
	s->cur_split = s->cur_tone = -1.0f;
	return (LV2_Handle)s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	BaixoDelay* s = (BaixoDelay*)h;
	if (port < P_COUNT) {
		s->port[port] = data;
	}
}

#define CTRL(i) (*(const float*)s->port[i])

static void activate(LV2_Handle h)
{
	BaixoDelay* s = (BaixoDelay*)h;
	for (int c = 0; c < 2; ++c) {
		Chan* ch = &s->ch[c];
		memset(ch->line, 0, s->size * sizeof(float));
		memset(ch->ap, 0, sizeof(ch->ap));
		memset(ch->ap_pos, 0, sizeof(ch->ap_pos));
		memset(&ch->tone, 0, sizeof(ch->tone));
		memset(&ch->hp, 0, sizeof(ch->hp));
	}
	memset(&s->send_state, 0, sizeof(s->send_state));
	s->wpos = 0;
	s->ph1 = s->ph2 = 0.0;
	s->cur_split = s->cur_tone = -1.0f;
	s->fresh = 1;
}

/* 4-point Hermite read, `d` samples behind the next write (d >= 3) */
static float line_read(const BaixoDelay* s, const float* line, float d)
{
	/* split integer / fraction before subtracting: a float of the full read
	 * position would lose sub-sample precision on long lines */
	const float di = floorf(d);
	const float t = 1.0f - (d - di);
	const uint32_t i = s->wpos - (uint32_t)di - 1u;
	const float xm1 = line[(i - 1) & s->mask];
	const float x0 = line[i & s->mask];
	const float x1 = line[(i + 1) & s->mask];
	const float x2 = line[(i + 2) & s->mask];
	const float c1 = 0.5f * (x1 - xm1);
	const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
	const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
	return ((c3 * t + c2) * t + c1) * t + x0;
}

/* Schroeder allpass chain */
static float diffuse(Chan* ch, float x)
{
	for (int k = 0; k < AP_N; ++k) {
		float* buf = ch->ap[k];
		const int p = ch->ap_pos[k];
		const float dl = buf[p];
		const float v = x - AP_G * dl;
		x = AP_G * v + dl;
		buf[p] = fabsf(v) < 1e-20f ? 0.0f : v;
		ch->ap_pos[k] = p + 1 == ch->ap_len[k] ? 0 : p + 1;
	}
	return x;
}

static void run(LV2_Handle h, uint32_t n)
{
	BaixoDelay* s = (BaixoDelay*)h;
	const float* in[2] = {(const float*)s->port[P_IN_L], (const float*)s->port[P_IN_R]};
	float* out[2] = {(float*)s->port[P_OUT_L], (float*)s->port[P_OUT_R]};

	design_filters(s, clampf(CTRL(P_SPLIT), 40.0f, 800.0f), clampf(CTRL(P_TONE), 500.0f, 12000.0f));

	const float t_delay = clampf(CTRL(P_TIME), 60.0f, (float)MAX_TIME_MS) * 0.001f * (float)s->sr;
	const float t_fb = clampf(CTRL(P_FEEDBACK), 0.0f, 100.0f) * 0.01f;
	const float t_level = CTRL(P_LEVEL) <= -40.0f ? 0.0f : db_to_lin(clampf(CTRL(P_LEVEL), -40.0f, 6.0f));
	const float t_depth = clampf(CTRL(P_MOD), 0.0f, 10.0f) * 0.1f * (float)(MAX_MOD_MS * 0.001 * s->sr);
	const float t_diff = clampf(CTRL(P_DIFFUSION), 0.0f, 100.0f) * 0.01f;
	const float t_width = clampf(CTRL(P_WIDTH), 0.0f, 100.0f) * 0.01f;
	const double dph = 2.0 * M_PI * clampf(CTRL(P_RATE), 0.05f, 5.0f) / s->sr;
	if (s->fresh) { /* no glide / fade-in on load */
		s->delay = t_delay;
		s->feedback = t_fb;
		s->level = t_level;
		s->depth = t_depth;
		s->diffusion = t_diff;
		s->width = t_width;
		s->fresh = 0;
	}

	for (uint32_t i = 0; i < n; ++i) {
		s->delay += (t_delay - s->delay) * s->glide;
		s->feedback += (t_fb - s->feedback) * s->smooth;
		s->level += (t_level - s->level) * s->smooth;
		s->depth += (t_depth - s->depth) * s->smooth;
		s->diffusion += (t_diff - s->diffusion) * s->smooth;
		s->width += (t_width - s->width) * s->smooth;

		s->ph1 += dph;
		s->ph2 += dph * LFO2_RATIO;
		if (s->ph1 > 2.0 * M_PI) s->ph1 -= 2.0 * M_PI;
		if (s->ph2 > 2.0 * M_PI) s->ph2 -= 2.0 * M_PI;

		/* read + diffuse both sides first: the writes cross-feed them */
		float tap[2];
		for (int c = 0; c < 2; ++c) {
			Chan* ch = &s->ch[c];
			const double q = c * M_PI_2; /* R in quadrature with L */
			const float lfo = (float)(sin(s->ph1 + q) + LFO2_AMOUNT * sin(s->ph2 - q)) /
			                  (1.0f + LFO2_AMOUNT);
			float d = s->delay + s->depth * lfo;
			if (d < 3.0f) d = 3.0f;
			const float r = line_read(s, ch->line, d);
			tap[c] = r + s->diffusion * (diffuse(ch, r) - r);
		}

		/* mono send, lows removed */
		float low, send;
		split_xover_run(&s->xover, &s->send_state, 0.5f * (in[0][i] + in[1][i]), &low, &send);

		const float w = s->width;
		for (int c = 0; c < 2; ++c) {
			Chan* ch = &s->ch[c];
			const float fb_in = (1.0f - w) * tap[c] + w * tap[1 - c];
			float x = (c == 0 ? send : (1.0f - w) * send) + s->feedback * fb_in;
			x = split_bq_run(&s->tone, &ch->tone, x);
			x = split_bq_run(&s->hp, &ch->hp, x);
			x = tanhf(x);
			ch->line[s->wpos] = fabsf(x) < 1e-20f ? 0.0f : x;
			out[c][i] = in[c][i] + s->level * tap[c];
		}
		s->wpos = (s->wpos + 1) & s->mask;
	}

	split_xover_flush(&s->send_state);
	for (int c = 0; c < 2; ++c) {
		split_bq_flush(&s->ch[c].tone);
		split_bq_flush(&s->ch[c].hp);
	}
}

static void deactivate(LV2_Handle h)
{
	(void)h;
}

static void cleanup(LV2_Handle h)
{
	BaixoDelay* s = (BaixoDelay*)h;
	free(s->ch[0].line);
	free(s->ch[1].line);
	free(s);
}

static const void* extension_data(const char* uri)
{
	(void)uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	BD_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
