// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "HwndTerminal.hpp"

#include <DefaultSettings.h>
#include "../inc/ControlProperties.h"
#include "../../types/inc/utils.hpp"
#include "../../types/inc/colorTable.hpp"
#include <windowsx.h>

#include "HwndTerminalAutomationPeer.hpp"
#include "../../cascadia/TerminalCore/Terminal.hpp"
#include "../../renderer/atlas/AtlasEngine.h"
#include "../../renderer/base/renderer.hpp"
#include "../../renderer/uia/UiaRenderer.hpp"
#include "../../types/viewport.cpp"

#include <unordered_set>

using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;

namespace
{
    class HwndTerminalSettings : public winrt::implements<
                                     HwndTerminalSettings,
                                     winrt::Microsoft::Terminal::Core::ICoreSettings,
                                     winrt::Microsoft::Terminal::Core::ICoreAppearance>
    {
        std::array<winrt::Microsoft::Terminal::Core::Color, COLOR_TABLE_SIZE> _ColorTable{};

    public:
#define SETTINGS_GEN(type, name, ...) til::property<type> name{ __VA_ARGS__ };
        CORE_SETTINGS(SETTINGS_GEN)
        CORE_APPEARANCE_SETTINGS(SETTINGS_GEN)
#undef SETTINGS_GEN

        explicit HwndTerminalSettings(const HwndTerminalOptions& options) :
            HistorySize{ options.HistorySize },
            InitialRows{ options.InitialSize.height },
            InitialCols{ options.InitialSize.width },
            SnapOnInput{ options.SnapOnInput },
            WordDelimiters{ options.WordDelimiters },
            CopyOnSelect{ options.CopyOnSelect },
            DetectURLs{ options.DetectUrls },
            AllowVtClipboardWrite{ options.AllowOscClipboard },
            DefaultForeground{ til::color{ options.Theme.DefaultForeground } },
            DefaultBackground{ til::color{ options.Theme.DefaultBackground } },
            CursorColor{ til::color{ options.CursorColor } },
            CursorShape{ static_cast<winrt::Microsoft::Terminal::Core::CursorStyle>(options.Theme.CursorStyle) },
            SelectionBackground{ til::color{ options.Theme.DefaultSelectionBackground } }
        {
            for (size_t index = 0; index < _ColorTable.size(); ++index)
            {
                _ColorTable[index] = til::color{ options.Theme.ColorTable[index] };
            }
        }

        winrt::Microsoft::Terminal::Core::Color GetColorTableEntry(const int32_t index) const
        {
            return _ColorTable.at(index);
        }

        void SetColorTableEntry(const int32_t index, const winrt::Microsoft::Terminal::Core::Color color)
        {
            _ColorTable.at(index) = color;
        }
    };
}

static LPCWSTR term_window_class = L"HwndTerminalClass";
static constexpr UINT WmReseshDispatchEvents = WM_APP + 0x51;
static constexpr uint32_t Osc8LinkSource = 1;
static constexpr uint32_t DetectedUrlLinkSource = 2;

STDMETHODIMP HwndTerminal::TsfDataProvider::QueryInterface(REFIID, void**) noexcept
{
    return E_NOTIMPL;
}

ULONG STDMETHODCALLTYPE HwndTerminal::TsfDataProvider::AddRef() noexcept
{
    return 1;
}

ULONG STDMETHODCALLTYPE HwndTerminal::TsfDataProvider::Release() noexcept
{
    return 1;
}

HWND HwndTerminal::TsfDataProvider::GetHwnd()
{
    return _terminal->GetHwnd();
}

RECT HwndTerminal::TsfDataProvider::GetViewport()
{
    const auto hwnd = GetHwnd();

    RECT rc;
    GetClientRect(hwnd, &rc);

    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclientrect
    // > The left and top members are zero. The right and bottom members contain the width and height of the window.
    // --> We can turn the client rect into a screen-relative rect by adding the left/top position.
    ClientToScreen(hwnd, reinterpret_cast<POINT*>(&rc));
    rc.right += rc.left;
    rc.bottom += rc.top;

    return rc;
}

RECT HwndTerminal::TsfDataProvider::GetCursorPosition()
{
    // Convert from columns/rows to pixels.
    til::point cursorPos;
    til::size fontSize;
    {
        const auto lock = _terminal->_terminal->LockForReading();
        cursorPos = _terminal->_terminal->GetCursorPosition(); // measured in terminal cells
        fontSize = _terminal->_actualFont.GetSize(); // measured in pixels, not DIP
    }
    POINT ptSuggestion = {
        .x = cursorPos.x * fontSize.width,
        .y = cursorPos.y * fontSize.height,
    };

    ClientToScreen(GetHwnd(), &ptSuggestion);

    // Final measurement should be in pixels
    return {
        .left = ptSuggestion.x,
        .top = ptSuggestion.y,
        .right = ptSuggestion.x + fontSize.width,
        .bottom = ptSuggestion.y + fontSize.height,
    };
}

void HwndTerminal::TsfDataProvider::HandleOutput(std::wstring_view text)
{
    _terminal->_WriteTextToConnection(text);
}

Microsoft::Console::Render::Renderer* HwndTerminal::TsfDataProvider::GetRenderer()
{
    return _terminal->_renderer.get();
}

// This magic flag is "documented" at https://msdn.microsoft.com/en-us/library/windows/desktop/ms646301(v=vs.85).aspx
// "If the high-order bit is 1, the key is down; otherwise, it is up."
static constexpr short KeyPressed{ gsl::narrow_cast<short>(0x8000) };

static constexpr bool _IsMouseMessage(UINT uMsg)
{
    return uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK ||
           uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP || uMsg == WM_MBUTTONDBLCLK ||
           uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_RBUTTONDBLCLK ||
           uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL;
}

