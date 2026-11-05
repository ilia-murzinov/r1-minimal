#include "eq.h"

#include <math.h>
#include <string.h>

#include "src/system/core/config.h"

/* Minimal R1: graphic EQ / PEQ / MSEB removed. Soundfield, balance, mono and
 * crossfeed stay (still applied from the audio thread when enabled in config). */

const int eq_band_freq[EQ_BANDS] = {31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
const eq_preset_t EQ_DEFAULT_PRESETS[] = {{"Flat", {0}}};
const int EQ_DEFAULT_PRESET_COUNT = 1;
const char *const mseb_band_name[MSEB_BANDS] = {
	"master_temp", "bass0", "bass1", "bass2", "mid0", "mid1", "mid2", "treble0", "treble1", "treble2",
};

static volatile int sf_enabled;
static volatile int sf_width = SOUNDFIELD_WIDTH_DEFAULT;

// Channel balance -- see eq.h for where its range comes from.
// ---------------------------------------------------------------------------

static volatile int bal_enabled;
static volatile int mono_enabled;
static volatile int bal_tenths;	  // -200..200, the slider's own value
static volatile int bal_left_q15; // the two attenuations, 32768 = untouched
static volatile int bal_right_q15;

static int clamp_balance(int tenths) {
	if (tenths < BALANCE_MIN) {
		tenths = BALANCE_MIN;
	}
	if (tenths > BALANCE_MAX) {
		tenths = BALANCE_MAX;
	}
	// Snapped to the half-decibel the original's slider steps by, rounded to
	// the nearest rather than towards zero.
	int step = BALANCE_STEP;
	return ((tenths < 0 ? tenths - step / 2 : tenths + step / 2) / step) * step;
}

// 10^(-dB/20) in Q15, without a pow() call anywhere near the audio path: every
// half decibel of attenuation from 0 to 20 dB, which is the whole range the
// slider can ask for.
static int attenuation_q15(int tenths_down) {
	static const int TABLE[41] = {
		32768, 30935, 29205, 27571, 26029, 24573, 23198, 21900, 20675, 19519, 18427,
		17396, 16423, 15504, 14637, 13818, 13045, 12315, 11627, 10976, 10362, 9783,
		9235,  8719,  8231,  7771,  7336,  6925,  6538,  6172,  5827,  5501,  5193,
		4903,  4629,  4370,  4125,  3894,  3677,  3471,  3277,
	};
	int index = tenths_down / BALANCE_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index > 40) {
		index = 40;
	}
	return TABLE[index];
}

static void balance_set_gains(int tenths) {
	bal_tenths = tenths;
	// Only ever downwards: the channel being favoured is left alone and the
	// other one turned down, so a track already at full scale cannot be pushed
	// into clipping by a balance setting.
	bal_left_q15 = tenths > 0 ? attenuation_q15(tenths) : 32768;
	bal_right_q15 = tenths < 0 ? attenuation_q15(-tenths) : 32768;
}

static volatile int cf_enabled;
static volatile int cf_level = CROSSFEED_LEVEL_DEFAULT; // per cent
static volatile int cf_cutoff = CROSSFEED_CUTOFF_DEFAULT;
static volatile int cf_delay_us = CROSSFEED_DELAY_DEFAULT;

// The delay line. The 600 us maximum delay is 231 samples at 384 kHz, the
// highest rate this DAC accepts, so 256 covers it with margin at 2 KB total.
#define CF_DELAY_MAX_SAMPLES 256

typedef struct {
	int32_t line[CF_DELAY_MAX_SAMPLES];
	int pos;
	int32_t lp; // one-pole low-pass state, Q8 above the sample
} cf_channel_t;

static cf_channel_t cf_left, cf_right;

// What was prepared last time: if any of the three settings changes, or the
// sample rate does (it enters both computations), the values are recomputed.
static int cf_ready_rate;
static int cf_ready_cutoff;
static int cf_ready_delay_us;
static int cf_delay_samples;
static int cf_alpha_q15;   // low-pass coefficient
static int cf_feed_q15;    // how much of the opposite channel
static int cf_direct_q15;  // and how much of its own, so the level does not rise

static int clamp_step(int value, int min, int max, int step) {
	if (value < min) {
		value = min;
	}
	if (value > max) {
		value = max;
	}
	return min + ((value - min + step / 2) / step) * step;
}

static int clamp_width(int hundredths) {
	if (hundredths < SOUNDFIELD_WIDTH_MIN) {
		hundredths = SOUNDFIELD_WIDTH_MIN;
	}
	if (hundredths > SOUNDFIELD_WIDTH_MAX) {
		hundredths = SOUNDFIELD_WIDTH_MAX;
	}
	// Snapped to the plugin's own 0.05 step, so a stale config value cannot
	// leave the slider between two positions.
	return (hundredths / SOUNDFIELD_WIDTH_STEP) * SOUNDFIELD_WIDTH_STEP;
}

