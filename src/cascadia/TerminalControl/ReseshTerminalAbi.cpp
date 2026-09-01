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
    constexpr wchar_t BuildId[]{ L"terminal-v1.24.11911.0-resesh-abi1" };
    constexpr size_t MaximumQueuedEventCharacters = 512 * 1024;
    constexpr uint32_t MaximumOutputCharacters = 16 * 1024 * 1024;
    constexpr uint32_t MaximumFontFamilyCharacters = 1024;

    struct QueuedEvent
    {
        uint64_t sequence;
        std::wstring text;
    };

    struct TerminalState
    {
        ReseshTerminalHandle handle{};
        HwndTerminal* terminal{};
        std::mutex operationMutex;
        std::mutex eventMutex;
        std::condition_variable callbackFinished;
        std::deque<QueuedEvent> events;
        size_t queuedCharacters{};
        uint64_t nextSequence{ 1 };
        ReseshTerminalEventCallback callback{};
        void* callbackContext{};
        uint32_t callbacksInProgress{};
        bool destroying{};

        void QueueInput(const std::wstring_view input)
        {
            if (input.empty())
            {
                return;
            }

            const std::scoped_lock lock{ eventMutex };
            if (destroying || input.size() > MaximumQueuedEventCharacters ||
                queuedCharacters > MaximumQueuedEventCharacters - input.size())
            {
                return;
            }

            queuedCharacters += input.size();
            events.emplace_back(QueuedEvent{ nextSequence++, std::wstring{ input } });
        }

        void DrainEvents()
        {
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
                    queuedCharacters -= queued.text.size();
                    currentCallback = callback;
                    currentContext = callbackContext;
                    ++callbacksInProgress;
                }

                const ReseshTerminalEvent eventData{
                    sizeof(ReseshTerminalEvent),
                    RESESH_TERMINAL_ABI_MAJOR,
                    RESESH_TERMINAL_ABI_MINOR,
                    ReseshTerminalEventTypeInput,
                    0,
                    queued.sequence,
                    queued.text.data(),
                    gsl::narrow<uint32_t>(queued.text.size()),
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

    *childHwnd = nullptr;
    *terminal = nullptr;

    auto state = std::make_shared<TerminalState>();
    void* inner{};
    void* child{};
    RETURN_IF_FAILED(CreateTerminal(options->parentHwnd, &child, &inner));

    state->handle = NewHandle();
    state->terminal = static_cast<HwndTerminal*>(inner);
    const std::weak_ptr<TerminalState> weakState{ state };
    state->terminal->RegisterWriteCallback([weakState](const std::wstring_view input) {
        if (const auto locked = weakState.lock())
        {
            locked->QueueInput(input);
        }
    });

    {
        const std::scoped_lock lock{ registryMutex };
        registry.emplace(state->handle, state);
    }

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
            state->queuedCharacters = 0;
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
            state->queuedCharacters = 0;
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
        state.terminal->SendOutput(std::wstring_view{ text, textLength });
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
    RETURN_HR_IF(E_INVALIDARG,
                 options->fontFamilyLength > MaximumFontFamilyCharacters ||
                     (options->fontFamilyLength > 0 && !options->fontFamily));
    RETURN_HR_IF(E_INVALIDARG, options->fontSize <= 0 || options->dpi <= 0);

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
