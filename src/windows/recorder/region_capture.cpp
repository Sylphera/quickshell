#include "region_capture.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <qt_windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <inspectable.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpoint.h>
#include <qrect.h>
#include <qstring.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "../cursor_image.hpp"
#include "../screen_zoom.hpp"
#include "../util.hpp"

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

namespace qs::windows::recorder {

namespace {
Q_LOGGING_CATEGORY(logRegionCapture, "quickshell.windows.recorder", QtWarningMsg);

constexpr DXGI_FORMAT FRAME_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DirectXPixelFormat FRAME_PIXEL_FORMAT = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int POOL_BUFFERS = 2;

QString hresultString(const winrt::hresult_error& e) {
	return QString("0x%1 %2")
	    .arg(static_cast<quint32>(e.code().value), 8, 16, QChar('0'))
	    .arg(QString::fromWCharArray(e.message().c_str()));
}

struct SessionFeatures {
	bool border = false;
	bool cursor = false;
};

SessionFeatures sessionFeatures() {
	static std::once_flag once;
	static SessionFeatures features;

	std::call_once(once, [] {
		const auto sessionClass = L"Windows.Graphics.Capture.GraphicsCaptureSession";

		try {
			features.border = ApiInformation::IsPropertyPresent(sessionClass, L"IsBorderRequired");
			features.cursor = ApiInformation::IsPropertyPresent(sessionClass, L"IsCursorCaptureEnabled");

			if (features.border
			    && ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess"))
			{
				auto status =
				    GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get();
				if (status != AppCapabilityAccessStatus::Allowed) {
					qCInfo(logRegionCapture) << "Borderless capture not allowed:" << static_cast<int>(status);
				}
			}
		} catch (const winrt::hresult_error& e) {
			qCWarning(logRegionCapture) << "Capture feature queries failed:" << hresultString(e);
		}
	});

	return features;
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/, LPARAM param) {
	reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor); // NOLINT
	return TRUE;
}

constexpr char ZOOM_SHADER[] = R"hlsl(
Texture2D source : register(t0);
SamplerState nearest : register(s0);
cbuffer Zoom : register(b0) {
	float2 targetSize;
	float factor;
	float unused;
	float2 offset;
	float2 sourceSize;
	float4 rect;
};
struct Vertex {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};
Vertex vertexMain(uint id : SV_VertexID) {
	float2 corner = float2(id & 1, id >> 1);
	Vertex result;
	result.position = float4((rect.xy + corner * rect.zw) / targetSize * float2(2, -2) + float2(-1, 1), 0, 1);
	result.uv = corner;
	return result;
}
float4 framePixel(Vertex input) : SV_Target {
	float2 pixel = (input.position.xy - offset) / factor;
	return float4(source.Sample(nearest, pixel / sourceSize).rgb, 1);
}
float4 cursorPixel(Vertex input) : SV_Target {
	return source.Sample(nearest, input.uv);
}
)hlsl";

struct ZoomConstants {
	float targetWidth;
	float targetHeight;
	float factor;
	float unused;
	float offsetX;
	float offsetY;
	float sourceWidth;
	float sourceHeight;
	float rect[4];
};

winrt::com_ptr<ID3DBlob> compileShader(const char* entry, const char* profile) {
	winrt::com_ptr<ID3DBlob> code;
	winrt::com_ptr<ID3DBlob> errors;
	const auto result = D3DCompile(ZOOM_SHADER, sizeof(ZOOM_SHADER) - 1, "zoom", nullptr, nullptr, entry,
	    profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
	if (FAILED(result) && errors)
		qCWarning(logRegionCapture) << static_cast<const char*>(errors->GetBufferPointer());
	winrt::check_hresult(result);
	return code;
}

bool zoomedOn(HMONITOR monitor, const ScreenZoomView& view) {
	return view.monitor == monitor && view.factor > 1.0f;
}

} // namespace

struct MonitorCapture {
	HMONITOR handle = nullptr;
	QPoint origin;
	QRect crop;
	GraphicsCaptureItem item {nullptr};
	Direct3D11CaptureFramePool pool {nullptr};
	GraphicsCaptureSession session {nullptr};
	winrt::event_token frameToken {};
	winrt::event_token closedToken {};
	SizeInt32 poolSize {};
	bool delivered = false;
	winrt::com_ptr<ID3D11Texture2D> frame;
	winrt::com_ptr<ID3D11ShaderResourceView> frameView;
	winrt::com_ptr<ID3D11Texture2D> zoomed;
	winrt::com_ptr<ID3D11RenderTargetView> zoomedView;
	QSize frameSize;
	QSize visibleSize;
	bool wasZoomed = false;
};

struct RegionCaptureState {
	std::mutex mutex;
	bool running = false;
	bool lost = false;
	int delivered = 0;

