#pragma once

#include "kqp_executer.h"
#include "kqp_executer_stats.h"
#include "kqp_planner.h"
#include "kqp_partition_helper.h"
#include "kqp_table_resolver.h"
#include "kqp_shards_resolver.h"


#include <ydb/core/kqp/common/kqp_ru_calc.h>
#include <ydb/core/kqp/common/kqp_lwtrace_probes.h>

#include <ydb/core/actorlib_impl/long_timer.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/library/ydb_issue/issue_helpers.h>
#include <ydb/core/protos/tx_datashard.pb.h>
#include <ydb/core/protos/pqconfig.pb.h>
#include <ydb/core/kqp/executer_actor/kqp_tasks_graph.h>
#include <ydb/core/kqp/node_service/kqp_node_service.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/common/kqp_user_request_context.h>
#include <ydb/core/grpc_services/local_rate_limiter.h>

#include <ydb/services/metadata/secret/fetcher.h>
#include <ydb/services/metadata/secret/snapshot.h>

#include <ydb/library/mkql_proto/mkql_proto.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor.h>
#include <ydb/library/yql/dq/proto/dq_transport.pb.h>
#include <ydb/library/yql/dq/proto/dq_tasks.pb.h>
#include <ydb/library/yql/dq/runtime/dq_transport.h>
#include <ydb/library/yql/providers/common/http_gateway/yql_http_gateway.h>
#include <ydb/library/yql/providers/common/structured_token/yql_token_builder.h>
#include <ydb/library/yql/public/issue/yql_issue.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/wilson/wilson_span.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>

#include <util/generic/size_literals.h>


LWTRACE_USING(KQP_PROVIDER);

namespace NKikimr {
namespace NKqp {

#define LOG_T(stream) LOG_TRACE_S(*TlsActivationContext,  NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_D(stream) LOG_DEBUG_S(*TlsActivationContext,  NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_I(stream) LOG_INFO_S(*TlsActivationContext,   NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_N(stream) LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_W(stream) LOG_WARN_S(*TlsActivationContext,   NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_E(stream) LOG_ERROR_S(*TlsActivationContext,  NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)
#define LOG_C(stream) LOG_CRIT_S(*TlsActivationContext,   NKikimrServices::KQP_EXECUTER, "ActorId: " << SelfId() << " TxId: " << TxId << ". " << "Ctx: " << *GetUserRequestContext() << ". " << stream)

enum class EExecType {
    Data,
    Scan
};

const ui64 MaxTaskSize = 48_MB;

std::pair<TString, TString> SerializeKqpTasksParametersForOlap(const TStageInfo& stageInfo, const TTask& task);

inline bool IsDebugLogEnabled() {
    return TlsActivationContext->LoggerSettings() &&
           TlsActivationContext->LoggerSettings()->Satisfies(NActors::NLog::PRI_DEBUG, NKikimrServices::KQP_EXECUTER);
}

TActorId ReportToRl(ui64 ru, const TString& database, const TString& userToken,
    const NKikimrKqp::TRlPath& path);

struct TEvPrivate {
    enum EEv {
        EvRetry = EventSpaceBegin(TEvents::ES_PRIVATE),
        EvResourcesSnapshot,
        EvReattachToShard,
    };

    struct TEvRetry : public TEventLocal<TEvRetry, EEv::EvRetry> {
        ui32 RequestId;
        TActorId Target;

        TEvRetry(ui64 requestId, const TActorId& target)
            : RequestId(requestId)
            , Target(target) {}
    };

    struct TEvResourcesSnapshot : public TEventLocal<TEvResourcesSnapshot, EEv::EvResourcesSnapshot> {
        TVector<NKikimrKqp::TKqpNodeResources> Snapshot;

        TEvResourcesSnapshot(TVector<NKikimrKqp::TKqpNodeResources>&& snapshot)
            : Snapshot(std::move(snapshot)) {}
    };

    struct TEvReattachToShard : public TEventLocal<TEvReattachToShard, EvReattachToShard> {
        const ui64 TabletId;

        explicit TEvReattachToShard(ui64 tabletId)
            : TabletId(tabletId) {}
    };
};

template <class TDerived, EExecType ExecType>
class TKqpExecuterBase : public TActorBootstrapped<TDerived> {
public:
    TKqpExecuterBase(IKqpGateway::TExecPhysicalRequest&& request, const TString& database,
        const TIntrusiveConstPtr<NACLib::TUserToken>& userToken,
        TKqpRequestCounters::TPtr counters,
        const NKikimrConfig::TTableServiceConfig::TExecuterRetriesConfig& executerRetriesConfig,
        const NKikimrConfig::TTableServiceConfig::EChannelTransportVersion chanTransportVersion,
        const NKikimrConfig::TTableServiceConfig::TAggregationConfig& aggregation,
        TDuration maximalSecretsSnapshotWaitTime, const TIntrusivePtr<TUserRequestContext>& userRequestContext,
        ui64 spanVerbosity = 0, TString spanName = "no_name")
        : Request(std::move(request))
        , Database(database)
        , UserToken(userToken)
        , Counters(counters)
        , ExecuterSpan(spanVerbosity, std::move(Request.TraceId), spanName)
        , Planner(nullptr)
        , ExecuterRetriesConfig(executerRetriesConfig)
        , MaximalSecretsSnapshotWaitTime(maximalSecretsSnapshotWaitTime)
        , AggregationSettings(aggregation)
        , HasOlapTable(false)
    {
        TasksGraph.GetMeta().Snapshot = IKqpGateway::TKqpSnapshot(Request.Snapshot.Step, Request.Snapshot.TxId);
        TasksGraph.GetMeta().Arena = MakeIntrusive<NActors::TProtoArenaHolder>();
        TasksGraph.GetMeta().Database = Database;
        TasksGraph.GetMeta().ChannelTransportVersion = chanTransportVersion;
        TasksGraph.GetMeta().UserRequestContext = userRequestContext;
        ResponseEv = std::make_unique<TEvKqpExecuter::TEvTxResponse>(Request.TxAlloc);
        ResponseEv->Orbit = std::move(Request.Orbit);
        Stats = std::make_unique<TQueryExecutionStats>(Request.StatsMode, &TasksGraph,
            ResponseEv->Record.MutableResponse()->MutableResult()->MutableStats());
    }

    void Bootstrap() {
        StartTime = TAppData::TimeProvider->Now();
        if (Request.Timeout) {
            Deadline = StartTime + Request.Timeout;
        }
        if (Request.CancelAfter) {
            CancelAt = StartTime + *Request.CancelAfter;
        }

        LOG_T("Bootstrap done, become ReadyState");
        this->Become(&TKqpExecuterBase::ReadyState);
        ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::ExecuterReadyState, ExecuterSpan.GetTraceId(), "ReadyState", NWilson::EFlags::AUTO_END);
    }

    TActorId SelfId() {
       return TActorBootstrapped<TDerived>::SelfId();
    }

    void ReportEventElapsedTime() {
        if (Stats) {
            ui64 elapsedMicros = TlsActivationContext->GetCurrentEventTicksAsSeconds() * 1'000'000;
            Stats->ExecuterCpuTime += TDuration::MicroSeconds(elapsedMicros);
        }
    }

protected:
    const NMiniKQL::TTypeEnvironment& TypeEnv() {
        return Request.TxAlloc->TypeEnv;
    }

    const NMiniKQL::THolderFactory& HolderFactory() {
        return Request.TxAlloc->HolderFactory;
    }

    [[nodiscard]]
    bool HandleResolve(TEvKqpExecuter::TEvTableResolveStatus::TPtr& ev) {
        auto& reply = *ev->Get();

        KqpTableResolverId = {};

        if (reply.Status != Ydb::StatusIds::SUCCESS) {
            ReplyErrorAndDie(reply.Status, reply.Issues);
            return false;
        }

        if (ExecuterTableResolveSpan) {
            ExecuterTableResolveSpan.End();
        }

        return true;
    }

