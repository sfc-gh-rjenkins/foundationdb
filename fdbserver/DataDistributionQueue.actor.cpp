/*
 * DataDistributionQueue.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits>
#include <numeric>
#include <vector>

#include "flow/ActorCollection.h"
#include "flow/FastRef.h"
#include "flow/Trace.h"
#include "flow/Util.h"
#include "fdbrpc/sim_validation.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/DataDistribution.actor.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/Knobs.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/DDTxnProcessor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

#define WORK_FULL_UTILIZATION 10000 // This is not a knob; it is a fixed point scaling factor!

typedef Reference<IDataDistributionTeam> ITeamRef;
typedef std::pair<ITeamRef, ITeamRef> SrcDestTeamPair;

inline bool isDataMovementForDiskBalancing(DataMovementReason reason) {
	return reason == DataMovementReason::REBALANCE_UNDERUTILIZED_TEAM ||
	       reason == DataMovementReason::REBALANCE_OVERUTILIZED_TEAM;
}

inline bool isDataMovementForReadBalancing(DataMovementReason reason) {
	return reason == DataMovementReason::REBALANCE_READ_OVERUTIL_TEAM ||
	       reason == DataMovementReason::REBALANCE_READ_UNDERUTIL_TEAM;
}

inline bool isDataMovementForMountainChopper(DataMovementReason reason) {
	return reason == DataMovementReason::REBALANCE_OVERUTILIZED_TEAM ||
	       reason == DataMovementReason::REBALANCE_READ_OVERUTIL_TEAM;
}

// FIXME: Always use DataMovementReason to invoke these functions.
inline bool isValleyFillerPriority(int priority) {
	return priority == SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM ||
	       priority == SERVER_KNOBS->PRIORITY_REBALANCE_READ_UNDERUTIL_TEAM;
}

inline bool isDataMovementForValleyFiller(DataMovementReason reason) {
	return reason == DataMovementReason::REBALANCE_UNDERUTILIZED_TEAM ||
	       reason == DataMovementReason::REBALANCE_READ_UNDERUTIL_TEAM;
}

int dataMovementPriority(DataMovementReason reason) {
	int priority;
	switch (reason) {
	case DataMovementReason::INVALID:
		priority = -1;
		break;
	case DataMovementReason::RECOVER_MOVE:
		priority = SERVER_KNOBS->PRIORITY_RECOVER_MOVE;
		break;
	case DataMovementReason::REBALANCE_UNDERUTILIZED_TEAM:
		priority = SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM;
		break;
	case DataMovementReason::REBALANCE_OVERUTILIZED_TEAM:
		priority = SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM;
		break;
	case DataMovementReason::REBALANCE_READ_OVERUTIL_TEAM:
		priority = SERVER_KNOBS->PRIORITY_REBALANCE_READ_OVERUTIL_TEAM;
		break;
	case DataMovementReason::REBALANCE_READ_UNDERUTIL_TEAM:
		priority = SERVER_KNOBS->PRIORITY_REBALANCE_READ_UNDERUTIL_TEAM;
		break;
	case DataMovementReason::PERPETUAL_STORAGE_WIGGLE:
		priority = SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE;
		break;
	case DataMovementReason::TEAM_HEALTHY:
		priority = SERVER_KNOBS->PRIORITY_TEAM_HEALTHY;
		break;
	case DataMovementReason::TEAM_CONTAINS_UNDESIRED_SERVER:
		priority = SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER;
		break;
	case DataMovementReason::TEAM_REDUNDANT:
		priority = SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT;
		break;
	case DataMovementReason::MERGE_SHARD:
		priority = SERVER_KNOBS->PRIORITY_MERGE_SHARD;
		break;
	case DataMovementReason::POPULATE_REGION:
		priority = SERVER_KNOBS->PRIORITY_POPULATE_REGION;
		break;
	case DataMovementReason::TEAM_UNHEALTHY:
		priority = SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY;
		break;
	case DataMovementReason::TEAM_2_LEFT:
		priority = SERVER_KNOBS->PRIORITY_TEAM_2_LEFT;
		break;
	case DataMovementReason::TEAM_1_LEFT:
		priority = SERVER_KNOBS->PRIORITY_TEAM_1_LEFT;
		break;
	case DataMovementReason::TEAM_FAILED:
		priority = SERVER_KNOBS->PRIORITY_TEAM_FAILED;
		break;
	case DataMovementReason::TEAM_0_LEFT:
		priority = SERVER_KNOBS->PRIORITY_TEAM_0_LEFT;
		break;
	case DataMovementReason::SPLIT_SHARD:
		priority = SERVER_KNOBS->PRIORITY_SPLIT_SHARD;
		break;
	}
	return priority;
}

struct RelocateData {
	KeyRange keys;
	int priority;
	int boundaryPriority;
	int healthPriority;
	RelocateReason reason;

	double startTime;
	UID randomId;
	UID dataMoveId;
	int workFactor;
	std::vector<UID> src;
	std::vector<UID> completeSources;
	std::vector<UID> completeDests;
	bool wantsNewServers;
	bool cancellable;
	TraceInterval interval;
	std::shared_ptr<DataMove> dataMove;

	RelocateData()
	  : priority(-1), boundaryPriority(-1), healthPriority(-1), reason(RelocateReason::INVALID), startTime(-1),
	    dataMoveId(anonymousShardId), workFactor(0), wantsNewServers(false), cancellable(false),
	    interval("QueuedRelocation") {}
	explicit RelocateData(RelocateShard const& rs)
	  : keys(rs.keys), priority(rs.priority), boundaryPriority(isBoundaryPriority(rs.priority) ? rs.priority : -1),
	    healthPriority(isHealthPriority(rs.priority) ? rs.priority : -1), reason(rs.reason), startTime(now()),
	    randomId(deterministicRandom()->randomUniqueID()), dataMoveId(rs.dataMoveId), workFactor(0),
	    wantsNewServers(
	        isDataMovementForMountainChopper(rs.moveReason) || isDataMovementForValleyFiller(rs.moveReason) ||
	        rs.moveReason == DataMovementReason::SPLIT_SHARD || rs.moveReason == DataMovementReason::TEAM_REDUNDANT),
	    cancellable(true), interval("QueuedRelocation"), dataMove(rs.dataMove) {
		if (dataMove != nullptr) {
			this->src.insert(this->src.end(), dataMove->meta.src.begin(), dataMove->meta.src.end());
		}
	}

	static bool isHealthPriority(int priority) {
		return priority == SERVER_KNOBS->PRIORITY_POPULATE_REGION ||
		       priority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY || priority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
		       priority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT || priority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT ||
		       priority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT || priority == SERVER_KNOBS->PRIORITY_TEAM_HEALTHY ||
		       priority == SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER ||
		       priority == SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE;
	}

	static bool isBoundaryPriority(int priority) {
		return priority == SERVER_KNOBS->PRIORITY_SPLIT_SHARD || priority == SERVER_KNOBS->PRIORITY_MERGE_SHARD;
	}

	bool isRestore() const { return this->dataMove != nullptr; }

	bool operator>(const RelocateData& rhs) const {
		return priority != rhs.priority
		           ? priority > rhs.priority
		           : (startTime != rhs.startTime ? startTime < rhs.startTime : randomId > rhs.randomId);
	}

	bool operator==(const RelocateData& rhs) const {
		return priority == rhs.priority && boundaryPriority == rhs.boundaryPriority &&
		       healthPriority == rhs.healthPriority && reason == rhs.reason && keys == rhs.keys &&
		       startTime == rhs.startTime && workFactor == rhs.workFactor && src == rhs.src &&
		       completeSources == rhs.completeSources && wantsNewServers == rhs.wantsNewServers &&
		       randomId == rhs.randomId;
	}
	bool operator!=(const RelocateData& rhs) const { return !(*this == rhs); }
};

class ParallelTCInfo final : public ReferenceCounted<ParallelTCInfo>, public IDataDistributionTeam {
	std::vector<Reference<IDataDistributionTeam>> teams;
	std::vector<UID> tempServerIDs;

	int64_t sum(std::function<int64_t(IDataDistributionTeam const&)> func) const {
		int64_t result = 0;
		for (const auto& team : teams) {
			result += func(*team);
		}
		return result;
	}

	template <class T>
	std::vector<T> collect(std::function<std::vector<T>(IDataDistributionTeam const&)> func) const {
		std::vector<T> result;

		for (const auto& team : teams) {
			std::vector<T> newItems = func(*team);
			result.insert(result.end(), newItems.begin(), newItems.end());
		}
		return result;
	}

	bool any(std::function<bool(IDataDistributionTeam const&)> func) const {
		for (const auto& team : teams) {
			if (func(*team)) {
				return true;
			}
		}
		return false;
	}

public:
	ParallelTCInfo() = default;
	explicit ParallelTCInfo(ParallelTCInfo const& info) : teams(info.teams), tempServerIDs(info.tempServerIDs){};

	void addTeam(Reference<IDataDistributionTeam> team) { teams.push_back(team); }

	void clear() { teams.clear(); }

	bool all(std::function<bool(IDataDistributionTeam const&)> func) const {
		return !any([func](IDataDistributionTeam const& team) { return !func(team); });
	}

	std::vector<StorageServerInterface> getLastKnownServerInterfaces() const override {
		return collect<StorageServerInterface>(
		    [](IDataDistributionTeam const& team) { return team.getLastKnownServerInterfaces(); });
	}

	int size() const override {
		int totalSize = 0;
		for (auto it = teams.begin(); it != teams.end(); it++) {
			totalSize += (*it)->size();
		}
		return totalSize;
	}

	std::vector<UID> const& getServerIDs() const override {
		static std::vector<UID> tempServerIDs;
		tempServerIDs.clear();
		for (const auto& team : teams) {
			std::vector<UID> const& childIDs = team->getServerIDs();
			tempServerIDs.insert(tempServerIDs.end(), childIDs.begin(), childIDs.end());
		}
		return tempServerIDs;
	}

	void addDataInFlightToTeam(int64_t delta) override {
		for (auto& team : teams) {
			team->addDataInFlightToTeam(delta);
		}
	}

	void addReadInFlightToTeam(int64_t delta) override {
		for (auto& team : teams) {
			team->addReadInFlightToTeam(delta);
		}
	}

	int64_t getDataInFlightToTeam() const override {
		return sum([](IDataDistributionTeam const& team) { return team.getDataInFlightToTeam(); });
	}

	int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0) const override {
		return sum([includeInFlight, inflightPenalty](IDataDistributionTeam const& team) {
			return team.getLoadBytes(includeInFlight, inflightPenalty);
		});
	}

	int64_t getReadInFlightToTeam() const override {
		return sum([](IDataDistributionTeam const& team) { return team.getReadInFlightToTeam(); });
	}

	double getLoadReadBandwidth(bool includeInFlight = true, double inflightPenalty = 1.0) const override {
		return sum([includeInFlight, inflightPenalty](IDataDistributionTeam const& team) {
			return team.getLoadReadBandwidth(includeInFlight, inflightPenalty);
		});
	}

	int64_t getMinAvailableSpace(bool includeInFlight = true) const override {
		int64_t result = std::numeric_limits<int64_t>::max();
		for (const auto& team : teams) {
			result = std::min(result, team->getMinAvailableSpace(includeInFlight));
		}
		return result;
	}

	double getMinAvailableSpaceRatio(bool includeInFlight = true) const override {
		double result = std::numeric_limits<double>::max();
		for (const auto& team : teams) {
			result = std::min(result, team->getMinAvailableSpaceRatio(includeInFlight));
		}
		return result;
	}

	bool hasHealthyAvailableSpace(double minRatio) const override {
		return all([minRatio](IDataDistributionTeam const& team) { return team.hasHealthyAvailableSpace(minRatio); });
	}

	Future<Void> updateStorageMetrics() override {
		std::vector<Future<Void>> futures;

		for (auto& team : teams) {
			futures.push_back(team->updateStorageMetrics());
		}
		return waitForAll(futures);
	}

	bool isOptimal() const override {
		return all([](IDataDistributionTeam const& team) { return team.isOptimal(); });
	}

	bool isWrongConfiguration() const override {
		return any([](IDataDistributionTeam const& team) { return team.isWrongConfiguration(); });
	}
	void setWrongConfiguration(bool wrongConfiguration) override {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setWrongConfiguration(wrongConfiguration);
		}
	}

	bool isHealthy() const override {
		return all([](IDataDistributionTeam const& team) { return team.isHealthy(); });
	}

	void setHealthy(bool h) override {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setHealthy(h);
		}
	}

	int getPriority() const override {
		int priority = 0;
		for (auto it = teams.begin(); it != teams.end(); it++) {
			priority = std::max(priority, (*it)->getPriority());
		}
		return priority;
	}

	void setPriority(int p) override {
		for (auto it = teams.begin(); it != teams.end(); it++) {
			(*it)->setPriority(p);
		}
	}
	void addref() const override { ReferenceCounted<ParallelTCInfo>::addref(); }
	void delref() const override { ReferenceCounted<ParallelTCInfo>::delref(); }

	void addServers(const std::vector<UID>& servers) override {
		ASSERT(!teams.empty());
		teams[0]->addServers(servers);
	}

	std::string getTeamID() const override {
		std::string id;
		for (int i = 0; i < teams.size(); i++) {
			auto const& team = teams[i];
			id += (i == teams.size() - 1) ? team->getTeamID() : format("%s, ", team->getTeamID().c_str());
		}
		return id;
	}
};

struct Busyness {
	std::vector<int> ledger;

	Busyness() : ledger(10, 0) {}

	bool canLaunch(int prio, int work) const {
		ASSERT(prio > 0 && prio < 1000);
		return ledger[prio / 100] <= WORK_FULL_UTILIZATION - work; // allow for rounding errors in double division
	}
	void addWork(int prio, int work) {
		ASSERT(prio > 0 && prio < 1000);
		for (int i = 0; i <= (prio / 100); i++)
			ledger[i] += work;
	}
	void removeWork(int prio, int work) { addWork(prio, -work); }
	std::string toString() {
		std::string result;
		for (int i = 1; i < ledger.size();) {
			int j = i + 1;
			while (j < ledger.size() && ledger[i] == ledger[j])
				j++;
			if (i != 1)
				result += ", ";
			result += i + 1 == j ? format("%03d", i * 100) : format("%03d/%03d", i * 100, (j - 1) * 100);
			result +=
			    format("=%1.02f (%d/%d)", (float)ledger[i] / WORK_FULL_UTILIZATION, ledger[i], WORK_FULL_UTILIZATION);
			i = j;
		}
		return result;
	}
};

// find the "workFactor" for this, were it launched now
int getSrcWorkFactor(RelocateData const& relocation, int singleRegionTeamSize) {
	if (relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT ||
	    relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT)
		return WORK_FULL_UTILIZATION / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
	else if (relocation.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT)
		return WORK_FULL_UTILIZATION / 2 / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
	else // for now we assume that any message at a lower priority can best be assumed to have a full team left for work
		return WORK_FULL_UTILIZATION / singleRegionTeamSize / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_SOURCE_SERVER;
}

int getDestWorkFactor() {
	// Work of moving a shard is even across destination servers
	return WORK_FULL_UTILIZATION / SERVER_KNOBS->RELOCATION_PARALLELISM_PER_DEST_SERVER;
}

// Data movement's resource control: Do not overload servers used for the RelocateData
// return true if servers are not too busy to launch the relocation
// This ensure source servers will not be overloaded.
bool canLaunchSrc(RelocateData& relocation,
                  int teamSize,
                  int singleRegionTeamSize,
                  std::map<UID, Busyness>& busymap,
                  std::vector<RelocateData> cancellableRelocations) {
	// assert this has not already been launched
	ASSERT(relocation.workFactor == 0);
	ASSERT(relocation.src.size() != 0);
	ASSERT(teamSize >= singleRegionTeamSize);

	// find the "workFactor" for this, were it launched now
	int workFactor = getSrcWorkFactor(relocation, singleRegionTeamSize);
	int neededServers = std::min<int>(relocation.src.size(), teamSize - singleRegionTeamSize + 1);
	if (SERVER_KNOBS->USE_OLD_NEEDED_SERVERS) {
		neededServers = std::max(1, (int)relocation.src.size() - teamSize + 1);
	}
	// see if each of the SS can launch this task
	for (int i = 0; i < relocation.src.size(); i++) {
		// For each source server for this relocation, copy and modify its busyness to reflect work that WOULD be
		// cancelled
		auto busyCopy = busymap[relocation.src[i]];
		for (int j = 0; j < cancellableRelocations.size(); j++) {
			auto& servers = cancellableRelocations[j].src;
			if (std::count(servers.begin(), servers.end(), relocation.src[i]))
				busyCopy.removeWork(cancellableRelocations[j].priority, cancellableRelocations[j].workFactor);
		}
		// Use this modified busyness to check if this relocation could be launched
		if (busyCopy.canLaunch(relocation.priority, workFactor)) {
			--neededServers;
			if (neededServers == 0)
				return true;
		}
	}
	return false;
}

// candidateTeams is a vector containing one team per datacenter, the team(s) DD is planning on moving the shard to.
bool canLaunchDest(const std::vector<std::pair<Reference<IDataDistributionTeam>, bool>>& candidateTeams,
                   int priority,
                   std::map<UID, Busyness>& busymapDest) {
	// fail switch if this is causing issues
	if (SERVER_KNOBS->RELOCATION_PARALLELISM_PER_DEST_SERVER <= 0) {
		return true;
	}
	int workFactor = getDestWorkFactor();
	for (auto& [team, _] : candidateTeams) {
		for (UID id : team->getServerIDs()) {
			if (!busymapDest[id].canLaunch(priority, workFactor)) {
				return false;
			}
		}
	}
	return true;
}

// update busyness for each server
void launch(RelocateData& relocation, std::map<UID, Busyness>& busymap, int singleRegionTeamSize) {
	// if we are here this means that we can launch and should adjust all the work the servers can do
	relocation.workFactor = getSrcWorkFactor(relocation, singleRegionTeamSize);
	for (int i = 0; i < relocation.src.size(); i++)
		busymap[relocation.src[i]].addWork(relocation.priority, relocation.workFactor);
}

void launchDest(RelocateData& relocation,
                const std::vector<std::pair<Reference<IDataDistributionTeam>, bool>>& candidateTeams,
                std::map<UID, Busyness>& destBusymap) {
	ASSERT(relocation.completeDests.empty());
	int destWorkFactor = getDestWorkFactor();
	for (auto& [team, _] : candidateTeams) {
		for (UID id : team->getServerIDs()) {
			relocation.completeDests.push_back(id);
			destBusymap[id].addWork(relocation.priority, destWorkFactor);
		}
	}
}
void completeDest(RelocateData const& relocation, std::map<UID, Busyness>& destBusymap) {
	int destWorkFactor = getDestWorkFactor();
	for (UID id : relocation.completeDests) {
		destBusymap[id].removeWork(relocation.priority, destWorkFactor);
	}
}

void complete(RelocateData const& relocation, std::map<UID, Busyness>& busymap, std::map<UID, Busyness>& destBusymap) {
	ASSERT(relocation.workFactor > 0);
	for (int i = 0; i < relocation.src.size(); i++)
		busymap[relocation.src[i]].removeWork(relocation.priority, relocation.workFactor);

	completeDest(relocation, destBusymap);
}

// Cancells in-flight data moves intersecting with range.
ACTOR Future<Void> cancelDataMove(struct DDQueueData* self, KeyRange range, const DDEnabledState* ddEnabledState);

ACTOR Future<Void> dataDistributionRelocator(struct DDQueueData* self,
                                             RelocateData rd,
                                             Future<Void> prevCleanup,
                                             const DDEnabledState* ddEnabledState);

struct DDQueueData {
	struct DDDataMove {
		DDDataMove() = default;
		explicit DDDataMove(UID id) : id(id) {}

		bool isValid() const { return id.isValid(); }

		UID id;
		Future<Void> cancel;
	};

	ActorCollectionNoErrors noErrorActors; // has to be the last one to be destroyed because other Actors may use it.
	UID distributorId;
	MoveKeysLock lock;
	Database cx;
	std::shared_ptr<IDDTxnProcessor> txnProcessor;

	std::vector<TeamCollectionInterface> teamCollections;
	Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure;
	PromiseStream<Promise<int64_t>> getAverageShardBytes;

	FlowLock startMoveKeysParallelismLock;
	FlowLock finishMoveKeysParallelismLock;
	FlowLock cleanUpDataMoveParallelismLock;
	Reference<FlowLock> fetchSourceLock;

	int activeRelocations;
	int queuedRelocations;
	int64_t bytesWritten;
	int teamSize;
	int singleRegionTeamSize;

	std::map<UID, Busyness> busymap; // UID is serverID
	std::map<UID, Busyness> destBusymap; // UID is serverID

	KeyRangeMap<RelocateData> queueMap;
	std::set<RelocateData, std::greater<RelocateData>> fetchingSourcesQueue;
	std::set<RelocateData, std::greater<RelocateData>> fetchKeysComplete;
	KeyRangeActorMap getSourceActors;
	std::map<UID, std::set<RelocateData, std::greater<RelocateData>>>
	    queue; // Key UID is serverID, value is the serverID's set of RelocateData to relocate
	// The last time one server was selected as source team for read rebalance reason. We want to throttle read
	// rebalance on time bases because the read workload sample update has delay after the previous moving
	std::map<UID, double> lastAsSource;

	KeyRangeMap<RelocateData> inFlight;
	// Track all actors that relocates specified keys to a good place; Key: keyRange; Value: actor
	KeyRangeActorMap inFlightActors;
	KeyRangeMap<DDDataMove> dataMoves;

	Promise<Void> error;
	PromiseStream<RelocateData> dataTransferComplete;
	PromiseStream<RelocateData> relocationComplete;
	PromiseStream<RelocateData> fetchSourceServersComplete; // find source SSs for a relocate range

	PromiseStream<RelocateShard> output;
	FutureStream<RelocateShard> input;
	PromiseStream<GetMetricsRequest> getShardMetrics;
	PromiseStream<GetTopKMetricsRequest> getTopKMetrics;

	double lastInterval;
	int suppressIntervals;

	Reference<AsyncVar<bool>> rawProcessingUnhealthy; // many operations will remove relocations before adding a new
	                                                  // one, so delay a small time before settling on a new number.
	Reference<AsyncVar<bool>> rawProcessingWiggle;

	std::map<int, int> priority_relocations;
	int unhealthyRelocations;

	Reference<EventCacheHolder> movedKeyServersEventHolder;

	void startRelocation(int priority, int healthPriority) {
		// Although PRIORITY_TEAM_REDUNDANT has lower priority than split and merge shard movement,
		// we must count it into unhealthyRelocations; because team removers relies on unhealthyRelocations to
		// ensure a team remover will not start before the previous one finishes removing a team and move away data
		// NOTE: split and merge shard have higher priority. If they have to wait for unhealthyRelocations = 0,
		// deadlock may happen: split/merge shard waits for unhealthyRelocations, while blocks team_redundant.
		if (healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT) {
			unhealthyRelocations++;
			rawProcessingUnhealthy->set(true);
		}
		if (healthPriority == SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE) {
			rawProcessingWiggle->set(true);
		}
		priority_relocations[priority]++;
	}
	void finishRelocation(int priority, int healthPriority) {
		if (healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT ||
		    healthPriority == SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT) {
			unhealthyRelocations--;
			ASSERT(unhealthyRelocations >= 0);
			if (unhealthyRelocations == 0) {
				rawProcessingUnhealthy->set(false);
			}
		}
		priority_relocations[priority]--;
		if (priority_relocations[SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE] == 0) {
			rawProcessingWiggle->set(false);
		}
	}

	DDQueueData(UID mid,
	            MoveKeysLock lock,
	            Database cx,
	            std::vector<TeamCollectionInterface> teamCollections,
	            Reference<ShardsAffectedByTeamFailure> sABTF,
	            PromiseStream<Promise<int64_t>> getAverageShardBytes,
	            int teamSize,
	            int singleRegionTeamSize,
	            PromiseStream<RelocateShard> output,
	            FutureStream<RelocateShard> input,
	            PromiseStream<GetMetricsRequest> getShardMetrics,
	            PromiseStream<GetTopKMetricsRequest> getTopKMetrics)
	  : distributorId(mid), lock(lock), cx(cx), txnProcessor(new DDTxnProcessor(cx)), teamCollections(teamCollections),
	    shardsAffectedByTeamFailure(sABTF), getAverageShardBytes(getAverageShardBytes),
	    startMoveKeysParallelismLock(SERVER_KNOBS->DD_MOVE_KEYS_PARALLELISM),
	    finishMoveKeysParallelismLock(SERVER_KNOBS->DD_MOVE_KEYS_PARALLELISM),
	    cleanUpDataMoveParallelismLock(SERVER_KNOBS->DD_MOVE_KEYS_PARALLELISM),
	    fetchSourceLock(new FlowLock(SERVER_KNOBS->DD_FETCH_SOURCE_PARALLELISM)), activeRelocations(0),
	    queuedRelocations(0), bytesWritten(0), teamSize(teamSize), singleRegionTeamSize(singleRegionTeamSize),
	    output(output), input(input), getShardMetrics(getShardMetrics), getTopKMetrics(getTopKMetrics), lastInterval(0),
	    suppressIntervals(0), rawProcessingUnhealthy(new AsyncVar<bool>(false)),
	    rawProcessingWiggle(new AsyncVar<bool>(false)), unhealthyRelocations(0),
	    movedKeyServersEventHolder(makeReference<EventCacheHolder>("MovedKeyServers")) {}

	void validate() {
		if (EXPENSIVE_VALIDATION) {
			for (auto it = fetchingSourcesQueue.begin(); it != fetchingSourcesQueue.end(); ++it) {
				// relocates in the fetching queue do not have src servers yet.
				if (it->src.size())
					TraceEvent(SevError, "DDQueueValidateError1")
					    .detail("Problem", "relocates in the fetching queue do not have src servers yet");

				// relocates in the fetching queue do not have a work factor yet.
				if (it->workFactor != 0.0)
					TraceEvent(SevError, "DDQueueValidateError2")
					    .detail("Problem", "relocates in the fetching queue do not have a work factor yet");

				// relocates in the fetching queue are in the queueMap.
				auto range = queueMap.rangeContaining(it->keys.begin);
				if (range.value() != *it || range.range() != it->keys)
					TraceEvent(SevError, "DDQueueValidateError3")
					    .detail("Problem", "relocates in the fetching queue are in the queueMap");
			}

			/*
			for( auto it = queue.begin(); it != queue.end(); ++it ) {
			    for( auto rdit = it->second.begin(); rdit != it->second.end(); ++rdit ) {
			        // relocates in the queue are in the queueMap exactly.
			        auto range = queueMap.rangeContaining( rdit->keys.begin );
			        if( range.value() != *rdit || range.range() != rdit->keys )
			            TraceEvent(SevError, "DDQueueValidateError4").detail("Problem", "relocates in the queue are in the queueMap exactly")
			            .detail("RangeBegin", range.range().begin)
			            .detail("RangeEnd", range.range().end)
			            .detail("RelocateBegin2", range.value().keys.begin)
			            .detail("RelocateEnd2", range.value().keys.end)
			            .detail("RelocateStart", range.value().startTime)
			            .detail("MapStart", rdit->startTime)
			            .detail("RelocateWork", range.value().workFactor)
			            .detail("MapWork", rdit->workFactor)
			            .detail("RelocateSrc", range.value().src.size())
			            .detail("MapSrc", rdit->src.size())
			            .detail("RelocatePrio", range.value().priority)
			            .detail("MapPrio", rdit->priority);

			        // relocates in the queue have src servers
			        if( !rdit->src.size() )
			            TraceEvent(SevError, "DDQueueValidateError5").detail("Problem", "relocates in the queue have src servers");

			        // relocates in the queue do not have a work factor yet.
			        if( rdit->workFactor != 0.0 )
			            TraceEvent(SevError, "DDQueueValidateError6").detail("Problem", "relocates in the queue do not have a work factor yet");

			        bool contains = false;
			        for( int i = 0; i < rdit->src.size(); i++ ) {
			            if( rdit->src[i] == it->first ) {
			                contains = true;
			                break;
			            }
			        }
			        if( !contains )
			            TraceEvent(SevError, "DDQueueValidateError7").detail("Problem", "queued relocate data does not include ss under which its filed");
			    }
			}*/

			auto inFlightRanges = inFlight.ranges();
			for (auto it = inFlightRanges.begin(); it != inFlightRanges.end(); ++it) {
				for (int i = 0; i < it->value().src.size(); i++) {
					// each server in the inFlight map is in the busymap
					if (!busymap.count(it->value().src[i]))
						TraceEvent(SevError, "DDQueueValidateError8")
						    .detail("Problem", "each server in the inFlight map is in the busymap");

					// relocate data that is inFlight is not also in the queue
					if (queue[it->value().src[i]].count(it->value()))
						TraceEvent(SevError, "DDQueueValidateError9")
						    .detail("Problem", "relocate data that is inFlight is not also in the queue");
				}

				for (int i = 0; i < it->value().completeDests.size(); i++) {
					// each server in the inFlight map is in the dest busymap
					if (!destBusymap.count(it->value().completeDests[i]))
						TraceEvent(SevError, "DDQueueValidateError10")
						    .detail("Problem", "each server in the inFlight map is in the destBusymap");
				}

				// in flight relocates have source servers
				if (it->value().startTime != -1 && !it->value().src.size())
					TraceEvent(SevError, "DDQueueValidateError11")
					    .detail("Problem", "in flight relocates have source servers");

				if (inFlightActors.liveActorAt(it->range().begin)) {
					// the key range in the inFlight map matches the key range in the RelocateData message
					if (it->value().keys != it->range())
						TraceEvent(SevError, "DDQueueValidateError12")
						    .detail(
						        "Problem",
						        "the key range in the inFlight map matches the key range in the RelocateData message");
				} else if (it->value().cancellable) {
					TraceEvent(SevError, "DDQueueValidateError13")
					    .detail("Problem", "key range is cancellable but not in flight!")
					    .detail("Range", it->range());
				}
			}

			for (auto it = busymap.begin(); it != busymap.end(); ++it) {
				for (int i = 0; i < it->second.ledger.size() - 1; i++) {
					if (it->second.ledger[i] < it->second.ledger[i + 1])
						TraceEvent(SevError, "DDQueueValidateError14")
						    .detail("Problem", "ascending ledger problem")
						    .detail("LedgerLevel", i)
						    .detail("LedgerValueA", it->second.ledger[i])
						    .detail("LedgerValueB", it->second.ledger[i + 1]);
					if (it->second.ledger[i] < 0.0)
						TraceEvent(SevError, "DDQueueValidateError15")
						    .detail("Problem", "negative ascending problem")
						    .detail("LedgerLevel", i)
						    .detail("LedgerValue", it->second.ledger[i]);
				}
			}

			for (auto it = destBusymap.begin(); it != destBusymap.end(); ++it) {
				for (int i = 0; i < it->second.ledger.size() - 1; i++) {
					if (it->second.ledger[i] < it->second.ledger[i + 1])
						TraceEvent(SevError, "DDQueueValidateError16")
						    .detail("Problem", "ascending ledger problem")
						    .detail("LedgerLevel", i)
						    .detail("LedgerValueA", it->second.ledger[i])
						    .detail("LedgerValueB", it->second.ledger[i + 1]);
					if (it->second.ledger[i] < 0.0)
						TraceEvent(SevError, "DDQueueValidateError17")
						    .detail("Problem", "negative ascending problem")
						    .detail("LedgerLevel", i)
						    .detail("LedgerValue", it->second.ledger[i]);
				}
			}

			std::set<RelocateData, std::greater<RelocateData>> queuedRelocationsMatch;
			for (auto it = queue.begin(); it != queue.end(); ++it)
				queuedRelocationsMatch.insert(it->second.begin(), it->second.end());
			ASSERT(queuedRelocations == queuedRelocationsMatch.size() + fetchingSourcesQueue.size());

			int testActive = 0;
			for (auto it = priority_relocations.begin(); it != priority_relocations.end(); ++it)
				testActive += it->second;
			ASSERT(activeRelocations + queuedRelocations == testActive);
		}
	}

	ACTOR static Future<Void> getSourceServersForRange(DDQueueData* self,
	                                                   RelocateData input,
	                                                   PromiseStream<RelocateData> output,
	                                                   Reference<FlowLock> fetchLock) {

		// FIXME: is the merge case needed
		if (input.priority == SERVER_KNOBS->PRIORITY_MERGE_SHARD) {
			wait(delay(0.5, TaskPriority::DataDistributionVeryLow));
		} else {
			wait(delay(0.0001, TaskPriority::DataDistributionLaunch));
		}

		wait(fetchLock->take(TaskPriority::DataDistributionLaunch));
		state FlowLock::Releaser releaser(*fetchLock);

		IDDTxnProcessor::SourceServers res = wait(self->txnProcessor->getSourceServersForRange(input.keys));
		input.src = std::move(res.srcServers);
		input.completeSources = std::move(res.completeSources);
		output.send(input);
		return Void();
	}

	// This function cannot handle relocation requests which split a shard into three pieces
	void queueRelocation(RelocateShard rs, std::set<UID>& serversToLaunchFrom) {
		//TraceEvent("QueueRelocationBegin").detail("Begin", rd.keys.begin).detail("End", rd.keys.end);

		// remove all items from both queues that are fully contained in the new relocation (i.e. will be overwritten)
		RelocateData rd(rs);
		bool hasHealthPriority = RelocateData::isHealthPriority(rd.priority);
		bool hasBoundaryPriority = RelocateData::isBoundaryPriority(rd.priority);

		auto ranges = queueMap.intersectingRanges(rd.keys);
		for (auto r = ranges.begin(); r != ranges.end(); ++r) {
			RelocateData& rrs = r->value();

			auto fetchingSourcesItr = fetchingSourcesQueue.find(rrs);
			bool foundActiveFetching = fetchingSourcesItr != fetchingSourcesQueue.end();
			std::set<RelocateData, std::greater<RelocateData>>* firstQueue;
			std::set<RelocateData, std::greater<RelocateData>>::iterator firstRelocationItr;
			bool foundActiveRelocation = false;

			if (!foundActiveFetching && rrs.src.size()) {
				firstQueue = &queue[rrs.src[0]];
				firstRelocationItr = firstQueue->find(rrs);
				foundActiveRelocation = firstRelocationItr != firstQueue->end();
			}

			// If there is a queued job that wants data relocation which we are about to cancel/modify,
			//  make sure that we keep the relocation intent for the job that we queue up
			if (foundActiveFetching || foundActiveRelocation) {
				rd.wantsNewServers |= rrs.wantsNewServers;
				rd.startTime = std::min(rd.startTime, rrs.startTime);
				if (!hasHealthPriority) {
					rd.healthPriority = std::max(rd.healthPriority, rrs.healthPriority);
				}
				if (!hasBoundaryPriority) {
					rd.boundaryPriority = std::max(rd.boundaryPriority, rrs.boundaryPriority);
				}
				rd.priority = std::max(rd.priority, std::max(rd.boundaryPriority, rd.healthPriority));
			}

			if (rd.keys.contains(rrs.keys)) {
				if (foundActiveFetching)
					fetchingSourcesQueue.erase(fetchingSourcesItr);
				else if (foundActiveRelocation) {
					firstQueue->erase(firstRelocationItr);
					for (int i = 1; i < rrs.src.size(); i++)
						queue[rrs.src[i]].erase(rrs);
				}
			}

			if (foundActiveFetching || foundActiveRelocation) {
				serversToLaunchFrom.insert(rrs.src.begin(), rrs.src.end());
				/*TraceEvent(rrs.interval.end(), mi.id()).detail("Result","Cancelled")
				    .detail("WasFetching", foundActiveFetching).detail("Contained", rd.keys.contains( rrs.keys ));*/
				queuedRelocations--;
				TraceEvent(SevVerbose, "QueuedRelocationsChanged")
				    .detail("DataMoveID", rrs.dataMoveId)
				    .detail("RandomID", rrs.randomId)
				    .detail("Total", queuedRelocations);
				finishRelocation(rrs.priority, rrs.healthPriority);
			}
		}

		// determine the final state of the relocations map
		auto affectedQueuedItems = queueMap.getAffectedRangesAfterInsertion(rd.keys, rd);

		// put the new request into the global map of requests (modifies the ranges already present)
		queueMap.insert(rd.keys, rd);

		// cancel all the getSourceServers actors that intersect the new range that we will be getting
		getSourceActors.cancel(KeyRangeRef(affectedQueuedItems.front().begin, affectedQueuedItems.back().end));

		// update fetchingSourcesQueue and the per-server queue based on truncated ranges after insertion, (re-)launch
		// getSourceServers
		auto queueMapItr = queueMap.rangeContaining(affectedQueuedItems[0].begin);
		for (int r = 0; r < affectedQueuedItems.size(); ++r, ++queueMapItr) {
			// ASSERT(queueMapItr->value() == queueMap.rangeContaining(affectedQueuedItems[r].begin)->value());
			RelocateData& rrs = queueMapItr->value();

			if (rrs.src.size() == 0 && (rrs.keys == rd.keys || fetchingSourcesQueue.erase(rrs) > 0)) {
				rrs.keys = affectedQueuedItems[r];

				rrs.interval = TraceInterval("QueuedRelocation");
				/*TraceEvent(rrs.interval.begin(), distributorId);
				  .detail("KeyBegin", rrs.keys.begin).detail("KeyEnd", rrs.keys.end)
				    .detail("Priority", rrs.priority).detail("WantsNewServers", rrs.wantsNewServers);*/
				queuedRelocations++;
				TraceEvent(SevVerbose, "QueuedRelocationsChanged")
				    .detail("DataMoveID", rrs.dataMoveId)
				    .detail("RandomID", rrs.randomId)
				    .detail("Total", queuedRelocations);
				startRelocation(rrs.priority, rrs.healthPriority);

				fetchingSourcesQueue.insert(rrs);
				getSourceActors.insert(
				    rrs.keys, getSourceServersForRange(this, rrs, fetchSourceServersComplete, fetchSourceLock));
			} else {
				RelocateData newData(rrs);
				newData.keys = affectedQueuedItems[r];
				ASSERT(rrs.src.size() || rrs.startTime == -1);

				bool foundActiveRelocation = false;
				for (int i = 0; i < rrs.src.size(); i++) {
					auto& serverQueue = queue[rrs.src[i]];

					if (serverQueue.erase(rrs) > 0) {
						if (!foundActiveRelocation) {
							newData.interval = TraceInterval("QueuedRelocation");
							/*TraceEvent(newData.interval.begin(), distributorId);
							  .detail("KeyBegin", newData.keys.begin).detail("KeyEnd", newData.keys.end)
							    .detail("Priority", newData.priority).detail("WantsNewServers",
							  newData.wantsNewServers);*/
							queuedRelocations++;
							TraceEvent(SevVerbose, "QueuedRelocationsChanged")
							    .detail("DataMoveID", newData.dataMoveId)
							    .detail("RandomID", newData.randomId)
							    .detail("Total", queuedRelocations);
							startRelocation(newData.priority, newData.healthPriority);
							foundActiveRelocation = true;
						}

						serverQueue.insert(newData);
					} else
						break;
				}

				// We update the keys of a relocation even if it is "dead" since it helps validate()
				rrs.keys = affectedQueuedItems[r];
				rrs.interval = newData.interval;
			}
		}

		/*TraceEvent("ReceivedRelocateShard", distributorId)
		  .detail("KeyBegin", rd.keys.begin)
		  .detail("KeyEnd", rd.keys.end)
		    .detail("Priority", rd.priority)
		    .detail("AffectedRanges", affectedQueuedItems.size()); */
	}

	void completeSourceFetch(const RelocateData& results) {
		ASSERT(fetchingSourcesQueue.count(results));

		// logRelocation( results, "GotSourceServers" );

		fetchingSourcesQueue.erase(results);
		queueMap.insert(results.keys, results);
		for (int i = 0; i < results.src.size(); i++) {
			queue[results.src[i]].insert(results);
		}
		updateLastAsSource(results.src);
	}

	void logRelocation(const RelocateData& rd, const char* title) {
		std::string busyString;
		for (int i = 0; i < rd.src.size() && i < teamSize * 2; i++)
			busyString += describe(rd.src[i]) + " - (" + busymap[rd.src[i]].toString() + "); ";

		TraceEvent(title, distributorId)
		    .detail("KeyBegin", rd.keys.begin)
		    .detail("KeyEnd", rd.keys.end)
		    .detail("Priority", rd.priority)
		    .detail("WorkFactor", rd.workFactor)
		    .detail("SourceServerCount", rd.src.size())
		    .detail("SourceServers", describe(rd.src, teamSize * 2))
		    .detail("SourceBusyness", busyString);
	}

	void launchQueuedWork(KeyRange keys, const DDEnabledState* ddEnabledState) {
		// combine all queued work in the key range and check to see if there is anything to launch
		std::set<RelocateData, std::greater<RelocateData>> combined;
		auto f = queueMap.intersectingRanges(keys);
		for (auto it = f.begin(); it != f.end(); ++it) {
			if (it->value().src.size() && queue[it->value().src[0]].count(it->value()))
				combined.insert(it->value());
		}
		launchQueuedWork(combined, ddEnabledState);
	}

	void launchQueuedWork(const std::set<UID>& serversToLaunchFrom, const DDEnabledState* ddEnabledState) {
		// combine all work from the source servers to see if there is anything new to launch
		std::set<RelocateData, std::greater<RelocateData>> combined;
		for (auto id : serversToLaunchFrom) {
			auto& queuedWork = queue[id];
			auto it = queuedWork.begin();
			for (int j = 0; j < teamSize && it != queuedWork.end(); j++) {
				combined.insert(*it);
				++it;
			}
		}
		launchQueuedWork(combined, ddEnabledState);
	}

	void launchQueuedWork(RelocateData launchData, const DDEnabledState* ddEnabledState) {
		// check a single RelocateData to see if it can be launched
		std::set<RelocateData, std::greater<RelocateData>> combined;
		combined.insert(launchData);
		launchQueuedWork(combined, ddEnabledState);
	}

	// For each relocateData rd in the queue, check if there exist inflight relocate data whose keyrange is overlapped
	// with rd. If there exist, cancel them by cancelling their actors and reducing the src servers' busyness of those
	// canceled inflight relocateData. Launch the relocation for the rd.
	void launchQueuedWork(std::set<RelocateData, std::greater<RelocateData>> combined,
	                      const DDEnabledState* ddEnabledState) {
		int startedHere = 0;
		double startTime = now();
		// kick off relocators from items in the queue as need be
		std::set<RelocateData, std::greater<RelocateData>>::iterator it = combined.begin();
		for (; it != combined.end(); it++) {
			RelocateData rd(*it);

			// Check if there is an inflight shard that is overlapped with the queued relocateShard (rd)
			bool overlappingInFlight = false;
			auto intersectingInFlight = inFlight.intersectingRanges(rd.keys);
			for (auto it = intersectingInFlight.begin(); it != intersectingInFlight.end(); ++it) {
				if (fetchKeysComplete.count(it->value()) && inFlightActors.liveActorAt(it->range().begin) &&
				    !rd.keys.contains(it->range()) && it->value().priority >= rd.priority &&
				    rd.healthPriority < SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY) {
					/*TraceEvent("OverlappingInFlight", distributorId)
					    .detail("KeyBegin", it->value().keys.begin)
					    .detail("KeyEnd", it->value().keys.end)
					    .detail("Priority", it->value().priority);*/
					overlappingInFlight = true;
					break;
				}
			}

			if (overlappingInFlight) {
				ASSERT(!rd.isRestore());
				// logRelocation( rd, "SkippingOverlappingInFlight" );
				continue;
			}

			// Because the busyness of a server is decreased when a superseding relocation is issued, we
			//  need to consider what the busyness of a server WOULD be if
			auto containedRanges = inFlight.containedRanges(rd.keys);
			std::vector<RelocateData> cancellableRelocations;
			for (auto it = containedRanges.begin(); it != containedRanges.end(); ++it) {
				if (it.value().cancellable) {
					cancellableRelocations.push_back(it->value());
				}
			}

			// Data movement avoids overloading source servers in moving data.
			// SOMEDAY: the list of source servers may be outdated since they were fetched when the work was put in the
			// queue
			// FIXME: we need spare capacity even when we're just going to be cancelling work via TEAM_HEALTHY
			if (!rd.isRestore() && !canLaunchSrc(rd, teamSize, singleRegionTeamSize, busymap, cancellableRelocations)) {
				// logRelocation( rd, "SkippingQueuedRelocation" );
				continue;
			}

			// From now on, the source servers for the RelocateData rd have enough resource to move the data away,
			// because they do not have too much inflight data movement.

			// logRelocation( rd, "LaunchingRelocation" );

			//TraceEvent(rd.interval.end(), distributorId).detail("Result","Success");
			if (!rd.isRestore()) {
				queuedRelocations--;
				TraceEvent(SevVerbose, "QueuedRelocationsChanged")
				    .detail("DataMoveID", rd.dataMoveId)
				    .detail("RandomID", rd.randomId)
				    .detail("Total", queuedRelocations);
				finishRelocation(rd.priority, rd.healthPriority);

				// now we are launching: remove this entry from the queue of all the src servers
				for (int i = 0; i < rd.src.size(); i++) {
					ASSERT(queue[rd.src[i]].erase(rd));
				}
			}

			Future<Void> fCleanup =
			    CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA ? cancelDataMove(this, rd.keys, ddEnabledState) : Void();

			// If there is a job in flight that wants data relocation which we are about to cancel/modify,
			//     make sure that we keep the relocation intent for the job that we launch
			auto f = inFlight.intersectingRanges(rd.keys);
			for (auto it = f.begin(); it != f.end(); ++it) {
				if (inFlightActors.liveActorAt(it->range().begin)) {
					rd.wantsNewServers |= it->value().wantsNewServers;
				}
			}
			startedHere++;

			// update both inFlightActors and inFlight key range maps, cancelling deleted RelocateShards
			std::vector<KeyRange> ranges;
			inFlightActors.getRangesAffectedByInsertion(rd.keys, ranges);
			inFlightActors.cancel(KeyRangeRef(ranges.front().begin, ranges.back().end));
			inFlight.insert(rd.keys, rd);
			for (int r = 0; r < ranges.size(); r++) {
				RelocateData& rrs = inFlight.rangeContaining(ranges[r].begin)->value();
				rrs.keys = ranges[r];
				if (rd.keys == ranges[r] && rd.isRestore()) {
					ASSERT(rd.dataMove != nullptr);
					ASSERT(CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA);
					rrs.dataMoveId = rd.dataMove->meta.id;
				} else {
					ASSERT_WE_THINK(!rd.isRestore()); // Restored data move should not overlap.
					// TODO(psm): The shard id is determined by DD.
					rrs.dataMove.reset();
					if (CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA) {
						rrs.dataMoveId = deterministicRandom()->randomUniqueID();
					} else {
						rrs.dataMoveId = anonymousShardId;
					}
				}

				launch(rrs, busymap, singleRegionTeamSize);
				activeRelocations++;
				TraceEvent(SevVerbose, "InFlightRelocationChange")
				    .detail("Launch", rrs.dataMoveId)
				    .detail("Total", activeRelocations);
				startRelocation(rrs.priority, rrs.healthPriority);
				// Start the actor that relocates data in the rrs.keys
				inFlightActors.insert(rrs.keys, dataDistributionRelocator(this, rrs, fCleanup, ddEnabledState));
			}

			// logRelocation( rd, "LaunchedRelocation" );
		}
		if (now() - startTime > .001 && deterministicRandom()->random01() < 0.001)
			TraceEvent(SevWarnAlways, "LaunchingQueueSlowx1000").detail("Elapsed", now() - startTime);

		/*if( startedHere > 0 ) {
		    TraceEvent("StartedDDRelocators", distributorId)
		        .detail("QueueSize", queuedRelocations)
		        .detail("StartedHere", startedHere)
		        .detail("ActiveRelocations", activeRelocations);
		} */

		validate();
	}

	int getHighestPriorityRelocation() const {
		int highestPriority{ 0 };
		for (const auto& [priority, count] : priority_relocations) {
			if (count > 0) {
				highestPriority = std::max(highestPriority, priority);
			}
		}
		return highestPriority;
	}

	// return true if the servers are throttled as source for read rebalance
	bool timeThrottle(const std::vector<UID>& ids) const {
		return std::any_of(ids.begin(), ids.end(), [this](const UID& id) {
			if (this->lastAsSource.count(id)) {
				return (now() - this->lastAsSource.at(id)) * SERVER_KNOBS->READ_REBALANCE_SRC_PARALLELISM <
				       SERVER_KNOBS->STORAGE_METRICS_AVERAGE_INTERVAL;
			}
			return false;
		});
	}

	void updateLastAsSource(const std::vector<UID>& ids, double t = now()) {
		for (auto& id : ids)
			lastAsSource[id] = t;
	}

	// Schedules cancellation of a data move.
	void enqueueCancelledDataMove(UID dataMoveId, KeyRange range, const DDEnabledState* ddEnabledState) {
		std::vector<Future<Void>> cleanup;
		auto f = this->dataMoves.intersectingRanges(range);
		for (auto it = f.begin(); it != f.end(); ++it) {
			if (it->value().isValid()) {
				TraceEvent(SevError, "DDEnqueueCancelledDataMoveConflict", this->distributorId)
				    .detail("DataMoveID", dataMoveId)
				    .detail("CancelledRange", range)
				    .detail("ConflictingDataMoveID", it->value().id)
				    .detail("ConflictingRange", KeyRangeRef(it->range().begin, it->range().end));
				return;
			}
		}

		DDQueueData::DDDataMove dataMove(dataMoveId);
		dataMove.cancel = cleanUpDataMove(
		    this->cx, dataMoveId, this->lock, &this->cleanUpDataMoveParallelismLock, range, ddEnabledState);
		this->dataMoves.insert(range, dataMove);
		TraceEvent(SevInfo, "DDEnqueuedCancelledDataMove", this->distributorId)
		    .detail("DataMoveID", dataMoveId)
		    .detail("Range", range);
	}
};

