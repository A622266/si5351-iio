/*
 *
 * Licensed under the GPL-2.
 * Based on the si5351-clk driver
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rational.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/regulator/consumer.h>
#include <asm/unaligned.h>
#include <asm/div64.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "si5351_defs.h"
#include "si5351-iio.h"

static ssize_t si5351_write_ext(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    const char *buf, size_t len)
{
	struct si5351_state *st = iio_priv(indio_dev);
	struct i2c_client *i2c = to_i2c_client(st->dev);
	unsigned long long readin;
	unsigned int phase, new_freq, new_phase;
	int ret;

	ret = kstrtoull(buf, 10, &readin);
	if (ret)
		return ret;

	mutex_lock(&indio_dev->mlock);
	switch ((u32)private) {
	case SI5351_FREQ:
		if (st->quad_mode)
		{
			st->fVCO = si5351_retune_pll_and_config_msynth_quad(i2c, PLL_A, st->xtal_rate, (unsigned int)readin, &new_freq, &new_phase);
			si5351_ctrl_msynth(i2c, 0, 1, SI5351_CLK_INPUT_MULTISYNTH_N, SI5351_CLK_DRIVE_STRENGTH_8MA, 0);
			si5351_ctrl_msynth(i2c, 1, 1, SI5351_CLK_INPUT_MULTISYNTH_N, SI5351_CLK_DRIVE_STRENGTH_8MA, 0);
		}
		else
		{
			ret = si5351_config_msynth_phase(i2c, chan->channel, PLL_A, (unsigned int)readin, st->fVCO, st->phase_cache[chan->channel], &new_freq, &new_phase);
			si5351_ctrl_msynth(i2c, chan->channel, 1, SI5351_CLK_INPUT_MULTISYNTH_N, SI5351_CLK_DRIVE_STRENGTH_8MA, 0);
		}
		ret = 0;
		break;
	case SI5351_PHASE:
		if (st->quad_mode)
			ret = -EINVAL;
		else
		{
			if (readin<180)
				phase = (unsigned int)readin;
			else
				phase = (unsigned int)(readin-180);
			ret = si5351_config_msynth_phase(i2c, chan->channel, PLL_A, st->freq_cache[chan->channel], st->fVCO, phase, &new_freq, &new_phase);
			si5351_ctrl_msynth(i2c, chan->channel, 1, SI5351_CLK_INPUT_MULTISYNTH_N, SI5351_CLK_DRIVE_STRENGTH_8MA, (readin<180)?0:1);
			if (!(readin<180))
				new_phase += 180;
			ret = 0;
		}
		break;
	default:
		ret = -EINVAL;
	}

	if (ret == 0)
	{
		if (st->quad_mode)
		{
			st->freq_cache[0] = new_freq;
			st->freq_cache[1] = new_freq;
			st->phase_cache[0] = 0;
			st->phase_cache[1] = new_phase;
		}
		else
		{
			st->freq_cache[chan->channel] = new_freq;
			st->phase_cache[chan->channel] = new_phase;
		}
	}
	mutex_unlock(&indio_dev->mlock);

	return ret ? ret : len;
}

static ssize_t si5351_read_ext(struct iio_dev *indio_dev,
				   uintptr_t private,
				   const struct iio_chan_spec *chan,
				   char *buf)
{
	struct si5351_state *st = iio_priv(indio_dev);
	unsigned long long val;
	int ret = 0;

	mutex_lock(&indio_dev->mlock);
	switch ((u32)private) {
	case SI5351_FREQ:
		val = st->freq_cache[chan->channel];
		break;
	case SI5351_PHASE:
		val = st->phase_cache[chan->channel];
		break;
	default:
		ret = -EINVAL;
		val = 0;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret < 0 ? ret : sprintf(buf, "%llu\n", val);
}



static const struct iio_info si5351_info = {
};

static const struct iio_chan_spec_ext_info si5351_ext_info[] = {
{ \
	.name = "frequency", \
	.read = si5351_read_ext, \
	.write = si5351_write_ext, \
	.private = SI5351_FREQ, \
	.shared = IIO_SEPARATE, \
},
{ \
	.name = "phase", \
	.read = si5351_read_ext, \
	.write = si5351_write_ext, \
	.private = SI5351_PHASE, \
	.shared = IIO_SEPARATE, \
},
	{ },
};

#define SI5351_CHANNEL(chan, _ext_info) {		\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.output = 1,						\
	.channel = (chan),					\
	.info_mask_separate = 0,				\
	.ext_info = (_ext_info),				\
}

#define DECLARE_SI5351C_CHANNELS(name, ext_info) \
const struct iio_chan_spec name[] = { \
	SI5351_CHANNEL(0, ext_info), \
	SI5351_CHANNEL(1, ext_info), \
	SI5351_CHANNEL(2, ext_info), \
	SI5351_CHANNEL(3, ext_info), \
	SI5351_CHANNEL(4, ext_info), \
	SI5351_CHANNEL(5, ext_info), \
	SI5351_CHANNEL(6, ext_info), \
	SI5351_CHANNEL(7, ext_info), \
}

#define DECLARE_SI5351A_CHANNELS(name, ext_info) \
const struct iio_chan_spec name[] = { \
	SI5351_CHANNEL(0, ext_info), \
	SI5351_CHANNEL(1, ext_info), \
	SI5351_CHANNEL(2, ext_info), \
}


static DECLARE_SI5351A_CHANNELS(si5351a_channels, si5351_ext_info);
static DECLARE_SI5351C_CHANNELS(si5351c_channels, si5351_ext_info);

static const struct si5351_chip_info si5351_chip_info_tbl[] = {
	[ID_SI5351A] = {
		.channels = si5351a_channels,
		.num_channels = 3,
	},
	[ID_SI5351C] = {
		.channels = si5351c_channels,
		.num_channels = 8,
	},
};

static void si5351_write_parameters(struct i2c_client *i2c,
				    unsigned int start_reg, struct si5351_multisynth_parameters *params)
{
	u8 buf[SI5351_PARAMETERS_LENGTH];

	switch (start_reg) {
	case SI5351_CLK6_PARAMETERS:
	case SI5351_CLK7_PARAMETERS:
		buf[0] = params->p1 & 0xff;
		i2c_smbus_write_byte_data(i2c, start_reg, buf[0]);
		break;
	default:
		buf[0] = ((params->p3 & 0x0ff00) >> 8) & 0xff;
		buf[1] = params->p3 & 0xff;
		/* save rdiv and divby4 */
		buf[2] = i2c_smbus_read_byte_data(i2c, start_reg + 2) & ~0x03;
		buf[2] |= ((params->p1 & 0x30000) >> 16) & 0x03;
		buf[3] = ((params->p1 & 0x0ff00) >> 8) & 0xff;
		buf[4] = params->p1 & 0xff;
		buf[5] = ((params->p3 & 0xf0000) >> 12) |
			((params->p2 & 0xf0000) >> 16);
		buf[6] = ((params->p2 & 0x0ff00) >> 8) & 0xff;
		buf[7] = params->p2 & 0xff;
		dev_dbg(&i2c->dev, "si5351a-iio: writing %02x %02x %02x %02x %02x %02x %02x %02x at reg %d\n",buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],start_reg);
		i2c_smbus_write_i2c_block_data(i2c, start_reg, SI5351_PARAMETERS_LENGTH, buf);
	}
}

