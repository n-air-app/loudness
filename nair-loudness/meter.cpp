// N Air Loudness - ラウドネスメーター
//
// N Air Loudness is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.

#include "meter.hpp"

#include <cmath>

namespace {

// 計算スレッドが止まった場合に audio スレッド側のバッファが際限なく伸びないようにする上限 (秒)
constexpr uint32_t MAX_PENDING_SECONDS = 10;

// Integrated / LRA を計算するため M | S | I | LRA を有効にする。
// True Peak はオーバーサンプリングで重いので、必要になるまで入れない。
constexpr int EBUR128_MODE = EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA;

// 窓が音声で埋まるまでの長さ (EBU Tech 3341)
constexpr uint64_t MOMENTARY_WINDOW_MS = 400;
constexpr uint64_t SHORT_TERM_WINDOW_MS = 3000;

std::optional<double> toLoudness(int err, double value)
{
	// libebur128 は値が確定しないとき -HUGE_VAL を返す
	if (err != EBUR128_SUCCESS || !std::isfinite(value))
		return std::nullopt;
	return value;
}

} // namespace

Meter::Meter(const MeterTap &tap, uint32_t channels, uint32_t sampleRate)
	: tap_(tap), channels_(channels), sampleRate_(sampleRate)
{
	state_ = createState();
}

Meter::~Meter()
{
	detach();
	if (state_)
		ebur128_destroy(&state_);
}

ebur128_state *Meter::createState() const
{
	// チャンネル配置は libebur128 のデフォルト (L, R, C, LFE=unused, Ls, Rs) が
	// OBS の SPEAKERS_STEREO / SPEAKERS_5POINT1 の並びと一致する
	return ebur128_init(channels_, sampleRate_, EBUR128_MODE);
}

bool Meter::attach()
{
	if (attached_ || !state_)
		return false;

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		running_ = true;
	}
	worker_ = std::thread(&Meter::workerLoop, this);

	if (tap_.kind == MeterTap::Kind::Track) {
		if (tap_.mixIdx >= MAX_AUDIO_MIXES) {
			detach();
			return false;
		}
		// 値初期化 (()) で zero[] をゼロ埋めする
		rawParam_ = std::unique_ptr<RawAudioParam>(new RawAudioParam());
		rawParam_->meter = this;
		obs_add_raw_audio_callback(tap_.mixIdx, nullptr, &Meter::onRawAudio, rawParam_.get());
	} else {
		obs_source_t *source = obs_get_source_by_name(tap_.sourceName.c_str());
		if (!source) {
			detach();
			return false;
		}
		// ソースの寿命はアプリ側が管理するので、こちらは弱参照だけ持つ
		weakSource_ = obs_source_get_weak_source(source);
		obs_source_add_audio_capture_callback(source, &Meter::onSourceAudio, this);
		obs_source_release(source);
	}

	attached_ = true;
	return true;
}