void crossfeed_reset(void); /* defined with the crossfeed block below */

// ---------------------------------------------------------------------------

void eq_init(void) {
	sf_enabled = (int)config_get_int("soundfield", "enabled", 0);
	sf_width = clamp_width((int)config_get_int("soundfield", "width", SOUNDFIELD_WIDTH_DEFAULT));
	bal_enabled = (int)config_get_int("balance", "enabled", 0);
	mono_enabled = config_get_int("audio", "mono", 0) ? 1 : 0;
	balance_set_gains(clamp_balance((int)config_get_int("balance", "value", BALANCE_CENTRE)));
	cf_enabled = (int)config_get_int("crossfeed", "enabled", 0);
	cf_level = clamp_step((int)config_get_int("crossfeed", "level", CROSSFEED_LEVEL_DEFAULT), CROSSFEED_LEVEL_MIN,
						  CROSSFEED_LEVEL_MAX, CROSSFEED_LEVEL_STEP);
	cf_cutoff = clamp_step((int)config_get_int("crossfeed", "cutoff", CROSSFEED_CUTOFF_DEFAULT), CROSSFEED_CUTOFF_MIN,
						   CROSSFEED_CUTOFF_MAX, CROSSFEED_CUTOFF_STEP);
	cf_delay_us = clamp_step((int)config_get_int("crossfeed", "delay_us", CROSSFEED_DELAY_DEFAULT),
							 CROSSFEED_DELAY_MIN, CROSSFEED_DELAY_MAX, CROSSFEED_DELAY_STEP);
	cf_ready_rate = 0;
	crossfeed_reset();
}

void eq_set_enabled(bool enabled) { (void)enabled; }
bool eq_get_enabled(void) { return false; }
void eq_set_band(int band, int gain_db) { (void)band; (void)gain_db; }
void eq_set_band_tenths(int band, int tenths) { (void)band; (void)tenths; }
int eq_get_band_tenths(int band) { (void)band; return 0; }
void eq_save(void) {}
void eq_set_width(int width) { (void)width; }
int eq_get_width(void) { return EQ_WIDTH_NORMAL; }
void eq_response_curve(const int *freqs, int count, int sample_rate, int *out_tenths) {
	(void)freqs; (void)sample_rate;
	if (out_tenths) for (int i = 0; i < count; i++) out_tenths[i] = 0;
}
void eq_reset(void) {}
void eq_apply_preset(const eq_preset_t *preset) { (void)preset; }
void eq_presets_set_dir(const char *sd_root) { (void)sd_root; }
bool eq_preset_save(const char *name) { (void)name; return false; }
bool eq_preset_load(const char *name) { (void)name; return false; }
bool eq_preset_delete(const char *name) { (void)name; return false; }
int eq_preset_for_each(eq_preset_name_cb_t cb, void *user) { (void)cb; (void)user; return 0; }

void peq_set_enabled(bool enabled) { (void)enabled; }
bool peq_get_enabled(void) { return false; }
void peq_get_band(int index, peq_band_t *out) {
	(void)index;
	if (out) { out->on = false; out->type = PEQ_TYPE_PEAK; out->freq = 1000; out->gain_tenths = 0; out->q_cent = PEQ_Q_DEFAULT; }
}
void peq_set_band(int index, const peq_band_t *band) { (void)index; (void)band; }
int peq_get_preamp(void) { return 0; }
void peq_set_preamp(int tenths) { (void)tenths; }
void peq_reset(void) {}
void peq_reset_band(int index) { (void)index; }
void peq_presets_set_dir(const char *sd_root) { (void)sd_root; }
bool peq_preset_save(const char *name) { (void)name; return false; }
bool peq_preset_load(const char *name, int *ignored) { (void)name; if (ignored) *ignored = 0; return false; }
bool peq_preset_delete(const char *name) { (void)name; return false; }
int peq_preset_for_each(peq_preset_name_cb_t cb, void *user) { (void)cb; (void)user; return 0; }
int peq_response_tenths(int freq, int sample_rate) { (void)freq; (void)sample_rate; return 0; }
int eq_auto_headroom_tenths(void) { return 0; }

