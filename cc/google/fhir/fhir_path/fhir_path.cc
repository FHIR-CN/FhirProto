// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/fhir/fhir_path/fhir_path.h"

#include <utility>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/util/message_differencer.h"
#include "absl/memory/memory.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "google/fhir/annotations.h"
#include "google/fhir/fhir_path/FhirPathBaseVisitor.h"
#include "google/fhir/fhir_path/FhirPathLexer.h"
#include "google/fhir/fhir_path/FhirPathParser.h"
#include "google/fhir/fhir_path/utils.h"
#include "google/fhir/primitive_wrapper.h"
#include "google/fhir/proto_util.h"
#include "google/fhir/r4/primitive_handler.h"
#include "google/fhir/status/status.h"
#include "google/fhir/status/statusor.h"
#include "google/fhir/stu3/primitive_handler.h"
#include "google/fhir/util.h"
#include "proto/annotations.pb.h"
#include "proto/r4/core/datatypes.pb.h"
#include "tensorflow/core/lib/core/errors.h"

namespace google {
namespace fhir {
namespace fhir_path {

using ::google::protobuf::Descriptor;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::MessageOptions;
using ::google::protobuf::util::MessageDifferencer;

// Used to wrap primitives in protobuf messages, and
// can be used against multiple versions of FHIR, not just R4.
using ::google::fhir::r4::core::Boolean;
using ::google::fhir::r4::core::DateTime;
using ::google::fhir::r4::core::Decimal;
using ::google::fhir::r4::core::Integer;
using ::google::fhir::r4::core::SimpleQuantity;
using ::google::fhir::r4::core::String;
using ::google::fhir::r4::core::UnsignedInt;

using antlr4::ANTLRInputStream;
using antlr4::BaseErrorListener;
using antlr4::CommonTokenStream;
using antlr4::tree::TerminalNode;

using antlr_parser::FhirPathBaseVisitor;
using antlr_parser::FhirPathLexer;
using antlr_parser::FhirPathParser;

using ::google::fhir::AreSameMessageType;
using ::google::fhir::ForEachMessageHalting;
using ::google::fhir::IsMessageType;
using ::google::fhir::JsonPrimitive;
using ::google::fhir::StatusOr;

using internal::ExpressionNode;

using ::tensorflow::errors::InvalidArgument;


namespace internal {

// TODO: This method forces linking in all supported versions of
// FHIR. It should be replaced by a passed-in PrimitiveHandler.
StatusOr<const PrimitiveHandler*> GetPrimitiveHandler(const Message& message) {
  const auto version = GetFhirVersion(message);
  switch (version) {
    case proto::FhirVersion::STU3:
      return stu3::Stu3PrimitiveHandler::GetInstance();
    case proto::FhirVersion::R4:
      return r4::R4PrimitiveHandler::GetInstance();
    default:
      return InvalidArgument("Invalid FHIR version for FhirPath: ",
                             FhirVersion_Name(version));
  }
}

// Returns true if the collection of messages represents
// a boolean value per FHIRPath conventions; that is it
// has exactly one item that is boolean.
bool IsSingleBoolean(const std::vector<const Message*>& messages) {
  return messages.size() == 1 && IsMessageType<Boolean>(*messages[0]);
}

// Returns success with a boolean value if the message collection
// represents a single boolean, or a failure status otherwise.
StatusOr<bool> MessagesToBoolean(const std::vector<const Message*>& messages) {
  if (IsSingleBoolean(messages)) {
    return dynamic_cast<const Boolean*>(messages[0])->value();
  }

  return InvalidArgument("Expression did not evaluate to boolean");
}

template <class T, class M>
StatusOr<absl::optional<T>> PrimitiveOrEmpty(
    const std::vector<const Message*>& messages) {
  if (messages.empty()) {
    return absl::optional<T>();
  }

  if (messages.size() > 1 || !IsPrimitive(messages[0]->GetDescriptor())) {
    return InvalidArgument(
        "Expression must be empty or represent a single primitive value.");
  }

  if (!IsMessageType<M>(*messages[0])) {
    return InvalidArgument("Single value expression of wrong type.");
  }

  return absl::optional<T>(dynamic_cast<const M*>(messages[0])->value());
}

// Returns the string representation of the provided message for messages that
// are represented in JSON as strings. For primitive messages that are not
// represented as a string in JSON a status other than OK will be returned.
StatusOr<std::string> MessageToString(const Message& message) {
  if (IsMessageType<String>(message)) {
    return dynamic_cast<const String*>(&message)->value();
  }

  if (!IsPrimitive(message.GetDescriptor())) {
    return InvalidArgument("Expression must be a primitive.");
  }

  FHIR_ASSIGN_OR_RETURN(const PrimitiveHandler* handler,
                        GetPrimitiveHandler(message));
  StatusOr<JsonPrimitive> json_primitive = handler->WrapPrimitiveProto(message);
  std::string json_string = json_primitive.ValueOrDie().value;

  if (!absl::StartsWith(json_string, "\"")) {
    return InvalidArgument("Expression must evaluate to a string.");
  }

  // Trim the starting and ending double quotation marks from the string (added
  // by JsonPrimitive.)
  return json_string.substr(1, json_string.size() - 2);
}

// Returns the string representation of the provided message for messages that
// are represented in JSON as strings. Requires the presence of exactly one
// message in the provided collection. For primitive messages that are not
// represented as a string in JSON a status other than OK will be returned.
StatusOr<std::string> MessagesToString(
    const std::vector<const Message*>& messages) {
  if (messages.size() != 1) {
    return InvalidArgument("Expression must represent a single value.");
  }

  return MessageToString(*messages[0]);
}

// Finds a field in the message descriptor whose JSON name matches the provided
// name or nullptr if one is not found.
//
// Neither Descriptor::FindFieldByName or Descriptor::FindFieldByCamelcaseName
// will suffice as some FHIR fields are renamed in the FHIR protos (e.g.
// "assert" becomes "assert_value" and "class" becomes "class_value").
const FieldDescriptor* FindFieldByJsonName(const Descriptor* descriptor,
                                           absl::string_view json_name) {
  for (int i = 0; i < descriptor->field_count(); ++i) {
    if (json_name == descriptor->field(i)->json_name()) {
      return descriptor->field(i);
    }
  }
  return nullptr;
}

// Expression node that returns literals wrapped in the corresponding
// protbuf wrapper
template <typename ProtoType, typename PrimitiveType>
class Literal : public ExpressionNode {
 public:
  explicit Literal(PrimitiveType value) : value_(value) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    auto value = new ProtoType();
    value->set_value(value_);
    work_space->DeleteWhenFinished(value);
    results->push_back(value);

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return ProtoType::descriptor();
  }

 private:
  const PrimitiveType value_;
};

// Expression node for the empty literal.
class EmptyLiteral : public ExpressionNode {
 public:
  EmptyLiteral() {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    return Status::OK();
  }

  // The return type of the empty literal is undefined. If this causes problems,
  // it is likely we could arbitrarily pick one of the primitive types without
  // ill-effect.
  const Descriptor* ReturnType() const override {
    return nullptr;
  }
};

// Implements the InvocationTerm from the FHIRPath grammar,
// producing a term from the root context message.
class InvokeTermNode : public ExpressionNode {
 public:
  explicit InvokeTermNode(const FieldDescriptor* field,
                          const std::string& field_name)
      : field_(field), field_name_(field_name) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    const Message& message = *work_space->MessageContext();
    const FieldDescriptor* field =
        field_ != nullptr
            ? field_
            : FindFieldByJsonName(message.GetDescriptor(), field_name_);

    // If the field cannot be found an empty collection is returned. This
    // matches the behavior of https://github.com/HL7/fhirpath.js and is
    // empirically necessitated by expressions such as "children().element"
    // where not every child necessarily has an "element" field (see FHIRPath
    // constraints on Bundle for a full example.)
    if (field == nullptr) {
      return Status::OK();
    }

    return RetrieveField(message, *field, results);
  }

  const Descriptor* ReturnType() const override {
    return field_ != nullptr ? field_->message_type() : nullptr;
  }

 private:
  const FieldDescriptor* field_;
  const std::string field_name_;
};

// Handles the InvocationExpression from the FHIRPath grammar,
// which can be a member of function called on the results of
// another expression.
class InvokeExpressionNode : public ExpressionNode {
 public:
  InvokeExpressionNode(std::shared_ptr<ExpressionNode> child_expression,
                       const FieldDescriptor* field,
                       const std::string& field_name)
      : child_expression_(std::move(child_expression)),
        field_(field),
        field_name_(field_name) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;

    FHIR_RETURN_IF_ERROR(
        child_expression_->Evaluate(work_space, &child_results));

    // Iterate through the results of the child expression and invoke
    // the appropriate field.
    for (const Message* child_message : child_results) {
      // In the case where the field descriptor was not known at compile time
      // (because ExpressionNode.ReturnType() currently doesn't support
      // collections with mixed types) we attempt to find it at evaluation time.
      const FieldDescriptor* field =
          field_ != nullptr ? field_
                            : FindFieldByJsonName(
              child_message->GetDescriptor(), field_name_);

      // If the field cannot be found the result is an empty collection. This
      // matches the behavior of https://github.com/HL7/fhirpath.js and is
      // empirically necessitated by expressions such as "children().element"
      // where not every child necessarily has an "element" field (see FHIRPath
      // constraints on Bundle for a full example.)
      if (field != nullptr) {
        FHIR_RETURN_IF_ERROR(RetrieveField(*child_message, *field, results));
      }
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return field_ != nullptr ? field_->message_type() : nullptr;
  }

 private:
  const std::shared_ptr<ExpressionNode> child_expression_;
  // Null if the child_expression_ may evaluate to a collection that contains
  // multiple types.
  const FieldDescriptor* field_;
  const std::string field_name_;
};

class FunctionNode : public ExpressionNode {
 public:
  template <class T>
  StatusOr<T*> static Create(
      const std::shared_ptr<ExpressionNode>& child_expression,
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* base_context_visitor,
      FhirPathBaseVisitor* child_context_visitor) {
    FHIR_ASSIGN_OR_RETURN(
        std::vector<std::shared_ptr<ExpressionNode>> compiled_params,
        T::CompileParams(params, base_context_visitor, child_context_visitor));
    FHIR_RETURN_IF_ERROR(T::ValidateParams(compiled_params));
    return new T(child_expression, compiled_params);
  }

  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* base_context_visitor,
      FhirPathBaseVisitor*) {
    return CompileParams(params, base_context_visitor);
  }

  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* visitor) {
    std::vector<std::shared_ptr<ExpressionNode>> compiled_params;

    for (auto it = params.begin(); it != params.end(); ++it) {
      antlrcpp::Any param_any = (*it)->accept(visitor);
      if (param_any.isNull()) {
        return InvalidArgument("Failed to compile parameter.");
      }
      compiled_params.push_back(
          param_any.as<std::shared_ptr<ExpressionNode>>());
    }

    return compiled_params;
  }

  // This is the default implementation. FunctionNodes's that need to validate
  // params at compile time should overwrite this definition with their own.
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    return Status::OK();
  }

 protected:
  FunctionNode(const std::shared_ptr<ExpressionNode>& child,
               const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : child_(child), params_(params) {}

  const std::shared_ptr<ExpressionNode> child_;
  const std::vector<std::shared_ptr<ExpressionNode>> params_;
};

