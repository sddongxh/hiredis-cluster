#include "adapters/libevent.h"
#include "hircluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"

void test_command_to_single_node(redisClusterContext *cc) {
    redisReply *reply;

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    dictEntry *de = dictNext(&di);
    assert(de);
    cluster_node *node = dictGetEntryVal(de);
    assert(node);

    reply = redisClusterCommandToNode(cc, node, "DBSIZE");
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
}

void test_command_to_all_nodes(redisClusterContext *cc) {

    nodeIterator ni;
    initNodeIterator(&ni, cc);

    cluster_node *node;
    while ((node = nodeNext(&ni)) != NULL) {

        redisReply *reply;
        reply = redisClusterCommandToNode(cc, node, "DBSIZE");
        CHECK_REPLY(cc, reply);
        CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
        // printf("DBSIZE=%lld\n", reply->integer);
        freeReplyObject(reply);
    }
}

void test_transaction(redisClusterContext *cc) {

    cluster_node *node = redisClusterKeyToNode(cc, "foo");
    assert(node);

    redisReply *reply;
    reply = redisClusterCommandToNode(cc, node, "MULTI");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    reply = redisClusterCommandToNode(cc, node, "SET foo 99");
    CHECK_REPLY_QUEUED(cc, reply);
    freeReplyObject(reply);

    reply = redisClusterCommandToNode(cc, node, "INCR foo");
    CHECK_REPLY_QUEUED(cc, reply);
    freeReplyObject(reply);

    reply = redisClusterCommandToNode(cc, node, "EXEC");
    CHECK_REPLY_ARRAY(cc, reply, 2);
    CHECK_REPLY_OK(cc, reply->element[0]);
    CHECK_REPLY_INT(cc, reply->element[1], 100);
    freeReplyObject(reply);
}

void test_pipeline_to_single_node(redisClusterContext *cc) {
    int status;
    redisReply *reply;

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    dictEntry *de = dictNext(&di);
    assert(de);
    cluster_node *node = dictGetEntryVal(de);
    assert(node);

    status = redisClusterAppendCommandToNode(cc, node, "DBSIZE");
    ASSERT_MSG(status == REDIS_OK, cc->errstr);

    // Trigger send of pipeline commands
    redisClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
}

void test_pipeline_to_all_nodes(redisClusterContext *cc) {

    nodeIterator ni;
    initNodeIterator(&ni, cc);

    cluster_node *node;
    while ((node = nodeNext(&ni)) != NULL) {
        int status = redisClusterAppendCommandToNode(cc, node, "DBSIZE");
        ASSERT_MSG(status == REDIS_OK, cc->errstr);
    }

    // Get replies from 3 node cluster
    redisReply *reply;
    redisClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
    freeReplyObject(reply);

    redisClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
    freeReplyObject(reply);

    redisClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, REDIS_REPLY_INTEGER);
    freeReplyObject(reply);

    redisClusterGetReply(cc, (void *)&reply);
    assert(reply == NULL);
}

void test_pipeline_transaction(redisClusterContext *cc) {
    int status;
    redisReply *reply;

    cluster_node *node = redisClusterKeyToNode(cc, "foo");
    assert(node);

    status = redisClusterAppendCommandToNode(cc, node, "MULTI");
    ASSERT_MSG(status == REDIS_OK, cc->errstr);
    status = redisClusterAppendCommandToNode(cc, node, "SET foo 199");
    ASSERT_MSG(status == REDIS_OK, cc->errstr);
    status = redisClusterAppendCommandToNode(cc, node, "INCR foo");
    ASSERT_MSG(status == REDIS_OK, cc->errstr);
    status = redisClusterAppendCommandToNode(cc, node, "EXEC");
    ASSERT_MSG(status == REDIS_OK, cc->errstr);

    // Trigger send of pipeline commands
    {
        redisClusterGetReply(cc, (void *)&reply); // MULTI
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);

        redisClusterGetReply(cc, (void *)&reply); // SET
        CHECK_REPLY_QUEUED(cc, reply);
        freeReplyObject(reply);

        redisClusterGetReply(cc, (void *)&reply); // INCR
        CHECK_REPLY_QUEUED(cc, reply);
        freeReplyObject(reply);

        redisClusterGetReply(cc, (void *)&reply); // EXEC
        CHECK_REPLY_ARRAY(cc, reply, 2);
        CHECK_REPLY_OK(cc, reply->element[0]);
        CHECK_REPLY_INT(cc, reply->element[1], 200);
        freeReplyObject(reply);
    }
}

