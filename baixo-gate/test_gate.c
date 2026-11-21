/* Offline test: loads baixo_gate.so like a host.
 *
 *   ./test_gate <plugin.so>                       synthetic note test
 *   ./test_gate <plugin.so> sweep                 band split flatness (NR on, no reduction)
 *   ./test_gate <plugin.so> in.f32 [out.f32 [thr hyst nr_red nr_str]]
 *
 * Reports gate events, RMS and "hiss" (first-difference energy, ~HF) per
 * 0.5 s, input vs output. Writes processed float32 if out.f32 is given.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>

#define SR 48000.0
#define BLOCK 128
#define NCTL 14

static double db(double v) { return v > 1e-15 ? 20.0 * log10(v) : -300.0; }

typedef struct {
	const LV2_Descriptor* d;
	LV2_Handle h;
	float ctl[NCTL]; /* ports 4..17 */
	float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
} Host;

enum { THR, HYST, ATT, HOLD, REL, RANGE, HISS, HISSF, LEVEL, GAIN, LAT, NRRED, NRSTR, NRMET };

static void host_init(Host* H, const LV2_Descriptor* d)
{
	memset(H, 0, sizeof(*H));
	H->d = d;
	float def[NCTL] = {-45, 13, 1, 100, 300, -45, 0.8f, 2500, 0, 0, 0, -30, 4.0f, 0};
	memcpy(H->ctl, def, sizeof(def));
	H->h = d->instantiate(d, SR, "", NULL);
	d->connect_port(H->h, 0, H->inL);
	d->connect_port(H->h, 1, H->inR);
	d->connect_port(H->h, 2, H->outL);
	d->connect_port(H->h, 3, H->outR);
	for (int i = 0; i < NCTL; ++i) d->connect_port(H->h, 4 + i, &H->ctl[i]);
	d->activate(H->h);
}

/* process whole buffer; prints gate events if verbose */
static void host_run(Host* H, const float* x, float* y, long n, int verbose)
{
	int was_open = 0, events = 0, bad = 0;
	for (long o = 0; o < n; o += BLOCK) {
		int m = (int)(n - o < BLOCK ? n - o : BLOCK);
		memcpy(H->inL, x + o, m * 4);
		memcpy(H->inR, x + o, m * 4);
		H->d->run(H->h, m);
		for (int i = 0; i < m; ++i) {
			if (!isfinite(H->outL[i])) bad = 1;
			y[o + i] = H->outL[i];
		}
		int is_open = H->ctl[GAIN] > -1.0f;
		if (is_open != was_open) {
			++events;
			if (verbose) printf("  %6.3fs  %s  (detector %.1f dB)\n", o / SR, is_open ? "ABRIU " : "FECHOU", H->ctl[LEVEL]);
			was_open = is_open;
		}
	}
	if (verbose) printf("latencia %.0f amostras; eventos abre/fecha %d; NaN/inf %s\n", H->ctl[LAT], events, bad ? "SIM" : "nao");
}

static void sweep(const LV2_Descriptor* d)
{
	const double freqs[] = {40, 80, 150, 300, 450, 600, 900, 1200, 1800, 2400, 3500, 4800, 7000, 9600, 13000, 16000};
	printf("resposta com bandas ligadas e reducao ~0 (deve ser ~0 dB):\n");
	for (unsigned f = 0; f < sizeof(freqs) / sizeof(freqs[0]); ++f) {
		Host H;
		host_init(&H, d);
		H.ctl[THR] = -90; H.ctl[RANGE] = 0; H.ctl[HISS] = 0; H.ctl[NRSTR] = 1e-6f; H.ctl[NRRED] = -20;
		long n = (long)SR;
		float* x = malloc(4 * n);
		float* y = malloc(4 * n);
		for (long i = 0; i < n; ++i) x[i] = 0.5f * sin(2 * M_PI * freqs[f] * i / SR);
		host_run(&H, x, y, n, 0);
		double si = 0, so = 0;
		for (long i = n / 2; i < n; ++i) { si += x[i] * (double)x[i]; so += y[i] * (double)y[i]; }
		printf("  %6.0f Hz: %+6.2f dB\n", freqs[f], 10 * log10(so / si));
		d->cleanup(H.h);
		free(x);
		free(y);
	}
}

