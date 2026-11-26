// Exports of a workflow definition's graph (`praenora graph`, Runtime::graph()).
#ifndef PRAENORA_RUNTIME_API_GRAPH_EXPORT_HPP
#define PRAENORA_RUNTIME_API_GRAPH_EXPORT_HPP

#include "catalog/catalog.hpp"

#include <praenora/runtime/options.hpp>

#include <string>

namespace praenora::detail {

// The graph of `definition`, whose entries `catalog` holds, in `format`; every format ends in a
// newline:
// - text: "workflow: <name>@<version>", "schedule: <step>, <step>, ...", then "connections:" and
//   one line per connection in definition order, "  <from> -> <to>", with " [delay 1]" for a
//   delayed one;
// - json: {"workflow": {"name", "version"}, "schedule": [step ids],
//   "connections": [{"from", "to", "delay", "type", "type_version"}]}, two-space indented;
// - dot: digraph "<name>@<version>" with the nodes "inputs" (when the definition has inputs),
//   each step in schedule order and "outputs" (when it has outputs), and one edge per connection
//   labeled "<from port> -> <to port>", delayed edges dashed;
// - asyncapi: an AsyncAPI 3.0.0 document, two-space indented with sorted keys: info (title the
//   name, version, description the summary when there is one), defaultContentType
//   application/json, one channel per connection source (key: the endpoint; address
//   "<name>/<endpoint>"; its message, by message type), one operation "<endpoint>.publish"
//   (action send) per channel, and one components message per message type ({name: the type,
//   title "<type>@<version>", contentType, payload}). A payload is the pmp-envelope schema with
//   properties.data replaced by the message type's schema; both are made self-contained: their
//   local references ("$ref": "#/...") are replaced by what they point to, and "$schema", "$id"
//   (of the envelope), "$defs" and "definitions" are dropped, so the document holds no duplicate
//   $id and every reference resolves within it.
// Throws praenora::Error: the first error of validate_graph() for an invalid definition;
// CATALOG_ENTRY_NOT_FOUND for a message type the catalog lacks; UNSUPPORTED (asyncapi) when the
// definition carries two versions of one message type, or a schema has a recursive local
// reference.
std::string export_graph(const WorkflowDef& definition, const Catalog& catalog, GraphFormat format);

} // namespace praenora::detail

#endif // PRAENORA_RUNTIME_API_GRAPH_EXPORT_HPP
