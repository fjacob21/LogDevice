/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <atomic>
#include <memory>
#include <random>
#include <vector>

#include "logdevice/common/DomainIsolationChecker.h"
#include "logdevice/common/EventLoopHandle.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/Request.h"
#include "logdevice/common/RequestPump.h"
#include "logdevice/common/SequencerPlacement.h"
#include "logdevice/common/Timestamp.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/configuration/ServerConfig.h"
#include "logdevice/common/protocol/GOSSIP_Message.h"
#include "logdevice/common/settings/GossipSettings.h"
#include "logdevice/common/settings/UpdateableSettings.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/server/sequencer_boycotting/BoycottTracker.h"

namespace facebook { namespace logdevice {

/**
 * @file A gossip-based failure detector. Responsible for exchanging GOSSIP
 *       messages carrying information about availability of other nodes.
 */

class GOSSIP_Message;
class ServerProcessor;
class Socket;
class StatsHolder;
class ClusterState;
class MockFailureDetector;

class FailureDetector {
 public:
  // Possible states each node can be in. All nodes initially start as DEAD, and
  // are moved into ALIVE as long as they're consistently up (as determined by
  // the failure detector) during the last `suspect_duration' seconds.
  enum NodeState : uint8_t {
    // Node has been known to be alive for some time now.
    ALIVE = 0,
    // Node is likely up (it's gossiping), but we'll give it some more time
    // before promoting it to ALIVE.
    SUSPECT = 1,
    DEAD = 2
  };

  /**
   * An interface used to pick a node to send a gossip message to. Subclasses
   * don't need to worry about thread safety (NodeSelector is only used from a
   * single worker thread).
   */
  class NodeSelector {
   public:
    virtual ~NodeSelector() {}
    virtual NodeID getNode(FailureDetector* failure_detector) = 0;
  };

  /**
   * @param settings   failure detector-specific settings
   * @param processor  processor pointer to get main settings and
   *                   pass for gossip thread(Worker class) constructor
   * @param stats      for updating FD counters
   * @param attach     skip gossiping for certain tests(if set to false)
   */
  FailureDetector(UpdateableSettings<GossipSettings> settings,
                  ServerProcessor* processor,
                  StatsHolder* stats,
                  bool attach = true);
  /*
   * For MockTest like FailureDetectorTest, which do their own mock gossips.
   * These tests don't set processor->failure_detector_
   */
  FailureDetector(UpdateableSettings<GossipSettings> settings,
                  ServerProcessor* processor,
                  bool attach = false);

  virtual ~FailureDetector() {}

  /**
   * Checks whether a node with the given index is considered to be
   * available.
   */
  bool isAlive(node_index_t idx) const;

  /**
   * Set a flag indicating that this node requests all shards to be moved to
   * other nodes. This is implemented by gossiping extra bits of information.
   * As a result, other nodes will treat this node as if it's dead.
   */
  void failover() {
    failover_.store(true);
  }

  /**
   * Called upon receiving a GOSSIP message.
   * This method will always be executed on gossip thread.
   */
  void onGossipReceived(const GOSSIP_Message& msg);

  /**
   * Returns a human-readable representation of the state of a node with the
   * given index in the cluster.
   */
  std::string getStateString(node_index_t idx) const;

  /**
   * Returns a human-readable representation of the isolation status of
   * local failure domains in different scopes.
   */
  std::string getDomainIsolationString() const;

  /**
   * Accessors for the blacklisted boolean member of the Node structure.
   *
   * Blacklisting a node means that this node is omitted when selecting
   * a destination for gossip messages. This allows virtually creating network
   * partitioning between nodes, which is handy for testing the gossip protocol.
   */
  void setBlacklisted(node_index_t idx, bool blacklisted);
  bool isBlacklisted(node_index_t idx) const;

  /**
   * Returns gossip specific settings
   */
  std::shared_ptr<const GossipSettings> getGossipSettings() const {
    return settings_.get();
  }

  /**
   * If a gossip message arrives on a non-gossip thread, create
   * a GossipRequest and post it to the gossip thread to process.
   * This can happen if the sending node(running on an older release)
   * sends gossip on data port and the message arrives on some worker
   * owning that connection.
   */
  int postGossipRequest(std::unique_ptr<Request> rq);

  /**
   * Check if the local failure domain determined by location scope @param scope
   * is isolated. Proxy the call to isolation_checker_.
   */
  bool isMyDomainIsolated(NodeLocationScope scope) const;

  /**
   * Called when cluster configuration has changed. Must be called on the
   * thread where FailureDetector is attached.
   */
  void noteConfigurationChanged();

  /**
   * Returns whether this node is isolated from a majority of the cluster.
   * This is true when it believes more than half of the cluster is dead.
   */
  bool isIsolated() const;

  void shutdown();

