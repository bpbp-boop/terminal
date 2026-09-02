// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalControl/ReseshTerminalAbi.h"

using namespace WEX::TestExecution;

namespace
{
    constexpr std::array<uint32_t, 16> CampbellColors{
        0x000C0C0C, 0x001F0FC5, 0x000EA113, 0x00009CC1,
        0x00DA3700, 0x00981788, 0x00DD963A, 0x00CCCCCC,
        0x00767676, 0x005648E7, 0x000CC616, 0x00A5F1F9,
        0x00FF783B, 0x009E00B4, 0x00D6D661, 0x00F2F2F2,
    };
    constexpr wchar_t DefaultFont[]{ L"Cascadia Mono" };
    constexpr wchar_t DefaultWordDelimiters[]{ L" ./\\()\"'-:,.;<>~!@#$%^&*|+=[]{}~?\u2502" };
    constexpr uint32_t DefaultCreateFlags =
        ReseshTerminalCreateEnableBuiltinGlyphs |
        ReseshTerminalCreateEnableColorGlyphs |
        ReseshTerminalCreateDetectUrls |
        ReseshTerminalCreateRightClickPaste |
        ReseshTerminalCreateSnapOnInput;

    HMODULE LoadAbiModule()
    {
        std::array<wchar_t, 32768> path{};
        const auto length = GetEnvironmentVariableW(L"RESESH_TEST_DLL", path.data(), gsl::narrow<DWORD>(path.size()));
        VERIFY_IS_TRUE(length > 0 && length < path.size());
        const auto module = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        VERIFY_IS_NOT_NULL(module);
        return module;
    }

    template<typename T>
    T LoadExport(const HMODULE module, const char* const name)
    {
        const auto address = GetProcAddress(module, name);
        VERIFY_IS_NOT_NULL(address);
        return reinterpret_cast<T>(address);
    }