class ZeroParameterFunctionNode : public FunctionNode {
 public:
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    if (!params.empty()) {
      return InvalidArgument("Function does not accept any arguments.");
    }

    return Status::OK();
  }

 protected:
  ZeroParameterFunctionNode(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }
};

class SingleParameterFunctionNode : public FunctionNode {
 private:
  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    //  requires a single parameter
    if (params_.size() != 1) {
      return InvalidArgument("this function requires a single parameter.");
    }

    std::vector<const Message*> first_param;
    FHIR_RETURN_IF_ERROR(params_[0]->Evaluate(work_space, &first_param));

    return Evaluate(work_space, first_param, results);
  }

 public:
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    if (params.size() != 1) {
      return InvalidArgument("Function requires exactly one argument.");
    }

    return Status::OK();
  }

 protected:
  SingleParameterFunctionNode(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }

  virtual Status Evaluate(WorkSpace* work_space,
                          std::vector<const Message*>& first_param,
                          std::vector<const Message*>* results) const = 0;
};

class SingleValueFunctionNode : public SingleParameterFunctionNode {
 private:
  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>& first_param,
                  std::vector<const Message*>* results) const override {
    //  requires a single parameter
    if (first_param.size() != 1) {
      return InvalidArgument(
          "this function requires a single value parameter.");
    }

    return EvaluateWithParam(work_space, *first_param[0], results);
  }

 protected:
  SingleValueFunctionNode(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleParameterFunctionNode(child, params) {}

  virtual Status EvaluateWithParam(
      WorkSpace* work_space, const Message& param,
      std::vector<const Message*>* results) const = 0;
};

// Implements the FHIRPath .exists() function
class ExistsFunction : public ZeroParameterFunctionNode {
 public:
  ExistsFunction(const std::shared_ptr<ExpressionNode>& child,
                 const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(!child_results.empty());
    results->push_back(result);

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

// Implements the FHIRPath .not() function.
class NotFunction : public ZeroParameterFunctionNode {
 public:
  explicit NotFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params = {})
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;

    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    // Per the FHIRPath spec, boolean operations on empty collection
    // propagate the empty collection.
    if (child_results.empty()) {
      return Status::OK();
    }

    // Per the FHIR spec, the not() function produces a value
    // IFF it is given a boolean input, and returns an empty result
    // otherwise.
    if (IsSingleBoolean(child_results)) {
      FHIR_ASSIGN_OR_RETURN(bool child_result,
                            MessagesToBoolean(child_results));

      Boolean* result = new Boolean();
      work_space->DeleteWhenFinished(result);
      result->set_value(!child_result);

      results->push_back(result);
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

// Implements the FHIRPath .hasValue() function, which returns true
// if and only if the child is a single primitive value.
class HasValueFunction : public ZeroParameterFunctionNode {
 public:
  HasValueFunction(const std::shared_ptr<ExpressionNode>& child,
                   const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;

    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);

    if (child_results.size() != 1) {
      result->set_value(false);
    } else {
      const MessageOptions& options =
          child_results[0]->GetDescriptor()->options();
      result->set_value(
          options.HasExtension(proto::structure_definition_kind) &&
          (options.GetExtension(proto::structure_definition_kind) ==
           proto::StructureDefinitionKindValue::KIND_PRIMITIVE_TYPE));
    }

    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

// Implements the FHIRPath .startsWith() function, which returns true if and
// only if the child string starts with the given string. When the given string
// is the empty string .startsWith() returns true.
//
// Missing or incorrect parameters will end evaluation and cause Evaluate to
// return a status other than OK. See
// http://hl7.org/fhirpath/2018Sep/index.html#functions-2.
//
// Please note that execution will proceed on any String-like type.
// Specifically, any type for which its JsonPrimitive value is a string. This
// differs from the allowed implicit conversions defined in
// https://hl7.org/fhirpath/2018Sep/index.html#conversion.
class StartsWithFunction : public SingleValueFunctionNode {
 public:
  explicit StartsWithFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleValueFunctionNode(child, params) {}

  Status EvaluateWithParam(WorkSpace* work_space, const Message& param,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() != 1) {
      return InvalidArgument(kInvalidArgumentMessage);
    }

    FHIR_ASSIGN_OR_RETURN(std::string item, MessagesToString(child_results));
    FHIR_ASSIGN_OR_RETURN(std::string prefix, MessageToString(param));

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(absl::StartsWith(item, prefix));
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }

 private:
  static constexpr char kInvalidArgumentMessage[] =
      "startsWith must be invoked on a string with a single string "
      "argument";
};
constexpr char StartsWithFunction::kInvalidArgumentMessage[];

// Implements the FHIRPath .contains() function.
class ContainsFunction : public SingleValueFunctionNode {
 public:
  explicit ContainsFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleValueFunctionNode(child, params) {}

  Status EvaluateWithParam(
      WorkSpace* work_space, const Message& param,
      std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.empty()) {
      return Status::OK();
    }

    if (child_results.size() > 1) {
      return InvalidArgument("contains() must be invoked on a single string.");
    }

    FHIR_ASSIGN_OR_RETURN(std::string haystack,
                          MessagesToString(child_results));
    FHIR_ASSIGN_OR_RETURN(std::string needle, MessageToString(param));

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(absl::StrContains(haystack, needle));
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

class MatchesFunction : public SingleValueFunctionNode {
 public:
  explicit MatchesFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleValueFunctionNode(child, params) {}

  Status EvaluateWithParam(WorkSpace* work_space, const Message& param,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.empty()) {
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(std::string item, MessagesToString(child_results));
    FHIR_ASSIGN_OR_RETURN(std::string re_string, MessageToString(param));

    RE2 re(re_string);

    if (!re.ok()) {
      return InvalidArgument(
          absl::StrCat("Unable to parse regular expression, '", re_string,
                       "'. ", re.error()));
    }

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(RE2::FullMatch(item, re));
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

class ToStringFunction : public ZeroParameterFunctionNode {
 public:
  ToStringFunction(const std::shared_ptr<ExpressionNode>& child,
                   const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      return InvalidArgument(
          "Input collection must not contain multiple items");
    }

    if (child_results.empty()) {
      return Status::OK();
    }

    const Message* child = child_results[0];

    if (IsMessageType<String>(*child)) {
      results->push_back(child);
      return Status::OK();
    }

    if (!IsPrimitive(child->GetDescriptor())) {
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(const PrimitiveHandler* handler,
                          GetPrimitiveHandler(*child));
    FHIR_ASSIGN_OR_RETURN(JsonPrimitive json_primitive,
                          handler->WrapPrimitiveProto(*child));
    std::string json_string = json_primitive.value;

    if (absl::StartsWith(json_string, "\"")) {
      json_string = json_string.substr(1, json_string.size() - 2);
    }

    String* result = new String();
    work_space->DeleteWhenFinished(result);
    result->set_value(json_string);
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return String::descriptor(); }
};

// Implements the FHIRPath .length() function.
class LengthFunction : public ZeroParameterFunctionNode {
 public:
  LengthFunction(const std::shared_ptr<ExpressionNode>& child,
                 const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.empty()) {
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(std::string item, MessagesToString(child_results));

    Integer* result = new Integer();
    work_space->DeleteWhenFinished(result);
    result->set_value(item.length());
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Integer::descriptor();
  }
};

// Implements the FHIRPath .empty() function.
//
// Returns true if the input collection is empty and false otherwise.
class EmptyFunction : public ZeroParameterFunctionNode {
 public:
  EmptyFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(child_results.empty());
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

// Implements the FHIRPath .count() function.
//
// Returns the size of the input collection as an integer.
class CountFunction : public ZeroParameterFunctionNode {
 public:
  CountFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    Integer* result = new Integer();
    work_space->DeleteWhenFinished(result);
    result->set_value(child_results.size());
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Integer::descriptor();
  }
};

// Implements the FHIRPath .first() function.
//
// Returns the first element of the input collection. Or an empty collection if
// if the input collection is empty.
class FirstFunction : public ZeroParameterFunctionNode {
 public:
  FirstFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (!child_results.empty()) {
      results->push_back(child_results[0]);
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return child_->ReturnType();
  }
};

// Implements the FHIRPath .tail() function.
class TailFunction : public ZeroParameterFunctionNode {
 public:
  TailFunction(const std::shared_ptr<ExpressionNode>& child,
               const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      results->insert(results->begin(), child_results.begin() + 1,
                      child_results.end());
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return child_->ReturnType(); }
};

// Implements the FHIRPath .trace() function.
class TraceFunction : public SingleValueFunctionNode {
 public:
  TraceFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleValueFunctionNode(child, params) {}

  Status EvaluateWithParam(WorkSpace* work_space, const Message& param,
                  std::vector<const Message*>* results) const override {
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, results));
    FHIR_ASSIGN_OR_RETURN(std::string name, MessageToString(param));

    LOG(INFO) << "trace(" << name << "):";
    for (auto it = results->begin(); it != results->end(); it++) {
      LOG(INFO) << (*it)->DebugString();
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return child_->ReturnType();
  }
};

// Implements the FHIRPath .toInteger() function.
class ToIntegerFunction : public ZeroParameterFunctionNode {
 public:
  ToIntegerFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      return InvalidArgument(
          "toInterger() requires a collection with no more than 1 item.");
    }

    if (child_results.empty()) {
      return Status::OK();
    }

    const Message* child_result = child_results[0];

    if (!IsPrimitive(child_result->GetDescriptor())) {
      return Status::OK();
    }

    if (IsMessageType<Integer>(*child_result)) {
      results->push_back(child_result);
      return Status::OK();
    }

    if (IsMessageType<Boolean>(*child_result)) {
      Integer* result = new Integer();
      work_space->DeleteWhenFinished(result);
      result->set_value(dynamic_cast<const Boolean*>(child_result)->value());
      results->push_back(result);
      return Status::OK();
    }

    auto child_as_string = MessagesToString(child_results);
    if (child_as_string.ok()) {
      int32_t value;
      if (absl::SimpleAtoi(child_as_string.ValueOrDie(), &value)) {
        Integer* result = new Integer();
        work_space->DeleteWhenFinished(result);
        result->set_value(value);
        results->push_back(result);
        return Status::OK();
      }
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Integer::descriptor();
  }
};

// Base class for FHIRPath binary operators.
class BinaryOperator : public ExpressionNode {
 public:
  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> left_results;
    FHIR_RETURN_IF_ERROR(left_->Evaluate(work_space, &left_results));

    std::vector<const Message*> right_results;
    FHIR_RETURN_IF_ERROR(right_->Evaluate(work_space, &right_results));

    return EvaluateOperator(left_results, right_results, work_space, results);
  }

  BinaryOperator(std::shared_ptr<ExpressionNode> left,
                 std::shared_ptr<ExpressionNode> right)
      : left_(left), right_(right) {}

  // Perform the actual boolean evaluation.
  virtual Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const = 0;

 protected:
  const std::shared_ptr<ExpressionNode> left_;

  const std::shared_ptr<ExpressionNode> right_;
};

class IndexerExpression : public BinaryOperator {
 public:
  IndexerExpression(std::shared_ptr<ExpressionNode> left,
                    std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(left, right) {}

  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    FHIR_ASSIGN_OR_RETURN(auto index,
                          (PrimitiveOrEmpty<int, Integer>(right_results)));
    if (!index.has_value()) {
      return InvalidArgument("Index must be present.");
    }

    if (left_results.empty() || left_results.size() <= index.value()) {
      return Status::OK();
    }

    out_results->push_back(left_results[index.value()]);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return left_->ReturnType(); }
};

class EqualsOperator : public BinaryOperator {
 public:
  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    if (left_results.empty() || right_results.empty()) {
      return Status::OK();
    }

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);

    if (left_results.size() != right_results.size()) {
      result->set_value(false);
    } else {
      // Scan for unequal messages.
      result->set_value(true);
      for (int i = 0; i < left_results.size(); ++i) {
        const Message* left = left_results.at(i);
        const Message* right = right_results.at(i);
        result->set_value(AreEqual(*left, *right));
      }
    }

    out_results->push_back(result);
    return Status::OK();
  }

  static bool AreEqual(const Message& left, const Message& right) {
    if (AreSameMessageType(left, right)) {
      return MessageDifferencer::Equals(left, right);
    } else {
      // TODO: This will crash on a non-STU3 or R4 primitive.
      // That's probably ok for now but we should fix this to never crash ASAP.
      const PrimitiveHandler* left_handler =
          GetPrimitiveHandler(left).ValueOrDie();
      const PrimitiveHandler* right_handler =
          GetPrimitiveHandler(right).ValueOrDie();

      // When dealing with different types we might be comparing a
      // primitive type (like an enum) to a literal string, which is
      // supported. Therefore we simply convert both to string form
      // and consider them unequal if either is not a string.
      StatusOr<JsonPrimitive> left_primitive =
          left_handler->WrapPrimitiveProto(left);
      StatusOr<JsonPrimitive> right_primitive =
          right_handler->WrapPrimitiveProto(right);

      // Comparisons between primitives and non-primitives are valid
      // in FHIRPath and should simply return false rather than an error.
      return left_primitive.ok() && right_primitive.ok() &&
             left_primitive.ValueOrDie().value ==
                 right_primitive.ValueOrDie().value;
    }
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }

  EqualsOperator(std::shared_ptr<ExpressionNode> left,
                 std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(left, right) {}
};

struct ProtoPtrSameTypeAndEqual {
  bool operator()(const google::protobuf::Message* lhs,
                  const google::protobuf::Message* rhs) const {
    return (lhs == rhs) || ((lhs != nullptr && rhs != nullptr) &&
                            EqualsOperator::AreEqual(*lhs, *rhs));
  }
};

struct ProtoPtrHash {
  size_t operator()(const google::protobuf::Message* message) const {
    const StatusOr<const PrimitiveHandler*> handler =
        GetPrimitiveHandler(*message);
    if (message == nullptr) {
      return 0;
    }

    // TODO: This will crash on a non-STU3 or R4 primitive.
    // That's probably ok for now but we should fix this to never crash ASAP.
    if (IsPrimitive(message->GetDescriptor())) {
      return std::hash<std::string>{}(handler.ValueOrDie()
                                          ->WrapPrimitiveProto(*message)
                                          .ValueOrDie()
                                          .value);
    }

    return std::hash<std::string>{}(message->SerializeAsString());
  }
};

class UnionOperator : public BinaryOperator {
 public:
  UnionOperator(std::shared_ptr<ExpressionNode> left,
                std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(std::move(left), std::move(right)) {}

  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    std::unordered_set<const Message*, ProtoPtrHash, ProtoPtrSameTypeAndEqual>
        results;
    results.insert(left_results.begin(), left_results.end());
    results.insert(right_results.begin(), right_results.end());
    out_results->insert(out_results->begin(), results.begin(), results.end());
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    // If the return type of one of the operands is unknown, the return type of
    // the union operator is unknown.
    if (left_->ReturnType() == nullptr || right_->ReturnType() == nullptr) {
      return nullptr;
    }

    if (AreSameMessageType(left_->ReturnType(), right_->ReturnType())) {
      return left_->ReturnType();
    }

    // TODO: Consider refactoring ReturnType to return a set of all types
    // in the collection.
    return nullptr;
  }
};

// Implements the FHIRPath .isDistinct() function.
class IsDistinctFunction : public ZeroParameterFunctionNode {
 public:
  IsDistinctFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    std::unordered_set<const Message*, ProtoPtrHash, ProtoPtrSameTypeAndEqual>
        child_results_set;
    child_results_set.insert(child_results.begin(), child_results.end());


    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(child_results_set.size() == child_results.size());
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::GetDescriptor();
  }
};

// Implements the FHIRPath .distinct() function.
class DistinctFunction : public ZeroParameterFunctionNode {
 public:
  DistinctFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    std::unordered_set<const Message*, ProtoPtrHash, ProtoPtrSameTypeAndEqual>
        result_set;
    result_set.insert(child_results.begin(), child_results.end());
    results->insert(results->begin(), result_set.begin(), result_set.end());
    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return child_->ReturnType(); }
};

