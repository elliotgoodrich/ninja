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

#include "manifest_parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <charconv>
#include <memory>
#include <vector>

#include "edge_bindings.h"
#include "file_scope.h"
#include "graph.h"
#include "state.h"
#include "util.h"
#include "version.h"

using namespace std;

namespace {

/// The scope for evaluating a build statement's paths: the variables set in
/// its build block, then the scope of the file it is in.  Unlike EdgeScope,
/// $in, $out and the rule's variables are not visible.
struct EdgePathScope {
  explicit EdgePathScope(const EdgeBindings& bindings)
      : bindings_(bindings) {}

  std::string LookupVariable(StringPiece var) const {
    if (std::optional<StringPiece> value = bindings_.FindBinding(var))
      return value->AsString();
    return bindings_.parent().LookupVariable(var);
  }

 private:
  const EdgeBindings& bindings_;
};

}  // anonymous namespace

ManifestParser::ManifestParser(State* state, FileReader* file_reader,
                               ManifestParserOptions options)
    : Parser(state, file_reader),
      options_(options), quiet_(false) {
  scope_ = &state->bindings_;
}

std::string ManifestParser::LookupVariable(const std::string& varname) {
  return scope_->LookupVariable(varname);
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  lexer_.Start(filename, input);

  for (;;) {
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
    case Lexer::POOL:
      if (!ParsePool(err))
        return false;
      break;
    case Lexer::BUILD:
      if (!ParseEdge(err))
        return false;
      break;
    case Lexer::RULE:
      if (!ParseRule(err))
        return false;
      break;
    case Lexer::DEFAULT:
      if (!ParseDefault(err))
        return false;
      break;
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      string name;
      value_.Clear();
      if (!ParseLet(&name, &value_, err))
        return false;
      string value = value_.Evaluate(scope_);
      // Check ninja_required_version immediately so we can exit
      // before encountering any syntactic surprises.
      if (name == "ninja_required_version")
        CheckNinjaVersion(value, &lexer_.manifest_version_major, &lexer_.manifest_version_minor);
      scope_->AddBinding(name, value);
      break;
    }
    case Lexer::INCLUDE:
      if (!ParseFileInclude(false, err))
        return false;
      break;
    case Lexer::SUBNINJA:
      if (!ParseFileInclude(true, err))
        return false;
      break;
    case Lexer::ERROR: {
      return lexer_.Error(lexer_.DescribeLastError(), err);
    }
    case Lexer::TEOF:
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}