    [[nodiscard]]
    bool HandleResolve(TEvKqpExecuter::TEvShardsResolveStatus::TPtr& ev) {
        auto& reply = *ev->Get();

        KqpShardsResolverId = {};

        // TODO: count resolve time in CpuTime

        if (reply.Status != Ydb::StatusIds::SUCCESS) {
            LOG_W("Shards nodes resolve failed, status: " << Ydb::StatusIds_StatusCode_Name(reply.Status)
                << ", issues: " << reply.Issues.ToString());
            ReplyErrorAndDie(reply.Status, reply.Issues);
            return false;
        }

        LOG_D("Shards nodes resolved, success: " << reply.ShardNodes.size() << ", failed: " << reply.Unresolved);

        ShardIdToNodeId = std::move(reply.ShardNodes);
        for (auto& [shardId, nodeId] : ShardIdToNodeId) {
            ShardsOnNode[nodeId].push_back(shardId);
        }

        if (IsDebugLogEnabled()) {
            TStringBuilder sb;
            sb << "Shards on nodes: " << Endl;
            for (auto& pair : ShardsOnNode) {
                sb << "  node " << pair.first << ": [";
                if (pair.second.size() <= 20) {
                    sb << JoinSeq(", ", pair.second) << "]" << Endl;
                } else {
                    sb << JoinRange(", ", pair.second.begin(), std::next(pair.second.begin(), 20)) << ", ...] "
                       << "(total " << pair.second.size() << ") " << Endl;
                }
            }
            LOG_D(sb);
        }
        return true;
    }

    void HandleComputeStats(NYql::NDq::TEvDqCompute::TEvState::TPtr& ev) {
        TActorId computeActor = ev->Sender;
        auto& state = ev->Get()->Record;
        ui64 taskId = state.GetTaskId();

        LOG_D("ActorState: " << CurrentStateFuncName()
            << ", got execution state from compute actor: " << computeActor
            << ", task: " << taskId
            << ", state: " << NYql::NDqProto::EComputeState_Name((NYql::NDqProto::EComputeState) state.GetState())
            << ", stats: " << state.GetStats());

        switch (state.GetState()) {
            case NYql::NDqProto::COMPUTE_STATE_UNKNOWN: {
                YQL_ENSURE(false, "unexpected state from " << computeActor << ", task: " << taskId);
                return;
            }

            case NYql::NDqProto::COMPUTE_STATE_FAILURE: {
                ReplyErrorAndDie(NYql::NDq::DqStatusToYdbStatus(state.GetStatusCode()), state.MutableIssues());
                return;
            }

            case NYql::NDqProto::COMPUTE_STATE_EXECUTING: {
                // initial TEvState event from Compute Actor
                // there can be race with RM answer
                if (Planner) {
                    if (Planner->GetPendingComputeTasks().erase(taskId)) {
                        auto it = Planner->GetPendingComputeActors().emplace(computeActor, TProgressStat());
                        YQL_ENSURE(it.second);

                        if (state.HasStats()) {
                            it.first->second.Set(state.GetStats());
                        }

                        auto& task = TasksGraph.GetTask(taskId);
                        task.ComputeActorId = computeActor;

                        THashMap<TActorId, THashSet<ui64>> updates;
                        CollectTaskChannelsUpdates(task, updates);
                        PropagateChannelsUpdates(updates);
                    } else {
                        auto it = Planner->GetPendingComputeActors().find(computeActor);
                        if (it != Planner->GetPendingComputeActors().end()) {
                            if (state.HasStats()) {
                                it->second.Set(state.GetStats());
                            }
                        }
                    }
                }
                break;
            }

            case NYql::NDqProto::COMPUTE_STATE_FINISHED: {
                if (Stats) {
                    Stats->AddComputeActorStats(computeActor.NodeId(), std::move(*state.MutableStats()));
                }
                ExtraData[computeActor].Swap(state.MutableExtraData());

                LastTaskId = taskId;
                LastComputeActorId = computeActor.ToString();

                if (Planner) {
                    auto it = Planner->GetPendingComputeActors().find(computeActor);
                    if (it == Planner->GetPendingComputeActors().end()) {
                        LOG_W("Got execution state for compute actor: " << computeActor
                            << ", task: " << taskId
                            << ", state: " << NYql::NDqProto::EComputeState_Name((NYql::NDqProto::EComputeState) state.GetState())
                            << ", too early (waiting reply from RM)");

                        if (Planner && Planner->GetPendingComputeTasks().erase(taskId)) {
                            LOG_E("Got execution state for compute actor: " << computeActor
                                << ", for unknown task: " << state.GetTaskId()
                                << ", state: " << NYql::NDqProto::EComputeState_Name((NYql::NDqProto::EComputeState) state.GetState()));
                            return;
                        }
                    } else {
                        if (state.HasStats()) {
                            it->second.Set(state.GetStats());
                        }
                        LastStats.emplace_back(std::move(it->second));
                        Planner->GetPendingComputeActors().erase(it);
                        YQL_ENSURE(Planner->GetPendingComputeTasks().find(taskId) == Planner->GetPendingComputeTasks().end());
                    }
                }
            }
        }

        static_cast<TDerived*>(this)->CheckExecutionComplete();
    }

    STATEFN(ReadyState) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvKqpExecuter::TEvTxRequest, HandleReady);
            hFunc(TEvKqp::TEvAbortExecution, HandleAbortExecution);
            default: {
                UnexpectedEvent("ReadyState", ev->GetTypeRewrite());
            }
        }
        ReportEventElapsedTime();
    }

    void HandleReady(TEvKqpExecuter::TEvTxRequest::TPtr& ev) {
        TxId = ev->Get()->Record.GetRequest().GetTxId();
        Target = ActorIdFromProto(ev->Get()->Record.GetTarget());

        auto lockTxId = Request.AcquireLocksTxId;
        if (lockTxId.Defined() && *lockTxId == 0) {
            lockTxId = TxId;
        }

        TasksGraph.GetMeta().SetLockTxId(lockTxId);

        LWTRACK(KqpBaseExecuterHandleReady, ResponseEv->Orbit, TxId);
        if (IsDebugLogEnabled()) {
            for (auto& tx : Request.Transactions) {
                LOG_D("Executing physical tx, type: " << (ui32) tx.Body->GetType() << ", stages: " << tx.Body->StagesSize());
            }
        }

        auto kqpTableResolver = CreateKqpTableResolver(this->SelfId(), TxId, UserToken, Request.Transactions,
            TasksGraph);
        KqpTableResolverId = this->RegisterWithSameMailbox(kqpTableResolver);

        LOG_T("Got request, become WaitResolveState");
        this->Become(&TDerived::WaitResolveState);
        if (ExecuterStateSpan) {
            ExecuterStateSpan.End();
            ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::ExecuterWaitResolveState, ExecuterSpan.GetTraceId(), "WaitResolveState", NWilson::EFlags::AUTO_END);
        }

        ExecuterTableResolveSpan = NWilson::TSpan(TWilsonKqp::ExecuterTableResolve, ExecuterStateSpan.GetTraceId(), "ExecuterTableResolve", NWilson::EFlags::AUTO_END);

        auto now = TAppData::TimeProvider->Now();
        StartResolveTime = now;

        if (Stats) {
            Stats->StartTs = now;
        }
    }

    TMaybe<size_t> FindReadRangesSource(const NKqpProto::TKqpPhyStage& stage) {
        TMaybe<size_t> res;
        for (size_t i = 0; i < stage.SourcesSize(); ++i) {
            auto& source = stage.GetSources(i);
            if (source.HasReadRangesSource()) {
                YQL_ENSURE(!res);
                res = i;

            }
        }
        return res;
    }

    NMetadata::NFetcher::ISnapshotsFetcher::TPtr GetSecretsSnapshotParser() {
        return std::make_shared<NMetadata::NSecret::TSnapshotsFetcher>();
    }

    void FetchSecrets(TVector<TString>&& secretNames) {
        YQL_ENSURE(NMetadata::NProvider::TServiceOperator::IsEnabled(), "metadata service is not active");
        SecretNames = std::move(secretNames);

        SubscribedOnSecrets = true;
        this->Send(NMetadata::NProvider::MakeServiceId(SelfId().NodeId()), new NMetadata::NProvider::TEvSubscribeExternal(GetSecretsSnapshotParser()));
        this->Schedule(MaximalSecretsSnapshotWaitTime, new NActors::TEvents::TEvWakeup());
    }

    void HandleRefreshSubscriberData(NMetadata::NProvider::TEvRefreshSubscriberData::TPtr& ev) {
        if (!SubscribedOnSecrets) {
            return;
        }

        Secrets = ev->Get()->GetSnapshotPtrAs<NMetadata::NSecret::TSnapshot>();

        TString secretValue;
        for (const TString& secretName : SecretNames) {
            auto secretId = NMetadata::NSecret::TSecretId(UserToken->GetUserSID(), secretName);
            if (!Secrets->GetSecretValue(NMetadata::NSecret::TSecretIdOrValue::BuildAsId(secretId), secretValue)) {
                return;
            }
        }

        UnsubscribeFromSecrets();
        OnSecretsFetched();
    }

    void HandleSecretsWaitingTimeout(NActors::TEvents::TEvWakeup::TPtr&) {
        if (!SubscribedOnSecrets) {
            return;
        }

        YQL_ENSURE(Secrets != nullptr, "secrets snapshot fetching timeout");
        UnsubscribeFromSecrets();
        OnSecretsFetched();
    }

    void UnsubscribeFromSecrets() {
        if (SubscribedOnSecrets) {
            SubscribedOnSecrets = false;
            this->Send(NMetadata::NProvider::MakeServiceId(SelfId().NodeId()), new NMetadata::NProvider::TEvUnsubscribeExternal(GetSecretsSnapshotParser()));
        }
    }

    virtual void OnSecretsFetched() {}

    TMap<TString, TString> ResolveSecretNames(const google::protobuf::RepeatedPtrField<TProtoStringType>& secretNames) {
        TMap<TString, TString> secureParams;
        for (const auto& secretName : secretNames) {
            auto secretId = NMetadata::NSecret::TSecretId(UserToken->GetUserSID(), secretName);

            TString secretValue;
            YQL_ENSURE(Secrets->GetSecretValue(NMetadata::NSecret::TSecretIdOrValue::BuildAsId(secretId), secretValue), "secret with name '" << secretName << "' not found");

            secureParams[secretName] = secretValue;
        }

        return secureParams;
    }

