/*
 * Copyright 2020 Google LLC
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

#ifndef GOOGLE_FHIR_R4_RESOURCE_VALIDATION_H_
#define GOOGLE_FHIR_R4_RESOURCE_VALIDATION_H_

#include "google/protobuf/message.h"
#include "absl/status/status.h"

namespace google {
namespace fhir {
namespace r4 {

::absl::Status ValidateResource(const ::google::protobuf::Message& resource);

::absl::Status ValidateResourceWithFhirPath(const ::google::protobuf::Message& resource);

}  // namespace r4
}  // namespace fhir
}  // namespace google

#endif  // GOOGLE_FHIR_R4_RESOURCE_VALIDATION_H_
