#!/bin/bash

set -euo pipefail

if [ "$#" -lt 1 ]
then
  suite=''
  args=''
else
  suite=$1
  shift 1
  args="$@"
fi

source tests/buildkite/conftest.sh

echo "--- Fetch build artifacts"
buildkite-agent artifact download "python-package/dist/*.whl" . --step build-cuda
buildkite-agent artifact download "build/testxgboost" . --step build-cuda
chmod +x build/testxgboost

# Allocate extra space in /dev/shm to enable NCCL
export CI_DOCKER_EXTRA_PARAMS_INIT='--shm-size=4g'

if [[ -z "${USE_DEPS_DEV_VER-}" ]]
then
  container_tag='gpu'
  rapids_version=${RAPIDS_VERSION}
else
  container_tag='gpu_dev_ver'
  rapids_version=${DEV_RAPIDS_VERSION}
fi

command_wrapper="tests/ci_build/ci_build.sh ${container_tag} --use-gpus --build-arg "`
                `"CUDA_VERSION_ARG=$CUDA_VERSION --build-arg "`
                `"RAPIDS_VERSION_ARG=${rapids_version} --build-arg "`
                `"NCCL_VERSION_ARG=$NCCL_VERSION"

# Run specified test suite
case "$suite" in
  gpu)
    export BUILDKITE_ANALYTICS_TOKEN=$(get_aws_secret buildkite/test_analytics/gpu)
    set_buildkite_env_vars_in_container
    echo "--- Test XGBoost Python package, single GPU"
    $command_wrapper tests/ci_build/test_python.sh $suite
    ;;

  mgpu)
    export BUILDKITE_ANALYTICS_TOKEN=$(get_aws_secret buildkite/test_analytics/mgpu)
    set_buildkite_env_vars_in_container
    echo "--- Test XGBoost Python package, 4 GPUs"
    $command_wrapper tests/ci_build/test_python.sh $suite
    ;;

  *)
    echo "Usage: $0 {gpu|mgpu} [extra args to pass to pytest]"
    exit 1
    ;;
esac
