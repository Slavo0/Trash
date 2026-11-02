/* Standalone latest version. Build: gcc -O2 -Wall -Wextra -Wno-unused-function -s -municode -mwindows layout_switcher_150ms.c -o layout_switcher_150ms.exe -luser32 -lshell32 -lole32 -loleaut32 -lgdi32 */
/* Native tray host. Conversion engine is shared with the recovered CLI. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define wmain recovered_cli_main
/*
 * convert_restored.c -- reconstructed layout converter, English US <-> Russian (ЙЦУКЕН).
 * Replaces convert.py and keeps its CLI interface:
 *
 *   convert.exe input.txt output.txt [--direction auto|en|ru] [--fallback en|ru]
 *
 * Files are UTF-8; a UTF-8 BOM in the input is accepted.
 * On Windows compile with MinGW-w64 GCC (-municode for Unicode file paths).
 * Build: gcc -O2 -s -municode convert_restored.c -o convert_restored.exe -luser32
 */

#ifdef _WIN32
#  include <windows.h>
#endif
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#  define ENTRY_POINT wmain
#  define ARG_CHAR wchar_t
#  define ARG_LIT(x) L##x
#  define ARG_CMP wcscmp
#  define OPEN_FILE _wfopen
#  define READ_MODE L"rb"
#  define WRITE_MODE L"wb"
#else
#  define ENTRY_POINT main
#  define ARG_CHAR char
#  define ARG_LIT(x) x
#  define ARG_CMP strcmp
#  define OPEN_FILE fopen
#  define READ_MODE "rb"
#  define WRITE_MODE "wb"
#endif

enum { DIR_AUTO = 0, DIR_EN = 1, DIR_RU = 2 };

typedef struct {
    uint32_t cp;
    unsigned char layout;
} Character;

typedef struct {
    const wchar_t *en;
    const wchar_t *ru;
} KeyRow;

/* Each position represents the same physical key in the US and RU layouts. */
static const KeyRow KEY_ROWS[] = {
    {L"qwertyuiop[]",   L"йцукенгшщзхъ"},
    {L"asdfghjkl;'",    L"фывапролджэ"},
    {L"zxcvbnm,./",     L"ячсмитьбю."},
    {L"QWERTYUIOP{}",   L"ЙЦУКЕНГШЩЗХЪ"},
    {L"ASDFGHJKL:\"",   L"ФЫВАПРОЛДЖЭ"},
    {L"ZXCVBNM<>?",     L"ЯЧСМИТЬБЮ,"},
    {L"`~",             L"ёЁ"},
    {L"@#$^&|",         L"\"№;:?/"}
};

static unsigned char get_layout(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return DIR_EN;
    if ((c >= 0x0410 && c <= 0x044f) || c == 0x0401 || c == 0x0451)
        return DIR_RU;
    return DIR_AUTO;
}

static uint32_t convert_cp(uint32_t cp, int direction)
{
    size_t row;

    /* Custom alias, matching convert.py: RU '|' -> EN '/'. */
    if (direction == DIR_RU && cp == '|')
        return '/';

    for (row = 0; row < sizeof(KEY_ROWS) / sizeof(KEY_ROWS[0]); ++row) {
        const wchar_t *src = (direction == DIR_EN) ? KEY_ROWS[row].en : KEY_ROWS[row].ru;
        const wchar_t *dst = (direction == DIR_EN) ? KEY_ROWS[row].ru : KEY_ROWS[row].en;
        size_t col;
        for (col = 0; src[col] != 0; ++col) {
            if ((uint32_t)src[col] == cp)
                return (uint32_t)dst[col];
        }
    }
    return cp;
}

/* Treat a trailing comma as a question mark, ignoring trailing whitespace. */
static int trailing_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n';
}