	QRect region;
	winrt::com_ptr<ID3D11DeviceContext> context;
	IDirect3DDevice winrtDevice {nullptr};
	winrt::com_ptr<ID3D11Texture2D> composite;
	std::vector<std::unique_ptr<MonitorCapture>> monitors;
	bool cursor = false;

	winrt::com_ptr<ID3D11VertexShader> zoomVertex;
	winrt::com_ptr<ID3D11PixelShader> framePixel;
	winrt::com_ptr<ID3D11PixelShader> cursorPixel;
	winrt::com_ptr<ID3D11SamplerState> nearest;
	winrt::com_ptr<ID3D11BlendState> blend;
	winrt::com_ptr<ID3D11Buffer> constants;
	winrt::com_ptr<ID3D11ShaderResourceView> cursorView;
	HCURSOR cursorHandle = nullptr;
	QPoint cursorHotspot;
	QSize cursorSize;

	void onFrame(MonitorCapture* monitor, const Direct3D11CaptureFramePool& sender);
	void keepFrame(MonitorCapture* monitor, ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& desc);
	void copyPart(MonitorCapture* monitor, ID3D11Texture2D* source);
	void renderZoom(MonitorCapture* monitor, const ScreenZoomView& view);
	void preparePipeline();
	void prepareCursor();
	void refresh();
};

void RegionCaptureState::keepFrame(
    MonitorCapture* monitor,
    ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& desc
) {
	const auto size = QSize(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
	if (!monitor->frame || monitor->frameSize != size) {
		winrt::com_ptr<ID3D11Device> device;
		this->context->GetDevice(device.put());

		D3D11_TEXTURE2D_DESC copy {};
		copy.Width = desc.Width;
		copy.Height = desc.Height;
		copy.MipLevels = 1;
		copy.ArraySize = 1;
		copy.Format = desc.Format;
		copy.SampleDesc.Count = 1;
		copy.Usage = D3D11_USAGE_DEFAULT;
		copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		monitor->frame = nullptr;
		monitor->frameView = nullptr;
		monitor->zoomed = nullptr;
		monitor->zoomedView = nullptr;
		winrt::check_hresult(device->CreateTexture2D(&copy, nullptr, monitor->frame.put()));
		winrt::check_hresult(device->CreateShaderResourceView(monitor->frame.get(), nullptr, monitor->frameView.put()));
		copy.BindFlags = D3D11_BIND_RENDER_TARGET;
		winrt::check_hresult(device->CreateTexture2D(&copy, nullptr, monitor->zoomed.put()));
		winrt::check_hresult(device->CreateRenderTargetView(monitor->zoomed.get(), nullptr, monitor->zoomedView.put()));
		monitor->frameSize = size;
	}

	this->context->CopyResource(monitor->frame.get(), source);
}

void RegionCaptureState::copyPart(MonitorCapture* monitor, ID3D11Texture2D* source) {
	auto part = monitor->crop.intersected(QRect(monitor->origin, monitor->visibleSize));
	if (part.isEmpty()) return;

	D3D11_BOX box {
	    static_cast<UINT>(part.left() - monitor->origin.x()),
	    static_cast<UINT>(part.top() - monitor->origin.y()),
	    0,
	    static_cast<UINT>(part.right() + 1 - monitor->origin.x()),
	    static_cast<UINT>(part.bottom() + 1 - monitor->origin.y()),
	    1,
	};

	this->context->CopySubresourceRegion(
	    this->composite.get(),
	    0,
	    static_cast<UINT>(part.left() - this->region.left()),
	    static_cast<UINT>(part.top() - this->region.top()),
	    0,
	    source,
	    0,
	    &box
	);
	this->context->Flush();

	if (!monitor->delivered) {
		monitor->delivered = true;
		this->delivered++;
	}
}

void RegionCaptureState::preparePipeline() {
	if (this->zoomVertex) return;
	winrt::com_ptr<ID3D11Device> device;
	this->context->GetDevice(device.put());

	const auto vertex = compileShader("vertexMain", "vs_5_0");
	const auto frame = compileShader("framePixel", "ps_5_0");
	const auto cursor = compileShader("cursorPixel", "ps_5_0");
	winrt::check_hresult(device->CreatePixelShader(frame->GetBufferPointer(), frame->GetBufferSize(), nullptr, this->framePixel.put()));
	winrt::check_hresult(device->CreatePixelShader(cursor->GetBufferPointer(), cursor->GetBufferSize(), nullptr, this->cursorPixel.put()));

	D3D11_SAMPLER_DESC sampler {};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	winrt::check_hresult(device->CreateSamplerState(&sampler, this->nearest.put()));

	D3D11_BLEND_DESC blend {};
	auto& target = blend.RenderTarget[0];
	target.BlendEnable = TRUE;
	target.SrcBlend = D3D11_BLEND_ONE;
	target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	target.BlendOp = D3D11_BLEND_OP_ADD;
	target.SrcBlendAlpha = D3D11_BLEND_ONE;
	target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	winrt::check_hresult(device->CreateBlendState(&blend, this->blend.put()));

	D3D11_BUFFER_DESC buffer {};
	buffer.ByteWidth = sizeof(ZoomConstants);
	buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, this->constants.put()));
	winrt::check_hresult(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, this->zoomVertex.put()));
}

