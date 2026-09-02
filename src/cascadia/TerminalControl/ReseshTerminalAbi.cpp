// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ReseshTerminalAbi.h"

#include "HwndTerminal.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace
{
    thread_local ReseshTerminalHandle callbackHandle{};
    constexpr wchar_t BuildId[]{ L"terminal-v1.24.11911.0-resesh-abi1.2" };
    constexpr size_t MaximumQueuedEventUnits = 16 * 1024 * 1024;
    constexpr uint32_t MaximumOutputCharacters = 16 * 1024 * 1024;
    constexpr uint32_t MaximumClipboardCharacters = 4 * 1024 * 1024;
    constexpr uint32_t MaximumFontFamilyCharacters = 1024;
    constexpr uint32_t MaximumWordDelimiterCharacters = 4096;
    constexpr int32_t MaximumHistorySize = 1'000'000;
    constexpr size_t MaximumQueuedEventCount = 1024;

    struct QueuedEvent
    {
        uint32_t type;
        uint32_t flags;
        uint64_t sequence;
        std::wstring text;
        std::string html;
        std::string rtf;
        int64_t value0{};
        int64_t value1{};
        int64_t value2{};
    };

    struct TerminalState
    {
        ReseshTerminalHandle handle{};
        HwndTerminal* terminal{};
        std::mutex operationMutex;
        std::mutex drainMutex;
        std::mutex eventMutex;
        std::condition_variable callbackFinished;
        std::deque<QueuedEvent> events;
        size_t queuedUnits{};
        uint64_t nextSequence{ 1 };
        ReseshTerminalEventCallback callback{};
        void* callbackContext{};
        uint32_t callbacksInProgress{};
        bool destroying{};

        void QueueEvent(
            const uint32_t type,
            std::wstring text = {},
            std::string html = {},
            std::string rtf = {},
            const uint32_t flags = 0,
            const int64_t value0 = 0,
            const int64_t value1 = 0,
            const int64_t value2 = 0)
        {
            const auto units = text.size() + html.size() + rtf.size();
            if (units > MaximumQueuedEventUnits)
            {
                return;
            }

            const std::scoped_lock lock{ eventMutex };
            if (destroying || events.size() >= MaximumQueuedEventCount ||
                queuedUnits > MaximumQueuedEventUnits - units)
            {
                return;
            }

            queuedUnits += units;
            events.emplace_back(QueuedEvent{
                type,
                flags,
                nextSequence++,
                std::move(text),
                std::move(html),
                std::move(rtf),
                value0,
                value1,
                value2,
            });
            if (terminal)
            {
                terminal->RequestEventDispatch();
            }
        }

        void QueueInput(const std::wstring_view input)
        {
            if (!input.empty())
            {
                QueueEvent(ReseshTerminalEventTypeInput, std::wstring{ input });
            }
        }

        void DrainEvents()
        {
            const std::scoped_lock drainLock{ drainMutex };
            for (;;)
            {
                QueuedEvent queued{};
                ReseshTerminalEventCallback currentCallback{};
                void* currentContext{};
                {
                    const std::scoped_lock lock{ eventMutex };
                    if (destroying || !callback || events.empty())
                    {
                        return;
                    }

                    queued = std::move(events.front());
                    events.pop_front();
                    queuedUnits -= queued.text.size() + queued.html.size() + queued.rtf.size();
                    currentCallback = callback;
                    currentContext = callbackContext;
                    ++callbacksInProgress;
                }

                const ReseshTerminalEvent eventData{
                    sizeof(ReseshTerminalEvent),
                    RESESH_TERMINAL_ABI_MAJOR,
                    RESESH_TERMINAL_ABI_MINOR,
                    queued.type,
                    queued.flags,
                    queued.sequence,
                    queued.text.data(),
                    gsl::narrow<uint32_t>(queued.text.size()),
                    queued.html.data(),
                    gsl::narrow<uint32_t>(queued.html.size()),
                    queued.rtf.data(),
                    gsl::narrow<uint32_t>(queued.rtf.size()),
                    queued.value0,
                    queued.value1,
                    queued.value2,
                };

                const auto previousCallbackHandle = callbackHandle;
                callbackHandle = handle;
                try
                {
                    currentCallback(currentContext, &eventData);
                }
                CATCH_LOG()
                callbackHandle = previousCallbackHandle;

                {
                    const std::scoped_lock lock{ eventMutex };
                    --callbacksInProgress;
                    if (callbacksInProgress == 0)
                    {
                        callbackFinished.notify_all();
                    }
                }
            }
        }
    };

    std::mutex registryMutex;
    std::unordered_map<ReseshTerminalHandle, std::shared_ptr<TerminalState>> registry;
    std::atomic_uintptr_t nextHandle{ 1 };

    ReseshTerminalHandle NewHandle() noexcept
    {
        return reinterpret_cast<ReseshTerminalHandle>(nextHandle.fetch_add(1, std::memory_order_relaxed));
    }

    std::shared_ptr<TerminalState> GetState(const ReseshTerminalHandle handle)
    {
        if (!handle || callbackHandle == handle)
        {
            return {};
        }

        const std::scoped_lock lock{ registryMutex };
        const auto found = registry.find(handle);
        return found == registry.end() ? nullptr : found->second;
    }

    bool IsCompatible(const uint32_t structSize, const uint16_t major, const uint32_t requiredSize) noexcept
    {
        return major == RESESH_TERMINAL_ABI_MAJOR && structSize >= requiredSize;
    }

    template<typename Callback>
    HRESULT WithTerminal(const ReseshTerminalHandle handle, Callback&& callback)
    {
        const auto state = GetState(handle);
        RETURN_HR_IF_NULL(E_HANDLE, state);

        {
            const std::scoped_lock lock{ state->operationMutex };
            RETURN_HR_IF(E_HANDLE, state->destroying || !state->terminal);
            RETURN_IF_FAILED(callback(*state));
        }
        state->DrainEvents();
        return S_OK;
    }
}