/* Read UTF-8 without depending on Windows locale or code page. */
static int decode_utf8(const unsigned char *data, size_t size,
                       Character **characters_out, size_t *count_out)
{
    size_t pos = 0, count = 0;
    Character *characters;

    if (size >= SIZE_MAX / sizeof(*characters))
        return 0;
    characters = malloc((size + 1) * sizeof(*characters));
    if (!characters)
        return 0;

    /* Accept an optional UTF-8 byte-order mark. */
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        pos = 3;

    while (pos < size) {
        uint32_t cp;
        unsigned char lead = data[pos];
        size_t length, j;

        if (lead < 0x80) {
            cp = lead;
            length = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            cp = lead & 0x1f;
            length = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            cp = lead & 0x0f;
            length = 3;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            cp = lead & 0x07;
            length = 4;
        } else {
            free(characters);
            return 0;
        }

        if (length > size - pos) {
            free(characters);
            return 0;
        }
        for (j = 1; j < length; ++j) {
            if ((data[pos + j] & 0xc0) != 0x80) {
                free(characters);
                return 0;
            }
            cp = (cp << 6) | (data[pos + j] & 0x3f);
        }
        /* Disallow overlong encodings, surrogates and out-of-range values. */
        if ((length == 2 && cp < 0x80) ||
            (length == 3 && cp < 0x800) ||
            (length == 4 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
            free(characters);
            return 0;
        }
        characters[count].cp = cp;
        characters[count].layout = get_layout(cp);
        ++count;
        pos += length;
    }

    *characters_out = characters;
    *count_out = count;
    return 1;
}

static size_t encode_utf8(uint32_t cp, unsigned char output[4])
{
    if (cp < 0x80) {
        output[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        output[0] = (unsigned char)(0xc0 | (cp >> 6));
        output[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        output[0] = (unsigned char)(0xe0 | (cp >> 12));
        output[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        output[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    output[0] = (unsigned char)(0xf0 | (cp >> 18));
    output[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    output[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    output[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Process a complete UTF-8 buffer, preserving newlines and all other chars. */
static int process_text(const unsigned char *input, size_t input_size,
                        int direction, int fallback,
                        unsigned char **output, size_t *output_size)
{
    Character *chars = NULL;
    size_t *next_letter = NULL;
    unsigned char *buffer = NULL;
    size_t count, i, previous = SIZE_MAX, next = SIZE_MAX, written = 0;
    size_t english = 0, russian = 0, end;
    int dominant;

    if (!decode_utf8(input, input_size, &chars, &count))
        return 0;
    if (count > (SIZE_MAX - 4) / 4) {
        free(chars);
        return 0;
    }
    buffer = malloc(count * 4 + 4);
    if (!buffer) {
        free(chars);
        return 0;
    }

    end = count;
    while (end && trailing_space(chars[end - 1].cp)) --end;
    for (i = 0; i < count; ++i) {
        if (chars[i].layout == DIR_EN)
            ++english;
        else if (chars[i].layout == DIR_RU)
            ++russian;
    }
    dominant = english > russian ? DIR_EN : russian > english ? DIR_RU : fallback;

    if (direction == DIR_AUTO && english + russian > 0) {
        next_letter = malloc((count ? count : 1) * sizeof(*next_letter));
        if (!next_letter)
            goto error;
        for (i = count; i-- > 0;) {
            if (chars[i].layout)
                next = i;
            next_letter[i] = next;
        }
    }

    for (i = 0; i < count; ++i) {
        int layout = direction;
        uint32_t cp;
        unsigned char encoded[4];
        size_t bytes;

        if (direction == DIR_AUTO) {
            layout = chars[i].layout;
            if (layout == DIR_AUTO) {
                layout = dominant;
                if (next_letter) {
                    size_t right = next_letter[i];
                    if (previous != SIZE_MAX &&
                        (right == SIZE_MAX || i - previous <= right - i))
                        layout = chars[previous].layout;
                    else if (right != SIZE_MAX)
                        layout = chars[right].layout;
                }
            }
        }
        cp = i + 1 == end && chars[i].cp == ',' ? '?' : convert_cp(chars[i].cp, layout);
        bytes = encode_utf8(cp, encoded);
        memcpy(buffer + written, encoded, bytes);
        written += bytes;

        if (chars[i].layout)
            previous = i;
    }
    free(chars);
    free(next_letter);
    *output = buffer;
    *output_size = written;
    return 1;

error:
    free(chars);
    free(next_letter);
    free(buffer);
    return 0;
}

static int read_entire_file(FILE *file, unsigned char **data, size_t *size)
{
    long length;
    unsigned char *buffer;

    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0)
        return 0;

    buffer = malloc((size_t)length + 1);
    if (!buffer)
        return 0;
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        return 0;
    }
    *data = buffer;
    *size = (size_t)length;
    return 1;
}


int ENTRY_POINT(int argc, ARG_CHAR **argv)
{
    int direction = DIR_AUTO, fallback = DIR_EN;
    int i, paths = 0, ok = 0;
    const ARG_CHAR *path[2];
    FILE *in = NULL, *out = NULL;
    unsigned char *input = NULL, *converted = NULL;
    size_t input_size = 0, converted_size = 0;

    for (i = 1; i < argc; ++i) {
        if (ARG_CMP(argv[i], ARG_LIT("--direction")) == 0 && i + 1 < argc) {
            const ARG_CHAR *value = argv[++i];
            if (ARG_CMP(value, ARG_LIT("auto")) == 0) direction = DIR_AUTO;
            else if (ARG_CMP(value, ARG_LIT("en")) == 0) direction = DIR_EN;
            else if (ARG_CMP(value, ARG_LIT("ru")) == 0) direction = DIR_RU;
            else goto usage;
        } else if (ARG_CMP(argv[i], ARG_LIT("--fallback")) == 0 && i + 1 < argc) {
            const ARG_CHAR *value = argv[++i];
            if (ARG_CMP(value, ARG_LIT("en")) == 0) fallback = DIR_EN;
            else if (ARG_CMP(value, ARG_LIT("ru")) == 0) fallback = DIR_RU;
            else goto usage;
        } else {
            if (paths == 2) goto usage;
            path[paths++] = argv[i];
        }
    }
    if (paths != 2) goto usage;
    in = OPEN_FILE(path[0], READ_MODE);
    if (!in) {
        fprintf(stderr, "Could not open input file.\n");
        goto cleanup;
    }
    if (!read_entire_file(in, &input, &input_size)) {
        fprintf(stderr, "Could not read input file.\n");
        goto cleanup;
    }
    fclose(in);
    in = NULL;
    if (!process_text(input, input_size, direction, fallback, &converted, &converted_size)) {
        fprintf(stderr, "Could not convert input (invalid UTF-8 or out of memory).\n");
        goto cleanup;
    }
    out = OPEN_FILE(path[1], WRITE_MODE);
    if (!out) {
        fprintf(stderr, "Could not open output file.\n");
        goto cleanup;
    }
    if (fwrite(converted, 1, converted_size, out) != converted_size || fclose(out) != 0) {
        out = NULL;
        fprintf(stderr, "Could not write output file.\n");
        goto cleanup;
    }
    out = NULL;
    ok = 1;
cleanup:
    if (in) fclose(in);
    if (out) fclose(out);
    free(input);
    free(converted);
    return ok ? 0 : 1;
usage:
    fprintf(stderr, "Usage: convert.exe IN.txt OUT.txt [--direction auto|en|ru] [--fallback en|ru]\n");
    return 2;
}

#undef wmain
#include <shellapi.h>
#include <initguid.h>
#include <uiautomationclient.h>
#ifndef SWITCHER_CONFIG_H
#define SWITCHER_CONFIG_H
/* Change here and rebuild. MOD_CONTROL | MOD_ALT + 'C' by default. */
#define SWITCH_MODIFIERS (MOD_CONTROL | MOD_ALT)
#define SWITCH_KEY 'C'
#define SWITCH_HOTKEY_TEXT L"Ctrl+Alt+C"
#define RELEASE_KEYS_TIMEOUT_MS 2000
#define SWITCH_LAYOUT_AFTER_CONVERSION 0
#endif

/* Requires get_layout(), convert_cp() and DIR_* from the recovered engine.
 * Windows wchar_t is a UTF-16 code unit. No transcoding or scratch allocation. */
#ifndef UTF16_ENGINE_H
#define UTF16_ENGINE_H
static uint32_t read_utf16(const wchar_t *s, size_t n, size_t *at)
{
    uint32_t cp = (uint16_t)s[(*at)++];
    if (cp >= 0xd800 && cp <= 0xdbff) {
        uint32_t low;
        if (*at == n) return UINT32_MAX;
        low = (uint16_t)s[(*at)++];
        if (low < 0xdc00 || low > 0xdfff) return UINT32_MAX;
        return 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
    }
    return cp >= 0xdc00 && cp <= 0xdfff ? UINT32_MAX : cp;
}

static int transform_utf16(wchar_t *s, size_t *length, int direction, int fallback)
{
    size_t n = *length, start = n && s[0] == 0xfeff ? 1 : 0, end = n;
    size_t at = start, english = 0, russian = 0, pos = 0, written = 0;
    size_t previous = SIZE_MAX, right = SIZE_MAX, scan_at = start, scan_pos = 0;
    int previous_layout = 0, right_layout = 0, dominant;
    uint32_t cp;
    while (end > start && trailing_space(s[end - 1])) --end;
    /* Validate before mutating, matching strict UTF-16 -> UTF-8 behavior. */
    while (at < n) {
        cp = read_utf16(s, n, &at);
        if (cp == UINT32_MAX) return 0;
        if (get_layout(cp) == DIR_EN) ++english;
        else if (get_layout(cp) == DIR_RU) ++russian;
    }
    dominant = english > russian ? DIR_EN : russian > english ? DIR_RU : fallback;
    at = start;
    while (at < n) {
        int own, layout = direction;
        size_t current_at = at;
        cp = read_utf16(s, n, &at);
        own = get_layout(cp);
        if (direction == DIR_AUTO) {
            layout = own;
            if (!own) {
                if (right == SIZE_MAX || right < pos) {
                    /* Start at untouched input. Distances count Unicode scalars,
                     * not UTF-16 units, preserving the old emoji tie behavior. */
                    if (scan_at < current_at) { scan_at = current_at; scan_pos = pos; }
                    right = SIZE_MAX;
                    while (scan_at < n) {
                        uint32_t next_cp = read_utf16(s, n, &scan_at);
                        int next_layout = get_layout(next_cp);
                        size_t candidate = scan_pos++;
                        if (next_layout) { right = candidate; right_layout = next_layout; break; }
                    }
                }
                layout = dominant;
                if (previous != SIZE_MAX && (right == SIZE_MAX || pos - previous <= right - pos))
                    layout = previous_layout;
                else if (right != SIZE_MAX) layout = right_layout;
            }
        }
        cp = at == end && cp == ',' ? '?' : convert_cp(cp, layout);
        if (cp < 0x10000) s[written++] = (wchar_t)cp;
        else {
            cp -= 0x10000;
            s[written++] = (wchar_t)(0xd800 + (cp >> 10));
            s[written++] = (wchar_t)(0xdc00 + (cp & 0x3ff));
        }
        if (own) { previous = pos; previous_layout = own; }
        ++pos;
    }
    s[written] = 0;
    *length = written;
    return 1;
}
#endif


#define WM_TRAY (WM_APP + 1)
#define WM_FINISHED (WM_APP + 2)
#define ID_HOTKEY 1
#define ID_EXIT 2
static HWND host;
static HANDLE worker;
static volatile LONG busy, stopping;
static UINT taskbar_created;
static NOTIFYICONDATAW tray;

static int target_active(HWND target)
{
    return !InterlockedCompareExchange(&stopping, 0, 0) &&
           GetForegroundWindow() == target && IsWindow(target);
}

static int keys_down(void)
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
           (GetAsyncKeyState(VK_MENU) & 0x8000) ||
           (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
           (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000) ||
           (GetAsyncKeyState(SWITCH_KEY) & 0x8000);
}

/* System messages provide clipboard-free access to classic Edit controls. */
static int classic_edit(HWND edit)
{
    wchar_t name[32];
    return GetClassNameW(edit, name, 32) && !_wcsicmp(name, L"Edit");
}

static BSTR edit_selection_text(HWND edit, HWND target)
{
    DWORD start = 0, end = 0;
    DWORD_PTR result;
    wchar_t *buffer = NULL;
    BSTR text = NULL;
    size_t length;
    if ((GetWindowLongPtrW(edit, GWL_STYLE) & ES_PASSWORD) ||
        !SendMessageTimeoutW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) ||
        !SendMessageTimeoutW(edit, WM_GETTEXTLENGTH, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) || result > 16 * 1024 * 1024) return NULL;
    length = (size_t)result;
    buffer = calloc(length + 1, sizeof(wchar_t));
    if (!buffer) return NULL;
    if (!SendMessageTimeoutW(edit, WM_GETTEXT, length + 1, (LPARAM)buffer,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result)) goto done;
    length = (size_t)result;
    if (start > length || end > length || start > end) goto done;
    if (start == end) {
        start = 0;
        if (!target_active(target) || keys_down() ||
            !SendMessageTimeoutW(edit, EM_SETSEL, 0, end,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result)) goto done;
    }
    text = SysAllocStringLen(buffer + start, end - start);
done:
    free(buffer);
    return text;
}

/* Read exactly one UIA selection. An empty range means a caret: select the
 * document prefix using range endpoints, without Ctrl+C or clipboard access. */
static BSTR selection_text(IUIAutomationElement *element, HWND target)
{
    IUIAutomationTextPattern *pattern = NULL;
    IUIAutomationTextRangeArray *ranges = NULL;
    IUIAutomationTextRange *range = NULL, *prefix = NULL;
    BSTR text = NULL;
    int count = 0;
    BOOL password = TRUE;
    if (FAILED(IUIAutomationElement_get_CurrentIsPassword(element, &password)) || password) return NULL;
    if (FAILED(IUIAutomationElement_GetCurrentPatternAs(element, UIA_TextPatternId,
            &IID_IUIAutomationTextPattern, (void **)&pattern)) || !pattern) goto done;
    if (FAILED(IUIAutomationTextPattern_GetSelection(pattern, &ranges)) || !ranges ||
        FAILED(IUIAutomationTextRangeArray_get_Length(ranges, &count)) || count != 1 ||
        FAILED(IUIAutomationTextRangeArray_GetElement(ranges, 0, &range)) || !range ||
        FAILED(IUIAutomationTextRange_GetText(range, -1, &text)) || !text) goto done;
    if (!SysStringLen(text)) {
        SysFreeString(text);
        text = NULL;
        if (!target_active(target) || keys_down() ||
            FAILED(IUIAutomationTextPattern_get_DocumentRange(pattern, &prefix)) || !prefix ||
            FAILED(IUIAutomationTextRange_MoveEndpointByRange(prefix,
                TextPatternRangeEndpoint_End, range, TextPatternRangeEndpoint_Start)) ||
            FAILED(IUIAutomationTextRange_GetText(prefix, -1, &text)) || !text) goto done;
        if (SysStringLen(text) && (!target_active(target) || keys_down() ||
            FAILED(IUIAutomationTextRange_Select(prefix)))) {
            SysFreeString(text);
            text = NULL;
        }
    }
done:
    if (prefix) IUIAutomationTextRange_Release(prefix);
    if (range) IUIAutomationTextRange_Release(range);
    if (ranges) IUIAutomationTextRangeArray_Release(ranges);
    if (pattern) IUIAutomationTextPattern_Release(pattern);
    return text;
}

/* One ordered Unicode input batch replaces the selection without Ctrl+V.
 * Normalize CRLF to one CR; Unicode tabs avoid navigation key shortcuts. */
static int type_text(HWND target, const wchar_t *text, size_t length)
{
    INPUT *input;
    size_t i, n = 0;
    UINT sent;
    if (!length || length > UINT_MAX / 2 || length > SIZE_MAX / (2 * sizeof(INPUT))) return 0;
    input = calloc(length * 2, sizeof(INPUT));
    if (!input) return 0;
    for (i = 0; i < length; ++i) {
        WORD unit = text[i];
        if (unit == L'\r' && i + 1 < length && text[i + 1] == L'\n') ++i;
        if (unit == L'\n') unit = L'\r';
        input[n].type = input[n + 1].type = INPUT_KEYBOARD;
        input[n].ki.wScan = input[n + 1].ki.wScan = unit;
        input[n].ki.dwFlags = KEYEVENTF_UNICODE;
        input[n + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        n += 2;
    }
    sent = target_active(target) && !keys_down() ? SendInput((UINT)n, input, sizeof(INPUT)) : 0;
    free(input);
    return sent == n;
}

typedef struct { UINT format; HANDLE data; } ClipboardItem;
typedef struct { ClipboardItem *items; size_t count; DWORD sequence; } ClipboardSnapshot;

static void free_clipboard_handle(UINT format, HANDLE data)
{
    if (!data) return;
    if (format == CF_BITMAP || format == CF_DSPBITMAP || format == CF_PALETTE) DeleteObject(data);
    else if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) DeleteEnhMetaFile(data);
    else if (format == CF_METAFILEPICT || format == CF_DSPMETAFILEPICT) {
        METAFILEPICT *picture = GlobalLock(data);
        if (picture) { DeleteMetaFile(picture->hMF); GlobalUnlock(data); }
        GlobalFree(data);
    } else GlobalFree(data);
}

static void discard_snapshot(ClipboardSnapshot *snapshot)
{
    size_t i;
    for (i = 0; i < snapshot->count; ++i)
        free_clipboard_handle(snapshot->items[i].format, snapshot->items[i].data);
    free(snapshot->items);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int open_clipboard_bounded(void)
{
    ULONGLONG deadline = GetTickCount64() + 1000;
    do {
        if (OpenClipboard(host)) return 1;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    return 0;
}

/* Abort before copying if any original format cannot be safely preserved. */
static int save_clipboard(ClipboardSnapshot *snapshot)
{
    UINT format = 0;
    int ok = 0;
    if (!open_clipboard_bounded()) return 0;
    for (;;) {
        ClipboardItem *items;
        HANDLE source, duplicate;
        SetLastError(ERROR_SUCCESS);
        format = EnumClipboardFormats(format);
        if (!format) { ok = GetLastError() == ERROR_SUCCESS; break; }
        if (format == CF_OWNERDISPLAY || (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST)) break;
        source = GetClipboardData(format);
        if (!source) break;
        if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE)
            duplicate = CopyEnhMetaFileW(source, NULL);
        else duplicate = OleDuplicateData(source, (CLIPFORMAT)(format == CF_DSPBITMAP ? CF_BITMAP :
            format == CF_DSPMETAFILEPICT ? CF_METAFILEPICT : format), GMEM_MOVEABLE);
        if (!duplicate) break;
        items = realloc(snapshot->items, (snapshot->count + 1) * sizeof(*items));
        if (!items) { free_clipboard_handle(format, duplicate); break; }
        snapshot->items = items;
        items[snapshot->count].format = format;
        items[snapshot->count++].data = duplicate;
    }
    snapshot->sequence = GetClipboardSequenceNumber();
    CloseClipboard();
    if (!ok) discard_snapshot(snapshot);
    return ok;
}

/* Suppress our publications in Windows clipboard history/cloud sync. The
 * source app's Ctrl+C publication is outside our control. Clipboard is open. */
static void exclude_clipboard_history(void)
{
    UINT format = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (data && (!format || !SetClipboardData(format, data))) GlobalFree(data);
}

static int restore_clipboard(ClipboardSnapshot *snapshot, DWORD expected)
{
    size_t i;
    int ok = 1;
    if (!open_clipboard_bounded()) return 0;
    /* Preserve a newer copy made by the user or another application. */
    if (GetClipboardSequenceNumber() != expected || expected == snapshot->sequence) { CloseClipboard(); return 1; }
    if (!EmptyClipboard()) { CloseClipboard(); return 0; }
    exclude_clipboard_history();
    for (i = 0; i < snapshot->count; ++i) {
        if (SetClipboardData(snapshot->items[i].format, snapshot->items[i].data))
            snapshot->items[i].data = NULL;
        else ok = 0;
    }
    CloseClipboard();
    return ok;
}

static int send_chord(HWND target, WORD key, int shift)
{
    INPUT input[6] = {0};
    WORD codes[] = {VK_CONTROL, VK_SHIFT, key, key, VK_SHIFT, VK_CONTROL};
    UINT i, n = 0, sent;
    if (!target_active(target) || keys_down()) return 0;
    for (i = 0; i < 6; ++i) {
        if (!shift && (i == 1 || i == 4)) continue;
        input[n].type = INPUT_KEYBOARD;
        input[n].ki.wVk = codes[i];
        input[n].ki.dwFlags = i >= 3 ? KEYEVENTF_KEYUP : 0;
        if (codes[i] == VK_HOME) input[n].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        ++n;
    }
    sent = SendInput(n, input, sizeof(INPUT));
    if (sent == n) return 1;
    for (i = 0; i < n; ++i) input[i].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(n, input, sizeof(INPUT));
    return 0;
}

/* 1 = new text, 0 = no selection/copy timeout, -1 = unsafe to continue. */
static int copy_selection(HWND target, DWORD timeout, DWORD *sequence)
{
    ULONGLONG deadline;
    DWORD process, owner_process = 0;
    if (GetClipboardSequenceNumber() != *sequence || !send_chord(target, 'C', 0)) return -1;
    deadline = GetTickCount64() + timeout;
    do {
        DWORD current = GetClipboardSequenceNumber();
        if (!target_active(target) || keys_down()) return -1;
        if (current != *sequence) {
            GetWindowThreadProcessId(target, &process);
            GetWindowThreadProcessId(GetClipboardOwner(), &owner_process);
            if (owner_process != process) return -1;
            *sequence = current;
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) return 1;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return 0;
}

static BSTR read_clipboard_text(void)
{
    HANDLE data;
    const wchar_t *input;
    size_t length = 0, capacity;
    BSTR text = NULL;
    data = GetClipboardData(CF_UNICODETEXT);
    if (!data) return NULL;
    capacity = GlobalSize(data) / sizeof(wchar_t);
    input = GlobalLock(data);
    if (!input) return NULL;
    while (length < capacity && input[length]) ++length;
    if (length < capacity && length <= UINT_MAX) text = SysAllocStringLen(input, (UINT)length);
    GlobalUnlock(data);
    return text;
}

static int clipboard_conversion(HWND target, int fallback)
{
    ClipboardSnapshot snapshot = {0};
    DWORD sequence;
    BSTR text = NULL;
    HGLOBAL output = NULL;
    wchar_t *locked;
    size_t length;
    int copied, success = 0;
    if (!target_active(target) || keys_down() || !save_clipboard(&snapshot)) return 0;
    sequence = snapshot.sequence;
    copied = copy_selection(target, 150, &sequence);
    if (copied == 0 && send_chord(target, VK_HOME, 1))
        copied = copy_selection(target, 800, &sequence);
    if (copied != 1 || !open_clipboard_bounded()) goto done;
    if (GetClipboardSequenceNumber() != sequence) { CloseClipboard(); goto done; }
    text = read_clipboard_text();
    if (!text || !(length = SysStringLen(text)) ||
        !transform_utf16(text, &length, DIR_AUTO, fallback)) { CloseClipboard(); goto done; }
    output = GlobalAlloc(GMEM_MOVEABLE, (length + 1) * sizeof(wchar_t));
    locked = output ? GlobalLock(output) : NULL;
    if (!locked) { CloseClipboard(); goto done; }
    memcpy(locked, text, (length + 1) * sizeof(wchar_t));
    GlobalUnlock(output);
    if (target_active(target) && !keys_down() && EmptyClipboard()) {
        exclude_clipboard_history();
        if (SetClipboardData(CF_UNICODETEXT, output)) { output = NULL; success = 1; }
        sequence = GetClipboardSequenceNumber();
    }
    CloseClipboard();
    if (success) {
        success = send_chord(target, 'V', 0);
        /* Generic applications have no paste-complete acknowledgement. Allow
         * asynchronous paste handlers time to read before restoring content. */
        Sleep(300);
    }
done:
    if (!restore_clipboard(&snapshot, sequence)) success = 0;
    discard_snapshot(&snapshot);
    if (output) GlobalFree(output);
    SysFreeString(text);
    return success;
}

static DWORD WINAPI run_conversion(void *unused)
{
    HWND target = GetForegroundWindow();
    ULONGLONG deadline = GetTickCount64() + RELEASE_KEYS_TIMEOUT_MS;
    int fallback, success = 0, allow_clipboard = 1;
    HRESULT initialized;
    IUIAutomation *automation = NULL;
    IUIAutomationElement *element = NULL;
    BSTR text = NULL;
    size_t length;
    GUITHREADINFO info = {0};
    DWORD thread;
    HKL layout;
    (void)unused;
    SetWindowTextW(host, L"Waiting for hotkey release");
    while (keys_down() && GetTickCount64() < deadline && target_active(target)) Sleep(2);
    if (keys_down() || !target_active(target)) goto done;
    thread = GetWindowThreadProcessId(target, NULL);
    layout = GetKeyboardLayout(thread);
    fallback = LOWORD((ULONG_PTR)layout) == 0x0419 ? DIR_RU : DIR_EN;
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(thread, &info) && info.hwndFocus && classic_edit(info.hwndFocus)) {
        if (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_PASSWORD) allow_clipboard = 0;
        else text = edit_selection_text(info.hwndFocus, target);
    } else {
        initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(initialized)) {
        if (SUCCEEDED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                &IID_IUIAutomation, (void **)&automation)) &&
            SUCCEEDED(IUIAutomation_GetFocusedElement(automation, &element)) && element &&
            target_active(target)) {
            BOOL password = FALSE;
            if (SUCCEEDED(IUIAutomationElement_get_CurrentIsPassword(element, &password)) && password)
                allow_clipboard = 0;
            else text = selection_text(element, target);
        }
        if (element) IUIAutomationElement_Release(element);
        if (automation) IUIAutomation_Release(automation);
        CoUninitialize();
        }
    }
    if (text && (length = SysStringLen(text)) != 0 &&
        transform_utf16(text, &length, DIR_AUTO, fallback)) {
        SetWindowTextW(host, L"Typing converted selection");
        success = type_text(target, text, length);
    }
    /* Only a failed read triggers fallback. Never repeat a partial input. */
    if (!text && allow_clipboard && target_active(target) && !keys_down())
        success = clipboard_conversion(target, fallback);
    SysFreeString(text);
#if SWITCH_LAYOUT_AFTER_CONVERSION
    if (success) {
        HKL next = LoadKeyboardLayoutW(fallback == DIR_EN ? L"00000419" : L"00000409", 0);
        if (next) PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)next);
    }
#endif
done:
    PostMessageW(host, WM_FINISHED, success, 0);
    return 0;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == taskbar_created) { Shell_NotifyIconW(NIM_ADD, &tray); return 0; }
    switch (message) {
    case WM_HOTKEY:
        if (wp == ID_HOTKEY && !stopping && InterlockedCompareExchange(&busy, 1, 0) == 0) {
            worker = CreateThread(NULL, 0, run_conversion, NULL, 0, NULL);
            if (!worker) InterlockedExchange(&busy, 0);
        }
        return 0;
    case WM_FINISHED:
        if (worker) { CloseHandle(worker); worker = NULL; }
        InterlockedExchange(&busy, 0);
        if (stopping) DestroyWindow(window);
        else if (!wp) MessageBeep(MB_ICONWARNING);
        return 0;
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            POINT point;
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Раскладка: " SWITCH_HOTKEY_TEXT);
            AppendMenuW(menu, MF_STRING, ID_EXIT, L"Выход");
            GetCursorPos(&point);
            SetForegroundWindow(window);
            if (TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                              0, window, NULL) == ID_EXIT) PostMessageW(window, WM_CLOSE, 0, 0);
            DestroyMenu(menu);
            PostMessageW(window, WM_NULL, 0, 0);
        }
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&stopping, 1);
        UnregisterHotKey(window, ID_HOTKEY);
        if (!busy) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show)
{
    WNDCLASSW cls = {0};
    MSG msg;
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\NativeLayoutSwitcher_72b158");
    (void)previous; (void)command; (void)show;
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Переключатель уже запущен.", L"Раскладка", MB_OK);
        if (mutex) CloseHandle(mutex);
        return 1;
    }
    cls.lpfnWndProc = window_proc;
    cls.hInstance = instance;
    cls.lpszClassName = L"NativeLayoutSwitcherWindow";
    RegisterClassW(&cls);
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    host = CreateWindowW(cls.lpszClassName, L"Раскладка", 0, 0, 0, 0, 0,
                         NULL, NULL, instance, NULL);
    if (!host ||
        !RegisterHotKey(host, ID_HOTKEY, SWITCH_MODIFIERS | MOD_NOREPEAT, SWITCH_KEY)) {
        MessageBoxW(NULL, L"Не удалось зарегистрировать " SWITCH_HOTKEY_TEXT
                    L". Закройте старый AHK-скрипт или другую программу с этим сочетанием.",
                    L"Раскладка", MB_OK | MB_ICONERROR);
        if (host) DestroyWindow(host);
        CloseHandle(mutex);
        return 1;
    }
    tray.cbSize = sizeof(tray);
    tray.hWnd = host;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(tray.szTip, L"Раскладка — " SWITCH_HOTKEY_TEXT, ARRAYSIZE(tray.szTip));
    Shell_NotifyIconW(NIM_ADD, &tray);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    return 0;
}

