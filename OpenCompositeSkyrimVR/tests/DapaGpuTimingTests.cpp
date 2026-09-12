#include "DrvOpenXR/DapaGpuTiming.h"
#include <d3d11sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void Run(D3D_DRIVER_TYPE driver)
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> ctx;
	Check(SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
	    D3D11_SDK_VERSION, &device, nullptr, &ctx)), "create D3D11 device");
	ComPtr<ID3D11InfoQueue> debug;
	Check(SUCCEEDED(device.As(&debug)), "debug queue");
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = desc.Height = 1024;
	desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11RenderTargetView> rtv;
	Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex)), "create texture");
	Check(SUCCEEDED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv)), "create RTV");
	DapaGpuTiming timer;
	{ auto sample = timer.Measure(ctx.Get(), DapaGpuTiming::Stage::WarpLeft, false); Check(!sample.Active(), "disabled diagnostics submit no queries"); }
	for (size_t i = 0; i < static_cast<size_t>(DapaGpuTiming::Stage::Count); ++i) {
		auto sample = timer.Measure(ctx.Get(), static_cast<DapaGpuTiming::Stage>(i), true);
		Check(sample.Active(), "timestamp starts");
		for (int clear = 0; clear < 32; ++clear) { const float color[] = {float(clear % 2), .5f, .25f, 1}; ctx->ClearRenderTargetView(rtv.Get(), color); }
	}
	{ auto sample = timer.Measure(ctx.Get(), DapaGpuTiming::Stage::WarpLeft, true); Check(!sample.Active(), "pending timestamp is never overwritten"); }
	// Only this standalone test submits its otherwise idle command buffer. The
	// production timer has no Flush/wait; regular game rendering submits it.
	ctx->Flush();
	int results = 0;
	const auto deadline = DapaGpuTiming::NowMs() + 5000;
	while (results < int(DapaGpuTiming::Stage::Count) && DapaGpuTiming::NowMs() < deadline) {
		timer.Poll(ctx.Get(), [&](const auto& result) { Check(result.valid && result.gpuMs >= 0, "valid GPU timestamp interval"); ++results; });
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	Check(results == int(DapaGpuTiming::Stage::Count), "all GPU stages resolved asynchronously");
	timer.Reset();
	{ auto sample = timer.Measure(ctx.Get(), DapaGpuTiming::Stage::WarpLeft, true); Check(sample.Active(), "reset clears device queries and deadlines"); }
	ctx->Flush();
	for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
		SIZE_T size = 0; debug->GetMessage(i, nullptr, &size);
		std::vector<unsigned char> bytes(size); auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
		debug->GetMessage(i, message, &size);
		if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { std::puts(message->pDescription); Check(false, "D3D11 validation warning"); }
	}
	std::printf("PASS %s: asynchronous GPU timestamps, checkbox off, pending ownership, reset, no D3D11 warnings\n", driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
}
int main() { try { Run(D3D_DRIVER_TYPE_WARP); Run(D3D_DRIVER_TYPE_HARDWARE); return 0; } catch (const std::exception& e) { std::puts(e.what()); return 1; } }
