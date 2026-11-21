/*
 * Baixo Fuzz: light, warm doom fuzz for bass, with clean low end.
 *
 *   in ──► LR4 crossover ──► low ──► delay (oversampler latency) ──► "Grave limpo" ─┐
 *                        └─► high ─► mid push ─► 4x [drive, 2 asymmetric tanh stages] │
 *                                     ─► DC block ─► "Médios" ─► "Tom" ─► "Nível fuzz" ┴─► "Volume"
 *
 * - The fuzz never sees the sub lows (no mud, no farting), and the clean lows
 *   come back underneath, time-aligned with the fuzz path.
 * - Two asymmetric soft-clipping stages (opposite bias) give tube-like even
 *   harmonics and a smooth, saturated, not fizzy, fuzz.
 * - The nonlinearity runs at 4x oversampling to keep aliasing out.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#include "bass_split.h"

#define BF_URI "urn:zegkreist:lv2:baixo-fuzz"

#define PRE_MID_HZ 800.0   /* mids pushed into the clipper */
#define PRE_MID_DB 4.0
#define MIDS_HZ 650.0      /* "Médios" post-EQ */
#define MIDS_Q 0.8
#define BIAS1 0.25f        /* stage asymmetries (even harmonics) */
#define BIAS2 -0.15f
#define STAGE2_GAIN 1.6f
#define INTERSTAGE_HZ 9000.0
#define DC_HZ 20.0
#define DIRTY_TRIM 0.5f    /* -6 dB: keeps the fuzz near the clean level */
#define SMOOTH_MS 20.0

typedef enum {
	P_IN_L = 0,
	P_IN_R,
	P_OUT_L,
	P_OUT_R,
	P_FUZZ,
	P_TONE,
	P_MIDS,
	P_FUZZ_LEVEL,
	P_CLEAN_LEVEL,
	P_SPLIT,
	P_VOLUME,
	P_LATENCY,
	P_COUNT
} PortIndex;

typedef struct {
	SplitXoverState xover;
	SplitDelay clean_delay;
	SplitBqState pre_mid;
	SplitOsState os;
	float inter_lp; /* 4x-rate 1-pole between the stages */
	float dc_x1, dc_y1;
	SplitBqState mids, tone;
} Chan;

typedef struct {
	void* port[P_COUNT];
	double sr;

	SplitXover xover;
	SplitOs os;
	SplitBq pre_mid, mids, tone;
	float inter_a, dc_a, smooth;

	/* last control values used to design filters */
	float cur_split, cur_mids, cur_tone;

	/* smoothed gains; `fresh` = jump to targets on the first run */
	float drive, comp, fuzz_gain, clean_gain, volume;
	int fresh;

	Chan ch[2];
} BaixoFuzz;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float db_to_lin(float db)
{
	return powf(10.0f, db / 20.0f);
}

static void design_filters(BaixoFuzz* s, float split, float mids, float tone)
{
	if (split != s->cur_split) {
		split_xover_design(&s->xover, s->sr, split);
		s->cur_split = split;
	}
	if (mids != s->cur_mids) {
		s->mids = split_bq_design(2, s->sr, MIDS_HZ, MIDS_Q, mids);
		s->cur_mids = mids;
	}
	if (tone != s->cur_tone) {
		s->tone = split_bq_design(0, s->sr, tone, M_SQRT1_2, 0.0);
		s->cur_tone = tone;
	}
}

/* Output make-up per Fuzz knob step (0..10), measured on a real bass DI so
 * the fuzz path keeps the same RMS level as the dry input at any setting. */
static const float COMP_DB[11] = {-2.6f, -5.0f, -6.9f, -8.6f, -9.8f, -10.7f,
                                  -11.4f, -11.8f, -12.2f, -12.4f, -12.6f};

/* Fuzz knob 0..10 -> drive into the clipper and output make-up. */
static void fuzz_gains(float fuzz, float* drive, float* comp)
{
	const float drive_db = 6.0f + 3.6f * fuzz; /* 6..42 dB */
	const int i = fuzz >= 10.0f ? 9 : (int)fuzz;
	const float t = fuzz - (float)i;
	*drive = db_to_lin(drive_db);
	*comp = db_to_lin(COMP_DB[i] + t * (COMP_DB[i + 1] - COMP_DB[i]));
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
                              const char* path, const LV2_Feature* const* f)
{
	(void)d;
	(void)path;
	(void)f;
	BaixoFuzz* s = (BaixoFuzz*)calloc(1, sizeof(BaixoFuzz));
	if (!s) {
		return NULL;
	}
	s->sr = rate;
	split_os_design(&s->os);
	s->pre_mid = split_bq_design(2, rate, PRE_MID_HZ, 0.7, PRE_MID_DB);
	s->inter_a = (float)exp(-2.0 * M_PI * INTERSTAGE_HZ / (rate * SPLIT_OS));
	s->dc_a = (float)exp(-2.0 * M_PI * DC_HZ / rate);
	s->smooth = (float)(1.0 - exp(-1.0 / (SMOOTH_MS * 0.001 * rate)));
	s->cur_split = s->cur_mids = s->cur_tone = -1.0f;
	return (LV2_Handle)s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	BaixoFuzz* s = (BaixoFuzz*)h;
	if (port < P_COUNT) {
		s->port[port] = data;
	}
}

