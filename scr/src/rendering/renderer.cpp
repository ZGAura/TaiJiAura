#include <desktopball/rendering/renderer.hpp>
#include "shader_bytecode.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace desktopball {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result, const char* context) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << context << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(message.str());
    }
}
HRESULT createHardwareDevice(ID3D11Device** device, ID3D11DeviceContext** context, std::uint64_t selectedLuid = 0) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return DXGI_ERROR_UNSUPPORTED;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const auto luid=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart))<<32)|desc.AdapterLuid.LowPart;
        if (selectedLuid && luid!=selectedLuid) continue;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, device, nullptr, context);
        if (SUCCEEDED(hr)) return hr;
    }
    return DXGI_ERROR_UNSUPPORTED;
}
std::wstring percent(std::optional<double> value) {
    if (!value) { return L"—"; }
    std::wostringstream text;
    text << std::fixed << std::setprecision(0) << *value << L'%';
    return text.str();
}
}
struct Renderer::Impl {
    struct Presentation {
        ComPtr<IDCompositionDevice> device;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual> visual;
        ComPtr<IDXGISwapChain1> content;
        explicit Presentation(HWND window) {
            // The HWND binding outlives CPU/GPU devices; content arrives through DXGI swap chains.
            check(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device)), "DirectComposition device");
            check(device->CreateTargetForHwnd(window, TRUE, &target), "Composition HWND target");
            check(device->CreateVisual(&visual), "Composition visual");
            check(target->SetRoot(visual.Get()), "Visual root");
        }
        void show(IDXGISwapChain1* next) {
            if (content.Get() == next) return;
            try {
                check(visual->SetContent(next), "Visual content");
                check(device->Commit(), "Composition commit");
                check(device->WaitForCommitCompletion(), "Composition handoff");
            } catch (...) {
                visual->SetContent(content.Get()); device->Commit();
                throw;
            }
            content = next;
        }
    };
    std::shared_ptr<Presentation> presentation;
    HWND window{};
    unsigned width{90}, height{40};
    double drawCost{};
    bool software{};
    float expandedPadding{};
    std::uint64_t actualLuid{}, preferredLuid{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11ShaderResourceView> noiseView;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2d;
    ComPtr<ID2D1Bitmap1> bitmap;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> textBrush;
    ComPtr<ID2D1LinearGradientBrush> downloadBrush, uploadBrush;
    ComPtr<ID2D1SolidColorBrush> invisibleArrowBrush;
    ComPtr<ID2D1StrokeStyle> networkArrowStroke;
    ComPtr<ID2D1LinearGradientBrush> textReflection;
    float textTime{};
    float textOpacity{1};
    bool gaugeText{};
    float canvasWidth{90};
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IDWriteTextFormat> labelFormat, valueFormat, smallFormat, networkFormat, gaugeFormat;
    struct NetworkText {
        std::wstring value;
        float width{};
        float baselineOffset{};
        float horizontalScale{1};
        float arrowCenter{}, arrowTop{}, arrowScale{1};
        ComPtr<IDWriteTextLayout1> layout;
    } downloadText, uploadText;
    struct MetricText {
        std::wstring value;
        float width{}, height{};
        IDWriteTextFormat* format{};
        ComPtr<IDWriteTextLayout> layout;
    };
    // A bounded slot per visible label/value, including both sides of a transition.
    std::array<MetricText,maxDisplayMetrics*2+2> metricText;
    size_t textIndex{};
    struct Readout {
        bool ready{};
        DeviceKind kind{};
        std::optional<double> value;
        std::wstring text;
    };
    std::array<Readout,maxDisplayMetrics+1> readouts;
    struct NetworkValue {
        bool ready{};
        std::optional<double> bytes;
        std::wstring text;
    } downloadValue, uploadValue;
    DeviceReading cpuReading{L"cpu",L"CPU 占用率",L"CPU",DeviceKind::cpu};
    DeviceReading memoryReading{L"memory",L"内存占用率",L"RAM",DeviceKind::memory};

    const std::wstring& readout(const DeviceReading* reading, size_t slot) {
        auto& cached=readouts[slot];
        const auto kind=reading ? reading->kind : DeviceKind::cpu;
        const auto value=reading ? (isTemperature(kind) ? reading->celsius : reading->percent) : std::nullopt;
        if (!cached.ready || cached.kind!=kind || cached.value!=value) {
            cached.text=reading && isTemperature(kind) ? temperatureText(*reading) : percent(value);
            cached.value=value; cached.kind=kind; cached.ready=true;
        }
        return cached.text;
    }
    const std::wstring& networkValue(NetworkValue& cached, std::optional<double> bytes, const wchar_t* arrow) {
        if (!cached.ready || cached.bytes!=bytes) {
            std::wostringstream result;
            result << arrow;
            if (!bytes) result << L"—";
            else if (*bytes>=1000000) result << std::fixed << std::setprecision(1) << *bytes/1000000 << L"M";
            else if (*bytes>=1000) result << std::fixed << std::setprecision(0) << *bytes/1000 << L"K";
            else result << std::fixed << std::setprecision(0) << *bytes << L"B";
            result << L"/s";
            cached.text=result.str(); cached.bytes=bytes; cached.ready=true;
        }
        return cached.text;
    }

    explicit Impl(HWND targetWindow, bool useSoftware, std::shared_ptr<Presentation> target = {}, float padding = 0, std::uint64_t selectedLuid = 0)
        : presentation(std::move(target)), window(targetWindow), software(useSoftware), expandedPadding(padding), preferredLuid(selectedLuid) {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = software ? D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context)
            : createHardwareDevice(device.GetAddressOf(), context.GetAddressOf(), selectedLuid);
        if (FAILED(hr) && !software && !selectedLuid) {
            software = true;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        check(hr, "D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi), "DXGI device");
        ComPtr<IDXGIAdapter> actualAdapter;
        DXGI_ADAPTER_DESC actualDescription{};
        check(dxgi->GetAdapter(&actualAdapter), "Selected adapter");
        check(actualAdapter->GetDesc(&actualDescription), "Selected adapter description");
        actualLuid=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(actualDescription.AdapterLuid.HighPart))<<32)|actualDescription.AdapterLuid.LowPart;
        if (window) {
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            check(dxgi->GetAdapter(&adapter), "DXGI adapter");
            check(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory");
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            check(factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &swap), "Composition swap chain");
            if (!presentation) presentation = std::make_shared<Presentation>(window);
        }
        D2D1_FACTORY_OPTIONS factoryOptions{};
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &factoryOptions, reinterpret_cast<void**>(d2dFactory.GetAddressOf())), "D2D factory");
        check(d2dFactory->CreateDevice(dxgi.Get(), &d2dDevice), "D2D device");
        check(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d), "D2D context");
        d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())), "DirectWrite factory");
        auto format = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& output) {
            check(writeFactory->CreateTextFormat(L"Times New Roman", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &output), "Text format");
            output->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        format(5.5f, DWRITE_FONT_WEIGHT_NORMAL, labelFormat);
        format(7.2f, DWRITE_FONT_WEIGHT_NORMAL, valueFormat);
        format(5.0f, DWRITE_FONT_WEIGHT_NORMAL, smallFormat);
        check(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 6.0f, L"zh-CN", &networkFormat), "Network text format");
        networkFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        check(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 5.0f, L"zh-CN", &gaugeFormat), "Collapsed text format");
        gaugeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        gaugeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        check(d2d->CreateSolidColorBrush(D2D1::ColorF(0.89f, 0.98f, 0.98f, 1.0f), &brush), "Text brush");
        check(d2d->CreateSolidColorBrush(D2D1::ColorF(0.0f,0.0f,0.0f,0.0f), &invisibleArrowBrush), "Invisible arrow glyph");
        D2D1_STROKE_STYLE_PROPERTIES arrowStroke{};
        arrowStroke.startCap=arrowStroke.endCap=arrowStroke.dashCap=D2D1_CAP_STYLE_ROUND;
        arrowStroke.lineJoin=D2D1_LINE_JOIN_ROUND;
        check(d2dFactory->CreateStrokeStyle(arrowStroke,nullptr,0,&networkArrowStroke), "Rounded network arrows");
        // Pearl highlights share the cool glass palette without dimming the white core.
        const D2D1_GRADIENT_STOP stops[]{
            {0.0f,D2D1::ColorF(.90f,.97f,1,.95f)},
            {0.65f,D2D1::ColorF(.80f,.92f,.97f,.91f)},
            {1.0f,D2D1::ColorF(.67f,.84f,.91f,.86f)}};
        ComPtr<ID2D1GradientStopCollection> textStops;
        check(d2d->CreateGradientStopCollection(stops,3,&textStops), "Text highlight stops");
        check(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0,0),D2D1::Point2F(0,1)),textStops.Get(),&textBrush), "Glass text brush");
        auto networkBrush = [&](UINT32 color, ComPtr<ID2D1LinearGradientBrush>& output) {
            const auto tint = D2D1::ColorF(color,1.0f);
            const D2D1_GRADIENT_STOP stops[]{
                {0,tint},
                {.65f,tint},
                {1,D2D1::ColorF(tint.r*.9f,tint.g*.9f,tint.b*.9f,1.0f)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            check(d2d->CreateGradientStopCollection(stops,3,&collection), "Network tint stops");
            check(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0,0),D2D1::Point2F(0,1)),collection.Get(),&output), "Network text brush");
        };
        networkBrush(0xFF813D,downloadBrush);
        networkBrush(0xFFA329,uploadBrush);
        const D2D1_GRADIENT_STOP reflectionStops[]{
            {0,D2D1::ColorF(1,1,1,0)},
            {.5f,D2D1::ColorF(1,1,1,.16f)},
            {1,D2D1::ColorF(1,1,1,0)}};
        ComPtr<ID2D1GradientStopCollection> reflectionCollection;
        check(d2d->CreateGradientStopCollection(reflectionStops,3,&reflectionCollection), "Text reflection stops");
        check(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0,0),D2D1::Point2F(1,0)),reflectionCollection.Get(),&textReflection), "Text reflection brush");
        // Identical compiler flags, but no runtime compiler DLL or retained compiler allocations.
        check(device->CreateVertexShader(capsuleVertexShader, sizeof(capsuleVertexShader), nullptr, &vertex), "Vertex shader");
        check(device->CreatePixelShader(capsulePixelShader, sizeof(capsulePixelShader), nullptr, &pixel), "Pixel shader");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 48; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants), "Frame constants");
        // Match the reference's seeded noise lattice.
        std::array<float, 128 * 128> noise{};
        std::uint32_t seed = 918271;
        for (auto& value : noise) {
            seed = seed * 1664525u + 1013904223u;
            value = static_cast<float>(static_cast<double>(seed) / 4294967296.0);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 128;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{noise.data(), 128 * sizeof(float), 0};
        ComPtr<ID3D11Texture2D> noiseTexture;
        check(device->CreateTexture2D(&desc, &initial, &noiseTexture), "Noise texture");
        check(device->CreateShaderResourceView(noiseTexture.Get(), nullptr, &noiseView), "Noise view");
        resize(width, height);
    }
    void resize(unsigned w, unsigned h) {
        if (!w || !h || (texture && w==width && h==height)) { return; }
        d2d->SetTarget(nullptr); bitmap.Reset(); renderTarget.Reset(); texture.Reset();
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->Flush();
        width = w; height = h;
        if (swap) {
            check(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
            check(swap->GetBuffer(0, IID_PPV_ARGS(&texture)), "Swap buffer");
        } else {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(&desc, nullptr, &texture), "Offscreen texture");
        }
        check(device->CreateRenderTargetView(texture.Get(), nullptr, &renderTarget), "Render target");
        ComPtr<IDXGISurface> surface;
        check(texture.As(&surface), "DXGI surface");
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        check(d2d->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap), "D2D bitmap");
        d2d->SetTarget(bitmap.Get());
    }
    void prepareNetworkText(const std::wstring& down, const std::wstring& up, float downWidth, float upWidth) {
        constexpr float layoutArrowScale = 1.775f;
        constexpr float arrowScale = layoutArrowScale-.2f;
        constexpr float halfSpacing = .08f;
        auto* format = networkFormat.Get();
        const auto prepare = [&](NetworkText& cached, const std::wstring& value, float w) {
            if (!cached.layout || cached.value != value || cached.width != w) {
                ComPtr<IDWriteTextLayout> layout;
                check(writeFactory->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),
                    format,w,16,&layout), "Network text layout");
                check(layout.As(&cached.layout), "Network character spacing");
                const DWRITE_TEXT_RANGE range{0,static_cast<UINT32>(value.size())};
                const DWRITE_TEXT_RANGE arrowRange{0,value.empty() ? 0u : 1u};
                // Retain the previous width budget so smaller arrows do not enlarge the digits.
                check(cached.layout->SetFontSize(format->GetFontSize()*layoutArrowScale,arrowRange), "Network arrow width budget");
                // Keep the original advance and text sizing, but draw the icon as native lines.
                check(cached.layout->SetDrawingEffect(invisibleArrowBrush.Get(),arrowRange), "Hide network arrow glyph");
                check(cached.layout->SetCharacterSpacing(halfSpacing,halfSpacing,0,range), "Network character spacing");
                cached.value = value; cached.width = w;
                return true;
            }
            return false;
        };
        const bool downChanged = prepare(downloadText,down,downWidth);
        const bool upChanged = prepare(uploadText,up,upWidth);
        if (downChanged || upChanged) {
            // Point size and icon size never depend on live telemetry or item count.
            const float fontSize = format->GetFontSize();
            for (auto* entry : {&downloadText,&uploadText}) {
                auto& cached = *entry;
                const DWRITE_TEXT_RANGE range{0,static_cast<UINT32>(cached.value.size())};
                const DWRITE_TEXT_RANGE arrowRange{0,cached.value.empty() ? 0u : 1u};
                check(cached.layout->SetFontSize(5.5f,range), "Shared network text size");
                check(cached.layout->SetFontSize(fontSize*arrowScale,arrowRange), "Shared network arrow size");
                // Enlarge the arrows without moving the digits' existing baseline.
                DWRITE_LINE_METRICS enlargedLine{}, previousLine{};
                UINT32 lineCount{};
                check(cached.layout->GetLineMetrics(&enlargedLine,1,&lineCount), "Network baseline");
                check(cached.layout->SetFontSize(fontSize*1.25f,arrowRange), "Previous arrow baseline");
                check(cached.layout->GetLineMetrics(&previousLine,1,&lineCount), "Previous network baseline");
                check(cached.layout->SetFontSize(fontSize*arrowScale,arrowRange), "Restore network arrow size");
                cached.baselineOffset = previousLine.baseline-enlargedLine.baseline;
                DWRITE_TEXT_METRICS metrics{};
                check(cached.layout->GetMetrics(&metrics), "Fixed-size network metrics");
                // Preserve all digits of unusually long speeds without changing their height.
                cached.horizontalScale=std::min(1.0f,cached.width/std::max(.1f,metrics.widthIncludingTrailingWhitespace));
                FLOAT glyphX{}, glyphY{};
                DWRITE_HIT_TEST_METRICS glyph{};
                check(cached.layout->HitTestTextPosition(0,FALSE,&glyphX,&glyphY,&glyph), "Network icon position");
                cached.arrowCenter=glyphX+glyph.width*.5f;
                cached.arrowScale=fontSize/format->GetFontSize();
                cached.arrowTop=previousLine.baseline-fontSize;
            }
        }
    }
    void text(const std::wstring& value, IDWriteTextFormat* format, float x, float y, float w, float h,
        ID2D1LinearGradientBrush* tint = nullptr) {
        if (tint) {
            auto& cached = tint == downloadBrush.Get() ? downloadText : uploadText;
            tint->SetStartPoint(D2D1::Point2F(x,y));
            tint->SetEndPoint(D2D1::Point2F(x,y+format->GetFontSize()));
            tint->SetOpacity(textOpacity);
            // A single regular-weight pass keeps tiny digits crisp, without a thick shadow.
            D2D1_MATRIX_3X2_F originalTransform{};
            d2d->GetTransform(&originalTransform);
            d2d->SetTransform(D2D1::Matrix3x2F::Scale(cached.horizontalScale,1,D2D1::Point2F(x,y))*originalTransform);
            d2d->DrawTextLayout(D2D1::Point2F(x,y+cached.baselineOffset),cached.layout.Get(),tint,D2D1_DRAW_TEXT_OPTIONS_NONE);
            d2d->SetTransform(originalTransform);
            const float scale=cached.arrowScale*(3.34f/3.2f), center=x+cached.arrowCenter*cached.horizontalScale;
            const float middle=y+cached.arrowTop+3.3f*cached.arrowScale;
            const float top=middle-1.6f*scale, bottom=middle+1.6f*scale;
            const bool download=tint==downloadBrush.Get();
            const float tip=download ? bottom : top;
            const float wing=download ? top : bottom;
            const auto line=[&](D2D1_POINT_2F a,D2D1_POINT_2F b) {
                d2d->DrawLine(a,b,tint,.55f*scale,networkArrowStroke.Get());
            };
            line(D2D1::Point2F(center-1.25f*scale,wing),D2D1::Point2F(center,tip));
            line(D2D1::Point2F(center,tip),D2D1::Point2F(center+1.25f*scale,wing));
            return;
        }
        auto& cached=metricText[textIndex++];
        if (!cached.layout || cached.value!=value || cached.format!=format || cached.width!=w || cached.height!=h) {
            check(writeFactory->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),
                format,w,h,cached.layout.ReleaseAndGetAddressOf()), "Metric text layout");
            cached.value=value; cached.format=format; cached.width=w; cached.height=h;
        }
        // A single restrained recess shadow replaces the detached four-sided outline.
        brush->SetColor(D2D1::ColorF(0.04f, 0.15f, 0.20f, 0.20f*textOpacity));
        D2D1_MATRIX_3X2_F transform{}; d2d->GetTransform(&transform);
        const float dy = 0.25f / transform._22;
        d2d->DrawTextLayout(D2D1::Point2F(x,y+dy),cached.layout.Get(),brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        auto* fill = textBrush.Get();
        fill->SetStartPoint(D2D1::Point2F(x,y));
        fill->SetEndPoint(D2D1::Point2F(x,y+format->GetFontSize()));
        fill->SetOpacity(textOpacity*(format == valueFormat.Get() || format == gaugeFormat.Get() ? 1.0f :
            (format == labelFormat.Get() ? .84f : .78f)));
        d2d->DrawTextLayout(D2D1::Point2F(x,y),cached.layout.Get(),fill,D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!gaugeText) {
            textReflection->SetOpacity(textOpacity);
            d2d->DrawTextLayout(D2D1::Point2F(x,y),cached.layout.Get(),textReflection.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    bool draw(float seconds, const Metrics& data, const DisplaySettings& settings, float collapse) {
        textTime = seconds;
        textIndex=0;
        const float blend=std::clamp(collapse,0.0f,1.0f);
        const auto drawStart = std::chrono::steady_clock::now();
        cpuReading.percent=data.cpu; memoryReading.percent=data.memoryPercent;
        const auto find=[&](const std::wstring& id) -> const DeviceReading* {
            if (data.cpuPresent && id==L"cpu") return &cpuReading;
            if (data.memoryPresent && id==L"memory") return &memoryReading;
            return findReading(data.accelerators,id);
        };
        std::array<const DeviceReading*,maxDisplayMetrics> selected{};
        size_t selectedCount=0;
        for (const auto& id : settings.selected)
            if (const auto* reading = find(id); reading && selectedCount < maxDisplayMetrics) selected[selectedCount++]=reading;
        const auto layout = capsuleLayout(selectedCount);
        canvasWidth = static_cast<float>(layout.width);
        const auto* gaugeReading = find(settings.gauge);
        // All text passes share this reflection; only opacity varies by transition side.
        const float center = (3 + (canvasWidth*6-21) * (.4f + .23f * std::sin(textTime * .55f)) + 43.75f) / 6;
        constexpr float slope=70.0f/192;
        constexpr float halfWidth=10.0f/6/(1+slope*slope);
        textReflection->SetStartPoint(D2D1::Point2F(center-halfWidth,-halfWidth*slope));
        textReflection->SetEndPoint(D2D1::Point2F(center+halfWidth,halfWidth*slope));
        // Celsius and utilization share the 0..100 smoke scale. Missing data
        // remains distinct from a valid zero; text retains its original unit.
        const auto gaugeValue=gaugeReading
            ? (isTemperature(gaugeReading->kind) ? gaugeReading->celsius : gaugeReading->percent) : std::nullopt;
        const float gaugeUsage=gaugeValue && std::isfinite(*gaugeValue)
            ? static_cast<float>(std::clamp(*gaugeValue/100.0,0.0,1.0)) : -1.0f;
        const float expandedScale=std::min(static_cast<float>(width)/(canvasWidth+2*expandedPadding),
            static_cast<float>(height)/(40+2*expandedPadding));
        const std::array<float, 12> frame{static_cast<float>(width), static_cast<float>(height), seconds,
            static_cast<float>(data.cpu.value_or(0)), static_cast<float>(data.memoryPercent.value_or(0)), blend,
            gaugeUsage, canvasWidth*6,
            settings.showOutline ? 1.0f : 0.0f, expandedPadding, expandedScale, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, frame.data(), 0, 0);
        // The full-screen triangle writes every pixel, including transparent regions.
        ID3D11RenderTargetView* view = renderTarget.Get();
        context->OMSetRenderTargets(1, &view, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffer = constants.Get();
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11ShaderResourceView* noiseResource = noiseView.Get();
        context->PSSetShaderResources(0, 1, &noiseResource);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        d2d->BeginDraw();
        if (blend>0) {
            gaugeText=true; textOpacity=blend;
            const float scale=std::min(static_cast<float>(width)/14,static_cast<float>(height)/100);
            d2d->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*
                D2D1::Matrix3x2F::Translation((width-14*scale)*.5f,(height-100*scale)*.5f));
            const std::wstring& label = gaugeReading ? gaugeReading->label : readout(nullptr,0);
            text(label, gaugeFormat.Get(), 0, 3, 14, 9);
            text(readout(gaugeReading,0), gaugeFormat.Get(), 0, 89, 14, 9);
        }
        if (blend<1) {
            gaugeText=false; textOpacity=1-blend;
            const float scale=expandedScale;
            d2d->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*
                D2D1::Matrix3x2F::Translation((width-canvasWidth*scale)*.5f,(height-40*scale)*.5f));
            const float left = layout.first, cell = layout.columnWidth;
            for (size_t i = 0; i < selectedCount; ++i) {
                // Reserve four logical pixels between the last information column and the glass edge.
                const float x = i+1==selectedCount ? capsuleLastColumnX(layout)
                    : left + static_cast<float>(i) * layout.step;
                text(selected[i]->label, labelFormat.Get(), x, 8, cell, 9);
                text(readout(selected[i],i+1),valueFormat.Get(), x, 17, cell, 12);
            }
            const auto& down=networkValue(downloadValue,data.network ? std::optional<double>{data.network->download} : std::nullopt,L"↓");
            const auto& up=networkValue(uploadValue,data.network ? std::optional<double>{data.network->upload} : std::nullopt,L"↑");
            // Anchor network readouts below the first and last hardware columns.
            const float downloadX=left-2.0f, uploadX=capsuleUploadX(layout);
            const float downloadWidth=uploadX-downloadX-1.0f;
            prepareNetworkText(down,up,downloadWidth,cell);
            text(down, networkFormat.Get(), downloadX, 27, downloadWidth, 16, downloadBrush.Get());
            text(up, networkFormat.Get(), uploadX, 27, cell, 16, uploadBrush.Get());
        }
        textOpacity=1;
        const HRESULT end = d2d->EndDraw();
        if (end == D2DERR_RECREATE_TARGET) { return false; }
        check(end, "D2D EndDraw");
        // Exclude the Present/vblank wait from measured CPU submission cost.
        drawCost = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-drawStart).count();
        if (swap) {
            // The waitable frame clock paces submissions; an additional vblank wait
            // here can make a 60 FPS timer miss every other refresh.
            const HRESULT present = swap->Present(0, 0);
            if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) { return false; }
            check(present, "Present");
            presentation->show(swap.Get());
        }
        return true;
    }
};
Renderer::Renderer(HWND window, bool software, float expandedPadding, std::uint64_t adapterLuid)
    : impl_(std::make_unique<Impl>(window, software, nullptr, expandedPadding, adapterLuid)) {}
