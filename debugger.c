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

#include "debugger.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "message_server.h"

static __thread bool g_isServerStarted = 0;
static __thread bool g_isBreakPointSet = 0;
static __thread bool g_isAttachMode = 0;

void DBG_SetDebugMode(bool isAttachMode)
{
    g_isAttachMode = isAttachMode;
}

static int DBG_ConnectToIde()
{
    const char *hostAddr = JS_DEBUGGER_HOST_ADDRESS;
    const int portNum = JS_DEBUGGER_PORT_NUM;
    struct sockaddr_in ideAddr;
    if (memset_s(&ideAddr, sizeof(ideAddr), 0, sizeof(ideAddr)) != 0) {
        DEBUGGER_LOGE("DBG_ConnectToIde memset_s fail");
        return FAIL_CAUSE_SOCKET_NO_CLIENT;
    }
    ideAddr.sin_port = htons(portNum);
    ideAddr.sin_family = AF_INET;
    ideAddr.sin_addr.s_addr = inet_addr(hostAddr);
    int client = TEMP_FAILURE_RETRY(socket(AF_INET, SOCK_STREAM, 0));
    if (client <= 0) {
        DEBUGGER_LOGI("DBG_ConnectToIde client=%d", client);
        return FAIL_CAUSE_SOCKET_NO_CLIENT;
    }
    if (TEMP_FAILURE_RETRY(connect(client,
        (const struct sockaddr *)&ideAddr, sizeof(ideAddr))) == -1) {
        DEBUGGER_LOGE("DBG_ConnectToIde fail connectResult fail");
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }

    return client;
}

static int DBG_SocketWrite(int client, const char *buf, int len)
{
    if (client <= 0 || len == 0 || buf == NULL) {
        DEBUGGER_LOGE("DBG_SocketWrite fail client=%d, len=%d, buf=%s", client, len, buf);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }

    int loc = 0;
    while (loc < len) {
        int ret = TEMP_FAILURE_RETRY(write(client, (const void *)(buf + loc), len - loc));
        if (ret <= 0 || ret > (len - loc)) {
            DEBUGGER_LOGE("DBG_SocketWrite fail ret=%d", ret);
            return FAIL_CAUSE_READ_MSG_FAIL;
        }
        loc = loc + ret;
    }

    return JS_SOCKET_SUCCESS;
}

static JSValue DBG_ReadMsg(DebuggerInfo *debuggerInfo)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_ReadMsg fail debuggerInfo=NULL");
        return JS_UNDEFINED;
    }
    while (QueueIsEmpty(debuggerInfo->client)) {
        usleep(DBG_WAITING_TIME);
    }

    const char *message = QueueFront(debuggerInfo->client);
    DEBUGGER_LOGI("DBG_ReadMsg %s", message);
    JSValue msg = JS_ParseJSON(debuggerInfo->cx, message, strlen(message), "<debugger>");
    QueuePop(debuggerInfo->client);

    return msg;
}

static void DBG_SendMsg(DebuggerInfo *debuggerInfo, JSValue msgBody)
{
    if (debuggerInfo == NULL) {
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        return;
    }
    JSValueConst args[] = {msgBody, JS_UNDEFINED, JS_UNDEFINED};
    JSValue jsValMsg = JS_JsonStringify(cx, JS_UNDEFINED, 1, args);
    size_t len = 0;
    const char *jsonStrMsg = JS_ToCStringLen(cx, &len, jsValMsg);
    if (jsonStrMsg == NULL) {
        DEBUGGER_LOGE("DBG_SendMsg fail jsonStrMsg=%s", jsonStrMsg);
        return;
    }
    if (len == 0) {
        DEBUGGER_LOGE("DBG_SendMsg fail len=%zu", len);
        JS_FreeValue(cx, jsValMsg);
        return;
    }

    DEBUGGER_LOGI("DBG_SendMsg: %s", jsonStrMsg);
    char msglen[WRITE_MSG_LEN] = {0};
    if (sprintf_s(msglen, sizeof(msglen), "%08x\n", (int)len + 1) < 0) {
        JS_FreeCString(cx, jsonStrMsg);
        JS_FreeValue(cx, jsValMsg);
        return;
    }
    int writeMsgLenRet = DBG_SocketWrite(debuggerInfo->client, msglen, WRITE_MSG_LEN - 1);
    if (!writeMsgLenRet) {
        JS_FreeCString(cx, jsonStrMsg);
        JS_FreeValue(cx, jsValMsg);
        return;
    }
    int writeMsgRet = DBG_SocketWrite(debuggerInfo->client, jsonStrMsg, len);
    if (!writeMsgRet) {
        JS_FreeCString(cx, jsonStrMsg);
        JS_FreeValue(cx, jsValMsg);
        return;
    }
    char addLine[WRITE_MSG_ADD_NEW_LINE] = { '\n', '\0' };
    int writeNewLineRet = DBG_SocketWrite(debuggerInfo->client, addLine, 1);
    if (!writeNewLineRet) {
        JS_FreeCString(cx, jsonStrMsg);
        JS_FreeValue(cx, jsValMsg);
        return;
    }
    JS_FreeCString(cx, jsonStrMsg);
    JS_FreeValue(cx, jsValMsg);
}
static void DBG_SendStopMsg(DebuggerInfo *debuggerInfo, const char *stopReason)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SendStopMsg fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_SendStopMsg fail cx=NULL");
        return;
    }
    JSValue stopEvent = JS_NewObject(cx);
    JS_SetPropertyStr(cx, stopEvent, "reason", JS_NewString(cx, stopReason));
    JS_SetPropertyStr(cx, stopEvent, "type", JS_NewString(cx, "StoppedEvent"));
    JS_SetPropertyStr(cx, stopEvent, "thread", JS_NewInt64(cx, (int64_t)debuggerInfo->client));
    JSValue msgBody = JS_NewObject(cx);
    JS_SetPropertyStr(cx, msgBody, "type", JS_NewString(cx, "event"));
    JS_SetPropertyStr(cx, msgBody, "event", stopEvent);
    DBG_SendMsg(debuggerInfo, msgBody);
    JS_FreeValue(cx, msgBody);

    return;
}

