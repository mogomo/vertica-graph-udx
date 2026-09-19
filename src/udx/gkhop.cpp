// gkhop: k-hop neighbourhood of each request row. Output (start, node, hops).
// Thin adapter around src/engine/bfs.h.
//
// Inline mode (no `graph` parameter): the graph is built from the edge rows of
// the input. Edge rows have src and dst set. Request rows have start set.
// Rows with neither (the delta view sentinel) are skipped.
#include "udx_common.h"
#include "../engine/bfs.h"
#include "../engine/builder.h"
#include "../engine/snapshot.h"

#include <vector>

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gkhop";

class GKhop : public TransformFunction
{
    vgraph::KhopOptions opt_;
    std::vector<vint> requests_;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        ParamReader params = srvInterface.getParamReader();
        if (params.containsParameter("graph"))
            vt_report_error(0, "%s: parameter graph is not supported yet; leave it out to build the graph "
                               "from the edge rows of the input", FN);
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
        if (params.containsParameter("start")) requests_.push_back(params.getIntRef("start"));
    }

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            vgraph::GraphBuilder builder(true, false);
            vint rows = 0;
            do {
                if (!inputReader.isNull(COL_SRC)) {
                    if (inputReader.isNull(COL_DST))
                        vt_report_error(0, "%s: edge row with src %lld has no dst", FN,
                                        (long long)inputReader.getIntRef(COL_SRC));
                    if (!inputReader.isNull(COL_OP) && inputReader.getIntRef(COL_OP) != 1)
                        vt_report_error(0, "%s: op must be 1 or NULL when the graph is built from the input "
                                           "(found %lld)", FN, (long long)inputReader.getIntRef(COL_OP));
                    builder.add_edge(inputReader.getIntRef(COL_SRC), inputReader.getIntRef(COL_DST));
                } else if (!inputReader.isNull(COL_START)) {
                    requests_.push_back(inputReader.getIntRef(COL_START));
                }
                if ((++rows & 0xFFFFF) == 0 && isCanceled()) return;
            } while (inputReader.next());

            if (requests_.empty())
                vt_report_error(0, "%s: no start node: add a request row or the start parameter", FN);

            vgraph::SnapshotBuffer buffer;
            builder.finish(0, buffer);
            vgraph::CsrGraph graph(vgraph::snapshot_open(buffer.data(), buffer.size(), false));

            vgraph::BfsScratch scratch;
            for (vint start : requests_) {
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
                vgraph::khop(graph, start_pos, opt_, scratch, [&](vgraph::pos_t p, std::int64_t hops) {
                    outputWriter.setInt(0, start);
                    outputWriter.setInt(1, graph.id_of(p));
                    outputWriter.setInt(2, hops);
                    outputWriter.next();
                });
                if (isCanceled()) return;
            }
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GKhopFactory : public TransformFunctionFactory
{
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
        parameterTypes.addVarchar(128, "graph");
        parameterTypes.addInt("depth");
        parameterTypes.addBool("exact");
        parameterTypes.addVarchar(8, "direction");
        parameterTypes.addInt("max_results");
        parameterTypes.addVarchar(1024, "cache_dir");
        parameterTypes.addInt("start");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GKhop>(srvInterface.allocator); }
};

RegisterFactory(GKhopFactory);