#define CTRL(i) (*(const float*)s->port[i])

static void activate(LV2_Handle h)
{
	BaixoFuzz* s = (BaixoFuzz*)h;
	memset(s->ch, 0, sizeof(s->ch));
	s->cur_split = s->cur_mids = s->cur_tone = -1.0f;
	s->fresh = 1;
}

static void run(LV2_Handle h, uint32_t n)
{
	BaixoFuzz* s = (BaixoFuzz*)h;
	const float* in[2] = {(const float*)s->port[P_IN_L], (const float*)s->port[P_IN_R]};
	float* out[2] = {(float*)s->port[P_OUT_L], (float*)s->port[P_OUT_R]};

	design_filters(s, clampf(CTRL(P_SPLIT), 40.0f, 500.0f), clampf(CTRL(P_MIDS), -12.0f, 12.0f),
	               clampf(CTRL(P_TONE), 600.0f, 8000.0f));

	float t_drive, t_comp;
	fuzz_gains(clampf(CTRL(P_FUZZ), 0.0f, 10.0f), &t_drive, &t_comp);
	const float t_fuzz = db_to_lin(clampf(CTRL(P_FUZZ_LEVEL), -40.0f, 12.0f));
	const float t_clean = CTRL(P_CLEAN_LEVEL) <= -40.0f ? 0.0f : db_to_lin(clampf(CTRL(P_CLEAN_LEVEL), -40.0f, 12.0f));
	const float t_vol = db_to_lin(clampf(CTRL(P_VOLUME), -24.0f, 12.0f));
	const float k1 = tanhf(BIAS1), k2 = tanhf(BIAS2);
	if (s->fresh) { /* no fade-in on load */
		s->drive = t_drive;
		s->comp = t_comp;
		s->fuzz_gain = t_fuzz;
		s->clean_gain = t_clean;
		s->volume = t_vol;
		s->fresh = 0;
	}

	for (uint32_t i = 0; i < n; ++i) {
		s->drive += (t_drive - s->drive) * s->smooth;
		s->comp += (t_comp - s->comp) * s->smooth;
		s->fuzz_gain += (t_fuzz - s->fuzz_gain) * s->smooth;
		s->clean_gain += (t_clean - s->clean_gain) * s->smooth;
		s->volume += (t_vol - s->volume) * s->smooth;

		for (int c = 0; c < 2; ++c) {
			Chan* ch = &s->ch[c];
			float low, high;
			split_xover_run(&s->xover, &ch->xover, in[c][i], &low, &high);
			const float clean = split_delay_run(&ch->clean_delay, low);

			/* ---- fuzz path ---- */
			const float pre = split_bq_run(&s->pre_mid, &ch->pre_mid, high) * s->drive;
			float up[SPLIT_OS];
			split_os_up(&s->os, &ch->os, pre, up);
			for (int k = 0; k < SPLIT_OS; ++k) {
				float y = tanhf(up[k] + BIAS1) - k1;
				ch->inter_lp = y + s->inter_a * (ch->inter_lp - y);
				up[k] = tanhf(STAGE2_GAIN * ch->inter_lp + BIAS2) - k2;
			}
			float dirty = split_os_down(&s->os, &ch->os, up);
			/* DC blocker: asymmetric clipping produces an offset */
			const float dc = s->dc_a * (ch->dc_y1 + dirty - ch->dc_x1);
			ch->dc_x1 = dirty;
			ch->dc_y1 = dc;
			dirty = split_bq_run(&s->mids, &ch->mids, dc);
			dirty = split_bq_run(&s->tone, &ch->tone, dirty);
			dirty *= DIRTY_TRIM * s->comp * s->fuzz_gain;

			out[c][i] = (clean * s->clean_gain + dirty) * s->volume;
		}
	}

	for (int c = 0; c < 2; ++c) {
		Chan* ch = &s->ch[c];
		split_xover_flush(&ch->xover);
		split_bq_flush(&ch->pre_mid);
		split_bq_flush(&ch->mids);
		split_bq_flush(&ch->tone);
		if (fabsf(ch->inter_lp) < 1e-20f) ch->inter_lp = 0.0f;
		if (fabsf(ch->dc_y1) < 1e-20f) ch->dc_y1 = 0.0f;
	}
	if (s->port[P_LATENCY]) {
		*(float*)s->port[P_LATENCY] = (float)SPLIT_OS_LATENCY;
	}
}

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
	BF_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
