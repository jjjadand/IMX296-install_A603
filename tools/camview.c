/* IMX296 V4L2 preview helper: RG10 -> RGB8 on stdout.
 *
 * JetPack 6.2 GStreamer 1.20 cannot map V4L2 RG10, so v4l2src ! bayer2rgb
 * fails with not-negotiated. This tool mmap-captures 10-bit Bayer and writes
 * RGB8 after a small CPU ISP. Tables come from the board ISP file:
 *   /usr/share/imx296-a603/imx296-isp.json
 * override with CAMVIEW_ISP_FILE=... ; CAMVIEW_ISP=0 skips AWB/CCM.
 * Default CFA is RGGB (empirically correct on A603 / Orin overlays).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/videodev2.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define NBUF 4
#define CT_MAX 8

enum mode {
	MODE_MONO = 0,
	MODE_GBRG,
	MODE_RGGB,
	MODE_GRBG,
	MODE_BGGR,
};

struct buf {
	void *start;
	size_t len;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static inline uint16_t clamp10(int v)
{
	return (uint16_t)(v < 0 ? 0 : (v > 1023 ? 1023 : v));
}

static const char *mode_name(enum mode mode)
{
	switch (mode) {
	case MODE_MONO:
		return "mono";
	case MODE_GBRG:
		return "gbrg";
	case MODE_GRBG:
		return "grbg";
	case MODE_BGGR:
		return "bggr";
	case MODE_RGGB:
	default:
		return "rggb";
	}
}

static enum mode parse_mode(const char *s)
{
	if (!s || !strcmp(s, "bayer") || !strcmp(s, "rggb"))
		return MODE_RGGB;
	if (!strcmp(s, "mono"))
		return MODE_MONO;
	if (!strcmp(s, "gbrg"))
		return MODE_GBRG;
	if (!strcmp(s, "grbg"))
		return MODE_GRBG;
	if (!strcmp(s, "bggr"))
		return MODE_BGGR;
	fprintf(stderr,
		"camview: unknown mode '%s' (mono|bayer|gbrg|rggb|grbg|bggr)\n",
		s);
	exit(1);
}

/* Raspberry Pi imx296_16mm.json rpi.contrast.gamma_curve */
static uint8_t gamma_lut[1024];

static const uint16_t gamma_x[] = {
	0, 512, 1024, 1536, 2048, 2560, 3072, 3584, 4096, 4608, 5120, 5632,
	6144, 6656, 7168, 7680, 8192, 9216, 10240, 11264, 12288, 13312, 14336,
	15360, 16384, 17408, 18432, 19456, 20480, 22528, 24576, 26624, 28672,
	30720, 32768, 34816, 36864, 38912, 40960, 43008, 45056, 47104, 49152,
	51200, 53248, 55296, 57344, 59392, 61440, 63488, 65535
};
static const uint16_t gamma_y[] = {
	0, 2518, 5033, 7175, 9309, 10814, 12312, 13773, 15225, 16566, 17899,
	19221, 20534, 21684, 22826, 24024, 25212, 27251, 29167, 30947, 32696,
	34309, 35849, 37194, 38445, 39598, 40732, 41717, 42687, 44343, 45871,
	47222, 48441, 49460, 50470, 51476, 52480, 53382, 54294, 55155, 56035,
	56920, 57824, 58737, 59666, 60604, 61558, 62529, 63516, 64519, 65535
};

static void init_gamma_lut(void)
{
	int i, k = 0;
	int n = (int)(sizeof(gamma_x) / sizeof(gamma_x[0]));

	for (i = 0; i < 1024; i++) {
		unsigned x = (unsigned)i * 64;
		unsigned y;
		while (k + 1 < n && gamma_x[k + 1] < x)
			k++;
		if (k + 1 >= n || x <= gamma_x[k]) {
			y = gamma_y[k];
		} else {
			unsigned x0 = gamma_x[k], x1 = gamma_x[k + 1];
			unsigned y0 = gamma_y[k], y1 = gamma_y[k + 1];
			y = y0 + (unsigned)((unsigned long)(y1 - y0) * (x - x0) /
					     (x1 - x0));
		}
		gamma_lut[i] = (uint8_t)((y * 255 + 32767) / 65535);
	}
}