    HWND CreateParentWindow()
    {
        const auto parent = CreateWindowExW(
            0, L"STATIC", L"", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        VERIFY_IS_NOT_NULL(parent);
        return parent;
    }

    ReseshTerminalCreateOptions DefaultOptions(const HWND parent, const int32_t historySize = 10000)
    {
        ReseshTerminalCreateOptions options{};
        options.structSize = sizeof(options);
        options.abiMajor = RESESH_TERMINAL_ABI_MAJOR;
        options.abiMinor = RESESH_TERMINAL_ABI_MINOR;
        options.parentHwnd = parent;
        options.initialColumns = 24;
        options.initialRows = 6;
        options.historySize = historySize;
        options.flags = DefaultCreateFlags;
        options.fontFamily = DefaultFont;
        options.fontFamilyLength = gsl::narrow<uint32_t>(std::size(DefaultFont) - 1);
        options.fontSize = 14;
        options.fontWeight = 400;
        options.defaultBackground = 0x000C0C0C;
        options.defaultForeground = 0x00CCCCCC;
        options.selectionBackground = 0x00784F26;
        options.cursorColor = 0x00F2F2F2;
        options.cursorStyle = 5;
        std::copy(CampbellColors.begin(), CampbellColors.end(), options.colorTable);
        options.copyFormatting = ReseshTerminalCopyFormatHtml | ReseshTerminalCopyFormatRtf;
        options.pasteFiltering = ReseshTerminalPasteFilterCarriageReturnNewline | ReseshTerminalPasteFilterControlCodes;
        options.wordDelimiters = DefaultWordDelimiters;
        options.wordDelimitersLength = gsl::narrow<uint32_t>(std::size(DefaultWordDelimiters) - 1);
        return options;
    }

    struct CapturedEvent
    {
        uint32_t type{};
        uint32_t flags{};
        uint64_t sequence{};
        std::wstring text;
        std::string html;
        std::string rtf;
        int64_t value0{};
        int64_t value1{};
        int64_t value2{};
    };

    struct EventLog
    {
        std::mutex mutex;
        std::vector<CapturedEvent> events;
        ReseshTerminalHandle reentryTerminal{};
        decltype(&ReseshTerminalSendOutput) reentrySendOutput{};
        std::optional<HRESULT> reentryResult;

        std::vector<CapturedEvent> Snapshot()
        {
            const std::scoped_lock lock{ mutex };
            return events;
        }

        std::optional<HRESULT> ReentryResult()
        {
            const std::scoped_lock lock{ mutex };
            return reentryResult;
        }
    };

    void __stdcall CaptureCallback(void* const context, const ReseshTerminalEvent* const eventData)
    try
    {
        if (!context || !eventData || eventData->structSize < sizeof(ReseshTerminalEvent))
        {
            return;
        }

        CapturedEvent captured{
            eventData->type,
            eventData->flags,
            eventData->sequence,
            eventData->textLength == 0 ? std::wstring{} : std::wstring{ eventData->text, eventData->textLength },
            eventData->htmlLength == 0 ? std::string{} : std::string{ eventData->html, eventData->htmlLength },
            eventData->rtfLength == 0 ? std::string{} : std::string{ eventData->rtf, eventData->rtfLength },
            eventData->value0,
            eventData->value1,
            eventData->value2,
        };
        auto& log = *static_cast<EventLog*>(context);
        if (log.reentryTerminal && log.reentrySendOutput && !log.reentryResult)
        {
            log.reentryResult = log.reentrySendOutput(log.reentryTerminal, L"x", 1);
        }
        const std::scoped_lock lock{ log.mutex };
        log.events.emplace_back(std::move(captured));
    }
    catch (...)
    {
    }

    void PumpPostedMessages()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void SelectVisibleText(const HWND child)
    {
        SendMessageW(child, WM_LBUTTONDOWN, 0, MAKELPARAM(2, 2));
        SendMessageW(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(300, 70));
    }

    ReseshTerminalOptions InteractionOptions(
        const uint32_t interactionFlags,
        const uint32_t copyFormatting = ReseshTerminalCopyFormatHtml | ReseshTerminalCopyFormatRtf,
        const uint32_t pasteFiltering = ReseshTerminalPasteFilterCarriageReturnNewline | ReseshTerminalPasteFilterControlCodes)
    {
        ReseshTerminalOptions options{};
        options.structSize = sizeof(options);
        options.abiMajor = RESESH_TERMINAL_ABI_MAJOR;
        options.abiMinor = RESESH_TERMINAL_ABI_MINOR;
        options.flags = ReseshTerminalOptionInteraction;
        options.interactionFlags = interactionFlags;
        options.copyFormatting = copyFormatting;
        options.pasteFiltering = pasteFiltering;
        return options;
    }
}

namespace ControlUnitTests
{
    class ReseshTerminalAbiTests
    {
        BEGIN_TEST_CLASS(ReseshTerminalAbiTests)
            TEST_CLASS_PROPERTY(L"TestTimeout", L"0:2:0")
        END_TEST_CLASS()

        TEST_METHOD(ReportsVersionBuildIdAndRequiredExports);
        TEST_METHOD(RejectsInvalidCreationStructures);
        TEST_METHOD(CreatesConfiguredHistorySizes);
        TEST_METHOD(CreatesAndDestroys100Terminals);
        TEST_METHOD(CopiesSelectionWithConfiguredFormats);
        TEST_METHOD(HonorsCopyOnSelectAndRightClickSettings);
        TEST_METHOD(HonorsOscClipboardPolicy);
        TEST_METHOD(FiltersNormalAndBracketedPaste);
        TEST_METHOD(EmitsTypedEventsAndObservesOscWithoutConsuming);
    };

