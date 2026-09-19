// Helpers shared by the vgraph UDx adapters.
#ifndef VGRAPH_UDX_COMMON_H
#define VGRAPH_UDX_COMMON_H

#include "Vertica.h"
#include "../engine/builder.h"
#include "../engine/cache.h"
#include "../engine/csr.h"

#include <string>
#include <vector>

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

// cache_dir precedence: function parameter, then session parameter
// (ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '...'), then the default.
inline std::string resolve_cache_dir(Vertica::ServerInterface &srvInterface)
{
    Vertica::ParamReader params = srvInterface.getParamReader();
    if (params.containsParameter("cache_dir")) return params.getStringRef("cache_dir").str();
    Vertica::ParamReader session = srvInterface.getUDSessionParamReader("library");
    if (session.containsParameter("cache_dir")) return session.getStringRef("cache_dir").str();
    return vgraph::DEFAULT_CACHE_DIR;
}

struct Request {
    Vertica::vint start;
    Vertica::vint target;
    bool has_target;
};

// The graph a query function runs on, plus the request rows of its input.
//   graph parameter given: the active snapshot of that graph, from the node's cache.
//   no graph parameter (inline mode): built from the edge rows of the input.
class QueryGraph {
public:
    // Reads the whole partition. Returns false if the query was canceled.
    template <class Canceled>
    bool read(const char *fn, Vertica::ServerInterface &srvInterface, Vertica::PartitionReader &in,
              Canceled canceled)
    {
        Vertica::ParamReader params = srvInterface.getParamReader();
        const bool inline_mode = !params.containsParameter("graph");
        vgraph::GraphBuilder builder(true, false);
        Vertica::vint rows = 0, wanted_epoch = -1;

        do {
            if (!in.isNull(COL_SRC)) {
                if (!inline_mode)
                    vt_report_error(0, "%s: edge rows in the input are not supported yet together with the "
                                       "graph parameter", fn);
                if (in.isNull(COL_DST))
                    vt_report_error(0, "%s: edge row with src %lld has no dst", fn, (long long)in.getIntRef(COL_SRC));
                if (!in.isNull(COL_OP) && in.getIntRef(COL_OP) != 1)
                    vt_report_error(0, "%s: op must be 1 or NULL when the graph is built from the input "
                                       "(found %lld)", fn, (long long)in.getIntRef(COL_OP));
                builder.add_edge(in.getIntRef(COL_SRC), in.getIntRef(COL_DST));
            } else if (!in.isNull(COL_START)) {
                Request r;
                r.start = in.getIntRef(COL_START);
                r.has_target = !in.isNull(COL_TARGET);
                r.target = r.has_target ? in.getIntRef(COL_TARGET) : 0;
                requests.push_back(r);
            }
            if (!in.isNull(COL_SNAPSHOT_EPOCH) && in.getIntRef(COL_SNAPSHOT_EPOCH) > wanted_epoch)
                wanted_epoch = in.getIntRef(COL_SNAPSHOT_EPOCH);
            if ((++rows & 0xFFFFF) == 0 && canceled()) return false;
        } while (in.next());

        if (inline_mode) {
            builder.finish(0, buffer_);
            csr_ = vgraph::snapshot_open(buffer_.data(), buffer_.size(), false);
        } else {
            mapped_.open_active(resolve_cache_dir(srvInterface), params.getStringRef("graph").str());
            csr_ = mapped_.csr();
            // The manifest already points to a newer snapshot than this node has cached.
            if (csr_.max_epoch < wanted_epoch)
                vt_report_error(0, "%s: snapshot cache stale on %s: run gload", fn,
                                srvInterface.getCurrentNodeName().c_str());
        }
        return true;
    }

    vgraph::CsrGraph graph() const { return vgraph::CsrGraph(csr_); }
    std::vector<Request> requests;

private:
    vgraph::MappedSnapshot mapped_;
    vgraph::SnapshotBuffer buffer_;
    vgraph::Csr csr_;
};

inline void add_common_query_parameters(Vertica::SizedColumnTypes &parameterTypes)
{
    parameterTypes.addVarchar(128, "graph");
    parameterTypes.addVarchar(1024, "cache_dir");
}

} // namespace vgraph_udx

#endif
