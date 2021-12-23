/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FOUNDATION_ACE_THIRD_PARTY_QUICKJS_DEBUGGER_H
#define FOUNDATION_ACE_THIRD_PARTY_QUICKJS_DEBUGGER_H

#include "quickjs.h"

#include <time.h>

#include "hilog/log.h"
#include "securec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JS_DEBUGGER_HOST_ADDRESS "127.0.0.1"
#define JS_DEBUGGER_PORT_NUM 5555
#define STR_BUF_SIZE 64
#define READ_MSG_HEX_NUM 16
#define READ_MSG_LEN 9
#define WRITE_MSG_LEN 10
#define WRITE_MSG_ADD_NEW_LINE 2
#define FRAME_MOVE_TWO_STEP 2
#define STACK_DEPTH_MOVE_TWO_STEP 2
#define TYPE_OF_SCOPE_NUM 4
#define MAX_STR_LEN 20
#define FAIL_CAUSE_SOCKET_NO_CLIENT (-1003)
#define FAIL_CAUSE_SOCKET_COMMON_FAIL (-1004)
#define FAIL_CAUSE_READ_MSG_FAIL (-1005)
#define JS_SOCKET_SUCCESS 1
#define DBG_NEED_READ_MSG 1
#define DBG_NO_NEED_READ_MSG 0
#define DBG_LOC_ISEQUAL 1
#define DBG_LOC_ISNOTEQUAL 0
#define DBG_WAITING_TIME 100

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0xD003F01
#define LOG_TAG "DebuggerLogger"

#define DEBUGGER_LOGI(...) HILOG_INFO(LOG_CORE, __VA_ARGS__)
#define DEBUGGER_LOGE(...) HILOG_ERROR(LOG_CORE, __VA_ARGS__)

enum StepOperaton {
    NO_STEP_OPERATION = 0,
    STEP_NEXT = 1,
    STEP_IN = 2,
    STEP_OUT = 3,
    STEP_CONTINUE = 4
};

enum ScopeType {
    GLOBAL = 0,
    LOCAL = 1,
    CLOSURE = 2
};

typedef struct LocInfo {
    JSAtom filename;
    int line;
} LocInfo;

typedef struct DebuggerInfo {
    JSContext *cx;
    volatile int client;
    int stepOperation;
    uint32_t depth;
    LocInfo loc;
    JSValue breakpoints;
    int buildConnect;
    volatile int isConnected;
} DebuggerInfo;

typedef struct DebuggerVariableState {
    uint32_t variableReferenceCount;
    JSValue variableReferences;
    JSValue variablePointers;
    const uint8_t *curPc;
} DebuggerVariableState;

DebuggerInfo *JS_GetDebuggerInfo(JSContext *cx);
JSValue JS_BuildStackTrace(JSContext *cx, const uint8_t *curPc);
uint32_t JS_GetStackDepth(const JSContext *ctx);
LocInfo JS_GetCurrentLocation(JSContext *ctx, const uint8_t *pc);
JSValue JS_DebuggerEvaluate(JSContext *ctx, int stackIndex, JSValue expression);
void DBG_FreeSources(JSContext *cx, DebuggerInfo *debuggerInfo);
void DBG_CallDebugger(JSContext *cx, const uint8_t *pc);
uint32_t DBG_GetValueAsUint32Type(JSContext *cx, JSValue obj, const char *property);
void DBG_SetDebugMode(bool isAttachMode);

#ifdef __cplusplus
}
#endif
#endif