static int si5351_setup_pll(struct i2c_client *i2c, unsigned int pll, unsigned int fVCO, unsigned int fXTAL)
{
	struct si5351_multisynth_parameters params;
	unsigned long rfrac, denom, a, b, c;
		unsigned long long lltmp;
		int val;

		unsigned int start_reg = (pll == PLL_A) ? SI5351_PLLA_PARAMETERS : SI5351_PLLB_PARAMETERS;

		if (fVCO < SI5351_PLL_VCO_MIN)
			fVCO = SI5351_PLL_VCO_MIN;
		if (fVCO > SI5351_PLL_VCO_MAX)
			fVCO = SI5351_PLL_VCO_MAX;

		/* determine integer part of feedback equation */
		a = fVCO / fXTAL;

		if (a < SI5351_PLL_A_MIN)
			fVCO = fXTAL * SI5351_PLL_A_MIN;
		if (a > SI5351_PLL_A_MAX)
			fVCO = fXTAL * SI5351_PLL_A_MAX;

		/* find best approximation for b/c = fVCO mod fIN */
		denom = 1000 * 1000;
		lltmp = fVCO % fXTAL;
		lltmp *= denom;
		do_div(lltmp, fXTAL);
		rfrac = (unsigned long)lltmp;

		b = 0;
		c = 1;
		if (rfrac)
			rational_best_approximation(rfrac, denom,
					    SI5351_PLL_B_MAX, SI5351_PLL_C_MAX, &b, &c);

		/* calculate parameters */
		params.p3  = c;
		params.p2  = (128 * b) % c;
		params.p1  = 128 * a;
		params.p1 += (128 * b / c);
		params.p1 -= 512;

		/* recalculate rate by fIN * (a + b/c) */
		lltmp  = fXTAL;
		lltmp *= b;
		do_div(lltmp, c);

		fVCO  = (unsigned long)lltmp;
		fVCO += fXTAL * a;

		dev_dbg(&i2c->dev, "si5351-iio: found a=%lu, b=%lu, c=%lu\n", a,  b,  c);
		dev_dbg(&i2c->dev, "si5351-iio: found p1=%lu, p2=%lu, p3=%lu\n", params.p1, params.p2, params.p3);

		si5351_write_parameters(i2c, start_reg, &params);
		/* plla/pllb ctrl is in clk6/clk7 ctrl registers */
		val = i2c_smbus_read_byte_data(i2c, SI5351_CLK6_CTRL + pll);
		if (params.p2 == 0)
			val |= SI5351_CLK_INTEGER_MODE;
		else
			val &= ~SI5351_CLK_INTEGER_MODE;
		i2c_smbus_write_byte_data(i2c, SI5351_CLK6_CTRL + pll, val);

			/* Do a pll soft reset on the affected pll */
		i2c_smbus_write_byte_data(i2c, SI5351_PLL_RESET,
					 (pll == PLL_A) ? SI5351_PLL_RESET_A :
							    SI5351_PLL_RESET_B);
		return fVCO;

}