// Implements the FHIRPath .combine() function.
class CombineFunction : public SingleParameterFunctionNode {
 public:
  CombineFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>& first_param,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    results->insert(results->end(), child_results.begin(), child_results.end());
    results->insert(results->end(), first_param.begin(), first_param.end());
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    DCHECK_EQ(params_.size(), 1);

    if (AreSameMessageType(child_->ReturnType(), params_[0]->ReturnType())) {
      return child_->ReturnType();
    }

    // TODO: Consider refactoring ReturnType to return a set of all types
    // in the collection.
    return nullptr;
  }
};

// Implements the FHIRPath .where() function.
class WhereFunction : public FunctionNode {
 public:
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    if (params.size() != 1) {
      return InvalidArgument("Function requires exactly one argument.");
    }

    return Status::OK();
  }

  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor*,
      FhirPathBaseVisitor* child_context_visitor) {
    return FunctionNode::CompileParams(params, child_context_visitor);
  }

  WhereFunction(const std::shared_ptr<ExpressionNode>& child,
                const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    for (const Message* message : child_results) {
      std::vector<const Message*> param_results;
      WorkSpace expression_work_space(work_space->MessageContextStack(),
                                      message);
      FHIR_RETURN_IF_ERROR(
          params_[0]->Evaluate(&expression_work_space, &param_results));
      FHIR_ASSIGN_OR_RETURN(StatusOr<absl::optional<bool>> allowed,
                            (PrimitiveOrEmpty<bool, Boolean>(param_results)));
      if (allowed.ValueOrDie().value_or(false)) {
        results->push_back(message);
      }
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return child_->ReturnType(); }
};

// Implements the FHIRPath .all() function.
class AllFunction : public FunctionNode {
 public:
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    if (params.size() != 1) {
      return InvalidArgument("Function requires exactly one argument.");
    }

    return Status::OK();
  }

  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor*,
      FhirPathBaseVisitor* child_context_visitor) {
    return FunctionNode::CompileParams(params, child_context_visitor);
  }

  AllFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));
    FHIR_ASSIGN_OR_RETURN(bool result, Evaluate(work_space, child_results));

    Boolean* result_message = new Boolean();
    work_space->DeleteWhenFinished(result_message);
    result_message->set_value(result);
    results->push_back(result_message);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::GetDescriptor();
  }

 private:
  StatusOr<bool> Evaluate(
      WorkSpace* work_space,
      const std::vector<const Message*>& child_results) const {
    for (const Message* message : child_results) {
      std::vector<const Message*> param_results;
      WorkSpace expression_work_space(work_space->MessageContextStack(),
                                      message);
      FHIR_RETURN_IF_ERROR(
          params_[0]->Evaluate(&expression_work_space, &param_results));
      FHIR_ASSIGN_OR_RETURN(StatusOr<absl::optional<bool>> criteria_met,
                            (PrimitiveOrEmpty<bool, Boolean>(param_results)));
      if (!criteria_met.ValueOrDie().value_or(false)) {
        return false;
      }
    }

    return true;
  }
};

// Implements the FHIRPath .select() function.
class SelectFunction : public FunctionNode {
 public:
  static Status ValidateParams(
      const std::vector<std::shared_ptr<ExpressionNode>>& params) {
    if (params.size() != 1) {
      return InvalidArgument("Function requires exactly one argument.");
    }

    return Status::OK();
  }

  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor*,
      FhirPathBaseVisitor* child_context_visitor) {
    return FunctionNode::CompileParams(params, child_context_visitor);
  }

  SelectFunction(const std::shared_ptr<ExpressionNode>& child,
                 const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    for (const Message* message : child_results) {
      work_space->PushMessageContext(message);
      Status status = params_[0]->Evaluate(work_space, results);
      work_space->PopMessageContext();
      FHIR_RETURN_IF_ERROR(status);
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return params_[0]->ReturnType();
  }
};

// Implements the FHIRPath .iif() function.
class IifFunction : public FunctionNode {
 public:
  static StatusOr<std::vector<std::shared_ptr<ExpressionNode>>> CompileParams(
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* base_context_visitor,
      FhirPathBaseVisitor* child_context_visitor) {
    if (params.size() < 2 || params.size() > 3) {
      return InvalidArgument("iif() requires 2 or 3 arugments.");
    }

    std::vector<std::shared_ptr<ExpressionNode>> compiled_params;

    antlrcpp::Any criterion = params[0]->accept(child_context_visitor);
    if (criterion.isNull()) {
      return InvalidArgument("Failed to compile parameter.");
    }
    compiled_params.push_back(criterion.as<std::shared_ptr<ExpressionNode>>());

    antlrcpp::Any true_result = params[1]->accept(base_context_visitor);
    if (true_result.isNull()) {
      return InvalidArgument("Failed to compile parameter.");
    }
    compiled_params.push_back(
        true_result.as<std::shared_ptr<ExpressionNode>>());

    if (params.size() > 2) {
      antlrcpp::Any otherwise_result = params[2]->accept(base_context_visitor);
      if (otherwise_result.isNull()) {
        return InvalidArgument("Failed to compile parameter.");
      }
      compiled_params.push_back(
          otherwise_result.as<std::shared_ptr<ExpressionNode>>());
    }

    return compiled_params;
  }

  IifFunction(const std::shared_ptr<ExpressionNode>& child,
              const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : FunctionNode(child, params) {
    TF_DCHECK_OK(ValidateParams(params));
  }

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      return InvalidArgument(
          "iif() requires a collection with no more than 1 item.");
    }

    if (child_results.empty()) {
      return Status::OK();
    }

    const Message* child = child_results[0];

    std::vector<const Message*> param_results;
    WorkSpace expression_work_space(work_space->MessageContextStack(), child);
    FHIR_RETURN_IF_ERROR(
        params_[0]->Evaluate(&expression_work_space, &param_results));
    FHIR_ASSIGN_OR_RETURN(StatusOr<absl::optional<bool>> criterion_met,
                          (PrimitiveOrEmpty<bool, Boolean>(param_results)));
    if (criterion_met.ValueOrDie().value_or(false)) {
      FHIR_RETURN_IF_ERROR(params_[1]->Evaluate(work_space, results));
    } else if (params_.size() > 2) {
      FHIR_RETURN_IF_ERROR(params_[2]->Evaluate(work_space, results));
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return child_->ReturnType(); }
};