static void DBG_SendResponseMsg(DebuggerInfo *debuggerInfo, JSValue request, JSValue body)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SendResponseMsg fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_SendResponseMsg fail cx=NULL");
        return;
    }
    JSValue msgBody = JS_NewObject(cx);
    JS_SetPropertyStr(cx, msgBody, "type", JS_NewString(cx, "response"));
    JS_SetPropertyStr(cx, msgBody, "request_seq", JS_GetPropertyStr(cx, request, "request_seq"));
    JS_SetPropertyStr(cx, msgBody, "body", body);
    DBG_SendMsg(debuggerInfo, msgBody);
    JS_FreeValue(cx, msgBody);

    return;
}


static void DBG_SetBreakpoints(DebuggerInfo *debuggerInfo, JSValue breakpoints)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SetBreakpoints fail debuggerInfo=NULL");
        return;
    }
    JSValue filename = JS_GetPropertyStr(debuggerInfo->cx, breakpoints, "path");
    const char *path = JS_ToCString(debuggerInfo->cx, filename);
    if (path == NULL) {
        DEBUGGER_LOGE("DBG_SetBreakpoints fail path=%s", path);
        JS_FreeValue(debuggerInfo->cx, filename);
        return;
    }
    JSValue pathData = JS_GetPropertyStr(debuggerInfo->cx, debuggerInfo->breakpoints, path);
    if (!JS_IsUndefined(pathData)) {
        DEBUGGER_LOGE("DBG_SetBreakpoints fail pathData=JS_Undefined");
        JS_FreeValue(debuggerInfo->cx, pathData);
    }

    pathData = JS_NewObject(debuggerInfo->cx);
    JS_SetPropertyStr(debuggerInfo->cx, debuggerInfo->breakpoints, path, pathData);
    JS_FreeCString(debuggerInfo->cx, path);
    JS_FreeValue(debuggerInfo->cx, filename);

    JSValue jsBreakpoints = JS_GetPropertyStr(debuggerInfo->cx, breakpoints, "breakpoints");
    JS_SetPropertyStr(debuggerInfo->cx, pathData, "breakpoints", jsBreakpoints);
    JS_FreeValue(debuggerInfo->cx, jsBreakpoints);

    return;
}

static void DBG_CotinueProcess(DebuggerInfo *debuggerInfo, JSValue msg, const uint8_t *pc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_CotinueProcess fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        return;
    }
    debuggerInfo->depth = JS_GetStackDepth(cx);
    LocInfo loc = JS_GetCurrentLocation(cx, pc);
    debuggerInfo->stepOperation = STEP_CONTINUE;
    debuggerInfo->loc = loc;
    DBG_SendResponseMsg(debuggerInfo, msg, JS_UNDEFINED);

    return;
}

static void DBG_StackTraceProcess(DebuggerInfo *debuggerInfo, JSValue msg, const uint8_t *curPc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_StackTraceProcess fail debuggerInfo=NULL");
        return;
    }
    JSValue stackTrace = JS_BuildStackTrace(debuggerInfo->cx, curPc);
    DBG_SendResponseMsg(debuggerInfo, msg, stackTrace);

    return;
}


