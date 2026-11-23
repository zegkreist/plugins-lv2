/* Offline tests for baixo_delay.so, loaded like a host would.
 *
 *   ./test_delay plugin.so echo     dry passes untouched; echoes land on "Tempo";
 *                                   feedback ratio; lows below "Divisão" do not
 *                                   repeat; ping-pong alternates sides
 *   ./test_delay plugin.so stable   worst case (feedback 100, everything maxed,
 *                                   "Tempo" swept): no NaN, bounded output, CPU
 *   ./test_delay plugin.so in.f32 out.f32 [time fb level tone split mod rate diff width]
 *                                   process a mono 48 kHz float32 file; writes
 *                                   stereo interleaved float32, with a 6 s tail
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lv2/core/lv2.h>

#define SR 48000.0
#define BLOCK 128

enum { TIME, FB, LEVEL, TONE, SPLIT, MOD, RATE, DIFF, WIDTH, NCTL };

typedef struct {
	const LV2_Descriptor* d;
	LV2_Handle h;
	float ctl[NCTL];
	float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
} Host;

static int failures = 0;

static double db(double v) { return v > 1e-15 ? 20.0 * log10(v) : -300.0; }

static void check(int ok, const char* what)
{
	printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
	if (!ok) failures++;
}

static void host_init(Host* H, const LV2_Descriptor* d)
{
	memset(H, 0, sizeof(*H));
	H->d = d;
	const float def[NCTL] = {420, 50, -4, 2800, 180, 4, 0.5f, 35, 70};
	memcpy(H->ctl, def, sizeof(def));
	H->h = d->instantiate(d, SR, "", NULL);
	d->connect_port(H->h, 0, H->inL);
	d->connect_port(H->h, 1, H->inR);
	d->connect_port(H->h, 2, H->outL);
	d->connect_port(H->h, 3, H->outR);
	for (int i = 0; i < NCTL; ++i) d->connect_port(H->h, 4 + i, &H->ctl[i]);
	d->activate(H->h);
}

/* mono x in (both inputs), L/R out; returns 1 on NaN/Inf */
static int host_run(Host* H, const float* x, float* yl, float* yr, long n)
{
	int bad = 0;
	for (long o = 0; o < n; o += BLOCK) {
		int m = (int)(n - o < BLOCK ? n - o : BLOCK);
		memcpy(H->inL, x + o, m * 4);
		memcpy(H->inR, x + o, m * 4);
		H->d->run(H->h, m);
		for (int i = 0; i < m; ++i) {
			if (!isfinite(H->outL[i]) || !isfinite(H->outR[i])) bad = 1;
			yl[o + i] = H->outL[i];
			yr[o + i] = H->outR[i];
		}
	}
	return bad;
}

/* RMS of y in a window [t0, t0+len) seconds */
static double rms(const float* y, double t0, double len)
{
	long a = (long)(t0 * SR), b = (long)((t0 + len) * SR);
	double s = 0;
	for (long i = a; i < b; ++i) s += y[i] * (double)y[i];
	return sqrt(s / (b - a));
}

/* first sample index at or after `from` whose |y| exceeds thr */
static long onset(const float* y, long from, long n, float thr)
{
	for (long i = from; i < n; ++i)
		if (fabsf(y[i]) > thr) return i;
	return -1;
}

/* 20 ms Hann-windowed sine burst at t=0 */
static void burst(float* x, long n, double f, double amp)
{
	memset(x, 0, 4 * n);
	const long len = (long)(0.02 * SR);
	for (long i = 0; i < len; ++i)
		x[i] = (float)(amp * sin(2 * M_PI * f * i / SR) * 0.5 * (1 - cos(2 * M_PI * i / len)));
}