ACTOR Future<Void> cancelDataMove(struct DDQueueData* self, KeyRange range, const DDEnabledState* ddEnabledState) {
	std::vector<Future<Void>> cleanup;
	auto f = self->dataMoves.intersectingRanges(range);
	for (auto it = f.begin(); it != f.end(); ++it) {
		if (!it->value().isValid()) {
			continue;
		}
		KeyRange keys = KeyRangeRef(it->range().begin, it->range().end);
		TraceEvent(SevInfo, "DDQueueCancelDataMove", self->distributorId)
		    .detail("DataMoveID", it->value().id)
		    .detail("DataMoveRange", keys)
		    .detail("Range", range);
		if (!it->value().cancel.isValid()) {
			it->value().cancel = cleanUpDataMove(
			    self->cx, it->value().id, self->lock, &self->cleanUpDataMoveParallelismLock, keys, ddEnabledState);
		}
		cleanup.push_back(it->value().cancel);
	}
	wait(waitForAll(cleanup));
	auto ranges = self->dataMoves.getAffectedRangesAfterInsertion(range);
	if (!ranges.empty()) {
		self->dataMoves.insert(KeyRangeRef(ranges.front().begin, ranges.back().end), DDQueueData::DDDataMove());
	}
	return Void();
}

static std::string destServersString(std::vector<std::pair<Reference<IDataDistributionTeam>, bool>> const& bestTeams) {
	std::stringstream ss;

	for (auto& tc : bestTeams) {
		for (const auto& id : tc.first->getServerIDs()) {
			ss << id.toString() << " ";
		}
	}

	return std::move(ss).str();
}