static inline u8 si5351_msynth_params_address(int num)
{
	if (num > 5)
		return SI5351_CLK6_PARAMETERS + (num - 6);
	return SI5351_CLK0_PARAMETERS + (SI5351_PARAMETERS_LENGTH * num);
}

static int si5351_config_msynth_phase(struct i2c_client *i2c, unsigned int output, unsigned int pll, unsigned int fout_target, const unsigned int fVCO, unsigned int phase_target, unsigned int *fout_real, unsigned int *phase_real)
{
	struct si5351_multisynth_parameters params;
	unsigned long a, b, c, phase_val, ultmp;
	unsigned long long lltmp;
	int divby4;
	u8 start_reg;
	int val;

	/* multisync6-7 can only handle freqencies < 150MHz */
	if (output >= 6 && fout_target > SI5351_MULTISYNTH67_MAX_FREQ)
		fout_target = SI5351_MULTISYNTH67_MAX_FREQ;

	/* multisync frequency is 1MHz .. 160MHz */
	if (fout_target > SI5351_MULTISYNTH_MAX_FREQ)
		fout_target = SI5351_MULTISYNTH_MAX_FREQ;
	if (fout_target < SI5351_MULTISYNTH_MIN_FREQ)
		fout_target = SI5351_MULTISYNTH_MIN_FREQ;

	divby4 = 0;
	if (fout_target > SI5351_MULTISYNTH_DIVBY4_FREQ)
		divby4 = 1;

	if (output >= 6) {
		/* determine the closest integer divider */
		a = DIV_ROUND_CLOSEST(fVCO, fout_target);
		if (a < SI5351_MULTISYNTH_A_MIN)
			a = SI5351_MULTISYNTH_A_MIN;
		if (a > SI5351_MULTISYNTH67_A_MAX)
			a = SI5351_MULTISYNTH67_A_MAX;

		b = 0;
		c = 1;
	} else {
		unsigned long rfrac, denom;

		/* disable divby4 */
		if (divby4) {
			fout_target = SI5351_MULTISYNTH_DIVBY4_FREQ;
			divby4 = 0;
		}

		/* determine integer part of divider equation */
		a = fVCO / fout_target;
		if (a < SI5351_MULTISYNTH_A_MIN)
			a = SI5351_MULTISYNTH_A_MIN;
		if (a > SI5351_MULTISYNTH_A_MAX)
			a = SI5351_MULTISYNTH_A_MAX;

		/* find best approximation for b/c = fVCO mod fOUT */
		denom = 1000 * 1000;
		lltmp = (fVCO) % fout_target;
		lltmp *= denom;
		do_div(lltmp, fout_target);
		rfrac = (unsigned long)lltmp;

		b = 0;
		c = 1;
		if (rfrac)
			rational_best_approximation(rfrac, denom,
			    SI5351_MULTISYNTH_B_MAX, SI5351_MULTISYNTH_C_MAX,
			    &b, &c);
	}
	if ((b==0) && (phase_target==0))
		params.intmode=1;
	else
		params.intmode=0;

	/* recalculate fout_target by fOUT = fIN / (a + b/c) */
	lltmp  = fVCO;
	lltmp *= c;
	do_div(lltmp, a * c + b);
	*fout_real  = (unsigned int)lltmp;
	//fout_real = f_VCO * c / (a*c + b)

	/* calculate parameters */
	if (divby4) {
		params.p3 = 1;
		params.p2 = 0;
		params.p1 = 0;
	} else if (output >= 6) {
		params.p3 = 0;
		params.p2 = 0;
		params.p1 = a;
	} else {
		params.p3  = c;
		params.p2  = (128 * b) % c;
		params.p1  = 128 * a;
		params.p1 += (128 * b / c);
		params.p1 -= 512;
	}

	lltmp = a*c + b;
	lltmp *= phase_target;
	do_div(lltmp, c * 90);
	phase_val = (unsigned long)lltmp;
	/*
	The formula for phase_val calculation is: phase_val = (fVCO / *fout_real) * phase_target / 90
	
	with fout_real = f_VCO * c / (a*c + b) from above we get
	
	phase_val = ((a*c + b) / c ) * phase_target / 90
	*/
	if (phase_val > 127)
	{
		dev_err(&i2c->dev, "si5351-iio: limiting phase_val from %lu to 127\n", phase_val);
		phase_val = 127;
	}
	lltmp = *fout_real;
	lltmp *= phase_val;
	lltmp *= 90;
	do_div(lltmp, fVCO);
	*phase_real = (unsigned int)lltmp;
	/*
	The chip implements phase shift by time shifting. The formula for the time shift is
       	Delta_t = phase_val / (4*fVCO)
	
	With the general relation phase = Delta_t * fout * 360 we get
	phase = phase_val * fout * 90 / fVCO
	*/

	dev_dbg(&i2c->dev, "si5351-iio: target freq=%u\n", fout_target);
	dev_dbg(&i2c->dev, "si5351-iio: target phase=%u\n", phase_target);
	dev_dbg(&i2c->dev, "si5351-iio: using fVCO=%u\n", fVCO);
	dev_dbg(&i2c->dev, "si5351-iio: found a=%lu, b=%lu, c=%lu\n", a, b, c);
	dev_dbg(&i2c->dev, "si5351-iio: found p1=%lu, p2=%lu, p3=%lu, divby4=%d\n", params.p1, params.p2, params.p3, divby4);
	dev_dbg(&i2c->dev, "si5351-iio: fout_real=%u\n", *fout_real);
	dev_dbg(&i2c->dev, "si5351-iio: phase_val=%lu\n",  phase_val);
	dev_dbg(&i2c->dev, "si5351-iio: phase_real=%lu\n",  *phase_real);

	start_reg = si5351_msynth_params_address(output);
	/* write multisynth parameters */
	si5351_write_parameters(i2c, start_reg, &params);

	if (fout_target > SI5351_MULTISYNTH_DIVBY4_FREQ)
		divby4 = 1;

	/* enable/disable integer mode and divby4 on multisynth0-5 */
	if (output < 6)
	{
		val = i2c_smbus_read_byte_data(i2c, start_reg + 2);
		if (divby4)
			val |= SI5351_OUTPUT_CLK_DIVBY4;
		else
			val &= ~SI5351_OUTPUT_CLK_DIVBY4;
		i2c_smbus_write_byte_data(i2c, start_reg + 2, val);

		val = i2c_smbus_read_byte_data(i2c, SI5351_CLK0_CTRL + output);
		if (params.intmode == 1)
			val |= SI5351_CLK_INTEGER_MODE;
		else
			val &= ~SI5351_CLK_INTEGER_MODE;
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + output, val);
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_PHASE_OFFSET + output, phase_val & 0x7F);
		dev_dbg(&i2c->dev, "si5351-iio: readback phase offset %d\n", i2c_smbus_read_byte_data(i2c, SI5351_CLK0_PHASE_OFFSET + output));
	}

	i2c_smbus_write_byte_data(i2c, SI5351_PLL_RESET,
						 (pll == PLL_A) ? SI5351_PLL_RESET_A :
								    SI5351_PLL_RESET_B);

	val = i2c_smbus_read_byte_data(i2c, SI5351_CLK0_CTRL + output);
	if (pll == PLL_B)
		val |= SI5351_CLK_PLL_SELECT;
	else
		val &= ~SI5351_CLK_PLL_SELECT;
	i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + output, val);

	dev_dbg(&i2c->dev, "si5351-iio: wrote CTRL byte %02x\n", val);

	return 0;
}


