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

#include "eval_env.h"

using namespace std;

void Rule::AddBinding(const string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(StringPiece key) const {
  Bindings::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return NULL;
  return &i->second;
}

std::unique_ptr<Rule> Rule::Phony() {
  auto rule = std::unique_ptr<Rule>(new Rule("phony"));
  rule->phony_ = true;
  return rule;
}

bool Rule::IsPhony() const {
  return phony_;
}

// static
bool Rule::IsReservedBinding(StringPiece var) {
  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "msvc_deps_prefix";
}