LRESULT CALLBACK HwndTerminal::HwndTerminalWndProc(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam) noexcept
try
{
    if (WM_NCCREATE == uMsg)
    {
#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        HwndTerminal* that = static_cast<HwndTerminal*>(cs->lpCreateParams);
        that->_hwnd = wil::unique_hwnd(hwnd);

#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
        return DefWindowProc(hwnd, WM_NCCREATE, wParam, lParam);
    }
#pragma warning(suppress : 26490) // Win32 APIs can only store void*, have to use reinterpret_cast
    const auto publicTerminal = reinterpret_cast<HwndTerminal*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    if (publicTerminal)
    {
        if (_IsMouseMessage(uMsg))
        {
            const auto shiftPressed = GetKeyState(VK_SHIFT) < 0;
            if (!shiftPressed && publicTerminal->_CanSendVTMouseInput() && publicTerminal->_SendMouseEvent(uMsg, wParam, lParam))
            {
                // GH#6401: Capturing the mouse ensures that we get drag/release events
                // even if the user moves outside the window.
                // _SendMouseEvent returns false if the terminal's not in VT mode, so we'll
                // fall through to release the capture.
                switch (uMsg)
                {
                case WM_LBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_RBUTTONDOWN:
                    SetCapture(hwnd);
                    break;
                case WM_LBUTTONUP:
                case WM_MBUTTONUP:
                case WM_RBUTTONUP:
                    ReleaseCapture();
                    break;
                default:
                    break;
                }

                // Suppress all mouse events that made it into the terminal.
                return 0;
            }
        }

        switch (uMsg)
        {
        case WM_GETOBJECT:
            if (lParam == UiaRootObjectId)
            {
                return UiaReturnRawElementProvider(hwnd, wParam, lParam, publicTerminal->_GetUiaProvider());
            }
            break;
        case WM_LBUTTONDOWN:
            publicTerminal->_UpdateHoveredLink(lParam);
            if (GetKeyState(VK_SHIFT) >= 0)
            {
                if (auto link = publicTerminal->_LinkAt(lParam))
                {
                    publicTerminal->_pressedLink = std::move(link);
                    SetCapture(hwnd);
                    return 0;
                }
            }
            LOG_IF_FAILED(publicTerminal->_StartSelection(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (publicTerminal->_pressedLink)
            {
                const auto pressed = std::exchange(publicTerminal->_pressedLink, std::nullopt);
                ReleaseCapture();
                if (const auto released = publicTerminal->_LinkAt(lParam);
                    released && released == pressed && publicTerminal->_openLinkCallback)
                {
                    publicTerminal->_openLinkCallback(released->first, released->second);
                }
                return 0;
            }
            if (publicTerminal->_copyOnSelect)
            {
                publicTerminal->CopySelection(false);
            }
            publicTerminal->_singleClickTouchdownPos = std::nullopt;
            [[fallthrough]];
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            ReleaseCapture();
            break;
        case WM_MOUSEMOVE:
            if (WI_IsFlagSet(wParam, MK_LBUTTON))
            {
                LOG_IF_FAILED(publicTerminal->_MoveSelection(lParam));
                return 0;
            }
            publicTerminal->_UpdateHoveredLink(lParam);
            return 0;
        case WM_MOUSELEAVE:
            publicTerminal->_ClearHoveredLink();
            return 0;
        case WM_SETCURSOR:
            if (publicTerminal->_hoveredHyperlinkId != 0 || publicTerminal->_hoveredHyperlinkInterval)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_RBUTTONDOWN:
        {
            const auto copied = !publicTerminal->_copyOnSelect && publicTerminal->CopySelection(true);
            if (publicTerminal->_copyOnSelect)
            {
                publicTerminal->_ClearSelection();
            }
            if (publicTerminal->_rightClickPaste && !publicTerminal->_readOnly &&
                (publicTerminal->_copyOnSelect || !copied) && publicTerminal->_pasteRequestCallback)
            {
                publicTerminal->_pasteRequestCallback();
            }
            return 0;
        }
        case WmReseshDispatchEvents:
            if (publicTerminal->_eventDispatchCallback)
            {
                publicTerminal->_eventDispatchCallback();
            }
            return 0;
        case WM_DESTROY:
            // Release Terminal's hwnd so Teardown doesn't try to destroy it again
            publicTerminal->_hwnd.release();
            publicTerminal->Teardown();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 0;
}

static bool RegisterTermClass(HINSTANCE hInstance) noexcept
{
    WNDCLASSW wc;
    if (GetClassInfoW(hInstance, term_window_class, &wc))
    {
        return true;
    }

    wc.style = 0;
    wc.lpfnWndProc = HwndTerminal::HwndTerminalWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = term_window_class;

    return RegisterClassW(&wc) != 0;
}

HwndTerminal::HwndTerminal(HWND parentHwnd, const HwndTerminalOptions& options) noexcept :
    _desiredFont{ options.FontFamily, 0, options.FontWeight, static_cast<float>(options.FontSize), CP_UTF8 },
    _actualFont{ options.FontFamily, 0, options.FontWeight, { 0, gsl::narrow_cast<til::CoordType>(options.FontSize) }, CP_UTF8, false },
    _currentDpi{ USER_DEFAULT_SCREEN_DPI },
    _copyOnSelect{ options.CopyOnSelect },
    _rightClickPaste{ options.RightClickPaste },
    _readOnly{ options.ReadOnly },
    _copyFormatting{ options.CopyFormatting },
    _pasteFiltering{ options.PasteFiltering },
    _multiClickTime{ 500 } // this will be overwritten by the windows system double-click time
{
    _desiredFont.SetEnableBuiltinGlyphs(options.EnableBuiltinGlyphs);
    _desiredFont.SetEnableColorGlyphs(options.EnableColorGlyphs);
    auto hInstance = wil::GetModuleInstanceHandle();

    if (RegisterTermClass(hInstance))
    {
        CreateWindowExW(
            0,
            term_window_class,
            nullptr,
            WS_CHILD |
                WS_CLIPCHILDREN |
                WS_CLIPSIBLINGS |
                WS_VISIBLE,
            0,
            0,
            0,
            0,
            parentHwnd,
            nullptr,
            hInstance,
            this);
    }
}

HwndTerminal::~HwndTerminal()
{
    Teardown();
}

HRESULT HwndTerminal::Initialize(const HwndTerminalOptions& options)
{
    _terminal = std::make_unique<::Microsoft::Terminal::Core::Terminal>();
    const auto lock = _terminal->LockForWriting();

    auto& renderSettings = _terminal->GetRenderSettings();
    renderSettings.SetColorTableEntry(TextColor::DEFAULT_BACKGROUND, RGB(12, 12, 12));
    renderSettings.SetColorTableEntry(TextColor::DEFAULT_FOREGROUND, RGB(204, 204, 204));
    _renderer = std::make_unique<::Microsoft::Console::Render::Renderer>(renderSettings, _terminal.get());

    auto engine = std::make_unique<::Microsoft::Console::Render::AtlasEngine>();
    RETURN_IF_FAILED(engine->SetHwnd(_hwnd.get()));
    _renderer->AddRenderEngine(engine.get());

    _UpdateFont(USER_DEFAULT_SCREEN_DPI);
    RECT windowRect;
    GetWindowRect(_hwnd.get(), &windowRect);

    const til::size windowSize{ windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };

    // Fist set up the dx engine with the window size in pixels.
    // Then, using the font, get the number of characters that can fit.
    const auto viewInPixels = Viewport::FromDimensions({ 0, 0 }, windowSize);
    RETURN_IF_FAILED(engine->SetWindowSize({ viewInPixels.Width(), viewInPixels.Height() }));

    _renderEngine = std::move(engine);

    const auto settings = winrt::make<HwndTerminalSettings>(options);
    _terminal->CreateFromSettings(settings, *_renderer);
    _terminal->SetWriteInputCallback([=](std::wstring_view input) noexcept { _WriteTextToConnection(input); });
    _terminal->SetCopyToClipboardCallback([=](const wil::zwstring_view text) noexcept {
        if (_clipboardCallback)
        {
            _clipboardCallback(text, {}, {});
        }
    });
    _renderer->EnablePainting();

    _multiClickTime = std::chrono::milliseconds{ GetDoubleClickTime() };

    return S_OK;
}

void HwndTerminal::Teardown() noexcept
try
{
    // As a rule, detach resources from the Terminal before shutting them down.
    // This ensures that teardown is reentrant.
    _tsfHandle = {};

    // Shut down the renderer (and therefore the thread) before we implode
    _renderer.reset();
    _renderEngine.reset();

    if (_terminal)
    {
        // These callbacks have a dangling reference to `this`; clear them before destruction.
        _terminal->SetWriteInputCallback(nullptr);
        _terminal->SetCopyToClipboardCallback(nullptr);
        _terminal->SetWarningBellCallback(nullptr);
        _terminal->SetTitleChangedCallback(nullptr);
        _terminal->SetScrollPositionChangedCallback(nullptr);
        _terminal->SetWorkingDirectoryChangedCallback(nullptr);
        _terminal->SetAlternateBufferChangedCallback(nullptr);
        _terminal->SetShellIntegrationMarkCallback(nullptr);
        _terminal->SetSystemModeChangedCallback(nullptr);
        _terminal->SetOscDispatchCallback(nullptr);
    }
    _clipboardCallback = {};
    _pasteRequestCallback = {};
    _eventDispatchCallback = {};
    _openLinkCallback = {};

    if (auto localHwnd{ _hwnd.release() })
    {
        // If we're being called through WM_DESTROY, we won't get here (hwnd is already released)
        // If we're not, we may end up in Teardown _again_... but by the time we do, all other
        // resources have been released and will not be released again.
        DestroyWindow(localHwnd);
    }
}
CATCH_LOG();

void HwndTerminal::RegisterScrollCallback(std::function<void(int, int, int)> callback)
{
    if (!_terminal)
    {
        return;
    }
    _terminal->SetScrollPositionChangedCallback(callback);
}
void HwndTerminal::RegisterTitleChangedCallback(std::function<void(std::wstring_view)> callback)
{
    if (_terminal)
    {
        _terminal->SetTitleChangedCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterWorkingDirectoryChangedCallback(std::function<void(std::wstring_view)> callback)
{
    if (_terminal)
    {
        _terminal->SetWorkingDirectoryChangedCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterBellCallback(std::function<void()> callback)
{
    if (_terminal)
    {
        _terminal->SetWarningBellCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterBufferChangedCallback(std::function<void(int, int, int)> callback)
{
    RegisterScrollCallback(std::move(callback));
}

void HwndTerminal::RegisterAlternateBufferChangedCallback(std::function<void(bool)> callback)
{
    if (_terminal)
    {
        _terminal->SetAlternateBufferChangedCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterShellIntegrationMarkCallback(std::function<void(std::wstring_view)> callback)
{
    if (_terminal)
    {
        _terminal->SetShellIntegrationMarkCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterSystemModeChangedCallback(std::function<void(size_t, bool)> callback)
{
    if (_terminal)
    {
        _terminal->SetSystemModeChangedCallback(
            [callback = std::move(callback)](const ITerminalApi::Mode mode, const bool enabled) {
                callback(static_cast<size_t>(mode), enabled);
            });
    }
}

void HwndTerminal::RegisterOscDispatchCallback(std::function<void(size_t, std::wstring_view)> callback)
{
    if (_terminal)
    {
        _terminal->SetOscDispatchCallback(std::move(callback));
    }
}

void HwndTerminal::RegisterOpenLinkCallback(std::function<void(std::wstring_view, uint32_t)> callback)
{
    _openLinkCallback = std::move(callback);
}


void HwndTerminal::_WriteTextToConnection(const std::wstring_view input) noexcept
{
    if (input.empty() || !_pfnWriteCallback)
    {
        return;
    }

    try
    {
        _pfnWriteCallback(input);
    }
    CATCH_LOG();
}

void HwndTerminal::RegisterWriteCallback(std::function<void(std::wstring_view)> callback)
{
    _pfnWriteCallback = std::move(callback);
}

void HwndTerminal::RegisterWriteCallback(const void _stdcall callback(wchar_t*))
{
    if (!callback)
    {
        _pfnWriteCallback = {};
        return;
    }

    RegisterWriteCallback([callback](const std::wstring_view input) {
        auto callingText{ wil::make_cotaskmem_string(input.data(), input.size()) };
        callback(callingText.release());
    });
}

void HwndTerminal::RegisterClipboardCallback(std::function<void(std::wstring_view, std::string_view, std::string_view)> callback)
{
    _clipboardCallback = std::move(callback);
}

void HwndTerminal::RegisterPasteRequestCallback(std::function<void()> callback)
{
    _pasteRequestCallback = std::move(callback);
}

void HwndTerminal::RegisterEventDispatchCallback(std::function<void()> callback)
{
    _eventDispatchCallback = std::move(callback);
}

void HwndTerminal::RequestEventDispatch() const noexcept
{
    if (_hwnd)
    {
        LOG_LAST_ERROR_IF(!PostMessageW(_hwnd.get(), WmReseshDispatchEvents, 0, 0));
    }
}

void HwndTerminal::ApplyInteractionOptions(
    const uint32_t flags,
    const uint32_t copyFormatting,
    const uint32_t pasteFiltering) noexcept
{
    _copyOnSelect = WI_IsFlagSet(flags, 0x8u);
    _rightClickPaste = WI_IsFlagSet(flags, 0x10u);
    _readOnly = WI_IsFlagSet(flags, 0x100u);
    _copyFormatting = copyFormatting;
    _pasteFiltering = pasteFiltering;
}

void HwndTerminal::SetCursorColor(const COLORREF color)
{
    if (!_terminal)
    {
        return;
    }

    const auto lock = _terminal->LockForWriting();
    auto& renderSettings = _terminal->GetRenderSettings();
    renderSettings.SetColorTableEntry(TextColor::CURSOR_COLOR, color);
    renderSettings.SaveDefaultSettings();
}

bool HwndTerminal::CopySelection(const bool clearSelection)
{
    if (!_terminal || !_clipboardCallback)
    {
        return false;
    }

    ::Microsoft::Terminal::Core::Terminal::TextCopyData payload;
    {
        const auto lock = _terminal->LockForWriting();
        if (!_terminal->IsSelectionActive())
        {
            return false;
        }

        const auto copyHtml = WI_IsFlagSet(_copyFormatting, 0x1u);
        const auto copyRtf = WI_IsFlagSet(_copyFormatting, 0x2u);
        payload = _terminal->RetrieveSelectedTextFromBuffer(false, false, copyHtml, copyRtf);
        if (clearSelection)
        {
            _ClearSelection();
        }
    }

    _clipboardCallback(payload.plainText, payload.html, payload.rtf);
    return true;
}

void HwndTerminal::PasteText(const std::wstring_view text)
{
    if (!_terminal || _readOnly || text.empty())
    {
        return;
    }

    auto filtered = ::Microsoft::Console::Utils::FilterStringForPaste(
        text,
        static_cast<::Microsoft::Console::Utils::FilterOption>(_pasteFiltering));
    {
        const auto lock = _terminal->LockForReading();
        if (_terminal->IsXtermBracketedPasteModeEnabled())
        {
            filtered.insert(0, L"\x1b[200~");
            filtered.append(L"\x1b[201~");
        }
    }

    _WriteTextToConnection(filtered);

    const auto lock = _terminal->LockForWriting();
    _ClearSelection();
    _terminal->TrySnapOnInput();
}

::Microsoft::Console::Render::IRenderData* HwndTerminal::GetRenderData() const noexcept
{
    return _terminal.get();
}

HWND HwndTerminal::GetHwnd() const noexcept
{
    return _hwnd.get();
}

void HwndTerminal::_UpdateFont(int newDpi)
{
    if (!_terminal)
    {
        return;
    }
    _currentDpi = newDpi;

    // TODO: MSFT:20895307 If the font doesn't exist, this doesn't
    //      actually fail. We need a way to gracefully fallback.
    _renderer->TriggerFontChange(newDpi, _desiredFont, _actualFont);
}

IRawElementProviderSimple* HwndTerminal::_GetUiaProvider() noexcept
{
    // If TermControlUiaProvider throws during construction,
    // we don't want to try constructing an instance again and again.
    if (!_uiaProvider)
    {
        try
        {
            if (!_terminal)
            {
                return nullptr;
            }
            LOG_IF_FAILED(::Microsoft::WRL::MakeAndInitialize<HwndTerminalAutomationPeer>(&_uiaProvider, this->GetRenderData(), this));
            _uiaEngine = std::make_unique<::Microsoft::Console::Render::UiaEngine>(_uiaProvider.Get());
            LOG_IF_FAILED(_uiaEngine->Enable());
            const auto lock = _terminal->LockForWriting();
            _renderer->AddRenderEngine(_uiaEngine.get());
        }
        catch (...)
        {
            LOG_HR(wil::ResultFromCaughtException());
            _uiaProvider = nullptr;
        }
    }

    return _uiaProvider.Get();
}

HRESULT HwndTerminal::Refresh(const til::size windowSize, _Out_ til::size* dimensions)
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    RETURN_HR_IF_NULL(E_INVALIDARG, dimensions);

    const auto lock = _terminal->LockForWriting();

    _terminal->ClearSelection();

    RETURN_IF_FAILED(_renderEngine->SetWindowSize(windowSize));

    // Invalidate everything
    _renderer->TriggerRedrawAll();

    // Convert our new dimensions to characters
    const auto viewInPixels = Viewport::FromDimensions({}, windowSize);
    const auto vp = _renderEngine->GetViewportInCharacters(viewInPixels);

    // Guard against resizing the window to 0 columns/rows, which the text buffer classes don't really support.
    auto size = vp.Dimensions();
    size.width = std::max(size.width, 1);
    size.height = std::max(size.height, 1);

    // If this function succeeds with S_FALSE, then the terminal didn't
    //      actually change size. No need to notify the connection of this
    //      no-op.
    // TODO: MSFT:20642295 Resizing the buffer will corrupt it
    // I believe we'll need support for CSI 2J, and additionally I think
    //      we're resetting the viewport to the top
    RETURN_IF_FAILED(_terminal->UserResize(size));
    _terminal->UpdatePatternsUnderLock();
    if (_searchActive)
    {
        _searchState.Invalidated = true;
    }
    dimensions->width = size.width;
    dimensions->height = size.height;

    return S_OK;
}

void HwndTerminal::SendOutput(std::wstring_view data)
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    _terminal->Write(data);
    _terminal->UpdatePatternsUnderLock();
    if (_searchActive)
    {
        _searchState.Invalidated = true;
    }
}

HwndTerminalSearchState HwndTerminal::Search(
    const std::wstring_view query,
    const bool forward,
    const bool caseSensitive,
    const bool regularExpression,
    const bool executeSearch,
    const bool scrollIntoView,
    const int32_t scrollOffset)
{
    if (!_terminal)
    {
        return {};
    }

    const auto lock = _terminal->LockForWriting();
    SearchFlag flags{};
    WI_SetFlagIf(flags, SearchFlag::CaseInsensitive, !caseSensitive);
    WI_SetFlagIf(flags, SearchFlag::RegularExpression, regularExpression);
    const auto invalidated = _searcher.IsStale(*_terminal, query, flags);
    if (invalidated || executeSearch)
    {
        std::vector<til::point_span> oldResults;
        if (invalidated)
        {
            oldResults = _searcher.ExtractResults();
            _searcher.Reset(*_terminal, query, flags, !forward);
            _terminal->SetSearchHighlights(_searcher.Results());
        }
        if (executeSearch)
        {
            _searcher.FindNext(!forward);
        }
        _terminal->SetSearchHighlightFocused(gsl::narrow<size_t>(std::max<ptrdiff_t>(0, _searcher.CurrentMatch())));
        _renderer->TriggerSearchHighlight(oldResults);
    }
    if (scrollIntoView)
    {
        _terminal->ScrollToSearchHighlight(scrollOffset);
    }

    _searchActive = !query.empty();
    _searchState = {
        .TotalMatches = gsl::narrow<int32_t>(_searcher.Results().size()),
        .CurrentMatch = gsl::narrow<int32_t>(_searcher.CurrentMatch()),
        .Invalidated = invalidated,
        .InvalidRegex = !_searcher.IsOk(),
    };
    return _searchState;
}

void HwndTerminal::ClearSearch()
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    _terminal->SetSearchHighlights({});
    _searchActive = false;
    _terminal->SetSearchHighlightFocused(0);
    _renderer->TriggerSearchHighlight(_searcher.Results());
    _searcher = {};
    _searchState = {};
}

HwndTerminalSearchState HwndTerminal::GetSearchState() const noexcept
{
    return _searchState;
}

uint64_t HwndTerminal::_ensureRowMarkId(const til::CoordType row, const uint8_t kind)
{
    auto& buffer = _terminal->GetTextBuffer();
    auto& bufferRow = buffer.GetMutableRowByOffset(row);
    auto data = bufferRow.GetScrollbarData().value_or(ScrollbarData{});
    if (data.reseshId == 0)
    {
        data.reseshId = _nextMarkId++;
    }
    if (kind != 0)
    {
        data.reseshKind = kind;
    }
    bufferRow.SetScrollbarData(data);
    return data.reseshId;
}

std::optional<til::CoordType> HwndTerminal::_findRowByMarkId(const uint64_t id) const
{
    if (!_terminal || id == 0)
    {
        return std::nullopt;
    }
    for (const auto& mark : _terminal->GetTextBuffer().GetMarkRows())
    {
        if (mark.data.reseshId == id || mark.data.reseshBookmarkId == id)
        {
            return mark.row;
        }
    }
    return std::nullopt;
}

til::CoordType HwndTerminal::_logicalLineStart(til::CoordType row) const
{
    const auto& buffer = _terminal->GetTextBuffer();
    while (row > 0 && buffer.GetRowByOffset(row - 1).WasWrapForced())
    {
        --row;
    }
    return row;
}

til::CoordType HwndTerminal::_logicalLineEnd(til::CoordType row) const
{
    const auto& buffer = _terminal->GetTextBuffer();
    const auto last = buffer.GetSize().BottomInclusive();
    while (row < last && buffer.GetRowByOffset(row).WasWrapForced())
    {
        ++row;
    }
    return row;
}

std::wstring HwndTerminal::_readRows(const til::CoordType first, const til::CoordType last) const
{
    std::wstring result;
    const auto& buffer = _terminal->GetTextBuffer();
    for (auto rowIndex = first; rowIndex <= last; ++rowIndex)
    {
        const auto& row = buffer.GetRowByOffset(rowIndex);
        const auto text = row.GetText();
        const auto end = text.find_last_not_of(UNICODE_SPACE);
        if (end != std::wstring_view::npos)
        {
            result.append(text.substr(0, end + 1));
        }
        if (rowIndex != last && !row.WasWrapForced())
        {
            result.push_back(L'\n');
        }
    }
    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == UNICODE_SPACE))
    {
        result.pop_back();
    }
    return result;
}

std::vector<HwndTerminalMark> HwndTerminal::GetMarks()
{
    if (!_terminal)
    {
        return {};
    }
    const auto lock = _terminal->LockForWriting();
    std::vector<HwndTerminalMark> result;

    auto exactMarks = _terminal->GetMarkExtents();
    result.reserve(exactMarks.size() + _applicationCommands.size());
    for (auto& mark : exactMarks)
    {
        const auto id = _ensureRowMarkId(mark.start.y, 0);
        mark.data.reseshId = id;
        result.push_back({
            id,
            _markGeneration,
            HwndTerminalMarkKind::ExactCommand,
            static_cast<uint32_t>(mark.data.category),
            static_cast<uint32_t>(static_cast<COLORREF>(_terminal->GetColorForMark(mark.data))),
            mark.data.exitCode,
            mark.start,
            mark.end,
            mark.commandEnd,
            mark.outputEnd,
        });
    }

    std::unordered_set<uint64_t> liveApplicationIds;
    for (const auto& rowMark : _terminal->GetTextBuffer().GetMarkRows())
    {
        if (rowMark.data.reseshBookmarkId != 0)
        {
            result.push_back({
                rowMark.data.reseshBookmarkId,
                _markGeneration,
                HwndTerminalMarkKind::Bookmark,
                static_cast<uint32_t>(rowMark.data.category),
                static_cast<uint32_t>(static_cast<COLORREF>(
                    rowMark.data.reseshBookmarkColor.value_or(_terminal->GetColorForMark(rowMark.data)))),
                std::nullopt,
                { 0, rowMark.row },
                { 0, rowMark.row },
                std::nullopt,
                std::nullopt,
            });
        }
        if (rowMark.data.reseshKind == 2)
        {
            liveApplicationIds.emplace(rowMark.data.reseshId);
            const auto logicalEnd = _logicalLineEnd(rowMark.row);
            result.push_back({
                rowMark.data.reseshId,
                _markGeneration,
                HwndTerminalMarkKind::ApplicationCommand,
                static_cast<uint32_t>(rowMark.data.category),
                static_cast<uint32_t>(static_cast<COLORREF>(_terminal->GetColorForMark(rowMark.data))),
                rowMark.data.exitCode,
                { 0, rowMark.row },
                { 0, rowMark.row },
                til::point{ 0, logicalEnd },
                std::nullopt,
            });
        }
    }
    std::erase_if(_applicationCommands, [&](const auto& entry) {
        return !liveApplicationIds.contains(entry.first);
    });
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.Start.y != right.Start.y)
        {
            return left.Start.y < right.Start.y;
        }
        return left.Kind < right.Kind;
    });
    return result;
}

std::vector<int32_t> HwndTerminal::GetSearchRows() const
{
    if (!_terminal)
    {
        return {};
    }
    const auto lock = _terminal->LockForReading();
    std::vector<int32_t> rows;
    rows.reserve(_searcher.Results().size());
    for (const auto& result : _searcher.Results())
    {
        rows.push_back(result.start.y);
    }
    return rows;
}

std::wstring HwndTerminal::GetMarkText(const uint64_t id, const bool includeOutput)
{
    if (!_terminal || id == 0)
    {
        return {};
    }
    const auto lock = _terminal->LockForWriting();
    const auto row = _findRowByMarkId(id);
    if (!row)
    {
        return {};
    }
    const auto rowData = _terminal->GetTextBuffer().GetRowByOffset(*row).GetScrollbarData();
    if (rowData && rowData->reseshKind == 3)
    {
        return _readRows(*row, _logicalLineEnd(*row));
    }

    if (const auto application = _applicationCommands.find(id); application != _applicationCommands.end())
    {
        if (!includeOutput)
        {
            return application->second.Command;
        }
        auto last = _terminal->GetTextBuffer().GetSize().BottomInclusive();
        for (const auto& mark : _terminal->GetTextBuffer().GetMarkRows())
        {
            if (mark.row > *row && mark.data.reseshId != 0 && mark.data.reseshKind != 3)
            {
                last = mark.row - 1;
                break;
            }
        }
        const auto cursor = _terminal->GetTextBuffer().GetCursor().GetPosition();
        const auto cursorStart = _logicalLineStart(cursor.y);
        if (cursorStart > *row)
        {
            last = std::min(last, cursorStart - 1);
        }
        return _readRows(*row, std::max(*row, last));
    }

    for (const auto& mark : _terminal->GetMarkExtents())
    {
        if (mark.data.reseshId != id)
        {
            continue;
        }
        const auto& buffer = _terminal->GetTextBuffer();
        if (!includeOutput)
        {
            return mark.commandEnd ? buffer.GetPlainText(mark.end, *mark.commandEnd) : std::wstring{};
        }
        const auto end = til::coalesce_value(mark.outputEnd, mark.commandEnd, mark.end);
        return buffer.GetPlainText(mark.start, end);
    }
    return {};
}

bool HwndTerminal::ScrollToMark(const uint64_t id)
{
    if (!_terminal)
    {
        return false;
    }
    const auto lock = _terminal->LockForWriting();
    if (const auto row = _findRowByMarkId(id))
    {
        const auto height = _terminal->GetViewport().Height();
        _terminal->UserScrollViewport(std::max(0, *row - height / 2));
        return true;
    }
    return false;
}

HwndTerminalPromptProbe HwndTerminal::BeginPromptProbe()
{
    if (!_terminal)
    {
        return {};
    }
    const auto lock = _terminal->LockForWriting();
    const auto cursor = _terminal->GetTextBuffer().GetCursor().GetPosition();
    const auto start = _logicalLineStart(cursor.y);
    auto& row = _terminal->GetTextBuffer().GetMutableRowByOffset(start);
    auto data = row.GetScrollbarData().value_or(ScrollbarData{});
    if (data.reseshKind == 3)
    {
        return {
            data.reseshId,
            _markGeneration,
            { 0, start },
            cursor,
            _readRows(start, cursor.y),
        };
    }
    if (data.category != MarkCategory::Default)
    {
        return {
            0,
            _markGeneration,
            { 0, start },
            cursor,
            _readRows(start, cursor.y),
        };
    }
    if (data.reseshId != 0)
    {
        return {};
    }

    data.reseshId = _nextMarkId++;
    data.reseshKind = 3;
    row.SetScrollbarData(data);
    return {
        data.reseshId,
        _markGeneration,
        { 0, start },
        cursor,
        _readRows(start, cursor.y),
    };
}

bool HwndTerminal::CommitPromptProbe(
    const uint64_t id,
    const std::wstring_view command,
    const std::optional<uint32_t> exitCode)
{
    if (!_terminal || id == 0 || command.empty())
    {
        return false;
    }
    const auto lock = _terminal->LockForWriting();
    const auto row = _findRowByMarkId(id);
    if (!row)
    {
        return false;
    }
    auto& bufferRow = _terminal->GetTextBuffer().GetMutableRowByOffset(*row);
    auto data = bufferRow.GetScrollbarData();
    if (!data || (data->reseshKind != 3 && data->reseshKind != 2))
    {
        return false;
    }
    data->reseshKind = 2;
    data->exitCode = exitCode;
    bufferRow.SetScrollbarData(data);
    _applicationCommands[id] = {
        std::wstring{ command },
        { 0, *row },
        _terminal->GetTextBuffer().GetCursor().GetPosition(),
    };
    ++_markGeneration;
    return true;
}

bool HwndTerminal::DiscardPromptProbe(const uint64_t id)
{
    if (!_terminal || id == 0)
    {
        return false;
    }
    const auto lock = _terminal->LockForWriting();
    const auto row = _findRowByMarkId(id);
    if (!row)
    {
        return false;
    }
    auto& bufferRow = _terminal->GetTextBuffer().GetMutableRowByOffset(*row);
    auto data = bufferRow.GetScrollbarData();
    if (!data || data->reseshKind != 3)
    {
        return false;
    }
    data->reseshId = 0;
    data->reseshKind = 0;
    if (data->reseshBookmarkId == 0 &&
        data->category == MarkCategory::Default &&
        !data->color &&
        !data->exitCode)
    {
        bufferRow.SetScrollbarData(std::nullopt);
    }
    else
    {
        bufferRow.SetScrollbarData(data);
    }
    return true;
}

uint64_t HwndTerminal::AddBookmark(const int32_t requestedRow, const std::optional<til::color> color)
{
    if (!_terminal)
    {
        return 0;
    }
    const auto lock = _terminal->LockForWriting();
    const auto& buffer = _terminal->GetTextBuffer();
    const auto cursorRow = buffer.GetCursor().GetPosition().y;
    const auto row = std::clamp(requestedRow < 0 ? cursorRow : requestedRow, 0, buffer.GetSize().BottomInclusive());
    auto& bufferRow = _terminal->GetTextBuffer().GetMutableRowByOffset(row);
    auto data = bufferRow.GetScrollbarData().value_or(ScrollbarData{});
    if (data.reseshBookmarkId != 0)
    {
        return data.reseshBookmarkId;
    }
    data.reseshBookmarkId = _nextMarkId++;
    data.reseshBookmarkColor = color;
    bufferRow.SetScrollbarData(data);
    ++_markGeneration;
    return data.reseshBookmarkId;
}

bool HwndTerminal::RemoveBookmark(const uint64_t id)
{
    if (!_terminal || id == 0)
    {
        return false;
    }
    const auto lock = _terminal->LockForWriting();
    const auto row = _findRowByMarkId(id);
    if (!row)
    {
        return false;
    }
    auto& bufferRow = _terminal->GetTextBuffer().GetMutableRowByOffset(*row);
    auto data = bufferRow.GetScrollbarData();
    if (!data || data->reseshBookmarkId != id)
    {
        return false;
    }
    data->reseshBookmarkId = 0;
    data->reseshBookmarkColor.reset();
    if (data->reseshId == 0 &&
        data->category == MarkCategory::Default &&
        !data->color &&
        !data->exitCode)
    {
        bufferRow.SetScrollbarData(std::nullopt);
    }
    else
    {
        bufferRow.SetScrollbarData(data);
    }
    ++_markGeneration;
    return true;
}

void HwndTerminal::ClearBookmarks()
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    bool changed = false;
    for (const auto& mark : _terminal->GetTextBuffer().GetMarkRows())
    {
        if (mark.data.reseshBookmarkId != 0)
        {
            auto data = mark.data;
            data.reseshBookmarkId = 0;
            data.reseshBookmarkColor.reset();
            auto& row = _terminal->GetTextBuffer().GetMutableRowByOffset(mark.row);
            if (data.reseshId == 0 &&
                data.category == MarkCategory::Default &&
                !data.color &&
                !data.exitCode)
            {
                row.SetScrollbarData(std::nullopt);
            }
            else
            {
                row.SetScrollbarData(data);
            }
            changed = true;
        }
    }
    if (changed)
    {
        ++_markGeneration;
    }
}

uint64_t HwndTerminal::MarkGeneration() const noexcept
{
    return _markGeneration;
}

void _stdcall AvoidBuggyTSFConsoleFlags()
{
    Microsoft::Console::TSF::Handle::AvoidBuggyTSFConsoleFlags();
}

HRESULT _stdcall CreateTerminal(HWND parentHwnd, _Out_ void** hwnd, _Out_ void** terminal)
{
    HwndTerminalOptions options{};
    options.Theme.DefaultBackground = RGB(12, 12, 12);
    options.Theme.DefaultForeground = RGB(204, 204, 204);
    options.Theme.DefaultSelectionBackground = RGB(38, 79, 120);
    options.CursorColor = RGB(255, 255, 255);
    options.Theme.CursorStyle = 0;
    const auto colors = ::Microsoft::Console::Utils::CampbellColorTable();
    options.WordDelimiters = DEFAULT_WORD_DELIMITERS;
    auto publicTerminal = std::make_unique<HwndTerminal>(parentHwnd, options);

    RETURN_IF_FAILED(publicTerminal->Initialize(options));

    *hwnd = publicTerminal->GetHwnd();
    *terminal = publicTerminal.release();

    return S_OK;
}

void _stdcall TerminalRegisterScrollCallback(void* terminal, void __stdcall callback(int, int, int))
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->RegisterScrollCallback(callback);
}
CATCH_LOG()

void _stdcall TerminalRegisterWriteCallback(void* terminal, const void __stdcall callback(wchar_t*))
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->RegisterWriteCallback(callback);
}
CATCH_LOG()

void _stdcall TerminalSendOutput(void* terminal, LPCWSTR data)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->SendOutput(data);
}
CATCH_LOG()

/// <summary>
/// Triggers a terminal resize using the new width and height in pixel.
/// </summary>
/// <param name="terminal">Terminal pointer.</param>
/// <param name="width">New width of the terminal in pixels.</param>
/// <param name="height">New height of the terminal in pixels</param>
/// <param name="dimensions">Out parameter containing the columns and rows that fit the new size.</param>
/// <returns>HRESULT of the attempted resize.</returns>
HRESULT _stdcall TerminalTriggerResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);

    LOG_IF_WIN32_BOOL_FALSE(SetWindowPos(
        publicTerminal->GetHwnd(),
        nullptr,
        0,
        0,
        static_cast<int>(width),
        static_cast<int>(height),
        0));

    const til::size windowSize{ width, height };
    return publicTerminal->Refresh(windowSize, dimensions);
}