// This actor relocates the specified keys to a good place.
// The inFlightActor key range map stores the actor for each RelocateData
ACTOR Future<Void> dataDistributionRelocator(DDQueueData* self,
                                             RelocateData rd,
                                             Future<Void> prevCleanup,
                                             const DDEnabledState* ddEnabledState) {
	state Promise<Void> errorOut(self->error);
	state TraceInterval relocateShardInterval("RelocateShard");
	state PromiseStream<RelocateData> dataTransferComplete(self->dataTransferComplete);
	state PromiseStream<RelocateData> relocationComplete(self->relocationComplete);
	state bool signalledTransferComplete = false;
	state UID distributorId = self->distributorId;
	state ParallelTCInfo healthyDestinations;

	state bool anyHealthy = false;
	state bool allHealthy = true;
	state bool anyWithSource = false;
	state bool anyDestOverloaded = false;
	state int destOverloadedCount = 0;
	state int stuckCount = 0;
	state std::vector<std::pair<Reference<IDataDistributionTeam>, bool>> bestTeams;
	state double startTime = now();
	state std::vector<UID> destIds;

	try {
		if (now() - self->lastInterval < 1.0) {
			relocateShardInterval.severity = SevDebug;
			self->suppressIntervals++;
		}

		TraceEvent(relocateShardInterval.begin(), distributorId)
		    .detail("KeyBegin", rd.keys.begin)
		    .detail("KeyEnd", rd.keys.end)
		    .detail("Priority", rd.priority)
		    .detail("RelocationID", relocateShardInterval.pairID)
		    .detail("SuppressedEventCount", self->suppressIntervals);

		if (relocateShardInterval.severity != SevDebug) {
			self->lastInterval = now();
			self->suppressIntervals = 0;
		}

		if (CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA) {
			auto inFlightRange = self->inFlight.rangeContaining(rd.keys.begin);
			ASSERT(inFlightRange.range() == rd.keys);
			ASSERT(inFlightRange.value().randomId == rd.randomId);
			ASSERT(inFlightRange.value().dataMoveId == rd.dataMoveId);
			inFlightRange.value().cancellable = false;

			wait(prevCleanup);

			auto f = self->dataMoves.intersectingRanges(rd.keys);
			for (auto it = f.begin(); it != f.end(); ++it) {
				KeyRangeRef kr(it->range().begin, it->range().end);
				const UID mId = it->value().id;
				if (mId.isValid() && mId != rd.dataMoveId) {
					TraceEvent("DDRelocatorConflictingDataMove", distributorId)
					    .detail("CurrentDataMoveID", rd.dataMoveId)
					    .detail("DataMoveID", mId)
					    .detail("Range", kr);
				}
			}
			self->dataMoves.insert(rd.keys, DDQueueData::DDDataMove(rd.dataMoveId));
		}

		state StorageMetrics metrics =
		    wait(brokenPromiseToNever(self->getShardMetrics.getReply(GetMetricsRequest(rd.keys))));

		ASSERT(rd.src.size());
		loop {
			destOverloadedCount = 0;
			stuckCount = 0;
			// state int bestTeamStuckThreshold = 50;
			loop {
				state int tciIndex = 0;
				state bool foundTeams = true;
				state bool bestTeamReady = false;
				anyHealthy = false;
				allHealthy = true;
				anyWithSource = false;
				anyDestOverloaded = false;
				bestTeams.clear();
				// Get team from teamCollections in different DCs and find the best one
				while (tciIndex < self->teamCollections.size()) {
					if (CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA && rd.isRestore()) {
						auto req = GetTeamRequest(tciIndex == 0 ? rd.dataMove->primaryDest : rd.dataMove->remoteDest);
						Future<std::pair<Optional<Reference<IDataDistributionTeam>>, bool>> fbestTeam =
						    brokenPromiseToNever(self->teamCollections[tciIndex].getTeam.getReply(req));
						bestTeamReady = fbestTeam.isReady();
						std::pair<Optional<Reference<IDataDistributionTeam>>, bool> bestTeam = wait(fbestTeam);
						if (tciIndex > 0 && !bestTeamReady) {
							// self->shardsAffectedByTeamFailure->moveShard must be called without any waits after
							// getting the destination team or we could miss failure notifications for the storage
							// servers in the destination team
							TraceEvent("BestTeamNotReady")
							    .detail("TeamCollectionIndex", tciIndex)
							    .detail("RestoreDataMoveForDest",
							            describe(tciIndex == 0 ? rd.dataMove->primaryDest : rd.dataMove->remoteDest));
							foundTeams = false;
							break;
						}
						if (!bestTeam.first.present() || !bestTeam.first.get()->isHealthy()) {
							foundTeams = false;
							break;
						}
						anyHealthy = true;
						bestTeams.emplace_back(bestTeam.first.get(), bestTeam.second);
					} else {
						double inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_HEALTHY;
						if (rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY ||
						    rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_2_LEFT)
							inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_UNHEALTHY;
						if (rd.healthPriority == SERVER_KNOBS->PRIORITY_POPULATE_REGION ||
						    rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_1_LEFT ||
						    rd.healthPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT)
							inflightPenalty = SERVER_KNOBS->INFLIGHT_PENALTY_ONE_LEFT;

						auto req = GetTeamRequest(WantNewServers(rd.wantsNewServers),
						                          WantTrueBest(isValleyFillerPriority(rd.priority)),
						                          PreferLowerDiskUtil::True,
						                          TeamMustHaveShards::False,
						                          ForReadBalance(rd.reason == RelocateReason::REBALANCE_READ),
						                          PreferLowerReadUtil::True,
						                          inflightPenalty);

						req.src = rd.src;
						req.completeSources = rd.completeSources;

						// bestTeam.second = false if the bestTeam in the teamCollection (in the DC) does not have any
						// server that hosts the relocateData. This is possible, for example, in a fearless
						// configuration when the remote DC is just brought up.
						Future<std::pair<Optional<Reference<IDataDistributionTeam>>, bool>> fbestTeam =
						    brokenPromiseToNever(self->teamCollections[tciIndex].getTeam.getReply(req));
						bestTeamReady = fbestTeam.isReady();
						std::pair<Optional<Reference<IDataDistributionTeam>>, bool> bestTeam = wait(fbestTeam);
						if (tciIndex > 0 && !bestTeamReady) {
							// self->shardsAffectedByTeamFailure->moveShard must be called without any waits after
							// getting the destination team or we could miss failure notifications for the storage
							// servers in the destination team
							TraceEvent("BestTeamNotReady");
							foundTeams = false;
							break;
						}
						// If a DC has no healthy team, we stop checking the other DCs until
						// the unhealthy DC is healthy again or is excluded.
						if (!bestTeam.first.present()) {
							foundTeams = false;
							break;
						}
						if (!bestTeam.first.get()->isHealthy()) {
							allHealthy = false;
						} else {
							anyHealthy = true;
						}

						if (bestTeam.second) {
							anyWithSource = true;
						}

						bestTeams.emplace_back(bestTeam.first.get(), bestTeam.second);
					}
					tciIndex++;
				}
				// once we've found healthy candidate teams, make sure they're not overloaded with outstanding moves
				// already
				anyDestOverloaded = !canLaunchDest(bestTeams, rd.priority, self->destBusymap);

				if (foundTeams && anyHealthy && !anyDestOverloaded) {
					ASSERT(rd.completeDests.empty());
					break;
				}

				if (anyDestOverloaded) {
					CODE_PROBE(true, "Destination overloaded throttled move");
					destOverloadedCount++;
					TraceEvent(destOverloadedCount > 50 ? SevInfo : SevDebug, "DestSSBusy", distributorId)
					    .suppressFor(1.0)
					    .detail("StuckCount", stuckCount)
					    .detail("DestOverloadedCount", destOverloadedCount)
					    .detail("TeamCollectionId", tciIndex)
					    .detail("AnyDestOverloaded", anyDestOverloaded)
					    .detail("NumOfTeamCollections", self->teamCollections.size())
					    .detail("Servers", destServersString(bestTeams));
					wait(delay(SERVER_KNOBS->DEST_OVERLOADED_DELAY, TaskPriority::DataDistributionLaunch));
				} else {
					CODE_PROBE(true, "did not find a healthy destination team on the first attempt");
					stuckCount++;
					TraceEvent(stuckCount > 50 ? SevWarnAlways : SevWarn, "BestTeamStuck", distributorId)
					    .suppressFor(1.0)
					    .detail("StuckCount", stuckCount)
					    .detail("DestOverloadedCount", destOverloadedCount)
					    .detail("TeamCollectionId", tciIndex)
					    .detail("AnyDestOverloaded", anyDestOverloaded)
					    .detail("NumOfTeamCollections", self->teamCollections.size());
					if (rd.isRestore() && stuckCount > 50) {
						throw data_move_dest_team_not_found();
					}
					wait(delay(SERVER_KNOBS->BEST_TEAM_STUCK_DELAY, TaskPriority::DataDistributionLaunch));
				}

				// TODO different trace event + knob for overloaded? Could wait on an async var for done moves
			}

			// set cancellable to false on inFlight's entry for this key range
			auto inFlightRange = self->inFlight.rangeContaining(rd.keys.begin);
			ASSERT(inFlightRange.range() == rd.keys);
			ASSERT(inFlightRange.value().randomId == rd.randomId);
			inFlightRange.value().cancellable = false;

			destIds.clear();
			state std::vector<UID> healthyIds;
			state std::vector<UID> extraIds;
			state std::vector<ShardsAffectedByTeamFailure::Team> destinationTeams;

			for (int i = 0; i < bestTeams.size(); i++) {
				auto& serverIds = bestTeams[i].first->getServerIDs();
				destinationTeams.push_back(ShardsAffectedByTeamFailure::Team(serverIds, i == 0));

				// TODO(psm): Make DataMoveMetaData aware of the two-step data move optimization.
				if (allHealthy && anyWithSource && !bestTeams[i].second) {
					// When all servers in bestTeams[i] do not hold the shard (!bestTeams[i].second), it indicates
					// the bestTeams[i] is in a new DC where data has not been replicated to.
					// To move data (specified in RelocateShard) to bestTeams[i] in the new DC AND reduce data movement
					// across DC, we randomly choose a server in bestTeams[i] as the shard's destination, and
					// move the shard to the randomly chosen server (in the remote DC), which will later
					// propogate its data to the servers in the same team. This saves data movement bandwidth across DC
					int idx = deterministicRandom()->randomInt(0, serverIds.size());
					destIds.push_back(serverIds[idx]);
					healthyIds.push_back(serverIds[idx]);
					for (int j = 0; j < serverIds.size(); j++) {
						if (j != idx) {
							extraIds.push_back(serverIds[j]);
						}
					}
					healthyDestinations.addTeam(bestTeams[i].first);
				} else {
					destIds.insert(destIds.end(), serverIds.begin(), serverIds.end());
					if (bestTeams[i].first->isHealthy()) {
						healthyIds.insert(healthyIds.end(), serverIds.begin(), serverIds.end());
						healthyDestinations.addTeam(bestTeams[i].first);
					}
				}
			}

			// Sanity check
			state int totalIds = 0;
			for (auto& destTeam : destinationTeams) {
				totalIds += destTeam.servers.size();
			}
			if (totalIds != self->teamSize) {
				TraceEvent(SevWarn, "IncorrectDestTeamSize")
				    .suppressFor(1.0)
				    .detail("ExpectedTeamSize", self->teamSize)
				    .detail("DestTeamSize", totalIds);
			}

			if (!rd.isRestore()) {
				self->shardsAffectedByTeamFailure->moveShard(rd.keys, destinationTeams);
			}

			// FIXME: do not add data in flight to servers that were already in the src.
			healthyDestinations.addDataInFlightToTeam(+metrics.bytes);
			healthyDestinations.addReadInFlightToTeam(+metrics.bytesReadPerKSecond);

			launchDest(rd, bestTeams, self->destBusymap);

			if (SERVER_KNOBS->DD_ENABLE_VERBOSE_TRACING) {
				// StorageMetrics is the rd shard's metrics, e.g., bytes and write bandwidth
				TraceEvent(SevInfo, "RelocateShardDecision", distributorId)
				    .detail("PairId", relocateShardInterval.pairID)
				    .detail("Priority", rd.priority)
				    .detail("KeyBegin", rd.keys.begin)
				    .detail("KeyEnd", rd.keys.end)
				    .detail("StorageMetrics", metrics.toString())
				    .detail("SourceServers", describe(rd.src))
				    .detail("DestinationTeam", describe(destIds))
				    .detail("ExtraIds", describe(extraIds));
			} else {
				TraceEvent(relocateShardInterval.severity, "RelocateShardHasDestination", distributorId)
				    .detail("PairId", relocateShardInterval.pairID)
				    .detail("Priority", rd.priority)
				    .detail("KeyBegin", rd.keys.begin)
				    .detail("KeyEnd", rd.keys.end)
				    .detail("SourceServers", describe(rd.src))
				    .detail("DestinationTeam", describe(destIds))
				    .detail("ExtraIds", describe(extraIds));
			}

			state Error error = success();
			state Promise<Void> dataMovementComplete;
			// Move keys from source to destination by changing the serverKeyList and keyServerList system keys
			state Future<Void> doMoveKeys = moveKeys(self->cx,
			                                         rd.dataMoveId,
			                                         rd.keys,
			                                         destIds,
			                                         healthyIds,
			                                         self->lock,
			                                         dataMovementComplete,
			                                         &self->startMoveKeysParallelismLock,
			                                         &self->finishMoveKeysParallelismLock,
			                                         self->teamCollections.size() > 1,
			                                         relocateShardInterval.pairID,
			                                         ddEnabledState,
			                                         CancelConflictingDataMoves::False);
			state Future<Void> pollHealth =
			    signalledTransferComplete ? Never()
			                              : delay(SERVER_KNOBS->HEALTH_POLL_TIME, TaskPriority::DataDistributionLaunch);
			try {
				loop {
					choose {
						when(wait(doMoveKeys)) {
							if (extraIds.size()) {
								destIds.insert(destIds.end(), extraIds.begin(), extraIds.end());
								healthyIds.insert(healthyIds.end(), extraIds.begin(), extraIds.end());
								extraIds.clear();
								ASSERT(totalIds == destIds.size()); // Sanity check the destIDs before we move keys
								doMoveKeys = moveKeys(self->cx,
								                      rd.dataMoveId,
								                      rd.keys,
								                      destIds,
								                      healthyIds,
								                      self->lock,
								                      Promise<Void>(),
								                      &self->startMoveKeysParallelismLock,
								                      &self->finishMoveKeysParallelismLock,
								                      self->teamCollections.size() > 1,
								                      relocateShardInterval.pairID,
								                      ddEnabledState,
								                      CancelConflictingDataMoves::False);
							} else {
								self->fetchKeysComplete.insert(rd);
								if (CLIENT_KNOBS->SHARD_ENCODE_LOCATION_METADATA) {
									auto ranges = self->dataMoves.getAffectedRangesAfterInsertion(rd.keys);
									if (ranges.size() == 1 && static_cast<KeyRange>(ranges[0]) == rd.keys &&
									    ranges[0].value.id == rd.dataMoveId && !ranges[0].value.cancel.isValid()) {
										self->dataMoves.insert(rd.keys, DDQueueData::DDDataMove());
										TraceEvent(SevVerbose, "DequeueDataMoveOnSuccess", self->distributorId)
										    .detail("DataMoveID", rd.dataMoveId)
										    .detail("DataMoveRange", rd.keys);
									}
								}
								break;
							}
						}
						when(wait(pollHealth)) {
							if (!healthyDestinations.isHealthy()) {
								if (!signalledTransferComplete) {
									signalledTransferComplete = true;
									self->dataTransferComplete.send(rd);
								}
							}
							pollHealth = signalledTransferComplete ? Never()
							                                       : delay(SERVER_KNOBS->HEALTH_POLL_TIME,
							                                               TaskPriority::DataDistributionLaunch);
						}
						when(wait(signalledTransferComplete ? Never() : dataMovementComplete.getFuture())) {
							self->fetchKeysComplete.insert(rd);
							if (!signalledTransferComplete) {
								signalledTransferComplete = true;
								self->dataTransferComplete.send(rd);
							}
						}
					}
				}
			} catch (Error& e) {
				error = e;
			}

			//TraceEvent("RelocateShardFinished", distributorId).detail("RelocateId", relocateShardInterval.pairID);

			if (error.code() != error_code_move_to_removed_server) {
				if (!error.code()) {
					try {
						wait(healthyDestinations
						         .updateStorageMetrics()); // prevent a gap between the polling for an increase in
						                                   // storage metrics and decrementing data in flight
					} catch (Error& e) {
						error = e;
					}
				}

				healthyDestinations.addDataInFlightToTeam(-metrics.bytes);
				auto readLoad = metrics.bytesReadPerKSecond;
				// Note: It’s equal to trigger([healthyDestinations, readLoad], which is a value capture of
				// healthyDestinations. Have to create a reference to healthyDestinations because in ACTOR the state
				// variable is actually a member variable, I can’t write trigger([healthyDestinations, readLoad]
				// directly.
				auto& destinationRef = healthyDestinations;
				self->noErrorActors.add(
				    trigger([destinationRef, readLoad]() mutable { destinationRef.addReadInFlightToTeam(-readLoad); },
				            delay(SERVER_KNOBS->STORAGE_METRICS_AVERAGE_INTERVAL)));

				// onFinished.send( rs );
				if (!error.code()) {
					TraceEvent(relocateShardInterval.end(), distributorId)
					    .detail("Duration", now() - startTime)
					    .detail("Result", "Success");
					if (now() - startTime > 600) {
						TraceEvent(SevWarnAlways, "RelocateShardTooLong")
						    .detail("Duration", now() - startTime)
						    .detail("Dest", describe(destIds))
						    .detail("Src", describe(rd.src));
					}
					if (rd.keys.begin == keyServersPrefix) {
						TraceEvent("MovedKeyServerKeys")
						    .detail("Dest", describe(destIds))
						    .trackLatest(self->movedKeyServersEventHolder->trackingKey);
					}

					if (!signalledTransferComplete) {
						signalledTransferComplete = true;
						dataTransferComplete.send(rd);
					}

					self->bytesWritten += metrics.bytes;
					self->shardsAffectedByTeamFailure->finishMove(rd.keys);
					relocationComplete.send(rd);
					return Void();
				} else {
					throw error;
				}
			} else {
				CODE_PROBE(true, "move to removed server");
				healthyDestinations.addDataInFlightToTeam(-metrics.bytes);
				auto readLoad = metrics.bytesReadPerKSecond;
				auto& destinationRef = healthyDestinations;
				self->noErrorActors.add(
				    trigger([destinationRef, readLoad]() mutable { destinationRef.addReadInFlightToTeam(-readLoad); },
				            delay(SERVER_KNOBS->STORAGE_METRICS_AVERAGE_INTERVAL)));

				completeDest(rd, self->destBusymap);
				rd.completeDests.clear();

				wait(delay(SERVER_KNOBS->RETRY_RELOCATESHARD_DELAY, TaskPriority::DataDistributionLaunch));
			}
		}
	} catch (Error& e) {
		state Error err = e;
		TraceEvent(relocateShardInterval.end(), distributorId)
		    .errorUnsuppressed(err)
		    .detail("Duration", now() - startTime);
		if (now() - startTime > 600) {
			TraceEvent(SevWarnAlways, "RelocateShardTooLong")
			    .errorUnsuppressed(err)
			    .detail("Duration", now() - startTime)
			    .detail("Dest", describe(destIds))
			    .detail("Src", describe(rd.src));
		}
		if (!signalledTransferComplete)
			dataTransferComplete.send(rd);

		relocationComplete.send(rd);

		if (err.code() == error_code_data_move_dest_team_not_found) {
			wait(cancelDataMove(self, rd.keys, ddEnabledState));
		}

		if (err.code() != error_code_actor_cancelled && err.code() != error_code_data_move_cancelled) {
			if (errorOut.canBeSet()) {
				errorOut.sendError(err);
			}
		}
		throw err;
	}
}

