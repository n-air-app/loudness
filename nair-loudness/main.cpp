// N Air Loudness - OBS Studioプラグイン
//
// 配信出力のミックストラックや個別ソースの音声を EBU R 128 / ITU-R BS.1770 で計測し、
// 名前付きパイプ経由の JSON RPC で N Air アプリケーションに結果を返す。
//
// Copyright (C) DWANGO Co., Ltd.
//
// N Air Loudness is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#define STRICT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <obs-module.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../thirdparty/json.hpp"
using json = nlohmann::json;

#include "meter.hpp"
#include "version.h"

#pragma comment(lib, "obs.lib")

namespace {

// 名前付きパイプの設定 - 名前がユニークなので複数起動はできないことに注意
constexpr wchar_t PIPE_NAME[] = L"\\\\.\\pipe\\NAirLoudness";
constexpr DWORD BUFFER_SIZE = 4096;

HANDLE serverThread = nullptr;
HANDLE stopEvent = nullptr;

std::mutex metersMutex;
std::map<uint64_t, std::unique_ptr<Meter>> meters;
uint64_t nextMeterId = 1;

json toJson(const std::optional<double> &v)
{
	return v ? json(*v) : json(nullptr);
}

json error(const std::string &message)
{
	return {{"error", message}};
}

// {"info"} -> プラグインと OBS の音声設定
json getInfo()
{
	obs_audio_info oai = {};
	const bool hasAudio = obs_get_audio_info(&oai);
	return {
		{"version", VERSION},
		{"targetLibobs", TARGET_LIBOBS_VERSION},
		{"targetObsStudioNode", TARGET_OBS_STUDIO_NODE_VERSION},
		{"runtimeLibobs", obs_get_version_string()},
		{"sampleRate", hasAudio ? json(oai.samples_per_sec) : json(nullptr)},
		{"channels", hasAudio ? json(get_audio_channels(oai.speakers)) : json(nullptr)},
	};
}

// {"create", {"type": "track", "track": 0}}
// {"create", {"type": "source", "source": "マイク", "postFader": false}}
// -> {"meterId": n}
json createMeter(const json &arg)
{
	MeterTap tap;
	const auto type = arg.value("type", std::string());
	if (type == "track") {
		tap.kind = MeterTap::Kind::Track;
		tap.mixIdx = arg.value("track", size_t(0));
	} else if (type == "source") {
		tap.kind = MeterTap::Kind::Source;
		tap.sourceName = arg.value("source", std::string());
		tap.postFader = arg.value("postFader", false);
		if (tap.sourceName.empty())
			return error("source is required");
	} else {
		return error("unknown type: " + type);
	}

	obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai))
		return error("obs_get_audio_info failed");

	auto meter = std::make_unique<Meter>(tap, get_audio_channels(oai.speakers), oai.samples_per_sec);
	if (!meter->attach())
		return error("attach failed");

	std::lock_guard<std::mutex> lock(metersMutex);
	const uint64_t id = nextMeterId++;
	meters.emplace(id, std::move(meter));
	return {{"meterId", id}};
}

// {"get", {"meterId": n}} -> {"momentary", "shortTerm", "integrated", "range", "frames"}
// 値が確定していない項目は null
json getMeter(const json &arg)
{
	const auto id = arg.value("meterId", uint64_t(0));
	std::lock_guard<std::mutex> lock(metersMutex);
	auto it = meters.find(id);
	if (it == meters.end())
		return error("meter not found");

	const auto s = it->second->snapshot();
	return {
		{"momentary", toJson(s.momentary)},
		{"shortTerm", toJson(s.shortTerm)},
		{"integrated", toJson(s.integrated)},
		{"range", toJson(s.range)},
		{"frames", s.frames},
	};
}

json resetMeter(const json &arg)
{
	const auto id = arg.value("meterId", uint64_t(0));
	std::lock_guard<std::mutex> lock(metersMutex);
	auto it = meters.find(id);
	if (it == meters.end())
		return error("meter not found");
	it->second->reset();
	return {{"result", "OK"}};
}

json destroyMeter(const json &arg)
{
	const auto id = arg.value("meterId", uint64_t(0));
	std::unique_ptr<Meter> meter;
	{
		std::lock_guard<std::mutex> lock(metersMutex);
		auto it = meters.find(id);
		if (it == meters.end())
			return error("meter not found");
		meter = std::move(it->second);
		meters.erase(it);
	}
	// detach は計算スレッドの join を含むので、metersMutex の外で破棄する
	meter.reset();
	return {{"result", "OK"}};
}

void destroyAllMeters()
{
	std::map<uint64_t, std::unique_ptr<Meter>> removed;
	{
		std::lock_guard<std::mutex> lock(metersMutex);
		removed.swap(meters);
	}
	removed.clear();
}

