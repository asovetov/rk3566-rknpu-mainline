/* Minimal RKNN runtime smoke test: load a model, run it N times, report latency.
 * Cross-compiled for aarch64 against librknnrt.so; no Python on the target. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rknn_api.h"

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s model.rknn [iters]\n", argv[0]);
		return 2;
	}
	int iters = (argc > 2) ? atoi(argv[2]) : 20;

	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror("open model");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *blob = malloc(sz);
	if (fread(blob, 1, sz, f) != (size_t)sz) {
		fprintf(stderr, "short read\n");
		return 2;
	}
	fclose(f);
	printf("model: %s (%ld bytes)\n", argv[1], sz);

	rknn_context ctx;
	int ret = rknn_init(&ctx, blob, sz, 0, NULL);
	if (ret < 0) {
		fprintf(stderr, "rknn_init failed: %d\n", ret);
		return 1;
	}

	rknn_sdk_version ver;
	if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0)
		printf("librknnrt: %s / driver: %s\n", ver.api_version, ver.drv_version);

	rknn_input_output_num ion;
	if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion)) != 0) {
		fprintf(stderr, "query in/out failed\n");
		return 1;
	}
	printf("inputs: %u outputs: %u\n", ion.n_input, ion.n_output);

	for (uint32_t i = 0; i < ion.n_input; i++) {
		rknn_tensor_attr a;
		memset(&a, 0, sizeof(a));
		a.index = i;
		if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a)) != 0)
			continue;
		printf("  in%u: %s dims=[", i, a.name);
		for (uint32_t d = 0; d < a.n_dims; d++)
			printf("%u%s", a.dims[d], d + 1 < a.n_dims ? "," : "");
		printf("] fmt=%d type=%d\n", a.fmt, a.type);
	}

	/* feed zeros: shape/type correctness is what we are testing, not accuracy */
	rknn_input inputs[8];
	uint32_t n = ion.n_input > 8 ? 8 : ion.n_input;
	size_t buf_sz[8];
	memset(inputs, 0, sizeof(inputs));
	for (uint32_t i = 0; i < n; i++) {
		rknn_tensor_attr a;
		memset(&a, 0, sizeof(a));
		a.index = i;
		rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a));
		/* always feed float32: RKNN converts to the model's native type */
		buf_sz[i] = (size_t)a.n_elems * sizeof(float);
		void *b = calloc(1, buf_sz[i] ? buf_sz[i] : 4);
		inputs[i].index = i;
		inputs[i].type = RKNN_TENSOR_FLOAT32;
		inputs[i].fmt = RKNN_TENSOR_NHWC;
		inputs[i].size = buf_sz[i];
		inputs[i].buf = b;
	}
	ret = rknn_inputs_set(ctx, n, inputs);
	if (ret < 0) {
		fprintf(stderr, "rknn_inputs_set failed: %d\n", ret);
		return 1;
	}

	/* warm-up */
	ret = rknn_run(ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "rknn_run (warmup) failed: %d\n", ret);
		return 1;
	}

	double t0 = now_ms();
	for (int i = 0; i < iters; i++) {
		ret = rknn_run(ctx, NULL);
		if (ret < 0) {
			fprintf(stderr, "rknn_run failed at iter %d: %d\n", i, ret);
			return 1;
		}
	}
	double dt = now_ms() - t0;

	rknn_output outs[8];
	memset(outs, 0, sizeof(outs));
	uint32_t no = ion.n_output > 8 ? 8 : ion.n_output;
	for (uint32_t i = 0; i < no; i++) {
		outs[i].index = i;
		outs[i].want_float = 1;
	}
	if (rknn_outputs_get(ctx, no, outs, NULL) == 0) {
		printf("first output: %u values, first %.6f\n", outs[0].size / 4,
		       outs[0].size >= 4 ? ((float *)outs[0].buf)[0] : 0.0f);
		rknn_outputs_release(ctx, no, outs);
	}

	printf("iters: %d  total: %.1f ms  per-frame: %.2f ms  fps: %.1f\n",
	       iters, dt, dt / iters, 1000.0 * iters / dt);

	rknn_destroy(ctx);
	free(blob);
	return 0;
}
