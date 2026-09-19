// Helpers shared by the vgraph UDx adapters.
#ifndef VGRAPH_UDX_COMMON_H
#define VGRAPH_UDX_COMMON_H

#include "Vertica.h"
#include "../engine/csr.h"

#include <string>

namespace vgraph_udx {

// Input columns of the query functions:
// (start, target, src, dst, op, epoch, snapshot_epoch), all INT.
enum InputColumn { COL_START = 0, COL_TARGET, COL_SRC, COL_DST, COL_OP, COL_EPOCH, COL_SNAPSHOT_EPOCH, COL_COUNT };

inline void add_query_input(Vertica::ColumnTypes &argTypes)
{
    for (int i = 0; i < COL_COUNT; ++i) argTypes.addInt();
}

inline vgraph::Direction parse_direction(const char *function, const std::string &value)
{
    if (value == "out") return vgraph::Direction::Out;
    if (value == "in") return vgraph::Direction::In;
    if (value == "both") return vgraph::Direction::Both;
    vt_report_error(0, "%s: direction must be out, in or both, not '%s'", function, value.c_str());
    return vgraph::Direction::Out;
}

} // namespace vgraph_udx

#endif