void Meter::detach()
{
	// コールバック解除は libobs 側のロック (audio の input_mutex / source の audio_cb_mutex) を取る。
	// コールバック実行中もそのロックが取られているため、解除から戻った後は push() は呼ばれない。
	// pendingMutex_ を持ったまま解除するとデッドロックするので、ロックは取らずに呼ぶこと。
	if (attached_) {
		if (tap_.kind == MeterTap::Kind::Track) {
			obs_remove_raw_audio_callback(tap_.mixIdx, &Meter::onRawAudio, rawParam_.get());
		} else if (weakSource_) {
			// ソースが既に破棄されていればコールバックリストごと消えているので何もしなくてよい
			obs_source_t *source = obs_weak_source_get_source(weakSource_);
			if (source) {
				obs_source_remove_audio_capture_callback(source, &Meter::onSourceAudio, this);
				obs_source_release(source);
			}
		}
		attached_ = false;
	}

	if (weakSource_) {
		obs_weak_source_release(weakSource_);
		weakSource_ = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		running_ = false;
	}
	pendingCv_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

void Meter::onRawAudio(void *param, size_t, struct audio_data *data)
{
	static_cast<RawAudioParam *>(param)->meter->push(data, 1.0f);
}

void Meter::onSourceAudio(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	auto *meter = static_cast<Meter *>(param);
	float mul = 1.0f;
	if (meter->tap_.postFader)
		mul = muted ? 0.0f : obs_source_get_volume(source);
	meter->push(data, mul);
}

void Meter::push(const struct audio_data *data, float mul)
{
	// 注意: OBS の音声設定 (サンプルレート・チャンネル数) は再起動しないと変わらない前提で、
	// channels_ は create 時の値を使う
	const size_t frames = data->frames;
	if (!frames)
		return;

	std::lock_guard<std::mutex> lock(pendingMutex_);
	if (!running_)
		return;

	if (pending_.size() > size_t(MAX_PENDING_SECONDS) * sampleRate_ * channels_)
		pending_.clear();

	const size_t offset = pending_.size();
	pending_.resize(offset + frames * channels_);
	float *out = pending_.data() + offset;

	// planar -> interleaved。プレーンが欠けていれば無音として扱う
	for (size_t ch = 0; ch < channels_; ch++) {
		const float *in = reinterpret_cast<const float *>(data->data[ch]);
		for (size_t i = 0; i < frames; i++)
			out[i * channels_ + ch] = in ? in[i] * mul : 0.0f;
	}

	pendingCv_.notify_one();
}

void Meter::workerLoop()
{
	std::vector<float> work;

	for (;;) {
		{
			std::unique_lock<std::mutex> lock(pendingMutex_);
			pendingCv_.wait(lock, [this] { return !running_ || !pending_.empty(); });
			if (!running_)
				return;
			work.swap(pending_);
		}

		const size_t frames = work.size() / channels_;
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			if (state_) {
				ebur128_add_frames_float(state_, work.data(), frames);
				frames_ += frames;
			}
		}
		work.clear();
	}
}

LoudnessSnapshot Meter::snapshot()
{
	LoudnessSnapshot s;
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (!state_)
		return s;

	// libebur128 は窓が埋まる前でも、足りない分を無音として計算した値を返す。
	// 計測開始直後に実際より小さい値が出るのを避けるため、窓が埋まるまでは null にする
	const bool momentaryReady = frames_ * 1000 >= MOMENTARY_WINDOW_MS * sampleRate_;
	const bool shortTermReady = frames_ * 1000 >= SHORT_TERM_WINDOW_MS * sampleRate_;

	double value;
	int err;

	if (momentaryReady) {
		err = ebur128_loudness_momentary(state_, &value);
		s.momentary = toLoudness(err, value);
	}
	if (shortTermReady) {
		err = ebur128_loudness_shortterm(state_, &value);
		s.shortTerm = toLoudness(err, value);
	}
	// Integrated はゲートを通るブロックがなければ -HUGE_VAL (= null) になる
	err = ebur128_loudness_global(state_, &value);
	s.integrated = toLoudness(err, value);
	// LRA はゲートを通る short-term ブロックがないと -HUGE_VAL ではなく 0.0 を返すので、
	// 本当に 0 LU なのか区別できない。short-term ブロックは 3 秒経つまで作られず、
	// 無音 (-70 LUFS 未満) のブロックは数えられない。ブロック数は公開 API で取れないため、
	// 「3 秒経過 かつ Integrated が確定 (= -70 LUFS を超える音がある)」を代わりの条件にする
	if (shortTermReady && s.integrated) {
		err = ebur128_loudness_range(state_, &value);
		s.range = toLoudness(err, value);
	}
	s.frames = frames_;
	return s;
}

void Meter::reset()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	if (state_)
		ebur128_destroy(&state_);
	state_ = createState();
	frames_ = 0;
}