static float* synth(long* n)
{
	long len = (long)(SR * 10);
	float* x = malloc(sizeof(float) * len);
	srand(1);
	for (long i = 0; i < len; ++i) {
		double t = i / SR;
		double hiss = ((rand() / (double)RAND_MAX) * 2 - 1) * 0.0019;
		double note = 0;
		if (t >= 1 && t < 7) note = 0.8 * exp(-(t - 1) / 1.2) * sin(2 * M_PI * 55 * (t - 1));
		if (t >= 8) {
			double tt = fmod(t - 8, 0.25);
			note = 0.5 * exp(-tt / 0.08) * sin(2 * M_PI * 82.4 * tt);
		}
		x[i] = (float)(note + hiss);
	}
	*n = len;
	return x;
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "uso: %s plugin.so [sweep | in.f32 [out.f32 [thr hyst nr_red nr_str]]]\n", argv[0]);
		return 1;
	}
	void* lib = dlopen(argv[1], RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	LV2_Descriptor_Function fn = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
	const LV2_Descriptor* d = fn(0);
	printf("plugin: %s\n", d->URI);

	if (argc > 2 && !strcmp(argv[2], "sweep")) {
		sweep(d);
		return 0;
	}

	long n = 0;
	float* x;
	if (argc > 2) {
		FILE* f = fopen(argv[2], "rb");
		if (!f) { perror(argv[2]); return 1; }
		fseek(f, 0, SEEK_END);
		n = ftell(f) / 4;
		rewind(f);
		x = malloc(sizeof(float) * n);
		if (fread(x, 4, n, f) != (size_t)n) { fprintf(stderr, "read error\n"); return 1; }
		fclose(f);
	} else {
		x = synth(&n);
	}
	float* y = malloc(sizeof(float) * n);

	Host H;
	host_init(&H, d);
	if (argc > 4) H.ctl[THR] = atof(argv[4]);
	if (argc > 5) H.ctl[HYST] = atof(argv[5]);
	if (argc > 6) H.ctl[NRRED] = atof(argv[6]);
	if (argc > 7) H.ctl[NRSTR] = atof(argv[7]);
	host_run(&H, x, y, n, 1);

	void (*dbg)(LV2_Handle, float*) = (void (*)(LV2_Handle, float*))dlsym(lib, "bg_debug_noise");
	if (dbg) {
		float nn[13];
		dbg(H.h, nn);
		printf("ruido aprendido por banda:");
		for (int b = 0; b < 13; ++b) printf(" %.3g", nn[b]);
		printf("\n");
	}

	/* "hiss" = first difference energy; compensate output latency */
	long lat = (long)H.ctl[LAT];
	printf("  tempo   RMS ent  RMS sai   agudo ent  agudo sai\n");
	long W = (long)(SR / 2);
	for (long o = 0; o + W + lat < n; o += W) {
		double si = 0, so = 0, hi = 0, ho = 0;
		for (long i = o + 1; i < o + W; ++i) {
			si += x[i] * (double)x[i];
			so += y[i + lat] * (double)y[i + lat];
			double a = x[i] - x[i - 1], b = y[i + lat] - y[i + lat - 1];
			hi += a * a;
			ho += b * b;
		}
		printf("  %5.1fs  %7.1f  %7.1f   %9.1f  %9.1f\n", o / SR, db(sqrt(si / W)), db(sqrt(so / W)),
		       db(sqrt(hi / W)), db(sqrt(ho / W)));
	}

	if (argc > 3 && strcmp(argv[3], "-")) {
		FILE* f = fopen(argv[3], "wb");
		fwrite(y, 4, n, f);
		fclose(f);
	}
	d->cleanup(H.h);
	return 0;
}