    void ReseshTerminalAbiTests::ReportsVersionBuildIdAndRequiredExports()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });

        constexpr std::array requiredExports{
            "ReseshTerminalGetAbiVersion",
            "ReseshTerminalGetBuildId",
            "ReseshTerminalCreate",
            "ReseshTerminalDestroy",
            "ReseshTerminalRegisterEventCallback",
            "ReseshTerminalSendOutput",
            "ReseshTerminalSendKeyEvent",
            "ReseshTerminalSendCharEvent",
            "ReseshTerminalSetFocused",
            "ReseshTerminalResizePixels",
            "ReseshTerminalSetOptions",
            "ReseshTerminalClearSelection",
            "ReseshTerminalIsSelectionActive",
            "ReseshTerminalUserScroll",
            "ReseshTerminalCopySelection",
            "ReseshTerminalPasteText",
        };
        for (const auto name : requiredExports)
        {
            VERIFY_IS_NOT_NULL(GetProcAddress(module, name));
        }

        const auto getAbiVersion = LoadExport<decltype(&ReseshTerminalGetAbiVersion)>(
            module, "ReseshTerminalGetAbiVersion");
        const auto getBuildId = LoadExport<decltype(&ReseshTerminalGetBuildId)>(
            module, "ReseshTerminalGetBuildId");
        VERIFY_ARE_EQUAL(RESESH_TERMINAL_ABI_VERSION, getAbiVersion());

        uint32_t required{};
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
            getBuildId(nullptr, 0, &required));
        VERIFY_IS_GREATER_THAN(required, 1u);

        std::wstring buildId(required, L'\0');
        uint32_t written{};
        VERIFY_SUCCEEDED(getBuildId(buildId.data(), required, &written));
        VERIFY_ARE_EQUAL(required, written);
        VERIFY_IS_TRUE(buildId.starts_with(L"terminal-v1.24.11911.0-resesh-abi1.2"));
    }

    void ReseshTerminalAbiTests::RejectsInvalidCreationStructures()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, create(nullptr, &child, &terminal));

        const auto expectRevisionMismatch = [&](ReseshTerminalCreateOptions options) {
            child = nullptr;
            terminal = nullptr;
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), create(&options, &child, &terminal));
            VERIFY_IS_NULL(child);
            VERIFY_IS_NULL(terminal);
        };
        auto options = DefaultOptions(parent);
        options.abiMajor = static_cast<uint16_t>(RESESH_TERMINAL_ABI_MAJOR + 1);
        expectRevisionMismatch(options);
        options = DefaultOptions(parent);
        options.structSize = sizeof(options) - 1;
        expectRevisionMismatch(options);

        const auto expectInvalid = [&](ReseshTerminalCreateOptions invalid) {
            child = nullptr;
            terminal = nullptr;
            VERIFY_ARE_EQUAL(E_INVALIDARG, create(&invalid, &child, &terminal));
            VERIFY_IS_NULL(child);
            VERIFY_IS_NULL(terminal);
        };
        options = DefaultOptions(parent);
        options.initialColumns = 0;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.initialRows = 0;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.historySize = -1;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.historySize = 1'000'001;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.fontSize = 0;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.flags |= 0x80000000u;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.copyFormatting = 4;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.pasteFiltering = 4;
        expectInvalid(options);
        options = DefaultOptions(parent);
        options.wordDelimitersLength = 4097;
        expectInvalid(options);
    }

    void ReseshTerminalAbiTests::CreatesConfiguredHistorySizes()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });

        constexpr std::array<int32_t, 5> historySizes{ 0, 1, 10000, 100000, 1000000 };
        for (const auto historySize : historySizes)
        {
            auto options = DefaultOptions(parent, historySize);
            HWND child{};
            ReseshTerminalHandle terminal{};
            VERIFY_SUCCEEDED(create(&options, &child, &terminal));
            VERIFY_IS_TRUE(IsWindow(child));
            VERIFY_SUCCEEDED(destroy(terminal));
        }
    }

    void ReseshTerminalAbiTests::CreatesAndDestroys100Terminals()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(
            module, "ReseshTerminalRegisterEventCallback");
        const auto sendCharacter = LoadExport<decltype(&ReseshTerminalSendCharEvent)>(
            module, "ReseshTerminalSendCharEvent");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        const auto options = DefaultOptions(parent);

        for (auto index = 0; index < 100; ++index)
        {
            HWND child{};
            ReseshTerminalHandle terminal{};
            VERIFY_SUCCEEDED(create(&options, &child, &terminal));
            VERIFY_IS_TRUE(IsWindow(child));

            EventLog events;
            VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));
            VERIFY_SUCCEEDED(sendCharacter(terminal, L'x', 0, 0));
            VERIFY_SUCCEEDED(destroy(terminal));
            const auto countAfterDestroy = events.Snapshot().size();
            VERIFY_ARE_EQUAL(E_HANDLE, sendCharacter(terminal, L'y', 0, 0));
            VERIFY_ARE_EQUAL(countAfterDestroy, events.Snapshot().size());
            VERIFY_IS_FALSE(IsWindow(child));
        }
    }

    void ReseshTerminalAbiTests::CopiesSelectionWithConfiguredFormats()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(module, "ReseshTerminalRegisterEventCallback");
        const auto sendOutput = LoadExport<decltype(&ReseshTerminalSendOutput)>(module, "ReseshTerminalSendOutput");
        const auto copySelection = LoadExport<decltype(&ReseshTerminalCopySelection)>(module, "ReseshTerminalCopySelection");
        const auto isSelectionActive = LoadExport<decltype(&ReseshTerminalIsSelectionActive)>(module, "ReseshTerminalIsSelectionActive");
        const auto setOptions = LoadExport<decltype(&ReseshTerminalSetOptions)>(module, "ReseshTerminalSetOptions");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        auto options = DefaultOptions(parent);
        options.initialColumns = 10;

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_SUCCEEDED(create(&options, &child, &terminal));
        const auto closeTerminal = wil::scope_exit([&]() noexcept { LOG_IF_FAILED(destroy(terminal)); });
        EventLog events;
        VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));

        constexpr wchar_t text[]{ L"wrapped-wide-\u754c-combining-e\u0301-finish" };
        VERIFY_SUCCEEDED(sendOutput(terminal, text, gsl::narrow<uint32_t>(std::size(text) - 1)));
        SelectVisibleText(child);
        uint8_t active{};
        VERIFY_SUCCEEDED(isSelectionActive(terminal, &active));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(1), active);
        VERIFY_SUCCEEDED(copySelection(terminal, 0));

        auto captured = events.Snapshot();
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), captured.size());
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(ReseshTerminalEventTypeClipboardCopy), captured[0].type);
        VERIFY_IS_TRUE(captured[0].text.find(L"wrapped") != std::wstring::npos);
        VERIFY_IS_TRUE(captured[0].text.find(L'\u754c') != std::wstring::npos);
        VERIFY_IS_TRUE(captured[0].text.find(L"e\u0301") != std::wstring::npos);
        VERIFY_IS_FALSE(captured[0].html.empty());
        VERIFY_IS_FALSE(captured[0].rtf.empty());
        VERIFY_SUCCEEDED(isSelectionActive(terminal, &active));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(1), active);

        auto interaction = InteractionOptions(ReseshTerminalCreateRightClickPaste, 0, 0);
        VERIFY_SUCCEEDED(setOptions(terminal, &interaction));
        VERIFY_SUCCEEDED(copySelection(terminal, 1));
        captured = events.Snapshot();
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), captured.size());
        VERIFY_IS_TRUE(captured[1].html.empty());
        VERIFY_IS_TRUE(captured[1].rtf.empty());
        VERIFY_SUCCEEDED(isSelectionActive(terminal, &active));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(0), active);
    }

    void ReseshTerminalAbiTests::HonorsCopyOnSelectAndRightClickSettings()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(module, "ReseshTerminalRegisterEventCallback");
        const auto sendOutput = LoadExport<decltype(&ReseshTerminalSendOutput)>(module, "ReseshTerminalSendOutput");
        const auto setOptions = LoadExport<decltype(&ReseshTerminalSetOptions)>(module, "ReseshTerminalSetOptions");
        const auto clearSelection = LoadExport<decltype(&ReseshTerminalClearSelection)>(module, "ReseshTerminalClearSelection");
        const auto isSelectionActive = LoadExport<decltype(&ReseshTerminalIsSelectionActive)>(module, "ReseshTerminalIsSelectionActive");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        auto options = DefaultOptions(parent);
        options.flags |= ReseshTerminalCreateCopyOnSelect;

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_SUCCEEDED(create(&options, &child, &terminal));
        const auto closeTerminal = wil::scope_exit([&]() noexcept { LOG_IF_FAILED(destroy(terminal)); });
        EventLog events;
        VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));
        constexpr wchar_t text[]{ L"copy-on-select matrix" };
        VERIFY_SUCCEEDED(sendOutput(terminal, text, gsl::narrow<uint32_t>(std::size(text) - 1)));

        SelectVisibleText(child);
        SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(300, 70));
        PumpPostedMessages();
        auto captured = events.Snapshot();
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), captured.size());
        uint8_t active{};
        VERIFY_SUCCEEDED(isSelectionActive(terminal, &active));
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(1), active);

        VERIFY_SUCCEEDED(clearSelection(terminal));
        auto interaction = InteractionOptions(ReseshTerminalCreateRightClickPaste);
        VERIFY_SUCCEEDED(setOptions(terminal, &interaction));
        SelectVisibleText(child);
        SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(300, 70));
        PumpPostedMessages();
        VERIFY_ARE_EQUAL(captured.size(), events.Snapshot().size());

        VERIFY_SUCCEEDED(clearSelection(terminal));
        SendMessageW(child, WM_RBUTTONDOWN, 0, MAKELPARAM(2, 2));
        PumpPostedMessages();
        captured = events.Snapshot();
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(ReseshTerminalEventTypeClipboardPasteRequest), captured.back().type);
        const auto countWithRightClick = captured.size();

        interaction = InteractionOptions(0);
        VERIFY_SUCCEEDED(setOptions(terminal, &interaction));
        SendMessageW(child, WM_RBUTTONDOWN, 0, MAKELPARAM(2, 2));
        PumpPostedMessages();
        VERIFY_ARE_EQUAL(countWithRightClick, events.Snapshot().size());
    }

    void ReseshTerminalAbiTests::HonorsOscClipboardPolicy()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(module, "ReseshTerminalRegisterEventCallback");
        const auto sendOutput = LoadExport<decltype(&ReseshTerminalSendOutput)>(module, "ReseshTerminalSendOutput");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        constexpr wchar_t oscClipboard[]{ L"\x1b]52;c;Y2xpcA==\x07" };

        const auto runPolicy = [&](const bool allowed) {
            auto options = DefaultOptions(parent);
            if (allowed)
            {
                options.flags |= ReseshTerminalCreateAllowOscClipboard |
                                 ReseshTerminalCreateAllowOscNotifications;
            }
            HWND child{};
            ReseshTerminalHandle terminal{};
            VERIFY_SUCCEEDED(create(&options, &child, &terminal));
            EventLog events;
            VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));
            VERIFY_SUCCEEDED(sendOutput(terminal, oscClipboard, gsl::narrow<uint32_t>(std::size(oscClipboard) - 1)));
            const auto captured = events.Snapshot();
            VERIFY_SUCCEEDED(destroy(terminal));
            return captured;
        };

        const auto blockedEvents = runPolicy(false);
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), blockedEvents.size());
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(ReseshTerminalEventTypeOscObserved), blockedEvents[0].type);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(52), blockedEvents[0].value0);
        VERIFY_ARE_EQUAL(std::wstring{ L"c;Y2xpcA==" }, blockedEvents[0].text);

        const auto allowedEvents = runPolicy(true);
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), allowedEvents.size());
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(ReseshTerminalEventTypeOscObserved), allowedEvents[0].type);
        VERIFY_ARE_EQUAL(static_cast<uint32_t>(ReseshTerminalEventTypeClipboardCopy), allowedEvents[1].type);
        VERIFY_ARE_EQUAL(std::wstring{ L"clip" }, allowedEvents[1].text);
    }

    void ReseshTerminalAbiTests::FiltersNormalAndBracketedPaste()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(module, "ReseshTerminalRegisterEventCallback");
        const auto sendOutput = LoadExport<decltype(&ReseshTerminalSendOutput)>(module, "ReseshTerminalSendOutput");
        const auto pasteText = LoadExport<decltype(&ReseshTerminalPasteText)>(module, "ReseshTerminalPasteText");
        const auto setOptions = LoadExport<decltype(&ReseshTerminalSetOptions)>(module, "ReseshTerminalSetOptions");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        const auto options = DefaultOptions(parent);

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_SUCCEEDED(create(&options, &child, &terminal));
        const auto closeTerminal = wil::scope_exit([&]() noexcept { LOG_IF_FAILED(destroy(terminal)); });
        EventLog events;
        VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));

        constexpr wchar_t filteredInput[]{ L"a\r\nb\x01" L"c" };
        VERIFY_SUCCEEDED(pasteText(terminal, filteredInput, gsl::narrow<uint32_t>(std::size(filteredInput) - 1)));
        auto captured = events.Snapshot();
        VERIFY_ARE_EQUAL(static_cast<size_t>(1), captured.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"a\rbc" }, captured.back().text);

        constexpr wchar_t enableBracketedPaste[]{ L"\x1b[?2004h" };
        VERIFY_SUCCEEDED(sendOutput(terminal, enableBracketedPaste, gsl::narrow<uint32_t>(std::size(enableBracketedPaste) - 1)));
        constexpr wchar_t bracketedInput[]{ L"z\n" };
        VERIFY_SUCCEEDED(pasteText(terminal, bracketedInput, gsl::narrow<uint32_t>(std::size(bracketedInput) - 1)));
        captured = events.Snapshot();
        VERIFY_ARE_EQUAL(std::wstring{ L"\x1b[200~z\r\x1b[201~" }, captured.back().text);

        constexpr wchar_t disableBracketedPaste[]{ L"\x1b[?2004l" };
        VERIFY_SUCCEEDED(sendOutput(terminal, disableBracketedPaste, gsl::narrow<uint32_t>(std::size(disableBracketedPaste) - 1)));
        auto interaction = InteractionOptions(ReseshTerminalCreateRightClickPaste, 0, 0);
        VERIFY_SUCCEEDED(setOptions(terminal, &interaction));
        const std::wstring unfilteredInput{ filteredInput, std::size(filteredInput) - 1 };
        VERIFY_SUCCEEDED(pasteText(terminal, filteredInput, gsl::narrow<uint32_t>(std::size(filteredInput) - 1)));
        captured = events.Snapshot();
        VERIFY_ARE_EQUAL(unfilteredInput, captured.back().text);

        const std::wstring largePaste(1024 * 1024, L'x');
        VERIFY_SUCCEEDED(pasteText(terminal, largePaste.data(), gsl::narrow<uint32_t>(largePaste.size())));
        captured = events.Snapshot();
        VERIFY_ARE_EQUAL(largePaste.size(), captured.back().text.size());
        VERIFY_ARE_EQUAL(E_INVALIDARG, pasteText(terminal, L"x", 4 * 1024 * 1024 + 1));

        const auto countBeforeReadOnly = captured.size();
        interaction = InteractionOptions(ReseshTerminalCreateReadOnly, 0, 0);
        VERIFY_SUCCEEDED(setOptions(terminal, &interaction));
        VERIFY_SUCCEEDED(pasteText(terminal, L"blocked", 7));
        VERIFY_ARE_EQUAL(countBeforeReadOnly, events.Snapshot().size());
    }
    void ReseshTerminalAbiTests::EmitsTypedEventsAndObservesOscWithoutConsuming()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");
        const auto destroy = LoadExport<decltype(&ReseshTerminalDestroy)>(module, "ReseshTerminalDestroy");
        const auto registerCallback = LoadExport<decltype(&ReseshTerminalRegisterEventCallback)>(
            module, "ReseshTerminalRegisterEventCallback");
        const auto sendOutput = LoadExport<decltype(&ReseshTerminalSendOutput)>(
            module, "ReseshTerminalSendOutput");
        const auto parent = CreateParentWindow();
        const auto closeParent = wil::scope_exit([&]() noexcept { DestroyWindow(parent); });
        const auto options = DefaultOptions(parent);

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_SUCCEEDED(create(&options, &child, &terminal));
        const auto closeTerminal = wil::scope_exit([&]() noexcept { LOG_IF_FAILED(destroy(terminal)); });
        EventLog events;
        events.reentryTerminal = terminal;
        events.reentrySendOutput = sendOutput;
        VERIFY_SUCCEEDED(registerCallback(terminal, CaptureCallback, &events));

        constexpr wchar_t chunkedOsc[]{ L"\x1b]7;file://host/tmp\x1b\\" };
        for (size_t index = 0; index + 1 < std::size(chunkedOsc) - 1; ++index)
        {
            VERIFY_SUCCEEDED(sendOutput(terminal, &chunkedOsc[index], 1));
            VERIFY_IS_TRUE(events.Snapshot().empty());
        }
        VERIFY_SUCCEEDED(sendOutput(terminal, &chunkedOsc[std::size(chunkedOsc) - 2], 1));

        constexpr wchar_t metadata[]{
            L"\x1b]2;phase3-title\x07"
            L"\x1b]9;9;\"D:/Work\"\x1b\\"
            L"\x07"
            L"\x1b]3008;start=id;type=shell;cwd=/tmp\x1b\\"
            L"\x1b]7377;agent;id=codex;state=working\x07"
            L"\x1b]9;4;1\x1b\\"
            L"\x1b]777;notify;done\x07"
            L"\x1b[?1049h\x1b[?1049l"
            L"\x1b[?2004h\x1b[?2004l"
            L"\x1b]133;A\x07PS> \x1b]133;B\x07echo phase3\x1b]133;C\x07\r\n"
            L"\x1b]133;D;0\x07"
        };
        VERIFY_SUCCEEDED(sendOutput(terminal, metadata, gsl::narrow<uint32_t>(std::size(metadata) - 1)));

        const auto captured = events.Snapshot();
        VERIFY_IS_GREATER_THAN(captured.size(), static_cast<size_t>(10));
        for (size_t index = 0; index < captured.size(); ++index)
        {
            VERIFY_ARE_EQUAL(gsl::narrow<uint64_t>(index + 1), captured[index].sequence);
        }

        const auto findEvent = [&](const uint32_t type, const int64_t value0 = INT64_MIN) {
            return std::find_if(captured.begin(), captured.end(), [&](const CapturedEvent& event) {
                return event.type == type && (value0 == INT64_MIN || event.value0 == value0);
            });
        };
        const auto osc7 = findEvent(ReseshTerminalEventTypeOscObserved, 7);
        VERIFY_IS_TRUE(osc7 != captured.end());
        VERIFY_ARE_EQUAL(std::wstring{ L"file://host/tmp" }, osc7->text);

        const auto oscTitle = findEvent(ReseshTerminalEventTypeOscObserved, 2);
        const auto title = findEvent(ReseshTerminalEventTypeTitleChanged);
        VERIFY_IS_TRUE(oscTitle != captured.end() && title != captured.end());
        VERIFY_ARE_EQUAL(std::wstring{ L"phase3-title" }, title->text);
        VERIFY_ARE_EQUAL(oscTitle->sequence + 1, title->sequence);

        const auto osc9 = std::find_if(captured.begin(), captured.end(), [](const CapturedEvent& event) {
            return event.type == ReseshTerminalEventTypeOscObserved &&
                   event.value0 == 9 &&
                   event.text == L"9;\"D:/Work\"";
        });
        const auto workingDirectory = findEvent(ReseshTerminalEventTypeWorkingDirectoryChanged);
        VERIFY_IS_TRUE(osc9 != captured.end() && workingDirectory != captured.end());
        VERIFY_ARE_EQUAL(std::wstring{ L"D:/Work" }, workingDirectory->text);
        VERIFY_ARE_EQUAL(osc9->sequence + 1, workingDirectory->sequence);

        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeBell) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeBufferOrViewportChanged) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeAlternateBufferChanged) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeTerminalModeChanged, 2) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeShellIntegrationMarkChanged) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeOscObserved, 133) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeOscObserved, 3008) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeOscObserved, 7377) != captured.end());
        VERIFY_IS_TRUE(findEvent(ReseshTerminalEventTypeOscObserved, 777) != captured.end());

        const auto countBeforeUnterminated = captured.size();
        constexpr wchar_t unterminated[]{ L"\x1b]7377;agent;state=working" };
        VERIFY_SUCCEEDED(sendOutput(terminal, unterminated, gsl::narrow<uint32_t>(std::size(unterminated) - 1)));
        VERIFY_ARE_EQUAL(countBeforeUnterminated, events.Snapshot().size());
        VERIFY_IS_TRUE(events.ReentryResult() == E_HANDLE);
    }
}