bool ManifestParser::ParsePool(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (state_->LookupPool(name) != NULL)
    return lexer_.Error("duplicate pool '" + name + "'", err);

  int depth = -1;

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    value_.Clear();
    if (!ParseLet(&key, &value_, err))
      return false;

    if (key == "depth") {
      string depth_string = value_.Evaluate(scope_);
      const char* begin = depth_string.data();
      const char* end = begin + depth_string.size();
      auto result = std::from_chars(begin, end, depth);
      if (result.ec != std::errc() || result.ptr != end || depth < 0) {
        return lexer_.Error("invalid pool depth", err);
      }
    } else {
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  state_->AddPool(new Pool(name, depth));
  return true;
}


bool ManifestParser::ParseRule(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (scope_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name + "'", err);

  auto rule = std::unique_ptr<Rule>(new Rule(name));

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    value_.Clear();
    if (!ParseLet(&key, &value_, err))
      return false;

    if (Rule::IsReservedBinding(key)) {
      rule->AddBinding(key, value_);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (rule->bindings_["rspfile"].empty() !=
      rule->bindings_["rspfile_content"].empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (rule->bindings_["command"].empty())
    return lexer_.Error("expected 'command =' line", err);

  scope_->AddRule(std::move(rule));
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::EQUALS, err))
    return false;
  if (!lexer_.ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseDefault(string* err) {
  value_.Clear();
  if (!lexer_.ReadPath(&value_, err))
    return false;
  if (value_.empty())
    return lexer_.Error("expected target name", err);

  do {
    string path = value_.Evaluate(scope_);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;  // Unused because this only does lookup.
    CanonicalizePath(&path, &slash_bits);
    std::string default_err;
    if (!state_->AddDefault(path, &default_err))
      return lexer_.Error(default_err, err);

    value_.Clear();
    if (!lexer_.ReadPath(&value_, err))
      return false;
  } while (!value_.empty());

  return ExpectToken(Lexer::NEWLINE, err);
}

EvalString ManifestParser::TakeEvalString() {
  if (cache_.empty())
    return EvalString();
  EvalString eval = std::move(cache_.back());
  cache_.pop_back();
  eval.Clear();
  return eval;
}

bool ManifestParser::ReadPaths(vector<EvalString>* paths, string* err) {
  for (;;) {
    EvalString path = TakeEvalString();
    if (!lexer_.ReadPath(&path, err)) {
      cache_.push_back(std::move(path));
      return false;
    }
    if (path.empty()) {
      cache_.push_back(std::move(path));
      return true;
    }
    paths->push_back(std::move(path));
  }
}

bool ManifestParser::ParseEdge(string* err) {
  // Move the previous edge's paths into the cache so their buffers are reused.
  for (vector<EvalString>* paths : { &ins_, &outs_, &validations_ }) {
    for (EvalString& path : *paths)
      cache_.push_back(std::move(path));
    paths->clear();
  }

  if (!ReadPaths(&outs_, err))
    return false;

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    const size_t explicit_outs = outs_.size();
    if (!ReadPaths(&outs_, err))
      return false;
    implicit_outs = static_cast<int>(outs_.size() - explicit_outs);
  }

  if (outs_.empty())
    return lexer_.Error("expected path", err);

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = scope_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name + "'", err);

  // XXX should we require one path here?
  if (!ReadPaths(&ins_, err))
    return false;

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    const size_t before = ins_.size();
    if (!ReadPaths(&ins_, err))
      return false;
    implicit = static_cast<int>(ins_.size() - before);
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    const size_t before = ins_.size();
    if (!ReadPaths(&ins_, err))
      return false;
    order_only = static_cast<int>(ins_.size() - before);
  }

  // Add all validations.
  if (lexer_.PeekToken(Lexer::PIPEAT)) {
    if (!ReadPaths(&validations_, err))
      return false;
  }

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  Edge* edge = state_->AddEdge(rule, scope_);
  // Edge bindings are evaluated in the enclosing scope, not the edge's.
  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    value_.Clear();
    if (!ParseLet(&key, &value_, err))
      return false;

    edge->bindings_.AddBinding(key, value_.Evaluate(scope_));
  }
  const EdgePathScope path_scope(edge->bindings_);

  string pool_name = edge->GetBinding("pool");
  if (!pool_name.empty()) {
    Pool* pool = state_->LookupPool(pool_name);
    if (pool == NULL)
      return lexer_.Error("unknown pool name '" + pool_name + "'", err);
    edge->pool_ = pool;
  }

  edge->outputs_.reserve(outs_.size());
  for (size_t i = 0, e = outs_.size(); i != e; ++i) {
    string path = outs_[i].Evaluate(&path_scope);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node *node = state_->GetNode(std::move(path), slash_bits);
    if (!state_->AddOut(edge, node, err)) {
      lexer_.Error(std::string(*err), err);
      return false;
    }
  }

  if (edge->outputs_.empty()) {
    // All outputs of the edge are already created by other edges. Don't add
    // this edge.  Do this check before input nodes are connected to the edge.
    state_->edges_.pop_back();
    delete edge;
    return true;
  }
  edge->implicit_outs_ = implicit_outs;

  edge->inputs_.reserve(ins_.size());
  for (vector<EvalString>::iterator i = ins_.begin(); i != ins_.end(); ++i) {
    string path = i->Evaluate(&path_scope);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node *node = state_->GetNode(std::move(path), slash_bits);
    state_->AddIn(edge, node);
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  edge->validations_.reserve(validations_.size());
  for (std::vector<EvalString>::iterator v = validations_.begin();
      v != validations_.end(); ++v) {
    string path = v->Evaluate(&path_scope);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node *node = state_->GetNode(std::move(path), slash_bits);
    state_->AddValidation(edge, node);
  }

  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements
    // that reference themselves.  Ninja used to tolerate these in the
    // build graph but that has since been fixed.  Filter them out to
    // support users of those old CMake versions.
    Node* out = edge->outputs_[0];
    vector<Node*>::iterator new_end =
        remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      out->RemoveOutEdge(edge);
      edge->inputs_.erase(new_end, edge->inputs_.end());
      if (!quiet_) {
        Warning("phony target '%s' names itself as an input; "
                "ignoring [-w phonycycle=warn]",
                out->path().c_str());
      }
    }
  }

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  string dyndep = edge->GetUnescapedDyndep();
  if (!dyndep.empty()) {
    uint64_t slash_bits;
    CanonicalizePath(&dyndep, &slash_bits);
    edge->dyndep_ = state_->GetNode(dyndep, slash_bits);
    edge->dyndep_->set_dyndep_pending(true);
    vector<Node*>::iterator dgi =
      std::find(edge->inputs_.begin(), edge->inputs_.end(), edge->dyndep_);
    if (dgi == edge->inputs_.end()) {
      return lexer_.Error("dyndep '" + dyndep + "' is not an input", err);
    }
    assert(!edge->dyndep_->generated_by_dep_loader());
  }

  return true;
}

bool ManifestParser::ParseFileInclude(bool new_scope, string* err) {
  value_.Clear();
  if (!lexer_.ReadPath(&value_, err))
    return false;
  string path = value_.Evaluate(scope_);

  if (subparser_ == nullptr) {
    subparser_.reset(new ManifestParser(state_, file_reader_, options_));
  }
  if (new_scope) {
    subparser_->scope_ = new FileScope(scope_);
  } else {
    subparser_->scope_ = scope_;
  }

  if (!subparser_->Load(path, err, &lexer_))
    return false;

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}
