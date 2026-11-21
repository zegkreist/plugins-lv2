/* Offline tests for baixo_fuzz.so, loaded like a host would.
 *
 *   ./test_fuzz plugin.so align     clean+fuzz paths line up: in the linear
 *                                   regime the sum must be flat around the split
 *   ./test_fuzz plugin.so alias     aliasing of a loud 2.7 kHz sine at max fuzz
 *   ./test_fuzz plugin.so in.f32 out.f32 [fuzz tone mids fuzz_lvl clean_lvl split vol]
 *                                   process a mono 48 kHz float32 file
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#define SR 48000.0
#define BLOCK 128

enum { FUZZ, TONE, MIDS, FLVL, CLVL, SPLIT, VOL, LAT, NCTL };

typedef struct {
	const LV2_Descriptor* d;
	LV2_Handle h;
	float ctl[NCTL];
	float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
} Host;

static double db(double v) { return v > 1e-15 ? 20.0 * log10(v) : -300.0; }

static void host_init(Host* H, const LV2_Descriptor* d)
{
	memset(H, 0, sizeof(*H));
	H->d = d;
	const float def[NCTL] = {4, 2500, 3, 0, 0, 120, 0, 0};
	memcpy(H->ctl, def, sizeof(def));
	H->h = d->instantiate(d, SR, "", NULL);
	d->connect_port(H->h, 0, H->inL);
	d->connect_port(H->h, 1, H->inR);
	d->connect_port(H->h, 2, H->outL);
	d->connect_port(H->h, 3, H->outR);
	for (int i = 0; i < NCTL; ++i) d->connect_port(H->h, 4 + i, &H->ctl[i]);
	d->activate(H->h);
}

static int host_run(Host* H, const float* x, float* y, long n)
{
	int bad = 0;
	for (long o = 0; o < n; o += BLOCK) {
		int m = (int)(n - o < BLOCK ? n - o : BLOCK);
		memcpy(H->inL, x + o, m * 4);
		memcpy(H->inR, x + o, m * 4);
		H->d->run(H->h, m);
		for (int i = 0; i < m; ++i) {
			if (!isfinite(H->outL[i])) bad = 1;
			y[o + i] = H->outL[i];
		}
	}
	return bad;
}

/* power of frequency f (integer Hz) over n samples via Goertzel */
static double tone_power(const float* y, long n, double f)
{
	const double w = 2 * M_PI * f / SR, c = 2 * cos(w);
	double s1 = 0, s2 = 0;
	for (long i = 0; i < n; ++i) {
		const double s0 = y[i] + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return (s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / ((double)n * n);
}

static void test_align(const LV2_Descriptor* d)
{
	/* Tiny signal => tanh stages are linear; compensate their small-signal
	 * gain by measuring the fuzz path alone, then check clean + fuzz sum. */
	const double freqs[] = {40, 60, 80, 100, 120, 150, 200, 300, 500, 1000};
	printf("alinhamento grave limpo + fuzz (regime linear, deve ficar ~0 dB):\n");
	for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); ++f) {
		long n = (long)SR;
		float* x = malloc(4 * n);
		float* y = malloc(4 * n);
		for (long i = 0; i < n; ++i) x[i] = 1e-4f * sin(2 * M_PI * freqs[f] * i / SR);
		double p[3];
		/* 0: clean only, 1: fuzz only, 2: both */
		for (int mode = 0; mode < 3; ++mode) {
			Host H;
			host_init(&H, d);
			H.ctl[FUZZ] = 0; H.ctl[TONE] = 8000; H.ctl[MIDS] = 0;
			H.ctl[CLVL] = mode == 1 ? -40 : 0;
			H.ctl[FLVL] = mode == 0 ? -40 : 0;
			host_run(&H, x, y, n);
			p[mode] = tone_power(y + n / 2, n / 2, freqs[f]);
			d->cleanup(H.h);
		}
		/* Fuzz path gain ~ G at this freq. Rescale its level so both paths are
		 * unity, then predict: in-phase sum => |1| ; misaligned => dip. */
		const double in_p = 1e-8 / 2;
		const double gc = sqrt(p[0] / in_p), gf = sqrt(p[1] / in_p), gs = sqrt(p[2] / in_p);
		/* if aligned, |clean + fuzz| == gc + gf (same phase) */
		printf("  %5.0f Hz: limpo %+6.1f dB  fuzz %+6.1f dB  soma %+6.1f dB  (perda por fase %.2f dB)\n",
		       freqs[f], db(gc), db(gf), db(gs), db(gc + gf) - db(gs));
		free(x);
		free(y);
	}
}

