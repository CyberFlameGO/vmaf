/**
 *
 *  Copyright 2016 Netflix, Inc.
 *
 *     Licensed under the Apache License, Version 2.0 (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "common/alloc.h"
#include "common/file_io.h"
#include "main.h"
#include "iqa/ssim.h"
#include "iqa/convolve.h"
#include "iqa/iqa.h"
#include "iqa/decimate.h"
#include "iqa/math_utils.h"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

// unlike psnr, ssim only works with single precision
typedef float number_t;
#define read_image_b  read_image_b2s
#define read_image_w  read_image_w2s

int compute_ssim(const number_t *ref, const number_t *cmp, int w, int h,
		int ref_stride, int cmp_stride, double *score)
{

	int ret = 1;

    int scale;
    int x,y,src_offset,offset;
    float *ref_f,*cmp_f;
    struct _kernel low_pass;
    struct _kernel window;
    float result = INFINITY;
    double ssim_sum=0.0;
    struct _map_reduce mr;

    /* check stride */
	int stride = ref_stride; /* stride in bytes */
	if (stride != cmp_stride)
	{
		printf("error: for ssim, ref_stride (%d) != dis_stride (%d) bytes.\n", ref_stride, cmp_stride);
		fflush(stdout);
		goto fail_or_end;
	}
	stride /= sizeof(float); /* stride_ in pixels */

	/* specify some default parameters */
	const struct iqa_ssim_args *args = 0; /* 0 for default */
	int gaussian = 1; /* 0 for 8x8 square window, 1 for 11x11 circular-symmetric Gaussian window (default) */

    /* initialize algorithm parameters */
    scale = _max( 1, _round( (float)_min(w,h) / 256.0f ) );
    if (args) {
        if(args->f) {
            scale = args->f;
        }
        mr.map     = _ssim_map;
        mr.reduce  = _ssim_reduce;
        mr.context = (void*)&ssim_sum;
    }
    window.kernel = (float*)g_square_window;
    window.w = window.h = SQUARE_LEN;
    window.normalized = 1;
    window.bnd_opt = KBND_SYMMETRIC;
    if (gaussian) {
        window.kernel = (float*)g_gaussian_window;
        window.w = window.h = GAUSSIAN_LEN;
    }

    /* convert image values to floats, forcing stride = width. */
    ref_f = (float*)malloc(w*h*sizeof(float));
    cmp_f = (float*)malloc(w*h*sizeof(float));
    if (!ref_f || !cmp_f) {
        if (ref_f) free(ref_f);
        if (cmp_f) free(cmp_f);
        printf("error: unable to malloc ref_f or cmp_f.\n");
		fflush(stdout);
		goto fail_or_end;
    }
    for (y=0; y<h; ++y) {
        src_offset = y * stride;
        offset = y * w;
        for (x=0; x<w; ++x, ++offset, ++src_offset) {
            ref_f[offset] = (float)ref[src_offset];
            cmp_f[offset] = (float)cmp[src_offset];
        }
    }

    /* scale the images down if required */
    if (scale > 1) {
        /* generate simple low-pass filter */
        low_pass.kernel = (float*)malloc(scale*scale*sizeof(float));
        if (!low_pass.kernel) {
            free(ref_f);
            free(cmp_f);
            printf("error: unable to malloc low-pass filter kernel.\n");
            fflush(stdout);
            goto fail_or_end;
        }
        low_pass.w = low_pass.h = scale;
        low_pass.normalized = 0;
        low_pass.bnd_opt = KBND_SYMMETRIC;
        for (offset=0; offset<scale*scale; ++offset)
            low_pass.kernel[offset] = 1.0f/(scale*scale);

        /* resample */
        if (_iqa_decimate(ref_f, w, h, scale, &low_pass, 0, 0, 0) ||
            _iqa_decimate(cmp_f, w, h, scale, &low_pass, 0, &w, &h)) { /* update w/h */
            free(ref_f);
            free(cmp_f);
            free(low_pass.kernel);
            printf("error: decimation fails on ref_f or cmp_f.\n");
            fflush(stdout);
            goto fail_or_end;
        }
        free(low_pass.kernel);
    }

	result = _iqa_ssim(ref_f, cmp_f, w, h, &window, &mr, args);

    free(ref_f);
    free(cmp_f);

	*score = (double)result;

	ret = 0;
fail_or_end:
	return ret;

}

