/*
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include "control-codes.h"

// Macros {{{
#ifdef DUMP_COMMANDS
static void _report_error(PyObject *dump_callback, const char *fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    PyObject *temp = PyUnicode_FromFormatV(fmt, argptr);
    va_end(argptr);
    if (temp != NULL) {
        Py_XDECREF(PyObject_CallFunctionObjArgs(dump_callback, temp, NULL)); PyErr_Clear();
        Py_CLEAR(temp);
    }
}

#define DUMP_UNUSED

#define REPORT_ERROR(...) _report_error(dump_callback, __VA_ARGS__);

#define REPORT_COMMAND1(name) \
        Py_XDECREF(PyObject_CallFunction(dump_callback, "s", #name)); PyErr_Clear();

#define REPORT_COMMAND2(name, x) \
        Py_XDECREF(PyObject_CallFunction(dump_callback, "si", #name, (int)x)); PyErr_Clear();

#define REPORT_COMMAND3(name, x, y) \
        Py_XDECREF(PyObject_CallFunction(dump_callback, "sii", #name, (int)x, (int)y)); PyErr_Clear();

#define GET_MACRO(_1,_2,_3,NAME,...) NAME
#define REPORT_COMMAND(...) GET_MACRO(__VA_ARGS__, REPORT_COMMAND3, REPORT_COMMAND2, REPORT_COMMAND1, SENTINEL)(__VA_ARGS__)

#define REPORT_DRAW(ch) \
    Py_XDECREF(PyObject_CallFunction(dump_callback, "sC", "draw", ch)); PyErr_Clear();

#else

#define DUMP_UNUSED UNUSED

#define REPORT_ERROR(...) fprintf(stderr, "[PARSE ERROR] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");

#define REPORT_COMMAND(...)
#define REPORT_DRAW(ch)

#endif

#define SET_STATE(state) screen->parser_state = state; screen->parser_buf_pos = 0;
// }}}

// Normal mode {{{
static inline void 
handle_normal_mode_char(Screen *screen, uint32_t ch, PyObject DUMP_UNUSED *dump_callback) {
#define CALL_SCREEN_HANDLER(name) REPORT_COMMAND(name, ch); name(screen); break;
    switch(ch) {
        case BEL:
            CALL_SCREEN_HANDLER(screen_bell);
        case BS:
            CALL_SCREEN_HANDLER(screen_backspace);
        case HT:
            CALL_SCREEN_HANDLER(screen_tab);
        case LF:
        case VT:
        case FF:
        case NEL:
            CALL_SCREEN_HANDLER(screen_linefeed);
        case CR:
            CALL_SCREEN_HANDLER(screen_carriage_return);
        case SO:
            REPORT_ERROR("Unhandled charset change command (SO), ignoring"); break;
        case SI:
            REPORT_ERROR("Unhandled charset change command (SI), ignoring"); break;
        case IND:
            CALL_SCREEN_HANDLER(screen_index);
        case RI:
            CALL_SCREEN_HANDLER(screen_reverse_index);
        case HTS:
            CALL_SCREEN_HANDLER(screen_set_tab_stop);
        case ESC:
        case CSI:
        case OSC:
        case DCS:
            SET_STATE(ch); break;
        case NUL:
        case DEL:
            break;  // no-op
        default:
            REPORT_DRAW(ch);
            screen_draw(screen, ch);
            break;
    }
#undef CALL_SCREEN_HANDLER
} // }}}

// Esc mode {{{
static inline void 
handle_esc_mode_char(Screen *screen, uint32_t ch, PyObject DUMP_UNUSED *dump_callback) {
#define CALL_ED(name) REPORT_COMMAND(name, ch); name(screen); SET_STATE(0); break;
    switch(screen->parser_buf_pos) {
        case 0:
            switch (ch) {
                case ESC_DCS:
                    SET_STATE(DCS); break;
                case ESC_OSC:
                    SET_STATE(OSC); break;
                case ESC_CSI:
                    SET_STATE(CSI); break;
                case ESC_RIS:
                    CALL_ED(screen_reset);
                case ESC_IND:
                    CALL_ED(screen_index);
                case ESC_NEL:
                    CALL_ED(screen_linefeed);
                case ESC_RI:
                    CALL_ED(screen_reverse_index);
                case ESC_HTS:
                    CALL_ED(screen_set_tab_stop);
                case ESC_DECSC:
                    CALL_ED(screen_save_cursor);
                case ESC_DECRC:
                    CALL_ED(screen_restore_cursor);
                case ESC_DECPNM: 
                    CALL_ED(screen_normal_keypad_mode);
                case ESC_DECPAM: 
                    CALL_ED(screen_alternate_keypad_mode);
                case '%':
                case '(':
                case ')':
                case '*':
                case '+':
                case '-':
                case '.':
                case '/':
                case ' ':
                    screen->parser_buf[screen->parser_buf_pos++] = ch;
                    break;
                default:
                    REPORT_ERROR("%s0x%x", "Unknown char after ESC: ", ch); 
                    SET_STATE(0); break;
            }
            break;
        default:
            if (screen->parser_buf[0] == '%' && ch == 'G') { 
                // switch to utf-8, since we are always in utf-8, ignore.
            } else {
                REPORT_ERROR("Unhandled charset related escape code: 0x%x 0x%x", screen->parser_buf[0], screen->parser_buf[1]);
            }
            SET_STATE(0); break;
    }
#undef CALL_ED
} // }}}

// OSC mode {{{
static inline void
dispatch_osc(Screen *screen) {
    screen->parser_buf_pos++;
}
// }}}

// DCS mode {{{
static inline void
dispatch_dcs(Screen *screen) {
    screen->parser_buf_pos++;
}
// }}}

// Parse loop {{{

static inline bool
accumulate_osc(Screen *screen, uint32_t ch, PyObject DUMP_UNUSED *dump_callback) {
    switch(ch) {
        case ST:
            return true;
        case ESC_ST:
            if (screen->parser_buf_pos > 0 && screen->parser_buf[screen->parser_buf_pos - 1] == ESC) {
                screen->parser_buf_pos--;
                return true;
            }
        case BEL:
            return true;
        case NUL:
        case DEL:
            break;
        default:
            if (screen->parser_buf_pos >= PARSER_BUF_SZ - 1) {
                REPORT_ERROR("OSC sequence too long, truncating.");
                return true;
            }
            screen->parser_buf[screen->parser_buf_pos++] = ch;
            break;
    }
    return false;
}

static inline bool
accumulate_dcs(Screen *screen, uint32_t ch, PyObject DUMP_UNUSED *dump_callback) {
    switch(ch) {
        case ST:
            return true;
        case NUL:
        case DEL:
            break;
        case ESC:
#pragma GCC diagnostic ignored "-Wpedantic"
        case 32 ... 126:
#pragma GCC diagnostic pop
            if (screen->parser_buf_pos > 0 && screen->parser_buf[screen->parser_buf_pos-1] == ESC) {
                if (ch == '\\') { screen->parser_buf_pos--; return true; }
                REPORT_ERROR("DCS sequence contained non-printable character: 0x%x ignoring the sequence", ESC);
                SET_STATE(ESC); return false;
            }
            if (screen->parser_buf_pos >= PARSER_BUF_SZ - 1) {
                REPORT_ERROR("DCS sequence too long, truncating.");
                return true;
            }
            screen->parser_buf[screen->parser_buf_pos++] = ch;
            break;
        default:
            REPORT_ERROR("DCS sequence contained non-printable character: 0x%x ignoring the sequence", ch);
    }
    return false;
}

static inline void 
_parse_bytes(Screen *screen, uint8_t *buf, Py_ssize_t len, PyObject DUMP_UNUSED *dump_callback) {
#define HANDLE(name) handle_##name(screen, codepoint, dump_callback); break
    uint32_t prev = screen->utf8_state, codepoint = 0;
    for (unsigned int i = 0; i < len; i++, prev = screen->utf8_state) {
        switch (decode_utf8(&screen->utf8_state, &codepoint, buf[i])) {
            case UTF8_ACCEPT:
                switch(screen->parser_state) {
                    case ESC:
                        HANDLE(esc_mode_char);
                    /* case CSI_STATE: */
                    /*     CALL_HANDLER(csi); */
                    case OSC:
                        if (accumulate_osc(screen, codepoint, dump_callback)) { dispatch_osc(screen); SET_STATE(0); }
                        break;
                    case DCS:
                        if (accumulate_dcs(screen, codepoint, dump_callback)) { dispatch_dcs(screen); SET_STATE(0); }
                        if (screen->parser_state == ESC) { HANDLE(esc_mode_char); }
                        break;
                    default:
                        HANDLE(normal_mode_char);
                }
                break;
            case UTF8_REJECT:
                screen->utf8_state = UTF8_ACCEPT;
                if (prev != UTF8_ACCEPT) i--;
                break;
        }
    }