void RegionCaptureState::prepareCursor() {
	const auto handle = currentCursorHandle();
	if (handle == nullptr || handle == this->cursorHandle) return;
	this->cursorHandle = handle;

	const auto shape = cursorImage(handle);
	if (shape.image.isNull()) return;

	winrt::com_ptr<ID3D11Device> device;
	this->context->GetDevice(device.put());
	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = static_cast<UINT>(shape.image.width());
	desc.Height = static_cast<UINT>(shape.image.height());
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = FRAME_FORMAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	const D3D11_SUBRESOURCE_DATA data {shape.image.constBits(), static_cast<UINT>(shape.image.bytesPerLine()), 0};
	winrt::com_ptr<ID3D11Texture2D> texture;
	winrt::check_hresult(device->CreateTexture2D(&desc, &data, texture.put()));
	this->cursorView = nullptr;
	winrt::check_hresult(device->CreateShaderResourceView(texture.get(), nullptr, this->cursorView.put()));
	this->cursorHotspot = shape.hotspot;
	this->cursorSize = shape.image.size();
}

void RegionCaptureState::renderZoom(MonitorCapture* monitor, const ScreenZoomView& view) {
	this->preparePipeline();

	const auto width = static_cast<float>(monitor->frameSize.width());
	const auto height = static_cast<float>(monitor->frameSize.height());
	ZoomConstants values {width, height, view.factor, 0, static_cast<float>(view.offset.x()),
	    static_cast<float>(view.offset.y()), width, height, {0, 0, width, height}};
	this->context->UpdateSubresource(this->constants.get(), 0, nullptr, &values, 0, 0);

	const D3D11_VIEWPORT viewport {0, 0, width, height, 0, 1};
	auto* target = monitor->zoomedView.get();
	auto* sampler = this->nearest.get();
	auto* buffer = this->constants.get();
	auto* frame = monitor->frameView.get();
	this->context->RSSetViewports(1, &viewport);
	this->context->OMSetRenderTargets(1, &target, nullptr);
	this->context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	this->context->IASetInputLayout(nullptr);
	this->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	this->context->VSSetShader(this->zoomVertex.get(), nullptr, 0);
	this->context->VSSetConstantBuffers(0, 1, &buffer);
	this->context->PSSetShader(this->framePixel.get(), nullptr, 0);
	this->context->PSSetConstantBuffers(0, 1, &buffer);
	this->context->PSSetSamplers(0, 1, &sampler);
	this->context->PSSetShaderResources(0, 1, &frame);
	this->context->Draw(4, 0);

	POINT pointer {};
	if (this->cursor && GetCursorPos(&pointer)) {
		this->prepareCursor();
		const auto local = QPoint(pointer.x, pointer.y) - monitor->origin - this->cursorHotspot;
		if (this->cursorView && QRect(QPoint(), monitor->frameSize).contains(local + this->cursorHotspot)) {
			values.rect[0] = static_cast<float>(local.x() * view.factor + view.offset.x());
			values.rect[1] = static_cast<float>(local.y() * view.factor + view.offset.y());
			values.rect[2] = static_cast<float>(this->cursorSize.width()) * view.factor;
			values.rect[3] = static_cast<float>(this->cursorSize.height()) * view.factor;
			this->context->UpdateSubresource(this->constants.get(), 0, nullptr, &values, 0, 0);
			auto* cursor = this->cursorView.get();
			this->context->OMSetBlendState(this->blend.get(), nullptr, 0xffffffff);
			this->context->PSSetShader(this->cursorPixel.get(), nullptr, 0);
			this->context->PSSetShaderResources(0, 1, &cursor);
			this->context->Draw(4, 0);
		}
	}

	ID3D11ShaderResourceView* none = nullptr;
	this->context->PSSetShaderResources(0, 1, &none);
	this->context->OMSetRenderTargets(0, nullptr, nullptr);
	this->context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

void RegionCaptureState::refresh() {
	std::lock_guard lock(this->mutex);
	if (!this->running) return;

	const auto view = screenZoomView();
	try {
		for (auto& monitor: this->monitors) {
			if (!monitor->frame) continue;
			if (zoomedOn(monitor->handle, view)) {
				this->renderZoom(monitor.get(), view);
				this->copyPart(monitor.get(), monitor->zoomed.get());
				monitor->wasZoomed = true;
			} else if (monitor->wasZoomed) {
				this->copyPart(monitor.get(), monitor->frame.get());
				monitor->wasZoomed = false;
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logRegionCapture) << "Zoomed frame failed:" << hresultString(e);
	}
}

void RegionCaptureState::onFrame(
    MonitorCapture* monitor,
    const Direct3D11CaptureFramePool& sender
) {
	std::lock_guard lock(this->mutex);
	if (!this->running) return;

	try {
		auto frame = sender.TryGetNextFrame();
		if (!frame) return;

		auto content = frame.ContentSize();
		auto access = frame.Surface().as<DxgiInterfaceAccess>();
		winrt::com_ptr<ID3D11Texture2D> source;
		winrt::check_hresult(
		    access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), source.put_void())
		);

		D3D11_TEXTURE2D_DESC desc {};
		source->GetDesc(&desc);

		monitor->visibleSize = QSize(
		    std::min(content.Width, static_cast<qint32>(desc.Width)),
		    std::min(content.Height, static_cast<qint32>(desc.Height))
		);
		this->keepFrame(monitor, source.get(), desc);

		const auto view = screenZoomView();
		if (zoomedOn(monitor->handle, view)) {
			this->renderZoom(monitor, view);
			this->copyPart(monitor, monitor->zoomed.get());
			monitor->wasZoomed = true;
		} else {
			this->copyPart(monitor, source.get());
			monitor->wasZoomed = false;
		}

		frame.Close();

		if ((content.Width != monitor->poolSize.Width || content.Height != monitor->poolSize.Height)
		    && content.Width > 0 && content.Height > 0)
		{
			sender.Recreate(this->winrtDevice, FRAME_PIXEL_FORMAT, POOL_BUFFERS, content);
			monitor->poolSize = content;
		}
	} catch (const winrt::hresult_error& e) {
		qCDebug(logRegionCapture) << "Frame callback failed:" << hresultString(e);
	}
}