inline double getWorstCpu(const HealthMetrics& metrics, const std::vector<UID>& ids) {
	double cpu = 0;
	for (auto& id : ids) {
		if (metrics.storageStats.count(id)) {
			cpu = std::max(cpu, metrics.storageStats.at(id).cpuUsage);
		} else {
			// assume the server is too busy to report its stats
			cpu = std::max(cpu, 100.0);
			break;
		}
	}
	return cpu;
}

// Move the shard with the top K highest read density of sourceTeam's to destTeam if sourceTeam has much more read load
// than destTeam
ACTOR Future<bool> rebalanceReadLoad(DDQueueData* self,
                                     DataMovementReason moveReason,
                                     Reference<IDataDistributionTeam> sourceTeam,
                                     Reference<IDataDistributionTeam> destTeam,
                                     bool primary,
                                     TraceEvent* traceEvent) {
	if (g_network->isSimulated() && g_simulator.speedUpSimulation) {
		traceEvent->detail("CancelingDueToSimulationSpeedup", true);
		return false;
	}

	state std::vector<KeyRange> shards = self->shardsAffectedByTeamFailure->getShardsFor(
	    ShardsAffectedByTeamFailure::Team(sourceTeam->getServerIDs(), primary));
	traceEvent->detail("ShardsInSource", shards.size());
	// For read rebalance if there is just 1 hot shard remained, move this shard to another server won't solve the
	// problem.
	// TODO: This situation should be solved by split and merge
	if (shards.size() <= 1) {
		traceEvent->detail("SkipReason", "NoShardOnSource");
		return false;
	}

	// Check lastAsSource, at most SERVER_KNOBS->READ_REBALANCE_SRC_PARALLELISM shards can be moved within a sample
	// period. It takes time for the sampled metrics being updated after a shard is moved, so we should control the
	// cadence of movement here to avoid moving churn caused by making many decision based on out-of-date sampled
	// metrics.
	if (self->timeThrottle(sourceTeam->getServerIDs())) {
		traceEvent->detail("SkipReason", "SourceTeamThrottle");
		return false;
	}
	// check team difference
	auto srcLoad = sourceTeam->getLoadReadBandwidth(false), destLoad = destTeam->getLoadReadBandwidth();
	traceEvent->detail("SrcReadBandwidth", srcLoad).detail("DestReadBandwidth", destLoad);

	// read bandwidth difference is less than 30% of src load
	if ((1.0 - SERVER_KNOBS->READ_REBALANCE_DIFF_FRAC) * srcLoad <= destLoad) {
		traceEvent->detail("SkipReason", "TeamTooSimilar");
		return false;
	}
	// randomly choose topK shards
	int topK = std::min(int(0.1 * shards.size()), SERVER_KNOBS->READ_REBALANCE_SHARD_TOPK);
	state Future<HealthMetrics> healthMetrics = self->cx->getHealthMetrics(true);
	state GetTopKMetricsRequest req(
	    shards, topK, (srcLoad - destLoad) * SERVER_KNOBS->READ_REBALANCE_MAX_SHARD_FRAC, srcLoad / shards.size());
	state GetTopKMetricsReply reply = wait(brokenPromiseToNever(self->getTopKMetrics.getReply(req)));
	wait(ready(healthMetrics));
	auto cpu = getWorstCpu(healthMetrics.get(), sourceTeam->getServerIDs());
	if (cpu < SERVER_KNOBS->READ_REBALANCE_CPU_THRESHOLD) { // 15.0 +- (0.3 * 15) < 20.0
		traceEvent->detail("SkipReason", "LowReadLoad").detail("WorstSrcCpu", cpu);
		return false;
	}

	auto& metricsList = reply.shardMetrics;
	// NOTE: randomize is important here since we don't want to always push the same shard into the queue
	deterministicRandom()->randomShuffle(metricsList);
	traceEvent->detail("MinReadLoad", reply.minReadLoad).detail("MaxReadLoad", reply.maxReadLoad);

	if (metricsList.empty()) {
		traceEvent->detail("SkipReason", "NoEligibleShards");
		return false;
	}

	auto& [shard, metrics] = metricsList[0];
	traceEvent->detail("ShardReadBandwidth", metrics.bytesReadPerKSecond);
	//  Verify the shard is still in ShardsAffectedByTeamFailure
	shards = self->shardsAffectedByTeamFailure->getShardsFor(
	    ShardsAffectedByTeamFailure::Team(sourceTeam->getServerIDs(), primary));
	for (int i = 0; i < shards.size(); i++) {
		if (shard == shards[i]) {
			self->output.send(RelocateShard(shard, moveReason, RelocateReason::REBALANCE_READ));
			self->updateLastAsSource(sourceTeam->getServerIDs());
			return true;
		}
	}
	traceEvent->detail("SkipReason", "ShardNotPresent");
	return false;
}

