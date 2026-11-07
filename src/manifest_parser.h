// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include "parser.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct FileScope;
struct EvalString;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

enum PhonyCycleAction {
  kPhonyCycleActionWarn,
  kPhonyCycleActionError,
};

struct ManifestParserOptions {
  PhonyCycleAction phony_cycle_action_ = kPhonyCycleActionWarn;
};

/// Parses .ninja files.
struct ManifestParser : public Parser {
  ManifestParser(State* state, FileReader* file_reader,
                 ManifestParserOptions options = ManifestParserOptions());

  /// Parse a text string of input.  Used by tests.
  bool ParseTest(const std::string& input, std::string* err) {
    quiet_ = true;
    return Parse("input", input, err);
  }

  /// Retrieve the expanded value of a top-level variable from the
  /// manifest. Returns an empty string if the variable is not defined.
  /// Must be called only after a successful Load() call.
  std::string LookupVariable(const std::string& varname);

 private:
  /// Parse a file, given its contents as a string.
  bool Parse(const std::string& filename, const std::string& input,
             std::string* err);

  /// Parse various statement types.
  bool ParsePool(std::string* err);
  bool ParseRule(std::string* err);
  bool ParseLet(std::string* key, EvalString* val, std::string* err);
  bool ParseEdge(std::string* err);
  bool ParseDefault(std::string* err);

  /// Parse either a 'subninja' or 'include' line.
  bool ParseFileInclude(bool new_scope, std::string* err);

  FileScope* scope_;
  ManifestParserOptions options_;
  bool quiet_;

  // ins_/out_/validations_ are reused across invocations to ParseEdge(),
  // to save on the otherwise constant memory reallocation.
  // subparser_ is reused solely to get better reuse out ins_/outs_/validation_.
  std::unique_ptr<ManifestParser> subparser_;
  std::vector<EvalString> ins_, outs_, validations_;

  // The variables in the build block of the edge being parsed.  Elements are
  // never removed, only overwritten, so the vector and the memory of its keys
  // are reused across invocations to ParseEdge() as well.
  std::vector<std::pair<std::string, std::string>> edge_bindings_;
};

#endif  // NINJA_MANIFEST_PARSER_H_