  /**
   * This method can be called in 2 cases:
   * 1. As part of notification of cluster state reply,
   *    so that FailureDetector can initialize its internal state at startup.
   * 2. On timing out waiting for cluster-state reply to come.
   *
   * @param
   *   cs_update :      default to empty vector
   *                    not-empty if a cluster-state update is received
   * @param
   *   boycotted_nodes: default to empty vector
   *                    not-empty if a cluster-state update is received
   */
  void buildInitialState(const std::vector<uint8_t>& cs_update = {},
                         std::vector<node_index_t> boycotted_nodes = {});

  // called by the onSent method of GOSSIP_Message to notify the
  // failure detector
  void onGossipMessageSent(Status st, const Address& to, uint64_t msg_id);

  // update the ClusterState with the boycotts contained in the BoycottTracker
  void updateBoycottedNodes();
  /**
   * Thread safe
   *
   * The NodeStatsController will update the BoycottTracker via this function
   * once outliers are detected
   */
  void setOutliers(std::vector<NodeID> outliers);
  /**
   * Thread safe
   *
   * Resets the boycott of the given node
   */
  void resetBoycottedNode(node_index_t node_index);

  /*
   * Returns whether the node is considered an outlier
   */
  bool isOutlier(node_index_t node_index);

  /**
   * Thread safe
   *
   * Used to get count of effective dead nodes and effective cluster size.
   *
   * Parameters are pointers to write back their respective values to.
   */
  void getClusterDeadNodeStats(size_t* effective_dead_cnt,
                               size_t* effective_cluster_size);

 protected:
  // send a gossip message to some node in the cluster
  void gossip();

  void cancelTimers();

 private:
  class InitRequest;
  class RandomSelector;
  class RoundRobinSelector;

  Timer cs_timer_;
  bool waiting_for_cluster_state_{true};

  // Don't rely on gossip() and detectFailures() to transition this node out
  // of suspect state(as it can cause 100ms race). This timer helps in making
  // sure that this node transitions from SUSPECT to ALIVE earliest(on this node
  // , as opposed to some other node). The rare case when this may not happen is
  // when gossip thread is not scheduled for some time.
  Timer suspect_timer_;
  void startSuspectTimer();

  void startGossiping();

  /**
   * Send a GET-CLUSTER-STATE to fetch ALIVE/DEAD status of cluster nodes
   * If reply comes within timeout, transition the cluster nodes to the
   * received state immediately, otherwise use the previous state transition
   * logic i.e. move everyone from DEAD to SUSPECT at the same time (i.e. while
   * executing gossip() and calling detectFailures())
   */
  void startClusterStateTimer();

  void sendGetClusterState();

  struct Node {
    NodeState state;
    bool blacklisted;
  };

  // Time when a node was suspected to be ALIVE, and transitioned
  // from DEAD to SUSPECT state
  std::unique_ptr<std::atomic<std::chrono::milliseconds>[]> last_suspected_at_ {
    nullptr
  };

  // Used for restricting the logging of FD state and Gossip messages
  // to the first 'gossip_msg_dump_duration' seconds
  std::chrono::steady_clock::time_point start_time_;

  // Used for rate limiting the output of dumpFDState().
  AtomicSteadyTimestamp last_state_dump_time_{AtomicSteadyTimestamp::min()};

  bool isTracingOn() {
    return (std::chrono::steady_clock::now() - start_time_ <=
            settings_->gossip_msg_dump_duration);
  }

  bool shouldDumpState() {
    if (isTracingOn()) {
      return true;
    }
    if (facebook::logdevice::dbg::currentLevel <
        facebook::logdevice::dbg::Level::SPEW) {
      return false;
    }
    // Throttle to once every 0.5 seconds.
    const std::chrono::milliseconds state_dump_period{500};
    if (SteadyTimestamp::now() < last_state_dump_time_ + state_dump_period) {
      return false;
    }
    last_state_dump_time_ = SteadyTimestamp::now();
    return true;
  }

  std::chrono::milliseconds getCurrentTimeInMillis() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
  }

  // Check if incoming gossip message is more than
  // 'gossip_time_skew_threshold' milli-seconds delayed
  bool checkSkew(const GOSSIP_Message& msg);

  bool isValidInstanceId(std::chrono::milliseconds id, node_index_t idx);

  // Detects which nodes are down based on the data in gossip_list_ and
  // suspect_matrix_.
  void detectFailures(node_index_t self, size_t n);

  // Executes a state transition and updates the dead list; `dead' is used to
  // indicates that the node was detected as unavailable through gossiping.
  void updateNodeState(node_index_t node, bool dead, bool self, bool failover);

  // Is `node' a good candidate to send a GOSSIP message to?
  bool isValidDestination(node_index_t node);

  // Returns the node state name. Used in dbg statements.
  const char* getNodeStateString(NodeState state) const;

  // returns a human-readable representation of the suspect matrix
  std::string dumpSuspectMatrix(const GOSSIP_Message::suspect_matrix_t& sm);

  // Dumps dead/suspect/alive status of all nodes in the cluster
  // as perceived by this node's Failure Detector.
  void dumpFDState();

  // used to dump gossip list
  std::string dumpGossipList(std::vector<uint32_t> list);

