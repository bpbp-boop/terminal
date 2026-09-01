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
    };

    struct EventLog
    {
        std::mutex mutex;
        std::vector<CapturedEvent> events;

        std::vector<CapturedEvent> Snapshot()
        {
            const std::scoped_lock lock{ mutex };
            return events;
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
        };
        auto& log = *static_cast<EventLog*>(context);
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
        TEST_METHOD(FiltersNormalAndBracketedPaste);
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
        VERIFY_IS_TRUE(buildId.starts_with(L"terminal-v1.24.11911.0-resesh-abi1.1"));
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
}