static void DBG_SetScopes(JSContext *cx, JSValue scopes, int scopeCount, int scopeType, int frameId)
{
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_StackTraceProcess fail cx=NULL");
        return;
    }
    JSValue scopeObj = JS_NewObject(cx);
    JSValue expensiveFlag = JS_FALSE;
    switch (scopeType) {
        case GLOBAL:
            expensiveFlag = JS_TRUE;
            JS_SetPropertyStr(cx, scopeObj, "name", JS_NewString(cx, "Global"));
            break;
        case LOCAL:
            JS_SetPropertyStr(cx, scopeObj, "name", JS_NewString(cx, "Locals"));
            break;
        case CLOSURE:
            JS_SetPropertyStr(cx, scopeObj, "name", JS_NewString(cx, "Closure"));
            break;
        default:
            return;
    }
    JS_SetPropertyStr(cx, scopeObj, "reference",
        JS_NewInt32(cx, (frameId << FRAME_MOVE_TWO_STEP) + scopeType));
    JS_SetPropertyStr(cx, scopeObj, "expensive", expensiveFlag);
    JS_SetPropertyUint32(cx, scopes, scopeCount, scopeObj);

    return;
}

uint32_t DBG_GetValueAsUint32Type(JSContext *cx, JSValue obj, const char *property)
{
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_GetValueAsUint32Type fail cx=NULL");
        return -1;
    }
    JSValue prop = JS_GetPropertyStr(cx, obj, property);
    uint32_t ret;
    JS_ToUint32(cx, &ret, prop);
    JS_FreeValue(cx, prop);

    return ret;
}


static void DBG_ScopesProcess(DebuggerInfo *debuggerInfo, JSValue msg)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_ScopesProcess fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_ScopesProcess fail cx=NULL");
        return;
    }
    JSValue argsValue = JS_GetPropertyStr(cx, msg, "args");
    int frameId = DBG_GetValueAsUint32Type(cx, argsValue, "frameId");
    if (frameId == -1) {
        DEBUGGER_LOGE("DBG_ScopesProcess fail frameId=%d", frameId);
        return;
    }
    JS_FreeValue(cx, argsValue);
    JSValue scopes = JS_NewArray(cx);
    int scopeCount = 0;
    DBG_SetScopes(cx, scopes, scopeCount++, LOCAL, frameId);
    DBG_SetScopes(cx, scopes, scopeCount++, CLOSURE, frameId);
    DBG_SetScopes(cx, scopes, scopeCount++, GLOBAL, frameId);
    DBG_SendResponseMsg(debuggerInfo, msg, scopes);
    JS_FreeValue(cx, scopes);

    return;
}

static void DBG_SetStepOverToDebugger(DebuggerInfo *debuggerInfo, JSValue msg, const uint8_t *pc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SetStepOverToDebugger fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_SetStepOverToDebugger fail cx=NULL");
        return;
    }
    debuggerInfo->depth = JS_GetStackDepth(cx);
    LocInfo loc = JS_GetCurrentLocation(cx, pc);
    debuggerInfo->stepOperation = STEP_NEXT;
    debuggerInfo->loc = loc;
    DBG_SendResponseMsg(debuggerInfo, msg, JS_UNDEFINED);

    return;
}

static void DBG_SetStepOutToDebugger(DebuggerInfo *debuggerInfo, JSValue msg, const uint8_t *pc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SetStepOutToDebugger fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_SetStepOutToDebugger fail cx=NULL");
        return;
    }
    debuggerInfo->depth = JS_GetStackDepth(cx);
    LocInfo loc = JS_GetCurrentLocation(cx, pc);
    debuggerInfo->stepOperation = STEP_OUT;
    debuggerInfo->loc = loc;
    DBG_SendResponseMsg(debuggerInfo, msg, JS_UNDEFINED);

    return;
}

static void DBG_SetStepInToDebugger(DebuggerInfo *debuggerInfo, JSValue msg, const uint8_t *pc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_SetStepInToDebugger fail debuggerInfo=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_SetStepInToDebugger fail cx=NULL");
        return;
    }
    debuggerInfo->depth = JS_GetStackDepth(cx);
    LocInfo loc = JS_GetCurrentLocation(cx, pc);
    debuggerInfo->stepOperation = STEP_IN;
    debuggerInfo->loc = loc;
    DBG_SendResponseMsg(debuggerInfo, msg, JS_UNDEFINED);

    return;
}

static void DBG_FreePropEnum(JSContext *cx, JSPropertyEnum *tab, uint32_t len)
{
    if (cx == NULL || tab == NULL) {
        DEBUGGER_LOGE("DBG_FreePropEnum fail cx=NULL or tab=NULL");
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        JS_FreeAtom(cx, tab[i].atom);
    }
    js_free(cx, tab);

    return;
}

