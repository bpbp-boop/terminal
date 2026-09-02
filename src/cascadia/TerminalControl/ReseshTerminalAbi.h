// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <stdint.h>
#include <windows.h>

#define RESESH_TERMINAL_ABI_MAJOR 1u
#define RESESH_TERMINAL_ABI_MINOR 2u
#define RESESH_TERMINAL_ABI_VERSION ((RESESH_TERMINAL_ABI_MAJOR << 16u) | RESESH_TERMINAL_ABI_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

typedef void* ReseshTerminalHandle;

typedef enum ReseshTerminalEventType
{
    ReseshTerminalEventTypeInput = 1,
    ReseshTerminalEventTypeClipboardCopy = 2,
    ReseshTerminalEventTypeClipboardPasteRequest = 3,
    ReseshTerminalEventTypeTitleChanged = 4,
    ReseshTerminalEventTypeWorkingDirectoryChanged = 5,
    ReseshTerminalEventTypeBell = 6,
    ReseshTerminalEventTypeBufferOrViewportChanged = 7,
    ReseshTerminalEventTypeAlternateBufferChanged = 8,
    ReseshTerminalEventTypeShellIntegrationMarkChanged = 9,
    ReseshTerminalEventTypeTerminalModeChanged = 10,
    ReseshTerminalEventTypeOscObserved = 11,
} ReseshTerminalEventType;

typedef enum ReseshTerminalEventFlags
{
    ReseshTerminalEventFlagEnabled = 0x00000001u,
} ReseshTerminalEventFlags;

typedef enum ReseshTerminalOptionFlags
{
    ReseshTerminalOptionTheme = 0x00000001u,
    ReseshTerminalOptionInteraction = 0x00000002u,
} ReseshTerminalOptionFlags;

typedef enum ReseshTerminalCreateFlags
{
    ReseshTerminalCreateEnableBuiltinGlyphs = 0x00000001u,
    ReseshTerminalCreateEnableColorGlyphs = 0x00000002u,
    ReseshTerminalCreateDetectUrls = 0x00000004u,
    ReseshTerminalCreateCopyOnSelect = 0x00000008u,
    ReseshTerminalCreateRightClickPaste = 0x00000010u,
    ReseshTerminalCreateSnapOnInput = 0x00000020u,
    ReseshTerminalCreateAllowOscClipboard = 0x00000040u,
    ReseshTerminalCreateAllowOscNotifications = 0x00000080u,
    ReseshTerminalCreateReadOnly = 0x00000100u,
} ReseshTerminalCreateFlags;

typedef enum ReseshTerminalCopyFormatFlags
{
    ReseshTerminalCopyFormatHtml = 0x00000001u,
    ReseshTerminalCopyFormatRtf = 0x00000002u,
} ReseshTerminalCopyFormatFlags;

typedef enum ReseshTerminalPasteFilterFlags
{
    ReseshTerminalPasteFilterCarriageReturnNewline = 0x00000001u,
    ReseshTerminalPasteFilterControlCodes = 0x00000002u,
} ReseshTerminalPasteFilterFlags;

typedef struct ReseshTerminalCreateOptions
{
    uint32_t structSize;
    uint16_t abiMajor;
    uint16_t abiMinor;
    HWND parentHwnd;
    int32_t initialColumns;
    int32_t initialRows;
    int32_t historySize;
    uint32_t flags;
    const wchar_t* fontFamily;
    uint32_t fontFamilyLength;
    int16_t fontSize;
    uint16_t fontWeight;
    uint32_t defaultBackground;
    uint32_t defaultForeground;
    uint32_t selectionBackground;
    uint32_t cursorColor;
    uint32_t cursorStyle;
    uint32_t colorTable[16];
    uint32_t copyFormatting;
    uint32_t pasteFiltering;
    const wchar_t* wordDelimiters;
    uint32_t wordDelimitersLength;
} ReseshTerminalCreateOptions;

typedef struct ReseshTerminalEvent
{
    uint32_t structSize;
    uint16_t abiMajor;
    uint16_t abiMinor;
    uint32_t type;
    uint32_t flags;
    uint64_t sequence;
    const wchar_t* text;
    uint32_t textLength;
    const char* html;
    uint32_t htmlLength;
    const char* rtf;
    uint32_t rtfLength;
    int64_t value0;
    int64_t value1;
    int64_t value2;
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
    uint32_t cursorColor;
    uint32_t cursorStyle;
    uint32_t colorTable[16];
    const wchar_t* fontFamily;
    uint32_t fontFamilyLength;
    int16_t fontSize;
    uint16_t reserved;
    int32_t dpi;
    uint32_t interactionFlags;
    uint32_t copyFormatting;
    uint32_t pasteFiltering;
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
__declspec(dllexport) HRESULT __stdcall ReseshTerminalCopySelection(
    ReseshTerminalHandle terminal,
    uint8_t clearSelection);
__declspec(dllexport) HRESULT __stdcall ReseshTerminalPasteText(
    ReseshTerminalHandle terminal,
    const wchar_t* text,
    uint32_t textLength);

#ifdef __cplusplus
}
#endif