static void test_echo(const LV2_Descriptor* d)
{
	const long n = (long)(SR * 2);
	float* x = malloc(4 * n);
	float* yl = malloc(4 * n);
	float* yr = malloc(4 * n);
	Host H;

	printf("som seco intacto:\n");
	for (long i = 0; i < n; ++i) x[i] = (float)(0.5 * sin(2 * M_PI * 55 * i / SR) + 0.2 * sin(2 * M_PI * 1234 * i / SR));
	host_init(&H, d);
	H.ctl[LEVEL] = -40;
	host_run(&H, x, yl, yr, n);
	double err = 0;
	for (long i = 0; i < n; ++i) err = fmax(err, fmax(fabs(yl[i] - x[i]), fabs(yr[i] - x[i])));
	printf("    erro maximo seco = %.2g\n", err);
	check(err == 0.0, "com Nivel eco no minimo a saida e exatamente a entrada");
	d->cleanup(H.h);

	/* crisp echoes: no modulation, no diffusion, mono, filters wide open */
	const double T = 0.300;
	printf("ecos (Tempo 300 ms, Repeticoes 50%%, sem modulacao):\n");
	for (int diff = 0; diff <= 1; ++diff) {
		burst(x, n, 1000, 0.1);
		host_init(&H, d);
		H.ctl[TIME] = (float)(T * 1000); H.ctl[FB] = 50; H.ctl[LEVEL] = 0; H.ctl[TONE] = 12000;
		H.ctl[SPLIT] = 40; H.ctl[MOD] = 0; H.ctl[DIFF] = diff ? 100 : 0; H.ctl[WIDTH] = 0;
		host_run(&H, x, yl, yr, n);
		const long o1 = onset(yl, (long)(0.05 * SR), n, 0.001f);
		const double e1 = rms(yl, T, 0.02), e2 = rms(yl, 2 * T, 0.02);
		if (!diff) {
			printf("    1o eco comeca em %.2f ms; 2o/1o = %.2f dB\n", o1 * 1000 / SR, db(e2 / e1));
			check(fabs(o1 / SR - T) < 0.001, "1o eco no Tempo (+-1 ms)");
			check(fabs(db(e2 / e1) - db(0.5)) < 0.5, "cada repeticao cai o valor de Repeticoes (-6 dB a 50%)");
		} else {
			/* allpass delay is compensated: nothing before T, energy right after */
			const double pre = rms(yl, 0.05, T - 0.06), post = rms(yl, T, 0.05);
			printf("    Difusao 100: 1o som em %.2f ms; energia antes/depois do Tempo %.1f dB\n",
			       o1 * 1000 / SR, db(pre / post));
			check(o1 / SR > T - 0.0005 && o1 / SR < T + 0.003 && db(pre / post) < -60, "com Difusao o eco continua comecando no Tempo, sem pre-eco");
		}
		d->cleanup(H.h);
	}

	printf("grave nao repete (Divisao 180 Hz):\n");
	double e[2];
	const double fr[2] = {50, 1000};
	for (int k = 0; k < 2; ++k) {
		burst(x, n, fr[k], 0.1);
		host_init(&H, d);
		H.ctl[TIME] = (float)(T * 1000); H.ctl[FB] = 0; H.ctl[LEVEL] = 0; H.ctl[TONE] = 12000;
		H.ctl[MOD] = 0; H.ctl[DIFF] = 0; H.ctl[WIDTH] = 0;
		host_run(&H, x, yl, yr, n);
		e[k] = rms(yl, T - 0.01, 0.06);
		d->cleanup(H.h);
	}
	printf("    eco de 50 Hz vs eco de 1 kHz: %.1f dB\n", db(e[0] / e[1]));
	check(db(e[0] / e[1]) < -30, "eco de 50 Hz pelo menos 30 dB abaixo do de 1 kHz");

	printf("ping-pong (Abertura 100):\n");
	burst(x, n, 1000, 0.1);
	host_init(&H, d);
	H.ctl[TIME] = (float)(T * 1000); H.ctl[FB] = 70; H.ctl[LEVEL] = 0; H.ctl[TONE] = 12000;
	H.ctl[SPLIT] = 40; H.ctl[MOD] = 0; H.ctl[DIFF] = 0; H.ctl[WIDTH] = 100;
	host_run(&H, x, yl, yr, n);
	const double l1 = rms(yl, T, 0.02), r1 = rms(yr, T, 0.02);
	const double l2 = rms(yl, 2 * T, 0.02), r2 = rms(yr, 2 * T, 0.02);
	printf("    1o eco: E %.1f / D %.1f dB   2o eco: E %.1f / D %.1f dB\n", db(l1), db(r1), db(l2), db(r2));
	check(db(r1 / l1) < -60 && db(l2 / r2) < -60, "ecos alternam entre esquerda e direita");
	d->cleanup(H.h);

	free(x);
	free(yl);
	free(yr);
}