static uint32_t DBG_GetObjectVariableReference(JSContext *cx,
                                               struct DebuggerVariableState *state,
                                               JSValue var,
                                               JSValue varVal)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_GetObjectVariableReference fail cx=NULL || state=NULL");
        return 0;
    }
    uint32_t reference = 0;
    JSObject *pObj = JS_VALUE_GET_OBJ(varVal);
    if (pObj == NULL) {
        return reference;
    }
    uint32_t pVal = (uint32_t)pObj;
    JSValue found = JS_GetPropertyUint32(cx, state->variablePointers, pVal);
    if (JS_IsUndefined(found)) {
        reference = state->variableReferenceCount++;
        JS_SetPropertyUint32(cx, state->variableReferences, reference, JS_DupValue(cx, varVal));
        JS_SetPropertyUint32(cx, state->variablePointers, pVal, JS_NewInt32(cx, reference));
    } else {
        JS_ToUint32(cx, &reference, found);
    }
    JS_FreeValue(cx, found);

    return reference;
}

static void DBG_SetVariableType(JSContext *cx,
                                struct DebuggerVariableState *state,
                                JSValue var,
                                JSValue varVal)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_SetVariableType fail cx=NULL || state=NULL");
        return;
    }
    uint32_t tag = JS_VALUE_GET_TAG(varVal);
    uint32_t reference = 0;
    switch (tag) {
        case JS_TAG_INT:      // Same processing as JS_TAG_BIG_INT
        case JS_TAG_BIG_INT:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "integer"));
            break;
        case JS_TAG_FLOAT64:  // Same processing as JS_TAG_BIG_FLOAT
        case JS_TAG_BIG_FLOAT:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "float"));
            break;
        case JS_TAG_BOOL:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "bool"));
            break;
        case JS_TAG_STRING:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "string"));
            break;
        case JS_TAG_NULL:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "null"));
            break;
        case JS_TAG_EXCEPTION:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "exception"));
            break;
        case JS_TAG_UNDEFINED:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "undefined"));
            break;
        case JS_TAG_OBJECT:
            JS_SetPropertyStr(cx, var, "type", JS_NewString(cx, "object"));
            reference = DBG_GetObjectVariableReference(cx, state, var, varVal);
            break;
        default:
            break;
    }
    JS_SetPropertyStr(cx, var, "variablesReference", JS_NewInt32(cx, reference));

    return;
}

static void DBG_SetVariableProperty(JSContext *cx,
                                    JSValue varVal,
                                    JSValue var,
                                    const char *valueProperty)
{
    if (cx == NULL || valueProperty == NULL) {
        DEBUGGER_LOGE("DBG_SetVariableProperty fail cx=NULL || valueProperty=%s", valueProperty);
        return;
    }
    if (JS_IsArray(cx, varVal)) {
        uint32_t len = DBG_GetValueAsUint32Type(cx, varVal, "length");
        char lenBuf[STR_BUF_SIZE] = {0};
        if (sprintf_s(lenBuf, sizeof(lenBuf), "Array (%d)", len) < 0) {
            return;
        }
        JS_SetPropertyStr(cx, var, valueProperty, JS_NewString(cx, lenBuf));
        JS_SetPropertyStr(cx, var, "indexedVariables", JS_NewInt32(cx, len));
    } else {
        JS_SetPropertyStr(cx, var, valueProperty, JS_ToString(cx, varVal));
    }

    return;
}

static JSValue DBG_GetVariableObj(JSContext *cx,
                                  struct DebuggerVariableState *state,
                                  JSValue varName,
                                  JSValue varVal)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_GetVariableObj fail cx=NULL || state=NULL");
        return JS_NULL;
    }
    JSValue var = JS_NewObject(cx);
    JS_SetPropertyStr(cx, var, "name", varName);
    DBG_SetVariableProperty(cx, varVal, var, "value");
    DBG_SetVariableType(cx, state, var, varVal);

    return var;
}

static JSValue DBG_ProcessUndefinedVariable(JSContext *cx, struct DebuggerVariableState *state,
                                            uint32_t reference, JSValue variable)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_ProcessUndefinedVariable fail cx=NULL || state=NULL");
        return JS_NULL;
    }
    int frame = reference >> FRAME_MOVE_TWO_STEP;
    int scope = reference % TYPE_OF_SCOPE_NUM;
    if (frame >= JS_GetStackDepth(cx)) {
        return variable;
    }
    switch (scope) {
        case GLOBAL:
            variable = JS_GetGlobalObject(cx);
            break;
        case LOCAL:
            variable = JS_GetLocalVariables(cx, frame);
            break;
        case CLOSURE:
            variable = JS_GetClosureVariables(cx, frame);
            break;
        default:
           break;
    }
    JS_SetPropertyUint32(cx, state->variableReferences, reference, JS_DupValue(cx, variable));
    return variable;
}

