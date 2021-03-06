/*
* libjingle
* Copyright 2012, Google Inc.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
*  1. Redistributions of source code must retain the above copyright notice,
*     this list of conditions and the following disclaimer.
*  2. Redistributions in binary form must reproduce the above copyright notice,
*     this list of conditions and the following disclaimer in the documentation
*     and/or other materials provided with the distribution.
*  3. The name of the author may not be used to endorse or promote products
*     derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
* EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
* OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "stdafx.h"

#include "main_wnd.h"
#include "javascript_callback.h"
#include "peerconnection_wrapper.h"
#include <utility>
#include <algorithm> 
//#include <functional> 
//#include <locale>
//#include <cctype>
#include "talk/app/webrtc/videosourceinterface.h"
#include "talk/app/webrtc/mediaconstraintsinterface.h"
#include "talk/base/win32socketserver.h"
#include "talk/base/common.h"
#include "talk/base/json.h"
#include "talk/base/logging.h"
#include "defaults.h"

#include "talk/app/webrtc/peerconnectioninterface.h"

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

std::string extractString(const char * field, std::string message){
	Json::Value json;
	Json::Reader reader;
	std::string val;

	if (!reader.parse(message, json)){
		LOG(INFO) << "Bad response...";
	}

	val = json[field].asString();
	return val;
}

const std::string gettime() {
	time_t     now = time(0);
	struct tm  tstruct;
	char       buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);
	return buf;
}

class DummySetSessionDescriptionObserver
	: public webrtc::SetSessionDescriptionObserver {
public:
	static DummySetSessionDescriptionObserver* Create()	{
		return
			new talk_base::RefCountedObject<DummySetSessionDescriptionObserver>();
	}
	virtual void OnSuccess() {
		LOG(INFO) << __FUNCTION__;
	}
	virtual void OnFailure(const std::string& error) {
		LOG(INFO) << __FUNCTION__ << " " << error;
	}

protected:
	DummySetSessionDescriptionObserver() {
	}
	~DummySetSessionDescriptionObserver() {
	}
};


PeerConnectionWrapper::PeerConnectionWrapper(JavaScriptCallback * cb,
					std::string easyRtcId, 
					DeviceController* device_controller, 
					talk_base::scoped_refptr<webrtc::PeerConnectionFactoryInterface> pfactory,
					std::string iceServerCandidates_) :
					atl_control(cb),
					device_controller_(device_controller),
					easyRtcId(easyRtcId), 
					peer_connection_factory_(pfactory), 
					iceCandidatesFromSS_(iceServerCandidates_) {
	SetAllowDtlsSctpDataChannels();
	ASSERT(peer_connection_factory_ != NULL);
}

PeerConnectionWrapper::~PeerConnectionWrapper() {
	if (peer_connection_ != NULL)
		if (peer_connection_.get() != NULL)
			LOG(LS_ERROR) << "leak, clean up peer_connection...";
}

bool PeerConnectionWrapper::connection_active() const {
	return peer_connection_.get() != NULL;
}

void PeerConnectionWrapper::Close() {
	DeletePeerConnection();
}

void PeerConnectionWrapper::ProcessCandidate(std::string candidate) {
	LOG(INFO) << "Got Candidate:" << candidate;
	OnMessageFromPeer(0, candidate);
}

void PeerConnectionWrapper::ProcessOffer(std::string offer)	{
	LOG(INFO) << "Got offer:" << offer;
	OnMessageFromPeer(0, offer);	
}

void PeerConnectionWrapper::Hangup() {
	if (peer_connection_.get()) {
		DeletePeerConnection();
	}
}


void PeerConnectionWrapper::ProcessAnswer(std::string answer) {

	std::string type = "answer";
	std::string sdp = extractString("sdp", answer); 

	LOG(INFO) << "Got offer:" << answer;
	
	if (peer_connection_ == NULL)
		InitializePeerConnection();

	webrtc::SessionDescriptionInterface* session_description(webrtc::CreateSessionDescription(type, sdp));
	peer_connection_->SetRemoteDescription(DummySetSessionDescriptionObserver::Create(), session_description);
}



void PeerConnectionWrapper::CreateOfferSDP() {
	LOG(INFO) << "createoffer ***********************";

	if (peer_connection_.get() == NULL)
		InitializePeerConnection();

	peer_connection_.get()->CreateOffer(this, NULL);
}

std::string trim(std::string str) {
	str.erase(std::remove(str.begin(), str.end(), '\"'), str.end());
	str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
	return str;
}


bool PeerConnectionWrapper::InitializePeerConnection() {
	LOG(INFO) << "\n InitializePeerConnection *********************** " << this->easyRtcId;
	ASSERT(peer_connection_.get() == NULL);


	if (!peer_connection_factory_.get()) {
//		mainWindow_->MessageBox("Error", "Failed to initialize PeerConnectionFactory", true);
		DeletePeerConnection();
		return false;
	}

	webrtc::PeerConnectionInterface::IceServers servers;

	Json::Reader reader;
	Json::Value jice;

	if (iceCandidatesFromSS_.length() > 0 && !reader.parse(iceCandidatesFromSS_, jice)) {
		LOG(WARNING) << "Received unknown message. " << iceCandidatesFromSS_;
		return false;
	}

	LOG(INFO) << "incoming ice setup:";
	for (unsigned int i = 0; i < jice.size(); i++) {
		Json::Value jobj = jice[i];

		webrtc::PeerConnectionInterface::IceServer server; // = new webrtc::PeerConnectionInterface::IceServer();
		if (!jobj.isMember("url"))
			continue;

		server.uri = trim(jobj["url"].toStyledString());
		if (jobj.isMember("username"))
			server.username = trim(jobj["username"].toStyledString());
		if (jobj.isMember("credential"))
			server.password = trim(jobj["credential"].toStyledString());

		LOG(INFO) << "c++ " << server.uri << "|" << server.username << "|" << server.password;
		servers.push_back(server);

		server.username = "";
		server.password = "";
	}

//	webrtc::PeerConnectionInterface::IceServer server;
//	server.uri = "stun:stun.l.google.com:19302";
//	servers.push_back(server);

	peer_connection_ = peer_connection_factory_.get()->CreatePeerConnection(servers, this, NULL, this); // NULL: media constraints

	if (!peer_connection_.get()) {
//		mainWindow_->MessageBox("Error", "CreatePeerConnection failed", true);
		DeletePeerConnection();
	}

	AddStreams();
	return peer_connection_.get() != NULL;
}

void PeerConnectionWrapper::DeletePeerConnection() {
	if (peer_connection_){
		peer_connection_->Close();
		peer_connection_.release();
	}
	active_streams_.clear();
	//device_controller_->StopLocalRenderer();
	device_controller_->StopRemoteRenderers();
}

//
// PeerConnectionObserver implementation.
//

void PeerConnectionWrapper::OnError() {
	LOG(LS_ERROR) << __FUNCTION__;
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::PEER_CONNECTION_ERROR, NULL);
}

// Called when a remote stream is added
void PeerConnectionWrapper::OnAddStream(webrtc::MediaStreamInterface* stream) {
	LOG(INFO) << __FUNCTION__ << " " << stream->label();

	stream->AddRef();
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::NEW_STREAM_ADDED, new EasyRtcStream(this->easyRtcId, stream));
}

void PeerConnectionWrapper::OnRemoveStream(webrtc::MediaStreamInterface* stream) {
	LOG(INFO) << __FUNCTION__ << " " << stream->label();
	stream->AddRef();
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::STREAM_REMOVED, new EasyRtcStream(this->easyRtcId, stream));
}

void PeerConnectionWrapper::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
	LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
	Json::StyledWriter writer;
	Json::Value jmessage;

	jmessage["easyrtcid"] = this->GetEasyRtcId();
	jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
	jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
	std::string sdp;
	if (!candidate->ToString(&sdp)) {
		LOG(LS_ERROR) << "Failed to serialize candidate";
		return;
	}
	jmessage[kCandidateSdpName] = sdp;
	PostToBrowser(writer.write(jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void PeerConnectionWrapper::OnSignedIn() {
	LOG(INFO) << __FUNCTION__;
}

void PeerConnectionWrapper::OnDisconnected() {
	LOG(INFO) << __FUNCTION__;

	DeletePeerConnection();
}

void PeerConnectionWrapper::OnPeerConnected(int id, const std::string& name) {
	LOG(INFO) << __FUNCTION__;
}

void PeerConnectionWrapper::OnPeerDisconnected(int id) {
	LOG(INFO) << __FUNCTION__;
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::PEER_CONNECTION_CLOSED, NULL);
}

void PeerConnectionWrapper::OnMessageFromPeer(int notused, const std::string& message) {
	LOG(INFO) << "OnMessageFromPeer: " << message;
	ASSERT(!message.empty());

	if (!peer_connection_.get()) {
		if (!InitializePeerConnection()) {
			LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
			return;
		}
	}

	Json::Reader reader;
	Json::Value jmessage;
	if (!reader.parse(message, jmessage)) {
		LOG(WARNING) << "Received unknown message. " << message;
		return;
	}
	std::string type;
	std::string json_object;

	GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type);
	if (!type.empty()) {
		std::string sdp;
		if (!GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp)) {
			LOG(WARNING) << "Can't parse received session description message.";
			return;
		}
		webrtc::SessionDescriptionInterface* session_description(webrtc::CreateSessionDescription(type, sdp));
		if (!session_description) {
			LOG(WARNING) << "Can't parse received session description message.";
			return;
		}
		LOG(INFO) << " Received session description :" << message;
		peer_connection_->SetRemoteDescription(DummySetSessionDescriptionObserver::Create(), session_description);
		if (session_description->type() == webrtc::SessionDescriptionInterface::kOffer)
			peer_connection_->CreateAnswer(this, NULL);
	}
	else {
		std::string sdp_mid;
		int sdp_mlineindex = 0;
		std::string sdp;

		// debug log if these are all there
		bool x = GetStringFromJsonObject(jmessage, kCandidateSdpMidName, &sdp_mid);
		bool y = GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName, &sdp_mlineindex);
		bool z = GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp);
		LOG(INFO) << "OnMessageFromPeer() " << x << y << z;

		if (!GetStringFromJsonObject(jmessage, kCandidateSdpMidName, &sdp_mid) ||
			!GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName, &sdp_mlineindex) || 
			!GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
			LOG(WARNING) << "Can't parse received message.";
			return;
		}
		talk_base::scoped_ptr<webrtc::IceCandidateInterface> candidate(webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp));
		if (!candidate.get()) {
			LOG(WARNING) << "Can't parse received candidate message.";
			return;
		}
		if (!peer_connection_->AddIceCandidate(candidate.get())) {
			LOG(WARNING) << "Failed to apply the received candidate";
			return;
		}
		LOG(INFO) << " Received candidate :" << message;
	}
}

void PeerConnectionWrapper::OnMessageSent(int err) {
	// Process the next pending message if any.
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::SEND_MESSAGE_TO_PEER, NULL);
}

void PeerConnectionWrapper::OnServerConnectionFailure() {
//	mainWindow_->MessageBox("Error", ("Failed to connect to " + server_).c_str(), true);
}

// If you see an assert of capturer != null, it's probably this function's fault.
void PeerConnectionWrapper::AddStreams() {

	if (active_streams_.find(kStreamLabel) != active_streams_.end())
		return;  // Already added.

	auto stream = device_controller_->GetLocalMediaStream();

	if (!peer_connection_->AddStream(stream, NULL)) {
		LOG(LS_ERROR) << "Adding stream to PeerConnection failed";
	}

	typedef std::pair<std::string, talk_base::scoped_refptr<webrtc::MediaStreamInterface> >	MediaStreamPair;
	active_streams_.insert(MediaStreamPair(stream->label(), stream));
}

void PeerConnectionWrapper::SendToBrowser(const std::string& json){
	PostToBrowser(json);
}


void PeerConnectionWrapper::UIThreadCallback(int msg_id, void* data) {
	std::string* msg;
	switch (msg_id)	{

	case DeviceController::PEER_CONNECTION_CLOSED:
			LOG(INFO) << "PEER_CONNECTION_CLOSED";
			DeletePeerConnection();
			ASSERT(active_streams_.empty());
			break;

	case DeviceController::SEND_MESSAGE_TO_BROWSER:
			msg = reinterpret_cast<std::string*>(data);
			atl_control->SendToBrowser(*msg);
			delete msg; //the other thread created msg, but we have to delete it.
			break;

	case DeviceController::SEND_MESSAGE_TO_PEER:
			LOG(INFO) << "SEND_MESSAGE_TO_PEER";
			msg = reinterpret_cast<std::string*>(data);
			if (msg) {
				// For convenience, we always run the message through the queue.
				// This way we can be sure that messages are sent to the server
				// in the same order they were signaled without much hassle.
				pending_messages_.push_back(msg);
			}

			if (!pending_messages_.empty()) {
				msg = pending_messages_.front();
				pending_messages_.pop_front();
				delete msg;
			}

			break;

	case DeviceController::PEER_CONNECTION_ERROR:
//			mainWindow_->MessageBox("Error", "an unknown error occurred", true);
			LOG(INFO) << "Peer connection error occurred.";
			break;

	case DeviceController::NEW_STREAM_ADDED: {
			
			auto estream = reinterpret_cast<EasyRtcStream*>(data);
			auto stream = estream->getStream();
			auto tracks = stream->GetVideoTracks();

			// Only render the first track.
			if (!tracks.empty()) {
				auto track = tracks[0];
				device_controller_->AddRemoteRenderer(estream->getEasyRtcId(), track);
			}
			stream->Release();

			break;
		}

	case DeviceController::STREAM_REMOVED: {
			// Remote peer stopped sending a stream.
			auto stream = reinterpret_cast<webrtc::MediaStreamInterface*>(data);
			stream->Release();
			break;
		}

		default:
			ASSERT(false);
			break;
	}
}

// seems to be called twice
void PeerConnectionWrapper::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
	LOG(INFO) << __FUNCTION__ ;
	peer_connection_->SetLocalDescription(DummySetSessionDescriptionObserver::Create(), desc);
	Json::StyledWriter writer;
	Json::Value jmessage;

	jmessage["easyrtcid"] = this->GetEasyRtcId();
	jmessage[kSessionDescriptionTypeName] = desc->type();

	std::string sdp;
	desc->ToString(&sdp);

	jmessage[kSessionDescriptionSdpName] = sdp;
	PostToBrowser(writer.write(jmessage));			// <-- send to signal server (JS)
}

void PeerConnectionWrapper::OnFailure(const std::string& error) {
	LOG(LERROR) << error;
}


// inherited generic callback from CreateSessionDescriptionObserver (returns SDP and candidates)
void PeerConnectionWrapper::PostToBrowser(const std::string& json_object) {
	std::string* json = new std::string(json_object); // must be deleted on the other side, after it's consumed.
	device_controller_->QueueUIThreadCallback(this->easyRtcId, DeviceController::SEND_MESSAGE_TO_BROWSER, json);
}