static void test_stable(const LV2_Descriptor* d)
{
	const long n = (long)(SR * 20);
	float* x = calloc(n, 4);
	float* yl = malloc(4 * n);
	float* yr = malloc(4 * n);
	unsigned r = 1;
	for (long i = 0; i < (long)(SR * 3); ++i) {
		r = r * 1664525u + 1013904223u;
		x[i] = 0.9f * ((float)(r >> 8) / 8388608.0f - 1.0f);
	}
	Host H;
	host_init(&H, d);
	H.ctl[FB] = 100; H.ctl[LEVEL] = 6; H.ctl[TONE] = 12000; H.ctl[SPLIT] = 40;
	H.ctl[MOD] = 10; H.ctl[RATE] = 5; H.ctl[DIFF] = 100; H.ctl[WIDTH] = 50;
	int bad = 0;
	double pk = 0;
	const clock_t t0 = clock();
	for (long o = 0; o < n; o += BLOCK * 64) {
		/* sweep Tempo through its whole range while it self-oscillates */
		H.ctl[TIME] = (float)(60 + 1940 * (0.5 + 0.5 * sin(o / SR)));
		const long m = n - o < BLOCK * 64 ? n - o : BLOCK * 64;
		bad |= host_run(&H, x + o, yl + o, yr + o, m);
	}
	const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
	for (long i = 0; i < n; ++i) pk = fmax(pk, fmax(fabs(yl[i]), fabs(yr[i])));
	printf("pior caso (Repeticoes 100, tudo no maximo, Tempo varrendo):\n");
	printf("    pico %.1f dBFS, NaN: %s, CPU %.2f%% de um nucleo\n", db(pk), bad ? "SIM" : "nao", 100 * cpu / 20.0);
	check(!bad, "sem NaN/Inf");
	check(pk < 4.0, "saida limitada (saturacao segura o auto-oscilar)");
	d->cleanup(H.h);
	free(x);
	free(yl);
	free(yr);
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "uso: %s plugin.so echo|stable|in.f32 [out.f32 params...]\n", argv[0]);
		return 1;
	}
	void* lib = dlopen(argv[1], RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	const LV2_Descriptor* d = ((LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor"))(0);

	if (!strcmp(argv[2], "echo")) { test_echo(d); return failures != 0; }
	if (!strcmp(argv[2], "stable")) { test_stable(d); return failures != 0; }

	FILE* f = fopen(argv[2], "rb");
	if (!f) { perror(argv[2]); return 1; }
	fseek(f, 0, SEEK_END);
	const long n_in = ftell(f) / 4;
	rewind(f);
	const long n = n_in + (long)(SR * 6); /* let the echoes ring out */
	float* x = calloc(n, 4);
	float* yl = malloc(4 * n);
	float* yr = malloc(4 * n);
	if (fread(x, 4, n_in, f) != (size_t)n_in) return 1;
	fclose(f);

	Host H;
	host_init(&H, d);
	for (int i = 0; i < NCTL && 4 + i < argc; ++i) H.ctl[i] = (float)atof(argv[4 + i]);
	const int bad = host_run(&H, x, yl, yr, n);
	double pk_in = 0, pk = 0;
	for (long i = 0; i < n; ++i) {
		pk_in = fmax(pk_in, fabs(x[i]));
		pk = fmax(pk, fmax(fabs(yl[i]), fabs(yr[i])));
	}
	printf("pico entrada %.1f dBFS, pico saida %.1f dBFS, NaN %s\n", db(pk_in), db(pk), bad ? "SIM" : "nao");
	if (argc > 3 && strcmp(argv[3], "-")) {
		FILE* o = fopen(argv[3], "wb");
		for (long i = 0; i < n; ++i) {
			fwrite(&yl[i], 4, 1, o);
			fwrite(&yr[i], 4, 1, o);
		}
		fclose(o);
	}
	return 0;
}
