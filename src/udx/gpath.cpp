// gpath: shortest path from start to target for each request row.
// Output (start, target, hop_no, node); hop_no 0 is the start node.
// No rows for a request without a path. Thin adapter around src/engine/path.h.
#include "udx_common.h"
#include "../engine/path.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gpath";

class GPath : public TransformFunction
{
    vgraph::PathOptions opt_;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        ParamReader params = srvInterface.getParamReader();
        if (params.containsParameter("max_depth")) {
            opt_.max_depth = params.getIntRef("max_depth");
            if (opt_.max_depth < 0) vt_report_error(0, "%s: max_depth must be 0 or more (0 = no limit)", FN);
        }
        if (params.containsParameter("direction"))
            opt_.direction = parse_direction(FN, params.getStringRef("direction").str());
        if (params.containsParameter("weighted")) opt_.weighted = params.getBoolRef("weighted") == vbool_true;
        if (params.containsParameter("start") != params.containsParameter("target"))
            vt_report_error(0, "%s: parameters start and target must be given together", FN);
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
                Request r = {params.getIntRef("start"), params.getIntRef("target"), true};
                q.requests.push_back(r);
            }
            if (q.requests.empty())
                vt_report_error(0, "%s: no request: add a row with start and target, or both parameters", FN);

            const vgraph::CsrGraph graph = q.graph();
            if (opt_.weighted && !graph.csr().weighted)
                vt_report_error(0, "%s: weighted=true, but the graph has no weights", FN);

            vgraph::PathScratch scratch;
            std::vector<vgraph::pos_t> path;
            for (const Request &r : q.requests) {
                if (!r.has_target)
                    vt_report_error(0, "%s: request row with start %lld has no target", FN, (long long)r.start);
                vgraph::pos_t s, t;
                if (!graph.find(r.start, s) || !graph.find(r.target, t)) {
                    if (r.start != r.target) continue;      // unknown node: no path
                    outputWriter.setInt(0, r.start);        // a node always reaches itself
                    outputWriter.setInt(1, r.target);
                    outputWriter.setInt(2, 0);
                    outputWriter.setInt(3, r.start);
                    outputWriter.next();
                    continue;
                }
                vgraph::shortest_path(graph, s, t, opt_, scratch, path);
                for (size_t i = 0; i < path.size(); ++i) {
                    outputWriter.setInt(0, r.start);
                    outputWriter.setInt(1, r.target);
                    outputWriter.setInt(2, (vint)i);
                    outputWriter.setInt(3, graph.id_of(path[i]));
                    outputWriter.next();
                }
                if (isCanceled()) return;
            }
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GPathFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        add_query_input(argTypes);
        for (int i = 0; i < 4; ++i) returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("start");
        outputTypes.addInt("target");
        outputTypes.addInt("hop_no");
        outputTypes.addInt("node");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("max_depth");
        parameterTypes.addVarchar(8, "direction");
        parameterTypes.addBool("weighted");
        parameterTypes.addInt("start");
        parameterTypes.addInt("target");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GPath>(srvInterface.allocator); }
};

RegisterFactory(GPathFactory);
