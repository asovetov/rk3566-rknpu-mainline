/* Каскадный трекер дрона на RK3566: поиск по полному кадру + сопровождение по кропу.
 *
 * Почему так: цель 10-15 px в 1080p. Модель 640 видит её только если кроп вокруг
 * цели (1:1), модель 1280 видит по всему кадру, но стоит 300 мс. Поэтому:
 *   TRACK  — кроп 640 вокруг предсказанной позиции (77 мс), ведём по скорости;
 *   SEARCH — полный кадр, letterbox 1280 (300 мс), пока не найдём.
 * Переход TRACK->SEARCH после MISS_LIMIT промахов подряд, SEARCH->TRACK по первому
 * попаданию. Позиция предсказывается по скорости (dead reckoning), чтобы окно
 * успевало за целью на рывках.
 *
 * Вход: rawvideo RGB24 кадрами FWxFH в stdin. Выход CSV:
 *   frame,state,ms,conf,cx,cy,w,h
 * где state: T=track, S=search, L=потерян (в этом кадре цели нет).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "rknn_api.h"

#define ROI 640
#define SRCH 1280
#define MAXBOX 128
#define MISS_LIMIT 2

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct { float x, y, w, h, conf; } box_t;
typedef struct { rknn_context ctx; int nout; int in_w, in_h; const char *name; } model_t;

static int load_model(model_t *m, const char *path, int w, int h)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return -1; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	void *blob = malloc(sz);
	if (fread(blob, 1, sz, f) != (size_t)sz) { fclose(f); return -1; }
	fclose(f);
	if (rknn_init(&m->ctx, blob, sz, 0, NULL) < 0) return -1;
	rknn_input_output_num ion;
	rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion));
	m->nout = (int)ion.n_output;
	m->in_w = w; m->in_h = h; m->name = path;
	free(blob);
	return 0;
}

/* лучший бокс из двух выходов (boxes [1,4,N] + scores [1,1,N]) */
static int infer(model_t *m, unsigned char *buf, float thr, box_t *out)
{
	rknn_input in;
	memset(&in, 0, sizeof(in));
	in.index = 0;
	in.type = RKNN_TENSOR_UINT8;
	in.fmt = RKNN_TENSOR_NHWC;
	in.size = (uint32_t)m->in_w * m->in_h * 3;
	in.buf = buf;
	if (rknn_inputs_set(m->ctx, 1, &in) < 0) return -1;
	if (rknn_run(m->ctx, NULL) < 0) return -1;

	rknn_output outs[8];
	memset(outs, 0, sizeof(outs));
	for (int i = 0; i < m->nout && i < 8; i++) { outs[i].index = i; outs[i].want_float = 1; }
	if (rknn_outputs_get(m->ctx, m->nout, outs, NULL) < 0) return -1;

	float *bp = NULL, *sp = NULL;
	int bch = 0, bn = 0, sn = 0;
	if (m->nout == 2) {
		rknn_tensor_attr a;
		int ch[2] = {0}, nn[2] = {0};
		float *ptr[2] = {NULL, NULL};
		for (int i = 0; i < 2; i++) {
			memset(&a, 0, sizeof(a)); a.index = i;
			if (rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a))) continue;
			ptr[i] = (float *)outs[i].buf;
			ch[i] = (int)a.dims[a.n_dims - 2];
			nn[i] = (int)a.dims[a.n_dims - 1];
			if (ch[i] > nn[i]) { int t = ch[i]; ch[i] = nn[i]; nn[i] = t; }
		}
		bp = ptr[0]; bch = ch[0]; bn = nn[0];
		sp = ptr[1]; sn = nn[1];
	}
	out->conf = 0; out->x = out->y = out->w = out->h = 0;
	if (bp && sp && bch >= 4) {
		for (int k = 0; k < sn && k < bn; k++) {
			float c = sp[k];
			if (c <= out->conf || c < thr) continue;
			out->conf = c;
			out->x = bp[0 * bn + k];
			out->y = bp[1 * bn + k];
			out->w = bp[2 * bn + k];
			out->h = bp[3 * bn + k];
		}
	}
	rknn_outputs_release(m->ctx, m->nout, outs);
	return 0;
}