protected:
    bool CheckExecutionComplete() {
        if (Planner && Planner->GetPendingComputeActors().empty() && Planner->GetPendingComputeTasks().empty()) {
            static_cast<TDerived*>(this)->Finalize();
            UpdateResourcesUsage(true);
            return true;
        }

        UpdateResourcesUsage(false);

        if (IsDebugLogEnabled()) {
            TStringBuilder sb;
            sb << "Waiting for: ";
            if (Planner) {
                for (auto ct : Planner->GetPendingComputeTasks()) {
                    sb << "CT " << ct << ", ";
                }
                for (auto ca : Planner->GetPendingComputeActors()) {
                    sb << "CA " << ca.first << ", ";
                }
            }
            LOG_D(sb);
        }

        return false;
    }

    void InvalidateNode(ui64 node) {
        for (auto tablet : ShardsOnNode[node]) {
            auto ev = MakeHolder<TEvPipeCache::TEvForcePipeReconnect>(tablet);
            this->Send(MakePipePeNodeCacheID(false), ev.Release());
        }
    }

    void HandleUndelivered(TEvents::TEvUndelivered::TPtr& ev) {
        ui32 eventType = ev->Get()->SourceType;
        auto reason = ev->Get()->Reason;
        switch (eventType) {
            case TEvKqpNode::TEvStartKqpTasksRequest::EventType: {
                if (reason == TEvents::TEvUndelivered::EReason::ReasonActorUnknown) {
                    LOG_D("Schedule a retry by ActorUnknown reason, nodeId:" << ev->Sender.NodeId() << " requestId: " << ev->Cookie);
                    this->Schedule(TDuration::MilliSeconds(Planner->GetCurrentRetryDelay(ev->Cookie)), new typename TEvPrivate::TEvRetry(ev->Cookie, ev->Sender));
                    return;
                }
                InvalidateNode(ev->Sender.NodeId());
                return InternalError(TStringBuilder()
                    << "TEvKqpNode::TEvStartKqpTasksRequest lost: " << reason);
            }
            default: {
                LOG_E("Event lost, type: " << eventType << ", reason: " << reason);
            }
        }
    }

    void HandleRetry(typename TEvPrivate::TEvRetry::TPtr& ev) {
        if (Planner && Planner->SendStartKqpTasksRequest(ev->Get()->RequestId, ev->Get()->Target)) {
            return;
        }
        InvalidateNode(Target.NodeId());
        return InternalError(TStringBuilder()
            << "TEvKqpNode::TEvStartKqpTasksRequest lost: ActorUnknown");
    }

    void HandleDisconnected(TEvInterconnect::TEvNodeDisconnected::TPtr& ev) {
        auto nodeId = ev->Get()->NodeId;
        LOG_N("Disconnected node " << nodeId);

        if (Planner) {
            for (auto computeActor : Planner->GetPendingComputeActors()) {
                if (computeActor.first.NodeId() == nodeId) {
                    return ReplyUnavailable(TStringBuilder() << "Connection with node " << nodeId << " lost.");
                }
            }

            for (auto& task : TasksGraph.GetTasks()) {
                if (task.Meta.NodeId == nodeId && Planner->GetPendingComputeTasks().contains(task.Id)) {
                    return ReplyUnavailable(TStringBuilder() << "Connection with node " << nodeId << " lost.");
                }
            }
        }
    }

    void HandleStartKqpTasksResponse(TEvKqpNode::TEvStartKqpTasksResponse::TPtr& ev) {
        auto& record = ev->Get()->Record;
        YQL_ENSURE(record.GetTxId() == TxId);

        if (record.NotStartedTasksSize() != 0) {
            auto reason = record.GetNotStartedTasks()[0].GetReason();
            auto& message = record.GetNotStartedTasks()[0].GetMessage();

            LOG_E("Stop executing, reason: " << NKikimrKqp::TEvStartKqpTasksResponse_ENotStartedTaskReason_Name(reason)
                << ", message: " << message);

            switch (reason) {
                case NKikimrKqp::TEvStartKqpTasksResponse::NOT_ENOUGH_MEMORY: {
                    ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED,
                        YqlIssue({}, NYql::TIssuesIds::KIKIMR_OVERLOADED, message));
                    break;
                }

                case NKikimrKqp::TEvStartKqpTasksResponse::NOT_ENOUGH_EXECUTION_UNITS: {
                    ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED,
                        YqlIssue({}, NYql::TIssuesIds::KIKIMR_OVERLOADED, message));
                    break;
                }

                case NKikimrKqp::TEvStartKqpTasksResponse::QUERY_MEMORY_LIMIT_EXCEEDED: {
                    ReplyErrorAndDie(Ydb::StatusIds::PRECONDITION_FAILED,
                        YqlIssue({}, NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED, message));
                    break;
                }

                case NKikimrKqp::TEvStartKqpTasksResponse::QUERY_EXECUTION_UNITS_LIMIT_EXCEEDED: {
                    ReplyErrorAndDie(Ydb::StatusIds::OVERLOADED,
                         YqlIssue({}, NYql::TIssuesIds::KIKIMR_OVERLOADED, "Not enough computation units to execute query"));
                    break;
                }

                case NKikimrKqp::TEvStartKqpTasksResponse::INTERNAL_ERROR: {
                    InternalError("KqpNode internal error");
                    break;
                }
            }

            return;
        }

        THashMap<TActorId, THashSet<ui64>> channelsUpdates;

        for (auto& startedTask : record.GetStartedTasks()) {
            auto taskId = startedTask.GetTaskId();
            auto& task = TasksGraph.GetTask(taskId);

            task.ComputeActorId = ActorIdFromProto(startedTask.GetActorId());

            LOG_D("Executing task: " << taskId << " on compute actor: " << task.ComputeActorId);

            if (Planner) {
                if (Planner->GetPendingComputeTasks().erase(taskId) == 0) {
                    LOG_D("Executing task: " << taskId << ", compute actor: " << task.ComputeActorId << ", already finished");
                } else {
                    auto result = Planner->GetPendingComputeActors().emplace(std::make_pair(task.ComputeActorId, TProgressStat()));
                    YQL_ENSURE(result.second);

                    CollectTaskChannelsUpdates(task, channelsUpdates);
                }
            }
        }

        PropagateChannelsUpdates(channelsUpdates);
    }

    void HandleAbortExecution(TEvKqp::TEvAbortExecution::TPtr& ev) {
        auto& msg = ev->Get()->Record;
        NYql::TIssues issues = ev->Get()->GetIssues();
        LOG_D("Got EvAbortExecution, status: " << NYql::NDqProto::StatusIds_StatusCode_Name(msg.GetStatusCode())
            << ", message: " << issues.ToOneLineString());
        auto statusCode = NYql::NDq::DqStatusToYdbStatus(msg.GetStatusCode());
        if (statusCode == Ydb::StatusIds::INTERNAL_ERROR) {
            InternalError(issues);
        } else if (statusCode == Ydb::StatusIds::TIMEOUT) {
            AbortExecutionAndDie(ev->Sender, NYql::NDqProto::StatusIds::TIMEOUT, "Request timeout exceeded");
        } else {
            RuntimeError(NYql::NDq::DqStatusToYdbStatus(msg.GetStatusCode()), issues);
        }
    }

