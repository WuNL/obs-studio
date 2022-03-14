// Copyright Dr. Alex. Gouaillard (2015, 2020)

#include "ByteWebRTCStream.h"
#include "SDPModif.h"

#include "media-io/video-io.h"
// #include "ui-validation.hpp"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video/i420_buffer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "pc/rtc_stats_collector.h"
#include "rtc_base/checks.h"
#include <libyuv.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include <locale>

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include "rtc_base/strings/json.h"

using namespace std;

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)
#define error(format, ...) blog(LOG_ERROR, format, ##__VA_ARGS__)

//回调函数
size_t write_data(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	std::ostringstream *stream = (std::ostringstream *)userdata;
	size_t count = size * nmemb;
	stream->write((const char *)ptr, count);
	return count;
}

//回调函数
size_t write_location_data(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	std::string tmp = ptr ? (const char *)ptr : "";
	std::string patten = "Location: ";
	size_t count = size * nmemb;
	if (tmp.rfind(patten, 0) == 0) {
		tmp.erase(0, patten.length());
		std::ostringstream *stream = (std::ostringstream *)userdata;
		stream->write(tmp.c_str(), count);
	}
	return count;
}

size_t split(const std::string &txt, std::vector<std::string> &strs, char ch)
{
	size_t pos = txt.find(ch);
	size_t initialPos = 0;
	strs.clear();

	// Decompose statement
	while (pos != std::string::npos) {
		strs.push_back(txt.substr(initialPos, pos - initialPos));
		initialPos = pos + 1;

		pos = txt.find(ch, initialPos);
	}

	// Add the last one
	strs.push_back(txt.substr(initialPos,
				  std::min(pos, txt.size()) - initialPos + 1));

	return strs.size();
}

class StatsCallback : public webrtc::RTCStatsCollectorCallback {
public:
	rtc::scoped_refptr<const webrtc::RTCStatsReport> report()
	{
		return report_;
	}
	bool called() const { return called_; }

protected:
	void OnStatsDelivered(
		const rtc::scoped_refptr<const webrtc::RTCStatsReport> &report)
		override
	{
		report_ = report;
		called_ = true;
	}

private:
	bool called_ = false;
	rtc::scoped_refptr<const webrtc::RTCStatsReport> report_;
};

class ByteCustomLogger : public rtc::LogSink {
public:
	void OnLogMessage(const std::string &message) override
	{
		info("%s", message.c_str());
	}
};

ByteCustomLogger logger;

ByteWebRTCStream::ByteWebRTCStream(obs_output_t *output)
{
	rtc::LogMessage::RemoveLogToStream(&logger);
	rtc::LogMessage::AddLogToStream(&logger,
					rtc::LoggingSeverity::LS_VERBOSE);

	resetStats();

	audio_bitrate = 128;
	video_bitrate = 2500;

	// Store output
	this->output = output;

	// Create audio device module
	// NOTE ALEX: check if we still need this
	adm = new rtc::RefCountedObject<AudioDeviceModuleWrapper>();

	// Network thread
	network = rtc::Thread::CreateWithSocketServer();
	network->SetName("network", nullptr);
	network->Start();

	// Worker thread
	worker = rtc::Thread::Create();
	worker->SetName("worker", nullptr);
	worker->Start();

	// Signaling thread
	signaling = rtc::Thread::Create();
	signaling->SetName("signaling", nullptr);
	signaling->Start();

	factory = webrtc::CreatePeerConnectionFactory(
		network.get(), worker.get(), signaling.get(), adm,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		webrtc::CreateBuiltinVideoEncoderFactory(),
		webrtc::CreateBuiltinVideoDecoderFactory(), nullptr, nullptr);
	//webrtc::PeerConnectionFactoryInterface::Options options;
	//options.disable_encryption = true;
	//factory->SetOptions(options);
	// Create video capture module
	videoCapturer = new rtc::RefCountedObject<VideoCapturer>();
}

ByteWebRTCStream::~ByteWebRTCStream()
{
	rtc::LogMessage::RemoveLogToStream(&logger);

	// Shutdown websocket connection and close Peer Connection
	close(false);

	// Free factories
	adm = nullptr;
	pc = nullptr;
	factory = nullptr;
	videoCapturer = nullptr;

	// Stop all threads
	if (!network->IsCurrent())
		network->Stop();
	if (!worker->IsCurrent())
		worker->Stop();
	if (!signaling->IsCurrent())
		signaling->Stop();

	network.release();
	worker.release();
	signaling.release();
}