/// <summary>
/// Helper method for resizing the terminal using character column and row counts
/// </summary>
/// <param name="terminal">Pointer to the terminal object.</param>
/// <param name="dimensionsInCharacters">New terminal size in row and column count.</param>
/// <param name="dimensionsInPixels">Out parameter with the new size of the renderer.</param>
/// <returns>HRESULT of the attempted resize.</returns>
HRESULT _stdcall TerminalTriggerResizeWithDimension(_In_ void* terminal, _In_ til::size dimensionsInCharacters, _Out_ til::size* dimensionsInPixels)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, dimensionsInPixels);

    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);

    Viewport viewInPixels;
    {
        const auto viewInCharacters = Viewport::FromDimensions({}, dimensionsInCharacters);
        const auto lock = publicTerminal->_terminal->LockForReading();
        viewInPixels = publicTerminal->_renderEngine->GetViewportInPixels(viewInCharacters);
    }

    dimensionsInPixels->width = viewInPixels.Width();
    dimensionsInPixels->height = viewInPixels.Height();

    til::size unused;

    return TerminalTriggerResize(terminal, viewInPixels.Width(), viewInPixels.Height(), &unused);
}
CATCH_RETURN()

/// <summary>
/// Calculates the amount of rows and columns that fit in the provided width and height.
/// </summary>
/// <param name="terminal">Terminal pointer</param>
/// <param name="width">Width of the terminal area to calculate.</param>
/// <param name="height">Height of the terminal area to calculate.</param>
/// <param name="dimensions">Out parameter containing the columns and rows that fit the new size.</param>
/// <returns>HRESULT of the calculation.</returns>
HRESULT _stdcall TerminalCalculateResize(_In_ void* terminal, _In_ til::CoordType width, _In_ til::CoordType height, _Out_ til::size* dimensions)
try
{
    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);

    const auto viewInPixels = Viewport::FromDimensions({}, { width, height });
    const auto lock = publicTerminal->_terminal->LockForReading();
    const auto viewInCharacters = publicTerminal->_renderEngine->GetViewportInCharacters(viewInPixels);

    dimensions->width = viewInCharacters.Width();
    dimensions->height = viewInCharacters.Height();

    return S_OK;
}
CATCH_RETURN()