json dispatch(const json &request)
{
	const auto id = request.value("id", std::string());
	const auto fn = request.value("fn", std::string());
	const json arg = request.contains("arg") ? request["arg"] : json::object();

	json result;
	try {
		if (fn == "info")
			result = getInfo();
		else if (fn == "create")
			result = createMeter(arg);
		else if (fn == "get")
			result = getMeter(arg);
		else if (fn == "reset")
			result = resetMeter(arg);
		else if (fn == "destroy")
			result = destroyMeter(arg);
		else if (fn == "destroyAll") {
			destroyAllMeters();
			result = {{"result", "OK"}};
		} else
			result = error("unknown function: " + fn);
	} catch (const json::exception &e) {
		result = error(std::string("invalid argument: ") + e.what());
	}

	return {{"id", id}, {"res", result}};
}

// overlapped I/O の完了か stopEvent のどちらかを待つ。停止要求なら false
bool waitIo(HANDLE pipe, OVERLAPPED &ov, DWORD &transferred)
{
	HANDLE handles[] = {ov.hEvent, stopEvent};
	const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	if (r != WAIT_OBJECT_0) {
		CancelIoEx(pipe, &ov);
		GetOverlappedResult(pipe, &ov, &transferred, TRUE);
		return false;
	}
	return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
}

bool writeAll(HANDLE pipe, OVERLAPPED &ov, const std::string &data)
{
	size_t written = 0;
	while (written < data.size()) {
		DWORD n = 0;
		ResetEvent(ov.hEvent);
		if (!WriteFile(pipe, data.data() + written, DWORD(data.size() - written), nullptr, &ov) &&
		    GetLastError() != ERROR_IO_PENDING)
			return false;
		if (!waitIo(pipe, ov, n))
			return false;
		written += n;
	}
	return true;
}

// 1クライアント分の接続を処理する。切断されたら戻る
void serveClient(HANDLE pipe, OVERLAPPED &ov)
{
	char buffer[BUFFER_SIZE];
	std::string received;

	for (;;) {
		DWORD n = 0;
		ResetEvent(ov.hEvent);
		if (!ReadFile(pipe, buffer, BUFFER_SIZE, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
			return;
		if (!waitIo(pipe, ov, n))
			return;
		if (!n)
			continue;

		received.append(buffer, n);

		// 改行区切りで JSON を処理
		size_t pos;
		while ((pos = received.find('\n')) != std::string::npos) {
			std::string line = received.substr(0, pos);
			received.erase(0, pos + 1);
			if (line.empty() || line == "\r")
				continue;

			json request;
			try {
				request = json::parse(line);
			} catch (const json::parse_error &e) {
				blog(LOG_WARNING, "[nair-loudness] JSON parse error: %s", e.what());
				continue;
			}
			if (!writeAll(pipe, ov, dispatch(request).dump(-1) + "\n"))
				return;
		}
	}
}

// 名前付きパイプでコマンドを受け付けるスレッド。
// クライアント (N Air) が再接続してもよいように、切断後は次の接続を待つ。
DWORD WINAPI serve(LPVOID)
{
	OVERLAPPED ov = {};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent)
		return 1;

	while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
		HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
					       BUFFER_SIZE, BUFFER_SIZE, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			blog(LOG_ERROR, "[nair-loudness] Failed to create named pipe: %lu", GetLastError());
			break;
		}

		bool connected = false;
		ResetEvent(ov.hEvent);
		if (ConnectNamedPipe(pipe, &ov)) {
			connected = true;
		} else {
			switch (GetLastError()) {
			case ERROR_PIPE_CONNECTED:
				connected = true;
				break;
			case ERROR_IO_PENDING: {
				DWORD unused;
				connected = waitIo(pipe, ov, unused);
				break;
			}
			default:
				break;
			}
		}

		if (connected) {
			blog(LOG_INFO, "[nair-loudness] client connected");
			serveClient(pipe, ov);
			blog(LOG_INFO, "[nair-loudness] client disconnected");
			DisconnectNamedPipe(pipe);
		}
		CloseHandle(pipe);
	}

	CloseHandle(ov.hEvent);
	return 0;
}

} // namespace

// OBSプラグインのエントリーポイント
extern "C" {
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("nair-loudness", "en-US")

bool obs_module_load(void)
{
	blog(LOG_INFO, "[nair-loudness] plugin %s loaded (built for libobs %s / obs-studio-node %s; runtime libobs %s)",
	     VERSION, TARGET_LIBOBS_VERSION, TARGET_OBS_STUDIO_NODE_VERSION, obs_get_version_string());

	stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!stopEvent)
		return false;

	serverThread = CreateThread(nullptr, 0, serve, nullptr, 0, nullptr);
	if (!serverThread) {
		blog(LOG_ERROR, "[nair-loudness] Failed to create thread.");
		CloseHandle(stopEvent);
		stopEvent = nullptr;
		return false;
	}
	return true;
}

// 音声コールバックが残ったまま DLL がアンロードされるとクラッシュするため、
// パイプスレッドを止めてから全メーターを解除する
void obs_module_unload(void)
{
	if (serverThread) {
		SetEvent(stopEvent);
		WaitForSingleObject(serverThread, INFINITE);
		CloseHandle(serverThread);
		serverThread = nullptr;
	}
	if (stopEvent) {
		CloseHandle(stopEvent);
		stopEvent = nullptr;
	}

	destroyAllMeters();
}
}