void ByteWebRTCStream::resetStats()
{
	stats_list_ = "";
	frame_id_ = 0;
	pli_received_ = 0;
	total_bytes_sent_ = 0;
	// #310 webrtc getstats()
	transport_bytes_sent_ = 0;
	transport_bytes_received_ = 0;
	video_packets_sent_ = 0;
	video_bytes_sent_ = 0;
	video_fir_count_ = 0;
	video_nack_count_ = 0;
	video_qp_sum_ = 0;
	audio_packets_sent_ = 0;
	audio_bytes_sent_ = 0;
	track_audio_level_ = 0;
	track_total_audio_energy_ = 0;
	track_total_samples_duration_ = 0;
	track_frame_width_ = 0;
	track_frame_height_ = 0;
	track_frames_sent_ = 0;
	track_huge_frames_sent_ = 0;
	previous_time_ = std::chrono::system_clock::time_point(
		std::chrono::duration<int>(0));
	previous_frames_sent_ = 0;
}

bool ByteWebRTCStream::start(ByteWebRTCStream::Type type)
{
	info("ByteWebRTCStream::start");
	this->type = type;

	resetStats();

	// Access service if started, or fail

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(
				output,
				"An unexpected error occurred during stream startup.");
			obs_output_signal_stop(output,
					       OBS_OUTPUT_CONNECT_FAILED);
		});
		thread.detach();
		return false;
	}

	// Extract setting from service
	url = obs_service_get_url(service) ? obs_service_get_url(service) : "";
	publish_key_ = obs_service_get_key(service)
			       ? obs_service_get_key(service)
			       : "";
	std::vector<std::string> params;

	split(publish_key_, params, '?');
	if (params.size() < 3) {
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(
				output,
				"An unexpected error occurred during stream startup.");
			obs_output_signal_stop(output,
					       OBS_OUTPUT_CONNECT_FAILED);
		});
		thread.detach();
		return false;
	} else {
		app_name_ = params[0];
		stream_name_ = params[1];
		stream_token_ = params[2];
	}
	// #271 do not list video codec H264 if it is not available in libwebrtc
#ifdef DISABLE_WEBRTC_H264
	if ("h264" == video_codec) {
		info("No video codec selected and H264 not available: set to VP8 by default");
		video_codec = "VP8";
	}
#endif		: "";
	// No Simulast for VP8 (not supported properly by libwebrtc) and AV1 codecs
	if (video_codec.empty() || "VP8" == video_codec ||
	    "AV1" == video_codec) {
		info("Simulcast not supported for %s: Disabling Simulcast\n",
		     video_codec.empty() ? "VP8" : video_codec.c_str());
		simulcast_ = false;
	}

	// Some extra log

	info("Video codec: %s",
	     video_codec.empty() ? "Automatic" : video_codec.c_str());
	if (0 == getVideoSourceCount()) {
		info("No video source in current scene");
	}
	info("Simulcast: %s", simulcast_ ? "true" : "false");
	if (multisource_) {
		info("Multisource: true, sourceID: %s", sourceId_.c_str());
	} else {
		info("Multisource: false");
	}
	info("Publish API URL: %s", publishApiUrl.c_str());
	info("Protocol:    %s",
	     protocol.empty() ? "Automatic" : protocol.c_str());

	// Stream setting sanity check

	bool isServiceValid = true;
	if (type != ByteWebRTCStream::Type::ByteWebRTC) {
		isServiceValid = false;
	}

	if (!isServiceValid) {
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(
				output,
				"Support ByteRTC Only.");
			obs_output_signal_stop(output,
					       OBS_OUTPUT_CONNECT_FAILED);
		});
		thread.detach();
		return false;
	}

	// Set up encoders.

	struct obs_audio_info audio_info;
	if (!obs_get_audio_info(&audio_info)) {
		warn("Failed to load audio settings.  Defaulting to opus.");
		audio_codec = "opus";
	} else {
		// NOTE ALEX: if input # channel > output we should down mix
		//            if input is > 2 but < 6 we might have a porblem with multiopus.
		channel_count = (int)(audio_info.speakers);
		audio_codec = channel_count <= 2 ? "opus" : "multiopus";
	}

	// Shutdown websocket connection and close Peer Connection (just in case)
	if (close(false)) {
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		});
		thread.detach();
	}

	webrtc::PeerConnectionInterface::RTCConfiguration config;
	webrtc::PeerConnectionInterface::IceServer server;
	server.urls = {"stun:stun.l.google.com:19302"};
	config.servers.push_back(server);
	// config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
	// config.disable_ipv6 = true;
	// config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
	config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	// config.set_cpu_adaptation(false);
	// config.set_suspend_below_min_bitrate(false);

	webrtc::PeerConnectionDependencies dependencies(this);

	pc = factory->CreatePeerConnection(config, std::move(dependencies));

	if (!pc.get()) {
		error("Error creating Peer Connection");
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(
				output,
				"There was an error connecting to the server. Are you connected to the internet?");
			obs_output_signal_stop(output,
					       OBS_OUTPUT_CONNECT_FAILED);
		});
		thread.detach();
		return false;
	} else {
		info("PEER CONNECTION CREATED\n");
	}

	cricket::AudioOptions options;
	options.echo_cancellation.emplace(false); // default: true
	options.auto_gain_control.emplace(false); // default: true
	options.noise_suppression.emplace(false); // default: true
	options.highpass_filter.emplace(false);   // default: true
	options.stereo_swapping.emplace(false);
	options.typing_detection.emplace(false); // default: true
	options.experimental_agc.emplace(false);
	// m79 options.extended_filter_aec.emplace(false);
	// m79 options.delay_agnostic_aec.emplace(false);
	options.experimental_ns.emplace(false);
	options.residual_echo_detector.emplace(false); // default: true
	// options.tx_agc_limiter.emplace(false);

	stream = factory->CreateLocalMediaStream("obs");

	audio_source = obsWebrtcAudioSource::Create(&options);
	audio_track = factory->CreateAudioTrack("audio", audio_source);
	// pc->AddTrack(audio_track, {"obs"});
	stream->AddTrack(audio_track);

	if (getVideoSourceCount() > 0) {
		video_track = factory->CreateVideoTrack("video", videoCapturer);
		// pc->AddTrack(video_track, {"obs"});
		stream->AddTrack(video_track);
	}

	//Add audio track
	webrtc::RtpTransceiverInit audio_init;
	audio_init.stream_ids.push_back(stream->id());
	audio_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
	pc->AddTransceiver(audio_track, audio_init);

	if (getVideoSourceCount() > 0) {
		//Add video track
		webrtc::RtpTransceiverInit video_init;
		video_init.stream_ids.push_back(stream->id());
		video_init.direction =
			webrtc::RtpTransceiverDirection::kSendOnly;
		if (simulcast_) {
			webrtc::RtpEncodingParameters large;
			webrtc::RtpEncodingParameters medium;
			webrtc::RtpEncodingParameters small;
			large.rid = "L";
			large.scale_resolution_down_by = 1;
			large.max_bitrate_bps = video_bitrate * 1000;
			medium.rid = "M";
			medium.scale_resolution_down_by = 2;
			medium.max_bitrate_bps = video_bitrate * 1000 / 3;
			small.rid = "S";
			small.scale_resolution_down_by = 4;
			small.max_bitrate_bps = video_bitrate * 1000 / 9;
			//In reverse order so large is dropped first on low network condition
			// Send small resolution only if output video resolution >= 480p
			if (obs_output_get_height(output) >= 480) {
				video_init.send_encodings.push_back(small);
			}
			video_init.send_encodings.push_back(medium);
			video_init.send_encodings.push_back(large);
		}
		pc->AddTransceiver(video_track, video_init);
	}

	webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offer_options;
	offer_options.voice_activity_detection = false;
	pc->CreateOffer(this, offer_options);

	return true;
}

