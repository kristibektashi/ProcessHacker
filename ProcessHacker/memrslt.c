/*
 * Process Hacker -
 *   memory search results
 *
 * Copyright (C) 2010-2016 wj32
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <phapp.h>
#include <emenu.h>
#include <procprv.h>
#include <settings.h>
#include <memsrch.h>
#include "pcre/pcre2.h"
#include <windowsx.h>
#ifdef _M_ARM
#include <malloc.h>
#endif

#define FILTER_CONTAINS 1
#define FILTER_CONTAINS_IGNORECASE 2
#define FILTER_REGEX 3
#define FILTER_REGEX_IGNORECASE 4

typedef struct _MEMORY_RESULTS_CONTEXT
{
    HANDLE ProcessId;
    PPH_LIST Results;

    PH_LAYOUT_MANAGER LayoutManager;
} MEMORY_RESULTS_CONTEXT, *PMEMORY_RESULTS_CONTEXT;

INT_PTR CALLBACK PhpMemoryResultsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    );

static RECT MinimumSize = { -1, -1, -1, -1 };

VOID PhShowMemoryResultsDialog(
    _In_ HANDLE ProcessId,
    _In_ PPH_LIST Results
    )
{
    HWND windowHandle;
    PMEMORY_RESULTS_CONTEXT context;
    ULONG i;

    context = PhAllocate(sizeof(MEMORY_RESULTS_CONTEXT));
    context->ProcessId = ProcessId;
    context->Results = Results;

    PhReferenceObject(Results);

    for (i = 0; i < Results->Count; i++)
        PhReferenceMemoryResult(Results->Items[i]);

    windowHandle = CreateDialogParam(
        PhInstanceHandle,
        MAKEINTRESOURCE(IDD_MEMRESULTS),
        NULL,
        PhpMemoryResultsDlgProc,
        (LPARAM)context
        );
    ShowWindow(windowHandle, SW_SHOW);
}

static PPH_STRING PhpGetStringForSelectedResults(
    _In_ HWND ListViewHandle,
    _In_ PPH_LIST Results,
    _In_ BOOLEAN All
    )
{
    PH_STRING_BUILDER stringBuilder;
    ULONG i;

    PhInitializeStringBuilder(&stringBuilder, 0x100);

    for (i = 0; i < Results->Count; i++)
    {
        PPH_MEMORY_RESULT result;

        if (!All)
        {
            if (!(ListView_GetItemState(ListViewHandle, i, LVIS_SELECTED) & LVIS_SELECTED))
                continue;
        }

        result = Results->Items[i];

        PhAppendFormatStringBuilder(&stringBuilder, L"0x%Ix (%u): %s\r\n", result->Address, result->Length,
            result->Display.Buffer ? result->Display.Buffer : L"");
    }

    return PhFinalStringBuilderString(&stringBuilder);
}

static VOID FilterResults(
    _In_ HWND hwndDlg,
    _In_ PMEMORY_RESULTS_CONTEXT Context,
    _In_ ULONG Type
    )
{
    PPH_STRING selectedChoice = NULL;
    PPH_LIST results;
    pcre2_code *compiledExpression;
    pcre2_match_data *matchData;

    results = Context->Results;

    SetCursor(LoadCursor(NULL, IDC_WAIT));

    while (PhaChoiceDialog(
        hwndDlg,
        L"Filter",
        L"Enter the filter pattern:",
        NULL,
        0,
        NULL,
        PH_CHOICE_DIALOG_USER_CHOICE,
        &selectedChoice,
        NULL,
        L"MemFilterChoices"
        ))
    {
        PPH_LIST newResults = NULL;
        ULONG i;

        if (Type == FILTER_CONTAINS || Type == FILTER_CONTAINS_IGNORECASE)
        {
            newResults = PhCreateList(1024);

            if (Type == FILTER_CONTAINS)
            {
                for (i = 0; i < results->Count; i++)
                {
                    PPH_MEMORY_RESULT result = results->Items[i];

                    if (wcsstr(result->Display.Buffer, selectedChoice->Buffer))
                    {
                        PhReferenceMemoryResult(result);
                        PhAddItemList(newResults, result);
                    }
                }
            }
            else
            {
                PPH_STRING upperChoice;

                upperChoice = PhaUpperString(selectedChoice);

                for (i = 0; i < results->Count; i++)
                {
                    PPH_MEMORY_RESULT result = results->Items[i];
                    PWSTR upperDisplay;

                    upperDisplay = PhAllocateForMemorySearch(result->Display.Length + sizeof(WCHAR));
                    // Copy the null terminator as well.
                    memcpy(upperDisplay, result->Display.Buffer, result->Display.Length + sizeof(WCHAR));

                    _wcsupr(upperDisplay);

                    if (wcsstr(upperDisplay, upperChoice->Buffer))
                    {
                        PhReferenceMemoryResult(result);
                        PhAddItemList(newResults, result);
                    }

                    PhFreeForMemorySearch(upperDisplay);
                }
            }
        }
        else if (Type == FILTER_REGEX || Type == FILTER_REGEX_IGNORECASE)
        {
            int errorCode;
            PCRE2_SIZE errorOffset;

            compiledExpression = pcre2_compile(
                selectedChoice->Buffer,
                selectedChoice->Length / sizeof(WCHAR),
                (Type == FILTER_REGEX_IGNORECASE ? PCRE2_CASELESS : 0) | PCRE2_DOTALL,
                &errorCode,
                &errorOffset,
                NULL
                );

            if (!compiledExpression)
            {
                PhShowError(hwndDlg, L"Unable to compile the regular expression: \"%s\" at position %zu.",
                    PhGetStringOrDefault(PH_AUTO(PhPcre2GetErrorMessage(errorCode)), L"Unknown error"),
                    errorOffset
                    );
                continue;
            }

            matchData = pcre2_match_data_create_from_pattern(compiledExpression, NULL);

            newResults = PhCreateList(1024);

            for (i = 0; i < results->Count; i++)
            {
                PPH_MEMORY_RESULT result = results->Items[i];

                if (pcre2_match(
                    compiledExpression,
                    result->Display.Buffer,
                    result->Display.Length / sizeof(WCHAR),
                    0,
                    0,
                    matchData,
                    NULL
                    ) >= 0)
                {
                    PhReferenceMemoryResult(result);
                    PhAddItemList(newResults, result);
                }
            }

            pcre2_match_data_free(matchData);
            pcre2_code_free(compiledExpression);
        }

        if (newResults)
        {
            PhShowMemoryResultsDialog(Context->ProcessId, newResults);
            PhDereferenceMemoryResults((PPH_MEMORY_RESULT *)newResults->Items, newResults->Count);
            PhDereferenceObject(newResults);
            break;
        }
    }

    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

INT_PTR CALLBACK PhpMemoryResultsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PMEMORY_RESULTS_CONTEXT context;

    if (uMsg != WM_INITDIALOG)
    {
        context = GetProp(hwndDlg, PhMakeContextAtom());
    }
    else
    {
        context = (PMEMORY_RESULTS_CONTEXT)lParam;
        SetProp(hwndDlg, PhMakeContextAtom(), (HANDLE)context);
    }

    if (!context)
        return FALSE;

    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            HWND lvHandle;

            PhRegisterDialog(hwndDlg);

            {
                PPH_PROCESS_ITEM processItem;

                if (processItem = PhReferenceProcessItem(context->ProcessId))
                {
                    SetWindowText(hwndDlg, PhaFormatString(L"Results - %s (%u)",
                        processItem->ProcessName->Buffer, HandleToUlong(processItem->ProcessId))->Buffer);
                    PhDereferenceObject(processItem);
                }
            }

            lvHandle = GetDlgItem(hwndDlg, IDC_LIST);
            PhSetListViewStyle(lvHandle, FALSE, TRUE);
            PhSetControlTheme(lvHandle, L"explorer");
            PhAddListViewColumn(lvHandle, 0, 0, 0, LVCFMT_LEFT, 120, L"Address");
            PhAddListViewColumn(lvHandle, 1, 1, 1, LVCFMT_LEFT, 80, L"Length");
            PhAddListViewColumn(lvHandle, 2, 2, 2, LVCFMT_LEFT, 200, L"Result");

            PhLoadListViewColumnsFromSetting(L"MemResultsListViewColumns", lvHandle);

            PhInitializeLayoutManager(&context->LayoutManager, hwndDlg);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_LIST), NULL,
                PH_ANCHOR_ALL);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDOK), NULL,
                PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_COPY), NULL,
                PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_SAVE), NULL,
                PH_ANCHOR_RIGHT | PH_ANCHOR_BOTTOM);
            PhAddLayoutItem(&context->LayoutManager, GetDlgItem(hwndDlg, IDC_FILTER), NULL,
                PH_ANCHOR_BOTTOM | PH_ANCHOR_LEFT);

            if (MinimumSize.left == -1)
            {
                RECT rect;

                rect.left = 0;
                rect.top = 0;
                rect.right = 250;
                rect.bottom = 180;
                MapDialogRect(hwndDlg, &rect);
                MinimumSize = rect;
                MinimumSize.left = 0;
            }

            ListView_SetItemCount(lvHandle, context->Results->Count);

            SetDlgItemText(hwndDlg, IDC_INTRO, PhaFormatString(L"%s results.",
                PhaFormatUInt64(context->Results->Count, TRUE)->Buffer)->Buffer);

            {
                PH_RECTANGLE windowRectangle;

                windowRectangle.Position = PhGetIntegerPairSetting(L"MemResultsPosition");
                windowRectangle.Size = PhGetScalableIntegerPairSetting(L"MemResultsSize", TRUE).Pair;
                PhAdjustRectangleToWorkingArea(NULL, &windowRectangle);

                MoveWindow(hwndDlg, windowRectangle.Left, windowRectangle.Top,
                    windowRectangle.Width, windowRectangle.Height, FALSE);

                // Implement cascading by saving an offsetted rectangle.
                windowRectangle.Left += 20;
                windowRectangle.Top += 20;

                PhSetIntegerPairSetting(L"MemResultsPosition", windowRectangle.Position);
                PhSetScalableIntegerPairSetting2(L"MemResultsSize", windowRectangle.Size);
            }
        }
        break;
    case WM_DESTROY:
        {
            PhSaveWindowPlacementToSetting(L"MemResultsPosition", L"MemResultsSize", hwndDlg);
            PhSaveListViewColumnsToSetting(L"MemResultsListViewColumns", GetDlgItem(hwndDlg, IDC_LIST));

            PhDeleteLayoutManager(&context->LayoutManager);
            PhUnregisterDialog(hwndDlg);
            RemoveProp(hwndDlg, PhMakeContextAtom());

            PhDereferenceMemoryResults((PPH_MEMORY_RESULT *)context->Results->Items, context->Results->Count);
            PhDereferenceObject(context->Results);
            PhFree(context);
        }
        break;
    case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDCANCEL:
            case IDOK:
                DestroyWindow(hwndDlg);
                break;
            case IDC_COPY:
                {
                    HWND lvHandle;
                    PPH_STRING string;
                    ULONG selectedCount;

                    lvHandle = GetDlgItem(hwndDlg, IDC_LIST);
                    selectedCount = ListView_GetSelectedCount(lvHandle);

                    if (selectedCount == 0)
                    {
                        // User didn't select anything, so copy all items.
                        string = PhpGetStringForSelectedResults(lvHandle, context->Results, TRUE);
                        PhSetStateAllListViewItems(lvHandle, LVIS_SELECTED, LVIS_SELECTED);
                    }
                    else
                    {
                        string = PhpGetStringForSelectedResults(lvHandle, context->Results, FALSE);
                    }

                    PhSetClipboardString(hwndDlg, &string->sr);
                    PhDereferenceObject(string);

                    SendMessage(hwndDlg, WM_NEXTDLGCTL, (WPARAM)lvHandle, TRUE);
                }
                break;
            case IDC_SAVE:
                {
                    static PH_FILETYPE_FILTER filters[] =
                    {
                        { L"Text files (*.txt)", L"*.txt" },
                        { L"All files (*.*)", L"*.*" }
                    };
                    PVOID fileDialog;

                    fileDialog = PhCreateSaveFileDialog();

                    PhSetFileDialogFilter(fileDialog, filters, sizeof(filters) / sizeof(PH_FILETYPE_FILTER));
                    PhSetFileDialogFileName(fileDialog, L"Search results.txt");

                    if (PhShowFileDialog(hwndDlg, fileDialog))
                    {
                        NTSTATUS status;
                        PPH_STRING fileName;
                        PPH_FILE_STREAM fileStream;
                        PPH_STRING string;

                        fileName = PH_AUTO(PhGetFileDialogFileName(fileDialog));

                        if (NT_SUCCESS(status = PhCreateFileStream(
                            &fileStream,
                            fileName->Buffer,
                            FILE_GENERIC_WRITE,
                            FILE_SHARE_READ,
                            FILE_OVERWRITE_IF,
                            0
                            )))
                        {
                            PhWriteStringAsUtf8FileStream(fileStream, &PhUnicodeByteOrderMark);
                            PhWritePhTextHeader(fileStream);

                            string = PhpGetStringForSelectedResults(GetDlgItem(hwndDlg, IDC_LIST), context->Results, TRUE);
                            PhWriteStringAsUtf8FileStreamEx(fileStream, string->Buffer, string->Length);
                            PhDereferenceObject(string);

                            PhDereferenceObject(fileStream);
                        }

                        if (!NT_SUCCESS(status))
                            PhShowStatus(hwndDlg, L"Unable to create the file", status, 0);
                    }

                    PhFreeFileDialog(fileDialog);
                }
                break;
            case IDC_FILTER:
                {
                    PPH_EMENU menu;
                    RECT buttonRect;
                    POINT point;
                    PPH_EMENU_ITEM selectedItem;
                    ULONG filterType = 0;

                    menu = PhCreateEMenu();
                    PhLoadResourceEMenuItem(menu, PhInstanceHandle, MAKEINTRESOURCE(IDR_MEMFILTER), 0);

                    GetClientRect(GetDlgItem(hwndDlg, IDC_FILTER), &buttonRect);
                    point.x = 0;
                    point.y = buttonRect.bottom;

                    ClientToScreen(GetDlgItem(hwndDlg, IDC_FILTER), &point);
                    selectedItem = PhShowEMenu(menu, hwndDlg, PH_EMENU_SHOW_LEFTRIGHT,
                        PH_ALIGN_LEFT | PH_ALIGN_TOP, point.x, point.y);

                    if (selectedItem)
                    {
                        switch (selectedItem->Id)
                        {
                        case ID_FILTER_CONTAINS:
                            filterType = FILTER_CONTAINS;
                            break;
                        case ID_FILTER_CONTAINS_CASEINSENSITIVE:
                            filterType = FILTER_CONTAINS_IGNORECASE;
                            break;
                        case ID_FILTER_REGEX:
                            filterType = FILTER_REGEX;
                            break;
                        case ID_FILTER_REGEX_CASEINSENSITIVE:
                            filterType = FILTER_REGEX_IGNORECASE;
                            break;
                        }
                    }

                    if (filterType != 0)
                        FilterResults(hwndDlg, context, filterType);

                    PhDestroyEMenu(menu);
                }
                break;
            }
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;
            HWND lvHandle;

            lvHandle = GetDlgItem(hwndDlg, IDC_LIST);
            PhHandleListViewNotifyForCopy(lParam, lvHandle);

            switch (header->code)
            {
            case LVN_GETDISPINFO:
                {
                    NMLVDISPINFO *dispInfo = (NMLVDISPINFO *)header;

                    if (dispInfo->item.mask & LVIF_TEXT)
                    {
                        PPH_MEMORY_RESULT result = context->Results->Items[dispInfo->item.iItem];

                        switch (dispInfo->item.iSubItem)
                        {
                        case 0:
                            {
                                WCHAR addressString[PH_PTR_STR_LEN_1];

                                PhPrintPointer(addressString, result->Address);
                                wcsncpy_s(
                                    dispInfo->item.pszText,
                                    dispInfo->item.cchTextMax,
                                    addressString,
                                    _TRUNCATE
                                    );
                            }
                            break;
                        case 1:
                            {
                                WCHAR lengthString[PH_INT32_STR_LEN_1];

                                PhPrintUInt32(lengthString, (ULONG)result->Length);
                                wcsncpy_s(
                                    dispInfo->item.pszText,
                                    dispInfo->item.cchTextMax,
                                    lengthString,
                                    _TRUNCATE
                                    );
                            }
                            break;
                        case 2:
                            wcsncpy_s(
                                dispInfo->item.pszText,
                                dispInfo->item.cchTextMax,
                                result->Display.Buffer,
                                _TRUNCATE
                                );
                            break;
                        }
                    }
                }
                break;
            case NM_DBLCLK:
                {
                    if (header->hwndFrom == lvHandle)
                    {
                        INT index;

                        if ((index = ListView_GetNextItem(
                            lvHandle,
                            -1,
                            LVNI_SELECTED
                            )) != -1)
                        {
                            NTSTATUS status;
                            PPH_MEMORY_RESULT result = context->Results->Items[index];
                            HANDLE processHandle;
                            MEMORY_BASIC_INFORMATION basicInfo;
                            PPH_SHOWMEMORYEDITOR showMemoryEditor;

                            if (NT_SUCCESS(status = PhOpenProcess(
                                &processHandle,
                                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                context->ProcessId
                                )))
                            {
                                if (NT_SUCCESS(status = NtQueryVirtualMemory(
                                    processHandle,
                                    result->Address,
                                    MemoryBasicInformation,
                                    &basicInfo,
                                    sizeof(MEMORY_BASIC_INFORMATION),
                                    NULL
                                    )))
                                {
                                    showMemoryEditor = PhAllocate(sizeof(PH_SHOWMEMORYEDITOR));
                                    memset(showMemoryEditor, 0, sizeof(PH_SHOWMEMORYEDITOR));
                                    showMemoryEditor->ProcessId = context->ProcessId;
                                    showMemoryEditor->BaseAddress = basicInfo.BaseAddress;
                                    showMemoryEditor->RegionSize = basicInfo.RegionSize;
                                    showMemoryEditor->SelectOffset = (ULONG)((ULONG_PTR)result->Address - (ULONG_PTR)basicInfo.BaseAddress);
                                    showMemoryEditor->SelectLength = (ULONG)result->Length;
                                    ProcessHacker_ShowMemoryEditor(PhMainWndHandle, showMemoryEditor);
                                }

                                NtClose(processHandle);
                            }

                            if (!NT_SUCCESS(status))
                                PhShowStatus(hwndDlg, L"Unable to edit memory", status, 0);
                        }
                    }
                }
                break;
            }
        }
        break;
    case WM_SIZE:
        {
            PhLayoutManagerLayout(&context->LayoutManager);
        }
        break;
    case WM_SIZING:
        {
            PhResizingMinimumSize((PRECT)lParam, wParam, MinimumSize.right, MinimumSize.bottom);
        }
        break;
    }

    return FALSE;
}