protected:
    void CollectTaskChannelsUpdates(const TKqpTasksGraph::TTaskType& task, THashMap<TActorId, THashSet<ui64>>& updates) {
        YQL_ENSURE(task.ComputeActorId);

        LOG_T("Collect channels updates for task: " << task.Id << " at actor " << task.ComputeActorId);

        auto& selfUpdates = updates[task.ComputeActorId];

        for (auto& input : task.Inputs) {
            for (auto channelId : input.Channels) {
                auto& channel = TasksGraph.GetChannel(channelId);
                YQL_ENSURE(channel.DstTask == task.Id);
                YQL_ENSURE(channel.SrcTask);

                auto& srcTask = TasksGraph.GetTask(channel.SrcTask);
                if (srcTask.ComputeActorId) {
                    updates[srcTask.ComputeActorId].emplace(channelId);
                    selfUpdates.emplace(channelId);
                }

                LOG_T("Task: " << task.Id << ", input channelId: " << channelId << ", src task: " << channel.SrcTask
                    << ", at actor " << srcTask.ComputeActorId);
            }
        }

        for (auto& output : task.Outputs) {
            for (auto channelId : output.Channels) {
                selfUpdates.emplace(channelId);

                auto& channel = TasksGraph.GetChannel(channelId);
                YQL_ENSURE(channel.SrcTask == task.Id);

                if (channel.DstTask) {
                    auto& dstTask = TasksGraph.GetTask(channel.DstTask);
                    if (dstTask.ComputeActorId) {
                        // not a optimal solution
                        updates[dstTask.ComputeActorId].emplace(channelId);
                    }

                    LOG_T("Task: " << task.Id << ", output channelId: " << channelId << ", dst task: " << channel.DstTask
                        << ", at actor " << dstTask.ComputeActorId);
                }
            }
        }
    }

    void PropagateChannelsUpdates(const THashMap<TActorId, THashSet<ui64>>& updates) {
        for (auto& pair : updates) {
            auto computeActorId = pair.first;
            auto& channelIds = pair.second;

            auto channelsInfoEv = MakeHolder<NYql::NDq::TEvDqCompute::TEvChannelsInfo>();
            auto& record = channelsInfoEv->Record;

            for (auto& channelId : channelIds) {
                FillChannelDesc(TasksGraph, *record.AddUpdate(), TasksGraph.GetChannel(channelId), TasksGraph.GetMeta().ChannelTransportVersion);
            }

            LOG_T("Sending channels info to compute actor: " << computeActorId << ", channels: " << channelIds.size());
            bool sent = this->Send(computeActorId, channelsInfoEv.Release());
            YQL_ENSURE(sent, "Failed to send event to " << computeActorId.ToString());
        }
    }

    void UpdateResourcesUsage(bool force) {
        TInstant now = TActivationContext::Now();
        if ((now - LastResourceUsageUpdate < ResourceUsageUpdateInterval) && !force)
            return;

        LastResourceUsageUpdate = now;

        TProgressStat::TEntry consumption;
        if (Planner) {
            for (const auto& p : Planner->GetPendingComputeActors()) {
                const auto& t = p.second.GetLastUsage();
                consumption += t;
            }
        }

        for (const auto& p : LastStats) {
            const auto& t = p.GetLastUsage();
            consumption += t;
        }

        auto ru = NRuCalc::CalcRequestUnit(consumption);

        // Some heuristic to reduce overprice due to round part stats
        if (ru <= 100 && !force)
            return;

        if (Planner) {
            for (auto& p : Planner->GetPendingComputeActors()) {
                p.second.Update();
            }
        }

        for (auto& p : LastStats) {
            p.Update();
        }

        if (Request.RlPath) {
            auto actorId = ReportToRl(ru, Database, UserToken->GetSerializedToken(), Request.RlPath.GetRef());

            LOG_D("Resource usage for last stat interval: " << consumption
                  << " ru: " << ru << " rl path: " << Request.RlPath.GetRef()
                  << " rl actor: " << actorId
                  << " force flag: " << force);
        } else {
            LOG_D("Resource usage for last stat interval: " << consumption
                  << " ru: " << ru << " rate limiter was not found"
                  << " force flag: " << force);
        }
    }