// Move a random shard from sourceTeam if sourceTeam has much more data than provided destTeam
ACTOR static Future<bool> rebalanceTeams(DDQueueData* self,
                                         DataMovementReason moveReason,
                                         Reference<IDataDistributionTeam const> sourceTeam,
                                         Reference<IDataDistributionTeam const> destTeam,
                                         bool primary,
                                         TraceEvent* traceEvent) {
	if (g_network->isSimulated() && g_simulator.speedUpSimulation) {
		traceEvent->detail("CancelingDueToSimulationSpeedup", true);
		return false;
	}

	Promise<int64_t> req;
	self->getAverageShardBytes.send(req);

	state int64_t averageShardBytes = wait(req.getFuture());
	state std::vector<KeyRange> shards = self->shardsAffectedByTeamFailure->getShardsFor(
	    ShardsAffectedByTeamFailure::Team(sourceTeam->getServerIDs(), primary));

	traceEvent->detail("AverageShardBytes", averageShardBytes).detail("ShardsInSource", shards.size());

	if (!shards.size()) {
		traceEvent->detail("SkipReason", "NoShardOnSource");
		return false;
	}

	state KeyRange moveShard;
	state StorageMetrics metrics;
	state int retries = 0;
	while (retries < SERVER_KNOBS->REBALANCE_MAX_RETRIES) {
		state KeyRange testShard = deterministicRandom()->randomChoice(shards);
		StorageMetrics testMetrics =
		    wait(brokenPromiseToNever(self->getShardMetrics.getReply(GetMetricsRequest(testShard))));
		if (testMetrics.bytes > metrics.bytes) {
			moveShard = testShard;
			metrics = testMetrics;
			if (metrics.bytes > averageShardBytes) {
				break;
			}
		}
		retries++;
	}

	int64_t sourceBytes = sourceTeam->getLoadBytes(false);
	int64_t destBytes = destTeam->getLoadBytes();

	bool sourceAndDestTooSimilar =
	    sourceBytes - destBytes <= 3 * std::max<int64_t>(SERVER_KNOBS->MIN_SHARD_BYTES, metrics.bytes);
	traceEvent->detail("SourceBytes", sourceBytes)
	    .detail("DestBytes", destBytes)
	    .detail("ShardBytes", metrics.bytes)
	    .detail("SourceAndDestTooSimilar", sourceAndDestTooSimilar);

	if (sourceAndDestTooSimilar || metrics.bytes == 0) {
		traceEvent->detail("SkipReason", sourceAndDestTooSimilar ? "TeamTooSimilar" : "ShardZeroSize");
		return false;
	}

	// Verify the shard is still in ShardsAffectedByTeamFailure
	shards = self->shardsAffectedByTeamFailure->getShardsFor(
	    ShardsAffectedByTeamFailure::Team(sourceTeam->getServerIDs(), primary));
	for (int i = 0; i < shards.size(); i++) {
		if (moveShard == shards[i]) {
			self->output.send(RelocateShard(moveShard, moveReason, RelocateReason::REBALANCE_DISK));
			return true;
		}
	}

	traceEvent->detail("SkipReason", "ShardNotPresent");
	return false;
}