void _stdcall TerminalDpiChanged(void* terminal, int newDpi)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_UpdateFont(newDpi);
}
CATCH_LOG()

void _stdcall TerminalUserScroll(void* terminal, int viewTop)
try
{
    if (const auto publicTerminal = static_cast<const HwndTerminal*>(terminal); publicTerminal && publicTerminal->_terminal)
    {
        const auto lock = publicTerminal->_terminal->LockForWriting();
        publicTerminal->_terminal->UserScrollViewport(viewTop);
    }
}
CATCH_LOG()

const unsigned int HwndTerminal::_NumberOfClicks(til::point point, std::chrono::steady_clock::time_point timestamp) noexcept
{
    // if click occurred at a different location or past the multiClickTimer...
    const auto delta{ timestamp - _lastMouseClickTimestamp };
    if (point != _lastMouseClickPos || delta > _multiClickTime)
    {
        // exit early. This is a single click.
        _multiClickCounter = 1;
    }
    else
    {
        _multiClickCounter++;
    }
    return _multiClickCounter;
}

std::optional<std::pair<std::wstring, uint32_t>> HwndTerminal::_LinkAt(const LPARAM lParam)
{
    if (!_terminal)
    {
        return std::nullopt;
    }
    const auto fontSize = _actualFont.GetSize();
    if (fontSize.area() == 0)
    {
        return std::nullopt;
    }
    const til::point pixel{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (pixel.x < 0 || pixel.y < 0)
    {
        return std::nullopt;
    }
    const auto cell = pixel / fontSize;
    const auto lock = _terminal->LockForReading();
    const auto viewportSize = _terminal->GetViewport().Dimensions();
    if (cell.x >= viewportSize.width || cell.y >= viewportSize.height)
    {
        return std::nullopt;
    }
    const auto hyperlinkId = _terminal->GetHyperlinkIdAtViewportPosition(cell);
    auto uri = _terminal->GetHyperlinkAtViewportPosition(cell);
    if (uri.empty())
    {
        return std::nullopt;
    }
    return std::pair{ std::move(uri), hyperlinkId == 0 ? DetectedUrlLinkSource : Osc8LinkSource };
}

void HwndTerminal::_UpdateHoveredLink(const LPARAM lParam)
{
    if (!_terminal)
    {
        return;
    }
    TRACKMOUSEEVENT tracking{
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = TME_LEAVE,
        .hwndTrack = _hwnd.get(),
    };
    TrackMouseEvent(&tracking);

    uint16_t hyperlinkId{};
    std::optional<interval_tree::IntervalTree<til::point, size_t>::interval> interval;
    const auto fontSize = _actualFont.GetSize();
    const til::point pixel{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (fontSize.area() != 0 && pixel.x >= 0 && pixel.y >= 0)
    {
        const auto cell = pixel / fontSize;
        const auto lock = _terminal->LockForReading();
        const auto viewportSize = _terminal->GetViewport().Dimensions();
        if (cell.x < viewportSize.width && cell.y < viewportSize.height)
        {
            hyperlinkId = _terminal->GetHyperlinkIdAtViewportPosition(cell);
            if (hyperlinkId == 0)
            {
                interval = _terminal->GetHyperlinkIntervalFromViewportPosition(cell);
            }
        }
    }
    if (hyperlinkId == _hoveredHyperlinkId && interval == _hoveredHyperlinkInterval)
    {
        return;
    }
    _hoveredHyperlinkId = hyperlinkId;
    _hoveredHyperlinkInterval = interval;
    _renderer->UpdateHyperlinkHoveredId(hyperlinkId);
    _renderer->UpdateLastHoveredInterval(interval);
    _renderer->TriggerRedrawAll();
    SetCursor(LoadCursorW(nullptr, hyperlinkId != 0 || interval ? IDC_HAND : IDC_IBEAM));
}

void HwndTerminal::_ClearHoveredLink()
{
    if (_hoveredHyperlinkId == 0 && !_hoveredHyperlinkInterval)
    {
        return;
    }
    _hoveredHyperlinkId = 0;
    _hoveredHyperlinkInterval = std::nullopt;
    _renderer->UpdateHyperlinkHoveredId(0);
    _renderer->UpdateLastHoveredInterval(std::nullopt);
    _renderer->TriggerRedrawAll();
}

HRESULT HwndTerminal::_StartSelection(LPARAM lParam) noexcept
try
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    const til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const auto lock = _terminal->LockForWriting();
    const auto altPressed = GetKeyState(VK_MENU) < 0;
    const til::size fontSize{ this->_actualFont.GetSize() };

    this->_terminal->SetBlockSelection(altPressed);

    const auto clickCount{ _NumberOfClicks(cursorPosition, std::chrono::steady_clock::now()) };

    // This formula enables the number of clicks to cycle properly between single-, double-, and triple-click.
    // To increase the number of acceptable click states, simply increment MAX_CLICK_COUNT and add another if-statement
    const unsigned int MAX_CLICK_COUNT = 3;
    const auto multiClickMapper = clickCount > MAX_CLICK_COUNT ? ((clickCount + MAX_CLICK_COUNT - 1) % MAX_CLICK_COUNT) + 1 : clickCount;

    if (multiClickMapper == 3)
    {
        _terminal->MultiClickSelection(cursorPosition / fontSize, ::Terminal::SelectionExpansion::Line);
    }
    else if (multiClickMapper == 2)
    {
        _terminal->MultiClickSelection(cursorPosition / fontSize, ::Terminal::SelectionExpansion::Word);
    }
    else
    {
        this->_terminal->ClearSelection();
        _singleClickTouchdownPos = cursorPosition;

        _lastMouseClickTimestamp = std::chrono::steady_clock::now();
        _lastMouseClickPos = cursorPosition;
    }
    this->_renderer->TriggerSelection();

    return S_OK;
}
CATCH_RETURN();

HRESULT HwndTerminal::_MoveSelection(LPARAM lParam) noexcept
try
{
    RETURN_HR_IF_NULL(E_NOT_VALID_STATE, _terminal);
    const til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const auto lock = _terminal->LockForWriting();
    const til::size fontSize{ this->_actualFont.GetSize() };

    RETURN_HR_IF(E_NOT_VALID_STATE, fontSize.area() == 0); // either dimension = 0, area == 0

    // This is a copy of ControlInteractivity::PointerMoved
    if (_singleClickTouchdownPos)
    {
        const auto touchdownPoint = *_singleClickTouchdownPos;
        const auto dx = cursorPosition.x - touchdownPoint.x;
        const auto dy = cursorPosition.y - touchdownPoint.y;
        const auto w = fontSize.width;
        const auto distanceSquared = dx * dx + dy * dy;
        const auto maxDistanceSquared = w * w / 16; // (w / 4)^2

        if (distanceSquared >= maxDistanceSquared)
        {
            _terminal->SetSelectionAnchor(touchdownPoint / fontSize);
            // stop tracking the touchdown point
            _singleClickTouchdownPos = std::nullopt;
        }
    }

    this->_terminal->SetSelectionEnd(cursorPosition / fontSize);
    this->_renderer->TriggerSelection();

    return S_OK;
}
CATCH_RETURN();

void HwndTerminal::_ClearSelection()
{
    if (!_terminal)
    {
        return;
    }
    _terminal->ClearSelection();
    _renderer->TriggerSelection();
}

void _stdcall TerminalClearSelection(void* terminal)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_ClearSelection();
}
CATCH_LOG()