protected:
    void BuildSysViewScanTasks(TStageInfo& stageInfo, const TMap<TString, TString>& secureParams) {
        Y_VERIFY_DEBUG(stageInfo.Meta.IsSysView());

        auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);

        const auto& tableInfo = stageInfo.Meta.TableConstInfo;
        const auto& keyTypes = tableInfo->KeyColumnTypes;

        for (auto& op : stage.GetTableOps()) {
            Y_VERIFY_DEBUG(stageInfo.Meta.TablePath == op.GetTable().GetPath());

            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.ExecuterId = this->SelfId();
            TShardKeyRanges keyRanges;

            switch (op.GetTypeCase()) {
                case NKqpProto::TKqpPhyTableOperation::kReadRange:
                    stageInfo.Meta.SkipNullKeys.assign(
                        op.GetReadRange().GetSkipNullKeys().begin(),
                        op.GetReadRange().GetSkipNullKeys().end()
                    );
                    keyRanges.Add(MakeKeyRange(
                        keyTypes, op.GetReadRange().GetKeyRange(),
                        stageInfo, HolderFactory(), TypeEnv())
                    );
                    break;
                case NKqpProto::TKqpPhyTableOperation::kReadRanges:
                    keyRanges.CopyFrom(FillReadRanges(keyTypes, op.GetReadRanges(), stageInfo, TypeEnv()));
                    break;
                default:
                    YQL_ENSURE(false, "Unexpected table scan operation: " << (ui32) op.GetTypeCase());
            }

            TTaskMeta::TShardReadInfo readInfo = {
                .Ranges = std::move(keyRanges),
                .Columns = BuildKqpColumns(op, tableInfo),
            };

            task.Meta.Reads.ConstructInPlace();
            task.Meta.Reads->emplace_back(std::move(readInfo));
            task.Meta.ReadInfo.Reverse = op.GetReadRange().GetReverse();
            task.Meta.Type = TTaskMeta::TTaskType::Compute;

            BuildSinks(stage, task, secureParams);

            LOG_D("Stage " << stageInfo.Id << " create sysview scan task: " << task.Id);
        }
    }

    void BuildSinks(const NKqpProto::TKqpPhyStage& stage, TKqpTasksGraph::TTaskType& task, const TMap<TString, TString>& secureParams) {
        if (stage.SinksSize() > 0) {
            YQL_ENSURE(stage.SinksSize() == 1, "multiple sinks are not supported");
            const auto& sink = stage.GetSinks(0);
            YQL_ENSURE(sink.HasExternalSink(), "only external sinks are supported");
            const auto& extSink = sink.GetExternalSink();
            YQL_ENSURE(sink.GetOutputIndex() < task.Outputs.size());

            auto sinkName = extSink.GetSinkName();
            if (sinkName) {
                auto structuredToken = NYql::CreateStructuredTokenParser(extSink.GetAuthInfo()).ToBuilder().ReplaceReferences(secureParams).ToJson();
                task.Meta.SecureParams.emplace(sinkName, structuredToken);
                if (GetUserRequestContext()->TraceId) {
                    task.Meta.TaskParams.emplace("fq.job_id", GetUserRequestContext()->TraceId);
                    // "fq.restart_count"
                }
            }

            auto& output = task.Outputs[sink.GetOutputIndex()];
            output.Type = TTaskOutputType::Sink;
            output.SinkType = extSink.GetType();
            output.SinkSettings = extSink.GetSettings();
        }
    }

    void BuildReadTasksFromSource(TStageInfo& stageInfo, const TMap<TString, TString>& secureParams, const TVector<NKikimrKqp::TKqpNodeResources>& resourceSnapshot) {
        const auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);

        YQL_ENSURE(stage.GetSources(0).HasExternalSource());
        YQL_ENSURE(stage.SourcesSize() == 1, "multiple sources in one task are not supported");

        const auto& stageSource = stage.GetSources(0);
        const auto& externalSource = stageSource.GetExternalSource();

        ui32 taskCount = externalSource.GetPartitionedTaskParams().size();

        if (!resourceSnapshot.empty()) {
            ui32 maxTaskcount = resourceSnapshot.size() * 2;
            if (taskCount > maxTaskcount) {
                taskCount = maxTaskcount;
            }
        }

        auto sourceName = externalSource.GetSourceName();
        TString structuredToken;
        if (sourceName) {
            structuredToken = NYql::CreateStructuredTokenParser(externalSource.GetAuthInfo()).ToBuilder().ReplaceReferences(secureParams).ToJson();
        }

        TVector<ui64> tasksIds;

        // generate all tasks
        for (ui32 i = 0; i < taskCount; i++) {
            auto& task = TasksGraph.AddTask(stageInfo);

            auto& input = task.Inputs[stageSource.GetInputIndex()];
            input.ConnectionInfo = NYql::NDq::TSourceInput{};
            input.SourceSettings = externalSource.GetSettings();
            input.SourceType = externalSource.GetType();

            if (structuredToken) {
                task.Meta.SecureParams.emplace(sourceName, structuredToken);
            }

            if (resourceSnapshot.empty()) {
                task.Meta.Type = TTaskMeta::TTaskType::Compute;
            } else {
                task.Meta.NodeId = resourceSnapshot[i % resourceSnapshot.size()].GetNodeId();
                task.Meta.Type = TTaskMeta::TTaskType::Scan;
            }

            tasksIds.push_back(task.Id);
        }

        // distribute read ranges between them
        ui32 currentTaskIndex = 0;
        for (const TString& partitionParam : externalSource.GetPartitionedTaskParams()) {
            TasksGraph.GetTask(tasksIds[currentTaskIndex]).Meta.ReadRanges.push_back(partitionParam);
            if (++currentTaskIndex >= tasksIds.size()) {
                currentTaskIndex = 0;
            }
        }

        // finish building
        for (auto taskId : tasksIds) {
            BuildSinks(stage, TasksGraph.GetTask(taskId), secureParams);
        }
    }

    TMaybe<size_t> BuildScanTasksFromSource(TStageInfo& stageInfo, const TMap<TString, TString>& secureParams) {
        THashMap<ui64, std::vector<ui64>> nodeTasks;
        THashMap<ui64, ui64> assignedShardsCount;

        auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);

        YQL_ENSURE(stage.GetSources(0).HasReadRangesSource());
        YQL_ENSURE(stage.GetSources(0).GetInputIndex() == 0 && stage.SourcesSize() == 1);
        for (auto& input : stage.inputs()) {
            YQL_ENSURE(input.HasBroadcast());
        }

        auto& source = stage.GetSources(0).GetReadRangesSource();

        const auto& tableInfo = stageInfo.Meta.TableConstInfo;
        const auto& keyTypes = tableInfo->KeyColumnTypes;

        YQL_ENSURE(tableInfo->TableKind != NKikimr::NKqp::ETableKind::Olap);

        auto columns = BuildKqpColumns(source, tableInfo);

        const auto& snapshot = GetSnapshot();

        auto addPartiton = [&](
            ui64 taskLocation,
            TMaybe<ui64> shardId,
            const TShardInfo& shardInfo,
            TMaybe<ui64> maxInFlightShards = Nothing())
        {
            YQL_ENSURE(!shardInfo.KeyWriteRanges);

            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.Type = TTaskMeta::TTaskType::Scan;
            task.Meta.ExecuterId = this->SelfId();
            if (auto ptr = ShardIdToNodeId.FindPtr(taskLocation)) {
                task.Meta.NodeId = *ptr;
            } else {
                task.Meta.ShardId = taskLocation;
            }

            const auto& stageSource = stage.GetSources(0);
            auto& input = task.Inputs[stageSource.GetInputIndex()];
            input.SourceType = NYql::KqpReadRangesSourceName;
            input.ConnectionInfo = NYql::NDq::TSourceInput{};

            // allocating source settings

            input.Meta.SourceSettings = TasksGraph.GetMeta().Allocate<NKikimrTxDataShard::TKqpReadRangesSourceSettings>();
            NKikimrTxDataShard::TKqpReadRangesSourceSettings* settings = input.Meta.SourceSettings;
            FillTableMeta(stageInfo, settings->MutableTable());

            for (auto& keyColumn : keyTypes) {
                auto columnType = NScheme::ProtoColumnTypeFromTypeInfoMod(keyColumn, "");
                if (columnType.TypeInfo) {
                    *settings->AddKeyColumnTypeInfos() = *columnType.TypeInfo;
                } else {
                    *settings->AddKeyColumnTypeInfos() = NKikimrProto::TTypeInfo();
                }
                settings->AddKeyColumnTypes(static_cast<ui32>(keyColumn.GetTypeId()));
            }

            for (auto& column : columns) {
                auto* protoColumn = settings->AddColumns();
                protoColumn->SetId(column.Id);
                auto columnType = NScheme::ProtoColumnTypeFromTypeInfoMod(column.Type, column.TypeMod);
                protoColumn->SetType(columnType.TypeId);
                if (columnType.TypeInfo) {
                    *protoColumn->MutableTypeInfo() = *columnType.TypeInfo;
                }
                protoColumn->SetName(column.Name);
            }

            if (AppData()->FeatureFlags.GetEnableArrowFormatAtDatashard()) {
                settings->SetDataFormat(NKikimrTxDataShard::EScanDataFormat::ARROW);
            } else {
                settings->SetDataFormat(NKikimrTxDataShard::EScanDataFormat::CELLVEC);
            }

            if (snapshot.IsValid()) {
                settings->MutableSnapshot()->SetStep(snapshot.Step);
                settings->MutableSnapshot()->SetTxId(snapshot.TxId);
            }

            shardInfo.KeyReadRanges->SerializeTo(settings);
            settings->SetReverse(source.GetReverse());
            settings->SetSorted(source.GetSorted());

            if (maxInFlightShards) {
                settings->SetMaxInFlightShards(*maxInFlightShards);
            }

            if (shardId) {
                settings->SetShardIdHint(*shardId);
                if (Stats) {
                    Stats->AffectedShards.insert(*shardId);
                }
            }

            ui64 itemsLimit = ExtractItemsLimit(stageInfo, source.GetItemsLimit(), Request.TxAlloc->HolderFactory,
                Request.TxAlloc->TypeEnv);
            settings->SetItemsLimit(itemsLimit);

            auto self = static_cast<TDerived*>(this)->SelfId();
            auto& lockTxId = TasksGraph.GetMeta().LockTxId;
            if (lockTxId) {
                settings->SetLockTxId(*lockTxId);
                settings->SetLockNodeId(self.NodeId());
            }

            BuildSinks(stage, task, secureParams);
        };

        THashMap<ui64, TShardInfo> partitions = PrunePartitions(source, stageInfo, HolderFactory(), TypeEnv());
        if (partitions.size() > 0 && source.GetSequentialInFlightShards() > 0 && partitions.size() > source.GetSequentialInFlightShards()) {
            auto [startShard, shardInfo] = MakeVirtualTablePartition(source, stageInfo, HolderFactory(), TypeEnv());
            if (Stats) {
                for (auto& [shardId, _] : partitions) {
                    Stats->AffectedShards.insert(shardId);
                }
            }
            if (shardInfo.KeyReadRanges) {
                addPartiton(startShard, {}, shardInfo, source.GetSequentialInFlightShards());
                return Nothing();
            } else {
                return 0;
            }
        } else {
            for (auto& [shardId, shardInfo] : partitions) {
                addPartiton(shardId, shardId, shardInfo, {});
            }
            return partitions.size();
        }
    }

    void FillReadInfo(TTaskMeta& taskMeta, ui64 itemsLimit, bool reverse, bool sorted) const
    {
        if (taskMeta.Reads && !taskMeta.Reads.GetRef().empty()) {
            // Validate parameters
            YQL_ENSURE(taskMeta.ReadInfo.ItemsLimit == itemsLimit);
            YQL_ENSURE(taskMeta.ReadInfo.Reverse == reverse);
            return;
        }

        taskMeta.ReadInfo.ItemsLimit = itemsLimit;
        taskMeta.ReadInfo.Reverse = reverse;
        taskMeta.ReadInfo.Sorted = sorted;
        taskMeta.ReadInfo.ReadType = TTaskMeta::TReadInfo::EReadType::Rows;
    }

    TTaskMeta::TReadInfo::EReadType OlapReadTypeFromProto(const NKqpProto::TKqpPhyOpReadOlapRanges::EReadType& type) const {
        switch (type) {
            case NKqpProto::TKqpPhyOpReadOlapRanges::ROWS:
                return TTaskMeta::TReadInfo::EReadType::Rows;
            case NKqpProto::TKqpPhyOpReadOlapRanges::BLOCKS:
                return TTaskMeta::TReadInfo::EReadType::Blocks;
            default:
                YQL_ENSURE(false, "Invalid read type from TKqpPhyOpReadOlapRanges protobuf.");
        }
    }

    void FillOlapReadInfo(TTaskMeta& taskMeta, NKikimr::NMiniKQL::TType* resultType, const TMaybe<::NKqpProto::TKqpPhyOpReadOlapRanges>& readOlapRange) const {
        if (taskMeta.Reads && !taskMeta.Reads.GetRef().empty()) {
            // Validate parameters
            if (!readOlapRange || readOlapRange->GetOlapProgram().empty()) {
                YQL_ENSURE(taskMeta.ReadInfo.OlapProgram.Program.empty());
                return;
            }

            YQL_ENSURE(taskMeta.ReadInfo.OlapProgram.Program == readOlapRange->GetOlapProgram());
            return;
        }

        if (resultType) {
            YQL_ENSURE(resultType->GetKind() == NKikimr::NMiniKQL::TType::EKind::Struct
                || resultType->GetKind() == NKikimr::NMiniKQL::TType::EKind::Tuple);

            auto* resultStructType = static_cast<NKikimr::NMiniKQL::TStructType*>(resultType);
            ui32 resultColsCount = resultStructType->GetMembersCount();

            taskMeta.ReadInfo.ResultColumnsTypes.reserve(resultColsCount);
            for (ui32 i = 0; i < resultColsCount; ++i) {
                auto memberType = resultStructType->GetMemberType(i);
                if (memberType->GetKind() == NKikimr::NMiniKQL::TType::EKind::Optional) {
                    memberType = static_cast<NKikimr::NMiniKQL::TOptionalType*>(memberType)->GetItemType();
                }
                // TODO: support pg types
                YQL_ENSURE(memberType->GetKind() == NKikimr::NMiniKQL::TType::EKind::Data,
                    "Expected simple data types to be read from column shard");
                auto memberDataType = static_cast<NKikimr::NMiniKQL::TDataType*>(memberType);
                taskMeta.ReadInfo.ResultColumnsTypes.push_back(NScheme::TTypeInfo(memberDataType->GetSchemeType()));
            }
        }

        if (!readOlapRange || readOlapRange->GetOlapProgram().empty()) {
            return;
        }
        taskMeta.ReadInfo.ReadType = OlapReadTypeFromProto(readOlapRange->GetReadType());
        taskMeta.ReadInfo.OlapProgram.Program = readOlapRange->GetOlapProgram();
        for (auto& name: readOlapRange->GetOlapProgramParameterNames()) {
            taskMeta.ReadInfo.OlapProgram.ParameterNames.insert(name);
        }
    }

    void MergeReadInfoToTaskMeta(TTaskMeta& meta, ui64 shardId, TMaybe<TShardKeyRanges>& keyReadRanges, const TPhysicalShardReadSettings& readSettings, const TVector<TTaskMeta::TColumn>& columns,
        const NKqpProto::TKqpPhyTableOperation& op, bool isPersistentScan) const
    {
        TTaskMeta::TShardReadInfo readInfo = {
            .Columns = columns,
        };
        if (keyReadRanges) {
            readInfo.Ranges = std::move(*keyReadRanges); // sorted & non-intersecting
        }

        if (isPersistentScan) {
            readInfo.ShardId = shardId;
        }

        FillReadInfo(meta, readSettings.ItemsLimit, readSettings.Reverse, readSettings.Sorted);
        if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kReadOlapRange) {
            FillOlapReadInfo(meta, readSettings.ResultType, op.GetReadOlapRange());
        }

        if (!meta.Reads) {
            meta.Reads.ConstructInPlace();
        }

        meta.Reads->emplace_back(std::move(readInfo));
    }

    ui32 GetScanTasksPerNode(TStageInfo& stageInfo, const bool isOlapScan, const ui64 /*nodeId*/) const {
        ui32 result = 0;
        if (isOlapScan) {
            if (AggregationSettings.HasCSScanThreadsPerNode()) {
                result = AggregationSettings.GetCSScanThreadsPerNode();
            } else {
                const TStagePredictor& predictor = stageInfo.Meta.Tx.Body->GetCalculationPredictor(stageInfo.Id.StageId);
                result = predictor.CalcTasksOptimalCount(TStagePredictor::GetUsableThreads(), {});
            }
        } else {
            const auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);
            result = AggregationSettings.GetDSScanMinimalThreads();
            if (stage.GetProgram().GetSettings().GetHasSort()) {
                result = std::max(result, AggregationSettings.GetDSBaseSortScanThreads());
            }
            if (stage.GetProgram().GetSettings().GetHasMapJoin()) {
                result = std::max(result, AggregationSettings.GetDSBaseJoinScanThreads());
            }
        }
        return Max<ui32>(1, result);
    }

    TTask& AssignScanTaskToShard(
        TStageInfo& stageInfo, const ui64 shardId,
        THashMap<ui64, std::vector<ui64>>& nodeTasks,
        THashMap<ui64, ui64>& assignedShardsCount,
        const bool sorted, const bool isOlapScan)
    {
        ui64 nodeId = ShardIdToNodeId.at(shardId);
        if (stageInfo.Meta.IsOlap() && sorted) {
            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.ExecuterId = SelfId();
            task.Meta.NodeId = nodeId;
            task.Meta.ScanTask = true;
            task.Meta.Type = TTaskMeta::TTaskType::Scan;
            return task;
        }

        auto& tasks = nodeTasks[nodeId];
        auto& cnt = assignedShardsCount[nodeId];
        const ui32 maxScansPerNode = GetScanTasksPerNode(stageInfo, isOlapScan, nodeId);
        if (cnt < maxScansPerNode) {
            auto& task = TasksGraph.AddTask(stageInfo);
            task.Meta.NodeId = nodeId;
            task.Meta.ScanTask = true;
            task.Meta.Type = TTaskMeta::TTaskType::Scan;
            tasks.push_back(task.Id);
            ++cnt;
            return task;
        } else {
            ui64 taskIdx = cnt % maxScansPerNode;
            ++cnt;
            return TasksGraph.GetTask(tasks[taskIdx]);
        }
    }

    void BuildScanTasksFromShards(TStageInfo& stageInfo) {
        THashMap<ui64, std::vector<ui64>> nodeTasks;
        THashMap<ui64, std::vector<TShardInfoWithId>> nodeShards;
        THashMap<ui64, ui64> assignedShardsCount;
        auto& stage = stageInfo.Meta.GetStage(stageInfo.Id);

        const auto& tableInfo = stageInfo.Meta.TableConstInfo;
        const auto& keyTypes = tableInfo->KeyColumnTypes;
        ui32 metaId = 0;
        for (auto& op : stage.GetTableOps()) {
            Y_VERIFY_DEBUG(stageInfo.Meta.TablePath == op.GetTable().GetPath());

            auto columns = BuildKqpColumns(op, tableInfo);
            auto partitions = PrunePartitions(op, stageInfo, HolderFactory(), TypeEnv());
            const bool isOlapScan = (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kReadOlapRange);
            auto readSettings = ExtractReadSettings(op, stageInfo, HolderFactory(), TypeEnv());

            if (op.GetTypeCase() == NKqpProto::TKqpPhyTableOperation::kReadRange) {
                stageInfo.Meta.SkipNullKeys.assign(op.GetReadRange().GetSkipNullKeys().begin(),
                                                   op.GetReadRange().GetSkipNullKeys().end());
                // not supported for scan queries
                YQL_ENSURE(!readSettings.Reverse);
            }

            for (auto&& i: partitions) {
                const ui64 nodeId = ShardIdToNodeId.at(i.first);
                nodeShards[nodeId].emplace_back(TShardInfoWithId(i.first, std::move(i.second)));
            }

            if (Stats && CollectProfileStats(Request.StatsMode)) {
                for (auto&& i : nodeShards) {
                    Stats->AddNodeShardsCount(stageInfo.Id.StageId, i.first, i.second.size());
                }
            }

            if (!AppData()->FeatureFlags.GetEnableSeparationComputeActorsFromRead() || (!isOlapScan && readSettings.Sorted)) {
                for (auto&& pair : nodeShards) {
                    auto& shardsInfo = pair.second;
                    for (auto&& shardInfo : shardsInfo) {
                        auto& task = AssignScanTaskToShard(stageInfo, shardInfo.ShardId, nodeTasks, assignedShardsCount, readSettings.Sorted, isOlapScan);
                        MergeReadInfoToTaskMeta(task.Meta, shardInfo.ShardId, shardInfo.KeyReadRanges, readSettings,
                            columns, op, /*isPersistentScan*/ true);
                    }
                }

                for (const auto& pair : nodeTasks) {
                    for (const auto& taskIdx : pair.second) {
                        auto& task = TasksGraph.GetTask(taskIdx);
                        task.Meta.SetEnableShardsSequentialScan(readSettings.Sorted);
                        PrepareScanMetaForUsage(task.Meta, keyTypes);
                    }
                }

            } else {
                for (auto&& pair : nodeShards) {
                    const auto nodeId = pair.first;
                    auto& shardsInfo = pair.second;
                    const ui32 metaGlueingId = ++metaId;
                    TTaskMeta meta;
                    {
                        for (auto&& shardInfo : shardsInfo) {
                            MergeReadInfoToTaskMeta(meta, shardInfo.ShardId, shardInfo.KeyReadRanges, readSettings,
                                columns, op, /*isPersistentScan*/ true);
                        }
                        PrepareScanMetaForUsage(meta, keyTypes);
                        LOG_D("Stage " << stageInfo.Id << " create scan task meta for node: " << nodeId
                            << ", meta: " << meta.ToString(keyTypes, *AppData()->TypeRegistry));
                    }
                    for (ui32 t = 0; t < GetScanTasksPerNode(stageInfo, isOlapScan, nodeId); ++t) {
                        auto& task = TasksGraph.AddTask(stageInfo);
                        task.Meta = meta;
                        task.Meta.SetEnableShardsSequentialScan(false);
                        task.Meta.ExecuterId = SelfId();
                        task.Meta.NodeId = nodeId;
                        task.Meta.ScanTask = true;
                        task.Meta.Type = TTaskMeta::TTaskType::Scan;
                        task.SetMetaId(metaGlueingId);
                    }
                }
            }
        }

        LOG_D("Stage " << stageInfo.Id << " will be executed on " << nodeTasks.size() << " nodes.");
    }

    void PrepareScanMetaForUsage(TTaskMeta& meta, const TVector<NScheme::TTypeInfo>& keyTypes) const {
        YQL_ENSURE(meta.Reads.Defined());
        auto& taskReads = meta.Reads.GetRef();

        /*
         * Sort read ranges so that sequential scan of that ranges produce sorted result.
         *
         * Partition pruner feed us with set of non-intersecting ranges with filled right boundary.
         * So we may sort ranges based solely on the their rightmost point.
         */
        std::sort(taskReads.begin(), taskReads.end(), [&](const auto& lhs, const auto& rhs) {
            if (lhs.ShardId == rhs.ShardId) {
                return false;
            }

            const std::pair<const TSerializedCellVec*, bool> k1 = lhs.Ranges.GetRightBorder();
            const std::pair<const TSerializedCellVec*, bool> k2 = rhs.Ranges.GetRightBorder();

            const int cmp = CompareBorders<false, false>(
                k1.first->GetCells(),
                k2.first->GetCells(),
                k1.second,
                k2.second,
                keyTypes);

            return (cmp < 0);
            });
    }

    void GetResourcesSnapshot() {
        GetKqpResourceManager()->RequestClusterResourcesInfo(
            [as = TlsActivationContext->ActorSystem(), self = SelfId()](TVector<NKikimrKqp::TKqpNodeResources>&& resources) {
                TAutoPtr<IEventHandle> eh = new IEventHandle(self, self, new typename TEvPrivate::TEvResourcesSnapshot(std::move(resources)));
                as->Send(eh);
            });
    }