#undef HANDLE
}
// }}}

// Boilerplate {{{
#ifdef DUMP_COMMANDS
#define FNAME(x) x##_dump
#else
#define FNAME(x) x
#endif

PyObject*
FNAME(parse_bytes)(PyObject UNUSED *self, PyObject *args) {
    PyObject *dump_callback = NULL;
    Py_buffer pybuf;
    Screen *screen;
#ifdef DUMP_COMMANDS
    if (!PyArg_ParseTuple(args, "OO!y*", &dump_callback, &Screen_Type, &screen, &pybuf)) return NULL;
#else
    if (!PyArg_ParseTuple(args, "O!y*", &Screen_Type, &screen, &pybuf)) return NULL;
#endif
    _parse_bytes(screen, pybuf.buf, pybuf.len, dump_callback);
    Py_RETURN_NONE;
}

PyObject*
FNAME(read_bytes)(PyObject UNUSED *self, PyObject *args) {
    PyObject *dump_callback = NULL;
    Py_ssize_t len;
    Screen *screen;
    int fd;
#ifdef DUMP_COMMANDS
    if (!PyArg_ParseTuple(args, "OOi", &dump_callback, &screen, &fd)) return NULL;
#else
    if (!PyArg_ParseTuple(args, "Oi", &screen, &fd)) return NULL;
#endif

    while(true) {
        len = read(fd, screen->read_buf, READ_BUF_SZ);
        if (len == -1) {
            if (errno == EINTR) continue;
            if (errno == EIO) { Py_RETURN_FALSE; }
            return PyErr_SetFromErrno(PyExc_OSError);
        }
        break;
    }
    _parse_bytes(screen, screen->read_buf, len, dump_callback);
    if(len > 0) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}
#undef FNAME
// }}}