bool _stdcall TerminalIsSelectionActive(void* terminal)
try
{
    if (const auto publicTerminal = static_cast<const HwndTerminal*>(terminal); publicTerminal && publicTerminal->_terminal)
    {
        const auto lock = publicTerminal->_terminal->LockForReading();
        return publicTerminal->_terminal->IsSelectionActive();
    }
    return false;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

// Returns the selected text in the terminal.
const wchar_t* _stdcall TerminalGetSelection(void* terminal)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return nullptr;
    }

    std::wstring selectedText;
    {
        const auto lock = publicTerminal->_terminal->LockForWriting();
        auto bufferData = publicTerminal->_terminal->RetrieveSelectedTextFromBuffer(false);
        selectedText = std::move(bufferData.plainText);
        publicTerminal->_ClearSelection();
    }

    auto returnText = wil::make_cotaskmem_string_nothrow(selectedText.c_str());
    return returnText.release();
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return nullptr;
}

static ControlKeyStates getControlKeyState() noexcept
{
    struct KeyModifier
    {
        int vkey;
        ControlKeyStates flags;
    };

    constexpr std::array<KeyModifier, 5> modifiers{ {
        { VK_RMENU, ControlKeyStates::RightAltPressed },
        { VK_LMENU, ControlKeyStates::LeftAltPressed },
        { VK_RCONTROL, ControlKeyStates::RightCtrlPressed },
        { VK_LCONTROL, ControlKeyStates::LeftCtrlPressed },
        { VK_SHIFT, ControlKeyStates::ShiftPressed },
    } };

    ControlKeyStates flags;

    for (const auto& mod : modifiers)
    {
        const auto state = GetKeyState(mod.vkey);
        const auto isDown = state < 0;

        if (isDown)
        {
            flags |= mod.flags;
        }
    }

    return flags;
}

