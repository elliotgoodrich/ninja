// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "edge_bindings.h"

#include "file_scope.h"
#include "string_piece_util.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <vector>

namespace {

/// Order a binding before a key, for binary search over sorted bindings.
struct BindingKeyLess {
  bool operator()(const EdgeBindings::KeyValuePair& binding,
                  StringPiece key) const {
    return StringPieceLess()(binding.first, key);
  }
};

}  // anonymous namespace

struct EdgeBindings::Impl {
  explicit Impl(FileScope* parent) : parent(parent) {}
  FileScope* parent;
  /// Sorted by key with StringPieceLess, for binary search.
  std::vector<KeyValuePair> bindings;
};

EdgeBindings::EdgeBindings(FileScope* parent) : data_(parent) {}

EdgeBindings::~EdgeBindings() {
  delete get_if<Impl*>(&data_);
}

FileScope* EdgeBindings::parent() const {
  return holds_alternative<FileScope*>(data_)
             ? get<FileScope*>(data_)
             : get<Impl*>(data_)->parent;
}

std::optional<StringPiece> EdgeBindings::FindBinding(StringPiece var) const {
  const Impl* impl = get_if<Impl*>(&data_);
  if (!impl)
    return std::nullopt;
  const std::vector<KeyValuePair>& bindings = impl->bindings;
  const auto i = std::lower_bound(bindings.begin(), bindings.end(), var,
                                  BindingKeyLess());
  if (i == bindings.end() || i->first != var)
    return std::nullopt;
  return StringPiece(i->second);
}

void EdgeBindings::AddBinding(const std::string& key, StringPiece val) {
  std::vector<KeyValuePair> bindings;
  // The Impl is about to be replaced, so take its bindings rather than
  // copying them.
  if (Impl* impl = get_if<Impl*>(&data_))
    bindings = std::move(impl->bindings);
  bindings.emplace_back(key, val.AsString());
  SetBindings(bindings.data(), bindings.data() + bindings.size());
}

void EdgeBindings::SetBindings(KeyValuePair* begin, KeyValuePair* end) {
  // Work on the reversed range so that, for each key, the last binding comes
  // first and is the one that unique keeps.  Sorting it by descending key
  // leaves the range itself in ascending order, and unique moves the kept
  // bindings to the back of the range, so they end up in [begin, end).
  const std::reverse_iterator<KeyValuePair*> rbegin(end), rend(begin);
  std::stable_sort(rbegin, rend,
                   [](const KeyValuePair& a, const KeyValuePair& b) {
                     return StringPieceLess()(b.first, a.first);
                   });
  begin = std::unique(rbegin, rend,
                      [](const KeyValuePair& a, const KeyValuePair& b) {
                        return a.first == b.first;
                      })
              .base();

  Impl* const old = get_if<Impl*>(&data_);
  if (begin == end) {
    if (old) {
      data_ = old->parent;
      delete old;
    }
    return;
  }

  auto impl = std::make_unique<Impl>(parent());
  impl->bindings.assign(begin, end);
  data_ = impl.release();
  delete old;
}
