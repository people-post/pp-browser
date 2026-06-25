#pragma once

#include <string>
#include <vector>

namespace pbr {

enum class WorkingSetKind { LongList, Form, Calendar, Table, Code, KeyValue, Card, None };

enum class WorkingSetAffinity {
  None,
  Feed,
  Form,
  DataTable,
  Document,
};

struct WorkingSetCandidate {
  int block_index = -1;
  WorkingSetKind kind = WorkingSetKind::None;
  WorkingSetAffinity affinity = WorkingSetAffinity::None;
  bool auto_open = false;
  std::string title;
  std::string subtitle;
  std::string artifact_rml;
  std::string teaser_rml;
};

} // namespace pbr