void ByteWebRTCStream::OnSuccess(webrtc::SessionDescriptionInterface *desc)
{
	info("ByteWebRTCStream::OnSuccess\n");
	std::string sdp;
	desc->ToString(&sdp);

	info("Audio codec:      %s", audio_codec.c_str());
	info("Audio bitrate:    %d\n", audio_bitrate);
	if (getVideoSourceCount() > 0) {
		info("Video codec:      %s",
		     video_codec.empty() ? "Automatic" : video_codec.c_str());
		info("Video bitrate:    %d\n", video_bitrate);
	} else {
		info("No video");
	}
	info("OFFER:\n\n%s\n", sdp.c_str());

	std::string sdpCopy = sdp;
	std::vector<int> audio_payloads;
	std::vector<int> video_payloads;
	// #271 do not list video codec H264 if it is not available in libwebrtc
	// If codec setting is Automatic, set it to h264 by default if available, else set it to VP8
	if (video_codec.empty()) {
#ifdef DISABLE_WEBRTC_H264
		video_codec = "VP8";
#else
		video_codec = "h264"; // h264 must be in lowercase (Firefox)
#endif
	}
	// Force specific video/audio payload
	SDPModif::forcePayload(sdpCopy, audio_payloads, video_payloads,
			       // the packaging mode needs to be 1
			       audio_codec, video_codec, 1, "42e01f", 0);
	// Constrain video bitrate
	SDPModif::bitrateMaxMinSDP(sdpCopy, video_bitrate, video_payloads);
	// Enable stereo & constrain audio bitrate
	// NOTE ALEX: check that it does not incorrectly detect multiopus as opus
	SDPModif::stereoSDP(sdpCopy, audio_bitrate);
	// NOTE ALEX: nothing special to do about multiopus with CoSMo libwebrtc package.

	info("SETTING LOCAL DESCRIPTION\n\n");
	pc->SetLocalDescription(this, desc);

	info("Sending OFFER (SDP) to remote peer:\n\n%s", sdpCopy.c_str());

	CURL *curl;
	CURLcode res;
	char tmp_str[40960] = {0};
	std::stringstream out;
	std::stringstream header_out;

	//HTTP报文头
	struct curl_slist *headers = NULL;
	std::string str_url = url + "/" + app_name_ + "/" + stream_name_;
	std::string str_token = "authorization:" + stream_token_;
	curl = curl_easy_init();

	if (curl) {
		std::string jsonout = sdpCopy;

		//设置url
		curl_easy_setopt(curl, CURLOPT_URL, str_url.c_str());

		//设置http发送的内容类型为JSON
		//构建HTTP报文头
		sprintf_s(tmp_str, "Content-Length: %s", jsonout.c_str());
		headers = curl_slist_append(headers,
					    "Content-Type:application/sdp");
		headers = curl_slist_append(headers, str_token.c_str());
		//headers=curl_slist_append(headers, tmp_str);//在请求头中设置长度,请求会失败,还没找到原因

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		//curl_easy_setopt(curl,  CURLOPT_CUSTOMREQUEST, "POST");//自定义请求方式
		curl_easy_setopt(curl, CURLOPT_POST,
				 1); //设置为非0表示本次操作为POST

		// 设置要POST的JSON数据
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonout.c_str());
		curl_easy_setopt(
			curl, CURLOPT_POSTFIELDSIZE,
			jsonout.size()); //设置上传json串长度,这个设置可以忽略

		// 设置接收数据的处理函数和存放变量
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
				 write_data); //设置回调函数
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out); //设置写数据

		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
				 write_location_data);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA,
				 &header_out); //设置写数据
		//curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);

		res = curl_easy_perform(curl); //执行

		curl_slist_free_all(headers); /* free the list again */
		string str_json = out.str();  //返回请求值
		printf("%s", str_json.c_str());
		localtionUrl_ = header_out.str(); //返回请求值

		long res_code = 0;

		res = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
					&res_code);

		if ((res_code == 200 || res_code == 201) && !str_json.empty()) {
			onOpened(str_json.c_str());
		} else {
			// Close must be carried out on a separate thread in order to avoid deadlock
			auto thread = std::thread([=]() {
				obs_output_set_last_error(
					output,
					"remote sdk error, check your token!\n\n");
				// Disconnect, this will call stop on main thread
				// #298 Close must be carried out on a separate thread in order to avoid deadlock
				auto thread = std::thread([=]() {
					obs_output_signal_stop(
						output, OBS_OUTPUT_ERROR);
				});
				thread.detach();
			});
			thread.detach();
		}

		/* always cleanup */
		curl_easy_cleanup(curl);
	}
}

