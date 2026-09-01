// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <stdint.h>
#include <windows.h>

#define RESESH_TERMINAL_ABI_MAJOR 1u
#define RESESH_TERMINAL_ABI_MINOR 0u
#define RESESH_TERMINAL_ABI_VERSION ((RESESH_TERMINAL_ABI_MAJOR << 16u) | RESESH_TERMINAL_ABI_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ReseshTerminalHandle;

typedef enum ReseshTerminalEventType
{
    ReseshTerminalEventTypeInput = 1,
} ReseshTerminalEventType;

typedef enum ReseshTerminalOptionFlags
{
    ReseshTerminalOptionTheme = 0x00000001u,
} ReseshTerminalOptionFlags;

typedef struct ReseshTerminalCreateOptions
{
    uint32_t structSize;
    uint16_t abiMajor;
    uint16_t abiMinor;
    HWND parentHwnd;
} ReseshTerminalCreateOptions;

typedef struct ReseshTerminalEvent
{
    uint32_t structSize;
    uint16_t abiMajor;
    uint16_t abiMinor;
    uint32_t type;
    uint32_t reserved;
    uint64_t sequence;
    const wchar_t* text;
    uint32_t textLength;
} ReseshTerminalEvent;

typedef struct ReseshTerminalOptions
{
    uint32_t structSize;
    uint16_t abiMajor;
    uint16_t abiMinor;
    uint32_t flags;
    uint32_t defaultBackground;
    uint32_t defaultForeground;
    uint32_t defaultSelectionBackground;
    uint32_t cursorStyle;
    uint32_t colorTable[16];
    const wchar_t* fontFamily;
    uint32_t fontFamilyLength;
    int16_t fontSize;
    uint16_t reserved;
    int32_t dpi;
} ReseshTerminalOptions;

typedef void(__stdcall* ReseshTerminalEventCallback)(
    void* context,
    const ReseshTerminalEvent* eventData);

__declspec(dllexport) uint32_t __stdcall ReseshTerminalGetAbiVersion(void);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalGetBuildId(
    wchar_t* buffer,
    uint32_t capacity,
    uint32_t* requiredCapacity);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalCreate(
    const ReseshTerminalCreateOptions* options,
    HWND* childHwnd,
    ReseshTerminalHandle* terminal);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalDestroy(ReseshTerminalHandle terminal);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalRegisterEventCallback(
    ReseshTerminalHandle terminal,
    ReseshTerminalEventCallback callback,
    void* context);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalSendOutput(
    ReseshTerminalHandle terminal,
    const wchar_t* text,
    uint32_t textLength);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalSendKeyEvent(
    ReseshTerminalHandle terminal,
    uint16_t virtualKey,
    uint16_t scanCode,
    uint16_t flags,
    uint8_t keyDown);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalSendCharEvent(
    ReseshTerminalHandle terminal,
    wchar_t character,
    uint16_t scanCode,
    uint16_t flags);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalSetFocused(
    ReseshTerminalHandle terminal,
    uint8_t focused);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalResizePixels(
    ReseshTerminalHandle terminal,
    int32_t width,
    int32_t height,
    int32_t* columns,
    int32_t* rows);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalSetOptions(
    ReseshTerminalHandle terminal,
    const ReseshTerminalOptions* options);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalClearSelection(ReseshTerminalHandle terminal);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalIsSelectionActive(
    ReseshTerminalHandle terminal,
    uint8_t* active);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalUserScroll(
    ReseshTerminalHandle terminal,
    int32_t viewTop);

#ifdef __cplusplus
}
#endif