ACTOR Future<SrcDestTeamPair> getSrcDestTeams(DDQueueData* self,
                                              int teamCollectionIndex,
                                              GetTeamRequest srcReq,
                                              GetTeamRequest destReq,
                                              int priority,
                                              TraceEvent* traceEvent) {

	state std::pair<Optional<ITeamRef>, bool> randomTeam =
	    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(destReq)));
	traceEvent->detail(
	    "DestTeam", printable(randomTeam.first.map<std::string>([](const ITeamRef& team) { return team->getDesc(); })));

	if (randomTeam.first.present()) {
		state std::pair<Optional<ITeamRef>, bool> loadedTeam =
		    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(srcReq)));

		traceEvent->detail("SourceTeam", printable(loadedTeam.first.map<std::string>([](const ITeamRef& team) {
			                   return team->getDesc();
		                   })));

		if (loadedTeam.first.present()) {
			return std::make_pair(loadedTeam.first.get(), randomTeam.first.get());
		}
	}
	return {};
}

ACTOR Future<Void> BgDDLoadRebalance(DDQueueData* self, int teamCollectionIndex, DataMovementReason reason) {
	state int resetCount = SERVER_KNOBS->DD_REBALANCE_RESET_AMOUNT;
	state Transaction tr(self->cx);
	state double lastRead = 0;
	state bool skipCurrentLoop = false;
	state Future<Void> delayF = Never();
	state const bool readRebalance = isDataMovementForReadBalancing(reason);
	state const char* eventName =
	    isDataMovementForMountainChopper(reason) ? "BgDDMountainChopper_New" : "BgDDValleyFiller_New";
	state int ddPriority = dataMovementPriority(reason);
	loop {
		state bool moved = false;
		state Reference<IDataDistributionTeam> sourceTeam;
		state Reference<IDataDistributionTeam> destTeam;
		state GetTeamRequest srcReq;
		state GetTeamRequest destReq;
		state TraceEvent traceEvent(eventName, self->distributorId);
		traceEvent.suppressFor(5.0)
		    .detail("PollingInterval", SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL)
		    .detail("Rebalance", readRebalance ? "Read" : "Disk");

		try {
			// NOTE: the DD throttling relies on DDQueue
			delayF = delay(SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL, TaskPriority::DataDistributionLaunch);
			if ((now() - lastRead) > SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				Optional<Value> val = wait(tr.get(rebalanceDDIgnoreKey));
				lastRead = now();
				if (!val.present()) {
					skipCurrentLoop = false;
				} else {
					// NOTE: check special value "" and "on" might written in old version < 7.2
					if (val.get().size() > 0 && val.get() != "on"_sr) {
						int ddIgnore = BinaryReader::fromStringRef<uint8_t>(val.get(), Unversioned());
						if (readRebalance) {
							skipCurrentLoop = (ddIgnore & DDIgnore::REBALANCE_READ) > 0;
						} else {
							skipCurrentLoop = (ddIgnore & DDIgnore::REBALANCE_DISK) > 0;
						}
					} else {
						skipCurrentLoop = true;
					}
				}
			}

			traceEvent.detail("Enabled", !skipCurrentLoop);

			wait(delayF);
			if (skipCurrentLoop) {
				tr.reset();
				continue;
			}

			traceEvent.detail("QueuedRelocations", self->priority_relocations[ddPriority]);

			if (self->priority_relocations[ddPriority] < SERVER_KNOBS->DD_REBALANCE_PARALLELISM) {
				bool mcMove = isDataMovementForMountainChopper(reason);
				srcReq = GetTeamRequest(WantNewServers::True,
				                        WantTrueBest(mcMove),
				                        PreferLowerDiskUtil::False,
				                        TeamMustHaveShards::True,
				                        ForReadBalance(readRebalance),
				                        PreferLowerReadUtil::False);
				destReq = GetTeamRequest(WantNewServers::True,
				                         WantTrueBest(!mcMove),
				                         PreferLowerDiskUtil::True,
				                         TeamMustHaveShards::False,
				                         ForReadBalance(readRebalance),
				                         PreferLowerReadUtil::True);
				state Future<SrcDestTeamPair> getTeamFuture =
				    getSrcDestTeams(self, teamCollectionIndex, srcReq, destReq, ddPriority, &traceEvent);
				wait(ready(getTeamFuture));
				sourceTeam = getTeamFuture.get().first;
				destTeam = getTeamFuture.get().second;

				// clang-format off
				if (sourceTeam.isValid() && destTeam.isValid()) {
					if (readRebalance) {
						wait(store(moved,rebalanceReadLoad(self, reason, sourceTeam, destTeam, teamCollectionIndex == 0, &traceEvent)));
					} else {
						wait(store(moved,rebalanceTeams(self, reason, sourceTeam, destTeam, teamCollectionIndex == 0, &traceEvent)));
					}
				}
				// clang-format on
				moved ? resetCount = 0 : resetCount++;
			}

			traceEvent.detail("ResetCount", resetCount);
			tr.reset();
		} catch (Error& e) {
			// Log actor_cancelled because it's not legal to suppress an event that's initialized
			traceEvent.errorUnsuppressed(e);
			wait(tr.onError(e));
		}

		traceEvent.detail("Moved", moved);
		traceEvent.log();
	}
}