static JSValue DBG_VariablesUnFilteredProcess(JSContext *cx,
                                              JSValue variable,
                                              JSValue properties,
                                              struct DebuggerVariableState *state)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_VariablesUnFilteredProcess fail cx=NULL || state=NULL");
        return JS_NULL;
    }
    JSPropertyEnum *tabAtom = NULL;
    uint32_t tabAtomCount = 0;
    if (JS_GetOwnPropertyNames(cx, &tabAtom, &tabAtomCount, variable,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)) {
        return properties;
    }

    for (uint32_t idx = 0; idx < tabAtomCount; idx++) {
        JSValue value = JS_GetProperty(cx, variable, tabAtom[idx].atom);
        JSValue variableJson = DBG_GetVariableObj(cx, state,
            JS_AtomToString(cx, tabAtom[idx].atom), value);
        JS_FreeValue(cx, value);
        JS_SetPropertyUint32(cx, properties, idx, variableJson);
    }
    DBG_FreePropEnum(cx, tabAtom, tabAtomCount);

    return properties;
}

static JSValue DBG_VariablesFilteredProcess(JSContext *cx,
                                            JSValue args,
                                            JSValue variable,
                                            JSValue properties,
                                            struct DebuggerVariableState *state)
{
    if (cx == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_VariablesFilteredProcess fail cx=NULL || state=NULL");
        return JS_NULL;
    }
    const char *filterStr = JS_ToCString(cx, JS_GetPropertyStr(cx, args, "filter"));
    if (filterStr == NULL) {
        DEBUGGER_LOGE("DBG_VariablesFilteredProcess fail filterStr=%s", filterStr);
        return JS_NULL;
    }
    int indexed = strcmp(filterStr, "indexed");
    JS_FreeCString(cx, filterStr);

    if (indexed != 0) {
        return DBG_VariablesUnFilteredProcess(cx, variable, properties, state);
    }

    uint32_t start = DBG_GetValueAsUint32Type(cx, args, "start");
    if (start < 0) {
        return JS_NULL;
    }
    uint32_t count = DBG_GetValueAsUint32Type(cx, args, "count");
    if (count < 0) {
        return JS_NULL;
    }
    char nameBuf[STR_BUF_SIZE] = {0};
    for (uint32_t i = 0; i < count; i++) {
        JSValue value = JS_GetPropertyUint32(cx, variable, start + i);
        if (sprintf_s(nameBuf, sizeof(nameBuf), "%d", i) < 0) {
            continue;
        }
        JSValue variableJson = DBG_GetVariableObj(cx, state, JS_NewString(cx, nameBuf), value);
        JS_FreeValue(cx, value);
        JS_SetPropertyUint32(cx, properties, i, variableJson);
    }

    return properties;
}

static void DBG_VariablesProcess(DebuggerInfo *debuggerInfo,
                                 JSValue request,
                                 struct DebuggerVariableState *state)
{
    if (debuggerInfo == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_VariablesProcess fail debuggerInfo=NULL || state=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_VariablesProcess fail cx=NULL");
        return;
    }
    JSValue args = JS_GetPropertyStr(cx, request, "args");
    uint32_t reference = DBG_GetValueAsUint32Type(cx, args, "variablesReference");
    JSValue properties = JS_NewArray(cx);
    JSValue variable = JS_GetPropertyUint32(cx, state->variableReferences, reference);
    if (JS_IsUndefined(variable)) {
        variable = DBG_ProcessUndefinedVariable(cx, state, reference, variable);
    }
    JSValue filter = JS_GetPropertyStr(cx, args, "filter");
    if (!JS_IsUndefined(filter)) {
        properties = DBG_VariablesFilteredProcess(cx, args, variable, properties, state);
    } else {
        properties = DBG_VariablesUnFilteredProcess(cx, variable, properties, state);
    }
    JS_FreeValue(cx, variable);
    JS_FreeValue(cx, args);
    DBG_SendResponseMsg(debuggerInfo, request, properties);
    JS_FreeValue(cx, properties);

    return;
}

