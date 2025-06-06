/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/metrics/field_names/sharding_data_transform_instance_metrics_field_name_provider.h"
#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics.h"
#include "mongo/db/s/metrics/sharding_data_transform_metrics_observer_interface.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

class ShardingDataTransformInstanceMetrics {
public:
    using Role = ShardingDataTransformMetrics::Role;
    using ObserverPtr = std::unique_ptr<ShardingDataTransformMetricsObserverInterface>;
    using FieldNameProviderPtr =
        std::unique_ptr<ShardingDataTransformInstanceMetricsFieldNameProvider>;

    ShardingDataTransformInstanceMetrics(UUID instanceId,
                                         BSONObj originalCommand,
                                         NamespaceString sourceNs,
                                         Role role,
                                         Date_t startTime,
                                         ClockSource* clockSource,
                                         ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                         FieldNameProviderPtr fieldNames);

    ShardingDataTransformInstanceMetrics(UUID instanceId,
                                         BSONObj originalCommand,
                                         NamespaceString sourceNs,
                                         Role role,
                                         Date_t startTime,
                                         ClockSource* clockSource,
                                         ShardingDataTransformCumulativeMetrics* cumulativeMetrics,
                                         FieldNameProviderPtr fieldNames,
                                         ObserverPtr observer);
    virtual ~ShardingDataTransformInstanceMetrics() = default;

    virtual BSONObj reportForCurrentOp() const;

    boost::optional<Milliseconds> getHighEstimateRemainingTimeMillis() const;
    boost::optional<Milliseconds> getLowEstimateRemainingTimeMillis() const;
    Date_t getStartTimestamp() const;
    const UUID& getInstanceId() const;

    void onStarted();
    void onSuccess();
    void onFailure();
    void onCanceled();

    void onDocumentsProcessed(int64_t documentCount,
                              int64_t totalDocumentsSizeBytes,
                              Milliseconds elapsed);
    int64_t getDocumentsProcessedCount() const;
    int64_t getBytesWrittenCount() const;
    int64_t getApproxBytesToScanCount() const;
    int64_t getWritesDuringCriticalSection() const;

    void setDocumentsToProcessCounts(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void setCoordinatorHighEstimateRemainingTimeMillis(Milliseconds milliseconds);
    void setCoordinatorLowEstimateRemainingTimeMillis(Milliseconds milliseconds);

    void onCloningRemoteBatchRetrieval(Milliseconds elapsed);
    void onWriteToStashedCollections();

    void onReadDuringCriticalSection();
    void onWriteDuringCriticalSection();

    Role getRole() const;
    Seconds getOperationRunningTimeSecs() const;

    void setLastOpEndingChunkImbalance(int64_t imbalanceCount);

    ReshardingCumulativeMetrics::AnyState getState() const {
        return _state.load();
    }

    template <typename T>
    void onStateTransition(T before, boost::none_t after) {
        getTypedCumulativeMetrics()->template onStateTransition<T>(before, boost::none);
    }

    template <typename T>
    void onStateTransition(boost::none_t before, T after) {
        setState(after);
        getTypedCumulativeMetrics()->template onStateTransition<T>(boost::none, after);
    }

    template <typename T>
    void onStateTransition(T before, T after) {
        setState(after);
        getTypedCumulativeMetrics()->template onStateTransition<T>(before, after);
    }

    void onInsertApplied();
    void onUpdateApplied();
    void onDeleteApplied();
    void onOplogEntriesFetched(int64_t numEntries);
    void onOplogEntriesApplied(int64_t numEntries);

    void onBatchRetrievedDuringOplogFetching(Milliseconds elapsed);
    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsed);
    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsed);
    void onOplogLocalBatchApplied(Milliseconds elapsed);

protected:
    static constexpr auto kNoDate = Date_t::min();
    using UniqueScopedObserver = ShardingDataTransformCumulativeMetrics::UniqueScopedObserver;

    template <typename T>
    T getElapsed(const AtomicWord<Date_t>& startTime,
                 const AtomicWord<Date_t>& endTime,
                 ClockSource* clock) const {
        auto start = startTime.load();
        if (start == kNoDate) {
            return T{0};
        }
        auto end = endTime.load();
        if (end == kNoDate) {
            end = clock->now();
        }
        return duration_cast<T>(end - start);
    }

    template <typename T>
    void setState(T state) {
        static_assert(std::is_assignable_v<ReshardingCumulativeMetrics::AnyState, T>);
        _state.store(state);
    }

    void restoreDocumentsProcessed(int64_t documentCount, int64_t totalDocumentsSizeBytes);
    void restoreWritesToStashCollections(int64_t writesToStashCollections);
    virtual std::string createOperationDescription() const;
    virtual StringData getStateString() const;
    virtual boost::optional<Milliseconds> getRecipientHighEstimateRemainingTimeMillis() const = 0;

    ShardingDataTransformCumulativeMetrics* getCumulativeMetrics();
    ReshardingCumulativeMetrics* getTypedCumulativeMetrics();
    ClockSource* getClockSource() const;
    UniqueScopedObserver registerInstanceMetrics();

    int64_t getInsertsApplied() const;
    int64_t getUpdatesApplied() const;
    int64_t getDeletesApplied() const;
    int64_t getOplogEntriesFetched() const;
    int64_t getOplogEntriesApplied() const;
    void restoreInsertsApplied(int64_t count);
    void restoreUpdatesApplied(int64_t count);
    void restoreDeletesApplied(int64_t count);
    void restoreOplogEntriesFetched(int64_t count);
    void restoreOplogEntriesApplied(int64_t count);

    template <typename FieldNameProvider>
    void reportOplogApplicationCountMetrics(const FieldNameProvider* names,
                                            BSONObjBuilder* bob) const {

        bob->append(names->getForOplogEntriesFetched(), getOplogEntriesFetched());
        bob->append(names->getForOplogEntriesApplied(), getOplogEntriesApplied());
        bob->append(names->getForInsertsApplied(), getInsertsApplied());
        bob->append(names->getForUpdatesApplied(), getUpdatesApplied());
        bob->append(names->getForDeletesApplied(), getDeletesApplied());
    }

    const UUID _instanceId;
    const BSONObj _originalCommand;
    const NamespaceString _sourceNs;
    const Role _role;
    FieldNameProviderPtr _fieldNames;

private:
    const Date_t _startTime;

    ClockSource* _clockSource;
    ObserverPtr _observer;
    ShardingDataTransformCumulativeMetrics* _cumulativeMetrics;

    AtomicWord<int64_t> _approxDocumentsToProcess;
    AtomicWord<int64_t> _documentsProcessed;
    AtomicWord<int64_t> _approxBytesToScan;
    AtomicWord<int64_t> _bytesWritten;

    AtomicWord<int64_t> _writesToStashCollections;

    AtomicWord<Milliseconds> _coordinatorHighEstimateRemainingTimeMillis;
    AtomicWord<Milliseconds> _coordinatorLowEstimateRemainingTimeMillis;

    AtomicWord<int64_t> _readsDuringCriticalSection;
    AtomicWord<int64_t> _writesDuringCriticalSection;

    AtomicWord<ReshardingCumulativeMetrics::AnyState> _state;

    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _oplogEntriesFetched{0};
};

}  // namespace mongo