static void test_alias(const LV2_Descriptor* d)
{
	const double f0 = 2700;
	long n = (long)SR * 2;
	float* x = malloc(4 * n);
	float* y = malloc(4 * n);
	for (long i = 0; i < n; ++i) x[i] = 0.5f * sin(2 * M_PI * f0 * i / SR);
	Host H;
	host_init(&H, d);
	H.ctl[FUZZ] = 10; H.ctl[TONE] = 8000; H.ctl[CLVL] = -40;
	int bad = host_run(&H, x, y, n);
	const float* t = y + n / 2;
	long m = n / 2;
	double fund = tone_power(t, m, f0), worst = 0, worst_f = 0;
	for (int k = 2; k <= 40; ++k) {
		double f = fmod(k * f0, SR);
		if (f > SR / 2) f = SR - f;
		if (fmod(f, f0) < 1 || f0 - fmod(f, f0) < 1 || f < 20) continue; /* lands on a harmonic */
		double p = tone_power(t, m, f);
		if (p > worst) { worst = p; worst_f = f; }
	}
	printf("aliasing com fuzz 10 em 2.7 kHz: pior componente espuria %.0f Hz a %.1f dB do fundamental; NaN: %s\n",
	       worst_f, 10 * log10(worst / fund), bad ? "SIM" : "nao");
	d->cleanup(H.h);
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "uso: %s plugin.so align|alias|in.f32 [out.f32 params...]\n", argv[0]);
		return 1;
	}
	void* lib = dlopen(argv[1], RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	const LV2_Descriptor* d = ((LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor"))(0);

	if (!strcmp(argv[2], "align")) { test_align(d); return 0; }
	if (!strcmp(argv[2], "alias")) { test_alias(d); return 0; }

	FILE* f = fopen(argv[2], "rb");
	if (!f) { perror(argv[2]); return 1; }
	fseek(f, 0, SEEK_END);
	long n = ftell(f) / 4;
	rewind(f);
	float* x = malloc(4 * n);
	float* y = malloc(4 * n);
	if (fread(x, 4, n, f) != (size_t)n) return 1;
	fclose(f);

	Host H;
	host_init(&H, d);
	for (int i = 0; i < 7 && 4 + i < argc; ++i) H.ctl[i] = atof(argv[4 + i]);
	int bad = host_run(&H, x, y, n);
	double si = 0, so = 0, pk = 0;
	for (long i = 0; i < n; ++i) {
		si += x[i] * (double)x[i];
		so += y[i] * (double)y[i];
		if (fabs(y[i]) > pk) pk = fabs(y[i]);
	}
	printf("fuzz %.1f: RMS entrada %.1f dB, saida %.1f dB, pico saida %.1f dBFS, latencia %.0f, NaN %s\n",
	       H.ctl[FUZZ], db(sqrt(si / n)), db(sqrt(so / n)), db(pk), H.ctl[LAT], bad ? "SIM" : "nao");
	if (argc > 3 && strcmp(argv[3], "-")) {
		FILE* o = fopen(argv[3], "wb");
		fwrite(y, 4, n, o);
		fclose(o);
	}
	return 0;
}