ACTOR Future<Void> BgDDMountainChopper(DDQueueData* self, int teamCollectionIndex) {
	state double rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
	state Transaction tr(self->cx);
	state double lastRead = 0;
	state bool skipCurrentLoop = false;
	loop {
		state std::pair<Optional<Reference<IDataDistributionTeam>>, bool> randomTeam;
		state bool moved = false;
		state TraceEvent traceEvent("BgDDMountainChopper_Old", self->distributorId);
		traceEvent.suppressFor(5.0).detail("PollingInterval", rebalancePollingInterval).detail("Rebalance", "Disk");

		try {
			state Future<Void> delayF = delay(rebalancePollingInterval, TaskPriority::DataDistributionLaunch);
			if ((now() - lastRead) > SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				Optional<Value> val = wait(tr.get(rebalanceDDIgnoreKey));
				lastRead = now();
				if (!val.present()) {
					// reset loop interval
					if (skipCurrentLoop) {
						rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
					}
					skipCurrentLoop = false;
				} else {
					// NOTE: check special value "" and "on" might written in old version < 7.2
					if (val.get().size() > 0 && val.get() != "on"_sr) {
						int ddIgnore = BinaryReader::fromStringRef<uint8_t>(val.get(), Unversioned());
						skipCurrentLoop = (ddIgnore & DDIgnore::REBALANCE_DISK) > 0;
					} else {
						skipCurrentLoop = true;
					}
				}
			}

			traceEvent.detail("Enabled", !skipCurrentLoop);

			wait(delayF);
			if (skipCurrentLoop) {
				// set loop interval to avoid busy wait here.
				rebalancePollingInterval =
				    std::max(rebalancePollingInterval, SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL);
				tr.reset();
				continue;
			}

			traceEvent.detail("QueuedRelocations",
			                  self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM]);
			if (self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM] <
			    SERVER_KNOBS->DD_REBALANCE_PARALLELISM) {
				std::pair<Optional<Reference<IDataDistributionTeam>>, bool> _randomTeam =
				    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(
				        GetTeamRequest(WantNewServers::True,
				                       WantTrueBest::False,
				                       PreferLowerDiskUtil::True,
				                       TeamMustHaveShards::False))));
				randomTeam = _randomTeam;
				traceEvent.detail("DestTeam",
				                  printable(randomTeam.first.map<std::string>(
				                      [](const Reference<IDataDistributionTeam>& team) { return team->getDesc(); })));

				if (randomTeam.first.present()) {
					std::pair<Optional<Reference<IDataDistributionTeam>>, bool> loadedTeam =
					    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(
					        GetTeamRequest(WantNewServers::True,
					                       WantTrueBest::True,
					                       PreferLowerDiskUtil::False,
					                       TeamMustHaveShards::True))));

					traceEvent.detail(
					    "SourceTeam",
					    printable(loadedTeam.first.map<std::string>(
					        [](const Reference<IDataDistributionTeam>& team) { return team->getDesc(); })));

					if (loadedTeam.first.present()) {
						bool _moved = wait(rebalanceTeams(self,
						                                  DataMovementReason::REBALANCE_OVERUTILIZED_TEAM,
						                                  loadedTeam.first.get(),
						                                  randomTeam.first.get(),
						                                  teamCollectionIndex == 0,
						                                  &traceEvent));
						moved = _moved;
					}
				}
			}

			tr.reset();
		} catch (Error& e) {
			// Log actor_cancelled because it's not legal to suppress an event that's initialized
			traceEvent.errorUnsuppressed(e);
			wait(tr.onError(e));
		}

		traceEvent.detail("Moved", moved);
		traceEvent.log();
	}
}

