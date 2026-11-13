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
static int CROP = 640;   /* размер кропа до масштабирования (env CROP) */
#define SRCH 1280
#define MAXBOX 128
#define MISS_LIMIT 2
#define COAST_LIMIT 12

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

/* кроп CROPxCROP из кадра, приведённый к ROIxROI (1:1 при CROP=ROI, иначе nearest-апскейл) */
static void crop_copy(const unsigned char *src, int fw, unsigned char *dst, int rx, int ry)
{
	int cs = CROP;
	for (int y = 0; y < ROI; y++) {
		int sy = ry + y * cs / ROI;
		const unsigned char *row = src + (size_t)sy * fw * 3;
		unsigned char *d = dst + (size_t)y * ROI * 3;
		for (int x = 0; x < ROI; x++) {
			int sx = rx + x * cs / ROI;
			d[x * 3 + 0] = row[sx * 3 + 0];
			d[x * 3 + 1] = row[sx * 3 + 1];
			d[x * 3 + 2] = row[sx * 3 + 2];
		}
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
	if (argc < 3) {
		fprintf(stderr, "usage: %s roi.rknn search.rknn [conf] [fw] [fh]\n", argv[0]);
		return 2;
	}
	float thr = argc > 3 ? atof(argv[3]) : 0.2f;
	int FW = argc > 4 ? atoi(argv[4]) : 1920;
	int FH = argc > 5 ? atoi(argv[5]) : 1080;
	if (getenv("CROP")) {
		CROP = atoi(getenv("CROP"));
		if (CROP < 64) CROP = 64;
		if (CROP > FW) CROP = FW;
	}

	model_t roi_m, srch_m;
	if (load_model(&roi_m, argv[1], ROI, ROI) < 0) return 1;
	if (load_model(&srch_m, argv[2], SRCH, SRCH) < 0) return 1;
	fprintf(stderr, "roi: %s (nout=%d), search: %s (nout=%d)\n",
		roi_m.name, roi_m.nout, srch_m.name, srch_m.nout);

	size_t fsz = (size_t)FW * FH * 3;
	unsigned char *frame = malloc(fsz);
	unsigned char *roi_buf = malloc((size_t)ROI * ROI * 3);
	unsigned char *srch_buf = malloc((size_t)SRCH * SRCH * 3);
	if (!frame || !roi_buf || !srch_buf) return 1;

	int tracking = 0, miss = 0;
	float cx = 0, cy = 0, vx = 0, vy = 0;
	float last_w = 0, last_h = 0;
	int nframes = 0, tracked_frames = 0, recovered = 0;
	double sum_ms = 0, t_start = now_ms();

	printf("frame,state,ms,conf,cx,cy,w,h\n");
	for (;;) {
		size_t got = 0;
		while (got < fsz) {
			size_t n = fread(frame + got, 1, fsz - got, stdin);
			if (!n) { if (!got) goto done; break; }
			got += n;
		}
		if (got < fsz) goto done;

		double t0 = now_ms();
		box_t b = {0};
		char state;

		if (tracking) {
			/* предсказание по скорости, затем кроп вокруг него */
			float px = cx + vx, py = cy + vy;
			int rx = (int)(px - CROP / 2.0f), ry = (int)(py - CROP / 2.0f);
			if (rx < 0) rx = 0;
			if (ry < 0) ry = 0;
			if (rx > FW - CROP) rx = FW - CROP;
			if (ry > FH - CROP) ry = FH - CROP;
			crop_copy(frame, FW, roi_buf, rx, ry);
			if (infer(&roi_m, roi_buf, thr, &b) < 0) return 1;
			if (b.conf >= thr) {
				float gx = rx + b.x * CROP / (float)ROI;
				float gy = ry + b.y * CROP / (float)ROI;
				vx = 0.6f * (gx - cx) + 0.4f * vx;
				vy = 0.6f * (gy - cy) + 0.4f * vy;
				cx = gx; cy = gy;
				last_w = b.w * CROP / (float)ROI;
				last_h = b.h * CROP / (float)ROI;
				miss = 0;
				state = 'T';
				tracked_frames++;
			} else {
				state = 'L';
				miss++;
				/* продолжаем по траектории: цель летит, окно едет за ней */
				cx += vx; cy += vy;
				vx *= 0.92f; vy *= 0.92f;
				if (miss >= MISS_LIMIT && miss < COAST_LIMIT) {
					/* ещё в режиме слежения по предсказанию */
				} else if (miss >= COAST_LIMIT) {
					tracking = 0;
				}
			}
		} else {
			/* поиск: сначала окно в предсказанной точке, потом сетка */
			int pad_x, pad_y;
			float scale;
			letterbox(frame, FW, FH, srch_buf, &pad_x, &pad_y, &scale);
			if (infer(&srch_m, srch_buf, thr, &b) < 0) return 1;
			if (b.conf >= thr) {
				cx = (b.x - pad_x) / scale;
				cy = (b.y - pad_y) / scale;
				last_w = b.w / scale; last_h = b.h / scale;
				tracking = 1; miss = 0; vx = vy = 0;
				state = 'S';       /* найдено в режиме поиска */
				recovered++;
				tracked_frames++;
			} else {
				state = 'L';
			}
		}

		double ms = now_ms() - t0;
		sum_ms += ms;
		nframes++;
		printf("%d,%c,%.2f,%.3f,%.1f,%.1f,%.1f,%.1f\n",
		       nframes, state, ms, b.conf, cx, cy, last_w, last_h);
	}

done:;
	double wall = now_ms() - t_start;
	rknn_destroy(roi_m.ctx);
	rknn_destroy(srch_m.ctx);
	free(frame); free(roi_buf); free(srch_buf);
	fprintf(stderr, "\n=== итог ===\n");
	fprintf(stderr, "кадров: %d, с целью: %d (%.1f%%), перезахватов: %d\n",
		nframes, tracked_frames, nframes ? 100.0 * tracked_frames / nframes : 0, recovered);
	fprintf(stderr, "среднее на кадр: %.1f мс (%.1f FPS)\n",
		nframes ? sum_ms / nframes : 0, sum_ms > 0 ? 1000.0 * nframes / sum_ms : 0);
	fprintf(stderr, "весь стенд: %.1f с → %.2f FPS\n", wall / 1000.0,
		wall > 0 ? 1000.0 * nframes / wall : 0);
	return 0;
}
