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

#include "message_server.h"

#include <fstream>
#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "debugger.h"
#include "securec.h"

static std::mutex g_mtx;
static std::list<std::string> g_readMsgQueue;
static constexpr int MSG_LEN = 9;
static constexpr int MAX_CONNECT_CLIENT_NUM = 1;

bool DBG_CopyComponentNameFromAce(const char *srcStr, char *destStr, int maxLen)
{
    if (strcpy_s(destStr, maxLen, srcStr) != 0) {
        DEBUGGER_LOGE("GetInstanceName strcpy_s fail");
        return false;
    }
    return true;
}

const char* QueueFront()
{
    g_mtx.lock();
    const char* res = g_readMsgQueue.front().c_str();
    g_mtx.unlock();
    return res;
}

void QueuePop()
{
    g_mtx.lock();
    g_readMsgQueue.pop_front();
    g_mtx.unlock();
}

int QueueIsEmpty()
{
    g_mtx.lock();
    int ret = g_readMsgQueue.empty();
    g_mtx.unlock();
    return ret;
}

static int DBG_SocketRead(int client, char *buf, int len)
{
    if (client <= 0 || len == 0 || buf == nullptr) {
        DEBUGGER_LOGE("DBG_SocketRead fail client=%d, len=%d, buf=%s", client, len, buf);
        return 0;
    }

    int loc = 0;
    while (loc < len) {
        int ret = TEMP_FAILURE_RETRY(read(client, reinterpret_cast<void*>(buf + loc), len - loc));
        if (ret <= 0 || ret > (len - loc)) {
            return 0;
        }
        loc = loc + ret;
    }

    return 1;
}

static int DBG_ReadMsgLen(int client)
{
    char msgLenChar[MSG_LEN] = {0};
    int readRet = DBG_SocketRead(client, msgLenChar, MSG_LEN);
    if (readRet != 1) {
        return 0;
    }
    msgLenChar[MSG_LEN - 1] = '\0';
    // turn str to hex
    const int base = 16;
    int msgLen = strtol(msgLenChar, nullptr, base);
    if (msgLen <= 0) {
        DEBUGGER_LOGE("DBG_ReadMsgLen fail msgLen=%d", msgLen);
        return 0;
    }

    return msgLen;
}

static void ReadMsg(int client)
{
    int msgLen = DBG_ReadMsgLen(client);
    if (msgLen <= 0) {
        return;
    }
    std::unique_ptr<char[]> msgBuf = std::make_unique<char[]>(msgLen + 1);
    if (msgBuf == nullptr) {
        DEBUGGER_LOGE("DBG_ReadMsg fail msgBuf=nullptr");
        return;
    }

    int readRet = DBG_SocketRead(client, msgBuf.get(), msgLen);
    if (!readRet) {
        DEBUGGER_LOGE("DBG_ReadMsg fail readRet=%d", readRet);
        return;
    }

    g_mtx.lock();
    msgBuf.get()[msgLen] = '\0';
    DEBUGGER_LOGI("dbg msg %s", msgBuf.get());
    std::string message(msgBuf.get());
    g_readMsgQueue.push_back(message);
    g_mtx.unlock();

    return;
}

static std::string g_componentName;

void DBG_SetComponentName(const char *name, int size)
{
    if (size <= 0) {
        DEBUGGER_LOGE("DBG_SetComponentName fail");
        return;
    }
    g_componentName = name;
}

static std::string DBG_GetComponentName()
{
    if (g_componentName.empty()) {
        DEBUGGER_LOGE("DBG_GetComponentName fail");
    }
    return g_componentName;
}

static int DBG_StartServer()
{
    DEBUGGER_LOGI("DBG_StartServer");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    int appPid = getpid();
    std::string pidStr = std::to_string(appPid);
    std::string component = DBG_GetComponentName();
    std::string sockName = pidStr + component;
    struct sockaddr_un  un;
    if (memset_s(&un, sizeof(un), 0, sizeof(un)) != EOK) {
        DEBUGGER_LOGE("DBG_StartServer memset_s fail");
        close(fd);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    un.sun_family = AF_UNIX;
    if (strcpy_s(un.sun_path + 1, sizeof(un.sun_path) - 1, sockName.c_str()) != EOK) {
        DEBUGGER_LOGE("DBG_StartServer strcpy_s fail");
        close(fd);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    un.sun_path[0] = '\0';
    int len = offsetof(struct sockaddr_un, sun_path) + strlen(sockName.c_str()) + 1;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&un), len) < 0) {
        close(fd);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    if (listen(fd, MAX_CONNECT_CLIENT_NUM) < 0) {
        close(fd);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    int client = 0;
    if ((client = accept(fd, nullptr, nullptr)) < 0) {
        close(fd);
        return FAIL_CAUSE_SOCKET_COMMON_FAIL;
    }
    return client;
}

void *DBG_StartAgent(void *args)
{
    int client = DBG_StartServer();
    DEBUGGER_LOGI("DBG_StartAgent client = %d", client);
    if (client < 0) {
        DEBUGGER_LOGE("DBG_StartAgent fail");
        return nullptr;
    }
    DBG_SetConnectFlag(1, client);
    while (true) {
        ReadMsg(client);
    }
}