static void DBG_EvaluateProcess(DebuggerInfo *debuggerInfo,
                                JSValue msg,
                                struct DebuggerVariableState *state)
{
    if (debuggerInfo == NULL || state == NULL) {
        DEBUGGER_LOGE("DBG_EvaluateProcess fail debuggerInfo=NULL || state=NULL");
        return;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_EvaluateProcess fail cx=NULL");
        return;
    }
    JSValue argsJsVal = JS_GetPropertyStr(cx, msg, "args");
    JSValue expressionJsVal = JS_GetPropertyStr(cx, argsJsVal, "expression");
    JS_FreeValue(cx, argsJsVal);
    int frameId;
    JSValue frameJsVal = JS_GetPropertyStr(cx, argsJsVal, "frameId");
    JS_ToInt32(cx, &frameId, frameJsVal);
    JS_FreeValue(cx, frameJsVal);
    JSValue ret = JS_DebuggerEvaluate(cx, frameId, expressionJsVal);
    if (JS_IsException(ret)) {
        DEBUGGER_LOGE("DBG_EvaluateProcess fail ret=JS_Exception");
        JS_FreeValue(cx, ret);
        ret = JS_GetException(cx);
    }
    JS_FreeValue(cx, expressionJsVal);

    JSValue body = JS_NewObject(cx);
    DBG_SetVariableProperty(cx, ret, body, "result");
    DBG_SetVariableType(cx, state, body, ret);
    DBG_SendResponseMsg(debuggerInfo, msg, body);
    JS_FreeValue(cx, body);

    return;
}

static void DBG_PauseProcess(DebuggerInfo *debuggerInfo, JSValue msg)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_PauseProcess fail debuggerInfo=NULL");
        return;
    }
    DBG_SendResponseMsg(debuggerInfo, msg, JS_UNDEFINED);
    DBG_SendStopMsg(debuggerInfo, "pause");

    return;
}

static int DBG_RequestProcess(DebuggerInfo *debuggerInfo,
                              JSValue msg,
                              struct DebuggerVariableState *state)
{
    if (debuggerInfo == NULL || state == NULL) {
        return DBG_NEED_READ_MSG;
    }
    JSContext *cx = debuggerInfo->cx;
    if (cx == NULL) {
        return DBG_NEED_READ_MSG;
    }
    const uint8_t *pc = state->curPc;
    int isNeedReadMsg = 0;
    JSValue jsRequestMsg = JS_GetPropertyStr(cx, msg, "request");
    JSValue jsCommand = JS_GetPropertyStr(cx, jsRequestMsg, "command");
    const char *command = JS_ToCString(cx, jsCommand);
    if (command == NULL) {
        DEBUGGER_LOGE("DBG_RequestProcess fail command=%s", command);
        return DBG_NEED_READ_MSG;
    }
    if (strcmp(command, "continue") == 0) {
        DBG_CotinueProcess(debuggerInfo, jsRequestMsg, pc);
    } else if (strcmp(command, "stackTrace") == 0) {
        DBG_StackTraceProcess(debuggerInfo, jsRequestMsg, pc);
        isNeedReadMsg = DBG_NEED_READ_MSG;
    } else if (strcmp(command, "scopes") == 0) {
        DBG_ScopesProcess(debuggerInfo, jsRequestMsg);
        isNeedReadMsg = DBG_NEED_READ_MSG;
    } else if (strcmp(command, "stepIn") == 0) {
        DBG_SetStepInToDebugger(debuggerInfo, jsRequestMsg, pc);
    } else if (strcmp(command, "stepOut") == 0) {
        DBG_SetStepOutToDebugger(debuggerInfo, jsRequestMsg, pc);
    } else if (strcmp(command, "next") == 0) {
        DBG_SetStepOverToDebugger(debuggerInfo, jsRequestMsg, pc);
    } else if (strcmp(command, "variables") == 0) {
        DBG_VariablesProcess(debuggerInfo, jsRequestMsg, state);
        isNeedReadMsg = DBG_NEED_READ_MSG;
    } else if (strcmp(command, "evaluate") == 0) {
        DBG_EvaluateProcess(debuggerInfo, jsRequestMsg, state);
        isNeedReadMsg = DBG_NEED_READ_MSG;
    } else if (strcmp(command, "pause") == 0) {
        DBG_PauseProcess(debuggerInfo, jsRequestMsg);
        isNeedReadMsg = DBG_NEED_READ_MSG;
    }
    JS_FreeCString(cx, command);
    JS_FreeValue(cx, jsRequestMsg);

    return isNeedReadMsg;
}

static int DBG_ProcessMsgByType(DebuggerInfo *debuggerInfo, const char *type,
                                JSValue msg, struct DebuggerVariableState *state)
{
    if (debuggerInfo == NULL || type == NULL) {
        DEBUGGER_LOGE("DBG_ProcessMsgByType fail debuggerInfo=NULL || type=%s", type);
        return DBG_NEED_READ_MSG;
    }
    if (strcmp(type, "request") == 0) {
        return  DBG_RequestProcess(debuggerInfo, msg, state);
    }
    if (strcmp(type, "breakpoints") == 0) {
        JSValue jsBreakpointsVal = JS_GetPropertyStr(debuggerInfo->cx, msg, "breakpoints");
        DBG_SetBreakpoints(debuggerInfo, jsBreakpointsVal);
        JS_FreeValue(debuggerInfo->cx, jsBreakpointsVal);
        return DBG_NEED_READ_MSG;
    }
    if (strcmp(type, "continue") == 0) {
        return DBG_NO_NEED_READ_MSG;
    }
    return DBG_NEED_READ_MSG;
}