protected:
    void TerminateComputeActors(Ydb::StatusIds::StatusCode code, const NYql::TIssues& issues) {
        for (const auto& task : this->TasksGraph.GetTasks()) {
            if (task.ComputeActorId) {
                LOG_I("aborting compute actor execution, message: " << issues.ToOneLineString()
                    << ", compute actor: " << task.ComputeActorId << ", task: " << task.Id);

                auto ev = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDq::YdbStatusToDqStatus(code), issues);
                this->Send(task.ComputeActorId, ev.Release());
            } else {
                LOG_I("task: " << task.Id << ", does not have Compute ActorId yet");
            }
        }
    }

    void TerminateComputeActors(Ydb::StatusIds::StatusCode code, const TString& message) {
        TerminateComputeActors(code, NYql::TIssues({NYql::TIssue(message)}));
    }

protected:
    void UnexpectedEvent(const TString& state, ui32 eventType) {
        LOG_C("TKqpExecuter, unexpected event: " << eventType << ", at state:" << state << ", selfID: " << this->SelfId());
        InternalError(TStringBuilder() << "Unexpected event at TKqpExecuter, state: " << state
            << ", event: " << eventType);
    }

    void InternalError(const NYql::TIssues& issues) {
        LOG_E(issues.ToOneLineString());
        TerminateComputeActors(Ydb::StatusIds::INTERNAL_ERROR, issues);
        auto issue = NYql::YqlIssue({}, NYql::TIssuesIds::UNEXPECTED, "Internal error while executing transaction.");
        for (const NYql::TIssue& i : issues) {
            issue.AddSubIssue(MakeIntrusive<NYql::TIssue>(i));
        }
        ReplyErrorAndDie(Ydb::StatusIds::INTERNAL_ERROR, issue);
    }

    void InternalError(const TString& message) {
        InternalError(NYql::TIssues({NYql::TIssue(message)}));
    }

    void ReplyUnavailable(const TString& message) {
        LOG_E("UNAVAILABLE: " << message);
        TerminateComputeActors(Ydb::StatusIds::UNAVAILABLE, message);
        auto issue = NYql::YqlIssue({}, NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE);
        issue.AddSubIssue(new NYql::TIssue(message));
        ReplyErrorAndDie(Ydb::StatusIds::UNAVAILABLE, issue);
    }

    void RuntimeError(Ydb::StatusIds::StatusCode code, const NYql::TIssues& issues) {
        LOG_E(Ydb::StatusIds_StatusCode_Name(code) << ": " << issues.ToOneLineString());
        TerminateComputeActors(code, issues);
        ReplyErrorAndDie(code, issues);
    }

    void ReplyErrorAndDie(Ydb::StatusIds::StatusCode status, const NYql::TIssues& issues) {
        google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage> protoIssues;
        IssuesToMessage(issues, &protoIssues);
        ReplyErrorAndDie(status, &protoIssues);
    }

    void ReplyErrorAndDie(Ydb::StatusIds::StatusCode status, const NYql::TIssue& issue) {
        google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage> issues;
        IssueToMessage(issue, issues.Add());
        ReplyErrorAndDie(status, &issues);
    }

    void AbortExecutionAndDie(TActorId abortSender, NYql::NDqProto::StatusIds::StatusCode status, const TString& message) {
        LOG_E("Abort execution: " << NYql::NDqProto::StatusIds_StatusCode_Name(status) << "," << message);
        if (ExecuterSpan) {
            ExecuterSpan.EndError(TStringBuilder() << NYql::NDqProto::StatusIds_StatusCode_Name(status));
        }

        // TEvAbortExecution can come from either ComputeActor or SessionActor (== Target).
        // If it have come from SessionActor there is no need to send new TEvAbortExecution back
        if (abortSender != Target) {
            auto abortEv = MakeHolder<TEvKqp::TEvAbortExecution>(status, "Request timeout exceeded");
            this->Send(Target, abortEv.Release());
        }
        Request.Transactions.crop(0);
        TerminateComputeActors(Ydb::StatusIds::TIMEOUT, message);
        this->PassAway();
    }

    virtual void ReplyErrorAndDie(Ydb::StatusIds::StatusCode status,
        google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>* issues)
    {
        if (Planner) {
            for (auto computeActor : Planner->GetPendingComputeActors()) {
                LOG_D("terminate compute actor " << computeActor.first);

                auto ev = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDq::YdbStatusToDqStatus(status), "Terminate execution");
                this->Send(computeActor.first, ev.Release());
            }
        }

        auto& response = *ResponseEv->Record.MutableResponse();

        response.SetStatus(status);
        response.MutableIssues()->Swap(issues);

        LOG_T("ReplyErrorAndDie. Response: " << response.DebugString()
            << ", to ActorId: " << Target);

        if constexpr (ExecType == EExecType::Data) {
            if (status != Ydb::StatusIds::SUCCESS) {
                Counters->TxProxyMon->ReportStatusNotOK->Inc();
            } else {
                Counters->TxProxyMon->ReportStatusOK->Inc();
            }
        }

        LWTRACK(KqpBaseExecuterReplyErrorAndDie, ResponseEv->Orbit, TxId);

        if (ExecuterSpan) {
            ExecuterSpan.EndError(response.DebugString());
        }

        Request.Transactions.crop(0);
        this->Send(Target, ResponseEv.release());
        this->PassAway();
    }