bool HwndTerminal::_CanSendVTMouseInput() const noexcept
{
    // Only allow the transit of mouse events if shift isn't pressed.
    const auto shiftPressed = GetKeyState(VK_SHIFT) < 0;
    const auto lock = _terminal->LockForReading();
    return !shiftPressed && _focused && _terminal && _terminal->IsTrackingMouseInput();
}

bool HwndTerminal::_SendMouseEvent(UINT uMsg, WPARAM wParam, LPARAM lParam) noexcept
try
{
    if (!_terminal)
    {
        return false;
    }

    til::point cursorPosition{
        GET_X_LPARAM(lParam),
        GET_Y_LPARAM(lParam),
    };

    const til::size fontSize{ this->_actualFont.GetSize() };
    short wheelDelta{ 0 };
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL)
    {
        wheelDelta = HIWORD(wParam);

        // If it's a *WHEEL event, it's in screen coordinates, not window (?!)
        ScreenToClient(_hwnd.get(), cursorPosition.as_win32_point());
    }

    const Microsoft::Console::VirtualTerminal::TerminalInput::MouseButtonState state{
        WI_IsFlagSet(GetKeyState(VK_LBUTTON), KeyPressed),
        WI_IsFlagSet(GetKeyState(VK_MBUTTON), KeyPressed),
        WI_IsFlagSet(GetKeyState(VK_RBUTTON), KeyPressed)
    };

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForReading();
        out = _terminal->SendMouseEvent(cursorPosition / fontSize, uMsg, getControlKeyState(), wheelDelta, state);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
        return true;
    }
    return false;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

