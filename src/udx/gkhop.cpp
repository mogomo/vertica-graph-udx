// gkhop: k-hop neighbourhood of each request row. Output (start, node, hops).
// Thin adapter around src/engine/bfs.h. Input handling is in udx_common.h.
#include "udx_common.h"
#include "../engine/bfs.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gkhop";

class GKhop : public TransformFunction
{
    vgraph::KhopOptions opt_;

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

            const vgraph::CsrGraph graph = q.graph();
            vgraph::BfsScratch scratch;
            for (const Request &r : q.requests) {
                vgraph::pos_t start_pos;
                if (!graph.find(r.start, start_pos)) {
                    // Unknown node: it has no edges, so it only reaches itself.
                    if (!opt_.exact || opt_.depth == 0) {
                        outputWriter.setInt(0, r.start);
                        outputWriter.setInt(1, r.start);
                        outputWriter.setInt(2, 0);
                        outputWriter.next();
                    }
                    continue;
                }
                vgraph::khop(graph, start_pos, opt_, scratch, [&](vgraph::pos_t p, std::int64_t hops) {
                    outputWriter.setInt(0, r.start);
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
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("depth");
        parameterTypes.addBool("exact");
        parameterTypes.addVarchar(8, "direction");
        parameterTypes.addInt("max_results");
        parameterTypes.addInt("start");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GKhop>(srvInterface.allocator); }
};

RegisterFactory(GKhopFactory);