  // used to dump instance ids(timestamps) of all nodes
  std::string dumpInstanceList(std::vector<std::chrono::milliseconds> list);

  void dumpGossipMessage(const GOSSIP_Message& msg);

  /**
   * Used for handling bringup & suspect-state-finished notifications.
   * All other information contained in the gossip message is ignored.
   *
   * @return: true:  if msg contains NODE_BRINGUP_FLAG or
   *                 SUSPECT_STATE_FINISHED flags
   *          false: otherwise
   */
  bool processFlags(const GOSSIP_Message& msg);

  std::string flagsToString(GOSSIP_Message::GOSSIP_flags_t flags);

  void updateDependencies(node_index_t idx, NodeState new_state, bool failover);

  // A gossip list -- indicates how many gossip periods have passed before
  // hearing from each node (either directly or through a gossip message).
  std::vector<uint32_t> gossip_list_;

  // instance ids(timestamps) of all nodes
  std::vector<std::chrono::milliseconds> gossip_ts_;

  // Monotonically increasing instance ID of logdeviced
  // This is used to distinguish b/w server instances across restarts.
  std::chrono::milliseconds instance_id_;

  // Failover value of some node(Ni) contains either of the following 2 values:
  // a) 0 : Ni is up
  // b) Ni's instance id(startup time in this case) : Ni requested failover
  std::vector<std::chrono::milliseconds> failover_list_;

  // A suspect matrix. An entry (i, j) is 1 if node i suspects node j to be
  // unavailable. Updated only from a dedicated worker thread.
  std::vector<std::vector<uint8_t>> suspect_matrix_;

  // Contains the current state for each node in the cluster, as well as the
  // most recent time the node entered the suspect state.
  std::vector<Node> nodes_;

  // Set when performing a graceful failover. Causes a failure list to be
  // updated so other nodes are notified that they should treat this one as if
  // it's down.
  std::atomic_bool failover_{false};

  // id of the gossiping worker thread
  // worker_id_t gossip_thread_id_{-1};

  // used by gossip() to pick a target node
  std::unique_ptr<NodeSelector> selector_;

  // Domain isolation checker for detecting isolation of the local domain
  std::unique_ptr<DomainIsolationChecker> isolation_checker_;

  // Protects access to failover_list_ and nodes_. No contention is expected on
  // this lock since it's only ever used from a gossip thread and an admin
  // thread when dumping the state of the failure detector.
  mutable std::mutex mutex_;

  // failure detection threshold and other settings
  UpdateableSettings<GossipSettings> settings_;

  // Stats holder to maintain counters
  StatsHolder* stats_{nullptr};

  ServerProcessor* processor_;

  // Whether this node is isolated or not. This is updated during failure
  // detection. It is set to True if the number of dead nodes is greater
  // than 50% of the cluster. It can be ignored via command line args.
  std::atomic_bool isolated_{false};

  size_t num_gossips_received_{0};

  // keep track of current gossip being sent
  uint64_t current_msg_id_{0};

  // keep track of consecutive failures to send
  size_t num_gossip_attempts_failed_{0};

  // below are calculated in detectFailures and used to calculate % of dead
  // nodes in cluster
  size_t effective_dead_cnt_{0};
  size_t effective_cluster_size_{0};

  // keep track of the time of the last gossip tick, which is when
  // the tick counters in gossip_list_ were last updated
  SteadyTimestamp last_gossip_tick_time_{SteadyTimestamp::min()};

  // save pointer to the timer so we can explicitly trigger it to force retries
  ExponentialBackoffTimerNode* gossip_timer_node_{nullptr};

  BoycottTracker boycott_tracker_;
  // returns the boycott tracker. Used to be able to mock it out in tests
  virtual BoycottTracker& getBoycottTracker() {
    return boycott_tracker_;
  }

  // these helper functions are overridden in unit tests
  virtual std::shared_ptr<ServerConfig> getServerConfig() const;
  virtual ClusterState* getClusterState() const;
  virtual int sendGossipMessage(NodeID, std::unique_ptr<GOSSIP_Message>);

  void broadcastWrapper(GOSSIP_Message::GOSSIP_flags_t flags);

  // Notify all cluster nodes that this node just came up.
  // This is a best-effort message and sent only once.
  // gossip_list, failover_list and suspect matrix are not included.
  // All further messages will be sent every 'gossip-interval'
  // milli-seconds via gossip().
  void broadcastBringup() {
    startSuspectTimer();
    broadcastWrapper(GOSSIP_Message::NODE_BRINGUP_FLAG);
  }

  void broadcastSuspectDurationFinished() {
    // clubbed with bringup flag for backward compatibility,
    // so that older code doesn't process this as a regular
    // gossip message.
    broadcastWrapper(GOSSIP_Message::SUSPECT_STATE_FINISHED |
                     GOSSIP_Message::NODE_BRINGUP_FLAG);
  }

  virtual Socket* getServerSocket(node_index_t idx);
  virtual StatsHolder* getStats();

  friend class MockFailureDetector;
};
}} // namespace facebook::logdevice