RegionCapture::~RegionCapture() { this->stop(); }

bool RegionCapture::start(ID3D11Device* device, const QRect& region, bool cursor, QString* error) {
	auto state = std::make_shared<RegionCaptureState>();
	state->region = region;
	state->cursor = cursor;
	device->GetImmediateContext(state->context.put());

	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = static_cast<UINT>(region.width());
	desc.Height = static_cast<UINT>(region.height());
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = FRAME_FORMAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;

	std::vector<quint32> black(static_cast<size_t>(region.width()) * region.height(), 0xff000000);
	D3D11_SUBRESOURCE_DATA initial {black.data(), static_cast<UINT>(region.width() * 4), 0};

	auto hr = device->CreateTexture2D(&desc, &initial, state->composite.put());
	if (FAILED(hr)) {
		*error = QString("cannot create the frame texture (0x%1)")
		             .arg(static_cast<quint32>(hr), 8, 16, QChar('0'));
		return false;
	}

	std::vector<HMONITOR> handles;
	auto param = reinterpret_cast<LPARAM>(&handles); // NOLINT
	EnumDisplayMonitors(nullptr, nullptr, collectMonitor, param);

	for (auto* handle: handles) {
		auto rects = monitorRects(handle);
		if (!rects.valid) continue;

		auto crop = rects.monitor.intersected(region);
		if (crop.isEmpty()) continue;

		auto monitor = std::make_unique<MonitorCapture>();
		monitor->handle = handle;
		monitor->origin = rects.monitor.topLeft();
		monitor->crop = crop;
		state->monitors.push_back(std::move(monitor));
	}

	if (state->monitors.empty()) {
		*error = "the region is not on any screen";
		return false;
	}

	auto features = sessionFeatures();

	try {
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
		winrt::com_ptr<::IInspectable> inspectable;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
		state->winrtDevice = inspectable.as<IDirect3DDevice>();

		auto factory =
		    winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

		for (auto& monitor: state->monitors) {
			winrt::check_hresult(factory->CreateForMonitor(
			    monitor->handle,
			    winrt::guid_of<GraphicsCaptureItem>(),
			    winrt::put_abi(monitor->item)
			));

			monitor->poolSize = monitor->item.Size();
			monitor->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
			    state->winrtDevice,
			    FRAME_PIXEL_FORMAT,
			    POOL_BUFFERS,
			    monitor->poolSize
			);
			monitor->session = monitor->pool.CreateCaptureSession(monitor->item);

			if (features.border) {
				try {
					monitor->session.IsBorderRequired(false);
				} catch (const winrt::hresult_error& e) {
					qCInfo(logRegionCapture) << "IsBorderRequired(false) refused:" << hresultString(e);
				}
			}

			if (features.cursor) {
				try {
					monitor->session.IsCursorCaptureEnabled(cursor);
				} catch (const winrt::hresult_error& e) {
					qCWarning(logRegionCapture) << "IsCursorCaptureEnabled failed:" << hresultString(e);
				}
			}

			auto* raw = monitor.get();
			std::weak_ptr<RegionCaptureState> weak = state;

			monitor->frameToken =
			    monitor->pool.FrameArrived([weak, raw](auto const& sender, auto const&) {
				    if (auto s = weak.lock()) s->onFrame(raw, sender);
			    });

			monitor->closedToken = monitor->item.Closed([weak](auto const&, auto const&) {
				if (auto s = weak.lock()) {
					std::lock_guard lock(s->mutex);
					s->lost = true;
				}
			});
		}

		{
			std::lock_guard lock(state->mutex);
			state->running = true;
		}

		for (auto& monitor: state->monitors) monitor->session.StartCapture();
	} catch (const winrt::hresult_error& e) {
		*error = "screen capture failed to start: " + hresultString(e);
		qCWarning(logRegionCapture) << *error;
		this->state = state;
		this->stop();
		return false;
	}

	this->state = state;
	return true;
}

