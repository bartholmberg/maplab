#include "maplab-server-node/maplab-server-node.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <landmark-triangulation/pose-interpolator.h>
#include <maplab-common/file-system-tools.h>
#include <maplab-common/sigint-breaker.h>
#include <maplab-common/threading-helpers.h>
#include <signal.h>
#include <vi-map-basic-plugin/vi-map-basic-plugin.h>

#include <atomic>
#include <memory>
#include <string>

DECLARE_bool(ros_free);

DEFINE_int32(
    maplab_server_submap_loading_thread_pool_size, 4,
    "Number of threads used to load and pre-process incoming submaps. These "
    "threads are different from the one thread that is merging and optimizing "
    "the global map.");

DEFINE_string(
    maplab_server_merged_map_folder, "",
    "Where the finished/intermediate maps should be stored. Not optional.");

DEFINE_string(
    maplab_server_resource_folder, "",
    "Where the resources of the merged map should be stored, if empty, the "
    "standard map resource folder is used.");

DEFINE_int32(
    maplab_server_backup_interval_s, 300,
    "Create a backup of the current map every n seconds. 0 = no backups.");

namespace maplab {
MaplabServerNode::MaplabServerNode(const MaplabServerNodeConfig& config)
    : config_(config),
      submap_loading_thread_pool_(
          FLAGS_maplab_server_submap_loading_thread_pool_size),
      base_console_("base_console", 0 /*argc*/, nullptr /*argv*/),
      plotter_(nullptr),
      is_running_(false),
      shut_down_requested_(false),
      merging_thread_busy_(false) {
  if (!FLAGS_ros_free) {
    visualization::RVizVisualizationSink::init();
    plotter_.reset(new visualization::ViwlsGraphRvizPlotter);
  }
}

MaplabServerNode::~MaplabServerNode() {
  shutdown();
}

void MaplabServerNode::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  LOG(INFO) << "[MaplabServerNode] Starting...";

  if (shut_down_requested_.load()) {
    LOG(ERROR)
        << "[MaplabServerNode] Cannot start node (again), a shutdown has "
        << "already been requrested!";
    return;
  }

  LOG(INFO) << "[MaplabServerNode] launching MapMerging thread...";