static DebuggerVariableState DBG_GetVariableState(DebuggerInfo *debuggerInfo, const uint8_t *pc)
{
    struct DebuggerVariableState state = {0};
    state.variableReferenceCount = JS_GetStackDepth(debuggerInfo->cx) << STACK_DEPTH_MOVE_TWO_STEP;
    state.variablePointers = JS_NewObject(debuggerInfo->cx);
    state.variableReferences = JS_NewObject(debuggerInfo->cx);
    state.curPc = pc;

    return state;
}

static void DBG_ProcessMsg(DebuggerInfo *debuggerInfo, const uint8_t *pc, int runningBreakpoint)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_ProcessMsg fail debuggerInfo=NULL");
        return;
    }
    int isNeedReadMsg = DBG_NEED_READ_MSG;
    struct DebuggerVariableState state = DBG_GetVariableState(debuggerInfo, pc);
    while (isNeedReadMsg) {
        fflush(stdout);
        fflush(stderr);
        JSValue msg = DBG_ReadMsg(debuggerInfo);
        if (JS_IsUndefined(msg)) {
            DEBUGGER_LOGE("DBG_ProcessMsg fail msg=JS_Undefined");
            return;
        }
        JSValue jsType = JS_GetPropertyStr(debuggerInfo->cx, msg, "type");
        const char *type = JS_ToCString(debuggerInfo->cx, jsType);
        JS_FreeValue(debuggerInfo->cx, jsType);
        if (type == NULL) {
            DEBUGGER_LOGE("DBG_ProcessMsg fail type is NULL");
            return;
        }
        DEBUGGER_LOGI("DBG_ProcessMsg type=%s", type);
        isNeedReadMsg = DBG_ProcessMsgByType(debuggerInfo, type, msg, &state);
        if (runningBreakpoint) {
            isNeedReadMsg = DBG_NO_NEED_READ_MSG;
        }
        JS_FreeCString(debuggerInfo->cx, type);
    }
}

static void DBG_Entry(JSContext *cx, int client)
{
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_Entry fail cx=NULL");
        return;
    }
    struct DebuggerInfo *debuggerInfo = JS_GetDebuggerInfo(cx);
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_Entry fail debuggerInfo=NULL");
        return;
    }
    debuggerInfo->cx = cx;
    // debuggerInfo->client = client;
    debuggerInfo->breakpoints = JS_NewObject(cx);
    // debuggerInfo->isConnected = 1;
    DBG_SendStopMsg(debuggerInfo, "entry");
}

static int DBG_IsLocEqual(DebuggerInfo *debuggerInfo, uint32_t depth, LocInfo loc)
{
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_IsLocEqual fail debuggerInfo=NULL");
        return DBG_LOC_ISEQUAL;
    }
    if (loc.line == 0 || loc.line == -1) {
        return DBG_LOC_ISEQUAL;
    }
    DEBUGGER_LOGI("DBG_IsLocEqual last depth %d, this depth %d, last line %d, this line %d", debuggerInfo->depth,
        depth, debuggerInfo->loc.line, loc.line);
    if ((debuggerInfo->depth == depth) && (debuggerInfo->loc.line == loc.line) &&
        (debuggerInfo->loc.filename == loc.filename)) {
        return DBG_LOC_ISEQUAL;
    }

    return DBG_LOC_ISNOTEQUAL;
}