//------------------------------------------------------------------------------
// Async API
//------------------------------------------------------------------------------
typedef struct ExpectedResult {
    int type;
    char *str;
    bool disconnect;
    bool noreply;
    char *errstr;
} ExpectedResult;

// Callback for Redis connects and disconnects
void callbackExpectOk(const redisAsyncContext *ac, int status) {
    UNUSED(ac);
    assert(status == REDIS_OK);
}

// Callback for async commands, verifies the redisReply
void commandCallback(redisClusterAsyncContext *cc, void *r, void *privdata) {
    redisReply *reply = (redisReply *)r;
    ExpectedResult *expect = (ExpectedResult *)privdata;
    if (expect->noreply) {
        assert(reply == NULL);
        assert(strcmp(cc->errstr, expect->errstr) == 0);
    } else {
        assert(reply != NULL);
        assert(reply->type == expect->type);
        if (reply->type != REDIS_REPLY_INTEGER)
            assert(strcmp(reply->str, expect->str) == 0);
    }
    if (expect->disconnect)
        redisClusterAsyncDisconnect(cc);
}

void test_async_to_single_node() {
    int status;

    redisClusterAsyncContext *acc = redisClusterAsyncContextInit();
    assert(acc);
    redisClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    redisClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    redisClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);
    redisClusterSetOptionMaxRedirect(acc->cc, 1);
    redisClusterSetOptionRouteUseSlots(acc->cc);
    status = redisClusterConnect2(acc->cc);
    ASSERT_MSG(status == REDIS_OK, acc->errstr);

    struct event_base *base = event_base_new();
    status = redisClusterLibeventAttach(acc, base);
    assert(status == REDIS_OK);

    dictIterator di;
    dictInitIterator(&di, acc->cc->nodes);

    dictEntry *de = dictNext(&di);
    assert(de);
    cluster_node *node = dictGetEntryVal(de);
    assert(node);

    ExpectedResult r1 = {.type = REDIS_REPLY_INTEGER, .disconnect = true};
    status = redisClusterAsyncCommandToNode(acc, node, commandCallback, &r1,
                                            "DBSIZE");
    ASSERT_MSG(status == REDIS_OK, acc->errstr);

    event_base_dispatch(base);

    redisClusterAsyncFree(acc);
    event_base_free(base);
}

void test_async_to_all_nodes() {
    int status;

    redisClusterAsyncContext *acc = redisClusterAsyncContextInit();
    assert(acc);
    redisClusterAsyncSetConnectCallback(acc, callbackExpectOk);
    redisClusterAsyncSetDisconnectCallback(acc, callbackExpectOk);
    redisClusterSetOptionAddNodes(acc->cc, CLUSTER_NODE);
    redisClusterSetOptionMaxRedirect(acc->cc, 1);
    redisClusterSetOptionRouteUseSlots(acc->cc);
    status = redisClusterConnect2(acc->cc);
    ASSERT_MSG(status == REDIS_OK, acc->errstr);

    struct event_base *base = event_base_new();
    status = redisClusterLibeventAttach(acc, base);
    assert(status == REDIS_OK);

    nodeIterator ni;
    initNodeIterator(&ni, acc->cc);

    ExpectedResult r1 = {.type = REDIS_REPLY_INTEGER};

    cluster_node *node;
    while ((node = nodeNext(&ni)) != NULL) {

        status = redisClusterAsyncCommandToNode(acc, node, commandCallback, &r1,
                                                "DBSIZE");
        ASSERT_MSG(status == REDIS_OK, acc->errstr);
    }

    // Normal command to trigger disconnect
    ExpectedResult r2 = {
        .type = REDIS_REPLY_STATUS, .str = "OK", .disconnect = true};
    status = redisClusterAsyncCommand(acc, commandCallback, &r2, "SET foo bar");

    event_base_dispatch(base);

    redisClusterAsyncFree(acc);
    event_base_free(base);
}

int main() {
    int status;

    redisClusterContext *cc = redisClusterContextInit();
    assert(cc);
    redisClusterSetOptionAddNodes(cc, CLUSTER_NODE);
    redisClusterSetOptionRouteUseSlots(cc);
    redisClusterSetOptionMaxRedirect(cc, 1);
    status = redisClusterConnect2(cc);
    ASSERT_MSG(status == REDIS_OK, cc->errstr);

    // Synchronous API
    test_command_to_single_node(cc);
    test_command_to_all_nodes(cc);
    test_transaction(cc);

    // Pipeline API
    test_pipeline_to_single_node(cc);
    test_pipeline_to_all_nodes(cc);
    test_pipeline_transaction(cc);

    redisClusterFree(cc);

    // Asynchronous API
    test_async_to_single_node();
    test_async_to_all_nodes();

    return 0;
}