protected:
    template <class TCollection>
    bool ValidateTaskSize(const TCollection& tasks) {
        for (const auto& task : tasks) {
            if (ui32 size = task->ByteSize(); size > MaxTaskSize) {
                LOG_E("Abort execution. Task #" << task->GetId() << " size is too big: " << size << " > " << MaxTaskSize);
                ReplyErrorAndDie(Ydb::StatusIds::ABORTED,
                    MakeIssue(NKikimrIssues::TIssuesIds::SHARD_PROGRAM_SIZE_EXCEEDED, TStringBuilder() <<
                        "Datashard program size limit exceeded (" << size << " > " << MaxTaskSize << ")"));
                return false;
            }
        }
        return true;
    }

    void InitializeChannelProxies() {
        for(const auto& channel: TasksGraph.GetChannels()) {
            if (channel.DstTask) {
                continue;
            }

            CreateChannelProxy(channel);
        }
    }

    const IKqpGateway::TKqpSnapshot& GetSnapshot() const {
        return TasksGraph.GetMeta().Snapshot;
    }

    void SetSnapshot(ui64 step, ui64 txId) {
        TasksGraph.GetMeta().SetSnapshot(step, txId);
    }

    IActor* CreateChannelProxy(const NYql::NDq::TChannel& channel) {
        auto channelIt = ResultChannelProxies.find(channel.Id);
        if (channelIt != ResultChannelProxies.end()) {
            return channelIt->second;
        }

        YQL_ENSURE(channel.DstInputIndex < ResponseEv->ResultsSize());
        const auto& txResult = ResponseEv->TxResults[channel.DstInputIndex];

        IActor* proxy;
        if (txResult.IsStream && txResult.QueryResultIndex.Defined()) {
            proxy = CreateResultStreamChannelProxy(TxId, channel.Id, txResult.MkqlItemType,
                txResult.ColumnOrder, *txResult.QueryResultIndex, Target, Stats, this->SelfId());
        } else {
            proxy = CreateResultDataChannelProxy(TxId, channel.Id, Stats, this->SelfId(),
                channel.DstInputIndex, ResponseEv.get());
        }

        this->RegisterWithSameMailbox(proxy);
        ResultChannelProxies.emplace(std::make_pair(channel.Id, proxy));
        TasksGraph.GetMeta().ResultChannelProxies.emplace(channel.Id, proxy->SelfId());

        return proxy;
    }