static unsigned int si5351_ctrl_msynth(struct i2c_client *i2c, unsigned int output, unsigned int enable, unsigned int input, unsigned int strength, unsigned int inversion)
{
	int val;
	unsigned int bits = 0, allmask = 0;

	allmask = SI5351_CLK_INPUT_MASK | SI5351_CLK_DRIVE_STRENGTH_MASK | SI5351_CLK_INVERT | SI5351_CLK_POWERDOWN;

	input &= SI5351_CLK_INPUT_MASK;
	bits |= input;

	strength &= SI5351_CLK_DRIVE_STRENGTH_MASK;
	bits |= strength;

	if (inversion!=0)
		bits |= SI5351_CLK_INVERT;

	if (enable==0)
		bits |= SI5351_CLK_POWERDOWN;

	if (output < 8)
	{
		val = i2c_smbus_read_byte_data(i2c, SI5351_CLK0_CTRL + output);
		val &= ~allmask; // remove all masked bits
		val |= bits; // set bits where needed
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + output, val);
		dev_dbg(&i2c->dev, "si5351-iio: wrote CTRL byte %02x\n", val);

		val = i2c_smbus_read_byte_data(i2c, SI5351_OUTPUT_ENABLE_CTRL);
		if (enable==1)
			val &= ~(1 << output);
		else
			val |=  (1 << output);
		i2c_smbus_write_byte_data(i2c, SI5351_OUTPUT_ENABLE_CTRL, val);
		dev_dbg(&i2c->dev, "si5351-iio: wrote OUTPUT ENABLE byte %02x\n", val);
	}

	return bits;
}