  submap_merging_thread_ = std::thread([this]() {
    // Loop until shutdown is requested.
    bool received_first_submap = false;
    double time_of_last_map_backup_s = 0;
    while (!shut_down_requested_.load()) {
      std::vector<std::string> all_map_keys;
      map_manager_.getAllMapKeys(&all_map_keys);

      if (!received_first_submap && all_map_keys.empty()) {
        VLOG(1) << "[MaplabServerNode] MapMerging - waiting for first "
                   "submap to be loaded...";
        std::this_thread::sleep_for(
            std::chrono::seconds(kSecondsToSleepBetweenAttempts));
        continue;
      }
      merging_thread_busy_ = true;
      if (VLOG_IS_ON(1)) {
        // List all loaded maps.
        std::stringstream ss;
        ss << "[MaplabServerNode] MapMerging - Loaded maps ("
           << all_map_keys.size() << " total):";
        std::sort(all_map_keys.begin(), all_map_keys.end());
        for (const std::string& key : all_map_keys) {
          ss << "\n  " << key;
        }
        VLOG(1) << ss.str();
      }

      // Check if there is are submaps ready to merge and add them to the merged
      // map.
      {
        std::lock_guard<std::mutex> lock(submap_processing_queue_mutex_);
        while (!submap_processing_queue_.empty() &&
               !shut_down_requested_.load()) {
          SubmapProcess& submap_process = submap_processing_queue_.front();

          // Try to lock the submap_process struct, if we fail this means
          // something is still processing so we need to stop and try again
          // later.
          if (!submap_process.mutex.try_lock()) {
            break;
          }
          // Check if submap has finished loading.
          if (!submap_process.is_loaded) {
            // Give up and try again later.
            submap_process.mutex.unlock();
            break;
          }

          // Check if submap has finished processing as well.
          if (!submap_process.is_processed) {
            // Give up and try again later.
            submap_process.mutex.unlock();
            break;
          }

          VLOG(3) << "[MaplabServerNode] MapMerging - submap with key '"
                  << submap_process.map_key << "' is ready to be merged.";

          CHECK(!submap_process.map_key.empty());
          CHECK(map_manager_.hasMap(submap_process.map_key));

          // If we don't have a merged map yet, simply rename the submap into
          // the merged map.
          {
            std::lock_guard<std::mutex> merge_command_lock(
                current_merge_command_mutex_);
            current_merge_command_ = "merging submap";
          }

          vi_map::MissionId submap_mission_id;
          {
            vi_map::VIMapManager::MapWriteAccess submap =
                map_manager_.getMapWriteAccess(submap_process.map_key);
            CHECK_EQ(submap->numMissions(), 1u);
            submap_mission_id = submap->getIdOfFirstMission();
          }

          if (!map_manager_.hasMap(kMergedMapKey)) {
            VLOG(3) << "[MaplabServerNode] MapMerging - first submap is "
                       "used to initalize merged map with key '"
                    << kMergedMapKey << "'.";
            map_manager_.renameMap(submap_process.map_key, kMergedMapKey);

            // Set baseframe of this first mission to known.
            vi_map::VIMapManager::MapWriteAccess map =
                map_manager_.getMapWriteAccess(kMergedMapKey);
            CHECK_EQ(map->numMissions(), 1u);
            const vi_map::MissionId mission_id = map->getIdOfFirstMission();
            CHECK(mission_id.isValid());
            const vi_map::MissionBaseFrameId& mission_baseframe_id =
                map->getMission(mission_id).getBaseFrameId();
            CHECK(mission_baseframe_id.isValid());
            map->getMissionBaseFrame(mission_baseframe_id)
                .set_is_T_G_M_known(true);

            received_first_submap = true;
            time_of_last_map_backup_s = aslam::time::nanoSecondsToSeconds(
                aslam::time::nanoSecondsSinceEpoch());
          } else {
            VLOG(3) << "[MaplabServerNode] MapMerging - merge submap into "
                       "merged map with key '"
                    << kMergedMapKey << "'";

            // TODO(mfehr): make this more robust: if merging fails, either
            // try again later or load as new mission so we don't loose the
            // data.
            CHECK(map_manager_.mergeSubmapIntoBaseMap(
                kMergedMapKey, submap_process.map_key));
            // Remove submap.
            map_manager_.deleteMap(submap_process.map_key);
          }
          CHECK(map_manager_.hasMap(kMergedMapKey));
          CHECK(!map_manager_.hasMap(submap_process.map_key));

          // Update robot to mission map.
          CHECK(submap_mission_id.isValid());
          if (!submap_process.robot_name.empty()) {
            std::lock_guard<std::mutex> lock(robot_to_mission_id_map_mutex_);
            robot_to_mission_id_map_[submap_process.robot_name] =
                submap_mission_id;
          } else {
            LOG(WARNING) << "[MaplabServerNode] Submap with key "
                         << submap_process.map_key
                         << " does not have a robot name associated with it!";
          }

          submap_process.is_merged = true;

          // Unlock the submap process struct.
          submap_process.mutex.unlock();

          // Remove the struct from the list of processed submaps.
          submap_processing_queue_.pop_front();
        }
      }

      if (received_first_submap) {
        VLOG(3) << "[MaplabServerNode] MapMerging - processing global map "
                << "commands on map with key '" << kMergedMapKey << "'";

        // Copy console to process the global map.
        MapLabConsole console(
            base_console_, "global_map_console", false /*disable plotter*/);

        // Select submap.
        console.setSelectedMapKey(kMergedMapKey);

        for (const std::string& command : config_.global_map_commands) {
          {
            std::lock_guard<std::mutex> merge_command_lock(
                current_merge_command_mutex_);
            current_merge_command_ = command;
          }
          VLOG(3) << "[MaplabServerNode] MapMerging console command: "
                  << command;
          if (console.RunCommand(command) != common::kSuccess) {
            LOG(ERROR) << "[MaplabServerNode] MapMerging - failed to run "
                       << "command: '" << command << "'.";
          } else {
            VLOG(3)
                << "[MaplabServerNode] MapMerging console command successful.";
          }
        }
        {
          std::lock_guard<std::mutex> merge_command_lock(
              current_merge_command_mutex_);
          current_merge_command_ = "";
        }
      }

      const double time_now_s = aslam::time::nanoSecondsToSeconds(
          aslam::time::nanoSecondsSinceEpoch());

      if ((time_now_s - time_of_last_map_backup_s) >
              FLAGS_maplab_server_backup_interval_s &&
          FLAGS_maplab_server_backup_interval_s > 0) {
        LOG(INFO) << "[MaplabServerNode] MapMerging - saving map as backup.";
        {
          std::lock_guard<std::mutex> merge_command_lock(
              current_merge_command_mutex_);
          current_merge_command_ = "save map";
        }
        saveMap();

        time_of_last_map_backup_s = time_now_s;
      }
      merging_thread_busy_ = false;
      std::this_thread::sleep_for(
          std::chrono::seconds(kSecondsToSleepBetweenAttempts));
    }
  });

