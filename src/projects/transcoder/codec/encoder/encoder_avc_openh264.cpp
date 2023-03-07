//==============================================================================
//
//  Transcoder
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "encoder_avc_openh264.h"

#include <unistd.h>

#include "../../transcoder_private.h"

EncoderAVCxOpenH264::~EncoderAVCxOpenH264()
{
}

bool EncoderAVCxOpenH264::SetCodecParams()
{
	_codec_context->framerate = ::av_d2q((GetRefTrack()->GetFrameRate() > 0) ? GetRefTrack()->GetFrameRate() : GetRefTrack()->GetEstimateFrameRate(), AV_TIME_BASE);
	_codec_context->bit_rate = _codec_context->rc_min_rate = _codec_context->rc_max_rate = GetRefTrack()->GetBitrate();
	_codec_context->sample_aspect_ratio = ::av_make_q(1, 1);
	_codec_context->ticks_per_frame = 2;
	_codec_context->time_base = ::av_inv_q(::av_mul_q(_codec_context->framerate, (AVRational){_codec_context->ticks_per_frame, 1}));
	_codec_context->pix_fmt = (AVPixelFormat)GetSupportedFormat();
	_codec_context->width = GetRefTrack()->GetWidth();
	_codec_context->height = GetRefTrack()->GetHeight();

	// Set KeyFrame Interval
	_codec_context->gop_size = (GetRefTrack()->GetKeyFrameInterval() == 0) ? (_codec_context->framerate.num / _codec_context->framerate.den) : GetRefTrack()->GetKeyFrameInterval();

	// -1(Default) => FFMIN(FFMAX(4, av_cpu_count() / 3), 8) 
	// 0 => Auto
	// >1 => Set
	_codec_context->thread_count = GetRefTrack()->GetThreadCount() < 0 ? FFMIN(FFMAX(4, av_cpu_count() / 3), 8) : GetRefTrack()->GetThreadCount();
	_codec_context->slices = _codec_context->thread_count;

	::av_opt_set(_codec_context->priv_data, "coder", "default", 0);

	// Use the main/high profile to remove this log.
	//  'Warning:bEnableFrameSkip = 0,bitrate can't be controlled for RC_QUALITY_MODE,RC_BITRATE_MODE and RC_TIMESTAMP_MODE without enabling skip frame'
	::av_opt_set(_codec_context->priv_data, "allow_skip_frames", "false", 0);

	// Profile
	// - B-frame must be disabled. because, WEBRTC does not support B-Frame.
	::av_opt_set(_codec_context->priv_data, "profile", "constrained_baseline", 0);
	// ::av_opt_set(_codec_context->priv_data, "profile", "high", 0);

	// Loop Filter
	::av_opt_set_int(_codec_context->priv_data, "loopfilter", 1, 0);

	auto preset = GetRefTrack()->GetPreset().LowerCaseString();
	if (preset.IsEmpty() == true)
	{
		::av_opt_set(_codec_context->priv_data, "rc_mode", "bitrate", 0);
	}
	else
	{
		logtd("If the preset is used in the openh264 codec, constant bitrate is not supported");

		::av_opt_set(_codec_context->priv_data, "rc_mode", "quality", 0);

		if (preset == "slower")
		{
			_codec_context->qmin = 10;
			_codec_context->qmax = 39;

		}
		else if (preset == "slow")
		{
			_codec_context->qmin = 16;
			_codec_context->qmax = 45;
		}
		else if (preset == "medium")
		{
			_codec_context->qmin = 24;
			_codec_context->qmax = 51;
		}		
		else if (preset == "fast")
		{
			_codec_context->qmin = 32;
			_codec_context->qmax = 51;
		}
		else if (preset == "faster")
		{
			_codec_context->qmin = 40;
			_codec_context->qmax = 51;
		}		
		else{
			logtw("Unknown preset: %s", preset.CStr());
		}
	}

	return true;
}

bool EncoderAVCxOpenH264::Configure(std::shared_ptr<MediaTrack> context)
{
	if (TranscodeEncoder::Configure(context) == false)
	{
		return false;
	}
	auto codec_id = GetCodecID();

	const AVCodec *codec = ::avcodec_find_encoder_by_name("libopenh264");
	if (codec == nullptr)
	{
		logte("Could not find encoder: %d (%s)", codec_id, ::avcodec_get_name(codec_id));
		return false;
	}

	_codec_context = ::avcodec_alloc_context3(codec);
	if (_codec_context == nullptr)
	{
		logte("Could not allocate codec context for %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	if (SetCodecParams() == false)
	{
		logte("Could not set codec parameters for %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	if (::avcodec_open2(_codec_context, codec, nullptr) < 0)
	{
		logte("Could not open codec: %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_codec_thread = std::thread(&TranscodeEncoder::CodecThread, this);
		pthread_setname_np(_codec_thread.native_handle(), ov::String::FormatString("Enc%s", avcodec_get_name(GetCodecID())).CStr());
	}
	catch (const std::system_error &e)
	{
		logte("Failed to start encoder thread.");
		_kill_flag = true;

		return false;
	}

	return true;
}

void EncoderAVCxOpenH264::CodecThread()
{
	while (!_kill_flag)
	{
		auto obj = _input_buffer.Dequeue();
		if (obj.has_value() == false)
			continue;

		auto media_frame = std::move(obj.value());

		///////////////////////////////////////////////////
		// Request frame encoding to codec
		///////////////////////////////////////////////////
		auto av_frame = ffmpeg::Conv::ToAVFrame(cmn::MediaType::Video, media_frame);
		if (!av_frame)
		{
			logte("Could not allocate the video frame data");
			break;
		}

		// AV_Frame.pict_type must be set to AV_PICTURE_TYPE_NONE. This will ensure that the keyframe interval option is applied correctly.
		av_frame->pict_type = AV_PICTURE_TYPE_NONE;

		int ret = ::avcodec_send_frame(_codec_context, av_frame);
		if (ret < 0)
		{
			logte("Error sending a frame for encoding : %d", ret);
		}

		///////////////////////////////////////////////////
		// The encoded packet is taken from the codec.
		///////////////////////////////////////////////////
		while (true)
		{
			// Check frame is availble
			int ret = ::avcodec_receive_packet(_codec_context, _packet);
			if (ret == AVERROR(EAGAIN))
			{
				// More packets are needed for encoding.
				break;
			}
			else if (ret == AVERROR_EOF && ret < 0)
			{
				logte("Error receiving a packet for decoding : %d", ret);
				break;
			}
			else
			{
				auto media_packet = ffmpeg::Conv::ToMediaPacket(_packet, cmn::MediaType::Video, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
				if (media_packet == nullptr)
				{
					logte("Could not allocate the media packet");
					break;
				}

				::av_packet_unref(_packet);

				SendOutputBuffer(std::move(media_packet));
			}
		}
	}
}