// Implements the FHIRPath .is() function.
//
// TODO: This does not currently validate that the tested type exists.
// According to the FHIRPath spec, if the type does not exist the expression
// should throw an error instead of returning false.
//
// TODO: Handle type namespaces (i.e. FHIR.* and System.*)
//
// TODO: Handle type inheritance correctly. For example, a Patient
// resource is a DomainResource, but this function, as is, will return false.
class IsFunction : public ExpressionNode {
 public:
  StatusOr<IsFunction*> static Create(
      const std::shared_ptr<ExpressionNode>& child_expression,
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* base_context_visitor,
      FhirPathBaseVisitor* child_context_visitor) {
    if (params.size() != 1) {
      return InvalidArgument("is() requires a single argument.");
    }

    return new IsFunction(child_expression, params[0]->getText());
  }

  IsFunction(const std::shared_ptr<ExpressionNode>& child,
             std::string type_name)
      : child_(child), type_name_(type_name) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      return InvalidArgument(
          "is() requires a collection with no more than 1 item.");
    }

    if (child_results.empty()) {
      return Status::OK();
    }

    auto result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(absl::EqualsIgnoreCase(
        child_results[0]->GetDescriptor()->name(), type_name_));
    results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::GetDescriptor();
  }

 private:
  const std::shared_ptr<ExpressionNode> child_;
  const std::string type_name_;
};

// Implements the FHIRPath .as() function.
//
// TODO: This does not currently validate that the tested type exists.
// According to the FHIRPath spec, if the type does not exist the expression
// should throw an error.
//
// TODO: Handle type namespaces (i.e. FHIR.* and System.*)
//
// TODO: Handle type inheritance correctly. For example, a Patient
// resource is a DomainResource, but this function, as is, will behave as if
// a Patient is not a DomainResource and return an empty collection.
class AsFunction : public ExpressionNode {
 public:
  StatusOr<AsFunction*> static Create(
      const std::shared_ptr<ExpressionNode>& child_expression,
      const std::vector<FhirPathParser::ExpressionContext*>& params,
      FhirPathBaseVisitor* base_context_visitor,
      FhirPathBaseVisitor* child_context_visitor) {
    if (params.size() != 1) {
      return InvalidArgument("as() requires a single argument.");
    }

    return new AsFunction(child_expression, params[0]->getText());
  }

  AsFunction(const std::shared_ptr<ExpressionNode>& child,
             std::string type_name)
      : child_(child), type_name_(type_name) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    if (child_results.size() > 1) {
      return InvalidArgument(
          "as() requires a collection with no more than 1 item.");
    }

    if (!child_results.empty() &&
        absl::EqualsIgnoreCase(child_results[0]->GetDescriptor()->name(),
                               type_name_)) {
      results->push_back(child_results[0]);
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    // TODO: Fetch the descriptor based on this->type_name_.
    return nullptr;
  }

 private:
  const std::shared_ptr<ExpressionNode> child_;
  const std::string type_name_;
};

class ChildrenFunction : public ZeroParameterFunctionNode {
 public:
  ChildrenFunction(const std::shared_ptr<ExpressionNode>& child,
                 const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : ZeroParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    for (const Message* child : child_results) {
      const Descriptor* descriptor = child->GetDescriptor();
      for (int i = 0; i < descriptor->field_count(); i++) {
        FHIR_RETURN_IF_ERROR(
            RetrieveField(*child, *descriptor->field(i), results));
      }
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return nullptr;
  }
};

// Implements the FHIRPath .intersect() function.
class IntersectFunction : public SingleParameterFunctionNode {
 public:
  IntersectFunction(
      const std::shared_ptr<ExpressionNode>& child,
      const std::vector<std::shared_ptr<ExpressionNode>>& params)
      : SingleParameterFunctionNode(child, params) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>& first_param,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> child_results;
    FHIR_RETURN_IF_ERROR(child_->Evaluate(work_space, &child_results));

    std::unordered_set<const Message*, ProtoPtrHash, ProtoPtrSameTypeAndEqual>
        child_set;
    child_set.insert(child_results.begin(), child_results.end());

    for (const auto& elem : first_param) {
      if (child_set.count(elem) > 0) {
        child_set.erase(elem);
        results->push_back(elem);
      }
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    if (AreSameMessageType(child_->ReturnType(), params_[0]->ReturnType())) {
      return child_->ReturnType();
    }

    // TODO: Consider refactoring ReturnType to return a set of all types
    // in the collection.
    return nullptr;
  }
};

// Converts decimal or integer container messages to a double value
static Status MessageToDouble(const Message& message, double* value) {
  if (IsMessageType<Decimal>(message)) {
    const Decimal* decimal = dynamic_cast<const Decimal*>(&message);

    if (!absl::SimpleAtod(decimal->value(), value)) {
      return InvalidArgument(
          absl::StrCat("Could not convert to numeric: ", decimal->value()));
    }

    return Status::OK();

  } else if (IsMessageType<Integer>(message)) {
    const Integer* integer = dynamic_cast<const Integer*>(&message);

    *value = integer->value();
    return Status::OK();
  }

  return InvalidArgument(
      absl::StrCat("Message type cannot be converted to double: ",
                   message.GetDescriptor()->full_name()));
}

class ComparisonOperator : public BinaryOperator {
 public:
  // Types of comparisons supported by this operator.
  enum ComparisonType {
    kLessThan,
    kGreaterThan,
    kLessThanEqualTo,
    kGreaterThanEqualTo
  };

  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    // Per the FHIRPath spec, comparison operators propagate empty results.
    if (left_results.empty() || right_results.empty()) {
      return Status::OK();
    }

    if (left_results.size() > 1 || right_results.size() > 1) {
      return InvalidArgument(
          "Comparison operators must have one element on each side.");
    }

    const Message* left_result = left_results[0];
    const Message* right_result = right_results[0];

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);

    if (IsMessageType<Integer>(*left_result) &&
        IsMessageType<Integer>(*right_result)) {
      EvalIntegerComparison(dynamic_cast<const Integer*>(left_result),
                            dynamic_cast<const Integer*>(right_result), result);

    } else if (IsMessageType<UnsignedInt>(*left_result) &&
               IsMessageType<Integer>(*right_result)) {
      EvalIntegerComparison(dynamic_cast<const UnsignedInt*>(left_result),
                            dynamic_cast<const Integer*>(right_result), result);

    } else if (IsMessageType<Integer>(*left_result) &&
               IsMessageType<UnsignedInt>(*right_result)) {
      EvalIntegerComparison(dynamic_cast<const Integer*>(left_result),
                            dynamic_cast<const UnsignedInt*>(right_result),
                            result);

    } else if (IsMessageType<UnsignedInt>(*left_result) &&
               IsMessageType<UnsignedInt>(*right_result)) {
      EvalIntegerComparison(dynamic_cast<const UnsignedInt*>(left_result),
                            dynamic_cast<const UnsignedInt*>(right_result),
                            result);

    } else if (IsMessageType<Decimal>(*left_result) ||
               IsMessageType<Decimal>(*right_result)) {
      FHIR_RETURN_IF_ERROR(
          EvalDecimalComparison(left_result, right_result, result));

    } else if (IsMessageType<String>(*left_result) &&
               IsMessageType<String>(*right_result)) {
      EvalStringComparison(dynamic_cast<const String*>(left_result),
                           dynamic_cast<const String*>(right_result), result);
    } else if (IsMessageType<DateTime>(*left_result) &&
               IsMessageType<DateTime>(*right_result)) {
      FHIR_RETURN_IF_ERROR(EvalDateTimeComparison(
          dynamic_cast<const DateTime*>(left_result),
          dynamic_cast<const DateTime*>(right_result), result));
    } else if (IsMessageType<SimpleQuantity>(*left_result) &&
               IsMessageType<SimpleQuantity>(*right_result)) {
      FHIR_RETURN_IF_ERROR(EvalSimpleQuantityComparison(
          dynamic_cast<const SimpleQuantity*>(left_result),
          dynamic_cast<const SimpleQuantity*>(right_result), result));
    } else {
      return InvalidArgument(
          "Unsupported comparison value types: ", left_result->GetTypeName(),
          " and ", right_result->GetTypeName());
    }