protected:
    void PassAway() override {
        UnsubscribeFromSecrets();

        for (auto channelPair: ResultChannelProxies) {
            LOG_D("terminate result channel " << channelPair.first << " proxy at " << channelPair.second->SelfId());

            TAutoPtr<IEventHandle> ev = new IEventHandle(
                channelPair.second->SelfId(), SelfId(), new TEvents::TEvPoison
            );
            channelPair.second->Receive(ev);
        }

        LOG_D("terminate execution.");
        if (KqpShardsResolverId) {
            this->Send(KqpShardsResolverId, new TEvents::TEvPoison);
        }

        if (Planner) {
            Planner->Unsubscribe();
        }

        if (KqpTableResolverId) {
            this->Send(KqpTableResolverId, new TEvents::TEvPoison);
            this->Send(this->SelfId(), new TEvents::TEvPoison);
            LOG_T("Terminate, become ZombieState");
            this->Become(&TKqpExecuterBase::ZombieState);
            if (ExecuterStateSpan) {
                ExecuterStateSpan.End();
                ExecuterStateSpan = NWilson::TSpan(TWilsonKqp::ExecuterZombieState, ExecuterSpan.GetTraceId(), "ZombieState", NWilson::EFlags::AUTO_END);
            }
        } else {
            IActor::PassAway();
        }
    }

    STATEFN(ZombieState) {
        if (ev->GetTypeRewrite() == TEvents::TEvPoison::EventType) {
            IActor::PassAway();
        }
    }

protected:
    virtual TString CurrentStateFuncName() const {
        const auto& func = this->CurrentStateFunc();
        if (func == &TKqpExecuterBase::ZombieState) {
            return "ZombieState";
        } else if (func == &TKqpExecuterBase::ReadyState) {
            return "ReadyState";
        } else {
            return "unknown state";
        }
    }

    std::unordered_map<ui64, IActor*>& GetResultChannelProxies() {
        return ResultChannelProxies;
    }

    TString DebugString() const {
        TStringBuilder sb;
        sb << "[KqpExecuter], type: " << (ExecType == EExecType::Data ? "Data" : "Scan")
           << ", Database: " << Database << ", TxId: " << TxId << ", TxCnt: " << Request.Transactions.size()
           << ", Transactions: " << Endl;
        for (const auto& tx : Request.Transactions) {
            sb << "tx: " << tx.Body->DebugString() << Endl;
        }
        return std::move(sb);
    }

    const TIntrusivePtr<TUserRequestContext>& GetUserRequestContext() const {
        return TasksGraph.GetMeta().UserRequestContext;
    }

    TIntrusivePtr<TUserRequestContext>& MutableUserRequestContext() {
        return TasksGraph.GetMeta().UserRequestContext;
    }

protected:
    IKqpGateway::TExecPhysicalRequest Request;
    const TString Database;
    const TIntrusiveConstPtr<NACLib::TUserToken> UserToken;
    TKqpRequestCounters::TPtr Counters;
    std::shared_ptr<TQueryExecutionStats> Stats;
    TInstant StartTime;
    TMaybe<TInstant> Deadline;
    TMaybe<TInstant> CancelAt;
    TActorId Target;
    ui64 TxId = 0;

    TKqpTasksGraph TasksGraph;

    TActorId KqpTableResolverId;
    TActorId KqpShardsResolverId;
    THashMap<TActorId, NYql::NDqProto::TComputeActorExtraData> ExtraData;

    TVector<TProgressStat> LastStats;

    TInstant StartResolveTime;
    TInstant LastResourceUsageUpdate;

    std::unordered_map<ui64, IActor*> ResultChannelProxies;
    std::unique_ptr<TEvKqpExecuter::TEvTxResponse> ResponseEv;
    NWilson::TSpan ExecuterSpan;
    NWilson::TSpan ExecuterStateSpan;
    NWilson::TSpan ExecuterTableResolveSpan;

    TMap<ui64, ui64> ShardIdToNodeId;
    TMap<ui64, TVector<ui64>> ShardsOnNode;

    ui64 LastTaskId = 0;
    TString LastComputeActorId = "";

    std::unique_ptr<TKqpPlanner> Planner;
    const NKikimrConfig::TTableServiceConfig::TExecuterRetriesConfig ExecuterRetriesConfig;

    std::shared_ptr<NMetadata::NSecret::TSnapshot> Secrets;
    TVector<TString> SecretNames;
    TDuration MaximalSecretsSnapshotWaitTime;
    bool SubscribedOnSecrets = false;

    const NKikimrConfig::TTableServiceConfig::TAggregationConfig AggregationSettings;
    TVector<NKikimrKqp::TKqpNodeResources> ResourcesSnapshot;
    bool HasOlapTable;

private:
    static constexpr TDuration ResourceUsageUpdateInterval = TDuration::MilliSeconds(100);
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

IActor* CreateKqpDataExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, TKqpRequestCounters::TPtr counters, bool streamResult,
    const NKikimrConfig::TTableServiceConfig::TAggregationConfig& aggregation,
    const NKikimrConfig::TTableServiceConfig::TExecuterRetriesConfig& executerRetriesConfig, NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory,
    const NKikimrConfig::TTableServiceConfig::EChannelTransportVersion chanTransportVersion, const TActorId& creator,
    TDuration maximalSecretsSnapshotWaitTime, const TIntrusivePtr<TUserRequestContext>& userRequestContext);

IActor* CreateKqpScanExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, TKqpRequestCounters::TPtr counters,
    const NKikimrConfig::TTableServiceConfig::TAggregationConfig& aggregation,
    const NKikimrConfig::TTableServiceConfig::TExecuterRetriesConfig& executerRetriesConfig,
    TPreparedQueryHolder::TConstPtr preparedQuery, const NKikimrConfig::TTableServiceConfig::EChannelTransportVersion chanTransportVersion,
    TDuration maximalSecretsSnapshotWaitTime, const TIntrusivePtr<TUserRequestContext>& userRequestContext);

} // namespace NKqp
} // namespace NKikimr