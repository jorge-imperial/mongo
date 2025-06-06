/**
 * Tests that change streams on a collection can be resumed during and after the given collection is
 * unsharded.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   uses_atclustertime,
 *   requires_fcv_80,
 *   featureFlagUnshardCollection,
 * ]
 */

import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

// Use a higher frequency for periodic noops to speed up the test.
const reshardingTest = new ReshardingTest(
    {numDonors: 2, numRecipients: 1, periodicNoopIntervalSecs: 1, writePeriodicNoops: true});
reshardingTest.setup();

const kDbName = "unshardDb";
const collName = "coll";
const ns = kDbName + "." + collName;

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]}
    ],
    primaryShardName: donorShardNames[0]
});

const mongos = sourceCollection.getMongo();
const unshardDb = mongos.getDB(kDbName);

const cst = new ChangeStreamTest(unshardDb);

// Open a change streams cursor on the collection that will be resharded.
let changeStreamsCursor =
    cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collName});
assert.eq([], changeStreamsCursor.firstBatch, "Expected cursor not to have changes, but it did");

// We want to confirm that change streams can see events before, during, and after the resharding
// operation. Note in particular that this test confirms that a regular user change stream does
// NOT see internal resharding events such as reshardBegin.
const expectedChanges = [
    {
        documentKey: {_id: 0, oldKey: 0},
        fullDocument: {_id: 0, oldKey: 0},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {oldKey: 1, _id: 1},
        fullDocument: {_id: 1, oldKey: 1},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {oldKey: 2, _id: 2},
        fullDocument: {_id: 2, oldKey: 2},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 3},
        fullDocument: {_id: 3, oldKey: 3},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    },
    {
        documentKey: {_id: 4},
        fullDocument: {_id: 4, oldKey: 4},
        ns: {db: kDbName, coll: collName},
        operationType: "insert",
    }
];
const preUnshardCollectionChange = expectedChanges[0];
const midUnshardCollectionChanges = expectedChanges.slice(1, 3);
const postUnshardCollectionChanges = expectedChanges.slice(3);

// Verify that the cursor sees changes before the collection is resharded.
assert.commandWorked(sourceCollection.insert({_id: 0, oldKey: 0}));
const preUnshardCollectionResumeToken =
    cst.assertNextChangesEqual(
           {cursor: changeStreamsCursor, expectedChanges: [preUnshardCollectionChange]})[0]
        ._id;

const recipientShardNames = reshardingTest.recipientShardNames;
let midUnshardCollectionResumeToken;
let changeStreamsCursor2;
reshardingTest.withUnshardCollectionInBackground({toShard: recipientShardNames[0]}, () => {
    // Wait until participants are aware of the resharding operation.
    reshardingTest.awaitCloneTimestampChosen();

    // Open another change streams cursor while the collection is being resharded.
    changeStreamsCursor2 =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collName});

    assert.commandWorked(sourceCollection.insert({_id: 1, oldKey: 1}));
    assert.commandWorked(sourceCollection.insert({_id: 2, oldKey: 2}));

    // Assert that both the cursors see the two new inserts.
    cst.assertNextChangesEqual(
        {cursor: changeStreamsCursor, expectedChanges: midUnshardCollectionChanges});
    cst.assertNextChangesEqual(
        {cursor: changeStreamsCursor2, expectedChanges: midUnshardCollectionChanges});

    // Check that we can resume from the token returned before resharding began.
    let resumedCursor = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: preUnshardCollectionResumeToken}}],
        collection: collName
    });
    midUnshardCollectionResumeToken =
        cst.assertNextChangesEqual(
               {cursor: resumedCursor, expectedChanges: midUnshardCollectionChanges})[1]
            ._id;
});

assert.commandWorked(sourceCollection.insert({_id: 3, oldKey: 3}));

// Assert that both the cursor opened before resharding started and the one opened during
// resharding see the insert after resharding has finished.
cst.assertNextChangesEqual(
    {cursor: changeStreamsCursor, expectedChanges: [postUnshardCollectionChanges[0]]});
cst.assertNextChangesEqual(
    {cursor: changeStreamsCursor2, expectedChanges: [postUnshardCollectionChanges[0]]});

// Check that we can resume from both the token returned before resharding began and the token
// returned during resharding.
let resumedCursorFromPreOperation = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: preUnshardCollectionResumeToken}}],
    collection: collName
});
let midAndPostUnshardCollectionChanges =
    midUnshardCollectionChanges.concat(postUnshardCollectionChanges);

let resumedCursorFromMidOperation = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: midUnshardCollectionResumeToken}}],
    collection: collName
});

assert.commandWorked(sourceCollection.insert({_id: 4, oldKey: 4}));

cst.assertNextChangesEqual(
    {cursor: resumedCursorFromPreOperation, expectedChanges: midAndPostUnshardCollectionChanges});
cst.assertNextChangesEqual(
    {cursor: resumedCursorFromMidOperation, expectedChanges: postUnshardCollectionChanges});

reshardingTest.teardown();