    out_results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }

  ComparisonOperator(std::shared_ptr<ExpressionNode> left,
                     std::shared_ptr<ExpressionNode> right,
                     ComparisonType comparison_type)
      : BinaryOperator(left, right), comparison_type_(comparison_type) {}

 private:
  template <class LT, class RT>
  void EvalIntegerComparison(const LT* left_wrapper,
                             const RT* right_wrapper,
                             Boolean* result) const {
    // It isn't necessary to widen the values from 32 to 64 bits when converting
    // a UnsignedInt or PositiveInt to an int32_t because FHIR restricts the
    // values of those types to 31 bits.
    const int32_t left = left_wrapper->value();
    const int32_t right = right_wrapper->value();

    switch (comparison_type_) {
      case kLessThan:
        result->set_value(left < right);
        break;
      case kGreaterThan:
        result->set_value(left > right);
        break;
      case kLessThanEqualTo:
        result->set_value(left <= right);
        break;
      case kGreaterThanEqualTo:
        result->set_value(left >= right);
        break;
    }
  }

  Status EvalDecimalComparison(const Message* left_message,
                               const Message* right_message,
                               Boolean* result) const {
    // Handle decimal comparisons, converting integer types
    // if necessary.
    double left;
    FHIR_RETURN_IF_ERROR(MessageToDouble(*left_message, &left));
    double right;
    FHIR_RETURN_IF_ERROR(MessageToDouble(*right_message, &right));

    switch (comparison_type_) {
      case kLessThan:
        result->set_value(left < right);
        break;
      case kGreaterThan:
        result->set_value(left > right);
        break;
      case kLessThanEqualTo:
        // Fallback to literal comparison for equality to avoid
        // rounding errors.
        result->set_value(
            left <= right ||
            (left_message->GetDescriptor() == right_message->GetDescriptor() &&
             MessageDifferencer::Equals(*left_message, *right_message)));
        break;
      case kGreaterThanEqualTo:
        // Fallback to literal comparison for equality to avoid
        // rounding errors.
        result->set_value(
            left >= right ||
            (left_message->GetDescriptor() == right_message->GetDescriptor() &&
             MessageDifferencer::Equals(*left_message, *right_message)));
        break;
    }

    return Status::OK();
  }

  void EvalStringComparison(const String* left_message,
                            const String* right_message,
                            Boolean* result) const {
    const std::string& left = left_message->value();
    const std::string& right = right_message->value();

    // FHIR defines string comparisons to be based on unicode values,
    // so simply comparison operators are not sufficient.
    static const std::locale locale("en_US.UTF-8");

    static const std::collate<char>& coll =
        std::use_facet<std::collate<char>>(locale);

    int compare_result =
        coll.compare(left.data(), left.data() + left.length(), right.data(),
                     right.data() + right.length());

    switch (comparison_type_) {
      case kLessThan:
        result->set_value(compare_result < 0);
        break;
      case kGreaterThan:
        result->set_value(compare_result > 0);
        break;
      case kLessThanEqualTo:
        result->set_value(compare_result <= 0);
        break;
      case kGreaterThanEqualTo:
        result->set_value(compare_result >= 0);
        break;
    }
  }

  Status EvalDateTimeComparison(const DateTime* left_message,
                                const DateTime* right_message,
                                Boolean* result) const {
    absl::Time left_time = absl::FromUnixMicros(left_message->value_us());
    absl::Time right_time = absl::FromUnixMicros(right_message->value_us());

    FHIR_ASSIGN_OR_RETURN(absl::TimeZone left_zone,
                          BuildTimeZoneFromString(left_message->timezone()));
    FHIR_ASSIGN_OR_RETURN(absl::TimeZone right_zone,
                          BuildTimeZoneFromString(right_message->timezone()));

    // negative if left < right, positive if left > right, 0 if equal
    absl::civil_diff_t time_difference;

    // The FHIRPath spec (http://hl7.org/fhirpath/#comparison) states that
    // datetime comparison is done at the finest precision BOTH
    // dates support. This is equivalent to finding the looser precision
    // between the two and comparing them, which is simpler to implement here.
    if (left_message->precision() == DateTime::YEAR ||
        right_message->precision() == DateTime::YEAR) {
      absl::CivilYear left_year = absl::ToCivilYear(left_time, left_zone);
      absl::CivilYear right_year = absl::ToCivilYear(right_time, right_zone);
      time_difference = left_year - right_year;

    } else if (left_message->precision() == DateTime::MONTH ||
               right_message->precision() == DateTime::MONTH) {
      absl::CivilMonth left_month = absl::ToCivilMonth(left_time, left_zone);
      absl::CivilMonth right_month = absl::ToCivilMonth(right_time, right_zone);
      time_difference = left_month - right_month;

    } else if (left_message->precision() == DateTime::DAY ||
               right_message->precision() == DateTime::DAY) {
      absl::CivilDay left_day = absl::ToCivilDay(left_time, left_zone);
      absl::CivilDay right_day = absl::ToCivilDay(right_time, right_zone);
      time_difference = left_day - right_day;

    } else if (left_message->precision() == DateTime::SECOND ||
               right_message->precision() == DateTime::SECOND) {
      absl::CivilSecond left_second = absl::ToCivilSecond(left_time, left_zone);
      absl::CivilSecond right_second =
          absl::ToCivilSecond(right_time, right_zone);
      time_difference = left_second - right_second;
    } else {
      // Abseil does not support sub-second civil time precision, so we handle
      // them by first comparing seconds (to resolve timezone differences)
      // and then comparing the sub-second component if the seconds are
      // equal.
      absl::CivilSecond left_second = absl::ToCivilSecond(left_time, left_zone);
      absl::CivilSecond right_second =
          absl::ToCivilSecond(right_time, right_zone);
      time_difference = left_second - right_second;

      // In the same second, so check for sub-second differences.
      if (time_difference == 0) {
        time_difference = left_message->value_us() % 1000000 -
                          right_message->value_us() % 1000000;
      }
    }

    switch (comparison_type_) {
      case kLessThan:
        result->set_value(time_difference < 0);
        break;
      case kGreaterThan:
        result->set_value(time_difference > 0);
        break;
      case kLessThanEqualTo:
        result->set_value(time_difference <= 0);
        break;
      case kGreaterThanEqualTo:
        result->set_value(time_difference >= 0);
        break;
    }

    return Status::OK();
  }

  Status EvalSimpleQuantityComparison(const SimpleQuantity* left_wrapper,
                                      const SimpleQuantity* right_wrapper,
                                      Boolean* result) const {
    if (left_wrapper->code().value() != right_wrapper->code().value() ||
        left_wrapper->system().value() != right_wrapper->system().value()) {
      // From the FHIRPath spec: "Implementations are not required to fully
      // support operations on units, but they must at least respect units,
      // recognizing when units differ."
      return InvalidArgument(
          "Compared quantities must have the same units. Got ",
          left_wrapper->unit().value(), " and ", right_wrapper->unit().value());
    }

    return EvalDecimalComparison(&left_wrapper->value(),
                                 &right_wrapper->value(), result);
  }

  ComparisonType comparison_type_;
};

// Implementation for FHIRPath's addition operator.
class AdditionOperator : public BinaryOperator {
 public:
  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    // Per the FHIRPath spec, comparison operators propagate empty results.
    if (left_results.empty() || right_results.empty()) {
      return Status::OK();
    }

    if (left_results.size() > 1 || right_results.size() > 1) {
      return InvalidArgument(
          "Addition operators must have one element on each side.");
    }

    const Message* left_result = left_results[0];
    const Message* right_result = right_results[0];

    if (IsMessageType<Integer>(*left_result) &&
        IsMessageType<Integer>(*right_result)) {
      Integer* result = new Integer();
      work_space->DeleteWhenFinished(result);
      result->set_value(
          EvalIntegerAddition(dynamic_cast<const Integer*>(left_result),
                              dynamic_cast<const Integer*>(right_result)));
      out_results->push_back(result);
    } else if (IsMessageType<String>(*left_result) &&
               IsMessageType<String>(*right_result)) {
      String* result = new String();
      work_space->DeleteWhenFinished(result);
      result->set_value(
          EvalStringAddition(dynamic_cast<const String*>(left_result),
                             dynamic_cast<const String*>(right_result)));
      out_results->push_back(result);
    } else {
      // TODO: Add implementation for Date, DateTime, Time, and Decimal
      // addition.
      return InvalidArgument(absl::StrCat("Addition not supported for ",
                                          left_result->GetTypeName(), " and ",
                                          right_result->GetTypeName()));
    }

    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return left_->ReturnType(); }

  AdditionOperator(std::shared_ptr<ExpressionNode> left,
                   std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(std::move(left), std::move(right)) {}

 private:
  int32_t EvalIntegerAddition(const Integer* left_wrapper,
                              const Integer* right_wrapper) const {
    const int32_t left = left_wrapper->value();
    const int32_t right = right_wrapper->value();
    return left + right;
  }

  std::string EvalStringAddition(const String* left_message,
                                 const String* right_message) const {
    const std::string& left = left_message->value();
    const std::string& right = right_message->value();

    return absl::StrCat(left, right);
  }
};

// Implementation for FHIRPath's string concatenation operator (&).
class StrCatOperator : public BinaryOperator {
 public:
  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results, WorkSpace* work_space,
      std::vector<const Message*>* out_results) const override {
    if (left_results.size() > 1 || right_results.size() > 1) {
      return InvalidArgument(
          "String concatenation operators must have one element on each side.");
    }

    std::string left;
    std::string right;

    if (!left_results.empty()) {
      FHIR_ASSIGN_OR_RETURN(left, MessageToString(*left_results[0]));
    }
    if (!right_results.empty()) {
      FHIR_ASSIGN_OR_RETURN(right, MessageToString(*right_results[0]));
    }

    String* result = new String();
    work_space->DeleteWhenFinished(result);
    result->set_value(absl::StrCat(left, right));
    out_results->push_back(result);
    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return String::GetDescriptor();
  }

  StrCatOperator(std::shared_ptr<ExpressionNode> left,
                 std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(std::move(left), std::move(right)) {}
};

class PolarityOperator : public ExpressionNode {
 public:
  // Supported polarity operations.
  enum PolarityOperation {
    kPositive,
    kNegative,
  };

  PolarityOperator(PolarityOperation operation,
                   std::shared_ptr<ExpressionNode>& operand)
      : operation_(operation), operand_(operand) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    std::vector<const Message*> operand_result;
    FHIR_RETURN_IF_ERROR(operand_->Evaluate(work_space, &operand_result));

    if (operand_result.size() > 1) {
      return InvalidArgument(
          "Polarity operators must operate on a single element.");
    }

    if (operand_result.empty()) {
      return Status::OK();
    }

    const Message* operand_value = operand_result[0];

    if (operation_ == kPositive) {
      results->push_back(operand_value);
      return Status::OK();
    }

    if (IsMessageType<Decimal>(*operand_value)) {
      Decimal* result = new Decimal();
      work_space->DeleteWhenFinished(result);
      result->CopyFrom(*operand_value);
      if (absl::StartsWith(result->value(), "-")) {
        result->set_value(result->value().substr(1));
      } else {
        result->set_value(absl::StrCat("-", result->value()));
      }
      results->push_back(result);
      return Status::OK();
    }

    if (IsMessageType<Integer>(*operand_value)) {
      Integer* result = new Integer();
      work_space->DeleteWhenFinished(result);
      result->CopyFrom(*operand_value);
      result->set_value(result->value() * -1);
      results->push_back(result);
      return Status::OK();
    }

    return InvalidArgument(
        "Polarity operators must operate on a decimal or integer type.");
  }

  const Descriptor* ReturnType() const override {
    return operand_->ReturnType();
  }

 private:
  const PolarityOperation operation_;
  const std::shared_ptr<ExpressionNode> operand_;
};

// Base class for FHIRPath binary boolean operators.
class BooleanOperator : public ExpressionNode {
 public:
  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }

  BooleanOperator(std::shared_ptr<ExpressionNode> left,
                  std::shared_ptr<ExpressionNode> right)
      : left_(left), right_(right) {}

 protected:
  void SetResult(bool eval_result, WorkSpace* work_space,
                 std::vector<const Message*>* results) const {
    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(eval_result);
    results->push_back(result);
  }

  StatusOr<absl::optional<bool>> EvaluateBooleanNode(
      std::shared_ptr<ExpressionNode> node, WorkSpace* work_space) const {
    std::vector<const Message*> results;
    FHIR_RETURN_IF_ERROR(node->Evaluate(work_space, &results));
    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> result,
                          (PrimitiveOrEmpty<bool, Boolean>(results)));
    return result;
  }

  const std::shared_ptr<ExpressionNode> left_;
  const std::shared_ptr<ExpressionNode> right_;
};

// Implements logic for the "implies" operator. Logic may be found in
// section 6.5.4 at http://hl7.org/fhirpath/#boolean-logic
class ImpliesOperator : public BooleanOperator {
 public:
  ImpliesOperator(std::shared_ptr<ExpressionNode> left,
                  std::shared_ptr<ExpressionNode> right)
      : BooleanOperator(left, right) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> left_result,
                          EvaluateBooleanNode(left_, work_space));

    // Short circuit evaluation when left_result == "false"
    if (left_result.has_value() && !left_result.value()) {
      SetResult(true, work_space, results);
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> right_result,
                          EvaluateBooleanNode(right_, work_space));

    if (!left_result.has_value()) {
      if (right_result.value_or(false)) {
        SetResult(true, work_space, results);
      }
    } else if (right_result.has_value()) {
      SetResult(right_result.value(), work_space, results);
    }

    return Status::OK();
  }
};

class XorOperator : public BooleanOperator {
 public:
  XorOperator(std::shared_ptr<ExpressionNode> left,
              std::shared_ptr<ExpressionNode> right)
      : BooleanOperator(left, right) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    // Logic from truth table spec: http://hl7.org/fhirpath/#boolean-logic
    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> left_result,
                          EvaluateBooleanNode(left_, work_space));
    if (!left_result.has_value()) {
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> right_result,
                          EvaluateBooleanNode(right_, work_space));
    if (!right_result.has_value()) {
      return Status::OK();
    }

    SetResult(left_result.value() != right_result.value(), work_space, results);
    return Status::OK();
  }
};