void ByteWebRTCStream::OnSuccess()
{
	info("Local Description set\n");
}

void ByteWebRTCStream::OnFailure(webrtc::RTCError error)
{
	warn("ByteWebRTCStream::OnFailure [%s]", error.message());
	// Shutdown websocket connection and close Peer Connection
	close(false);
	// Disconnect, this will call stop on main thread
	obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
}

void ByteWebRTCStream::OnIceCandidate(
	const webrtc::IceCandidateInterface *candidate)
{
	std::string str;
	candidate->ToString(&str);
}

void ByteWebRTCStream::OnIceConnectionChange(
	webrtc::PeerConnectionInterface::IceConnectionState
		state /* new_state */)
{
	using namespace webrtc;
	info("ByteWebRTCStream::OnIceConnectionChange [%u]", state);

	switch (state) {
	case PeerConnectionInterface::IceConnectionState::kIceConnectionFailed: {
		// Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(
				output,
				"We found your room, but streaming failed. Are you behind a firewall?\n\n");
			// Disconnect, this will call stop on main thread
			// #298 Close must be carried out on a separate thread in order to avoid deadlock
			auto thread = std::thread([=]() {
				obs_output_signal_stop(output,
						       OBS_OUTPUT_ERROR);
			});
			thread.detach();
		});
		thread.detach();
		break;
	}
	default:
		break;
	}
}

void ByteWebRTCStream::OnConnectionChange(
	webrtc::PeerConnectionInterface::PeerConnectionState state)
{
	using namespace webrtc;
	info("ByteWebRTCStream::OnConnectionChange [%u]", state);

	switch (state) {
	case PeerConnectionInterface::PeerConnectionState::kFailed: {

		// Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_set_last_error(output,
						  "Connection failure\n\n");
			// Disconnect, this will call stop on main thread
			// #298 Close must be carried out on a separate thread in order to avoid deadlock
			auto thread = std::thread([=]() {
				obs_output_signal_stop(output,
						       OBS_OUTPUT_ERROR);
			});
			thread.detach();
		});
		//Detach
		thread.detach();
		break;
	}
	default:
		break;
	}
}