void mseb_set_enabled(bool enabled) { (void)enabled; }
bool mseb_get_enabled(void) { return false; }
void mseb_set_value(int band, int value) { (void)band; (void)value; }
int mseb_get_value(int band) { (void)band; return 0; }
void mseb_save(void) {}
int mseb_get_range(void) { return MSEB_RANGE_DEFAULT; }
void mseb_set_range(int range) { (void)range; }
void mseb_reset(void) {}
void mseb_presets_set_dir(const char *sd_root) { (void)sd_root; }
bool mseb_preset_save(const char *name) { (void)name; return false; }
bool mseb_preset_load(const char *name) { (void)name; return false; }
bool mseb_preset_delete(const char *name) { (void)name; return false; }
int mseb_preset_for_each(mseb_preset_cb_t cb, void *user) { (void)cb; (void)user; return 0; }

void eq_process(short *frames, int frame_count, int channels, int sample_rate) {
	(void)frames; (void)frame_count; (void)channels; (void)sample_rate;
}
void eq_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate) {
	(void)frames; (void)frame_count; (void)channels; (void)sample_rate;
}

// Soundfield. The stock module's arithmetic, kept in integers: the original
// normalises to float, does mid/side, and scales back; the same result comes
// out of
//
//     sum = L + R          d = (R - L) * width
//     L'  = (sum - d) / 2  R' = (sum + d) / 2
//
// with `width` in hundredths, which is exact at width = 1.00 (L' = L, R' = R)
// rather than merely very close -- a "no effect" setting that quietly dithers
// the bottom bit is worse than no module at all.
// ---------------------------------------------------------------------------

void soundfield_set_enabled(bool enabled) {
	sf_enabled = enabled ? 1 : 0;
	config_set_int("soundfield", "enabled", sf_enabled);
	config_save();
}

bool soundfield_get_enabled(void) { return sf_enabled != 0; }

void soundfield_set_width(int hundredths) {
	sf_width = clamp_width(hundredths);
	config_set_int("soundfield", "width", sf_width);
	config_save();
}

int soundfield_get_width(void) { return sf_width; }

void balance_set_enabled(bool enabled) {
	bal_enabled = enabled ? 1 : 0;
	config_set_int("balance", "enabled", bal_enabled);
	config_save();
}

bool balance_get_enabled(void) { return bal_enabled != 0; }

void balance_set_value(int tenths_db) {
	balance_set_gains(clamp_balance(tenths_db));
	config_set_int("balance", "value", bal_tenths);
	config_save();
}

int balance_get_value(void) { return bal_tenths; }