class OrOperator : public BooleanOperator {
 public:
  OrOperator(std::shared_ptr<ExpressionNode> left,
             std::shared_ptr<ExpressionNode> right)
      : BooleanOperator(left, right) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    // Logic from truth table spec: http://hl7.org/fhirpath/#boolean-logic
    // Short circuit and return true on the first true result.
    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> left_result,
                          EvaluateBooleanNode(left_, work_space));
    if (left_result.has_value() && left_result.value()) {
      SetResult(true, work_space, results);
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> right_result,
                          EvaluateBooleanNode(right_, work_space));
    if (right_result.has_value() && right_result.value()) {
      SetResult(true, work_space, results);
      return Status::OK();
    }

    if (left_result.has_value() && right_result.has_value()) {
      // Both children must be false to get here, so return false.
      SetResult(false, work_space, results);
      return Status::OK();
    }

    // Neither child is true and at least one is empty, so propagate
    // empty per the FHIRPath spec.
    return Status::OK();
  }
};

class AndOperator : public BooleanOperator {
 public:
  AndOperator(std::shared_ptr<ExpressionNode> left,
              std::shared_ptr<ExpressionNode> right)
      : BooleanOperator(left, right) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    // Logic from truth table spec: http://hl7.org/fhirpath/#boolean-logic
    // Short circuit and return false on the first false result.
    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> left_result,
                          EvaluateBooleanNode(left_, work_space));
    if (left_result.has_value() && !left_result.value()) {
      SetResult(false, work_space, results);
      return Status::OK();
    }

    FHIR_ASSIGN_OR_RETURN(absl::optional<bool> right_result,
                          EvaluateBooleanNode(right_, work_space));
    if (right_result.has_value() && !right_result.value()) {
      SetResult(false, work_space, results);
      return Status::OK();
    }

    if (left_result.has_value() && right_result.has_value()) {
      // Both children must be true to get here, so return true.
      SetResult(true, work_space, results);
      return Status::OK();
    }

    // Neither child is false and at least one is empty, so propagate
    // empty per the FHIRPath spec.
    return Status::OK();
  }
};

// Implements the "contain" operator. This may also be used for the "in"
// operator by switching the left and right operands.
//
// See https://hl7.org/fhirpath/#collections-2
class ContainsOperator : public BinaryOperator {
 public:
  ContainsOperator(std::shared_ptr<ExpressionNode> left,
                  std::shared_ptr<ExpressionNode> right)
      : BinaryOperator(std::move(left), std::move(right)) {}

  Status EvaluateOperator(
      const std::vector<const Message*>& left_results,
      const std::vector<const Message*>& right_results,
      WorkSpace* work_space,
      std::vector<const Message*>* results) const override {
    if (right_results.empty()) {
      return Status::OK();
    }

    if (right_results.size() > 1) {
      return InvalidArgument(
          "in/contains must have one or fewer items in the left/right "
          "operand.");
    }

    const Message* right_operand = right_results[0];

    bool found =
        std::any_of(left_results.begin(), left_results.end(),
                    [right_operand](const Message* message) {
                      return EqualsOperator::AreEqual(*right_operand, *message);
                    });

    Boolean* result = new Boolean();
    work_space->DeleteWhenFinished(result);
    result->set_value(found);
    results->push_back(result);

    return Status::OK();
  }

  const Descriptor* ReturnType() const override {
    return Boolean::descriptor();
  }
};

// Expression node for a reference to $this.
class ThisReference : public ExpressionNode {
 public:
  explicit ThisReference(const Descriptor* descriptor)
      : descriptor_(descriptor){}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    results->push_back(work_space->MessageContext());
    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return descriptor_; }

 private:
  const Descriptor* descriptor_;
};

// Expression node for a reference to %context.
class ContextReference : public ExpressionNode {
 public:
  explicit ContextReference(const Descriptor* descriptor)
      : descriptor_(descriptor) {}

  Status Evaluate(WorkSpace* work_space,
                  std::vector<const Message*>* results) const override {
    results->push_back(work_space->BottomMessageContext());
    return Status::OK();
  }

  const Descriptor* ReturnType() const override { return descriptor_; }

 private:
  const Descriptor* descriptor_;
};

// Produces a shared pointer explicitly of ExpressionNode rather
// than a subclass to work well with ANTLR's "Any" semantics.
inline std::shared_ptr<ExpressionNode> ToAny(
    std::shared_ptr<ExpressionNode> node) {
  return node;
}

// Internal structure that defines an invocation. This is used
// at points when visiting the AST that do not have enough context
// to produce an ExpressionNode (e.g., they do not see the type of
// the calling object), and is a placeholder for a higher-level
// visitor to transform into an ExpressionNode
struct InvocationDefinition {
  InvocationDefinition(const std::string& name, const bool is_function)
      : name(name), is_function(is_function) {}

  InvocationDefinition(
      const std::string& name, const bool is_function,
      const std::vector<FhirPathParser::ExpressionContext*>& params)
      : name(name), is_function(is_function), params(params) {}

  const std::string name;

  // Indicates it is a function invocation rather than a member lookup.
  const bool is_function;

  const std::vector<FhirPathParser::ExpressionContext*> params;
};

// ANTLR Visitor implementation to translate the AST
// into ExpressionNodes that can run the expression over
// given protocol buffers.
//
// Note that the return value of antlrcpp::Any assumes returned values
// have copy constructors, which means we cannot use std::unique_ptr even
// if that's the most logical choice. Therefore we'll use std:shared_ptr
// more frequently here, but the costs in this case are negligible.
class FhirPathCompilerVisitor : public FhirPathBaseVisitor {
 public:
  explicit FhirPathCompilerVisitor(const Descriptor* descriptor)
      : error_listener_(this), descriptor_stack_({descriptor}) {}

  explicit FhirPathCompilerVisitor(
      const std::vector<const Descriptor*>& descriptor_stack_history,
      const Descriptor* descriptor)
      : error_listener_(this), descriptor_stack_(descriptor_stack_history) {
    descriptor_stack_.push_back(descriptor);
  }

  antlrcpp::Any visitInvocationExpression(
      FhirPathParser::InvocationExpressionContext* node) override {
    antlrcpp::Any expression = node->children[0]->accept(this);

    // This could be a simple member name or a parameterized function...
    antlrcpp::Any invocation = node->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto definition = invocation.as<std::shared_ptr<InvocationDefinition>>();

    std::shared_ptr<ExpressionNode> expr =
        expression.as<std::shared_ptr<ExpressionNode>>();

    if (definition->is_function) {
      auto function_node =
          createFunction(definition->name, expr, definition->params);

      if (function_node == nullptr || !CheckOk()) {
        return nullptr;
      } else {
        return ToAny(function_node);
      }
    } else {
      const Descriptor* descriptor = expr->ReturnType();
      const FieldDescriptor* field =
          descriptor != nullptr
              ? FindFieldByJsonName(descriptor, definition->name)
              : nullptr;

      // If we know the return type of the expression, and the return type
      // doesn't have the referenced field, set an error and return.
      if (descriptor != nullptr && field == nullptr) {
        SetError(absl::StrCat("Unable to find field ", definition->name));
        return nullptr;
      }

      return ToAny(std::make_shared<InvokeExpressionNode>(expression, field,
                                                          definition->name));
    }
  }

  antlrcpp::Any visitInvocationTerm(
      FhirPathParser::InvocationTermContext* ctx) override {
    antlrcpp::Any invocation = visitChildren(ctx);

    if (!CheckOk()) {
      return nullptr;
    }

    if (invocation.is<std::shared_ptr<ExpressionNode>>()) {
      return invocation;
    }

    auto definition = invocation.as<std::shared_ptr<InvocationDefinition>>();

    if (definition->is_function) {
      auto function_node = createFunction(
          definition->name,
          std::make_shared<ThisReference>(descriptor_stack_.back()),
          definition->params);

      return function_node == nullptr || !CheckOk() ? nullptr
                                                    : ToAny(function_node);
    }

    const FieldDescriptor* field =
        descriptor_stack_.back() != nullptr
            ? FindFieldByJsonName(descriptor_stack_.back(), definition->name)
            : nullptr;

    // If we know the return type of the expression, and the return type
    // doesn't have the referenced field, set an error and return.
    if (descriptor_stack_.back() != nullptr && field == nullptr) {
      SetError(absl::StrCat("Unable to find field ", definition->name));
      return nullptr;
    }

    return ToAny(std::make_shared<InvokeTermNode>(field, definition->name));
  }

  antlrcpp::Any visitIndexerExpression(
      FhirPathParser::IndexerExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    return ToAny(std::make_shared<IndexerExpression>(left, right));
  }

  antlrcpp::Any visitUnionExpression(
      FhirPathParser::UnionExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    return ToAny(std::make_shared<UnionOperator>(left, right));
  }

  antlrcpp::Any visitAdditiveExpression(
      FhirPathParser::AdditiveExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    if (op == "+") {
      return ToAny(std::make_shared<AdditionOperator>(left, right));
    }

    if (op == "&") {
      return ToAny(std::make_shared<StrCatOperator>(left, right));
    }

    // TODO: Support "-"

    SetError(absl::StrCat("Unsupported additive operator: ", op));
    return nullptr;
  }

  antlrcpp::Any visitPolarityExpression(
      FhirPathParser::PolarityExpressionContext* ctx) override {
    std::string op = ctx->children[0]->getText();
    antlrcpp::Any operand_any = ctx->children[1]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto operand = operand_any.as<std::shared_ptr<ExpressionNode>>();

    if (op == "+") {
      return ToAny(std::make_shared<PolarityOperator>(
          PolarityOperator::kPositive, operand));
    }

    if (op == "-") {
      return ToAny(std::make_shared<PolarityOperator>(
          PolarityOperator::kNegative, operand));
    }

    SetError(absl::StrCat("Unsupported polarity operator: ", op));
    return nullptr;
  }

  antlrcpp::Any visitTypeExpression(
      FhirPathParser::TypeExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    std::string type = ctx->children[2]->getText();

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();

    if (op == "is") {
      return ToAny(std::make_shared<IsFunction>(left, type));
    }

    if (op == "as") {
      return ToAny(std::make_shared<AsFunction>(left, type));
    }

    SetError(absl::StrCat("Unsupported type operator: ", op));
    return nullptr;
  }

  antlrcpp::Any visitEqualityExpression(
      FhirPathParser::EqualityExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    if (op == "=") {
      return ToAny(std::make_shared<EqualsOperator>(left, right));
    }
    if (op == "!=") {
      // Negate the equals function to implement !=
      auto equals_op = std::make_shared<EqualsOperator>(left, right);
      return ToAny(std::make_shared<NotFunction>(equals_op));
    }

    SetError(absl::StrCat("Unsupported equality operator: ", op));
    return nullptr;
  }

