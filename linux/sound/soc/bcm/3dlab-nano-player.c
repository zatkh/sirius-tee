/*
 * 3Dlab Nano Player ALSA SoC Audio driver.
 *
 * Copyright (C) 2018 3Dlab.
 *
 * Author: GT <dev@3d-lab-av.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>

#define NANO_ID		0x00
#define NANO_VER	0x01
#define NANO_CFG	0x02
#define NANO_STATUS	0x03
#define NANO_SPI_ADDR	0x04
#define NANO_SPI_DATA	0x05

#define NANO_ID_VAL	0x3D
#define NANO_CFG_OFF	0x00
#define NANO_CFG_MULT1	0
#define NANO_CFG_MULT2	1
#define NANO_CFG_MULT4	2
#define NANO_CFG_MULT8	3
#define NANO_CFG_MULT16	4
#define NANO_CFG_CLK22	0
#define NANO_CFG_CLK24	BIT(3)
#define NANO_CFG_DSD	BIT(4)
#define NANO_CFG_ENA	BIT(5)
#define NANO_CFG_BLINK	BIT(6)
#define NANO_STATUS_P1  BIT(0)
#define NANO_STATUS_P2  BIT(1)
#define NANO_STATUS_FLG BIT(2)
#define NANO_STATUS_CLK BIT(3)
#define NANO_SPI_READ	0
#define NANO_SPI_WRITE	BIT(5)

#define NANO_DAC_CTRL1	0x00
#define NANO_DAC_CTRL2	0x01
#define NANO_DAC_CTRL3	0x02
#define NANO_DAC_LATT	0x03
#define NANO_DAC_RATT	0x04

#define NANO_CTRL2_VAL	0x22

static int nano_player_spi_write(struct regmap *map,
				 unsigned int reg, unsigned int val)
{
	/* indirect register access */
	regmap_write(map, NANO_SPI_DATA, val);
	regmap_write(map, NANO_SPI_ADDR, reg | NANO_SPI_WRITE);
	return 0;
}

static int nano_player_ctrl_info(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_info *uinfo)
{
	/* describe control element */
	if (strstr(kcontrol->id.name, "Volume")) {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 100;
	} else {
		uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
		uinfo->count = 1;
		uinfo->value.integer.min = 0;
		uinfo->value.integer.max = 1;
	}

	return 0;
}

static int nano_player_ctrl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	/* program control value to hardware */
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct regmap *regmap = snd_soc_card_get_drvdata(card);

	if (strstr(kcontrol->id.name, "Volume")) {
		unsigned int vol = ucontrol->value.integer.value[0];
		unsigned int att = 255 - (2 * (100 - vol));

		nano_player_spi_write(regmap, NANO_DAC_LATT, att);
		nano_player_spi_write(regmap, NANO_DAC_RATT, att);
		kcontrol->private_value = vol;
	} else {
		unsigned int mute = ucontrol->value.integer.value[0];
		unsigned int reg = NANO_CTRL2_VAL | mute;

		nano_player_spi_write(regmap, NANO_DAC_CTRL2, reg);
		kcontrol->private_value = mute;
	}
	return 0;
}

static int nano_player_ctrl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	/* return last programmed value */
	ucontrol->value.integer.value[0] = kcontrol->private_value;
	return 0;
}

#define SOC_NANO_PLAYER_CTRL(xname) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.info = nano_player_ctrl_info, \
	.put = nano_player_ctrl_put, \
	.get = nano_player_ctrl_get }

static const struct snd_kcontrol_new nano_player_controls[] = {
	SOC_NANO_PLAYER_CTRL("Master Playback Volume"),
	SOC_NANO_PLAYER_CTRL("Master Playback Switch"),
};

static const unsigned int nano_player_rates[] = {
	44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000,
	705600, 768000 /* only possible with fast clocks */
};

static struct snd_pcm_hw_constraint_list nano_player_constraint_rates = {
	.list	= nano_player_rates,
	.count	= ARRAY_SIZE(nano_player_rates),
};

static int nano_player_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct regmap *regmap = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_stream *cpu = &rtd->cpu_dai->driver->playback;
	struct snd_soc_pcm_stream *codec = &rtd->codec_dai->driver->playback;
	unsigned int sample_bits = 32;
	unsigned int val;

	/* configure cpu dai */
	cpu->formats |= SNDRV_PCM_FMTBIT_DSD_U32_LE;
	cpu->rate_max = 768000;

	/* configure dummy codec dai */
	codec->rate_min = 44100;
	codec->rates = SNDRV_PCM_RATE_KNOT;
	codec->formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_DSD_U32_LE;

	/* configure max supported rate */
	regmap_read(regmap, NANO_STATUS, &val);
	if (val & NANO_STATUS_CLK) {
		dev_notice(card->dev, "Board with fast clocks installed\n");
		codec->rate_max = 768000;
	} else {
		dev_notice(card->dev, "Board with normal clocks installed\n");
		codec->rate_max = 384000;
	}

	/* frame length enforced by hardware */
	return snd_soc_dai_set_bclk_ratio(rtd->cpu_dai, sample_bits * 2);
}

static int nano_player_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_RATE,
					  &nano_player_constraint_rates);
}