void balance_process(short *frames, int frame_count, int channels) {
	if (!bal_enabled || bal_tenths == BALANCE_CENTRE || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	int gl = bal_left_q15;
	int gr = bal_right_q15;
	for (int n = 0; n < frame_count; n++) {
		// Attenuation only, so the product cannot leave the 16-bit range and
		// there is nothing to clamp.
		frames[n * 2] = (short)((frames[n * 2] * gl) >> 15);
		frames[n * 2 + 1] = (short)((frames[n * 2 + 1] * gr) >> 15);
	}
}

void balance_process_s32(int32_t *frames, int frame_count, int channels) {
	if (!bal_enabled || bal_tenths == BALANCE_CENTRE || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	int64_t gl = bal_left_q15;
	int64_t gr = bal_right_q15;
	for (int n = 0; n < frame_count; n++) {
		frames[n * 2] = (int32_t)((frames[n * 2] * gl) >> 15);
		frames[n * 2 + 1] = (int32_t)((frames[n * 2 + 1] * gr) >> 15);
	}
}

void mono_set_enabled(bool enabled) {
	mono_enabled = enabled ? 1 : 0;
	config_set_int("audio", "mono", mono_enabled);
	config_save();
}

bool mono_get_enabled(void) { return mono_enabled != 0; }

// The mean of the two channels, rounded towards zero: it cannot leave the
// range of either.
void mono_process(short *frames, int frame_count, int channels) {
	if (!mono_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	for (int n = 0; n < frame_count; n++) {
		short m = (short)(((int)frames[n * 2] + (int)frames[n * 2 + 1]) / 2);
		frames[n * 2] = m;
		frames[n * 2 + 1] = m;
	}
}

void mono_process_s32(int32_t *frames, int frame_count, int channels) {
	if (!mono_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	for (int n = 0; n < frame_count; n++) {
		int32_t m = (int32_t)(((int64_t)frames[n * 2] + (int64_t)frames[n * 2 + 1]) / 2);
		frames[n * 2] = m;
		frames[n * 2 + 1] = m;
	}
}

void soundfield_process(short *frames, int frame_count, int channels) {
	int w = sf_width;
	// Stereo only, exactly as the original: there is no width to speak of in a
	// mono stream, and the module has nothing to say about anything else.
	if (!sf_enabled || w == 100 || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	for (int n = 0; n < frame_count; n++) {
		int l = frames[n * 2];
		int r = frames[n * 2 + 1];
		int sum = l + r;
		int d = ((r - l) * w) / 100;

		int out_l = (sum - d) / 2;
		int out_r = (sum + d) / 2;

		if (out_l > 32767) {
			out_l = 32767;
		} else if (out_l < -32768) {
			out_l = -32768;
		}
		if (out_r > 32767) {
			out_r = 32767;
		} else if (out_r < -32768) {
			out_r = -32768;
		}

		frames[n * 2] = (short)out_l;
		frames[n * 2 + 1] = (short)out_r;
	}
}

void soundfield_process_s32(int32_t *frames, int frame_count, int channels) {
	int w = sf_width;
	if (!sf_enabled || w == 100 || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}

	for (int n = 0; n < frame_count; n++) {
		// 64-bit throughout: L + R already overflows 32 bits, and the
		// difference times a width of 2.00 overflows it twice over.
		int64_t l = frames[n * 2];
		int64_t r = frames[n * 2 + 1];
		int64_t sum = l + r;
		int64_t d = ((r - l) * w) / 100;

		int64_t out_l = (sum - d) / 2;
		int64_t out_r = (sum + d) / 2;

		if (out_l > INT32_MAX) {
			out_l = INT32_MAX;
		} else if (out_l < INT32_MIN) {
			out_l = INT32_MIN;
		}
		if (out_r > INT32_MAX) {
			out_r = INT32_MAX;
		} else if (out_r < INT32_MIN) {
			out_r = INT32_MIN;
		}

		frames[n * 2] = (int32_t)out_l;
		frames[n * 2 + 1] = (int32_t)out_r;
	}
}

// ---------------------------------------------------------------------------
// Crossfeed -- see eq.h for what it does and why.
// ---------------------------------------------------------------------------

// The expensive part (one exp and two divisions) runs once, never from the
// middle of a buffer: only when something actually changed.
static void crossfeed_prepare(int sample_rate) {
	if (sample_rate <= 0) {
		return;
	}
	if (cf_ready_rate == sample_rate && cf_ready_cutoff == cf_cutoff && cf_ready_delay_us == cf_delay_us) {
		return;
	}

	cf_ready_rate = sample_rate;
	cf_ready_cutoff = cf_cutoff;
	cf_ready_delay_us = cf_delay_us;

	// One-pole low-pass: y += alpha * (x - y), with
	// alpha = 1 - exp(-2*pi*fc/fs). The simplest filter there is, and enough
	// here -- the head is not a steep filter either.
	double alpha = 1.0 - exp(-2.0 * 3.14159265358979 * (double)cf_cutoff / (double)sample_rate);
	if (alpha > 1.0) {
		alpha = 1.0;
	}
	if (alpha < 0.0) {
		alpha = 0.0;
	}
	cf_alpha_q15 = (int)(alpha * 32768.0 + 0.5);

	int samples = (int)(((long long)cf_delay_us * sample_rate + 500000) / 1000000);
	if (samples < 0) {
		samples = 0;
	}
	if (samples > CF_DELAY_MAX_SAMPLES - 1) {
		samples = CF_DELAY_MAX_SAMPLES - 1;
	}
	cf_delay_samples = samples;

	// Strength and normalisation. A mono signal (L == R) would come out at
	// 1 + feed, so everything is divided by that: switching crossfeed on does
	// not raise the level or push a track already at full scale into clipping.
	double feed = (double)cf_level / 100.0;
	double norm = 1.0 / (1.0 + feed);
	cf_direct_q15 = (int)(norm * 32768.0 + 0.5);
	cf_feed_q15 = (int)(feed * norm * 32768.0 + 0.5);
}

void crossfeed_reset(void) {
	memset(&cf_left, 0, sizeof(cf_left));
	memset(&cf_right, 0, sizeof(cf_right));
}

void crossfeed_set_enabled(bool enabled) {
	cf_enabled = enabled ? 1 : 0;
	if (cf_enabled) {
		crossfeed_reset(); // no tail of the previous track when it comes back on
	}
	config_set_int("crossfeed", "enabled", cf_enabled);
	config_save();
}

bool crossfeed_get_enabled(void) { return cf_enabled != 0; }

void crossfeed_set_level(int percent) {
	cf_level = clamp_step(percent, CROSSFEED_LEVEL_MIN, CROSSFEED_LEVEL_MAX, CROSSFEED_LEVEL_STEP);
	cf_ready_rate = 0; // force the recompute
	config_set_int("crossfeed", "level", cf_level);
	config_save();
}

int crossfeed_get_level(void) { return cf_level; }

void crossfeed_set_cutoff(int hz) {
	cf_cutoff = clamp_step(hz, CROSSFEED_CUTOFF_MIN, CROSSFEED_CUTOFF_MAX, CROSSFEED_CUTOFF_STEP);
	cf_ready_rate = 0;
	config_set_int("crossfeed", "cutoff", cf_cutoff);
	config_save();
}

int crossfeed_get_cutoff(void) { return cf_cutoff; }

void crossfeed_set_delay(int microseconds) {
	cf_delay_us = clamp_step(microseconds, CROSSFEED_DELAY_MIN, CROSSFEED_DELAY_MAX, CROSSFEED_DELAY_STEP);
	cf_ready_rate = 0;
	config_set_int("crossfeed", "delay_us", cf_delay_us);
	config_save();
}

int crossfeed_get_delay(void) { return cf_delay_us; }

// One sample into the line, one out of the other end, low-passed. A delay of
// zero samples means no trip around the head: the sample itself is read back,
// not the previous one.
static inline int32_t cf_step(cf_channel_t *c, int32_t sample) {
	c->line[c->pos] = sample;
	int read = c->pos - cf_delay_samples;
	if (read < 0) {
		read += CF_DELAY_MAX_SAMPLES;
	}
	int32_t delayed = c->line[read];
	c->pos++;
	if (c->pos >= CF_DELAY_MAX_SAMPLES) {
		c->pos = 0;
	}
	c->lp += (int32_t)(((int64_t)(delayed - c->lp) * cf_alpha_q15) >> 15);
	return c->lp;
}

void crossfeed_process(short *frames, int frame_count, int channels, int sample_rate) {
	if (!cf_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	crossfeed_prepare(sample_rate);

	for (int n = 0; n < frame_count; n++) {
		int32_t l = frames[n * 2];
		int32_t r = frames[n * 2 + 1];

		// Both channels are filtered before the mix: what reaches an ear is the
		// opposite channel as it was, not an already mixed signal.
		int32_t lp_l = cf_step(&cf_left, l);
		int32_t lp_r = cf_step(&cf_right, r);

		int32_t out_l = (int32_t)(((int64_t)l * cf_direct_q15 + (int64_t)lp_r * cf_feed_q15) >> 15);
		int32_t out_r = (int32_t)(((int64_t)r * cf_direct_q15 + (int64_t)lp_l * cf_feed_q15) >> 15);

		if (out_l > 32767) {
			out_l = 32767;
		}
		if (out_l < -32768) {
			out_l = -32768;
		}
		if (out_r > 32767) {
			out_r = 32767;
		}
		if (out_r < -32768) {
			out_r = -32768;
		}

		frames[n * 2] = (short)out_l;
		frames[n * 2 + 1] = (short)out_r;
	}
}

void crossfeed_process_s32(int32_t *frames, int frame_count, int channels, int sample_rate) {
	if (!cf_enabled || channels != 2 || frame_count <= 0 || !frames) {
		return;
	}
	crossfeed_prepare(sample_rate);

	// At 32 bits every intermediate stays in 64: the sample already fills most
	// of the word, and multiplying it by a Q15 factor would overflow it.
	for (int n = 0; n < frame_count; n++) {
		int64_t l = frames[n * 2];
		int64_t r = frames[n * 2 + 1];

		// The delay line holds the shifted-down version: the low-pass works on
		// smaller numbers and full resolution buys nothing here, since this
		// signal is attenuated and summed under the direct one anyway.
		int32_t lp_l = cf_step(&cf_left, (int32_t)(l >> 8));
		int32_t lp_r = cf_step(&cf_right, (int32_t)(r >> 8));

		// Multiplied by 256 rather than shifted left by eight: left-shifting a
		// negative value is undefined behaviour in C, and samples are negative
		// half the time. The compiler emits a shift either way.
		int64_t out_l = (l * cf_direct_q15 + (int64_t)lp_r * 256 * cf_feed_q15) >> 15;
		int64_t out_r = (r * cf_direct_q15 + (int64_t)lp_l * 256 * cf_feed_q15) >> 15;

		if (out_l > INT32_MAX) {
			out_l = INT32_MAX;
		}
		if (out_l < INT32_MIN) {
			out_l = INT32_MIN;
		}
		if (out_r > INT32_MAX) {
			out_r = INT32_MAX;
		}
		if (out_r < INT32_MIN) {
			out_r = INT32_MIN;
		}

		frames[n * 2] = (int32_t)out_l;
		frames[n * 2 + 1] = (int32_t)out_r;
	}
}