uint32_t __stdcall ReseshTerminalGetAbiVersion(void)
{
    return RESESH_TERMINAL_ABI_VERSION;
}

HRESULT __stdcall ReseshTerminalGetBuildId(
    wchar_t* const buffer,
    const uint32_t capacity,
    uint32_t* const requiredCapacity)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, requiredCapacity);
    constexpr auto required = static_cast<uint32_t>(std::size(BuildId));
    *requiredCapacity = required;
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), !buffer || capacity < required);
    std::copy_n(BuildId, required, buffer);
    return S_OK;
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalCreate(
    const ReseshTerminalCreateOptions* const options,
    HWND* const childHwnd,
    ReseshTerminalHandle* const terminal)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, options);
    RETURN_HR_IF_NULL(E_INVALIDARG, childHwnd);
    RETURN_HR_IF_NULL(E_INVALIDARG, terminal);
    RETURN_HR_IF(E_INVALIDARG, !options->parentHwnd);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                 !IsCompatible(options->structSize, options->abiMajor, sizeof(ReseshTerminalCreateOptions)));
    RETURN_HR_IF(E_INVALIDARG,
                 options->initialColumns <= 0 || options->initialColumns > SHRT_MAX ||
                     options->initialRows <= 0 || options->initialRows > SHRT_MAX ||
                     options->historySize < 0 || options->historySize > MaximumHistorySize ||
                     options->fontSize <= 0 || options->fontSize > 512 ||
                     options->fontWeight == 0 || options->fontWeight > 999 ||
                     options->cursorStyle > 6 ||
                     options->fontFamilyLength == 0 ||
                     options->fontFamilyLength > MaximumFontFamilyCharacters ||
                     !options->fontFamily ||
                     options->wordDelimitersLength > MaximumWordDelimiterCharacters ||
                     (options->wordDelimitersLength > 0 && !options->wordDelimiters) ||
                     (options->copyFormatting & ~0x3u) != 0 ||
                     (options->pasteFiltering & ~0x3u) != 0 ||
                     (options->flags & ~0x1ffu) != 0);

    *childHwnd = nullptr;
    *terminal = nullptr;

    HwndTerminalOptions creation{};
    creation.InitialSize = { options->initialColumns, options->initialRows };
    creation.HistorySize = options->historySize;
    creation.FontFamily.assign(options->fontFamily, options->fontFamilyLength);
    creation.FontSize = options->fontSize;
    creation.FontWeight = options->fontWeight;
    creation.EnableBuiltinGlyphs = WI_IsFlagSet(options->flags, ReseshTerminalCreateEnableBuiltinGlyphs);
    creation.EnableColorGlyphs = WI_IsFlagSet(options->flags, ReseshTerminalCreateEnableColorGlyphs);
    creation.DetectUrls = WI_IsFlagSet(options->flags, ReseshTerminalCreateDetectUrls);
    creation.CopyOnSelect = WI_IsFlagSet(options->flags, ReseshTerminalCreateCopyOnSelect);
    creation.RightClickPaste = WI_IsFlagSet(options->flags, ReseshTerminalCreateRightClickPaste);
    creation.SnapOnInput = WI_IsFlagSet(options->flags, ReseshTerminalCreateSnapOnInput);
    creation.AllowOscClipboard = WI_IsFlagSet(options->flags, ReseshTerminalCreateAllowOscClipboard);
    creation.AllowOscNotifications = WI_IsFlagSet(options->flags, ReseshTerminalCreateAllowOscNotifications);
    creation.ReadOnly = WI_IsFlagSet(options->flags, ReseshTerminalCreateReadOnly);
    creation.Theme.DefaultBackground = options->defaultBackground;
    creation.Theme.DefaultForeground = options->defaultForeground;
    creation.Theme.DefaultSelectionBackground = options->selectionBackground;
    creation.Theme.CursorStyle = options->cursorStyle;
    std::copy_n(options->colorTable, std::size(creation.Theme.ColorTable), creation.Theme.ColorTable);
    creation.CursorColor = options->cursorColor;
    creation.CopyFormatting = options->copyFormatting;
    creation.PasteFiltering = options->pasteFiltering;
    if (options->wordDelimitersLength > 0)
    {
        creation.WordDelimiters.assign(options->wordDelimiters, options->wordDelimitersLength);
    }

    auto state = std::make_shared<TerminalState>();
    auto innerTerminal = std::make_unique<HwndTerminal>(options->parentHwnd, creation);
    RETURN_IF_FAILED(innerTerminal->Initialize(creation));
    const auto child = innerTerminal->GetHwnd();
    RETURN_HR_IF_NULL(E_FAIL, child);
    auto destroyOnFailure = wil::scope_exit([&]() noexcept {
        DestroyTerminal(innerTerminal.release());
    });

    state->handle = NewHandle();
    state->terminal = innerTerminal.get();
    const std::weak_ptr<TerminalState> weakState{ state };
    state->terminal->RegisterWriteCallback([weakState](const std::wstring_view input) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueInput(input);
        }
    });
    state->terminal->RegisterClipboardCallback(
        [weakState](const std::wstring_view text, const std::string_view html, const std::string_view rtf) {
            if (const auto locked = weakState.lock())
            {
                locked->QueueEvent(
                    ReseshTerminalEventTypeClipboardCopy,
                    std::wstring{ text },
                    std::string{ html },
                    std::string{ rtf });
            }
        });
    state->terminal->RegisterPasteRequestCallback([weakState]() {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeClipboardPasteRequest);
        }
    });
    state->terminal->RegisterTitleChangedCallback([weakState](const std::wstring_view title) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeTitleChanged, std::wstring{ title });
        }
    });
    state->terminal->RegisterWorkingDirectoryChangedCallback([weakState](const std::wstring_view uri) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeWorkingDirectoryChanged, std::wstring{ uri });
        }
    });
    state->terminal->RegisterBellCallback([weakState]() {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeBell);
        }
    });
    state->terminal->RegisterBufferChangedCallback([weakState](const int top, const int height, const int bottom) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeBufferOrViewportChanged, {}, {}, {}, 0, top, height, bottom);
        }
    });
    state->terminal->RegisterAlternateBufferChangedCallback([weakState](const bool enabled) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(
                ReseshTerminalEventTypeAlternateBufferChanged,
                {},
                {},
                {},
                enabled ? ReseshTerminalEventFlagEnabled : 0);
        }
    });
    state->terminal->RegisterShellIntegrationMarkCallback([weakState](const std::wstring_view command) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(ReseshTerminalEventTypeShellIntegrationMarkChanged, std::wstring{ command });
        }
    });
    state->terminal->RegisterSystemModeChangedCallback([weakState](const size_t mode, const bool enabled) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueEvent(
                ReseshTerminalEventTypeTerminalModeChanged,
                {},
                {},
                {},
                enabled ? ReseshTerminalEventFlagEnabled : 0,
                gsl::narrow_cast<int64_t>(mode));
        }
    });
    state->terminal->RegisterOscDispatchCallback([weakState](const size_t code, const std::wstring_view payload) {
        if (code <= gsl::narrow_cast<size_t>(INT64_MAX))
        {
            if (const auto locked = weakState.lock())
            {
                locked->QueueEvent(
                    ReseshTerminalEventTypeOscObserved,
                    std::wstring{ payload },
                    {},
                    {},
                    0,
                    gsl::narrow_cast<int64_t>(code));
            }
        }
    });
    state->terminal->RegisterEventDispatchCallback([weakState]() {
        if (const auto locked = weakState.lock())
        {
            locked->DrainEvents();
        }
    });

    {
        const std::scoped_lock lock{ registryMutex };
        registry.emplace(state->handle, state);
    }
    innerTerminal.release();
    destroyOnFailure.release();

    *childHwnd = static_cast<HWND>(child);
    *terminal = state->handle;
    return S_OK;
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalDestroy(const ReseshTerminalHandle terminal)
try
{
    RETURN_HR_IF(RPC_E_CANTCALLOUT_ININPUTSYNCCALL, callbackHandle == terminal);
    std::shared_ptr<TerminalState> state;
    {
        const std::scoped_lock lock{ registryMutex };
        const auto found = registry.find(terminal);
        RETURN_HR_IF(E_HANDLE, found == registry.end());
        state = std::move(found->second);
        registry.erase(found);
    }

    {
        const std::scoped_lock operationLock{ state->operationMutex };
        {
            const std::scoped_lock eventLock{ state->eventMutex };
            state->destroying = true;
            state->events.clear();
            state->queuedUnits = 0;
            state->callback = nullptr;
            state->callbackContext = nullptr;
        }
        state->terminal->RegisterWriteCallback(std::function<void(std::wstring_view)>{});
        DestroyTerminal(state->terminal);
        state->terminal = nullptr;
    }

    std::unique_lock eventLock{ state->eventMutex };
    state->callbackFinished.wait(eventLock, [&] { return state->callbacksInProgress == 0; });
    return S_OK;
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalRegisterEventCallback(
    const ReseshTerminalHandle terminal,
    const ReseshTerminalEventCallback callback,
    void* const context)
try
{
    const auto state = GetState(terminal);
    RETURN_HR_IF_NULL(E_HANDLE, state);
    {
        const std::scoped_lock lock{ state->eventMutex };
        RETURN_HR_IF(E_HANDLE, state->destroying);
        state->callback = callback;
        state->callbackContext = callback ? context : nullptr;
        if (!callback)
        {
            state->events.clear();
            state->queuedUnits = 0;
        }
    }
    state->DrainEvents();
    return S_OK;
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalSendOutput(
    const ReseshTerminalHandle terminal,
    const wchar_t* const text,
    const uint32_t textLength)
try
{
    RETURN_HR_IF(E_INVALIDARG, textLength > MaximumOutputCharacters || (textLength > 0 && !text));
    return WithTerminal(terminal, [&](TerminalState& state) {
        const std::wstring_view output = textLength == 0 ? std::wstring_view{} : std::wstring_view{ text, textLength };
        state.terminal->SendOutput(output);
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalSendKeyEvent(
    const ReseshTerminalHandle terminal,
    const uint16_t virtualKey,
    const uint16_t scanCode,
    const uint16_t flags,
    const uint8_t keyDown)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        TerminalSendKeyEvent(state.terminal, virtualKey, scanCode, flags, keyDown != 0);
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalSendCharEvent(
    const ReseshTerminalHandle terminal,
    const wchar_t character,
    const uint16_t scanCode,
    const uint16_t flags)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        TerminalSendCharEvent(state.terminal, character, scanCode, flags);
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalSetFocused(
    const ReseshTerminalHandle terminal,
    const uint8_t focused)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        if (focused)
        {
            TerminalSetFocus(state.terminal);
        }
        else
        {
            TerminalKillFocus(state.terminal);
        }
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalResizePixels(
    const ReseshTerminalHandle terminal,
    const int32_t width,
    const int32_t height,
    int32_t* const columns,
    int32_t* const rows)
try
{
    RETURN_HR_IF(E_INVALIDARG, width <= 0 || height <= 0);
    RETURN_HR_IF_NULL(E_INVALIDARG, columns);
    RETURN_HR_IF_NULL(E_INVALIDARG, rows);
    return WithTerminal(terminal, [&](TerminalState& state) {
        til::size dimensions{};
        RETURN_IF_FAILED(TerminalTriggerResize(state.terminal, width, height, &dimensions));
        *columns = dimensions.width;
        *rows = dimensions.height;
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalSetOptions(
    const ReseshTerminalHandle terminal,
    const ReseshTerminalOptions* const options)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, options);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                 !IsCompatible(options->structSize, options->abiMajor, sizeof(ReseshTerminalOptions)));
    RETURN_HR_IF(E_INVALIDARG, (options->flags & ~0x3u) != 0);
    if (WI_IsFlagSet(options->flags, ReseshTerminalOptionTheme))
    {
        RETURN_HR_IF(E_INVALIDARG,
                     options->fontFamilyLength == 0 ||
                         options->fontFamilyLength > MaximumFontFamilyCharacters ||
                         !options->fontFamily ||
                         options->fontSize <= 0 ||
                         options->cursorStyle > 6 ||
                         options->dpi <= 0);
    }
    if (WI_IsFlagSet(options->flags, ReseshTerminalOptionInteraction))
    {
        RETURN_HR_IF(E_INVALIDARG,
                     (options->interactionFlags & ~0x118u) != 0 ||
                         (options->copyFormatting & ~0x3u) != 0 ||
                         (options->pasteFiltering & ~0x3u) != 0);
    }

    return WithTerminal(terminal, [&](TerminalState& state) {
        if ((options->flags & ReseshTerminalOptionTheme) != 0)
        {
            TerminalTheme theme{};
            theme.DefaultBackground = options->defaultBackground;
            theme.DefaultForeground = options->defaultForeground;
            theme.DefaultSelectionBackground = options->defaultSelectionBackground;
            theme.CursorStyle = options->cursorStyle;
            std::copy_n(options->colorTable, std::size(theme.ColorTable), theme.ColorTable);
            const std::wstring fontFamily{ options->fontFamily, options->fontFamilyLength };
            TerminalSetTheme(state.terminal, theme, fontFamily.c_str(), options->fontSize, options->dpi);
            state.terminal->SetCursorColor(options->cursorColor);
        }
        if (WI_IsFlagSet(options->flags, ReseshTerminalOptionInteraction))
        {
            state.terminal->ApplyInteractionOptions(
                options->interactionFlags,
                options->copyFormatting,
                options->pasteFiltering);
        }
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalClearSelection(const ReseshTerminalHandle terminal)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        TerminalClearSelection(state.terminal);
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalIsSelectionActive(
    const ReseshTerminalHandle terminal,
    uint8_t* const active)
try
{
    RETURN_HR_IF_NULL(E_INVALIDARG, active);
    return WithTerminal(terminal, [&](TerminalState& state) {
        *active = TerminalIsSelectionActive(state.terminal) ? 1 : 0;
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalUserScroll(
    const ReseshTerminalHandle terminal,
    const int32_t viewTop)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        TerminalUserScroll(state.terminal, viewTop);
        return S_OK;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalCopySelection(
    const ReseshTerminalHandle terminal,
    const uint8_t clearSelection)
try
{
    return WithTerminal(terminal, [&](TerminalState& state) {
        return state.terminal->CopySelection(clearSelection != 0) ? S_OK : S_FALSE;
    });
}
CATCH_RETURN()

HRESULT __stdcall ReseshTerminalPasteText(
    const ReseshTerminalHandle terminal,
    const wchar_t* const text,
    const uint32_t textLength)
try
{
    RETURN_HR_IF(E_INVALIDARG, textLength > MaximumClipboardCharacters || (textLength > 0 && !text));
    return WithTerminal(terminal, [&](TerminalState& state) {
        state.terminal->PasteText(textLength == 0 ? std::wstring_view{} : std::wstring_view{ text, textLength });
        return S_OK;
    });
}
CATCH_RETURN()
