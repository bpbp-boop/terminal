// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalControl/ReseshTerminalAbi.h"

using namespace WEX::TestExecution;

namespace ControlUnitTests
{
    class ReseshTerminalAbiTests
    {
        BEGIN_TEST_CLASS(ReseshTerminalAbiTests)
            TEST_CLASS_PROPERTY(L"TestTimeout", L"0:0:10")
        END_TEST_CLASS()

        TEST_METHOD(ReportsVersionAndBuildId);
        TEST_METHOD(RejectsInvalidCreationStructures);
    };

    void ReseshTerminalAbiTests::ReportsVersionAndBuildId()
    {
        VERIFY_ARE_EQUAL(RESESH_TERMINAL_ABI_VERSION, ReseshTerminalGetAbiVersion());

        uint32_t required{};
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
            ReseshTerminalGetBuildId(nullptr, 0, &required));
        VERIFY_IS_GREATER_THAN(required, 1u);

        std::wstring buildId(required, L'\0');
        uint32_t written{};
        VERIFY_SUCCEEDED(ReseshTerminalGetBuildId(buildId.data(), required, &written));
        VERIFY_ARE_EQUAL(required, written);
        VERIFY_IS_TRUE(buildId.starts_with(L"terminal-v1.24.11911.0-resesh-abi1"));
    }

    void ReseshTerminalAbiTests::RejectsInvalidCreationStructures()
    {
        HWND child{};
        ReseshTerminalHandle terminal{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, ReseshTerminalCreate(nullptr, &child, &terminal));

        ReseshTerminalCreateOptions options{
            sizeof(ReseshTerminalCreateOptions),
            static_cast<uint16_t>(RESESH_TERMINAL_ABI_MAJOR + 1),
            RESESH_TERMINAL_ABI_MINOR,
            reinterpret_cast<HWND>(1),
        };
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
            ReseshTerminalCreate(&options, &child, &terminal));

        options.abiMajor = RESESH_TERMINAL_ABI_MAJOR;
        options.structSize = sizeof(ReseshTerminalCreateOptions) - 1;
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
            ReseshTerminalCreate(&options, &child, &terminal));
    }
}