static int DBG_ProcessStepOperation(JSContext *cx, DebuggerInfo *debuggerInfo, const uint8_t *pc)
{
    if (cx == NULL || debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_ProcessStepOperation fail cx=NULL || debuggerInfo==NULL");
        return 1;
    }
    int isNeedReadMsg = 0;
    uint32_t depth = JS_GetStackDepth(cx);
    LocInfo loc = JS_GetCurrentLocation(cx, pc);
    if (debuggerInfo->stepOperation == STEP_CONTINUE) {
        if (depth == debuggerInfo->depth) {
            // Depth is equal indicates that current opcode's line is not the same as last one.
            // Clear stepOperation so that it's possible to break on the same line inside loops.
            debuggerInfo->stepOperation = NO_STEP_OPERATION;
        }
        isNeedReadMsg = 0;
        return isNeedReadMsg;
    }
    if (debuggerInfo->stepOperation == STEP_NEXT) {
        if (depth > debuggerInfo->depth || (loc.line == debuggerInfo->loc.line &&
            loc.filename == debuggerInfo->loc.filename)) {
            isNeedReadMsg = 0;
            return isNeedReadMsg;
        }
        DBG_SendStopMsg(debuggerInfo, "step");
        debuggerInfo->stepOperation = NO_STEP_OPERATION;
        isNeedReadMsg = 1;
        return isNeedReadMsg;
    }
    if (debuggerInfo->stepOperation == STEP_IN) {
        if ((loc.line == 0) || (loc.line == debuggerInfo->loc.line &&
            loc.filename == debuggerInfo->loc.filename)) {
            isNeedReadMsg = 0;
            return isNeedReadMsg;
        }
        DBG_SendStopMsg(debuggerInfo, "stepIn");
        debuggerInfo->stepOperation = NO_STEP_OPERATION;
        isNeedReadMsg = 1;
        return isNeedReadMsg;
    }
    if (debuggerInfo->stepOperation == STEP_OUT) {
        if (depth >= debuggerInfo->depth) {
            isNeedReadMsg = 0;
            return isNeedReadMsg;
        }
        DBG_SendStopMsg(debuggerInfo, "stepOut");
        debuggerInfo->stepOperation = NO_STEP_OPERATION;
        isNeedReadMsg = 1;
        return isNeedReadMsg;
    }
    debuggerInfo->stepOperation = NO_STEP_OPERATION;

    return isNeedReadMsg;
}

void DBG_FreeSources(JSContext *cx, DebuggerInfo *debuggerInfo)
{
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_FreeSources fail cx=NULL");
        return;
    }
    if (debuggerInfo->client <= 0) {
        return;
    }
    close(debuggerInfo->client);
    JS_FreeValue(cx, debuggerInfo->breakpoints);
}

void DBG_CallDebugger(JSContext *cx, const uint8_t *pc)
{
    if (cx == NULL) {
        DEBUGGER_LOGE("DBG_CallDebugger fail cx=NULL");
        return;
    }
    struct DebuggerInfo *debuggerInfo = JS_GetDebuggerInfo(cx);
    if (debuggerInfo == NULL) {
        DEBUGGER_LOGE("DBG_CallDebugger fail debuggerInfo=NULL");
        return;
    }

    if (!g_isServerStarted) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, &DBG_StartAgent, (void*)debuggerInfo) != 0) {
            DEBUGGER_LOGE("pthread_create fail!");
            return;
        }
        g_isServerStarted = true;
        if (!g_isAttachMode) {
            while (debuggerInfo->isConnected != 1) {
                usleep(DBG_WAITING_TIME);
            }
            DBG_Entry(cx, debuggerInfo->client);
            // read msg when first time must be breakpoints msg, and set breakpoints to debugger
            DBG_ProcessMsg(debuggerInfo, pc, 0);
            g_isBreakPointSet = true;
        }
    }

    if (g_isAttachMode && (debuggerInfo->isConnected == 1 && !g_isBreakPointSet)) {
            // ide attached, accept breakpoints msg
            DBG_Entry(cx, debuggerInfo->client);
            DBG_ProcessMsg(debuggerInfo, pc, 0);
            g_isBreakPointSet = true;
    }

    if (QueueIsEmpty(debuggerInfo->client) == 0) {
        DBG_ProcessMsg(debuggerInfo, pc, 1);
    }

    // when last time the debugger have step operaton, check the location is changed
    if (debuggerInfo->stepOperation != NO_STEP_OPERATION) {
        // check loction
        uint32_t depth = JS_GetStackDepth(cx);
        LocInfo loc = JS_GetCurrentLocation(cx, pc);
        if (DBG_IsLocEqual(debuggerInfo, depth, loc) == DBG_LOC_ISEQUAL) {
            return;
        }
    }
    // must check breakpotint first, then process step operation
    if (g_isBreakPointSet && JS_HitBreakpoint(cx, pc) && JS_JudgeConditionBreakPoint(cx, pc)) {
        LocInfo loc = JS_GetCurrentLocation(cx, pc);
        DEBUGGER_LOGI("DBG_CallDebugger hit breakpoint at line %d", loc.line);

        debuggerInfo->stepOperation = NO_STEP_OPERATION;
        debuggerInfo->depth = JS_GetStackDepth(cx);
        DBG_SendStopMsg(debuggerInfo, "breakpoint");
        DBG_ProcessMsg(debuggerInfo, pc, 0);
        return;
    }
    // process step operation
    if (debuggerInfo->stepOperation != NO_STEP_OPERATION) {
        // process step operation
        int isNeedReadMsg = DBG_ProcessStepOperation(cx, debuggerInfo, pc);
        if (!isNeedReadMsg) {
            return;
        }
        // continue process msg
        DBG_ProcessMsg(debuggerInfo, pc, 0);
    }

    return;
}
