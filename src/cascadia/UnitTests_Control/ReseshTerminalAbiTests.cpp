// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalControl/ReseshTerminalAbi.h"

using namespace WEX::TestExecution;

namespace
{
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
}

namespace ControlUnitTests
{
    class ReseshTerminalAbiTests
    {
        BEGIN_TEST_CLASS(ReseshTerminalAbiTests)
            TEST_CLASS_PROPERTY(L"TestTimeout", L"0:0:10")
        END_TEST_CLASS()

        TEST_METHOD(ReportsVersionBuildIdAndRequiredExports);
        TEST_METHOD(RejectsInvalidCreationStructures);
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
        VERIFY_IS_TRUE(buildId.starts_with(L"terminal-v1.24.11911.0-resesh-abi1"));
    }

    void ReseshTerminalAbiTests::RejectsInvalidCreationStructures()
    {
        const auto module = LoadAbiModule();
        const auto closeModule = wil::scope_exit([&]() noexcept { FreeLibrary(module); });
        const auto create = LoadExport<decltype(&ReseshTerminalCreate)>(module, "ReseshTerminalCreate");

        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, create(nullptr, &child, &terminal));

        ReseshTerminalCreateOptions options{
            sizeof(ReseshTerminalCreateOptions),
            static_cast<uint16_t>(RESESH_TERMINAL_ABI_MAJOR + 1),
            RESESH_TERMINAL_ABI_MINOR,
            reinterpret_cast<HWND>(1),
        };
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
            create(&options, &child, &terminal));

        options.abiMajor = RESESH_TERMINAL_ABI_MAJOR;
        options.structSize = sizeof(ReseshTerminalCreateOptions) - 1;
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
            create(&options, &child, &terminal));
    }
}