  LOG(INFO) << "[MaplabServerNode] launching Status thread...";

  status_thread_ = std::thread([this]() {
    // Loop until shutdown is requested.
    while (!shut_down_requested_.load()) {
      std::stringstream ss;

      ss << "\n================================================================"
         << "==\n";
      ss << "[MaplabServerNode] Status:\n";
      {
        std::lock_guard<std::mutex> lock(submap_processing_queue_mutex_);
        if (submap_processing_queue_.empty()) {
          ss << " - No submaps to process or merge...\n";
        } else {
          for (const SubmapProcess& submap_process : submap_processing_queue_) {
            ss << " - " << submap_process.robot_name << " - map '"
               << submap_process.map_key << "'\t: ";

            bool was_locked_by_other_process = false;
            if (submap_process.mutex.try_lock()) {
              ss << "(unlocked)";
              was_locked_by_other_process = false;
            } else {
              was_locked_by_other_process = true;
              ss << "(locked)";
            }

            if (submap_process.is_merged && was_locked_by_other_process) {
              ss << " merged\n";
            } else if (
                submap_process.is_merged && !was_locked_by_other_process) {
              LOG(ERROR)
                  << "[MaplabServerNode] A submap process cannot be merged and "
                  << "unlocked at the same time! Something is wrong!";
              ss << " ERROR!\n";
            } else if (
                submap_process.is_processed && was_locked_by_other_process) {
              ss << " merging...\n";
            } else if (
                submap_process.is_processed && !was_locked_by_other_process) {
              ss << " ready to merge\n";
            } else if (
                submap_process.is_loaded && was_locked_by_other_process) {
              ss << " processing...\n";
            } else if (
                submap_process.is_loaded && !was_locked_by_other_process) {
              ss << " queued for processing\n";
            } else if (
                !submap_process.is_loaded && was_locked_by_other_process) {
              ss << " loading...\n";
            } else if (
                !submap_process.is_loaded && !was_locked_by_other_process) {
              ss << " queued for loading\n";
            }

            if (!was_locked_by_other_process) {
              submap_process.mutex.unlock();
            }
          }
        }
      }
      ss << "================================================================"
         << "==\n";
      ss << " - Active submap threads: "
         << submap_loading_thread_pool_.numActiveThreads() << "/"
         << FLAGS_maplab_server_submap_loading_thread_pool_size << "\n";
      {
        std::lock_guard<std::mutex> command_lock(submap_commands_mutex_);
        for (const std::pair<const size_t, std::string>& comm :
             submap_commands_) {
          ss << "   - submap " << std::to_string(comm.first)
             << " - command: " << comm.second << "\n";
        }
      }

      ss << " - Active merging thread: ";
      if (merging_thread_busy_.load()) {
        ss << "yes\n";
        std::lock_guard<std::mutex> merge_command_lock(
            current_merge_command_mutex_);
        ss << "   - current command: " << current_merge_command_ << "\n";
      } else {
        ss << "no\n";
      }
      ss << "================================================================"
         << "==\n";
      {
        std::lock_guard<std::mutex> lock(robot_to_mission_id_map_mutex_);
        ss << "Robot to mission map: ";
        for (const std::pair<const std::string, vi_map::MissionId>& pair :
             robot_to_mission_id_map_) {
          ss << "\n - " << pair.first << "\t\t mission id: " << pair.second;
        }
      }
      ss << "\n================================================================"
         << "==\n";
      LOG(INFO) << ss.str();
      std::this_thread::sleep_for(
          std::chrono::seconds(kSecondsToSleepBetweenStatus));
    }
  });

  is_running_ = true;
  LOG(INFO) << "[MaplabServerNode] MapMerging - thread launched.";
}