Renderer::~Renderer() = default;
bool Renderer::software() const { return impl_->software; }
std::uint64_t Renderer::adapterLuid() const { return impl_->actualLuid; }
void Renderer::setSoftware(bool software, float seconds, const Metrics& metrics,
    const DisplaySettings& settings, float collapse) {
    auto replacement = std::make_unique<Impl>(impl_->window, software, impl_->presentation, impl_->expandedPadding, impl_->preferredLuid);
    replacement->resize(impl_->width, impl_->height);
    if (!replacement->draw(seconds, metrics, settings, collapse))
        throw std::runtime_error("Replacement renderer could not present its first frame");
    impl_.swap(replacement); // The previous device stays alive throughout the composition handoff.
}
void Renderer::setHardware(std::uint64_t adapterLuid, float seconds, const Metrics& metrics,
    const DisplaySettings& settings, float collapse) {
    auto replacement=std::make_unique<Impl>(impl_->window,false,impl_->presentation,impl_->expandedPadding,adapterLuid);
    replacement->resize(impl_->width,impl_->height);
    if (!replacement->draw(seconds,metrics,settings,collapse))
        throw std::runtime_error("Selected GPU could not present its first frame");
    impl_.swap(replacement);
}
bool Renderer::hardwareAvailable(std::uint64_t adapterLuid) {
    ComPtr<ID3D11Device> device;
    return SUCCEEDED(createHardwareDevice(device.GetAddressOf(), nullptr, adapterLuid));
}
double Renderer::drawMilliseconds() const { return impl_->drawCost; }
void Renderer::resize(unsigned width, unsigned height) { impl_->resize(width, height); }
bool Renderer::draw(float seconds, const Metrics& metrics, const DisplaySettings& settings, float collapse) {
    return impl_->draw(seconds, metrics, settings, collapse);
}
std::vector<std::uint32_t> Renderer::readPixels() {
    D3D11_TEXTURE2D_DESC desc{};
    impl_->texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(impl_->device->CreateTexture2D(&desc, nullptr, &staging), "Readback texture");
    impl_->context->CopyResource(staging.Get(), impl_->texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(impl_->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Readback map");
    std::vector<std::uint32_t> pixels(static_cast<size_t>(impl_->width) * impl_->height);
    for (unsigned y = 0; y < impl_->height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * impl_->width,
            static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
            static_cast<size_t>(impl_->width) * sizeof(std::uint32_t));
    }
    impl_->context->Unmap(staging.Get(), 0);
    return pixels;
}
}