void HwndTerminal::_SendKeyEvent(WORD vkey, WORD scanCode, WORD flags, bool keyDown) noexcept
try
{
    if (!_terminal)
    {
        return;
    }

    auto modifiers = getControlKeyState();
    if (WI_IsFlagSet(flags, ENHANCED_KEY))
    {
        modifiers |= ControlKeyStates::EnhancedKey;
    }
    if (vkey && keyDown && _uiaProvider)
    {
        _uiaProvider->RecordKeyEvent(vkey);
    }

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForReading();
        out = _terminal->SendKeyEvent(vkey, scanCode, modifiers, keyDown);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
    }
}
CATCH_LOG();

void HwndTerminal::_SendCharEvent(wchar_t ch, WORD scanCode, WORD flags) noexcept
try
{
    if (!_terminal)
    {
        return;
    }

    TerminalInput::OutputType out;
    {
        const auto lock = _terminal->LockForWriting();

        if (_terminal->IsSelectionActive())
        {
            _ClearSelection();
            if (ch == UNICODE_ESC)
            {
                // ESC should clear any selection before it triggers input.
                // Other characters pass through.
                return;
            }
        }

        if (ch == UNICODE_TAB)
        {
            // TAB was handled as a keydown event (cf. Terminal::SendKeyEvent)
            return;
        }

        auto modifiers = getControlKeyState();
        if (WI_IsFlagSet(flags, ENHANCED_KEY))
        {
            modifiers |= ControlKeyStates::EnhancedKey;
        }

        out = _terminal->SendCharEvent(ch, scanCode, modifiers);
    }
    if (out)
    {
        _WriteTextToConnection(*out);
    }
}
CATCH_LOG();

