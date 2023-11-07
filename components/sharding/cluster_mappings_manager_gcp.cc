/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "components/errors/retry.h"
#include "components/sharding/cluster_mappings_manager.h"

namespace kv_server {
namespace {
constexpr char kShardNumberTag[] = "shard-num";
constexpr char kProjectIdParameterName[] = "project-id";
}  // namespace

class GcpClusterMappingsManager : public ClusterMappingsManager {
 public:
  GcpClusterMappingsManager(
      std::string environment, int32_t num_shards,
      privacy_sandbox::server_common::MetricsRecorder& metrics_recorder,
      InstanceClient& instance_client, std::string project_id)
      : ClusterMappingsManager(std::move(environment), num_shards,
                               metrics_recorder, instance_client),
        project_id_{project_id} {}

  std::vector<absl::flat_hash_set<std::string>> GetClusterMappings() override {
    DescribeInstanceGroupInput describe_instance_group_input =
        GcpDescribeInstanceGroupInput{.project_id = project_id_};
    auto instance_group_instances = TraceRetryUntilOk(
        [&instance_client = instance_client_, &describe_instance_group_input] {
          return instance_client.DescribeInstanceGroupInstances(
              describe_instance_group_input);
        },
        "DescribeInstanceGroupInstances", &metrics_recorder_);

    return GroupInstancesToClusterMappings(instance_group_instances);
  }

 private:
  absl::StatusOr<int32_t> GetShardNumberOffLabels(
      const absl::flat_hash_map<std::string, std::string>& labels) const {
    std::smatch match_result;
    const auto key_iter = labels.find(kShardNumberTag);
    if (key_iter == labels.end()) {
      return absl::NotFoundError("Can't find the shard number tag");
    }
    int32_t shard_num;
    if (!absl::SimpleAtoi(key_iter->second, &shard_num)) {
      std::string error =
          absl::StrFormat("Failed converting %s to int32.", key_iter->second);
      return absl::InvalidArgumentError(error);
    }
    return shard_num;
  }

  std::vector<absl::flat_hash_set<std::string>> GroupInstancesToClusterMappings(
      std::vector<InstanceInfo>& instance_group_instances) const {
    std::vector<absl::flat_hash_set<std::string>> cluster_mappings(num_shards_);
    for (const auto& instance : instance_group_instances) {
      if (instance.service_status != InstanceServiceStatus::kInService) {
        continue;
      }

      auto shard_num_status = GetShardNumberOffLabels(instance.labels);
      if (!shard_num_status.ok()) {
        continue;
      }
      int32_t shard_num = *shard_num_status;
      if (shard_num >= num_shards_ || instance.private_ip_address.empty()) {
        continue;
      }
      cluster_mappings[shard_num].insert(instance.private_ip_address);
    }
    return cluster_mappings;
  }

  std::string project_id_;
};

std::unique_ptr<ClusterMappingsManager> ClusterMappingsManager::Create(
    std::string environment, int32_t num_shards,
    privacy_sandbox::server_common::MetricsRecorder& metrics_recorder,
    InstanceClient& instance_client, ParameterFetcher& parameter_fetcher) {
  std::string project_id =
      parameter_fetcher.GetParameter(kProjectIdParameterName);
  return std::make_unique<GcpClusterMappingsManager>(
      environment, num_shards, metrics_recorder, instance_client,
      std::move(project_id));
}

}  // namespace kv_server
