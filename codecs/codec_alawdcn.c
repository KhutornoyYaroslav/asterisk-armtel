/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief codec_alawdcn.c - translate between signed linear and alawdcn
 * 
 * \ingroup codecs
 */

/*** MODULEINFO
	<support_level>core</support_level>
 ***/

#include "asterisk.h"


ASTERISK_FILE_VERSION(__FILE__, "$Revision: 328259 $")

#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/translate.h"
#include "asterisk/alaw.h"
#include "asterisk/utils.h"

#define BUFFER_SAMPLES   8096	/* size for the translation buffers */

/* Sample frame data */
#include "asterisk/slin.h"
#include "alawdcn.h"

/*! \brief decode frame into lin and fill output buffer. */
static int alawtolin16_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	int i = f->samples;
	unsigned char *src = f->data.ptr;
	int16_t *dst = pvt->outbuf.i16 + pvt->samples;

	pvt->samples += i;
	pvt->datalen += i * 2;	/* 2 bytes/sample */
	
	while (i--)
		*dst++ = AST_ALAW(*src++);

	return 0;
}

/*! \brief convert and store input samples in output buffer */
static int lin16toalaw_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	int i = f->samples;
	char *dst = pvt->outbuf.c + pvt->samples;
	int16_t *src = f->data.ptr;

	pvt->samples += i;
	pvt->datalen += i;	/* 1 byte/sample */

	while (i--) 
		*dst++ = AST_LIN2A(*src++);

	return 0;
}

static struct ast_translator alawtolin16 = {
	.name = "alawdcntolin16",
	.framein = alawtolin16_framein,
	.sample = alaw_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator lin16toalaw = {
	"lin16toalawdcn",
	.framein = lin16toalaw_framein,
	.sample = slin16_sample,
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES,
};

/*! \brief standard module stuff */

static int reload(void)
{
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int res;

	res = ast_unregister_translator(&lin16toalaw);
	res |= ast_unregister_translator(&alawtolin16);

	return res;
}

static int load_module(void)
{
	int res;

	ast_format_set(&lin16toalaw.src_format, AST_FORMAT_SLINEAR16, 0);
	ast_format_set(&lin16toalaw.dst_format, AST_FORMAT_ALAWDCN, 0);

	ast_format_set(&alawtolin16.src_format, AST_FORMAT_ALAWDCN, 0);
	ast_format_set(&alawtolin16.dst_format, AST_FORMAT_SLINEAR16, 0);

	res = ast_register_translator(&alawtolin16);
	if (!res)
		res = ast_register_translator(&lin16toalaw);
	else
		ast_unregister_translator(&alawtolin16);
	if (res)
		return AST_MODULE_LOAD_FAILURE;
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "A-law(Armtel DCN) Coder/Decoder",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