  antlrcpp::Any visitInequalityExpression(
      FhirPathParser::InequalityExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    ComparisonOperator::ComparisonType op_type;

    if (op == "<") {
      op_type = ComparisonOperator::kLessThan;
    } else if (op == ">") {
      op_type = ComparisonOperator::kGreaterThan;
    } else if (op == "<=") {
      op_type = ComparisonOperator::kLessThanEqualTo;
    } else if (op == ">=") {
      op_type = ComparisonOperator::kGreaterThanEqualTo;
    } else {
      SetError(absl::StrCat("Unsupported comparison operator: ", op));
      return nullptr;
    }

    return ToAny(std::make_shared<ComparisonOperator>(left, right, op_type));
  }

  antlrcpp::Any visitMembershipExpression(
      FhirPathParser::MembershipExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    if (op == "in") {
      return ToAny(std::make_shared<ContainsOperator>(right, left));
    } else if (op == "contains") {
      return ToAny(std::make_shared<ContainsOperator>(left, right));
    }

    SetError(absl::StrCat("Unsupported membership operator: ", op));
    return nullptr;
  }

  antlrcpp::Any visitMemberInvocation(
      FhirPathParser::MemberInvocationContext* ctx) override {
    std::string text = ctx->identifier()->IDENTIFIER()->getSymbol()->getText();
    return std::make_shared<InvocationDefinition>(text, false);
  }

  antlrcpp::Any visitFunctionInvocation(
      FhirPathParser::FunctionInvocationContext* ctx) override {
    if (!CheckOk()) {
      return nullptr;
    }

    std::string text = ctx->function()->identifier()->getText();
    std::vector<FhirPathParser::ExpressionContext*> params;
    if (ctx->function()->paramList()) {
      params = ctx->function()->paramList()->expression();
    }

    return std::make_shared<InvocationDefinition>(text, true, params);
  }

  antlrcpp::Any visitImpliesExpression(
      FhirPathParser::ImpliesExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    return ToAny(std::make_shared<ImpliesOperator>(left, right));
  }

  antlrcpp::Any visitOrExpression(
      FhirPathParser::OrExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    std::string op = ctx->children[1]->getText();
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    return op == "or"
        ? ToAny(std::make_shared<OrOperator>(left, right))
        : ToAny(std::make_shared<XorOperator>(left, right));
  }

  antlrcpp::Any visitAndExpression(
      FhirPathParser::AndExpressionContext* ctx) override {
    antlrcpp::Any left_any = ctx->children[0]->accept(this);
    antlrcpp::Any right_any = ctx->children[2]->accept(this);

    if (!CheckOk()) {
      return nullptr;
    }

    auto left = left_any.as<std::shared_ptr<ExpressionNode>>();
    auto right = right_any.as<std::shared_ptr<ExpressionNode>>();

    return ToAny(std::make_shared<AndOperator>(left, right));
  }

  antlrcpp::Any visitParenthesizedTerm(
      FhirPathParser::ParenthesizedTermContext* ctx) override {
    // Simply propagate the value of the parenthesized term.
    return ctx->children[1]->accept(this);
  }

  antlrcpp::Any visitThisInvocation(
      FhirPathParser::ThisInvocationContext* ctx) override {
    return ToAny(std::make_shared<ThisReference>(descriptor_stack_.back()));
  }

  antlrcpp::Any visitExternalConstant(
      FhirPathParser::ExternalConstantContext* ctx) override {
    std::string name = ctx->children[1]->getText();
    if (name == "ucum") {
      return ToAny(std::make_shared<Literal<String, std::string>>(
          "http://unitsofmeasure.org"));
    } else if (name == "sct") {
      return ToAny(std::make_shared<Literal<String, std::string>>(
          "http://snomed.info/sct"));
    } else if (name == "loinc") {
      return ToAny(
          std::make_shared<Literal<String, std::string>>("http://loinc.org"));
    } else if (name == "context") {
      return ToAny(
          std::make_shared<ContextReference>(descriptor_stack_.front()));
    }

    SetError(absl::StrCat("Unknown external constant: ", name));
    return nullptr;
  }

  antlrcpp::Any visitTerminal(TerminalNode* node) override {
    const std::string& text = node->getSymbol()->getText();

    switch (node->getSymbol()->getType()) {
      case FhirPathLexer::NUMBER:
        // Determine if the number is an integer or decimal, propagating
        // decimal types in string form to preserve precision.
        if (text.find(".") != std::string::npos) {
          return ToAny(std::make_shared<Literal<Decimal, std::string>>(text));
        } else {
          int32_t value;
          if (!absl::SimpleAtoi(text, &value)) {
            SetError(absl::StrCat("Malformed integer ", text));
            return nullptr;
          }

          return ToAny(std::make_shared<Literal<Integer, int32_t>>(value));
        }

      case FhirPathLexer::STRING: {
        // The lexer keeps the quotes around string literals,
        // so we remove them here. The following assert simply reflects
        // the lexer's guarantees as defined.
        assert(text.length() >= 2);
        const std::string& trimmed = text.substr(1, text.length() - 2);
        std::string unescaped;
        // CUnescape handles additional escape sequences not allowed by
        // FHIRPath. However, these additional sequences are disallowed by the
        // grammar rules (FhirPath.g4) which are enforced by the parser. In
        // addition, CUnescape does not handle escaped forward slashes.
        absl::CUnescape(trimmed, &unescaped);
        return ToAny(std::make_shared<Literal<String, std::string>>(unescaped));
      }

      case FhirPathLexer::BOOL:
        return ToAny(std::make_shared<Literal<Boolean, bool>>(text == "true"));

      case FhirPathLexer::EMPTY:
        return ToAny(std::make_shared<EmptyLiteral>());

      default:

        SetError(absl::StrCat("Unknown terminal type: ", text));
        return nullptr;
    }
  }

  antlrcpp::Any defaultResult() override { return nullptr; }

  bool CheckOk() { return error_message_.empty(); }

  std::string GetError() { return error_message_; }

  BaseErrorListener* GetErrorListener() { return &error_listener_; }

 private:
  typedef std::function<StatusOr<ExpressionNode*>(
      std::shared_ptr<ExpressionNode>,
      const std::vector<FhirPathParser::ExpressionContext*>&,
      FhirPathBaseVisitor*, FhirPathBaseVisitor*)>
      FunctionFactory;

  std::map<std::string, FunctionFactory> function_map{
      {"exists", FunctionNode::Create<ExistsFunction>},
      {"not", FunctionNode::Create<NotFunction>},
      {"hasValue", FunctionNode::Create<HasValueFunction>},
      {"startsWith", FunctionNode::Create<StartsWithFunction>},
      {"contains", FunctionNode::Create<ContainsFunction>},
      {"empty", FunctionNode::Create<EmptyFunction>},
      {"first", FunctionNode::Create<FirstFunction>},
      {"tail", FunctionNode::Create<TailFunction>},
      {"trace", FunctionNode::Create<TraceFunction>},
      {"toInteger", FunctionNode::Create<ToIntegerFunction>},
      {"count", FunctionNode::Create<CountFunction>},
      {"combine", FunctionNode::Create<CombineFunction>},
      {"distinct", FunctionNode::Create<DistinctFunction>},
      {"matches", FunctionNode::Create<MatchesFunction>},
      {"length", FunctionNode::Create<LengthFunction>},
      {"isDistinct", FunctionNode::Create<IsDistinctFunction>},
      {"intersect", FunctionNode::Create<IntersectFunction>},
      {"where", FunctionNode::Create<WhereFunction>},
      {"select", FunctionNode::Create<SelectFunction>},
      {"all", FunctionNode::Create<AllFunction>},
      {"toString", FunctionNode::Create<ToStringFunction>},
      {"iif", FunctionNode::Create<IifFunction>},
      {"is", IsFunction::Create},
      {"as", AsFunction::Create},
      {"children", FunctionNode::Create<ChildrenFunction>},
  };

  // Returns an ExpressionNode that implements the specified FHIRPath function.
  std::shared_ptr<ExpressionNode> createFunction(
      const std::string& function_name,
      std::shared_ptr<ExpressionNode> child_expression,
      const std::vector<FhirPathParser::ExpressionContext*>& params) {
    std::map<std::string, FunctionFactory>::iterator function_factory =
        function_map.find(function_name);
    if (function_factory != function_map.end()) {
      // Some functions accept parameters that are expressions evaluated using
      // the child expression's result as context, not the base context of the
      // FHIRPath expression. In order to compile such parameters, we need to
      // visit it with the child expression's type and not the base type of the
      // current visitor. Therefore, both the current visitor and a visitor with
      // the child expression as the context are provided. The function factory
      // will use whichever visitor (or both) is needed to compile the function
      // invocation.
      FhirPathCompilerVisitor child_context_visitor(
          descriptor_stack_, child_expression->ReturnType());
      StatusOr<ExpressionNode*> result = function_factory->second(
          child_expression, params, this, &child_context_visitor);
      if (!result.ok()) {
        this->SetError(absl::StrCat(
            "Failed to compile call to ", function_name,
            "(): ", result.status().error_message(),
            !child_context_visitor.CheckOk()
                ? absl::StrCat("; ", child_context_visitor.GetError())
                : ""));
        return std::shared_ptr<ExpressionNode>(nullptr);
      }

      return std::shared_ptr<ExpressionNode>(result.ValueOrDie());
    } else {
      // TODO: Implement set of functions for initial use cases.
      SetError(absl::StrCat("The function ", function_name,
                            " is not yet implemented"));

      return std::shared_ptr<ExpressionNode>(nullptr);
    }
  }

  // ANTLR listener to report syntax errors.
  class FhirPathErrorListener : public BaseErrorListener {
   public:
    explicit FhirPathErrorListener(FhirPathCompilerVisitor* visitor)
        : visitor_(visitor) {}

    void syntaxError(antlr4::Recognizer* recognizer,
                     antlr4::Token* offending_symbol, size_t line,
                     size_t position_in_line, const std::string& message,
                     std::exception_ptr e) override {
      visitor_->SetError(message);
    }

   private:
    FhirPathCompilerVisitor* visitor_;
  };

  void SetError(const std::string& error_message) {
    error_message_ = error_message;
  }

  FhirPathErrorListener error_listener_;
  std::vector<const Descriptor*> descriptor_stack_;
  std::string error_message_;
};

}  // namespace internal

EvaluationResult::EvaluationResult(EvaluationResult&& result)
    : work_space_(std::move(result.work_space_)) {}

EvaluationResult& EvaluationResult::operator=(EvaluationResult&& result) {
  work_space_ = std::move(result.work_space_);

  return *this;
}

EvaluationResult::EvaluationResult(
    std::unique_ptr<internal::WorkSpace> work_space)
    : work_space_(std::move(work_space)) {}

EvaluationResult::~EvaluationResult() {}

const std::vector<const Message*>& EvaluationResult::GetMessages() const {
  return work_space_->GetResultMessages();
}

StatusOr<bool> EvaluationResult::GetBoolean() const {
  if (internal::IsSingleBoolean(work_space_->GetResultMessages())) {
    return internal::MessagesToBoolean(work_space_->GetResultMessages());
  }

  return InvalidArgument("Expression did not evaluate to boolean");
}

StatusOr<int32_t> EvaluationResult::GetInteger() const {
  auto messages = work_space_->GetResultMessages();

  if (messages.size() == 1 && IsMessageType<Integer>(*messages[0])) {
    return dynamic_cast<const Integer*>(messages[0])->value();
  }

  return InvalidArgument("Expression did not evaluate to integer");
}