int ssim(const char *ref_path, const char *dis_path, int w, int h, const char *fmt)
{
	double score = 0;
	number_t *ref_buf = 0;
	number_t *dis_buf = 0;
	number_t *temp_buf = 0;

	FILE *ref_rfile = 0;
	FILE *dis_rfile = 0;
	size_t data_sz;
	int stride;
	int ret = 1;

	if (w <= 0 || h <= 0 || (size_t)w > ALIGN_FLOOR(INT_MAX) / sizeof(number_t))
	{
		goto fail_or_end;
	}

	stride = ALIGN_CEIL(w * sizeof(number_t));

	if ((size_t)h > SIZE_MAX / stride)
	{
		goto fail_or_end;
	}

	data_sz = (size_t)stride * h;

	if (!(ref_buf = aligned_malloc(data_sz, MAX_ALIGN)))
	{
		printf("error: aligned_malloc failed for ref_buf.\n");
		fflush(stdout);
		goto fail_or_end;
	}
	if (!(dis_buf = aligned_malloc(data_sz, MAX_ALIGN)))
	{
		printf("error: aligned_malloc failed for dis_buf.\n");
		fflush(stdout);
		goto fail_or_end;
	}
	if (!(temp_buf = aligned_malloc(data_sz * 2, MAX_ALIGN)))
	{
		printf("error: aligned_malloc failed for temp_buf.\n");
		fflush(stdout);
		goto fail_or_end;
	}

	if (!(ref_rfile = fopen(ref_path, "rb")))
	{
		printf("error: fopen ref_path %s failed\n", ref_path);
		fflush(stdout);
		goto fail_or_end;
	}
	if (!(dis_rfile = fopen(dis_path, "rb")))
	{
		printf("error: fopen dis_path %s failed.\n", dis_path);
		fflush(stdout);
		goto fail_or_end;
	}

	size_t offset;
	if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv420p10le"))
	{
		if ((w * h) % 2 != 0)
		{
			printf("error: (w * h) %% 2 != 0, w = %d, h = %d.\n", w, h);
			fflush(stdout);
			goto fail_or_end;
		}
		offset = w * h / 2;
	}
	else if (!strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv422p10le"))
	{
		offset = w * h;
	}
	else if (!strcmp(fmt, "yuv444p") || !strcmp(fmt, "yuv444p10le"))
	{
		offset = w * h * 2;
	}
	else
	{
		printf("error: unknown format %s.\n", fmt);
		fflush(stdout);
		goto fail_or_end;
	}

	int frm_idx = 0;
	while (1)
	{
		// read ref y
		if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
		{
			ret = read_image_b(ref_rfile, ref_buf, 0, w, h, stride);
		}
		else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
		{
			ret = read_image_w(ref_rfile, ref_buf, 0, w, h, stride);
		}
		else
		{
			printf("error: unknown format %s.\n", fmt);
			fflush(stdout);
			goto fail_or_end;
		}
		if (ret)
		{
			if (feof(ref_rfile))
			{
				ret = 0; // OK if end of file
			}
			goto fail_or_end;
		}

		// read dis y
		if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
		{
			ret = read_image_b(dis_rfile, dis_buf, 0, w, h, stride);
		}
		else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
		{
			ret = read_image_w(dis_rfile, dis_buf, 0, w, h, stride);
		}
		else
		{
			printf("error: unknown format %s.\n", fmt);
			fflush(stdout);
			goto fail_or_end;
		}
		if (ret)
		{
			if (feof(dis_rfile))
			{
				ret = 0; // OK if end of file
			}
			goto fail_or_end;
		}

		// compute
		ret = compute_ssim(ref_buf, dis_buf, w, h, stride, stride, &score);
		if (ret)
		{
			printf("error: compute_ssim failed.\n");
			fflush(stdout);
			goto fail_or_end;
		}

		// print
		printf("ssim: %d %f\n", frm_idx, score);
		fflush(stdout);

		// ref skip u and v
		if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
		{
			if (fread(temp_buf, 1, offset, ref_rfile) != (size_t)offset)
			{
				printf("error: ref fread u and v failed.\n");
				fflush(stdout);
				goto fail_or_end;
			}
		}
		else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
		{
			if (fread(temp_buf, 2, offset, ref_rfile) != (size_t)offset)
			{
				printf("error: ref fread u and v failed.\n");
				fflush(stdout);
				goto fail_or_end;
			}
		}
		else
		{
			printf("error: unknown format %s.\n", fmt);
			fflush(stdout);
			goto fail_or_end;
		}

		// dis skip u and v
		if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p"))
		{
			if (fread(temp_buf, 1, offset, dis_rfile) != (size_t)offset)
			{
				printf("error: dis fread u and v failed.\n");
				fflush(stdout);
				goto fail_or_end;
			}
		}
		else if (!strcmp(fmt, "yuv420p10le") || !strcmp(fmt, "yuv422p10le") || !strcmp(fmt, "yuv444p10le"))
		{
			if (fread(temp_buf, 2, offset, dis_rfile) != (size_t)offset)
			{
				printf("error: dis fread u and v failed.\n");
				fflush(stdout);
				goto fail_or_end;
			}
		}
		else
		{
			printf("error: unknown format %s.\n", fmt);
			fflush(stdout);
			goto fail_or_end;
		}

		frm_idx++;
	}

	ret = 0;

fail_or_end:
	if (ref_rfile)
	{
		fclose(ref_rfile);
	}
	if (dis_rfile)
	{
		fclose(dis_rfile);
	}
	aligned_free(ref_buf);
	aligned_free(dis_buf);
	aligned_free(temp_buf);

	return ret;
}


static void usage(void)
{
	puts("usage: ssim fmt ref dis w h\n"
		 "fmts:\n"
		 "\tyuv420p\n"
		 "\tyuv422p\n"
		 "\tyuv444p\n"
		 "\tyuv420p10le\n"
		 "\tyuv422p10le\n"
		 "\tyuv444p10le"
	);
}

int main(int argc, const char **argv)
{
	const char *ref_path;
	const char *dis_path;
	const char *fmt;
	int w;
	int h;
	int ret;

	if (argc < 6) {
		usage();
		return 2;
	}

	fmt		 = argv[1];
	ref_path = argv[2];
	dis_path = argv[3];
	w        = atoi(argv[4]);
	h        = atoi(argv[5]);

	if (w <= 0 || h <= 0)
		return 2;

	ret = ssim(ref_path, dis_path, w, h, fmt);

	if (ret)
		return ret;

	return 0;
}
