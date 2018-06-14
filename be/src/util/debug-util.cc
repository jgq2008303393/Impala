// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "util/debug-util.h"

#include <iomanip>
#include <sstream>
#include <boost/tokenizer.hpp>

#include "common/version.h"
#include "runtime/collection-value.h"
#include "runtime/descriptors.h"
#include "runtime/raw-value.inline.h"
#include "runtime/tuple-row.h"
#include "runtime/row-batch.h"
#include "util/cpu-info.h"
#include "util/string-parser.h"
#include "util/uid-util.h"
#include "util/time.h"

// / WARNING this uses a private API of GLog: DumpStackTraceToString().
namespace google {
namespace glog_internal_namespace_ {
extern void DumpStackTraceToString(std::string* s);
}
}

#include "common/names.h"

using boost::char_separator;
using boost::tokenizer;
using namespace beeswax;
using namespace parquet;

DECLARE_int32(be_port);
DECLARE_string(hostname);

namespace impala {

#define PRINT_THRIFT_ENUM_IMPL(T) \
  string PrintThriftEnum(const T::type& value) { \
    map<int, const char*>::const_iterator it = _##T##_VALUES_TO_NAMES.find(value); \
    return it == _##T##_VALUES_TO_NAMES.end() ? std::to_string(value) : it->second; \
  }

PRINT_THRIFT_ENUM_IMPL(QueryState)
PRINT_THRIFT_ENUM_IMPL(Encoding)
PRINT_THRIFT_ENUM_IMPL(TCatalogObjectType)
PRINT_THRIFT_ENUM_IMPL(TCatalogOpType)
PRINT_THRIFT_ENUM_IMPL(TDdlType)
PRINT_THRIFT_ENUM_IMPL(TExplainLevel)
PRINT_THRIFT_ENUM_IMPL(THdfsCompression)
PRINT_THRIFT_ENUM_IMPL(THdfsFileFormat)
PRINT_THRIFT_ENUM_IMPL(THdfsSeqCompressionMode)
PRINT_THRIFT_ENUM_IMPL(TImpalaQueryOptions)
PRINT_THRIFT_ENUM_IMPL(TJoinDistributionMode)
PRINT_THRIFT_ENUM_IMPL(TMetricKind)
PRINT_THRIFT_ENUM_IMPL(TParquetArrayResolution)
PRINT_THRIFT_ENUM_IMPL(TParquetFallbackSchemaResolution)
PRINT_THRIFT_ENUM_IMPL(TPlanNodeType)
PRINT_THRIFT_ENUM_IMPL(TPrefetchMode)
PRINT_THRIFT_ENUM_IMPL(TReplicaPreference)
PRINT_THRIFT_ENUM_IMPL(TRuntimeFilterMode)
PRINT_THRIFT_ENUM_IMPL(TSessionType)
PRINT_THRIFT_ENUM_IMPL(TStmtType)
PRINT_THRIFT_ENUM_IMPL(TUnit)

string PrintId(const TUniqueId& id, const string& separator) {
  stringstream out;
  out << hex << id.hi << separator << id.lo;
  return out.str();
}

bool ParseId(const string& s, TUniqueId* id) {
  // For backwards compatibility, this method parses two forms of query ID from text:
  //  - <hex-int64_t><colon><hex-int64_t> - this format is the standard going forward
  //  - <decimal-int64_t><space><decimal-int64_t> - legacy compatibility with CDH4 CM
  DCHECK(id != NULL);

  const char* hi_part = s.c_str();
  char* separator = const_cast<char*>(strchr(hi_part, ':'));
  if (separator == NULL) {
    // Legacy compatibility branch
    char_separator<char> sep(" ");
    tokenizer< char_separator<char>> tokens(s, sep);
    int i = 0;
    for (const string& token: tokens) {
      StringParser::ParseResult parse_result = StringParser::PARSE_SUCCESS;
      int64_t component = StringParser::StringToInt<int64_t>(
          token.c_str(), token.length(), &parse_result);
      if (parse_result != StringParser::PARSE_SUCCESS) return false;
      if (i == 0) {
        id->hi = component;
      } else if (i == 1) {
        id->lo = component;
      } else {
        // Too many tokens, must be ill-formed.
        return false;
      }
      ++i;
    }
    return true;
  }

  // Parse an ID from <int64_t_as_hex><colon><int64_t_as_hex>
  const char* lo_part = separator + 1;
  *separator = '\0';

  char* error_hi = NULL;
  char* error_lo = NULL;
  id->hi = strtoul(hi_part, &error_hi, 16);
  id->lo = strtoul(lo_part, &error_lo, 16);

  bool valid = *error_hi == '\0' && *error_lo == '\0';
  *separator = ':';
  return valid;
}

string PrintTuple(const Tuple* t, const TupleDescriptor& d) {
  if (t == NULL) return "null";
  stringstream out;
  out << "(";
  bool first_value = true;
  for (int i = 0; i < d.slots().size(); ++i) {
    SlotDescriptor* slot_d = d.slots()[i];
    if (first_value) {
      first_value = false;
    } else {
      out << " ";
    }
    if (t->IsNull(slot_d->null_indicator_offset())) {
      out << "null";
    } else if (slot_d->type().IsCollectionType()) {
      const TupleDescriptor* item_d = slot_d->collection_item_descriptor();
      const CollectionValue* coll_value =
          reinterpret_cast<const CollectionValue*>(t->GetSlot(slot_d->tuple_offset()));
      uint8_t* coll_buf = coll_value->ptr;
      out << "[";
      for (int j = 0; j < coll_value->num_tuples; ++j) {
        out << PrintTuple(reinterpret_cast<Tuple*>(coll_buf), *item_d);
        coll_buf += item_d->byte_size();
      }
      out << "]";
    } else {
      string value_str;
      RawValue::PrintValue(
          t->GetSlot(slot_d->tuple_offset()), slot_d->type(), -1, &value_str);
      out << value_str;
    }
  }
  out << ")";
  return out.str();
}

string PrintRow(TupleRow* row, const RowDescriptor& d) {
  stringstream out;
  out << "[";
  for (int i = 0; i < d.tuple_descriptors().size(); ++i) {
    if (i != 0) out << " ";
    out << PrintTuple(row->GetTuple(i), *d.tuple_descriptors()[i]);
  }
  out << "]";
  return out.str();
}

string PrintBatch(RowBatch* batch) {
  stringstream out;
  for (int i = 0; i < batch->num_rows(); ++i) {
    out << PrintRow(batch->GetRow(i), *batch->row_desc()) << "\n";
  }
  return out.str();
}

string PrintPath(const TableDescriptor& tbl_desc, const SchemaPath& path) {
  stringstream ss;
  ss << tbl_desc.database() << "." << tbl_desc.name();
  const ColumnType* type = NULL;
  if (path.size() > 0) {
    ss << "." << tbl_desc.col_descs()[path[0]].name();
    type = &tbl_desc.col_descs()[path[0]].type();
  }
  for (int i = 1; i < path.size(); ++i) {
    ss << ".";
    switch (type->type) {
      case TYPE_ARRAY:
        if (path[i] == 0) {
          ss << "item";
          type = &type->children[0];
        } else {
          DCHECK_EQ(path[i], 1);
          ss << "pos";
          type = NULL;
        }
        break;
      case TYPE_MAP:
        if (path[i] == 0) {
          ss << "key";
          type = &type->children[0];
        } else if (path[i] == 1) {
          ss << "value";
          type = &type->children[1];
        } else {
          DCHECK_EQ(path[i], 2);
          ss << "pos";
          type = NULL;
        }
        break;
      case TYPE_STRUCT:
        DCHECK_LT(path[i], type->children.size());
        ss << type->field_names[path[i]];
        type = &type->children[path[i]];
        break;
      default:
        DCHECK(false) << PrintNumericPath(path) << " " << i << " " << type->DebugString();
        return PrintNumericPath(path);
    }
  }
  return ss.str();
}

string PrintSubPath(const TableDescriptor& tbl_desc, const SchemaPath& path,
    int end_path_idx) {
  DCHECK_GE(end_path_idx, 0);
  SchemaPath::const_iterator subpath_end = path.begin() + end_path_idx + 1;
  SchemaPath subpath(path.begin(), subpath_end);
  return PrintPath(tbl_desc, subpath);
}

string PrintNumericPath(const SchemaPath& path) {
  stringstream ss;
  ss << "[";
  if (path.size() > 0) ss << path[0];
  for (int i = 1; i < path.size(); ++i) {
    ss << " ";
    ss << path[i];
  }
  ss << "]";
  return ss.str();
}

string GetBuildVersion(bool compact) {
  stringstream ss;
  ss << GetDaemonBuildVersion()
#ifdef NDEBUG
     << " RELEASE"
#else
     << " DEBUG"
#endif
     << " (build " << GetDaemonBuildHash()
     << ")";
  if (!compact) {
    ss << endl << "Built on " << GetDaemonBuildTime();
  }
  return ss.str();
}

string GetVersionString(bool compact) {
  stringstream ss;
  ss << google::ProgramInvocationShortName()
     << " version " << GetBuildVersion(compact);
  return ss.str();
}

string GetStackTrace() {
  string s;
  google::glog_internal_namespace_::DumpStackTraceToString(&s);
  return s;
}

string GetBackendString() {
  return Substitute("$0:$1", FLAGS_hostname, FLAGS_be_port);
}

void SleepIfSetInDebugOptionsImpl(
    const TQueryOptions& query_options, const string& sleep_label) {
  vector<string> components;
  boost::split(components, query_options.debug_action, boost::is_any_of(":"));
  if (components.size() == 2 && components[0].compare(sleep_label) == 0) {
    SleepForMs(atoi(components[1].c_str()));
  }
}

}