ACTOR Future<Void> BgDDValleyFiller(DDQueueData* self, int teamCollectionIndex) {
	state double rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
	state Transaction tr(self->cx);
	state double lastRead = 0;
	state bool skipCurrentLoop = false;

	loop {
		state std::pair<Optional<Reference<IDataDistributionTeam>>, bool> randomTeam;
		state bool moved = false;
		state TraceEvent traceEvent("BgDDValleyFiller_Old", self->distributorId);
		traceEvent.suppressFor(5.0).detail("PollingInterval", rebalancePollingInterval).detail("Rebalance", "Disk");

		try {
			state Future<Void> delayF = delay(rebalancePollingInterval, TaskPriority::DataDistributionLaunch);
			if ((now() - lastRead) > SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				Optional<Value> val = wait(tr.get(rebalanceDDIgnoreKey));
				lastRead = now();
				if (!val.present()) {
					// reset loop interval
					if (skipCurrentLoop) {
						rebalancePollingInterval = SERVER_KNOBS->BG_REBALANCE_POLLING_INTERVAL;
					}
					skipCurrentLoop = false;
				} else {
					// NOTE: check special value "" and "on" might written in old version < 7.2
					if (val.get().size() > 0 && val.get() != "on"_sr) {
						int ddIgnore = BinaryReader::fromStringRef<uint8_t>(val.get(), Unversioned());
						skipCurrentLoop = (ddIgnore & DDIgnore::REBALANCE_DISK) > 0;
					} else {
						skipCurrentLoop = true;
					}
				}
			}

			traceEvent.detail("Enabled", !skipCurrentLoop);

			wait(delayF);
			if (skipCurrentLoop) {
				// set loop interval to avoid busy wait here.
				rebalancePollingInterval =
				    std::max(rebalancePollingInterval, SERVER_KNOBS->BG_REBALANCE_SWITCH_CHECK_INTERVAL);
				tr.reset();
				continue;
			}

			traceEvent.detail("QueuedRelocations",
			                  self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM]);
			if (self->priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM] <
			    SERVER_KNOBS->DD_REBALANCE_PARALLELISM) {
				std::pair<Optional<Reference<IDataDistributionTeam>>, bool> _randomTeam =
				    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(
				        GetTeamRequest(WantNewServers::True,
				                       WantTrueBest::False,
				                       PreferLowerDiskUtil::False,
				                       TeamMustHaveShards::True))));
				randomTeam = _randomTeam;
				traceEvent.detail("SourceTeam",
				                  printable(randomTeam.first.map<std::string>(
				                      [](const Reference<IDataDistributionTeam>& team) { return team->getDesc(); })));

				if (randomTeam.first.present()) {
					std::pair<Optional<Reference<IDataDistributionTeam>>, bool> unloadedTeam =
					    wait(brokenPromiseToNever(self->teamCollections[teamCollectionIndex].getTeam.getReply(
					        GetTeamRequest(WantNewServers::True,
					                       WantTrueBest::True,
					                       PreferLowerDiskUtil::True,
					                       TeamMustHaveShards::False))));

					traceEvent.detail(
					    "DestTeam",
					    printable(unloadedTeam.first.map<std::string>(
					        [](const Reference<IDataDistributionTeam>& team) { return team->getDesc(); })));

					if (unloadedTeam.first.present()) {
						bool _moved = wait(rebalanceTeams(self,
						                                  DataMovementReason::REBALANCE_UNDERUTILIZED_TEAM,
						                                  randomTeam.first.get(),
						                                  unloadedTeam.first.get(),
						                                  teamCollectionIndex == 0,
						                                  &traceEvent));
						moved = _moved;
					}
				}
			}

			tr.reset();
		} catch (Error& e) {
			// Log actor_cancelled because it's not legal to suppress an event that's initialized
			traceEvent.errorUnsuppressed(e);
			wait(tr.onError(e));
		}

		traceEvent.detail("Moved", moved);
		traceEvent.log();
	}
}

ACTOR Future<Void> dataDistributionQueue(Database cx,
                                         PromiseStream<RelocateShard> output,
                                         FutureStream<RelocateShard> input,
                                         PromiseStream<GetMetricsRequest> getShardMetrics,
                                         PromiseStream<GetTopKMetricsRequest> getTopKMetrics,
                                         Reference<AsyncVar<bool>> processingUnhealthy,
                                         Reference<AsyncVar<bool>> processingWiggle,
                                         std::vector<TeamCollectionInterface> teamCollections,
                                         Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure,
                                         MoveKeysLock lock,
                                         PromiseStream<Promise<int64_t>> getAverageShardBytes,
                                         FutureStream<Promise<int>> getUnhealthyRelocationCount,
                                         UID distributorId,
                                         int teamSize,
                                         int singleRegionTeamSize,
                                         const DDEnabledState* ddEnabledState) {
	state DDQueueData self(distributorId,
	                       lock,
	                       cx,
	                       teamCollections,
	                       shardsAffectedByTeamFailure,
	                       getAverageShardBytes,
	                       teamSize,
	                       singleRegionTeamSize,
	                       output,
	                       input,
	                       getShardMetrics,
	                       getTopKMetrics);
	state std::set<UID> serversToLaunchFrom;
	state KeyRange keysToLaunchFrom;
	state RelocateData launchData;
	state Future<Void> recordMetrics = delay(SERVER_KNOBS->DD_QUEUE_LOGGING_INTERVAL);

	state std::vector<Future<Void>> balancingFutures;

	state PromiseStream<KeyRange> rangesComplete;
	state Future<Void> launchQueuedWorkTimeout = Never();

	for (int i = 0; i < teamCollections.size(); i++) {
		// FIXME: Use BgDDLoadBalance for disk rebalance too after DD simulation test proof.
		// balancingFutures.push_back(BgDDLoadRebalance(&self, i, DataMovementReason::REBALANCE_OVERUTILIZED_TEAM));
		// balancingFutures.push_back(BgDDLoadRebalance(&self, i, DataMovementReason::REBALANCE_UNDERUTILIZED_TEAM));
		if (SERVER_KNOBS->READ_SAMPLING_ENABLED) {
			balancingFutures.push_back(BgDDLoadRebalance(&self, i, DataMovementReason::REBALANCE_READ_OVERUTIL_TEAM));
			balancingFutures.push_back(BgDDLoadRebalance(&self, i, DataMovementReason::REBALANCE_READ_UNDERUTIL_TEAM));
		}
		balancingFutures.push_back(BgDDMountainChopper(&self, i));
		balancingFutures.push_back(BgDDValleyFiller(&self, i));
	}
	balancingFutures.push_back(delayedAsyncVar(self.rawProcessingUnhealthy, processingUnhealthy, 0));
	balancingFutures.push_back(delayedAsyncVar(self.rawProcessingWiggle, processingWiggle, 0));

	try {
		loop {
			self.validate();

			// For the given servers that caused us to go around the loop, find the next item(s) that can be
			// launched.
			if (launchData.startTime != -1) {
				// Launch dataDistributionRelocator actor to relocate the launchData
				self.launchQueuedWork(launchData, ddEnabledState);
				launchData = RelocateData();
			} else if (!keysToLaunchFrom.empty()) {
				self.launchQueuedWork(keysToLaunchFrom, ddEnabledState);
				keysToLaunchFrom = KeyRangeRef();
			}

			ASSERT(launchData.startTime == -1 && keysToLaunchFrom.empty());

			choose {
				when(RelocateShard rs = waitNext(self.input)) {
					if (rs.isRestore()) {
						ASSERT(rs.dataMove != nullptr);
						ASSERT(rs.dataMoveId.isValid());
						self.launchQueuedWork(RelocateData(rs), ddEnabledState);
					} else if (rs.cancelled) {
						self.enqueueCancelledDataMove(rs.dataMoveId, rs.keys, ddEnabledState);
					} else {
						bool wasEmpty = serversToLaunchFrom.empty();
						self.queueRelocation(rs, serversToLaunchFrom);
						if (wasEmpty && !serversToLaunchFrom.empty())
							launchQueuedWorkTimeout = delay(0, TaskPriority::DataDistributionLaunch);
					}
				}
				when(wait(launchQueuedWorkTimeout)) {
					self.launchQueuedWork(serversToLaunchFrom, ddEnabledState);
					serversToLaunchFrom = std::set<UID>();
					launchQueuedWorkTimeout = Never();
				}
				when(RelocateData results = waitNext(self.fetchSourceServersComplete.getFuture())) {
					// This when is triggered by queueRelocation() which is triggered by sending self.input
					self.completeSourceFetch(results);
					launchData = results;
				}
				when(RelocateData done = waitNext(self.dataTransferComplete.getFuture())) {
					complete(done, self.busymap, self.destBusymap);
					if (serversToLaunchFrom.empty() && !done.src.empty())
						launchQueuedWorkTimeout = delay(0, TaskPriority::DataDistributionLaunch);
					serversToLaunchFrom.insert(done.src.begin(), done.src.end());
				}
				when(RelocateData done = waitNext(self.relocationComplete.getFuture())) {
					self.activeRelocations--;
					TraceEvent(SevVerbose, "InFlightRelocationChange")
					    .detail("Complete", done.dataMoveId)
					    .detail("IsRestore", done.isRestore())
					    .detail("Total", self.activeRelocations);
					self.finishRelocation(done.priority, done.healthPriority);
					self.fetchKeysComplete.erase(done);
					// self.logRelocation( done, "ShardRelocatorDone" );
					self.noErrorActors.add(
					    tag(delay(0, TaskPriority::DataDistributionLaunch), done.keys, rangesComplete));
					if (g_network->isSimulated() && debug_isCheckRelocationDuration() && now() - done.startTime > 60) {
						TraceEvent(SevWarnAlways, "RelocationDurationTooLong")
						    .detail("Duration", now() - done.startTime);
						debug_setCheckRelocationDuration(false);
					}
				}
				when(KeyRange done = waitNext(rangesComplete.getFuture())) { keysToLaunchFrom = done; }
				when(wait(recordMetrics)) {
					Promise<int64_t> req;
					getAverageShardBytes.send(req);

					recordMetrics = delay(SERVER_KNOBS->DD_QUEUE_LOGGING_INTERVAL, TaskPriority::FlushTrace);

					auto const highestPriorityRelocation = self.getHighestPriorityRelocation();

					TraceEvent("MovingData", distributorId)
					    .detail("InFlight", self.activeRelocations)
					    .detail("InQueue", self.queuedRelocations)
					    .detail("AverageShardSize", req.getFuture().isReady() ? req.getFuture().get() : -1)
					    .detail("UnhealthyRelocations", self.unhealthyRelocations)
					    .detail("HighestPriority", highestPriorityRelocation)
					    .detail("BytesWritten", self.bytesWritten)
					    .detail("PriorityRecoverMove", self.priority_relocations[SERVER_KNOBS->PRIORITY_RECOVER_MOVE])
					    .detail("PriorityRebalanceUnderutilizedTeam",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_UNDERUTILIZED_TEAM])
					    .detail("PriorityRebalanceOverutilizedTeam",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_OVERUTILIZED_TEAM])
					    .detail("PriorityRebalanceReadUnderutilTeam",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_READ_UNDERUTIL_TEAM])
					    .detail("PriorityRebalanceReadOverutilTeam",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_REBALANCE_READ_OVERUTIL_TEAM])
					    .detail("PriorityStorageWiggle",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE])
					    .detail("PriorityTeamHealthy", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_HEALTHY])
					    .detail("PriorityTeamContainsUndesiredServer",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER])
					    .detail("PriorityTeamRedundant",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT])
					    .detail("PriorityMergeShard", self.priority_relocations[SERVER_KNOBS->PRIORITY_MERGE_SHARD])
					    .detail("PriorityPopulateRegion",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_POPULATE_REGION])
					    .detail("PriorityTeamUnhealthy",
					            self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY])
					    .detail("PriorityTeam2Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_2_LEFT])
					    .detail("PriorityTeam1Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_1_LEFT])
					    .detail("PriorityTeam0Left", self.priority_relocations[SERVER_KNOBS->PRIORITY_TEAM_0_LEFT])
					    .detail("PrioritySplitShard", self.priority_relocations[SERVER_KNOBS->PRIORITY_SPLIT_SHARD])
					    .trackLatest("MovingData"); // This trace event's trackLatest lifetime is controlled by
					                                // DataDistributor::movingDataEventHolder. The track latest
					                                // key we use here must match the key used in the holder.
				}
				when(wait(self.error.getFuture())) {} // Propagate errors from dataDistributionRelocator
				when(wait(waitForAll(balancingFutures))) {}
				when(Promise<int> r = waitNext(getUnhealthyRelocationCount)) { r.send(self.unhealthyRelocations); }
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_broken_promise && // FIXME: Get rid of these broken_promise errors every time we
		                                             // are killed by the master dying
		    e.code() != error_code_movekeys_conflict && e.code() != error_code_data_move_cancelled &&
		    e.code() != error_code_data_move_dest_team_not_found)
			TraceEvent(SevError, "DataDistributionQueueError", distributorId).error(e);
		throw e;
	}
}
