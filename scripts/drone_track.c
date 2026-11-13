/* Drone tracker smoke test for RK3566 NPU (librknnrt).
 *
 * Reads 640x640 RGB24 frames from stdin (rawvideo), runs the RKNN YOLO model,
 * does a greedy NMS, keeps the "target" with a nearest-centre association and
 * prints one CSV line per frame:
 *
 *   frame,read_ms,infer_ms,post_ms,ndet,best_conf,cx,cy,w,h,tracked
 *
 * read_ms is the blocking read() of one frame — it includes whatever the
 * producer (ffmpeg) has not decoded yet, so the end-to-end figure must come
 * from wall-clock of the whole pipeline, not from this column.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include "rknn_api.h"

#define NC 1
#define MAXBOX 256

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct { float x, y, w, h, conf; } box_t;

static int cmp_conf(const void *a, const void *b)
{
	float ca = ((const box_t *)a)->conf, cb = ((const box_t *)b)->conf;
	return (ca < cb) - (ca > cb);
}

static float iou(const box_t *a, const box_t *b)
{
	float ax1 = a->x - a->w / 2, ay1 = a->y - a->h / 2, ax2 = a->x + a->w / 2, ay2 = a->y + a->h / 2;
	float bx1 = b->x - b->w / 2, by1 = b->y - b->h / 2, bx2 = b->x + b->w / 2, by2 = b->y + b->h / 2;
	float ix = fmaxf(0, fminf(ax2, bx2) - fmaxf(ax1, bx1));
	float iy = fmaxf(0, fminf(ay2, by2) - fmaxf(ay1, by1));
	float inter = ix * iy, uni = a->w * a->h + b->w * b->h - inter;
	return uni > 0 ? inter / uni : 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s model.rknn [conf_thres]\n", argv[0]);
		return 2;
	}
	float conf_thres = argc > 2 ? atof(argv[2]) : 0.25f;

	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("model"); return 2; }
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	void *blob = malloc(sz);
	if (fread(blob, 1, sz, f) != (size_t)sz) return 2;
	fclose(f);

	rknn_context ctx;
	if (rknn_init(&ctx, blob, sz, 0, NULL) < 0) {
		fprintf(stderr, "rknn_init failed\n");
		return 1;
	}
	rknn_input_output_num ion;
	rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion));

	/* диагностика выходов */
	fprintf(stderr, "outputs: %u\n", ion.n_output);
	for (uint32_t i = 0; i < ion.n_output; i++) {
		rknn_tensor_attr a;
		memset(&a, 0, sizeof(a));
		a.index = i;
		if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) == 0) {
			fprintf(stderr, "  out%u dims=[", i);
			for (uint32_t d = 0; d < a.n_dims; d++)
				fprintf(stderr, "%d%s", a.dims[d], d + 1 < a.n_dims ? "," : "");
			fprintf(stderr, "] fmt=%d type=%d\n", a.fmt, a.type);
		}
	}

	int W = getenv("SIZE") ? atoi(getenv("SIZE")) : 640;
	int H = W;
	unsigned char *frame = malloc((size_t)W * H * 3);
	box_t boxes[MAXBOX], track = {0};
	int have_track = 0, nframes = 0, det_frames = 0;
	double sum_infer = 0, sum_post = 0, sum_read = 0;
	double t_start = now_ms();

	printf("frame,read_ms,infer_ms,post_ms,ndet,best_conf,cx,cy,w,h,tracked\n");

	for (;;) {
		double t0 = now_ms();
		size_t got = 0;
		size_t fsz = (size_t)W * H * 3;
		while (got < fsz) {
			size_t n = fread(frame + got, 1, fsz - got, stdin);
			if (n == 0) {
				if (got == 0) goto done;
				break;
			}
			got += n;
		}
		if (got < fsz) goto done;   /* last partial frame */
		double t1 = now_ms();

		rknn_input in;
		memset(&in, 0, sizeof(in));
		in.index = 0;
		in.type = RKNN_TENSOR_UINT8;
		in.fmt = RKNN_TENSOR_NHWC;
		in.size = fsz;
		in.buf = frame;
		if (rknn_inputs_set(ctx, 1, &in) < 0) {
			fprintf(stderr, "inputs_set failed at frame %d\n", nframes);
			return 1;
		}
		double t2 = now_ms();
		if (rknn_run(ctx, NULL) < 0) {
			fprintf(stderr, "rknn_run failed at frame %d\n", nframes);
			return 1;
		}
		double t3 = now_ms();

		rknn_output outs[8];
		memset(outs, 0, sizeof(outs));
		int want_float = getenv("NOFLOAT") ? 0 : 1;
		for (uint32_t i = 0; i < ion.n_output && i < 8; i++) {
			outs[i].index = i;
			outs[i].want_float = want_float;
		}
		if (rknn_outputs_get(ctx, ion.n_output, outs, NULL) < 0) {
			fprintf(stderr, "outputs_get failed at frame %d\n", nframes);
			return 1;
		}

		/* YOLOv8 head: one tensor [1, 4+nc, N] (or [1, N, 4+nc]); detect layout */
		float *data = NULL;
		int attrs = 0, anchors = 0;
		for (uint32_t i = 0; i < ion.n_output; i++) {
			int elems = outs[i].size / sizeof(float);
			rknn_tensor_attr a;
			memset(&a, 0, sizeof(a));
			a.index = i;
			if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) != 0)
				continue;
			int dims = 1;
			for (uint32_t d = 0; d < a.n_dims; d++) dims *= a.dims[d];
			if (dims == elems && elems > 100) {
				data = (float *)outs[i].buf;
				attrs = (int)a.dims[a.n_dims - 2];
				anchors = (int)a.dims[a.n_dims - 1];
				/* [1, 4+nc, N]: attrs small, anchors big -> transpose on read */
				if (attrs > anchors) {
					int tmp = attrs;
					attrs = anchors;
					anchors = tmp;
				}
			}
		}

		if (getenv("DBG") && data) {
			float mn = 1e9f, mx = -1e9f;
			for (int k = 0; k < attrs * anchors; k++) {
				float v = data[k];
				if (v < mn) mn = v;
				if (v > mx) mx = v;
			}
			float confmax = 0;
			float chmax[5] = {0};
			for (int c = 0; c < attrs && c < 5; c++)
				for (int k = 0; k < anchors; k++) {
					float v = data[c * anchors + k];
					if (v > chmax[c]) chmax[c] = v;
				}
			for (int k = 0; k < anchors; k++) {
				float c = data[4 * anchors + k];
				if (c > confmax) confmax = c;
			}
			fprintf(stderr, "[dbg] chmax: %.3f %.3f %.3f %.3f %.5f\n",
				chmax[0], chmax[1], chmax[2], chmax[3], chmax[4]);
			fprintf(stderr, "[dbg] attrs=%d anchors=%d min=%.4f max=%.4f confmax=%.5f\n",
				attrs, anchors, mn, mx, confmax);
		}

		/* два выхода: boxes [1,4,N] + scores [1,1,N] */
		float *bp = NULL, *sp = NULL; int bch = 0, bn = 0, sn = 0;
		if (ion.n_output == 2) {
			rknn_tensor_attr a;
			int ch[2] = {0}, nn[2] = {0};
			float *ptr[2] = {NULL, NULL};
			for (uint32_t i = 0; i < 2; i++) {
				memset(&a, 0, sizeof(a)); a.index = i;
				if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a))) continue;
				ptr[i] = (float *)outs[i].buf;
				ch[i] = (int)a.dims[a.n_dims - 2];
				nn[i] = (int)a.dims[a.n_dims - 1];
				if (ch[i] > nn[i]) { int t = ch[i]; ch[i] = nn[i]; nn[i] = t; }
			}
			bp = ptr[0]; bch = ch[0]; bn = nn[0];
			sp = ptr[1]; sn = nn[1];
		}

		int ndet = 0;
		float bx_best = 0;
		if (bp && sp && bch >= 4) {
			for (int k = 0; k < sn && k < bn; k++) {
				float c = sp[k];
				if (c < conf_thres) continue;
				boxes[ndet].x = bp[0 * bn + k];
				boxes[ndet].y = bp[1 * bn + k];
				boxes[ndet].w = bp[2 * bn + k];
				boxes[ndet].h = bp[3 * bn + k];
				boxes[ndet].conf = c;
				if (c > bx_best) bx_best = c;
				ndet++;
				if (ndet >= MAXBOX) break;
			}
		}

		if (!want_float) {
			if (rknn_outputs_release(ctx, ion.n_output, outs) < 0) return 1;
			double t4 = now_ms();
			sum_read += t1 - t0; sum_infer += t3 - t2; sum_post += t4 - t3;
			nframes++;
			printf("%d,%.2f,%.2f,%.2f,0,0,0,0,0,0,0\n", nframes, t1 - t0, t3 - t2, t4 - t3);
			continue;
		}
		float best = 0;
		if (data && attrs >= 4 + NC) {
			int n = attrs * anchors;
			int transposed = (attrs < anchors);
			for (int k = 0; k < anchors && ndet < MAXBOX; k++) {
				float conf = transposed ? data[(4 + 0) * anchors + k] : data[k * attrs + 4];
				if (conf < conf_thres) continue;
				float cx = transposed ? data[0 * anchors + k] : data[k * attrs + 0];
				float cy = transposed ? data[1 * anchors + k] : data[k * attrs + 1];
				float w = transposed ? data[2 * anchors + k] : data[k * attrs + 2];
				float h = transposed ? data[3 * anchors + k] : data[k * attrs + 3];
				boxes[ndet].x = cx; boxes[ndet].y = cy;
				boxes[ndet].w = w; boxes[ndet].h = h;
				boxes[ndet].conf = conf;
				if (conf > best) best = conf;
				ndet++;
			}
			(void)n;
			qsort(boxes, ndet, sizeof(box_t), cmp_conf);
			int keep = 0;
			for (int i = 0; i < ndet; i++) {
				int dup = 0;
				for (int j = 0; j < keep; j++)
					if (iou(&boxes[i], &boxes[j]) > 0.45f) { dup = 1; break; }
				if (!dup) boxes[keep++] = boxes[i];
			}
			ndet = keep;
		}

		/* nearest-centre association: keeps the same target across frames */
		int tracked = 0;
		if (ndet > 0) {
			int pick = 0;
			if (have_track) {
				float bestd = 1e9f;
				for (int i = 0; i < ndet; i++) {
					float dx = boxes[i].x - track.x, dy = boxes[i].y - track.y;
					float d = dx * dx + dy * dy;
					if (d < bestd) { bestd = d; pick = i; }
				}
				if (bestd > 200.0f * 200.0f) pick = 0;   /* too far -> re-acquire on best conf */
			}
			track = boxes[pick];
			have_track = 1;
			tracked = 1;
			det_frames++;
		}

		if (rknn_outputs_release(ctx, ion.n_output, outs) < 0) return 1;

		double t4 = now_ms();
		sum_read += t1 - t0;
		sum_infer += t3 - t2;
		sum_post += t4 - t3;
		nframes++;

		printf("%d,%.2f,%.2f,%.2f,%d,%.3f,%.1f,%.1f,%.1f,%.1f,%d\n",
		       nframes, t1 - t0, t3 - t2, t4 - t3, ndet, best,
		       track.x, track.y, track.w, track.h, tracked);
	}

done:;
	double wall = now_ms() - t_start;
	rknn_destroy(ctx);
	free(blob);
	free(frame);

	fprintf(stderr, "\n=== итог ===\n");
	fprintf(stderr, "кадров: %d (с детекцией: %d, %.1f%%)\n", nframes, det_frames,
		nframes ? 100.0 * det_frames / nframes : 0.0);
	fprintf(stderr, "NPU: %.2f мс/кадр (%.1f FPS по NPU)\n",
		nframes ? sum_infer / nframes : 0, sum_infer > 0 ? 1000.0 * nframes / sum_infer : 0);
	fprintf(stderr, "постобработка+чтение: %.2f + %.2f мс/кадр\n",
		nframes ? sum_post / nframes : 0, nframes ? sum_read / nframes : 0);
	fprintf(stderr, "всего по стенду (вкл. ожидание декодера): %.1f с → %.1f FPS\n",
		wall / 1000.0, wall > 0 ? 1000.0 * nframes / wall : 0);
	return 0;
}