/* imx296_16mm.json CCM at 4560 K, Q10 */
static int ccm_q10[9] = {
	2204, -868, -312,
	-496, 2344, -825,
	-155, -543, 1722
};

static int ct_n = 5;
static int ct_r_q10[CT_MAX] = { 481, 419, 336, 306, 234 };
static int ct_b_q10[CT_MAX] = { 329, 437, 555, 591, 668 };
static int ct_gain_r[CT_MAX] = { 2179, 2504, 3121, 3422, 4481 };
static int ct_gain_b[CT_MAX] = { 3191, 2401, 1890, 1774, 1570 };
static int black_10 = 60; /* rpi.black_level 3840/64 */

static int gain_r_q10 = 3121; /* 4640 K indoor default */
static int gain_b_q10 = 1890;
static int isp_enable = 1;
static int skip_frames = 3;
static int hl_start = 780;
static int hl_full = 940;
static int fc_start = 220;
static int fc_full = 500;
static int fc_ratio_lo = 250;
static int fc_ratio_hi = 650;

static const char *skip_ws(const char *p)
{
	while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static const char *find_json_key(const char *js, const char *key)
{
	char pat[128];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(js, pat);
	if (!p)
		return NULL;
	p = strchr(p + strlen(pat), ':');
	return p ? skip_ws(p + 1) : NULL;
}

static int parse_json_int(const char *js, const char *key, int *out)
{
	const char *p = find_json_key(js, key);
	char *end;
	long v;

	if (!p)
		return -1;
	v = strtol(p, &end, 10);
	if (end == p)
		return -1;
	*out = (int)v;
	return 0;
}

static int parse_json_int_array(const char *js, const char *key, int *dst, int maxn)
{
	const char *p = find_json_key(js, key);
	int n = 0;

	if (!p)
		return -1;
	while (*p && *p != '[')
		p++;
	if (*p != '[')
		return -1;
	p++;
	while (*p && *p != ']' && n < maxn) {
		char *end;
		long v;

		p = skip_ws(p);
		if (*p == ',' ) {
			p++;
			continue;
		}
		if (*p == ']')
			break;
		v = strtol(p, &end, 10);
		if (end == p)
			break;
		dst[n++] = (int)v;
		p = end;
	}
	return n;
}

static int load_isp_file(const char *path)
{
	FILE *fp;
	char *js = NULL;
	long sz;
	int n, i, tmp[1024];

	fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	sz = ftell(fp);
	if (sz <= 0 || sz > 2 * 1024 * 1024) {
		fclose(fp);
		return -1;
	}
	rewind(fp);
	js = malloc((size_t)sz + 1);
	if (!js) {
		fclose(fp);
		return -1;
	}
	if (fread(js, 1, (size_t)sz, fp) != (size_t)sz) {
		free(js);
		fclose(fp);
		return -1;
	}
	js[sz] = 0;
	fclose(fp);

	parse_json_int(js, "black_10", &black_10);
	parse_json_int(js, "hl_start", &hl_start);
	parse_json_int(js, "hl_full", &hl_full);
	parse_json_int(js, "fc_start", &fc_start);
	parse_json_int(js, "fc_full", &fc_full);
	parse_json_int(js, "fc_ratio_lo", &fc_ratio_lo);
	parse_json_int(js, "fc_ratio_hi", &fc_ratio_hi);
	n = parse_json_int_array(js, "ccm_q10", tmp, 9);
	if (n == 9)
		memcpy(ccm_q10, tmp, sizeof(ccm_q10));
	n = parse_json_int_array(js, "ct_r_q10", tmp, CT_MAX);
	if (n > 0) {
		ct_n = n;
		memcpy(ct_r_q10, tmp, n * sizeof(int));
	}
	n = parse_json_int_array(js, "ct_b_q10", tmp, CT_MAX);
	if (n > 0)
		memcpy(ct_b_q10, tmp, n * sizeof(int));
	n = parse_json_int_array(js, "ct_gain_r_q10", tmp, CT_MAX);
	if (n > 0)
		memcpy(ct_gain_r, tmp, n * sizeof(int));
	n = parse_json_int_array(js, "ct_gain_b_q10", tmp, CT_MAX);
	if (n > 0)
		memcpy(ct_gain_b, tmp, n * sizeof(int));
	n = parse_json_int_array(js, "gamma_lut", tmp, 1024);
	if (n == 1024) {
		for (i = 0; i < 1024; i++)
			gamma_lut[i] = (uint8_t)(tmp[i] < 0 ? 0 : (tmp[i] > 255 ? 255 : tmp[i]));
	}
	{
		int idx = 2;
		parse_json_int(js, "default_ct_index", &idx);
		if (idx >= 0 && idx < ct_n) {
			gain_r_q10 = ct_gain_r[idx];
			gain_b_q10 = ct_gain_b[idx];
		}
	}
	fprintf(stderr,
		"camview: isp file %s (black=%d ct_n=%d hl=%d..%d fc=%d..%d r=%d..%d)\n",
		path, black_10, ct_n, hl_start, hl_full, fc_start, fc_full,
		fc_ratio_lo, fc_ratio_hi);
	free(js);
	return 0;
}

static void load_isp(void)
{
	const char *env = getenv("CAMVIEW_ISP_FILE");
	const char *cands[4];
	int n = 0;
	int i;

	init_gamma_lut();
	if (env && env[0])
		cands[n++] = env;
	cands[n++] = "/etc/imx296-a603/imx296-isp.json";
	cands[n++] = "/usr/share/imx296-a603/imx296-isp.json";
	for (i = 0; i < n; i++) {
		if (load_isp_file(cands[i]) == 0)
			return;
	}
	fprintf(stderr, "camview: using built-in ISP tables\n");
}

static void awb_update(const uint16_t *rgb, int w, int h)
{
	long r_sum = 0, g_sum = 0, b_sum = 0;
	long n = 0;
	int y, x, i, best = 2;
	long best_d = 1L << 30;
	int r_q, b_q;

	for (y = h / 6; y < h - h / 6; y += 8) {
		const uint16_t *row = rgb + (size_t)y * w * 3;
		for (x = w / 6; x < w - w / 6; x += 8) {
			int r = row[x * 3 + 0];
			int g = row[x * 3 + 1];
			int b = row[x * 3 + 2];
			int lum = r + g + b;
			if (lum < 48 || lum > 2700)
				continue;
			r_sum += r;
			g_sum += g;
			b_sum += b;
			n++;
		}
	}
	if (n < 32 || g_sum <= 0)
		return;
	r_q = (int)(r_sum * 1024 / g_sum);
	b_q = (int)(b_sum * 1024 / g_sum);
	for (i = 0; i < ct_n; i++) {
		long dr = r_q - ct_r_q10[i];
		long db = b_q - ct_b_q10[i];
		long d = dr * dr + db * db;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	gain_r_q10 = ct_gain_r[best];
	gain_b_q10 = ct_gain_b[best];
}

static int smoothstep_q10(int v, int a, int b)
{
	int t, t2, f, span;

	if (v <= a)
		return 0;
	if (v >= b)
		return 1024;
	span = b - a;
	if (span < 1)
		span = 1;
	t = ((v - a) << 10) / span;
	t2 = (t * t) >> 10;
	f = (t2 * (3072 - 2 * t)) >> 10;
	return f < 0 ? 0 : (f > 1024 ? 1024 : f);
}

static int rgb_peak(const uint16_t *rgb, int idx)
{
	int r = rgb[idx], g = rgb[idx + 1], b = rgb[idx + 2];
	int mx = r > g ? r : g;

	if (b > mx)
		mx = b;
	return mx;
}

static int neigh_peak(const uint16_t *rgb, int w, int h, int x, int y, int mx)
{
	int dy, dx, nmax = mx;

	for (dy = -1; dy <= 1; dy++) {
		int yy = y + dy;

		if (yy < 0 || yy >= h)
			continue;
		for (dx = -1; dx <= 1; dx++) {
			int xx = x + dx;
			int p;

			if (xx < 0 || xx >= w)
				continue;
			p = rgb_peak(rgb, (yy * w + xx) * 3);
			if (p > nmax)
				nmax = p;
		}
	}
	return nmax;
}

static void apply_isp(const uint16_t *rgb, uint8_t *out, int w, int h)
{
	int n = w * h;
	int i;
	int gr = gain_r_q10, gb = gain_b_q10;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
	for (i = 0; i < n; i++) {
		int r0 = rgb[i * 3 + 0];
		int g0 = rgb[i * 3 + 1];
		int b0 = rgb[i * 3 + 2];
		int x = i % w, y = i / w;
		int mx, mn, nmax, rel, r, g, b, rr, gg, bb, yy, f;

		mx = r0 > g0 ? r0 : g0;
		if (b0 > mx)
			mx = b0;
		mn = r0 < g0 ? r0 : g0;
		if (b0 < mn)
			mn = b0;
		rel = mx > 0 ? ((mx - mn) << 10) / mx : 0;
		nmax = neigh_peak(rgb, w, h, x, y, mx);

		/* AWB then CCM. Clipped whites are not equal after AWB, and
		 * the 4560 K CCM then drives G negative -> purple lamps. */
		r = (r0 * gr) >> 10;
		g = g0;
		b = (b0 * gb) >> 10;
		rr = (ccm_q10[0] * r + ccm_q10[1] * g + ccm_q10[2] * b) >> 10;
		gg = (ccm_q10[3] * r + ccm_q10[4] * g + ccm_q10[5] * b) >> 10;
		bb = (ccm_q10[6] * r + ccm_q10[7] * g + ccm_q10[8] * b) >> 10;

		/* Bright, channel-imbalanced pixels are Bayer zipper around
		 * clipped lamps (cyan/orange). Use the 3x3 peak so the darker
		 * zipper ring still desaturates, but fade to luma not white. */
		f = (smoothstep_q10(nmax, fc_start, fc_full) *
		     smoothstep_q10(rel, fc_ratio_lo, fc_ratio_hi)) >> 10;
		if (f > 0) {
			yy = (rr + 2 * gg + bb) >> 2;
			rr += ((yy - rr) * f) >> 10;
			gg += ((yy - gg) * f) >> 10;
			bb += ((yy - bb) * f) >> 10;
		}
		{
			int pmn = rr < gg ? rr : gg;
			int pmx = rr > gg ? rr : gg;

			if (bb < pmn)
				pmn = bb;
			if (bb > pmx)
				pmx = bb;
			if (pmn < 8 && pmx > 256 && nmax > 200) {
				yy = (rr + 2 * gg + bb) >> 2;
				rr = gg = bb = yy;
			} else if (pmn < 0 && pmx > 0) {
				int fn = ((-pmn) << 10) / pmx;

				if (fn > 1024)
					fn = 1024;
				yy = (rr + 2 * gg + bb) >> 2;
				rr += ((yy - rr) * fn) >> 10;
				gg += ((yy - gg) * fn) >> 10;
				bb += ((yy - bb) * fn) >> 10;
			}
		}

		f = mx > hl_start ? smoothstep_q10(mx, hl_start, hl_full) : 0;
		if (f > 0) {
			rr += ((1023 - rr) * f) >> 10;
			gg += ((1023 - gg) * f) >> 10;
			bb += ((1023 - bb) * f) >> 10;
		}

		out[i * 3 + 0] = gamma_lut[clamp10(rr)];
		out[i * 3 + 1] = gamma_lut[clamp10(gg)];
		out[i * 3 + 2] = gamma_lut[clamp10(bb)];
	}
}

static void apply_gamma_only(const uint16_t *rgb, uint8_t *out, int w, int h)
{
	int n = w * h;
	int i;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
	for (i = 0; i < n; i++) {
		out[i * 3 + 0] = gamma_lut[clamp10(rgb[i * 3 + 0])];
		out[i * 3 + 1] = gamma_lut[clamp10(rgb[i * 3 + 1])];
		out[i * 3 + 2] = gamma_lut[clamp10(rgb[i * 3 + 2])];
	}
}

/* kind: 0=R, 1=G, 2=B at (y,x) */
static inline int pixel_kind(enum mode mode, int y, int x)
{
	int ye = y & 1;
	int xe = x & 1;

	switch (mode) {
	case MODE_RGGB:
		return ye ? (xe ? 2 : 1) : (xe ? 1 : 0);
	case MODE_GRBG:
		return ye ? (xe ? 1 : 2) : (xe ? 0 : 1);
	case MODE_BGGR:
		return ye ? (xe ? 0 : 1) : (xe ? 1 : 2);
	case MODE_GBRG:
	default:
		return ye ? (xe ? 1 : 0) : (xe ? 2 : 1);
	}
}

static void demosaic(const uint16_t *bayer, uint16_t *rgb, int w, int h,
		     enum mode mode)
{
	int y, x;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
	for (y = 1; y < h - 1; y++) {
		const uint16_t *pm = bayer + (size_t)(y - 1) * w;
		const uint16_t *p = bayer + (size_t)y * w;
		const uint16_t *pp = bayer + (size_t)(y + 1) * w;
		uint16_t *d = rgb + (size_t)y * w * 3;

		for (x = 1; x < w - 1; x++) {
			int kind = pixel_kind(mode, y, x);
			int r, g, b;

			if (kind == 1) {
				g = p[x];
				if (pixel_kind(mode, y, x - 1) == 0) {
					r = (p[x - 1] + p[x + 1]) >> 1;
					b = (pm[x] + pp[x]) >> 1;
				} else {
					b = (p[x - 1] + p[x + 1]) >> 1;
					r = (pm[x] + pp[x]) >> 1;
				}
			} else if (kind == 0) {
				r = p[x];
				g = (p[x - 1] + p[x + 1] + pm[x] + pp[x]) >> 2;
				b = (pm[x - 1] + pm[x + 1] + pp[x - 1] +
				     pp[x + 1]) >> 2;
			} else {
				b = p[x];
				g = (p[x - 1] + p[x + 1] + pm[x] + pp[x]) >> 2;
				r = (pm[x - 1] + pm[x + 1] + pp[x - 1] +
				     pp[x + 1]) >> 2;
			}
			d[x * 3 + 0] = (uint16_t)r;
			d[x * 3 + 1] = (uint16_t)g;
			d[x * 3 + 2] = (uint16_t)b;
		}
	}
	for (x = 0; x < w; x++) {
		memcpy(rgb + x * 3, rgb + (w + x) * 3, 6);
		memcpy(rgb + ((h - 1) * w + x) * 3,
		       rgb + ((h - 2) * w + x) * 3, 6);
	}
	for (y = 0; y < h; y++) {
		memcpy(rgb + (y * w) * 3, rgb + (y * w + 1) * 3, 6);
		memcpy(rgb + (y * w + w - 1) * 3,
		       rgb + (y * w + w - 2) * 3, 6);
	}
}

static void mono_to_rgb(const uint16_t *g, uint16_t *rgb, int w, int h)
{
	int n = w * h, i;

	for (i = 0; i < n; i++) {
		rgb[i * 3 + 0] = g[i];
		rgb[i * 3 + 1] = g[i];
		rgb[i * 3 + 2] = g[i];
	}
}

int main(int argc, char **argv)
{
	const char *dev;
	enum mode mode = MODE_RGGB;
	long maxframes = -1;
	int want_w = 1456, want_h = 1088;
	int fd, i, shift = 6;
	unsigned maxv = 0;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	enum v4l2_buf_type type;
	struct buf bufs[NBUF];
	uint16_t *bayer = NULL, *rgb16 = NULL;
	uint8_t *rgb = NULL;
	int w, h, stride;
	size_t rgb_size;
	long frames = 0;
	struct timeval t0, t1;
	const char *isp_env;
	const char *skip_env;

	if (argc < 2) {
		fprintf(stderr,
			"usage: camview <device> [mono|bayer|gbrg|rggb|grbg|bggr] [maxframes] [width height]\n");
		return 1;
	}
	isp_env = getenv("CAMVIEW_ISP");
	if (isp_env && (!strcmp(isp_env, "0") || !strcmp(isp_env, "off") ||
			!strcmp(isp_env, "OFF")))
		isp_enable = 0;
	skip_env = getenv("CAMVIEW_SKIP");
	if (skip_env)
		skip_frames = atoi(skip_env);
	load_isp();
	dev = argv[1];
	if (argc >= 3)
		mode = parse_mode(argv[2]);
	if (argc >= 4)
		maxframes = strtol(argv[3], NULL, 10);
	if (argc >= 6) {
		want_w = atoi(argv[4]);
		want_h = atoi(argv[5]);
	}

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("G_FMT");
		return 1;
	}
	fmt.fmt.pix.width = want_w;
	fmt.fmt.pix.height = want_h;
	fmt.fmt.pix.pixelformat = v4l2_fourcc('R', 'G', '1', '0');
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		return 1;
	}
	w = fmt.fmt.pix.width;
	h = fmt.fmt.pix.height;
	stride = fmt.fmt.pix.bytesperline;

	fprintf(stderr, "camview: %s %dx%d stride=%d size=%d mode=%s isp=%s\n",
		dev, w, h, stride, fmt.fmt.pix.sizeimage, mode_name(mode),
		isp_enable ? "on" : "off");

	memset(&req, 0, sizeof(req));
	req.count = NBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		return 1;
	}
	for (i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("QUERYBUF");
			return 1;
		}
		bufs[i].len = buf.length;
		bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, buf.m.offset);
		if (bufs[i].start == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			return 1;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		return 1;
	}

	bayer = malloc((size_t)w * h * sizeof(uint16_t));
	rgb16 = malloc((size_t)w * h * 3 * sizeof(uint16_t));
	rgb = malloc((size_t)w * h * 3);
	if (!bayer || !rgb16 || !rgb) {
		perror("malloc");
		return 1;
	}
	rgb_size = (size_t)w * h * 3;
	gettimeofday(&t0, NULL);

	for (;;) {
		fd_set fds;
		struct timeval tv;
		int r;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = 2;
		tv.tv_usec = 0;
		r = select(fd + 1, &fds, NULL, NULL, &tv);
		if (r <= 0) {
			fprintf(stderr, "camview: timeout waiting for frame\n");
			return 1;
		}

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("DQBUF");
			return 1;
		}

		{
			const uint8_t *src = bufs[buf.index].start;
			int y, x;

			if (frames == 0) {
				for (y = 0; y < h; y++) {
					const uint16_t *row =
						(const uint16_t *)(src +
								    (size_t)y * stride);
					for (x = 0; x < w; x++) {
						if (row[x] > maxv)
							maxv = row[x];
					}
				}
				/* Orin VI stores 10-bit as (p<<6)|(p>>4). */
				shift = (maxv > 0x3ff) ? 6 : 0;
				fprintf(stderr, "camview: maxv=%u shift=%d\n",
					maxv, shift);
			}
#ifdef _OPENMP
#pragma omp parallel for schedule(static) private(x)
#endif
			for (y = 0; y < h; y++) {
				const uint16_t *row =
					(const uint16_t *)(src + (size_t)y * stride);
				uint16_t *dst = bayer + (size_t)y * w;

				for (x = 0; x < w; x++) {
					int v = (int)(row[x] >> shift) - black_10;
					dst[x] = (uint16_t)(v < 0 ? 0 : v);
				}
			}
		}

		if (mode == MODE_MONO)
			mono_to_rgb(bayer, rgb16, w, h);
		else
			demosaic(bayer, rgb16, w, h, mode);

		if (isp_enable) {
			awb_update(rgb16, w, h);
			apply_isp(rgb16, rgb, w, h);
		} else {
			apply_gamma_only(rgb16, rgb, w, h);
		}

		if (frames >= skip_frames) {
			if (fwrite(rgb, 1, rgb_size, stdout) != rgb_size) {
				fprintf(stderr, "camview: pipe closed\n");
				break;
			}
		}

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			return 1;
		}
		frames++;
		if ((frames & 15) == 0) {
			double dt;
			gettimeofday(&t1, NULL);
			dt = (t1.tv_sec - t0.tv_sec) +
			     (t1.tv_usec - t0.tv_usec) / 1e6;
			if (dt > 0.2)
				fprintf(stderr,
					"camview: %.1f fps  awb R=%.2f B=%.2f\n",
					frames / dt, gain_r_q10 / 1024.0,
					gain_b_q10 / 1024.0);
		}
		if (maxframes >= 0 && (frames - skip_frames) >= maxframes)
			break;
	}

	gettimeofday(&t1, NULL);
	{
		double dt = (t1.tv_sec - t0.tv_sec) +
			    (t1.tv_usec - t0.tv_usec) / 1e6;
		if (dt > 0)
			fprintf(stderr, "camview: %ld frames done (%.1f fps)\n",
				frames, frames / dt);
		else
			fprintf(stderr, "camview: %ld frames done\n", frames);
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	return 0;
}