static int si5351_retune_pll_and_config_msynth_quad(struct i2c_client *i2c, unsigned int pll, unsigned int fXTAL, unsigned int fout_target, unsigned int *fout_real, unsigned int *phase_real)
{
	struct si5351_multisynth_parameters pll_params, msynth_params;
	unsigned long a, b, c, c_start, d;
	long b_start;
	unsigned long long lltmp;
	int val;
	unsigned long fVCO;
	unsigned int phase_val;
	int output;
	unsigned int fout_by_step;

	unsigned int start_reg = (pll == PLL_A) ? SI5351_PLLA_PARAMETERS : SI5351_PLLB_PARAMETERS;
		
	fout_by_step = fout_target / TUNE_STEP;

	a = 32;
	c_start = fXTAL / TUNE_STEP;
	d = SI5351_PLL_VCO_MIN / fout_target;
	while(d <= (SI5351_PLL_VCO_MAX / fout_target))
	{
		b_start = (fout_by_step * d) - (a * c_start);
		if ((b_start >= 0) && (b_start <= (c_start-1)))
			break;
		else
			d++;
	}

	if ((b_start < 0) || (b_start > (c_start-1)))
	{
		dev_err(&i2c->dev, "si5351-iio: can't tune to %u Hz\n", fout_target);
		b_start = 0;
	}

	b = 0;
	c = 1;
	rational_best_approximation(b_start, c_start, SI5351_PLL_B_MAX, SI5351_PLL_C_MAX, &b, &c);

	/* calculate parameters */
	pll_params.p3  = c;
	pll_params.p2  = (128 * b) % c;
	pll_params.p1  = 128 * a;
	pll_params.p1 += (128 * b / c);
	pll_params.p1 -= 512;

	/* recalculate rate by fIN * (a + b/c) */
	lltmp  = fXTAL;
	lltmp *= b;
	do_div(lltmp, c);

	fVCO  = (unsigned long)lltmp;
	fVCO += fXTAL * a;

	dev_dbg(&i2c->dev, "si5351-iio: found a=%lu, b=%lu, c=%lu\n", a, b, c);
	dev_dbg(&i2c->dev, "si5351-iio: found p1=%lu, p2=%lu, p3=%lu\n", pll_params.p1, pll_params.p2, pll_params.p3);

	si5351_write_parameters(i2c, start_reg, &pll_params);
	/* plla/pllb ctrl is in clk6/clk7 ctrl registers */
	val = i2c_smbus_read_byte_data(i2c, SI5351_CLK6_CTRL + pll);
	if (pll_params.p2 == 0)
		val |= SI5351_CLK_INTEGER_MODE;
	else
		val &= ~SI5351_CLK_INTEGER_MODE;
	i2c_smbus_write_byte_data(i2c, SI5351_CLK6_CTRL + pll, val);

	/* Do a pll soft reset on the affected pll */
	i2c_smbus_write_byte_data(i2c, SI5351_PLL_RESET, (pll == PLL_A) ? SI5351_PLL_RESET_A : SI5351_PLL_RESET_B);

	// msynth part starts here
	lltmp  = fVCO;
	do_div(lltmp, d);
	*fout_real  = (unsigned int)lltmp;

	/* calculate parameters */
	msynth_params.p3  = 1;
	msynth_params.p2  = 0;
	msynth_params.p1  = 128 * d;
	msynth_params.p1 -= 512;

	lltmp = fVCO;
	do_div(lltmp, *fout_real);
	phase_val = (unsigned int)lltmp;
	if (phase_val > 127)
	{
		phase_val = 127;
		dev_err(&i2c->dev, "si5351-iio: limiting phase_val to %u\n",  (unsigned int)phase_val);
	}
	lltmp = *fout_real;
	lltmp *= phase_val;
	lltmp *= 90;
	do_div(lltmp, fVCO);
	*phase_real = (unsigned int)lltmp;

	dev_dbg(&i2c->dev, "si5351-iio: using fVCO=%lu\n", fVCO);
	dev_dbg(&i2c->dev, "si5351-iio: found d=%lu\n", d);
	dev_dbg(&i2c->dev, "si5351-iio: found p1=%lu, p2=%lu, p3=%lu\n", msynth_params.p1, msynth_params.p2, msynth_params.p3);
	dev_dbg(&i2c->dev, "si5351-iio: fout_real=%u\n", *fout_real);
	dev_dbg(&i2c->dev, "si5351-iio: phase_val=%u\n", phase_val);

	/* enable/disable integer mode and divby4 on multisynth0-5 */
	for (output=0; 2 > output;++output)
	{
		start_reg = si5351_msynth_params_address(output);
		/* write multisynth parameters */
		si5351_write_parameters(i2c, start_reg, &msynth_params);

		val = i2c_smbus_read_byte_data(i2c, start_reg + 2);
		val &= ~SI5351_OUTPUT_CLK_DIVBY4;
		i2c_smbus_write_byte_data(i2c, start_reg + 2, val);

		val = i2c_smbus_read_byte_data(i2c, SI5351_CLK0_CTRL + output);
		val &= ~SI5351_CLK_INTEGER_MODE;
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + output, val);
		if(output==1)
			i2c_smbus_write_byte_data(i2c, SI5351_CLK0_PHASE_OFFSET + output, phase_val & 0x7F);
		else
			i2c_smbus_write_byte_data(i2c, SI5351_CLK0_PHASE_OFFSET + output, 0);

		dev_dbg(&i2c->dev, "si5351-iio: readback phase offset %d\n", i2c_smbus_read_byte_data(i2c, SI5351_CLK0_PHASE_OFFSET + output));
	}

	i2c_smbus_write_byte_data(i2c, SI5351_PLL_RESET, (pll == PLL_A) ? SI5351_PLL_RESET_A : SI5351_PLL_RESET_B);

	for (output=0; 2 > output;++output)
	{
		val = i2c_smbus_read_byte_data(i2c, SI5351_CLK0_CTRL + output);
		if (pll == PLL_B)
			val |= SI5351_CLK_PLL_SELECT;
		else
			val &= ~SI5351_CLK_PLL_SELECT;
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + output, val);

		dev_dbg(&i2c->dev, "si5351-iio: wrote CTRL byte %02x\n", val);
	}

		return fVCO;

}	

