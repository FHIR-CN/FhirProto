#!/bin/bash
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ROOT_PATH=../..
PROTO_GENERATOR=$ROOT_PATH/bazel-bin/java/ProtoGenerator

OUTPUT_PATH="$(dirname $0)/../../proto/r4/core"
DESCRIPTOR_OUTPUT_PATH="$(dirname $0)/../../testdata/r4/descriptors/"

bazel build //spec/fhir_r4_definitions.zip
FHIR_PACKAGE="$ROOT_PATH/bazel-genfiles/spec/fhir_r4_definitions.zip"

COMMON_FLAGS=" \
  --emit_proto \
  --emit_descriptors \
  --sort \
  --r4_core_dep $FHIR_PACKAGE \
  --input_package $FHIR_PACKAGE \
  --descriptor_output_directory $DESCRIPTOR_OUTPUT_PATH "
#
# Build the binary.
bazel build //java:ProtoGenerator

if [ $? -ne 0 ]
then
 echo "Build Failed"
 exit 1;
fi

# generate datatypes.proto
$PROTO_GENERATOR \
  $COMMON_FLAGS \
  --output_directory $OUTPUT_PATH \
  --filter datatype \
  --exclude elementdefinition-de \
  --exclude Reference \
  --exclude Extension \
  --exclude Element

# Some datatypes are manually generated.
# These include:
# * FHIR-defined valueset codes
# * Proto for Reference, which allows more structure than FHIR spec provides.
# * Extension, which has a field order discrepancy between spec and test data.
# TODO: generate Extension proto with custom ordering.
if [ $? -eq 0 ]
then
  echo -e "\n//End of auto-generated messages.\n" >> $OUTPUT_PATH/datatypes.proto
  cat "$(dirname $0)/r4/datatypes_supplement.txt" >> $OUTPUT_PATH/datatypes.proto
fi

# generate resource protos
$PROTO_GENERATOR \
  $COMMON_FLAGS \
  --output_directory $OUTPUT_PATH/resources \
  --filter resource

# generate profiles.proto
# exclude familymemberhistory-genetic due to
# https://gforge.hl7.org/gf/project/fhir/tracker/?action=TrackerItemEdit&tracker_id=677&tracker_item_id=19239
$PROTO_GENERATOR \
  $COMMON_FLAGS \
  --output_directory $OUTPUT_PATH/profiles \
  --filter profile \
  --exclude familymemberhistory-genetic


# generate extensions
$PROTO_GENERATOR \
  $COMMON_FLAGS \
  --output_directory $OUTPUT_PATH \
  --filter extension