void RegionCapture::stop() {
	if (!this->state) return;
	auto state = std::move(this->state);

	{
		std::lock_guard lock(state->mutex);
		state->running = false;
	}

	for (auto& monitor: state->monitors) {
		try {
			if (monitor->pool) monitor->pool.FrameArrived(monitor->frameToken);
			if (monitor->item) monitor->item.Closed(monitor->closedToken);
			if (monitor->session) monitor->session.Close();
			if (monitor->pool) monitor->pool.Close();
		} catch (const winrt::hresult_error& e) {
			qCWarning(logRegionCapture) << "Stopping capture failed:" << hresultString(e);
		}

		monitor->session = nullptr;
		monitor->pool = nullptr;
		monitor->item = nullptr;
	}
}

void RegionCapture::refresh() {
	if (this->state) this->state->refresh();
}

ID3D11Texture2D* RegionCapture::composite() const {
	return this->state ? this->state->composite.get() : nullptr;
}

bool RegionCapture::primed() const {
	if (!this->state) return false;
	std::lock_guard lock(this->state->mutex);
	return this->state->delivered == static_cast<int>(this->state->monitors.size());
}

bool RegionCapture::lost() const {
	if (!this->state) return true;
	std::lock_guard lock(this->state->mutex);
	return this->state->lost;
}

} // namespace qs::windows::recorder
