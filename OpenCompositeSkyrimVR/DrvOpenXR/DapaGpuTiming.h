#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstdint>

// Sparse GPU timestamps, independent of the frame pacing policy. Never flush,
// spin, map a resource, or wait for a GPU result on the application's thread.
class DapaGpuTiming {
public:
	enum class Stage : size_t { CacheLeft, CacheRight, WarpLeft, WarpRight, Output, DepthOutput, Count };
	static const char* Name(Stage stage)
	{
		constexpr const char* names[] = { "cache-left", "cache-right", "warp-left", "warp-right", "output-colour", "output-depth" };
		return names[static_cast<size_t>(stage)];
	}
	static uint64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	struct Result { Stage stage; bool valid; double gpuMs; uint64_t readyObservedAfterMs; HRESULT status; };
	class Scope {
	public:
		Scope(DapaGpuTiming& owner, ID3D11DeviceContext* context, Stage stage, bool enabled)
		    : timer(owner), ctx(context), index(static_cast<size_t>(stage)), active(owner.Begin(context, index, enabled)) {}
		~Scope() { if (active) timer.End(ctx, index); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
		bool Active() const { return active; }
	private:
		DapaGpuTiming& timer;
		ID3D11DeviceContext* ctx;
		size_t index;
		bool active;
	};
	Scope Measure(ID3D11DeviceContext* ctx, Stage stage, bool enabled)
	{
		return Scope(*this, ctx, stage, enabled);
	}
	void Reset()
	{
		slots = {};
		device.Reset();
		nextPollMs = 0;
		unavailable = false;
	}
	bool Unavailable() const { return unavailable; }
	template<class Report> void Poll(ID3D11DeviceContext* ctx, Report report)
	{
		if (!ctx || !device) return;
		const auto now = NowMs();
		if (now < nextPollMs) return;
		nextPollMs = now + 100;
		for (size_t i = 0; i < slots.size(); ++i) {
			auto& slot = slots[i];
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
			uint64_t begin{}, end{};
			HRESULT status = ctx->GetData(slot.disjoint.Get(), &frequency, sizeof(frequency), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (status == S_FALSE) continue;
			if (status == S_OK) status = ctx->GetData(slot.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (status == S_FALSE) continue;
			if (status == S_OK) status = ctx->GetData(slot.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (status == S_FALSE) continue;
			const bool valid = status == S_OK && !frequency.Disjoint && frequency.Frequency && end >= begin;
			report(Result{static_cast<Stage>(i), valid, valid ? double(end - begin) * 1000.0 / frequency.Frequency : 0.0,
			    now - slot.queuedMs, status});
			slot.pending = false;
		}
	}
private:
	struct Slot {
		Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
		bool pending = false;
		uint64_t nextMs = 0, queuedMs = 0;
	};
	std::array<Slot, static_cast<size_t>(Stage::Count)> slots{};
	Microsoft::WRL::ComPtr<ID3D11Device> device;
	uint64_t nextPollMs = 0;
	bool unavailable = false;
	bool Begin(ID3D11DeviceContext* ctx, size_t index, bool enabled)
	{
		if (!enabled || !ctx || unavailable || index >= slots.size()) return false;
		auto& slot = slots[index];
		const auto now = NowMs();
		if (slot.pending || now < slot.nextMs) return false;
		if (!device) ctx->GetDevice(&device);
		if (!slot.disjoint) {
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			HRESULT result = device->CreateQuery(&desc, &slot.disjoint);
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (SUCCEEDED(result)) result = device->CreateQuery(&desc, &slot.begin);
			if (SUCCEEDED(result)) result = device->CreateQuery(&desc, &slot.end);
			if (FAILED(result)) { unavailable = true; return false; }
		}
		slot.queuedMs = now;
		slot.nextMs = now + 1000;
		ctx->Begin(slot.disjoint.Get());
		ctx->End(slot.begin.Get());
		return true;
	}
	void End(ID3D11DeviceContext* ctx, size_t index)
	{
		auto& slot = slots[index];
		ctx->End(slot.end.Get());
		ctx->End(slot.disjoint.Get());
		slot.pending = true;
	}
};
