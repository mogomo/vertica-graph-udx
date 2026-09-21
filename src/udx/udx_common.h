// Helpers shared by the vgraph UDx adapters.
#ifndef VGRAPH_UDX_COMMON_H
#define VGRAPH_UDX_COMMON_H

#include "Vertica.h"
#include "../engine/builder.h"
#include "../engine/cache.h"
#include "../engine/csr.h"
#include "../engine/delta.h"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vgraph_udx {

// Input columns of the query functions:
//   start INT, target INT          a request row
//   src INT, dst INT, del BOOLEAN, weight FLOAT, ver INT     a journal (delta) row
//   snapshot_id INT                on every row of a delta view: the snapshot the view belongs to
// del is a BOOLEAN on purpose: 1 byte per row instead of 8.
enum InputColumn { COL_START = 0, COL_TARGET, COL_SRC, COL_DST, COL_DEL, COL_WEIGHT, COL_VER, COL_SNAPSHOT_ID, COL_COUNT };

inline void add_query_input(Vertica::ColumnTypes &argTypes)
{
    argTypes.addInt();      // start
    argTypes.addInt();      // target
    argTypes.addInt();      // src
    argTypes.addInt();      // dst
    argTypes.addBool();     // del
    argTypes.addFloat();    // weight
    argTypes.addInt();      // ver
    argTypes.addInt();      // snapshot_id
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
//   graph parameter given: the active snapshot of that graph from the node's
//     cache, with the journal rows of the input applied on top (delta overlay).
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
        Vertica::vint rows = 0, wanted_snapshot = -1;

        do {
            if (!in.isNull(COL_SRC)) {
                if (in.isNull(COL_DST))
                    vt_report_error(0, "%s: edge row with src %lld has no dst", fn, (long long)in.getIntRef(COL_SRC));
                const bool del = !in.isNull(COL_DEL) && in.getBoolRef(COL_DEL) == Vertica::vbool_true;
                const bool has_weight = !in.isNull(COL_WEIGHT);
                const float weight = has_weight ? (float)in.getFloatRef(COL_WEIGHT) : 1.0f;
                if (inline_mode) {
                    if (del)
                        vt_report_error(0, "%s: deleted edges (del = true) need a snapshot: use the graph parameter", fn);
                    if (has_weight) builder.enable_weights();
                    builder.add_edge(in.getIntRef(COL_SRC), in.getIntRef(COL_DST), weight);
                } else {
                    DeltaRow d;
                    d.src = in.getIntRef(COL_SRC);
                    d.dst = in.getIntRef(COL_DST);
                    d.ver = in.isNull(COL_VER) ? 0 : in.getIntRef(COL_VER);
                    d.weight = weight;
                    d.del = del;
                    delta_.push_back(d);
                }
            } else if (!in.isNull(COL_START)) {
                Request r;
                r.start = in.getIntRef(COL_START);
                r.has_target = !in.isNull(COL_TARGET);
                r.target = r.has_target ? in.getIntRef(COL_TARGET) : 0;
                requests.push_back(r);
            }
            if (!in.isNull(COL_SNAPSHOT_ID) && in.getIntRef(COL_SNAPSHOT_ID) > wanted_snapshot)
                wanted_snapshot = in.getIntRef(COL_SNAPSHOT_ID);
            if ((++rows & 0xFFFFF) == 0 && canceled()) return false;
        } while (in.next());

        if (inline_mode) {
            builder.finish(0, buffer_);
            csr_ = vgraph::snapshot_open(buffer_.data(), buffer_.size(), false);
            return true;
        }
        mapped_.open_active(resolve_cache_dir(srvInterface), params.getStringRef("graph").str());
        csr_ = mapped_.csr();
        // The delta view belongs to a newer snapshot than this node has cached.
        if (mapped_.snapshot_id() < wanted_snapshot)
            vt_report_error(0, "%s: snapshot cache stale on %s: run gload", fn,
                            srvInterface.getCurrentNodeName().c_str());
        if (!delta_.empty()) {
            // Last op wins: apply in version order. Stable, so equal versions keep their input order.
            std::stable_sort(delta_.begin(), delta_.end(),
                             [](const DeltaRow &a, const DeltaRow &b) { return a.ver < b.ver; });
            overlay_.reset(new vgraph::Overlay(csr_));
            for (const DeltaRow &d : delta_) overlay_->apply(d.src, d.dst, d.del ? -1 : 1, d.weight);
            std::vector<DeltaRow>().swap(delta_);
            if (overlay_->empty()) overlay_.reset();      // every row was already in the snapshot
        }
        return true;
    }

    // Calls f(graph) with the overlay when the delta changed something, else with the plain snapshot.
    template <class F> void run(F &&f) const
    {
        if (overlay_) f(*overlay_);
        else f(vgraph::CsrGraph(csr_));
    }
    bool weighted() const { return csr_.weighted; }
    std::vector<Request> requests;

private:
    struct DeltaRow {
        Vertica::vint src, dst, ver;
        float weight;
        bool del;
    };
    vgraph::MappedSnapshot mapped_;
    vgraph::SnapshotBuffer buffer_;
    vgraph::Csr csr_;
    std::vector<DeltaRow> delta_;
    std::unique_ptr<vgraph::Overlay> overlay_;
};

inline void add_common_query_parameters(Vertica::SizedColumnTypes &parameterTypes)
{
    parameterTypes.addVarchar(128, "graph");
    parameterTypes.addVarchar(1024, "cache_dir");
}

// threads: worker threads of gkhop, gkhop_count, gpath, gcomponents and gpagerank. Default: one per core
// of the node (at most 64); 1 switches them off. Small searches never start a thread.
// They run outside Vertica's resource pools and end before the function returns.
inline int read_threads(const char *fn, Vertica::ServerInterface &srvInterface)
{
    Vertica::ParamReader params = srvInterface.getParamReader();
    Vertica::vint threads = std::thread::hardware_concurrency();
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (params.containsParameter("threads")) threads = params.getIntRef("threads");
    if (threads < 1 || threads > 64) vt_report_error(0, "%s: threads must be between 1 and 64", fn);
    return static_cast<int>(threads);
}

} // namespace vgraph_udx

#endif