void MaplabServerNode::shutdown() {
  if (shut_down_requested_.load()) {
    // Already shut down.
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  LOG(INFO) << "[MaplabServerNode] Shutting down...";
  shut_down_requested_.store(true);

  LOG(INFO) << "[MaplabServerNode] Stopping MapMerging thread...";
  submap_merging_thread_.join();
  LOG(INFO) << "[MaplabServerNode] Done.";

  LOG(INFO) << "[MaplabServerNode] Stopping SubmapProcessing threads...";
  submap_loading_thread_pool_.stop();
  submap_loading_thread_pool_.waitForEmptyQueue();
  LOG(INFO) << "[MaplabServerNode] Done.";

  LOG(INFO) << "[MaplabServerNode] Stopping Status thread...";
  status_thread_.join();
  LOG(INFO) << "[MaplabServerNode] Done.";

  is_running_ = false;
}

bool MaplabServerNode::saveMap(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  LOG(INFO) << "[MaplabServerNode] Saving map to '" << path << "'.";
  if (map_manager_.hasMap(kMergedMapKey)) {
    return map_manager_.saveMapToFolder(
        kMergedMapKey, path, vi_map::parseSaveConfigFromGFlags());
  } else {
    return false;
  }
}

bool MaplabServerNode::loadAndProcessSubmap(
    const std::string& robot_name, const std::string& submap_path) {
  CHECK(!submap_path.empty());
  CHECK(!robot_name.empty());

  std::lock_guard<std::mutex> lock(mutex_);

  if (shut_down_requested_.load()) {
    LOG(WARNING) << "[MaplabServerNode] shutdown was requrested, will ignore "
                 << " SubmapProcessing thread for submap at '" << submap_path
                 << "'.";
    return false;
  }

  LOG(INFO) << "[MaplabServerNode] launching SubmapProcessing thread for "
            << "submap at '" << submap_path << "'.";

  std::lock_guard<std::mutex> submap_queue_lock(submap_processing_queue_mutex_);
  // Add new element at the back.
  submap_processing_queue_.emplace_back();

  SubmapProcess& submap_process = submap_processing_queue_.back();
  submap_process.path = submap_path;
  submap_process.robot_name = robot_name;
  submap_process.map_hash = std::hash<std::string>{}(submap_path);
  submap_process.map_key =
      submap_process.robot_name + "_" + std::to_string(submap_process.map_hash);

  // Start a thread that loads the map and updates the submap entry in the
  // queue when done. The submap mutex will be released in the meantime,
  // such that submaps that arrive later can be loaded and processed in
  // parallel.
  const size_t kSubmapLoadingExclusivityGroup =
      // std::hash<std::string>{}(submap_process.robot_name);
      aslam::ThreadPool::kGroupdIdNonExclusiveTask;
  submap_loading_thread_pool_.enqueueOrdered(
      kSubmapLoadingExclusivityGroup, [&submap_process, this]() {
        std::lock_guard<std::mutex> submap_process_lock(submap_process.mutex);

        VLOG(3) << "[MaplabServerNode] SubmapProcessing - loading and "
                << "processing submap from '" << submap_process.path << "'...";

        {
          std::lock_guard<std::mutex> command_lock(submap_commands_mutex_);
          submap_commands_[submap_process.map_hash] = "loading";
        }

        // TODO(mfehr): Make this more robust: in case of a submap failing to
        // load (submap is lost), treat subsequent submaps of the same robot
        // as new trajectory and start a new mission.

        if (map_manager_.hasMap(submap_process.map_key)) {
          LOG(FATAL)
              << "[MaplabServerNode] There is already a map with this map "
              << "key in storage, something went wrong! key '"
              << submap_process.map_key << "'";
        }

        CHECK(map_manager_.loadMapFromFolder(
            submap_process.path, submap_process.map_key));

        submap_process.is_loaded = true;

        VLOG(3) << "[MaplabServerNode] SubmapProcessing - finished loading "
                   "submap with key '"
                << submap_process.map_key << "', starts processing...";

        // Copy console to process the global map.
        const std::string console_name =
            "submap_processing_console_" + submap_process.map_key;
        MapLabConsole console(
            base_console_, console_name, false /*disable plotter*/);

        // Select submap.
        console.setSelectedMapKey(submap_process.map_key);

        for (const std::string& command : config_.submap_commands) {
          {
            std::lock_guard<std::mutex> command_lock(submap_commands_mutex_);
            submap_commands_[submap_process.map_hash] = command;
          }
          VLOG(3) << "[MaplabServerNode] SubmapProcessing console command: "
                  << command;
          if (console.RunCommand(command) != common::kSuccess) {
            LOG(ERROR) << "[MaplabServerNode] SubmapProcessing - failed to run "
                          "command: '"
                       << command << "' on submap '" << submap_process.map_key
                       << "'.";
          } else {
            VLOG(3) << "[MaplabServerNode] SubmapProcessing console command "
                       "successful.";
          }

          if (shut_down_requested_.load()) {
            LOG(WARNING)
                << "[MaplabServerNode] SubmapProcessing - shutdown was "
                   "requested, aborting processing of submap with key '"
                << submap_process.map_key << "'...";
            {
              std::lock_guard<std::mutex> command_lock(submap_commands_mutex_);
              submap_commands_.erase(submap_process.map_hash);
            }
            return true;
          }
        }
        {
          std::lock_guard<std::mutex> command_lock(submap_commands_mutex_);
          submap_commands_.erase(submap_process.map_hash);
        }
        submap_process.is_processed = true;

        VLOG(3) << "[MaplabServerNode] SubmapProcessing - finished processing "
                   "submap with key '"
                << submap_process.map_key << "'.";
        return true;
      });

  LOG(INFO) << "[MaplabServerNode] SubmapProcessing - thread launched.";
  return true;
}

bool MaplabServerNode::saveMap() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (FLAGS_maplab_server_merged_map_folder.empty()) {
    LOG(ERROR) << "[MaplabServerNode] Cannot save map because "
                  "--maplab_server_merged_map_folder is empty!";
    return false;
  }

  LOG(INFO) << "[MaplabServerNode] Saving map to '"
            << FLAGS_maplab_server_merged_map_folder << "'.";
  if (map_manager_.hasMap(kMergedMapKey)) {
    return map_manager_.saveMapToFolder(
        kMergedMapKey, FLAGS_maplab_server_merged_map_folder,
        vi_map::parseSaveConfigFromGFlags());
  } else {
    return false;
  }
}

