// N Air Loudness - ラウドネスメーター
//
// 1つの Meter が 1つの音声タップ (出力ミックストラック or ソース) を監視し、
// libebur128 で EBU R 128 / ITU-R BS.1770 のラウドネスを計算する。
//
// N Air Loudness is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include <obs-module.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../thirdparty/libebur128/ebur128.h"

// 音声を取り出す位置
struct MeterTap {
	enum class Kind {
		Track,  // 出力ミックストラック (obs_add_raw_audio_callback)
		Source, // ソース単位 (obs_source_add_audio_capture_callback)
	};

	Kind kind = Kind::Track;
	size_t mixIdx = 0;      // Kind::Track: 0 始まりのトラック番号
	std::string sourceName; // Kind::Source: ソース名
	bool postFader = false; // Kind::Source: true ならミキサー音量・ミュートを反映する
};

struct LoudnessSnapshot {
	// 値が未確定 (計測開始直後・無音) の場合は nullopt
	std::optional<double> momentary;  // LUFS, 400ms
	std::optional<double> shortTerm;  // LUFS, 3s
	std::optional<double> integrated; // LUFS, ゲート付き全区間
	std::optional<double> range;      // LU, LRA
	uint64_t frames = 0;              // reset 以降に処理したフレーム数
};

class Meter {
public:
	Meter(const MeterTap &tap, uint32_t channels, uint32_t sampleRate);
	~Meter();

	Meter(const Meter &) = delete;
	Meter &operator=(const Meter &) = delete;

	// 音声コールバックを登録する。失敗時は false
	bool attach();
	// 音声コールバックを解除し、計算スレッドを停止する。何度呼んでもよい
	void detach();

	LoudnessSnapshot snapshot();
	// Integrated / LRA を含めて計測をやり直す
	void reset();

	const MeterTap &tap() const { return tap_; }

private:
	static void onRawAudio(void *param, size_t mixIdx, struct audio_data *data);
	static void onSourceAudio(void *param, obs_source_t *source, const struct audio_data *data, bool muted);

	// audio スレッドから呼ばれる。バッファに積むだけで計算はしない
	void push(const struct audio_data *data, float mul);
	void workerLoop();
	ebur128_state *createState() const;

	const MeterTap tap_;
	const uint32_t channels_;
	const uint32_t sampleRate_;

	obs_weak_source_t *weakSource_ = nullptr;
	bool attached_ = false;

	// audio スレッド -> 計算スレッドの受け渡し (インターリーブ済み float)
	std::mutex pendingMutex_;
	std::condition_variable pendingCv_;
	std::vector<float> pending_;
	bool running_ = false;
	std::thread worker_;

	// libebur128 の状態。計算スレッドと RPC スレッドから触る
	std::mutex stateMutex_;
	ebur128_state *state_ = nullptr;
	uint64_t frames_ = 0;
};
