/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / BPG image decoder filter
 *  based on libbpg (https://bellard.org/bpg/) - decode-only: this
 *  filter links libbpg's own bundled HEVC decoder (a stripped-down
 *  libavcodec, decoder-only, no x265/JCTVC encoder code), since BPG's
 *  HEVC payload is a reduced format (synthetic SPS/PPS reconstructed
 *  from BPG's compact header, proprietary NAL framing) that only
 *  libbpg's own code correctly parses - unlike AVIF/HEVC-in-ISOBMFF,
 *  there is no standard container to hand off to a generic decoder
 *  such as this repo's libde265-based filter.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>

#include <bpg/libbpg.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_BPGDecCtx;

static GF_Err bpgdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_BPGDecCtx *ctx = (GF_BPGDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	/* PIXFMT must be set here, not just after decode in process(): under
	 * solver_1, GPAC's writegen output resolution needs it on the PID
	 * before any data flows, or it fails with "No suitable filter to
	 * adapt caps" even though the value gets overwritten (RGB vs RGBA,
	 * once has_alpha is known) once decoding actually starts. */
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	return GF_OK;
}

static Bool bpgdec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_BPGDecCtx *ctx = (GF_BPGDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err bpgdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size;
	BPGDecoderContext *bpg;
	BPGImageInfo info;
	BPGDecoderOutputFormat out_fmt;
	u32 bpp, w, h, y, out_size, stride;
	u8 *output;
	GF_FilterPacket *dst_pck;
	GF_BPGDecCtx *ctx = (GF_BPGDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	bpg = bpg_decoder_open();
	if (!bpg)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	if (bpg_decoder_decode(bpg, data, (int)size) < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[BPGDec] Failed to decode BPG image\n"));
		bpg_decoder_close(bpg);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_drop_packet(ctx->ipid);

	if (bpg_decoder_get_info(bpg, &info) < 0)
	{
		bpg_decoder_close(bpg);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	out_fmt = info.has_alpha ? BPG_OUTPUT_FORMAT_RGBA32 : BPG_OUTPUT_FORMAT_RGB24;
	if (bpg_decoder_start(bpg, out_fmt) < 0)
	{
		bpg_decoder_close(bpg);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	w = info.width;
	h = info.height;
	bpp = info.has_alpha ? 4 : 3;
	stride = w * bpp;
	out_size = stride * h;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(stride));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(info.has_alpha ? GF_PIXEL_RGBA : GF_PIXEL_RGB));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		bpg_decoder_close(bpg);
		return GF_OUT_OF_MEM;
	}

	for (y = 0; y < h; y++)
	{
		bpg_decoder_get_line(bpg, output + y * stride);
	}
	bpg_decoder_close(bpg);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void bpgdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability BPGDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "bpg"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/bpg|image/x-bpg"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister BPGDecoderRegister = {
	.name = "bpgdec",
	GF_FS_SET_DESCRIPTION("BPG image decoder")
		GF_FS_SET_HELP("This filter decodes BPG (\"Better Portable Graphics\") images using libbpg's own bundled HEVC decoder.")
			.private_size = sizeof(GF_BPGDecCtx),
	SETCAPS(BPGDecCaps),
	.configure_pid = bpgdec_configure_pid,
	.process = bpgdec_process,
	.process_event = bpgdec_process_event,
	.finalize = bpgdec_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE bpgdec_register(GF_FilterSession *session)
{
	return &BPGDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_bpgdec(void) {
    gf_filter_auto_register("bpgdec", bpgdec_register);
}