void ByteWebRTCStream::onOpened(const char *sdp)
{
	info("ANSWER:\n\n%s\n", sdp);

	std::string sdpCopy = sdp;

	if (getVideoSourceCount() > 0) {
		// Constrain video bitrate
		SDPModif::bitrateSDP(sdpCopy, video_bitrate);
	}
	// Enable stereo & constrain audio bitrate
	SDPModif::stereoSDP(sdpCopy, audio_bitrate);

	// SetRemoteDescription observer
	srd_observer = make_scoped_refptr(this);

	// Create ANSWER from sdpCopy
	webrtc::SdpParseError error;
	std::unique_ptr<webrtc::SessionDescriptionInterface> answer =
		webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer,
						 sdpCopy, &error);

	info("SETTING REMOTE DESCRIPTION\n\n%s", sdpCopy.c_str());
	pc->SetRemoteDescription(std::move(answer), srd_observer);

	// Set audio conversion info
	audio_convert_info conversion;
	conversion.format = AUDIO_FORMAT_16BIT;
	conversion.samples_per_sec = 48000;
	conversion.speakers = (speaker_layout)channel_count;
	obs_output_set_audio_conversion(output, &conversion);

	info("Begin data capture...");
	obs_output_begin_data_capture(output, 0);
}

void ByteWebRTCStream::OnSetRemoteDescriptionComplete(webrtc::RTCError error)
{
	if (error.ok())
		info("Remote Description set\n");
	else {
		warn("Error setting Remote Description: %s\n", error.message());
		// Shutdown websocket connection and close Peer Connection
		close(false);
		// Disconnect, this will call stop on main thread
		// #298 Close must be carried out on a separate thread in order to avoid deadlock
		auto thread = std::thread([=]() {
			obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		});
		thread.detach();
	}
}

bool ByteWebRTCStream::close(bool wait)
{
	if (!pc.get())
		return false;
	// Get pointer
	auto old = pc.release();
	// Close Peer Connection
	old->Close();
	// stop signal
	if (!localtionUrl_.empty()) {
		std::stringstream out;
		localtionUrl_.erase(std::remove(localtionUrl_.begin(),
						localtionUrl_.end(), '\r'),
				    localtionUrl_.end());
		localtionUrl_.erase(std::remove(localtionUrl_.begin(),
						localtionUrl_.end(), '\n'),
				    localtionUrl_.end());
		CURL *hnd = curl_easy_init();
		curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "DELETE");
		curl_easy_setopt(hnd, CURLOPT_URL, localtionUrl_.c_str());
		struct curl_slist *headers = NULL;
		headers = curl_slist_append(headers,
					    "content-type: application/json");
		curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);

		// 设置接收数据的处理函数和存放变量
		curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION,
				 write_data); //设置回调函数
		curl_easy_setopt(hnd, CURLOPT_WRITEDATA, &out); //设置写数据

		CURLcode ret = curl_easy_perform(hnd);
		// do something...
		long res_code = 0;

		auto res = curl_easy_getinfo(hnd, CURLINFO_RESPONSE_CODE,
					     &res_code);
		std::string responseStr = out.str();
		curl_slist_free_all(headers);
		curl_easy_cleanup(hnd);
		localtionUrl_.clear();
	}
	return true;
}

bool ByteWebRTCStream::stop()
{
	info("ByteWebRTCStream::stop");
	// Shutdown websocket connection and close Peer Connection
	close(true);
	// Disconnect, this will call stop on main thread
	obs_output_end_data_capture(output);
	return true;
}

void ByteWebRTCStream::onAudioFrame(audio_data *frame)
{
	if (!frame)
		return;
	// Push it to the device
	audio_source->OnAudioData(frame);
}

void ByteWebRTCStream::onVideoFrame(video_data *frame)
{
	if (!frame)
		return;
	if (!videoCapturer)
		return;

	if (std::chrono::system_clock::time_point(
		    std::chrono::duration<int>(0)) == previous_time_) {
		// First frame sent: Initialize previous_time
		previous_time_ = std::chrono::system_clock::now();
	}

	// Calculate size
	int outputWidth = obs_output_get_width(output);
	int outputHeight = obs_output_get_height(output);
	auto videoType = webrtc::VideoType::kIYUV;
	uint32_t size = outputWidth * outputHeight * 3 / 2;

	int stride_y = outputWidth;
	int stride_uv = (outputWidth + 1) / 2;
	int target_width = abs(outputWidth);
	int target_height = abs(outputHeight);

	// Convert frame
	rtc::scoped_refptr<webrtc::I420Buffer> buffer =
		webrtc::I420Buffer::Create(target_width, target_height,
					   stride_y, stride_uv, stride_uv);

	libyuv::RotationMode rotation_mode = libyuv::kRotate0;

	const int conversionResult = libyuv::ConvertToI420(
		frame->data[0], size, buffer.get()->MutableDataY(),
		buffer.get()->StrideY(), buffer.get()->MutableDataU(),
		buffer.get()->StrideU(), buffer.get()->MutableDataV(),
		buffer.get()->StrideV(), 0, 0, outputWidth, outputHeight,
		target_width, target_height, rotation_mode,
		libyuv::FOURCC_NV12);

	// not using the result yet, silence compiler
	(void)conversionResult;

	const int64_t obs_timestamp_us =
		(int64_t)frame->timestamp / rtc::kNumNanosecsPerMicrosec;

	// Align timestamps from OBS capturer with rtc::TimeMicros timebase
	const int64_t aligned_timestamp_us =
		timestamp_aligner_.TranslateTimestamp(obs_timestamp_us,
						      rtc::TimeMicros());

	// Create a webrtc::VideoFrame to pass to the capturer
	webrtc::VideoFrame video_frame =
		webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(buffer)
			.set_rotation(webrtc::kVideoRotation_0)
			.set_timestamp_us(aligned_timestamp_us)
			.set_id(++frame_id_)
			.build();

	// Send frame to video capturer
	videoCapturer->OnFrameCaptured(video_frame);
}