static void si5351_safe_defaults(struct i2c_client *i2c)
{
	int i;
	i2c_smbus_write_byte_data(i2c, SI5351_OUTPUT_ENABLE_CTRL, 0xFF);
	for(i=0;i<8;i++)
		i2c_smbus_write_byte_data(i2c, SI5351_CLK0_CTRL + i, 0x80);
	i2c_smbus_write_byte_data(i2c, SI5351_CRYSTAL_LOAD, SI5351_CRYSTAL_LOAD_10PF);

	return;
}

static int si5351_identify(struct i2c_client *client)
{
	int retval;

	retval = i2c_smbus_read_byte_data(client, 0);
	if (retval < 0)
		return retval;
	else
		return 0;
}

static int si5351_i2c_probe(struct i2c_client *i2c,	const struct i2c_device_id *id)
{
		struct iio_dev *indio_dev;
		struct device_node *np = i2c->dev.of_node;
		struct si5351_state *st;
		unsigned int i;
		int ret;

		if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		{
			dev_err(&i2c->dev, "I2C adapter not functional\n");
			return -EOPNOTSUPP;
		}
		dev_dbg(&i2c->dev, "si5351-iio: I2C adapter ok\n");
		ret = si5351_identify(i2c);
		if (ret < 0) {
			dev_err(&i2c->dev, "Si5351 not found: error %d\n", ret);
			return -ENODEV;
		}
		dev_dbg(&i2c->dev, "si5351-iio: I2C slave device found\n");
		indio_dev = devm_iio_device_alloc(&i2c->dev, sizeof(*st));
		if (indio_dev == NULL)
			return  -ENOMEM;


		st = iio_priv(indio_dev);
		dev_set_drvdata(&i2c->dev, indio_dev);

		st->chip_info = &si5351_chip_info_tbl[id->driver_data];
		st->dev = &i2c->dev;

		indio_dev->dev.parent = &i2c->dev;
		indio_dev->name = id->name;
		if (IS_ENABLED(CONFIG_OF) && np)
			of_property_read_string(np, "devname", &indio_dev->name);
		else
			dev_dbg(&i2c->dev, "using default name\n");

		st->xtal_rate = DEFAULT_XTAL_RATE;
		if (IS_ENABLED(CONFIG_OF) && np)
		{
			ret = of_property_read_u32(np, "xtal-freq", &st->xtal_rate);
			if(ret)
                		st->xtal_rate = DEFAULT_XTAL_RATE;
		}
		st->quad_mode = 0;
		if (IS_ENABLED(CONFIG_OF) && np)
		{
			if (of_property_read_bool(np, "quadrature-mode"))
                		st->quad_mode = 1;
		}
		if (st->quad_mode==1)
			dev_dbg(&i2c->dev, "si5351-iio: quadrature mode detected\n");

		indio_dev->info = &si5351_info;
		indio_dev->modes = INDIO_DIRECT_MODE;
		indio_dev->channels = st->chip_info->channels;
		indio_dev->num_channels = st->chip_info->num_channels;

		for (i = 0; i < st->chip_info->num_channels; ++i) {
			st->freq_cache[i] = 0;
			st->phase_cache[i] = 0;
		}

		ret = iio_device_register(indio_dev);

		si5351_safe_defaults(i2c);

		st->fVCO = si5351_setup_pll(i2c, PLL_A, 32*st->xtal_rate, st->xtal_rate);
		printk(KERN_INFO "si5351-iio: Si5351 detected, xtal freq = %d MHz, using PLL_A VCO freq = %d MHz\n", st->xtal_rate/1000000, st->fVCO/1000000);

		return 0;
}

