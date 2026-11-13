/* ROI-трекер для RK3566: детекция 640-моделью по кропу 1:1 вокруг цели.
 *
 * Зачем: цель в контрольном видео 10-15 px в 1080p. При ресайзе всего кадра в
 * 640 она превращается в 4-5 px и детектор её не видит (проверено на float
 * ONNX). Кроп 640x640 из 1920x1080 идёт в модель 1:1 — масштаб цели сохраняется,
 * а стоимость инференса остаётся 640-й (~80 мс на RK3566 вместо 300-1200 мс на
 * 1280).
 *
 * Вход: rawvideo RGB24 кадрами FWxFH (по умолчанию 1920x1080) в stdin.
 * Выход: CSV на кадр — frame,ms,conf,cx,cy,w,h,roi_x,roi_y,found
 * Координаты — в пикселях полного кадра.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "rknn_api.h"

#define ROI 640
#define MAXBOX 64

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct { float x, y, w, h, conf; } box_t;

/* ближайший сосед: для детекции достаточно, а по времени дешевле */
static void crop_resize(const unsigned char *src, int fw, int fh,
			const unsigned char *dst, int rx, int ry)
{
	(void)fh;
	unsigned char *d = (unsigned char *)dst;
	for (int y = 0; y < ROI; y++) {
		int sy = ry + y;
		const unsigned char *row = src + (size_t)sy * fw * 3;
		for (int x = 0; x < ROI; x++) {
			int sx = rx + x;
			const unsigned char *p = row + (size_t)sx * 3;
			*d++ = p[0]; *d++ = p[1]; *d++ = p[2];
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s model.rknn [conf] [frame_w] [frame_h]\n", argv[0]);
		return 2;
	}
	float conf_thres = argc > 2 ? atof(argv[2]) : 0.25f;
	int FW = argc > 3 ? atoi(argv[3]) : 1920;
	int FH = argc > 4 ? atoi(argv[4]) : 1080;

	FILE *fm = fopen(argv[1], "rb");
	if (!fm) { perror("model"); return 2; }
	fseek(fm, 0, SEEK_END); long msz = ftell(fm); fseek(fm, 0, SEEK_SET);
	void *blob = malloc(msz);
	if (fread(blob, 1, msz, fm) != (size_t)msz) return 2;
	fclose(fm);

	rknn_context ctx;
	if (rknn_init(&ctx, blob, msz, 0, NULL) < 0) {
		fprintf(stderr, "rknn_init failed\n");
		return 1;
	}
	rknn_input_output_num ion;
	rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion));

	size_t fsz = (size_t)FW * FH * 3;
	unsigned char *frame = malloc(fsz);
	unsigned char *roi = malloc((size_t)ROI * ROI * 3);
	if (!frame || !roi) return 1;

	int rx = (FW - ROI) / 2, ry = (FH - ROI) / 2;   /* старт: центр кадра */
	/* сетка поиска при потере: 640x640 окна, покрывающие кадр целиком */
	int scan = 0;
	int gx_n = (FW + ROI - 1) / ROI, gy_n = (FH + ROI - 1) / ROI;
	float last_cx = -1, last_cy = -1;
	int nframes = 0, found_frames = 0;
	double sum_infer = 0, t_start = now_ms();

	printf("frame,ms,conf,cx,cy,w,h,roi_x,roi_y,found\n");
	for (;;) {
		size_t got = 0;
		while (got < fsz) {
			size_t n = fread(frame + got, 1, fsz - got, stdin);
			if (!n) { if (!got) goto done; break; }
			got += n;
		}
		if (got < fsz) goto done;

		if (rx < 0) rx = 0;
		if (ry < 0) ry = 0;
		if (rx > FW - ROI) rx = FW - ROI;
		if (ry > FH - ROI) ry = FH - ROI;
		crop_resize(frame, FW, FH, roi, rx, ry);

		rknn_input in;
		memset(&in, 0, sizeof(in));
		in.index = 0;
		in.type = RKNN_TENSOR_UINT8;
		in.fmt = RKNN_TENSOR_NHWC;
		in.size = (uint32_t)ROI * ROI * 3;
		in.buf = roi;
		if (rknn_inputs_set(ctx, 1, &in) < 0) return 1;

		double t0 = now_ms();
		if (rknn_run(ctx, NULL) < 0) return 1;
		double infer = now_ms() - t0;

		rknn_output outs[8];
		memset(outs, 0, sizeof(outs));
		for (uint32_t i = 0; i < ion.n_output && i < 8; i++) {
			outs[i].index = i;
			outs[i].want_float = 1;
		}
		if (rknn_outputs_get(ctx, ion.n_output, outs, NULL) < 0) return 1;

		float *data = NULL;
		int attrs = 0, anchors = 0;
		/* два выхода (boxes / scores) — int8-модель со разделённой головой */
		float *boxes_p = NULL, *scores_p = NULL;
		int bx_ch = 0, bx_n = 0, sc_n = 0;
		for (uint32_t i = 0; i < ion.n_output; i++) {
			rknn_tensor_attr a;
			memset(&a, 0, sizeof(a));
			a.index = i;
			if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a))) continue;
			int dims = 1;
			for (uint32_t d = 0; d < a.n_dims; d++) dims *= a.dims[d];
			if (dims == (int)(outs[i].size / sizeof(float)) && dims > 100) {
				data = (float *)outs[i].buf;
				attrs = (int)a.dims[a.n_dims - 2];
				anchors = (int)a.dims[a.n_dims - 1];
				if (attrs > anchors) { int t = attrs; attrs = anchors; anchors = t; }
			}
		}

		if (ion.n_output == 2) {
			rknn_tensor_attr a;
			int ch[2] = {0}, n[2] = {0};
			float *ptr[2] = {NULL, NULL};
			for (uint32_t i = 0; i < 2; i++) {
				memset(&a, 0, sizeof(a));
				a.index = i;
				if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a))) continue;
				ptr[i] = (float *)outs[i].buf;
				ch[i] = (int)a.dims[a.n_dims - 2];
				n[i] = (int)a.dims[a.n_dims - 1];
				if (ch[i] > n[i]) { int t = ch[i]; ch[i] = n[i]; n[i] = t; }
			}
			boxes_p = ptr[0]; bx_ch = ch[0]; bx_n = n[0];
			scores_p = ptr[1]; sc_n = n[1];
		}

		float best = 0;
		box_t pick = {0};
		if (boxes_p && scores_p && bx_ch >= 4) {
			for (int k = 0; k < sc_n && k < bx_n; k++) {
				float c = scores_p[k];
				if (c <= best || c < conf_thres) continue;
				best = c;
				pick.x = boxes_p[0 * bx_n + k];
				pick.y = boxes_p[1 * bx_n + k];
				pick.w = boxes_p[2 * bx_n + k];
				pick.h = boxes_p[3 * bx_n + k];
			}
			pick.conf = best;
		} else if (data && attrs >= 5) {
			int transposed = (attrs < anchors);
			for (int k = 0; k < anchors; k++) {
				float c = transposed ? data[4 * anchors + k] : data[k * attrs + 4];
				if (c <= best || c < conf_thres) continue;
				best = c;
				pick.x = transposed ? data[0 * anchors + k] : data[k * attrs + 0];
				pick.y = transposed ? data[1 * anchors + k] : data[k * attrs + 1];
				pick.w = transposed ? data[2 * anchors + k] : data[k * attrs + 2];
				pick.h = transposed ? data[3 * anchors + k] : data[k * attrs + 3];
			}
			pick.conf = best;
		}
		rknn_outputs_release(ctx, ion.n_output, outs);

		int found = best >= conf_thres;
		if (found) {
			/* координаты кропа -> координаты кадра */
			float gx = rx + pick.x, gy = ry + pick.y;
			last_cx = gx; last_cy = gy;
			/* окно следит за целью, но не уезжает за края */
			rx = (int)(gx - ROI / 2);
			ry = (int)(gy - ROI / 2);
			found_frames++;
			printf("%d,%.2f,%.3f,%.1f,%.1f,%.1f,%.1f,%d,%d,1\n",
			       ++nframes, infer, best, gx, gy, pick.w, pick.h, rx, ry);
		} else {
			/* не нашли — сразу переходим к следующему окну сетки */
			printf("%d,%.2f,%.3f,%.1f,%.1f,0,0,%d,%d,0\n",
			       ++nframes, infer, best, last_cx, last_cy, rx, ry);
			scan++;
			rx = (scan % gx_n) * ROI;
			ry = ((scan / gx_n) % gy_n) * ROI;
		}
		sum_infer += infer;
	}

done:;
	double wall = now_ms() - t_start;
	rknn_destroy(ctx);
	free(blob); free(frame); free(roi);
	fprintf(stderr, "\n=== итог ===\n");
	fprintf(stderr, "кадров: %d, с целью: %d (%.1f%%)\n", nframes, found_frames,
		nframes ? 100.0 * found_frames / nframes : 0);
	fprintf(stderr, "NPU: %.2f мс/кадр (%.1f FPS)\n",
		nframes ? sum_infer / nframes : 0, sum_infer > 0 ? 1000.0 * nframes / sum_infer : 0);
	fprintf(stderr, "весь стенд: %.1f с → %.1f FPS\n", wall / 1000.0,
		wall > 0 ? 1000.0 * nframes / wall : 0);
	return 0;
}