void MaplabServerNode::visualizeMap() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (plotter_ != nullptr) {
    if (map_manager_.hasMap(kMergedMapKey)) {
      LOG(INFO) << "[MaplabServerNode] Visualizing map...";
      const vi_map::VIMapManager map_manager;
      vi_map::VIMapManager::MapReadAccess map =
          map_manager.getMapReadAccess(kMergedMapKey);
      plotter_->visualizeMap(*map);
    } else {
      LOG(WARNING) << "[MaplabServerNode] Could not visualize merged map, as "
                      "it doesn't exist yet!";
    }
  } else {
    LOG(WARNING) << "[MaplabServerNode] No plotter was added to the maplab "
                 << "server node, cannot visualize map!";
  }
}

MaplabServerNode::MapLookupStatus MaplabServerNode::mapLookup(
    const std::string& robot_name, const vi_map::SensorType sensor_type,
    const int64_t timestamp_ns, const Eigen::Vector3d& p_S,
    Eigen::Vector3d* p_G, Eigen::Vector3d* sensor_p_G) const {
  CHECK_NOTNULL(p_G);
  CHECK_NOTNULL(sensor_p_G);

  if (robot_name.empty()) {
    LOG(WARNING)
        << "[MaplabServerNode] Received map lookup with empty robot name!";
    return MapLookupStatus::kNoSuchMission;
  }
  vi_map::MissionId submap_mission_id;
  {
    std::lock_guard<std::mutex> lock(robot_to_mission_id_map_mutex_);
    if (robot_to_mission_id_map_.count(robot_name) == 0u) {
      LOG(WARNING)
          << "[MaplabServerNode] Received map lookup with invalid robot name: "
          << robot_name;
      return MapLookupStatus::kNoSuchMission;
    }

    submap_mission_id = robot_to_mission_id_map_.at(robot_name);
  }

  if (timestamp_ns < 0) {
    LOG(WARNING)
        << "[MaplabServerNode] Received map lookup with invalid timestamp: "
        << timestamp_ns << "ns";
    return MapLookupStatus::kPoseNeverAvailable;
  }

  CHECK(submap_mission_id.isValid());
  {
    vi_map::VIMapManager::MapReadAccess map =
        map_manager_.getMapReadAccess(kMergedMapKey);

    const vi_map::VIMission& mission = map->getMission(submap_mission_id);

    aslam::SensorId sensor_id;
    if (sensor_type == vi_map::SensorType::kNCamera) {
      if (!mission.hasNCamera()) {
        LOG(WARNING) << "[MaplabServerNode] Received map lookup with NCamera "
                     << "sensor, but there is no such sensor in the map!";
        return MapLookupStatus::kNoSuchSensor;
      }
      sensor_id = mission.getNCameraId();
    } else if (sensor_type == vi_map::SensorType::kImu) {
      if (!mission.hasImu()) {
        LOG(WARNING) << "[MaplabServerNode] Received map lookup with IMU "
                     << "sensor, but there is no such sensor in the map!";
        return MapLookupStatus::kNoSuchSensor;
      }
      sensor_id = mission.getImuId();
    } else if (sensor_type == vi_map::SensorType::kLidar) {
      if (!mission.hasLidar()) {
        LOG(WARNING) << "[MaplabServerNode] Received map lookup with Lidar "
                     << "sensor, but there is no such sensor in the map!";
        return MapLookupStatus::kNoSuchSensor;
      }
      sensor_id = mission.getLidarId();
    } else if (sensor_type == vi_map::SensorType::kOdometry6DoF) {
      if (!mission.hasOdometry6DoFSensor()) {
        LOG(WARNING) << "[MaplabServerNode] Received map lookup with Odometry "
                     << "sensor, but there is no such sensor in the map!";
        return MapLookupStatus::kNoSuchSensor;
      }
      sensor_id = mission.getOdometry6DoFSensor();
    } else {
      LOG(WARNING)
          << "[MaplabServerNode] Received map lookup with invalid sensor!";
      return MapLookupStatus::kNoSuchSensor;
    }
    const aslam::Transformation T_B_S =
        map->getSensorManager().getSensor_T_B_S(sensor_id);

    const aslam::Transformation& T_G_M =
        map->getMissionBaseFrameForMission(submap_mission_id).get_T_G_M();

    landmark_triangulation::VertexToTimeStampMap vertex_to_time_map;
    int64_t min_timestamp_ns;
    int64_t max_timestamp_ns;
    const landmark_triangulation::PoseInterpolator pose_interpolator;
    pose_interpolator.getVertexToTimeStampMap(
        *map, submap_mission_id, &vertex_to_time_map, &min_timestamp_ns,
        &max_timestamp_ns);
    if (timestamp_ns < min_timestamp_ns) {
      LOG(WARNING) << "[MaplabServerNode] Received map lookup with timestamp "
                      "that is before the selected robot mission, this "
                      "position will never be available: "
                   << aslam::time::timeNanosecondsToString(timestamp_ns)
                   << " - earliest map time: "
                   << aslam::time::timeNanosecondsToString(min_timestamp_ns);
      return MapLookupStatus::kPoseNeverAvailable;
    } else if (timestamp_ns > max_timestamp_ns) {
      LOG(WARNING) << "[MaplabServerNode] Received map lookup with timestamp "
                      "that is not yet available: "
                   << aslam::time::timeNanosecondsToString(timestamp_ns)
                   << " - most recent map time: "
                   << aslam::time::timeNanosecondsToString(max_timestamp_ns);
      return MapLookupStatus::kPoseNotAvailableYet;
    }

    Eigen::Matrix<int64_t, 1, Eigen::Dynamic> timestamps_ns =
        Eigen::Matrix<int64_t, 1, 1>::Constant(timestamp_ns);

    aslam::TransformationVector T_M_B_vector;
    pose_interpolator.getPosesAtTime(
        *map, submap_mission_id, timestamps_ns, &T_M_B_vector);
    CHECK_EQ(static_cast<int>(T_M_B_vector.size()), timestamps_ns.cols());

    const aslam::Transformation T_G_B = T_G_M * T_M_B_vector[0];
    const aslam::Transformation T_G_S = T_G_B * T_B_S;

    *p_G = T_G_S * p_S;
    *sensor_p_G = T_G_S * Eigen::Vector3d::Zero();
  }
  return MapLookupStatus::kSuccess;
}

}  // namespace maplab