static int si5351_i2c_remove(struct i2c_client *i2c)
{
		struct iio_dev *indio_dev = dev_get_drvdata(&i2c->dev);

		iio_device_unregister(indio_dev);
		
		return 0;
}

static const struct i2c_device_id si5351_i2c_ids[] = {
	{"si5351a", ID_SI5351A },
	{"si5351c", ID_SI5351C },
	{}
};
MODULE_DEVICE_TABLE(i2c, si5351_i2c_ids);

#ifdef CONFIG_OF
static const struct of_device_id si5351_of_i2c_match[] = {
	{ .compatible = "silabs,si5351a", .data = (void *)ID_SI5351A },
	{ .compatible = "silabs,si5351c", .data = (void *)ID_SI5351C },
	{ },
};
MODULE_DEVICE_TABLE(of, si5351_of_i2c_match);
#else
#define si5351_of_i2c_match NULL
#endif


static struct i2c_driver si5351_i2c_driver = {
	.driver = {
		   .name = "si5351",
	},
	.probe = si5351_i2c_probe,
	.remove = si5351_i2c_remove,
	.id_table = si5351_i2c_ids,
};

static int __init si5351_i2c_register_driver(void)
{
	return i2c_add_driver(&si5351_i2c_driver);
}

static void __exit si5351_i2c_unregister_driver(void)
{
	i2c_del_driver(&si5351_i2c_driver);
}


static int __init si5351_init(void)
{
	int ret;

	ret = si5351_i2c_register_driver();

	return 0;
}
module_init(si5351_init);

static void __exit si5351_exit(void)
{
	si5351_i2c_unregister_driver();
}
module_exit(si5351_exit);

MODULE_AUTHOR("Henning Paul <hnch@gmx.net>");
MODULE_DESCRIPTION("Si5351 IIO driver");
MODULE_LICENSE("GPL v2");