// Count number of video sources in current scene
int ByteWebRTCStream::getVideoSourceCount() const
{
	auto countSources = [](void *param, obs_source_t *source) {
		if (!source)
			return true;

		uint32_t flags = obs_source_get_output_flags(source);
		if ((flags & OBS_SOURCE_VIDEO) != 0)
			(*reinterpret_cast<int *>(param))++;

		return true;
	};
	int count_video_source = 0;
	obs_enum_sources(countSources, &count_video_source);
	return count_video_source;
}

// NOTE LUDO: #80 add getStats
void ByteWebRTCStream::getStats()
{
	auto reports = NewGetStats();
	if (reports.empty()) {
		return;
	}

	// FPS = frames_sent / (time_now - start_time)
	auto time_now = std::chrono::system_clock::now();
	auto elapsed_seconds = time_now - previous_time_;
	previous_time_ = time_now;

	stats_list_ = "";
	total_bytes_sent_ = 0;
	uint32_t data_messages_sent = 0;
	uint64_t data_bytes_sent = 0;
	uint32_t data_messages_received = 0;
	uint64_t data_bytes_received = 0;

	transport_bytes_sent_ = 0;
	transport_bytes_received_ = 0;
	video_packets_sent_ = 0;
	video_bytes_sent_ = 0;
	pli_received_ = 0;
	video_fir_count_ = 0;
	video_nack_count_ = 0;
	video_qp_sum_ = 0;
	audio_packets_sent_ = 0;
	audio_bytes_sent_ = 0;
	track_audio_level_ = 0;
	track_total_audio_energy_ = 0;
	track_total_samples_duration_ = 0;
	track_frame_width_ = 0;
	track_frame_height_ = 0;
	track_frames_sent_ = 0;
	track_huge_frames_sent_ = 0;

	for (const auto &report : reports) {
		// RTCDataChannelStats
		std::vector<const webrtc::RTCDataChannelStats *>
			data_channel_stats = report->GetStatsOfType<
				webrtc::RTCDataChannelStats>();
		for (const auto &stat : data_channel_stats) {
			data_messages_sent +=
				std::stoi(stat->messages_sent.ValueToJson());
			data_bytes_sent +=
				std::stoll(stat->bytes_sent.ValueToJson());
			data_messages_received += std::stoi(
				stat->messages_received.ValueToJson());
			data_bytes_received +=
				std::stoll(stat->bytes_received.ValueToJson());
		}

		// RTCMediaStreamTrackStats
		// double audio_track_jitter_buffer_delay = 0.0;
		// double video_track_jitter_buffer_delay = 0.0;
		// uint64_t audio_track_jitter_buffer_emitted_count = 0;
		// uint64_t video_track_jitter_buffer_emitted_count = 0;
		std::vector<const webrtc::RTCMediaStreamTrackStats *>
			media_stream_track_stats = report->GetStatsOfType<
				webrtc::RTCMediaStreamTrackStats>();
		for (const auto &stat : media_stream_track_stats) {
			if (stat->kind.ValueToString() == "audio") {
				if (stat->audio_level.is_defined()) {
					track_audio_level_ += std::stoi(
						stat->audio_level.ValueToJson());
					stats_list_ +=
						"track_audio_level:" +
						stat->audio_level.ValueToJson() +
						"\n";
				}
				if (stat->total_audio_energy.is_defined()) {
					track_total_audio_energy_ += std::stoi(
						stat->total_audio_energy
							.ValueToJson());
					stats_list_ +=
						"track_total_audio_energy:" +
						stat->total_audio_energy
							.ValueToJson() +
						"\n";
				}
				// stats_list += "track_echo_return_loss:"   + stat->echo_return_loss.ValueToJson() + "\n";
				// stats_list += "track_echo_return_loss_enhancement:" + stat->echo_return_loss_enhancement.ValueToJson() + "\n";
				// stats_list += "track_total_samples_received:" + stat->total_samples_received.ValueToJson() + "\n";
				if (stat->total_samples_duration.is_defined()) {
					track_total_samples_duration_ +=
						std::stoi(
							stat->total_samples_duration
								.ValueToJson());
					stats_list_ +=
						"track_total_samples_duration:" +
						stat->total_samples_duration
							.ValueToJson() +
						"\n";
				}
				// stats_list += "track_concealed_samples:" + stat->concealed_samples.ValueToJson() + "\n";
				// stats_list += "track_concealment_events:" + stat->concealment_events.ValueToJson() + "\n";
			}
			if (stat->kind.ValueToString() == "video") {
				// stats_list += "track_jitter_buffer_delay:"    + stat->jitter_buffer_delay.ValueToJson() + "\n";
				// stats_list += "track_jitter_buffer_emitted_count:" + stat->jitter_buffer_emitted_count.ValueToJson() + "\n";
				track_frame_width_ = std::stoi(
					stat->frame_width.ValueToJson());
				stats_list_ += "track_frame_width:" +
					       stat->frame_width.ValueToJson() +
					       "\n";
				track_frame_height_ = std::stoi(
					stat->frame_height.ValueToJson());
				stats_list_ +=
					"track_frame_height:" +
					stat->frame_height.ValueToJson() + "\n";
				track_frames_sent_ += std::stoll(
					stat->frames_sent.ValueToJson());
				stats_list_ += "track_frames_sent:" +
					       stat->frames_sent.ValueToJson() +
					       "\n";
				track_huge_frames_sent_ += std::stoll(
					stat->huge_frames_sent.ValueToJson());
				stats_list_ +=
					"track_huge_frames_sent:" +
					stat->huge_frames_sent.ValueToJson() +
					"\n";

				// FPS = frames_sent / (time_now - start_time)
				uint32_t frames_sent = std::stol(
					stat->frames_sent.ValueToJson());
				stats_list_ +=
					"track_fps:" +
					std::to_string(
						(frames_sent -
						 previous_frames_sent_) /
						elapsed_seconds.count()) +
					"\n";
				previous_frames_sent_ = frames_sent;

				// stats_list += "track_frames_received:"        + stat->frames_received.ValueToJson() + "\n";
				// stats_list += "track_frames_decoded:"         + stat->frames_decoded.ValueToJson() + "\n";
				// stats_list += "track_frames_dropped:"         + stat->frames_dropped.ValueToJson() + "\n";
				// stats_list += "track_frames_corrupted:"       + stat->frames_corrupted.ValueToJson() + "\n";
				// stats_list += "track_partial_frames_lost:"    + stat->partial_frames_lost.ValueToJson() + "\n";
				// stats_list += "track_full_frames_lost:"       + stat->full_frames_lost.ValueToJson() + "\n";
			}
		}

		// RTCOutboundRTPStreamStats
		std::vector<const webrtc::RTCOutboundRTPStreamStats *>
			outbound_stream_stats = report->GetStatsOfType<
				webrtc::RTCOutboundRTPStreamStats>();
		for (const auto &stat : outbound_stream_stats) {
			if (stat->kind.ValueToString() == "audio") {
				audio_packets_sent_ += std::stoll(
					stat->packets_sent.ValueToJson());
				stats_list_ +=
					"outbound_audio_packets_sent:" +
					stat->packets_sent.ValueToJson() + "\n";
				audio_bytes_sent_ += std::stoll(
					stat->bytes_sent.ValueToJson());
				stats_list_ += "outbound_audio_bytes_sent:" +
					       stat->bytes_sent.ValueToJson() +
					       "\n";
				// stats_list += "outbound_audio_target_bitrate:" + stat->target_bitrate.ValueToJson() + "\n";
				// stats_list += "outbound_audio_frames_encoded:" + stat->frames_encoded.ValueToJson() + "\n";
			}
			if (stat->kind.ValueToString() == "video") {
				video_packets_sent_ += std::stoll(
					stat->packets_sent.ValueToJson());
				stats_list_ +=
					"outbound_video_packets_sent:" +
					stat->packets_sent.ValueToJson() + "\n";
				video_bytes_sent_ += std::stoll(
					stat->bytes_sent.ValueToJson());
				stats_list_ += "outbound_video_bytes_sent:" +
					       stat->bytes_sent.ValueToJson() +
					       "\n";
				// stats_list += "outbound_video_target_bitrate:" + stat->target_bitrate.ValueToJson() + "\n";
				// stats_list += "outbound_video_frames_encoded:" + stat->frames_encoded.ValueToJson() + "\n";
				video_fir_count_ += std::stoll(
					stat->fir_count.ValueToJson());
				stats_list_ += "outbound_video_fir_count:" +
					       stat->fir_count.ValueToJson() +
					       "\n";
				pli_received_ += std::stoi(
					stat->pli_count.ValueToJson());
				stats_list_ += "outbound_video_pli_count:" +
					       stat->pli_count.ValueToJson() +
					       "\n";
				video_nack_count_ += std::stoll(
					stat->nack_count.ValueToJson());
				stats_list_ += "outbound_video_nack_count:" +
					       stat->nack_count.ValueToJson() +
					       "\n";
				// stats_list += "outbound_video_sli_count:"      + stat->sli_count.ValueToJson() + "\n";
				if (stat->qp_sum.is_defined()) {
					video_qp_sum_ += std::stoll(
						stat->qp_sum.ValueToJson());
					stats_list_ +=
						"outbound_video_qp_sum:" +
						stat->qp_sum.ValueToJson() +
						"\n";
				}
			}
		}

		// RTCTransportStats
		std::vector<const webrtc::RTCTransportStats *> transport_stats =
			report->GetStatsOfType<webrtc::RTCTransportStats>();
		for (const auto &stat : transport_stats) {
			transport_bytes_sent_ =
				std::stoll(stat->bytes_sent.ValueToJson());
			stats_list_ += "transport_bytes_sent:" +
				       stat->bytes_sent.ValueToJson() + "\n";
			transport_bytes_received_ =
				std::stoll(stat->bytes_received.ValueToJson());
			stats_list_ += "transport_bytes_received:" +
				       stat->bytes_received.ValueToJson() +
				       "\n";
		}

	} // loop on reports

	total_bytes_sent_ = audio_bytes_sent_ + video_bytes_sent_;

	// RTCDataChannelStats
	stats_list_ += "data_messages_sent:" +
		       std::to_string(data_messages_sent) + "\n";
	stats_list_ +=
		"data_bytes_sent:" + std::to_string(data_bytes_sent) + "\n";
	stats_list_ += "data_messages_received:" +
		       std::to_string(data_messages_received) + "\n";
	stats_list_ += "data_bytes_received:" +
		       std::to_string(data_bytes_received) + "\n";

	// RTCPeerConnectionStats
	// std::vector<const webrtc::RTCPeerConnectionStats*> pc_stats =
	//         report->GetStatsOfType<webrtc::RTCPeerConnectionStats>();
	// for (const auto& stat : pc_stats) {
	//   stats_list += "data_channels_opened:" + stat->data_channels_opened.ValueToJson() + "\n";
	//   stats_list += "data_channels_closed:" + stat->data_channels_closed.ValueToJson() + "\n";
	// }

	// RTCRTPStreamStats
	// std::vector<const webrtc::RTCRTPStreamStats*> rtcrtp_stats =
	//         report->GetStatsOfType<webrtc::RTCRTPStreamStats>();
	// for (const auto& stat : rtcrtp_stats) {
	//   if (stat->kind.ValueToString() == "video") {
	//     stats_list += "rtcrtp_fir_count:" + stat->fir_count.ValueToJson() + "\n";
	//     stats_list += "rtcrtp_pli_count:" + stat->pli_count.ValueToJson() + "\n";
	//     stats_list += "rtcrtp_nack_count:" + stat->nack_count.ValueToJson() + "\n";
	//     stats_list += "rtcrtp_sli_count:" + stat->sli_count.ValueToJson() + "\n";
	//     stats_list += "rtcrtp_qp_sum:" + stat->qp_sum.ValueToJson() + "\n";
	//   }
	// }

	// RTCInboundRTPStreamStats
	// std::vector<const webrtc::RTCInboundRTPStreamStats*> inbound_stream_stats =
	//         report->GetStatsOfType<webrtc::RTCInboundRTPStreamStats>();
	// for (const auto& stat : inbound_stream_stats) {
	//   stats_list += "inbound_stream_packets_received:"        + stat->packets_received.ValueToJson() + "\n";
	//   stats_list += "inbound_stream_bytes_received:"          + stat->bytes_received.ValueToJson() + "\n";
	//   stats_list += "inbound_stream_packets_lost:"            + stat->packets_lost.ValueToJson() + "\n";

	//   if (stat->kind.ValueToString() == "audio") {
	//     stats_list += "inbound_stream_jitter:" + stat->jitter.ValueToJson() + "\n";
	//   }
	// }
}

std::vector<rtc::scoped_refptr<const webrtc::RTCStatsReport>>
ByteWebRTCStream::NewGetStats()
{
	webrtc::MutexLock lock(&crit_);

	if (nullptr == pc) {
		return {};
	}

	std::vector<rtc::scoped_refptr<StatsCallback>> vector_stats_callbacks;
	for (const auto &transceiver : pc->GetTransceivers()) {
		auto sender = transceiver->sender();
		rtc::scoped_refptr<StatsCallback> stats_callback =
			new rtc::RefCountedObject<StatsCallback>();
		pc->GetStats(sender, stats_callback);
		vector_stats_callbacks.push_back(stats_callback);
	}

	std::vector<rtc::scoped_refptr<const webrtc::RTCStatsReport>> reports;
	for (const auto &stats_callback : vector_stats_callbacks) {
		while (!stats_callback->called()) {
			std::this_thread::sleep_for(
				std::chrono::microseconds(1));
		}
		reports.push_back(stats_callback->report());
	}

	return reports;
}
