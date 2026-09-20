// gkhop:       k-hop neighbourhood of each request row. Output (start, node, hops).
// gkhop_count: same search, but only the number of nodes per hop count.
//              Output (start, hops, nodes). A deep search finds millions of nodes;
//              returning them costs more than finding them.
// Thin adapter around src/engine/bfs.h and bfs_parallel.h (several threads; small searches stay on
// one). max_results uses the single-threaded search: which nodes come first must not depend on threads.
// Input handling is in udx_common.h.
#include "udx_common.h"
#include "../engine/bfs.h"
#include "../engine/bfs_parallel.h"

#include <vector>

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gkhop";

class GKhop : public TransformFunction
{
protected:
    vgraph::KhopOptions opt_;
    bool count_only_ = false;
    int threads_ = 1;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        ParamReader params = srvInterface.getParamReader();
        if (!params.containsParameter("depth"))
            vt_report_error(0, "%s: parameter depth is required", FN);
        opt_.depth = params.getIntRef("depth");
        if (opt_.depth < 0) vt_report_error(0, "%s: depth must be 0 or more", FN);
        if (params.containsParameter("exact")) opt_.exact = params.getBoolRef("exact") == vbool_true;
        if (params.containsParameter("direction"))
            opt_.direction = parse_direction(FN, params.getStringRef("direction").str());
        if (params.containsParameter("max_results")) {
            opt_.max_results = params.getIntRef("max_results");
            if (opt_.max_results < 0) vt_report_error(0, "%s: max_results must be 0 or more", FN);
        }
        threads_ = read_threads(FN, srvInterface);
    }

    // One row per node found. Kept small and separate from count_rows: this loop writes millions
    // of rows, and the compiler must be able to inline the row writer into the search.
    template <class G>
    void node_rows(const G &graph, const std::vector<Request> &requests, PartitionWriter &outputWriter)
    {
        static const size_t BATCH = 4096;
        vgraph::BfsScratch scratch;
        vgraph::ParallelBfsScratch pscratch;
        std::vector<vgraph::pos_t> batch;
        batch.reserve(BATCH);
        for (const Request &r : requests) {
            const vint start = r.start;
            vgraph::pos_t start_pos;
            if (!graph.find(start, start_pos)) {
                // Unknown node: it has no edges, so it only reaches itself.
                if (!opt_.exact || opt_.depth == 0) {
                    outputWriter.setInt(0, start);
                    outputWriter.setInt(1, start);
                    outputWriter.setInt(2, 0);
                    outputWriter.next();
                }
                continue;
            }
            if (opt_.max_results == 0) {
                // level by level on several threads; the rows of a level are written here
                vgraph::khop_parallel(graph, start_pos, opt_, pscratch, threads_, true,
                    [&](std::int64_t hops, std::int64_t, const std::vector<std::vector<vgraph::pos_t>> *lists) {
                        for (const auto &list : *lists) {
                            for (vgraph::pos_t p : list) {
                                outputWriter.setInt(0, start);
                                outputWriter.setInt(1, graph.id_of(p));
                                outputWriter.setInt(2, hops);
                                outputWriter.next();
                            }
                        }
                    });
                if (isCanceled()) return;
                continue;
            }
            // The search fills a small batch; the rows are written in a tight loop of their own.
            batch.clear();
            std::int64_t batch_hops = 0;
            auto flush = [&]() {
                for (vgraph::pos_t p : batch) {
                    outputWriter.setInt(0, start);
                    outputWriter.setInt(1, graph.id_of(p));
                    outputWriter.setInt(2, batch_hops);
                    outputWriter.next();
                }
                batch.clear();
            };
            vgraph::khop(graph, start_pos, opt_, scratch, [&](vgraph::pos_t p, std::int64_t hops) {
                if (hops != batch_hops || batch.size() == BATCH) {
                    flush();
                    batch_hops = hops;
                }
                batch.push_back(p);
            });
            flush();
            if (isCanceled()) return;
        }
    }

    // One row per hop count.
    template <class G>
    void count_rows(const G &graph, const std::vector<Request> &requests, PartitionWriter &outputWriter)
    {
        vgraph::BfsScratch scratch;
        vgraph::ParallelBfsScratch pscratch;
        std::vector<vint> per_hop;
        for (const Request &r : requests) {
            vgraph::pos_t start_pos;
            per_hop.assign(1, 0);
            if (!graph.find(r.start, start_pos)) {
                if (!opt_.exact || opt_.depth == 0) per_hop[0] = 1;     // an unknown node reaches itself
            } else if (opt_.max_results == 0) {
                vgraph::khop_parallel(graph, start_pos, opt_, pscratch, threads_, false,
                    [&](std::int64_t hops, std::int64_t count, const std::vector<std::vector<vgraph::pos_t>> *) {
                        if ((size_t)hops >= per_hop.size()) per_hop.resize(hops + 1, 0);
                        per_hop[hops] += count;
                    });
            } else {
                vgraph::khop(graph, start_pos, opt_, scratch, [&](vgraph::pos_t, std::int64_t hops) {
                    if ((size_t)hops >= per_hop.size()) per_hop.resize(hops + 1, 0);
                    ++per_hop[hops];
                });
            }
            for (size_t h = 0; h < per_hop.size(); ++h) {
                if (per_hop[h] == 0) continue;
                outputWriter.setInt(0, r.start);
                outputWriter.setInt(1, (vint)h);
                outputWriter.setInt(2, per_hop[h]);
                outputWriter.next();
            }
            if (isCanceled()) return;
        }
    }

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            QueryGraph q;
            if (!q.read(FN, srvInterface, inputReader, [this]() { return isCanceled(); })) return;

            ParamReader params = srvInterface.getParamReader();
            if (params.containsParameter("start")) {
                Request r = {params.getIntRef("start"), 0, false};
                q.requests.push_back(r);
            }
            if (q.requests.empty())
                vt_report_error(0, "%s: no start node: add a request row or the start parameter", FN);

            if (count_only_) q.run([&](const auto &graph) { count_rows(graph, q.requests, outputWriter); });
            else             q.run([&](const auto &graph) { node_rows(graph, q.requests, outputWriter); });
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GKhopCount : public GKhop
{
public:
    GKhopCount() { count_only_ = true; }
};

class GKhopFactory : public TransformFunctionFactory
{
protected:
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        add_query_input(argTypes);
        returnType.addInt();
        returnType.addInt();
        returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("start");
        outputTypes.addInt("node");
        outputTypes.addInt("hops");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("depth");
        parameterTypes.addBool("exact");
        parameterTypes.addVarchar(8, "direction");
        parameterTypes.addInt("max_results");
        parameterTypes.addInt("start");
        parameterTypes.addInt("threads");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GKhop>(srvInterface.allocator); }
};

class GKhopCountFactory : public GKhopFactory
{
    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("start");
        outputTypes.addInt("hops");
        outputTypes.addInt("nodes");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GKhopCount>(srvInterface.allocator); }
};

RegisterFactory(GKhopFactory);
RegisterFactory(GKhopCountFactory);