/* кроп 1:1 (без масштабирования) */
static void crop_copy(const unsigned char *src, int fw, unsigned char *dst, int rx, int ry)
{
	unsigned char *d = dst;
	for (int y = 0; y < ROI; y++) {
		const unsigned char *row = src + (size_t)(ry + y) * fw * 3;
		memcpy(d, row + (size_t)rx * 3, (size_t)ROI * 3);
		d += (size_t)ROI * 3;
	}
}

/* letterbox: масштаб с сохранением пропорций + серые поля (114), как в ultralytics */
static void letterbox(const unsigned char *src, int fw, int fh, unsigned char *dst,
		      int *pad_x, int *pad_y, float *scale)
{
	float s = fminf((float)SRCH / fw, (float)SRCH / fh);
	int nw = (int)(fw * s), nh = (int)(fh * s);
	int px = (SRCH - nw) / 2, py = (SRCH - nh) / 2;
	*pad_x = px; *pad_y = py; *scale = s;
	memset(dst, 114, (size_t)SRCH * SRCH * 3);
	for (int y = 0; y < nh; y++) {
		int sy = (int)(y / s);
		if (sy >= fh) sy = fh - 1;
		const unsigned char *srow = src + (size_t)sy * fw * 3;
		unsigned char *drow = dst + (size_t)(py + y) * SRCH * 3 + (size_t)px * 3;
		for (int x = 0; x < nw; x++) {
			int sx = (int)(x / s);
			if (sx >= fw) sx = fw - 1;
			drow[x * 3 + 0] = srow[sx * 3 + 0];
			drow[x * 3 + 1] = srow[sx * 3 + 1];
			drow[x * 3 + 2] = srow[sx * 3 + 2];
		}
	}
}


int main(int argc, char **argv)
{
	const char *mp = argc > 1 ? argv[1] : "/tmp/drone_split_int8.rknn";
	float thr = argc > 2 ? atof(argv[2]) : 0.2f;
	int FW = argc > 3 ? atoi(argv[3]) : 1920;
	int FH = argc > 4 ? atoi(argv[4]) : 1080;

	model_t m;
	if (load_model(&m, mp, ROI, ROI) < 0) return 1;
	unsigned char *frame = malloc((size_t)FW * FH * 3);
	unsigned char *buf = malloc((size_t)ROI * ROI * 3);
	if (!frame || !buf) return 1;

	int gx_n = (FW + ROI - 1) / ROI, gy_n = (FH + ROI - 1) / ROI;
	int nfr = 0, any = 0;
	double t0 = now_ms(), sum = 0;
	printf("frame,best_conf,best_cx,best_cy,best_w,best_h\n");
	for (;;) {
		size_t got = 0, need = (size_t)FW * FH * 3;
		while (got < need) {
			size_t n = fread(frame + got, 1, need - got, stdin);
			if (!n) { if (!got) goto out; break; }
			got += n;
		}
		if (got < need) goto out;
		float bc = 0, bx = 0, by = 0, bw = 0, bh = 0;
		double f0 = now_ms();
		for (int gy = 0; gy < gy_n; gy++) {
			for (int gx = 0; gx < gx_n; gx++) {
				int rx = gx * ROI, ry = gy * ROI;
				if (rx > FW - ROI) rx = FW - ROI;
				if (ry > FH - ROI) ry = FH - ROI;
				crop_copy(frame, FW, buf, rx, ry);
				box_t b;
				if (infer(&m, buf, thr, &b) < 0) return 1;
				if (b.conf > bc) { bc = b.conf; bx = rx + b.x; by = ry + b.y; bw = b.w; bh = b.h; }
			}
		}
		sum += now_ms() - f0;
		nfr++;
		if (bc >= thr) any++;
		printf("%d,%.3f,%.1f,%.1f,%.1f,%.1f\n", nfr, bc, bx, by, bw, bh);
	}
out:;
	rknn_destroy(m.ctx);
	double wall = now_ms() - t0;
	fprintf(stderr, "\nкадров: %d, хотя бы одно попадание в кадре: %d (%.1f%%)\n", nfr, any, nfr ? 100.0*any/nfr : 0);
	fprintf(stderr, "6 кропов на кадр: %.1f мс/кадр (%.1f FPS)\n", nfr ? sum/nfr : 0, sum ? 1000.0*nfr/sum : 0);
	fprintf(stderr, "весь стенд: %.1f с\n", wall/1000.0);
	return 0;
}