static int nano_player_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct regmap *regmap = snd_soc_card_get_drvdata(card);
	unsigned int config = NANO_CFG_ENA;
	struct snd_mask *fmt;

	/* configure PCM or DSD */
	fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	if (snd_mask_test(fmt, SNDRV_PCM_FORMAT_DSD_U32_LE)) {
		/* embed DSD in PCM data */
		snd_mask_none(fmt);
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_S32_LE);
		/* enable DSD mode */
		config |= NANO_CFG_DSD;
	}

	/* configure clocks */
	switch (params_rate(params)) {
	case 44100:
		config |= NANO_CFG_MULT1 | NANO_CFG_CLK22;
		break;
	case 88200:
		config |= NANO_CFG_MULT2 | NANO_CFG_CLK22;
		break;
	case 176400:
		config |= NANO_CFG_MULT4 | NANO_CFG_CLK22;
		break;
	case 352800:
		config |= NANO_CFG_MULT8 | NANO_CFG_CLK22;
		break;
	case 705600:
		config |= NANO_CFG_MULT16 | NANO_CFG_CLK22;
		break;
	case 48000:
		config |= NANO_CFG_MULT1 | NANO_CFG_CLK24;
		break;
	case 96000:
		config |= NANO_CFG_MULT2 | NANO_CFG_CLK24;
		break;
	case 192000:
		config |= NANO_CFG_MULT4 | NANO_CFG_CLK24;
		break;
	case 384000:
		config |= NANO_CFG_MULT8 | NANO_CFG_CLK24;
		break;
	case 768000:
		config |= NANO_CFG_MULT16 | NANO_CFG_CLK24;
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(card->dev, "Send CFG register 0x%02X\n", config);
	return regmap_write(regmap, NANO_CFG, config);
}

static struct snd_soc_ops nano_player_ops = {
	.startup	= nano_player_startup,
	.hw_params	= nano_player_hw_params,
};

static struct snd_soc_dai_link nano_player_link = {
	.name		= "3Dlab Nano Player",
	.stream_name	= "3Dlab Nano Player HiFi",
	.platform_name	= "bcm2708-i2s.0",
	.cpu_dai_name	= "bcm2708-i2s.0",
	.codec_name	= "snd-soc-dummy",
	.codec_dai_name	= "snd-soc-dummy-dai",
	.dai_fmt	= SND_SOC_DAIFMT_I2S |
			  SND_SOC_DAIFMT_CONT |
			  SND_SOC_DAIFMT_NB_NF |
			  SND_SOC_DAIFMT_CBM_CFM,
	.init		= nano_player_init,
	.ops		= &nano_player_ops,
};

static const struct regmap_config nano_player_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 128,
	.cache_type	= REGCACHE_RBTREE,
};

static int nano_player_card_probe(struct snd_soc_card *card)
{
	struct regmap *regmap = snd_soc_card_get_drvdata(card);
	unsigned int val;

	/* check hardware integrity */
	regmap_read(regmap, NANO_ID, &val);
	if (val != NANO_ID_VAL) {
		dev_err(card->dev, "Invalid ID register 0x%02X\n", val);
		return -ENODEV;
	}

	/* report version to the user */
	regmap_read(regmap, NANO_VER, &val);
	dev_notice(card->dev, "Started 3Dlab Nano Player driver (v%d)\n", val);

	/* enable internal audio bus and blink status LED */
	return regmap_write(regmap, NANO_CFG, NANO_CFG_ENA | NANO_CFG_BLINK);
}

static int nano_player_card_remove(struct snd_soc_card *card)
{
	/* disable internal audio bus */
	struct regmap *regmap = snd_soc_card_get_drvdata(card);

	return regmap_write(regmap, NANO_CFG, NANO_CFG_OFF);
}

static struct snd_soc_card nano_player_card = {
	.name		= "3Dlab_Nano_Player",
	.owner		= THIS_MODULE,
	.dai_link	= &nano_player_link,
	.num_links	= 1,
	.controls	= nano_player_controls,
	.num_controls	= ARRAY_SIZE(nano_player_controls),
	.probe		= nano_player_card_probe,
	.remove		= nano_player_card_remove,
};

static int nano_player_i2c_probe(struct i2c_client *i2c,
				 const struct i2c_device_id *id)
{
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &nano_player_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to init regmap %d\n", ret);
		return ret;
	}

	if (i2c->dev.of_node) {
		struct snd_soc_dai_link *dai = &nano_player_link;
		struct device_node *node;

		/* cpu handle configured by device tree */
		node = of_parse_phandle(i2c->dev.of_node, "i2s-controller", 0);
		if (node) {
			dai->platform_name = NULL;
			dai->platform_of_node = node;
			dai->cpu_dai_name = NULL;
			dai->cpu_of_node = node;
		}
	}

	nano_player_card.dev = &i2c->dev;
	snd_soc_card_set_drvdata(&nano_player_card, regmap);
	ret = devm_snd_soc_register_card(&i2c->dev, &nano_player_card);

	if (ret && ret != -EPROBE_DEFER)
		dev_err(&i2c->dev, "Failed to register card %d\n", ret);

	return ret;
}

static const struct of_device_id nano_player_of_match[] = {
	{ .compatible = "3dlab,nano-player", },
	{ }
};
MODULE_DEVICE_TABLE(of, nano_player_of_match);

static const struct i2c_device_id nano_player_i2c_id[] = {
	{ "nano-player", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nano_player_i2c_id);

static struct i2c_driver nano_player_i2c_driver = {
	.probe		= nano_player_i2c_probe,
	.id_table	= nano_player_i2c_id,
	.driver		= {
		.name		= "nano-player",
		.owner		= THIS_MODULE,
		.of_match_table	= nano_player_of_match,
	},
};

module_i2c_driver(nano_player_i2c_driver);

MODULE_DESCRIPTION("ASoC 3Dlab Nano Player driver");
MODULE_AUTHOR("GT <dev@3d-lab-av.com>");
MODULE_LICENSE("GPL v2");

/* EOF */