void _stdcall TerminalSendKeyEvent(void* terminal, WORD vkey, WORD scanCode, WORD flags, bool keyDown)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_SendKeyEvent(vkey, scanCode, flags, keyDown);
}
CATCH_LOG()

void _stdcall TerminalSendCharEvent(void* terminal, wchar_t ch, WORD scanCode, WORD flags)
try
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_SendCharEvent(ch, scanCode, flags);
}
CATCH_LOG()

void _stdcall DestroyTerminal(void* terminal)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    delete publicTerminal;
}

// Updates the terminal font type, size, color, as well as the background/foreground colors to a specified theme.
void _stdcall TerminalSetTheme(void* terminal, TerminalTheme theme, LPCWSTR fontFamily, til::CoordType fontSize, int newDpi)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return;
    }

    {
        const auto lock = publicTerminal->_terminal->LockForWriting();

        auto& renderSettings = publicTerminal->_terminal->GetRenderSettings();
        renderSettings.SetColorTableEntry(TextColor::DEFAULT_FOREGROUND, theme.DefaultForeground);
        renderSettings.SetColorTableEntry(TextColor::DEFAULT_BACKGROUND, theme.DefaultBackground);
        renderSettings.SetColorTableEntry(TextColor::SELECTION_BACKGROUND, theme.DefaultSelectionBackground);

        // Set the font colors
        for (size_t tableIndex = 0; tableIndex < 16; tableIndex++)
        {
            // It's using gsl::at to check the index is in bounds, but the analyzer still calls this array-to-pointer-decay
            GSL_SUPPRESS(bounds .3)
            renderSettings.SetColorTableEntry(tableIndex, gsl::at(theme.ColorTable, tableIndex));
        }

        // Save these values as the new default render settings.
        renderSettings.SaveDefaultSettings();

        publicTerminal->_terminal->SetCursorStyle(static_cast<Microsoft::Console::VirtualTerminal::DispatchTypes::CursorStyle>(theme.CursorStyle));

        publicTerminal->_desiredFont = { fontFamily, 0, DEFAULT_FONT_WEIGHT, static_cast<float>(fontSize), CP_UTF8 };
        publicTerminal->_desiredFont.SetEnableBuiltinGlyphs(true);
        publicTerminal->_UpdateFont(newDpi);
    }

    // When the font changes the terminal dimensions need to be recalculated since the available row and column
    // space will have changed.
    RECT windowRect;
    GetWindowRect(publicTerminal->_hwnd.get(), &windowRect);

    til::size dimensions;
    const til::size windowSize{ windowRect.right - windowRect.left, windowRect.bottom - windowRect.top };
    publicTerminal->Refresh(windowSize, &dimensions);
}

void _stdcall TerminalBlinkCursor(void* terminal)
try
{
    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return;
    }

    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_terminal->BlinkCursor();
}
CATCH_LOG()

void _stdcall TerminalSetCursorVisible(void* terminal, const bool visible)
try
{
    const auto publicTerminal = static_cast<const HwndTerminal*>(terminal);
    if (!publicTerminal || !publicTerminal->_terminal)
    {
        return;
    }
    const auto lock = publicTerminal->_terminal->LockForWriting();
    publicTerminal->_terminal->SetCursorOn(visible);
}
CATCH_LOG()

void __stdcall TerminalSetFocus(void* terminal)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_focused = true;
    if (auto uiaEngine = publicTerminal->_uiaEngine.get())
    {
        LOG_IF_FAILED(uiaEngine->Enable());
    }
    publicTerminal->_FocusTSF();
}

void HwndTerminal::_FocusTSF() noexcept
{
    if (!_tsfHandle)
    {
        _tsfHandle = Microsoft::Console::TSF::Handle::Create();
        _tsfHandle.AssociateFocus(&_tsfDataProvider);
    }
}

void __stdcall TerminalKillFocus(void* terminal)
{
    const auto publicTerminal = static_cast<HwndTerminal*>(terminal);
    publicTerminal->_focused = false;
    if (auto uiaEngine = publicTerminal->_uiaEngine.get())
    {
        LOG_IF_FAILED(uiaEngine->Disable());
    }
}


til::size HwndTerminal::GetFontSize() const noexcept
{
    return _actualFont.GetSize();
}

til::rect HwndTerminal::GetBounds() const noexcept
{
    til::rect windowRect;
    GetWindowRect(_hwnd.get(), windowRect.as_win32_rect());
    return windowRect;
}

til::rect HwndTerminal::GetPadding() const noexcept
{
    return {};
}

void HwndTerminal::ChangeViewport(const til::inclusive_rect& NewWindow)
{
    if (!_terminal)
    {
        return;
    }
    const auto lock = _terminal->LockForWriting();
    _terminal->UserScrollViewport(NewWindow.top);
}

HRESULT HwndTerminal::GetHostUiaProvider(IRawElementProviderSimple** provider) noexcept
{
    return UiaHostProviderFromHwnd(_hwnd.get(), provider);
}