StatusOr<std::string> EvaluationResult::GetDecimal() const {
  auto messages = work_space_->GetResultMessages();

  if (messages.size() == 1 && IsMessageType<Decimal>(*messages[0])) {
    return dynamic_cast<const Decimal*>(messages[0])->value();
  }

  return InvalidArgument("Expression did not evaluate to decimal");
}

StatusOr<std::string> EvaluationResult::GetString() const {
  auto messages = work_space_->GetResultMessages();

  if (messages.size() == 1 && IsMessageType<String>(*messages[0])) {
    return dynamic_cast<const String*>(messages[0])->value();
  }

  return InvalidArgument("Expression did not evaluate to string");
}

CompiledExpression::CompiledExpression(CompiledExpression&& other)
    : fhir_path_(std::move(other.fhir_path_)),
      root_expression_(std::move(other.root_expression_)) {}

CompiledExpression& CompiledExpression::operator=(CompiledExpression&& other) {
  fhir_path_ = std::move(other.fhir_path_);
  root_expression_ = std::move(other.root_expression_);

  return *this;
}

CompiledExpression::CompiledExpression(const CompiledExpression& other)
    : fhir_path_(other.fhir_path_), root_expression_(other.root_expression_) {}

CompiledExpression& CompiledExpression::operator=(
    const CompiledExpression& other) {
  fhir_path_ = other.fhir_path_;
  root_expression_ = other.root_expression_;

  return *this;
}

const std::string& CompiledExpression::fhir_path() const { return fhir_path_; }

CompiledExpression::CompiledExpression(
    const std::string& fhir_path,
    std::shared_ptr<internal::ExpressionNode> root_expression)
    : fhir_path_(fhir_path), root_expression_(root_expression) {}

StatusOr<CompiledExpression> CompiledExpression::Compile(
    const Descriptor* descriptor, const std::string& fhir_path) {
  ANTLRInputStream input(fhir_path);
  FhirPathLexer lexer(&input);
  CommonTokenStream tokens(&lexer);
  FhirPathParser parser(&tokens);

  internal::FhirPathCompilerVisitor visitor(descriptor);
  parser.addErrorListener(visitor.GetErrorListener());
  antlrcpp::Any result = visitor.visit(parser.expression());

  // TODO: the visitor error check should be redundant
  if (result.isNotNull() && visitor.GetError().empty()) {
    auto root_node = result.as<std::shared_ptr<internal::ExpressionNode>>();
    return CompiledExpression(fhir_path, root_node);
  } else {
    auto status = InvalidArgument(visitor.GetError());
    return InvalidArgument(visitor.GetError());
  }
}

StatusOr<EvaluationResult> CompiledExpression::Evaluate(
    const Message& message) const {
  auto work_space = absl::make_unique<internal::WorkSpace>(&message);

  std::vector<const Message*> results;

  FHIR_RETURN_IF_ERROR(root_expression_->Evaluate(work_space.get(), &results));

  work_space->SetResultMessages(results);

  return EvaluationResult(std::move(work_space));
}

MessageValidator::MessageValidator() {}
MessageValidator::~MessageValidator() {}

// Build the constraints for the given message type and
// add it to the constraints cache.
StatusOr<MessageValidator::MessageConstraints*>
MessageValidator::ConstraintsFor(const Descriptor* descriptor) {
  // Simply return the cached constraint if it exists.
  auto iter = constraints_cache_.find(descriptor->full_name());

  if (iter != constraints_cache_.end()) {
    return iter->second.get();
  }

  auto constraints = absl::make_unique<MessageConstraints>();

  int ext_size =
      descriptor->options().ExtensionSize(proto::fhir_path_message_constraint);

  for (int i = 0; i < ext_size; ++i) {
    const std::string& fhir_path = descriptor->options().GetExtension(
        proto::fhir_path_message_constraint, i);
    auto constraint = CompiledExpression::Compile(descriptor, fhir_path);
    if (constraint.ok()) {
      CompiledExpression expression = constraint.ValueOrDie();
      constraints->message_expressions_.push_back(expression);
    } else {
      LOG(WARNING) << "Ignoring message constraint on " << descriptor->name()
                   << " (" << fhir_path << "). "
                   << constraint.status().error_message();
    }

    // TODO: Unsupported FHIRPath expressions are simply not
    // validated for now; this should produce an error once we support
    // all of FHIRPath.
  }

  for (int i = 0; i < descriptor->field_count(); i++) {
    const FieldDescriptor* field = descriptor->field(i);

    const Descriptor* field_type = field->message_type();

    // Constraints only apply to non-primitives.
    if (field_type != nullptr) {
      int ext_size =
          field->options().ExtensionSize(proto::fhir_path_constraint);

      for (int j = 0; j < ext_size; ++j) {
        const std::string& fhir_path =
            field->options().GetExtension(proto::fhir_path_constraint, j);

        auto constraint = CompiledExpression::Compile(field_type, fhir_path);

        if (constraint.ok()) {
          constraints->field_expressions_.push_back(
              std::make_pair(field, constraint.ValueOrDie()));
        } else {
          LOG(WARNING) << "Ignoring field constraint on " << descriptor->name()
                       << "." << field_type->name() << " (" << fhir_path
                       << "). " << constraint.status().error_message();
        }

        // TODO: Unsupported FHIRPath expressions are simply not
        // validated for now; this should produce an error once we support
        // all of FHIRPath.
      }
    }
  }

  // Add the successful constraints to the cache while keeping a local
  // reference.
  MessageConstraints* constraints_local = constraints.get();
  constraints_cache_[descriptor->full_name()] = std::move(constraints);

  // Now we recursively look for fields with constraints.
  for (int i = 0; i < descriptor->field_count(); i++) {
    const FieldDescriptor* field = descriptor->field(i);

    const Descriptor* field_type = field->message_type();

    // Constraints only apply to non-primitives.
    if (field_type != nullptr) {
      // Validate the field type.
      FHIR_ASSIGN_OR_RETURN(auto child_constraints, ConstraintsFor(field_type));

      // Nested fields that directly or transitively have constraints
      // are retained and used when applying constraints.
      if (!child_constraints->message_expressions_.empty() ||
          !child_constraints->field_expressions_.empty() ||
          !child_constraints->nested_with_constraints_.empty()) {
        constraints_local->nested_with_constraints_.push_back(field);
      }
    }
  }

  return constraints_local;
}

// Default handler halts on first error
bool HaltOnErrorHandler(const Message& message, const FieldDescriptor* field,
                        const std::string& constraint) {
  return true;
}

Status MessageValidator::Validate(const Message& message) {
  return Validate(message, HaltOnErrorHandler);
}

// Validates that the given message satisfies the
// the given FHIRPath expression, invoking the handler in case
// of failure.
Status ValidateMessageConstraint(const Message& message,
                                 const CompiledExpression& expression,
                                 const ViolationHandlerFunc handler,
                                 bool* halt_validation) {
  FHIR_ASSIGN_OR_RETURN(const EvaluationResult expr_result,
                        expression.Evaluate(message));

  if (!expr_result.GetBoolean().ok()) {
    *halt_validation = true;
    return InvalidArgument(absl::StrCat(
        "Constraint did not evaluate to boolean: ",
        message.GetDescriptor()->name(), ": \"", expression.fhir_path(), "\""));
  }

  if (!expr_result.GetBoolean().ValueOrDie()) {
    std::string err_msg = absl::StrCat("fhirpath-constraint-violation-",
                                       message.GetDescriptor()->name(), ": \"",
                                       expression.fhir_path(), "\"");

    *halt_validation = handler(message, nullptr, expression.fhir_path());
    return ::tensorflow::errors::FailedPrecondition(err_msg);
  }

  return Status::OK();
}

// Validates that the given field in the given parent satisfies the
// the given FHIRPath expression, invoking the handler in case
// of failure.
Status ValidateFieldConstraint(const Message& parent,
                               const FieldDescriptor* field,
                               const Message& field_value,
                               const CompiledExpression& expression,
                               const ViolationHandlerFunc handler,
                               bool* halt_validation) {
  FHIR_ASSIGN_OR_RETURN(const EvaluationResult expr_result,
                        expression.Evaluate(field_value));

  if (!expr_result.GetBoolean().ValueOrDie()) {
    std::string err_msg = absl::StrCat(
        "fhirpath-constraint-violation-", field->containing_type()->name(), ".",
        field->json_name(), ": \"", expression.fhir_path(), "\"");

    *halt_validation = handler(parent, field, expression.fhir_path());
    return ::tensorflow::errors::FailedPrecondition(err_msg);
  }

  return Status::OK();
}

// Store the first detected failure in the accumulative status.
void UpdateStatus(Status* accumulative_status, const Status& current_status) {
  if (accumulative_status->ok() && !current_status.ok()) {
    *accumulative_status = current_status;
  }
}

Status MessageValidator::Validate(const Message& message,
                                  ViolationHandlerFunc handler) {
  bool halt_validation = false;
  return Validate(message, handler, &halt_validation);
}

Status MessageValidator::Validate(const Message& message,
                                  ViolationHandlerFunc handler,
                                  bool* halt_validation) {
  // ConstraintsFor may recursively build constraints so
  // we lock the mutex here to ensure thread safety.
  mutex_.Lock();
  auto status_or_constraints = ConstraintsFor(message.GetDescriptor());
  mutex_.Unlock();

  if (!status_or_constraints.ok()) {
    return status_or_constraints.status();
  }

  auto constraints = status_or_constraints.ValueOrDie();

  // Keep the first failure to return to the caller.
  Status accumulative_status = Status::OK();

  // Validate the constraints attached to the message root.
  for (const CompiledExpression& expr : constraints->message_expressions_) {
    UpdateStatus(
        &accumulative_status,
        ValidateMessageConstraint(message, expr, handler, halt_validation));
    if (*halt_validation) {
      return accumulative_status;
    }
  }

  // Validate the constraints attached to the message's fields.
  for (auto expression : constraints->field_expressions_) {
    if (*halt_validation) {
      return accumulative_status;
    }

    const FieldDescriptor* field = expression.first;
    const CompiledExpression& expr = expression.second;

    ForEachMessageHalting<Message>(message, field, [&](const Message& child) {
      UpdateStatus(&accumulative_status,
                   ValidateFieldConstraint(message, field, child, expr, handler,
                                           halt_validation));

      return *halt_validation;
    });
  }

  // Recursively validate constraints for nested messages that have them.
  for (const FieldDescriptor* field : constraints->nested_with_constraints_) {
    if (*halt_validation) {
      return accumulative_status;
    }

    ForEachMessageHalting<Message>(message, field, [&](const Message& child) {
      UpdateStatus(&accumulative_status,
                   Validate(child, handler, halt_validation));
      return *halt_validation;
    });
  }

  return accumulative_status;
}

// Common validator instance for the lifetime of the process.
static MessageValidator* validator = new MessageValidator();

Status ValidateMessage(const ::google::protobuf::Message& message) {
  return validator->Validate(message);
}

Status ValidateMessage(const ::google::protobuf::Message& message,
                       ViolationHandlerFunc handler) {
  return validator->Validate(message, handler);
}

}  // namespace fhir_path
}  // namespace fhir
}  // namespace google
